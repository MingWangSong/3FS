# Listener实现原理

## 1. 概述

Listener是网络通信框架中负责监听和接受连接的核心组件，它支持多种网络类型，包括TCP、UNIX域套接字和RDMA。本文档主要分析Listener相关文件的设计原理和调用规则，包括：

- `Listener.h`：定义了Listener类的接口和配置
- `Listener.cc`：实现了Listener的核心逻辑

Listener组件的主要职责是在指定的网络接口和端口上监听连接请求，并将接受的连接交给IOWorker处理。它支持多种网络类型，并能够根据配置选择合适的网络接口。

## 2. 核心数据结构

### 2.1 Listener::Config

`Listener::Config`类定义了Listener的配置参数：

```cpp
class Config : public ConfigBase<Config> {
  CONFIG_ITEM(listen_port, uint16_t{0});
  CONFIG_ITEM(reuse_port, false);
  CONFIG_ITEM(listen_queue_depth, 4096u);
  CONFIG_ITEM(filter_list, std::set<std::string>{});  // use all available network cards if filter is empty.
  CONFIG_ITEM(rdma_listen_ethernet, true);            // support setup RDMA connection with Ethernet by default.
  CONFIG_ITEM(domain_socket_index, 1u);
  CONFIG_HOT_UPDATED_ITEM(rdma_accept_timeout, 15_s);
};
```

配置参数的含义：
- `listen_port`：监听端口，0表示自动分配
- `reuse_port`：是否允许端口重用
- `listen_queue_depth`：监听队列深度
- `filter_list`：网络接口过滤列表，为空时使用所有可用网卡
- `rdma_listen_ethernet`：是否支持通过以太网设置RDMA连接
- `domain_socket_index`：UNIX域套接字索引
- `rdma_accept_timeout`：RDMA接受连接超时时间

### 2.2 Listener类

`Listener`类是实现监听功能的核心类：

```cpp
class Listener {
public:
  Listener(const Config &config,
           const IBSocket::Config &ibconfig,
           IOWorker &ioWorker,
           folly::IOThreadPoolExecutor &connThreadPool,
           Address::Type networkType);
  ~Listener() { stopAndJoin(); }

  // setup listener.
  Result<Void> setup();

  // start listening.
  Result<Void> start(ServiceGroup &group);

  // stop listening.
  void stopAndJoin();

  // get listening address list.
  const std::vector<Address> &addressList() const { return addressList_; }

protected:
  // do listen with calcellation support.
  CoTask<void> listen(folly::coro::ServerSocket socket);

  // accept a TCP connection.
  CoTask<void> acceptTCP(std::unique_ptr<folly::coro::Transport> tr);

  // accept a RDMA connection.
  void acceptRDMA(std::unique_ptr<IBSocket> socket);
  CoTask<void> checkRDMA(std::weak_ptr<Transport> weak);

  // release a TCP connection.
  CoTask<void> release(folly::coro::ServerSocket /* socket */);

private:
  const Config &config_;
  const IBSocket::Config &ibconfig_;
  IOWorker &ioWorker_;
  folly::IOThreadPoolExecutor &connThreadPool_;
  Address::Type networkType_;

  CancellationSource cancel_;
  std::vector<Address> addressList_;
  std::vector<folly::coro::ServerSocket> serverSockets_;
  std::atomic<size_t> running_{0};
};
```

`Listener`类的主要成员：
- `config_`：Listener配置
- `ibconfig_`：IBSocket配置，用于RDMA连接
- `ioWorker_`：IO工作线程，用于处理接受的连接
- `connThreadPool_`：连接线程池，用于处理连接事件
- `networkType_`：网络类型（TCP、UNIX、RDMA等）
- `cancel_`：取消源，用于取消监听任务
- `addressList_`：监听地址列表
- `serverSockets_`：服务器套接字列表
- `running_`：正在运行的任务数量

## 3. 工作流程

### 3.1 初始化和设置

Listener的初始化和设置过程由构造函数和`setup()`方法完成：

```cpp
Listener::Listener(const Config &config,
                  const IBSocket::Config &ibconfig,
                  IOWorker &ioWorker,
                  folly::IOThreadPoolExecutor &connThreadPool,
                  Address::Type networkType)
    : config_(config),
      ibconfig_(ibconfig),
      ioWorker_(ioWorker),
      connThreadPool_(connThreadPool),
      networkType_(networkType) {}

Result<Void> Listener::setup() {
  auto nics = IfAddrs::load();
  if (!nics) {
    XLOGF(ERR, "nic list is empty!");
    return makeError(RPCCode::kListenFailed);
  }

  auto &filters = config_.filter_list();
  for (auto [name, addr] : nics.value()) {
    if (addr.up && (filters.empty() || filters.count(name) != 0) && checkNicType(name, networkType_)) {
      addressList_.push_back(Address{addr.ip.toLong(),
                                    config_.listen_port(),
                                    networkType_ == Address::LOCAL ? Address::TCP : networkType_});
    }
  }

  if (networkType_ == Address::Type::UNIX) {
    auto port = config_.listen_port();
    if (port == 0) {
      port = folly::Random::rand32();
    }
    addressList_.push_back(Address{config_.domain_socket_index(), port, Address::Type::UNIX});
  }

  if (UNLIKELY(addressList_.empty())) {
    XLOGF(ERR,
          "No available address for listener with network type: {}, filter list: {}",
          magic_enum::enum_name(networkType_),
          fmt::join(filters, ","));
    return makeError(RPCCode::kListenFailed);
  }
  
  // 创建服务器套接字
  for (auto &address : addressList_) {
    auto evb = connThreadPool_.getEventBase();
    auto createSocket = [&]() -> CoTask<folly::coro::ServerSocket> {
      co_return folly::coro::ServerSocket(folly::AsyncServerSocket::newSocket(evb),
                                          address.toFollyAddress(),
                                          config_.listen_queue_depth(),
                                          config_.reuse_port());
    };
    auto socket = folly::coro::blockingWait(folly::coro::co_awaitTry(createSocket().scheduleOn(evb)));
    if (socket.hasException()) {
      XLOGF(ERR, "create socket failed: {}", socket.exception().what());
      return makeError(RPCCode::kListenFailed);
    }
    if (networkType_ != Address::Type::UNIX) {
      address.port = socket->getAsyncServerSocket()->getAddress().getPort();
    }
    serverSockets_.push_back(std::move(*socket));
  }
  XLOGF(INFO, "Listener setup: {}", fmt::join(addressList_, ", "));
  return Void{};
}
```

设置过程的主要步骤：
1. 加载网络接口列表
2. 根据过滤条件和网络类型选择合适的网络接口
3. 对于UNIX域套接字，创建特殊的地址
4. 检查是否有可用的地址
5. 为每个地址创建服务器套接字
6. 记录实际使用的端口号

### 3.2 启动监听

Listener的启动过程由`start()`方法完成：

```cpp
Result<Void> Listener::start(ServiceGroup &group) {
  if (networkType_ == Address::RDMA) {
    if (!IBManager::initialized()) {
      XLOGF(CRITICAL, "Address::Type is RDMA, but IBDevice not initialized!");
      return makeError(RPCCode::kIBDeviceNotInitialized);
    }
    auto accept = [this](auto socket) { acceptRDMA(std::move(socket)); };
    auto service = std::make_unique<IBConnectService>(ibconfig_, accept, config_.rdma_accept_timeout_getter());
    group.addSerdeService(std::move(service), Address::Type::TCP);
  }

  if (UNLIKELY(addressList_.empty())) {
    XLOGF(ERR, "Listener address list is empty!");
    return makeError(RPCCode::kListenFailed, "Listener address list is empty");
  }
  if (UNLIKELY(addressList_.size() != serverSockets_.size())) {
    XLOGF(ERR, "address list size {} != server sockets size {}", addressList_.size(), serverSockets_.size());
    return makeError(RPCCode::kListenFailed);
  }

  for (auto &socket : serverSockets_) {
    ++running_;
    auto evb = socket.getAsyncServerSocket()->getEventBase();
    co_withCancellation(cancel_.getToken(), listen(std::move(socket))).scheduleOn(evb).start();
  }
  serverSockets_.clear();
  return Void{};
}
```

启动过程的主要步骤：
1. 对于RDMA网络类型，创建IBConnectService并添加到ServiceGroup
2. 检查地址列表和套接字列表是否匹配
3. 为每个服务器套接字启动监听协程
4. 清空服务器套接字列表（套接字已被移动到监听协程中）

### 3.3 监听和接受连接

Listener的监听和接受连接过程由`listen()`、`acceptTCP()`和`acceptRDMA()`方法完成：

```cpp
CoTask<void> Listener::listen(folly::coro::ServerSocket socket) {
  while (true) {
    auto result = co_await co_awaitTry(socket.accept());
    if (UNLIKELY(result.hasException())) {
      if (result.hasException<OperationCancelled>()) {
        XLOGF(DBG, "Listener::listen is cancelled.");
        break;
      }

      XLOGF(ERR, "Listener::listen has exception: {}", result.exception().what());
      break;
    }

    co_await acceptTCP(std::move(result.value()));
  }
  --running_;
}

CoTask<void> Listener::acceptTCP(std::unique_ptr<folly::coro::Transport> tr) {
  auto result =
      ioWorker_.addTcpSocket(tr->getTransport()->getUnderlyingTransport<folly::AsyncSocket>()->detachNetworkSocket(),
                            networkType_ == Address::UNIX);
  if (UNLIKELY(!result)) {
    XLOGF(ERR, "IOWorker::addTcpSocket failed: {}", result.error());
  }
  co_return;
}

void Listener::acceptRDMA(std::unique_ptr<IBSocket> socket) {
  auto result = ioWorker_.addIBSocket(std::move(socket));
  if (result.hasError()) {
    // log CRITICAL here
    XLOGF(CRITICAL, "failed to add IBSocket {}", result.error());
    return;
  }

  ++running_;
  co_withCancellation(cancel_.getToken(), checkRDMA(*result)).scheduleOn(&connThreadPool_).start();
}
```

监听和接受连接的主要步骤：
1. `listen()`方法在循环中等待连接请求
2. 当接收到连接请求时，调用`acceptTCP()`方法处理TCP连接
3. `acceptTCP()`方法将TCP套接字添加到IOWorker中
4. 对于RDMA连接，`acceptRDMA()`方法将IBSocket添加到IOWorker中，并启动检查协程

### 3.4 RDMA连接检查

RDMA连接需要额外的检查，由`checkRDMA()`方法完成：

```cpp
CoTask<void> Listener::checkRDMA(std::weak_ptr<Transport> weak) {
  SCOPE_EXIT { --running_; };
  auto result = co_await co_awaitTry(folly::coro::sleep(config_.rdma_accept_timeout()));
  if (result.hasException()) {
    XLOGF(DBG, "checkRDMA is canceled");
    co_return;
  }
  auto transport = weak.lock();
  if (!transport) {
    co_return;
  }
  if (auto ib = transport->ibSocket(); ib && !ib->checkConnectFinished()) {
    XLOGF(ERR, "IBSocket {} still in ACCEPTED state after wait {}", ib->describe(), config_.rdma_accept_timeout());
    transport->invalidate();
    co_return;
  }
}
```

RDMA连接检查的主要步骤：
1. 等待一段时间（由`rdma_accept_timeout`配置）
2. 检查Transport是否仍然存在
3. 检查IBSocket是否完成连接
4. 如果连接未完成，则使Transport无效

### 3.5 停止和清理

Listener的停止和清理过程由`stopAndJoin()`方法完成：

```cpp
void Listener::stopAndJoin() {
  for (auto &socket : serverSockets_) {
    // folly ServerSocket's destructor must call on evb thread...
    auto evb = socket.getAsyncServerSocket()->getEventBase();
    running_++;
    release(std::move(socket)).scheduleOn(evb).start();
  }
  serverSockets_.clear();
  cancel_.requestCancellation();
  for (auto r = running_.load(); r != 0; r = running_.load()) {
    XLOGF(INFO, "Listener {} is waiting for {} tasks to finish...", fmt::ptr(this), r);
    std::this_thread::sleep_for(10ms);
  }
  for (auto &address : addressList_) {
    if (address.isUNIX()) {
      auto pathResult = address.domainSocketPath();
      if (bool(pathResult)) {
        boost::filesystem::remove(*pathResult);
      }
    }
  }
}

CoTask<void> Listener::release(folly::coro::ServerSocket /* socket */) {
  XLOGF(DBG, "release a unused TCP server socket");
  --running_;
  co_return;
}
```

停止和清理的主要步骤：
1. 释放所有未使用的服务器套接字
2. 请求取消所有监听协程
3. 等待所有任务完成
4. 删除UNIX域套接字文件

## 4. 网络类型支持

Listener支持多种网络类型，通过`networkType_`成员变量和`checkNicType()`函数来区分：

```cpp
static bool checkNicType(std::string_view nic, Address::Type type) {
  switch (type) {
    case Address::TCP:
      return nic.starts_with("en") || nic.starts_with("eth");
    case Address::IPoIB:
      return nic.starts_with("ib");
    case Address::RDMA:
      return nic.starts_with("en") || nic.starts_with("eth");
    case Address::LOCAL:
      return nic.starts_with("lo");
    default:
      return false;
  }
}
```

支持的网络类型：
- `Address::TCP`：TCP连接，使用以太网接口（en*或eth*）
- `Address::IPoIB`：IPoIB连接，使用InfiniBand接口（ib*）
- `Address::RDMA`：RDMA连接，使用以太网接口（en*或eth*）
- `Address::LOCAL`：本地连接，使用回环接口（lo*）
- `Address::UNIX`：UNIX域套接字连接，不使用网络接口

## 5. RDMA连接处理

Listener对RDMA连接有特殊处理，主要包括：

### 5.1 RDMA服务注册

在`start()`方法中，对于RDMA网络类型，Listener会创建IBConnectService并添加到ServiceGroup：

```cpp
if (networkType_ == Address::RDMA) {
  if (!IBManager::initialized()) {
    XLOGF(CRITICAL, "Address::Type is RDMA, but IBDevice not initialized!");
    return makeError(RPCCode::kIBDeviceNotInitialized);
  }
  auto accept = [this](auto socket) { acceptRDMA(std::move(socket)); };
  auto service = std::make_unique<IBConnectService>(ibconfig_, accept, config_.rdma_accept_timeout_getter());
  group.addSerdeService(std::move(service), Address::Type::TCP);
}
```

这里的关键点是：
1. 检查IBManager是否已初始化
2. 创建接受回调函数，指向`acceptRDMA()`方法
3. 创建IBConnectService，传入IBSocket配置、接受回调函数和超时获取函数
4. 将服务添加到ServiceGroup，注意网络类型是TCP，因为RDMA连接是通过TCP建立的

### 5.2 RDMA连接接受

RDMA连接的接受过程由`acceptRDMA()`方法完成：

```cpp
void Listener::acceptRDMA(std::unique_ptr<IBSocket> socket) {
  auto result = ioWorker_.addIBSocket(std::move(socket));
  if (result.hasError()) {
    // log CRITICAL here
    XLOGF(CRITICAL, "failed to add IBSocket {}", result.error());
    return;
  }

  ++running_;
  co_withCancellation(cancel_.getToken(), checkRDMA(*result)).scheduleOn(&connThreadPool_).start();
}
```

接受过程的主要步骤：
1. 将IBSocket添加到IOWorker中
2. 启动检查协程，确保连接正确建立

### 5.3 RDMA连接检查

RDMA连接需要额外的检查，由`checkRDMA()`方法完成：

```cpp
CoTask<void> Listener::checkRDMA(std::weak_ptr<Transport> weak) {
  SCOPE_EXIT { --running_; };
  auto result = co_await co_awaitTry(folly::coro::sleep(config_.rdma_accept_timeout()));
  if (result.hasException()) {
    XLOGF(DBG, "checkRDMA is canceled");
    co_return;
  }
  auto transport = weak.lock();
  if (!transport) {
    co_return;
  }
  if (auto ib = transport->ibSocket(); ib && !ib->checkConnectFinished()) {
    XLOGF(ERR, "IBSocket {} still in ACCEPTED state after wait {}", ib->describe(), config_.rdma_accept_timeout());
    transport->invalidate();
    co_return;
  }
}
```

检查过程的主要步骤：
1. 等待一段时间（由`rdma_accept_timeout`配置）
2. 检查Transport是否仍然存在
3. 检查IBSocket是否完成连接
4. 如果连接未完成，则使Transport无效

## 6. 协程和异步处理

Listener大量使用协程和异步处理技术，主要包括：

### 6.1 协程任务

Listener使用`CoTask<T>`类型表示协程任务，这是一个基于folly::coro的协程类型：

```cpp
CoTask<void> Listener::listen(folly::coro::ServerSocket socket) {
  while (true) {
    auto result = co_await co_awaitTry(socket.accept());
    // ...
  }
  --running_;
}
```

协程任务的特点：
1. 使用`co_await`等待异步操作完成
2. 使用`co_return`返回结果
3. 可以在不阻塞线程的情况下等待IO操作

### 6.2 取消支持

Listener使用`co_withCancellation`支持取消操作：

```cpp
co_withCancellation(cancel_.getToken(), listen(std::move(socket))).scheduleOn(evb).start();
```

取消支持的特点：
1. 使用`CancellationSource`和`CancellationToken`实现取消机制
2. 在`stopAndJoin()`方法中调用`cancel_.requestCancellation()`请求取消
3. 协程可以检查取消请求并提前退出

### 6.3 异步调度

Listener使用`scheduleOn()`方法将协程调度到特定的事件基础上执行：

```cpp
co_withCancellation(cancel_.getToken(), listen(std::move(socket))).scheduleOn(evb).start();
```

异步调度的特点：
1. 使用`folly::IOThreadPoolExecutor`提供的事件基础
2. 确保套接字操作在正确的线程上执行
3. 使用`start()`方法启动协程，不等待其完成

## 7. 错误处理

Listener使用多种机制进行错误处理：

### 7.1 返回值检查

Listener的`setup()`和`start()`方法返回`Result<Void>`类型，用于表示操作是否成功：

```cpp
Result<Void> Listener::setup() {
  auto nics = IfAddrs::load();
  if (!nics) {
    XLOGF(ERR, "nic list is empty!");
    return makeError(RPCCode::kListenFailed);
  }
  // ...
}
```

返回值检查的特点：
1. 使用`Result<T>`类型表示可能失败的操作
2. 使用`makeError()`创建错误结果
3. 调用者需要检查返回值是否有错误

### 7.2 异常处理

Listener使用`co_awaitTry()`处理可能抛出异常的协程操作：

```cpp
auto result = co_await co_awaitTry(socket.accept());
if (UNLIKELY(result.hasException())) {
  if (result.hasException<OperationCancelled>()) {
    XLOGF(DBG, "Listener::listen is cancelled.");
    break;
  }

  XLOGF(ERR, "Listener::listen has exception: {}", result.exception().what());
  break;
}
```

异常处理的特点：
1. 使用`co_awaitTry()`捕获协程操作的异常
2. 检查异常类型，区分取消操作和其他错误
3. 记录错误信息并采取适当的行动

### 7.3 日志记录

Listener使用XLOG记录关键操作和错误情况：

```cpp
XLOGF(INFO, "Listener setup: {}", fmt::join(addressList_, ", "));
XLOGF(ERR, "create socket failed: {}", socket.exception().what());
XLOGF(CRITICAL, "failed to add IBSocket {}", result.error());
```

日志记录的特点：
1. 使用不同的日志级别（INFO、DBG、ERR、CRITICAL）表示不同的重要性
2. 记录详细的错误信息和上下文
3. 使用fmt格式化字符串，提高可读性

## 8. 调用规则

### 8.1 创建和初始化Listener

```cpp
// 创建Listener配置
Listener::Config config;
config.listen_port = 8080;
config.reuse_port = true;
config.filter_list = {"eth0", "eth1"};

// 创建IBSocket配置
IBSocket::Config ibconfig;

// 创建Listener
Listener listener(config, ibconfig, ioWorker, connThreadPool, Address::TCP);

// 设置Listener
auto result = listener.setup();
if (result.hasError()) {
  // 处理设置失败
}
```

### 8.2 启动Listener

```cpp
// 创建ServiceGroup
ServiceGroup group;

// 启动Listener
auto result = listener.start(group);
if (result.hasError()) {
  // 处理启动失败
}

// 获取监听地址列表
auto addressList = listener.addressList();
```

### 8.3 停止Listener

```cpp
// 停止Listener
listener.stopAndJoin();
```

## 9. 性能优化

Listener实现了多项性能优化：

### 9.1 多网卡支持

Listener支持在多个网卡上监听连接，提高系统的可用性和性能：

```cpp
for (auto [name, addr] : nics.value()) {
  if (addr.up && (filters.empty() || filters.count(name) != 0) && checkNicType(name, networkType_)) {
    addressList_.push_back(Address{addr.ip.toLong(),
                                  config_.listen_port(),
                                  networkType_ == Address::LOCAL ? Address::TCP : networkType_});
  }
}
```

多网卡支持的特点：
1. 自动发现系统中的网卡
2. 支持通过`filter_list`配置选择特定的网卡
3. 根据网络类型选择合适的网卡类型

### 9.2 端口重用

Listener支持端口重用，允许多个Listener实例绑定到同一个端口：

```cpp
co_return folly::coro::ServerSocket(folly::AsyncServerSocket::newSocket(evb),
                                    address.toFollyAddress(),
                                    config_.listen_queue_depth(),
                                    config_.reuse_port());
```

端口重用的特点：
1. 通过`reuse_port`配置控制是否启用端口重用
2. 允许多个进程或线程绑定到同一个端口
3. 提高系统的并发处理能力

### 9.3 异步IO

Listener使用异步IO和协程技术，避免阻塞线程：

```cpp
auto result = co_await co_awaitTry(socket.accept());
```

异步IO的特点：
1. 使用协程等待IO操作完成，不阻塞线程
2. 使用事件驱动模型处理连接请求
3. 提高系统的吞吐量和响应性

### 9.4 连接线程池

Listener使用连接线程池处理连接请求，避免主线程阻塞：

```cpp
auto evb = connThreadPool_.getEventBase();
co_withCancellation(cancel_.getToken(), listen(std::move(socket))).scheduleOn(evb).start();
```

连接线程池的特点：
1. 使用`folly::IOThreadPoolExecutor`提供线程池
2. 将连接请求分发到不同的线程处理
3. 提高系统的并发处理能力

## 10. 总结

Listener是网络通信框架中负责监听和接受连接的核心组件，它支持多种网络类型，包括TCP、UNIX域套接字和RDMA。主要特点包括：

1. **多网络类型支持**：支持TCP、UNIX域套接字和RDMA等多种网络类型
2. **多网卡支持**：可以在多个网卡上监听连接，提高系统的可用性和性能
3. **协程和异步处理**：使用协程和异步IO技术，避免阻塞线程
4. **取消支持**：支持取消操作，优雅地停止监听
5. **错误处理**：完善的错误处理机制，提高系统的可靠性
6. **性能优化**：多项性能优化，提高系统的吞吐量和响应性

Listener与IOWorker和ServiceGroup紧密配合，共同构成了完整的网络通信框架，为应用提供高性能的网络通信能力。 