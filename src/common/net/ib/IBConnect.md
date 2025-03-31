# IBConnect实现原理

## 1. 概述

IBConnect是IB通信模块中负责建立InfiniBand连接的核心组件，它提供了一套完整的机制来实现IB设备的发现、匹配和连接建立。本文档主要分析IBConnect相关文件的设计原理和调用规则，包括：

- `IBConnect.h`：定义了连接相关的数据结构和接口
- `IBConnect.cc`：实现了连接建立的核心逻辑
- `IBConnectService.h`：定义了连接服务的RPC接口

IBConnect的主要功能是在TCP连接的基础上，协助两个节点建立InfiniBand连接。这个过程包括设备发现、设备匹配、连接参数交换和连接建立等步骤。

## 2. 核心数据结构

### 2.1 设备信息相关结构

#### IBDeviceInfo

`IBDeviceInfo`结构用于描述IB设备的信息，包括设备ID、名称和端口信息：

```cpp
struct IBDeviceInfo {
  SERDE_STRUCT_FIELD(dev, uint8_t(0));
  SERDE_STRUCT_FIELD(dev_name, std::string());
  SERDE_STRUCT_FIELD(ports, std::vector<PortInfo>());
};
```

其中`PortInfo`结构描述了设备的端口信息：

```cpp
struct PortInfo {
  SERDE_STRUCT_FIELD(port, uint8_t(0));
  SERDE_STRUCT_FIELD(zones, std::vector<std::string>());
  SERDE_STRUCT_FIELD(link_layer, uint8_t(IBV_LINK_LAYER_UNSPECIFIED));
};
```

这些结构使用了`SERDE_STRUCT_FIELD`宏，这是一个序列化/反序列化宏，用于支持结构的网络传输。

### 2.2 连接配置结构

#### IBConnectConfig

`IBConnectConfig`结构定义了IB连接的各种参数，包括服务质量、重试次数、缓冲区大小等：

```cpp
struct IBConnectConfig {
  SERDE_STRUCT_FIELD(sl, uint8_t(0));                    // 服务级别
  SERDE_STRUCT_FIELD(traffic_class, uint8_t(0));         // 流量类别
  SERDE_STRUCT_FIELD(pkey_index, uint16_t(0));           // 分区键索引
  
  SERDE_STRUCT_FIELD(start_psn, uint32_t(0));            // 起始包序号
  SERDE_STRUCT_FIELD(min_rnr_timer, uint8_t(0));         // 最小RNR定时器
  SERDE_STRUCT_FIELD(timeout, uint8_t(0));               // 超时时间
  SERDE_STRUCT_FIELD(retry_cnt, uint8_t(0));             // 重试次数
  SERDE_STRUCT_FIELD(rnr_retry, uint8_t(0));             // RNR重试次数
  
  SERDE_STRUCT_FIELD(max_sge, uint32_t(0));              // 最大分散/聚集元素
  SERDE_STRUCT_FIELD(max_rdma_wr, uint32_t(0));          // 最大RDMA工作请求
  SERDE_STRUCT_FIELD(max_rdma_wr_per_post, uint32_t(0)); // 每次提交的最大RDMA工作请求
  SERDE_STRUCT_FIELD(max_rd_atomic, uint32_t(0));        // 最大原子读操作
  
  SERDE_STRUCT_FIELD(buf_size, uint32_t(0));             // 缓冲区大小
  SERDE_STRUCT_FIELD(send_buf_cnt, uint32_t(0));         // 发送缓冲区数量
  SERDE_STRUCT_FIELD(buf_ack_batch, uint32_t(0));        // 缓冲区确认批次
  SERDE_STRUCT_FIELD(buf_signal_batch, uint32_t(0));     // 缓冲区信号批次
  SERDE_STRUCT_FIELD(event_ack_batch, uint32_t(0));      // 事件确认批次
  
  SERDE_STRUCT_FIELD(record_latency_per_peer, false);    // 是否记录每个对等点的延迟
};
```

该结构还提供了一些辅助方法来计算队列对(QP)的参数：

```cpp
uint32_t qpAckBufs() const { return (send_buf_cnt + buf_ack_batch - 1) / buf_ack_batch + 4; }
uint32_t qpMaxSendWR() const { return send_buf_cnt + qpAckBufs() + max_rdma_wr + 1 + 1; }
uint32_t qpMaxRecvWR() const { return send_buf_cnt + qpAckBufs() + 1; }
uint32_t qpMaxCQEntries() const { return qpMaxSendWR() + qpMaxRecvWR(); }
```

### 2.3 连接信息结构

#### IBConnectInfo

`IBConnectInfo`结构包含了建立IB连接所需的信息，包括主机名、设备信息、队列对号等：

```cpp
struct IBConnectInfo {
  SERDE_STRUCT_FIELD(hostname, std::string());
  SERDE_STRUCT_FIELD(dev, uint8_t(0));
  SERDE_STRUCT_FIELD(dev_name, std::string());
  SERDE_STRUCT_FIELD(port, uint8_t(0));

  SERDE_STRUCT_FIELD(mtu, uint8_t(0));
  SERDE_STRUCT_FIELD(qp_num, uint32_t(0));
  SERDE_STRUCT_FIELD(linklayer, (std::variant<IBConnectIBInfo, IBConnectRoCEInfo>()));
};
```

其中`linklayer`字段是一个变体类型，可以是`IBConnectIBInfo`或`IBConnectRoCEInfo`，分别对应InfiniBand和RoCE（RDMA over Converged Ethernet）两种链路层类型：

```cpp
struct IBConnectIBInfo {
  SERDE_STRUCT_FIELD(lid, uint16_t(0));
  static constexpr auto kType = IBV_LINK_LAYER_INFINIBAND;
};

struct IBConnectRoCEInfo {
  SERDE_STRUCT_FIELD(gid, ibv_gid{});
  static constexpr auto kType = IBV_LINK_LAYER_ETHERNET;
};
```

### 2.4 连接请求和响应结构

#### IBConnectReq

`IBConnectReq`结构继承自`IBConnectInfo`，用于发起连接请求：

```cpp
struct IBConnectReq : IBConnectInfo {
  SERDE_STRUCT_FIELD(target_dev, uint8_t(0));
  SERDE_STRUCT_FIELD(target_devname, std::string());
  SERDE_STRUCT_FIELD(target_port, uint8_t(0));
  SERDE_STRUCT_FIELD(config, IBConnectConfig());
};
```

#### IBConnectRsp

`IBConnectRsp`结构也继承自`IBConnectInfo`，用于响应连接请求：

```cpp
struct IBConnectRsp : IBConnectInfo {
  SERDE_STRUCT_FIELD(dummy, Void{});
};
```

## 3. 连接建立流程

### 3.1 设备查询和匹配

连接建立的第一步是查询远程节点的IB设备信息，并与本地设备进行匹配。这个过程由`IBQueryRsp::selectDevice`和`IBQueryRsp::findMatchDevices`方法实现：

```cpp
Result<IBQueryRsp::Match> IBQueryRsp::selectDevice(const std::string &describe, uint64_t counter) {
  // 检查远程设备列表是否为空
  if (devices.empty()) {
    return makeError(RPCCode::kIBDeviceNotFound, "...");
  }

  // 尝试按区域匹配，如果失败则尝试不按区域匹配
  for (auto checkZone : {true, false}) {
    if (!checkZone && !IBManager::instance().config().allow_unknown_zone()) {
      break;
    }

    std::vector<Match> ibMatches;
    std::vector<Match> roceMatches;
    findMatchDevices(describe, ibMatches, roceMatches, checkZone);

    // 合并匹配结果并选择一个
    std::vector<Match> allMatches;
    allMatches.insert(allMatches.end(), ibMatches.begin(), ibMatches.end());
    allMatches.insert(allMatches.end(), roceMatches.begin(), roceMatches.end());
    if (!allMatches.empty()) {
      auto idx = counter % allMatches.size();
      if (!ibMatches.empty() && IBManager::instance().config().prefer_ibdevice()) {
        idx = counter % ibMatches.size();
      }
      auto match = allMatches[idx];
      return match;
    }
  }

  return makeError(RPCCode::kIBDeviceNotFound, "No match device to connect");
}
```

设备匹配的规则包括：
1. 链路层类型必须相同（InfiniBand或RoCE）
2. 端口必须处于活动状态
3. 如果启用了区域检查，则设备必须在同一个区域

### 3.2 客户端连接流程

客户端通过`IBSocket::connect`方法发起连接，主要步骤包括：

1. 查询远程节点的设备信息
2. 选择匹配的设备
3. 创建和初始化队列对(QP)
4. 发送连接请求
5. 接收连接响应
6. 设置QP状态为RTR（Ready to Receive）和RTS（Ready to Send）
7. 发送一个空消息通知对方连接已建立

```cpp
CoTryTask<void> IBSocket::connect(serde::ClientContext &ctx, Duration timeout) {
  // 查询远程节点的设备信息
  IBQueryReq query;
  auto queryRsp = co_await IBConnect<>::query(ctx, query, &opts);
  
  // 选择匹配的设备
  auto match = queryRsp->selectDevice(describe(), cnt);
  
  // 打开本地端口
  auto port = match->localDev->openPort(match->localPort);
  port_ = std::move(*port);
  
  // 创建和初始化QP
  connectConfig_ = config_.clone().toIBConnectConfig(port_.isRoCE());
  if (qpCreate() != 0 || qpInit() != 0) {
    co_return makeError(...);
  }
  
  // 获取本地连接信息
  auto info = getConnectInfo();
  
  // 发送连接请求
  IBConnectReq connect(*info, match->remoteDev, match->remoteDevName, match->remotePort, connectConfig_);
  auto connectRsp = co_await IBConnect<>::connect(ctx, connect, &opts);
  
  // 设置对等信息
  setPeerInfo(folly::IPAddressV4::fromLong(ctx.addr().ip), *connectRsp);
  
  // 设置QP状态为RTR和RTS
  if (qpReadyToRecv() != 0 || qpReadyToSend() != 0) {
    co_return makeError(...);
  }
  
  // 发送空消息通知对方
  auto bufIdx = sendBufs_.front().first;
  sendBufs_.pop();
  if (postSend(bufIdx, 0, IBV_SEND_SIGNALED) != 0) {
    co_return makeError(...);
  }
  
  co_return Void{};
}
```

### 3.3 服务端接受连接流程

服务端通过`IBSocket::accept`方法接受连接，主要步骤包括：

1. 检查本地端口
2. 创建和初始化队列对(QP)
3. 设置对等信息
4. 设置QP状态为RTR（Ready to Receive）
5. 等待客户端发送的空消息

```cpp
Result<Void> IBSocket::accept(folly::IPAddressV4 ip, const IBConnectReq &req, Duration acceptTimeout) {
  // 检查本地端口
  RETURN_ON_ERROR(checkPort());
  
  // 设置连接配置
  connectConfig_ = req.config;
  
  // 创建和初始化QP
  if (qpCreate() != 0 || qpInit() != 0) {
    return makeError(...);
  }
  
  // 设置对等信息
  setPeerInfo(ip, req);
  
  // 设置QP状态为RTR
  if (qpReadyToRecv() != 0) {
    return makeError(...);
  }
  
  // 设置状态为ACCEPTED，等待客户端发送空消息
  auto state = state_.exchange(State::ACCEPTED);
  
  // 设置接受超时
  acceptTimeout_ = SteadyClock::now() + acceptTimeout;
  
  return Void{};
}
```

### 3.4 队列对(QP)状态转换

IB连接建立过程中，队列对(QP)需要经历多个状态转换：

1. **RESET → INIT**：通过`qpInit`方法实现
   ```cpp
   int IBSocket::qpInit() {
     ibv_qp_attr attr;
     memset(&attr, 0, sizeof(attr));
     attr.qp_state = IBV_QPS_INIT;
     attr.qp_access_flags = IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_READ;
     attr.pkey_index = connectConfig_.pkey_index;
     attr.port_num = port_.portNum();
     
     int ret = ibv_modify_qp(qp_.get(), &attr, IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT | IBV_QP_ACCESS_FLAGS);
     return ret;
   }
   ```

2. **INIT → RTR**：通过`qpReadyToRecv`方法实现
   ```cpp
   int IBSocket::qpReadyToRecv() {
     ibv_qp_attr attr;
     memset(&attr, 0, sizeof(attr));
     attr.qp_state = IBV_QPS_RTR;
     attr.path_mtu = std::min(port_.attr().active_mtu, (enum ibv_mtu)peerInfo_.mtu);
     attr.dest_qp_num = peerInfo_.qp_num;
     attr.rq_psn = connectConfig_.start_psn;
     attr.max_dest_rd_atomic = connectConfig_.max_rd_atomic;
     attr.min_rnr_timer = connectConfig_.min_rnr_timer;
     
     // 设置地址句柄属性，根据链路层类型不同有不同的设置
     if (port_.attr().link_layer == IBV_LINK_LAYER_INFINIBAND) {
       // InfiniBand设置
       auto conn = std::get<IBConnectIBInfo>(peerInfo_.linklayer);
       attr.ah_attr.is_global = 0;
       attr.ah_attr.dlid = conn.lid;
     } else if (port_.attr().link_layer == IBV_LINK_LAYER_ETHERNET) {
       // RoCE设置
       auto conn = std::get<IBConnectRoCEInfo>(peerInfo_.linklayer);
       attr.ah_attr.is_global = 1;
       attr.ah_attr.grh.dgid = conn.gid;
       attr.ah_attr.grh.flow_label = 0;
       attr.ah_attr.grh.sgid_index = port_.getRoCEv2Gid().second;
       attr.ah_attr.grh.hop_limit = 255;
       attr.ah_attr.grh.traffic_class = connectConfig_.traffic_class;
     }
     
     attr.ah_attr.sl = connectConfig_.sl;
     attr.ah_attr.src_path_bits = 0;
     attr.ah_attr.port_num = peerInfo_.port;
     
     int mask = IBV_QP_STATE | IBV_QP_AV | IBV_QP_PATH_MTU | IBV_QP_DEST_QPN | 
                IBV_QP_RQ_PSN | IBV_QP_MAX_DEST_RD_ATOMIC | IBV_QP_MIN_RNR_TIMER;
     int ret = ibv_modify_qp(qp_.get(), &attr, mask);
     
     // 提交接收缓冲区
     for (size_t idx = 0; idx < recvBufs_.getBufCnt(); idx++) {
       if (postRecv(idx)) {
         return -1;
       }
     }
     
     return ret;
   }
   ```

3. **RTR → RTS**：通过`qpReadyToSend`方法实现（仅客户端需要）
   ```cpp
   int IBSocket::qpReadyToSend() {
     ibv_qp_attr attr;
     memset(&attr, 0, sizeof(attr));
     attr.qp_state = IBV_QPS_RTS;
     attr.timeout = connectConfig_.timeout;
     attr.retry_cnt = connectConfig_.retry_cnt;
     attr.rnr_retry = connectConfig_.rnr_retry;
     attr.sq_psn = connectConfig_.start_psn;
     attr.max_rd_atomic = connectConfig_.max_rd_atomic;
     
     int ret = ibv_modify_qp(qp_.get(), &attr, 
                            IBV_QP_STATE | IBV_QP_TIMEOUT | IBV_QP_RETRY_CNT | 
                            IBV_QP_RNR_RETRY | IBV_QP_SQ_PSN | IBV_QP_MAX_QP_RD_ATOMIC);
     
     // 初始化发送缓冲区和信号量
     sendBufs_.push(sendBufs_.getBufCnt());
     rdmaSem_.signal(connectConfig_.max_rdma_wr);
     ackBufAvailable_.store(connectConfig_.qpAckBufs());
     
     return ret;
   }
   ```

## 4. 连接服务实现

### 4.1 IBConnectService

`IBConnectService`类实现了IB连接服务的RPC接口，包括设备查询和连接建立：

```cpp
class IBConnectService : public serde::ServiceWrapper<IBConnectService, IBConnect> {
public:
  using AcceptFn = std::function<void(std::unique_ptr<IBSocket>)>;

  IBConnectService(const IBSocket::Config &config, AcceptFn accept, std::function<Duration()> acceptTimeout)
      : config_(config),
        accept_(std::move(accept)),
        acceptTimeout_(acceptTimeout) {}

  CoTryTask<IBQueryRsp> query(serde::CallContext &ctx, const IBQueryReq &req);
  CoTryTask<IBConnectRsp> connect(serde::CallContext &ctx, const IBConnectReq &req);

private:
  const IBSocket::Config &config_;
  AcceptFn accept_;
  std::function<Duration()> acceptTimeout_;
};
```

### 4.2 设备查询接口

`query`方法实现了设备查询接口，返回本地所有可用的IB设备信息：

```cpp
CoTryTask<IBQueryRsp> IBConnectService::query(serde::CallContext &ctx, const IBQueryReq &req) {
  XLOGF_IF(FATAL, !IBManager::initialized(), "IBDevice not initialized.");

  // 检查传输类型
  if (!ctx.transport()->isTCP()) {
    co_return makeError(StatusCode::kInvalidArg);
  }

  // 构建响应
  IBQueryRsp rsp;
  for (const auto &dev : IBDevice::all()) {
    IBDeviceInfo devInfo;
    devInfo.dev = dev->id();
    devInfo.dev_name = dev->name();
    
    // 添加活动端口信息
    for (const auto &[portNum, port] : dev->ports()) {
      if (auto state = port.attr.rlock()->state; state != IBV_PORT_ACTIVE && state != IBV_PORT_ACTIVE_DEFER) {
        continue;
      }
      
      auto &portInfo = devInfo.ports.emplace_back();
      portInfo.port = portNum;
      portInfo.link_layer = port.attr.rlock()->link_layer;
      
      for (const auto &zone : port.zones) {
        portInfo.zones.push_back(zone);
      }
    }
    
    // 只添加有活动端口的设备
    if (devInfo.ports.empty()) {
      continue;
    }
    
    rsp.devices.push_back(devInfo);
  }

  co_return rsp;
}
```

### 4.3 连接建立接口

`connect`方法实现了连接建立接口，接受远程节点的连接请求并建立IB连接：

```cpp
CoTryTask<IBConnectRsp> IBConnectService::connect(serde::CallContext &ctx, const IBConnectReq &req) {
  XLOGF_IF(FATAL, !IBManager::initialized(), "IBDevice not initialized.");

  // 检查传输类型
  if (!ctx.transport()->isTCP()) {
    co_return makeError(StatusCode::kInvalidConfig);
  }

  // 查找目标设备
  auto dev = IBDevice::get(req.target_dev);
  if (!dev || dev->name() != req.target_devname) {
    co_return makeError(RPCCode::kIBDeviceNotFound);
  }
  
  // 打开端口
  auto port = dev->openPort(req.target_port);
  if (port.hasError()) {
    co_return makeError(RPCCode::kIBOpenPortFailed);
  }

  // 创建IBSocket并接受连接
  auto ibsocket = std::make_unique<IBSocket>(config_, std::move(*port));
  if (auto result = ibsocket->accept(ctx.transport()->peerIP(), req, acceptTimeout_()); result.hasError()) {
    co_return makeError(result.error());
  }
  
  // 获取连接信息
  auto info = ibsocket->getConnectInfo();
  CO_RETURN_ON_ERROR(info);

  // 调用接受回调函数
  accept_(std::move(ibsocket));

  // 返回连接响应
  co_return IBConnectInfo(*info);
}
```

## 5. 错误处理和监控

### 5.1 错误处理

IBConnect模块使用`Result<T>`和`CoTryTask<T>`类型来处理错误，这些类型可以表示成功的结果或错误码。主要的错误处理策略包括：

1. **连接错误**：如设备不匹配、端口不可用等，返回相应的错误码
2. **超时处理**：连接请求和接受都有超时机制
3. **资源清理**：在错误发生时释放已分配的资源

### 5.2 监控和统计

IBConnect模块使用监控和统计机制跟踪连接状态和性能：

```cpp
monitor::CountRecorder connecting("common.ib.connecting", {}, false);
monitor::CountRecorder accepting("common.ib.accepting", {}, false);
monitor::CountRecorder connectFailed("common.ib.connect_failed");
monitor::CountRecorder acceptedFailed("common.ib.accept_failed");

monitor::LatencyRecorder connectLatency("common.ib.connect_latency");
monitor::LatencyRecorder acceptLatency("common.ib.accept_latency");
```

这些统计信息可以用于监控系统性能，发现潜在问题。

## 6. 调用规则

### 6.1 客户端调用规则

客户端建立IB连接的调用规则如下：

1. 创建`IBSocket`对象
2. 调用`connect`方法发起连接
3. 等待连接建立完成
4. 使用`send`、`recv`、`rdmaRead`、`rdmaWrite`等方法进行数据传输
5. 完成后调用`close`方法关闭连接

示例代码：
```cpp
// 创建IBSocket对象
auto socket = std::make_unique<IBSocket>(config);

// 发起连接
co_await socket->connect(ctx, timeout);

// 数据传输
socket->send(data);
socket->recv(buffer);

// 关闭连接
co_await socket->close();
```

### 6.2 服务端调用规则

服务端接受IB连接的调用规则如下：

1. 创建`IBConnectService`对象，提供接受回调函数
2. 在接受回调函数中处理新的`IBSocket`连接
3. 使用`send`、`recv`、`rdmaRead`、`rdmaWrite`等方法进行数据传输
4. 完成后调用`close`方法关闭连接

示例代码：
```cpp
// 接受回调函数
auto acceptFn = [](std::unique_ptr<IBSocket> socket) {
  // 处理新连接
  // ...
};

// 创建IBConnectService对象
auto service = std::make_shared<IBConnectService>(config, acceptFn, []() { return 15_s; });

// 注册服务
server.registerService(service);

// 在acceptFn中处理数据传输
socket->send(data);
socket->recv(buffer);

// 关闭连接
co_await socket->close();
```

## 7. 总结

IBConnect模块是IB通信框架中负责连接建立的核心组件，它通过TCP连接交换IB设备信息和连接参数，然后建立高性能的IB连接。主要特点包括：

1. **设备自动匹配**：自动发现和匹配兼容的IB设备和端口
2. **多链路层支持**：同时支持InfiniBand和RoCE两种链路层类型
3. **区域感知**：支持按区域匹配设备，提高安全性
4. **灵活配置**：支持丰富的连接参数配置
5. **错误处理**：完善的错误处理和监控机制
6. **协程支持**：使用协程简化异步编程

IBConnect模块与IBSocket和IBSocketManager模块紧密配合，共同构成了完整的IB通信框架，为应用提供高性能的网络通信能力。 