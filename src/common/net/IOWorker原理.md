# IOWorker实现原理

## 1. 概述

IOWorker是网络通信框架中负责处理所有IO任务的核心组件，它管理TCP和RDMA连接，处理数据的发送和接收，并负责连接的建立和释放。本文档主要分析IOWorker相关文件的设计原理和调用规则，包括：

- `IOWorker.h`：定义了IOWorker类的接口和配置
- `IOWorker.cc`：实现了IOWorker的核心逻辑

IOWorker组件的主要职责包括：
1. 管理网络连接（TCP和RDMA）
2. 处理异步消息发送和接收
3. 协调事件循环和任务调度
4. 提供连接池和资源管理

## 2. 核心数据结构

### 2.1 IOWorker::Config

`IOWorker::Config`类定义了IOWorker的配置参数：

```cpp
class Config : public ConfigBase<Config> {
  CONFIG_HOT_UPDATED_ITEM(read_write_tcp_in_event_thread, false);
  CONFIG_HOT_UPDATED_ITEM(read_write_rdma_in_event_thread, false);
  CONFIG_HOT_UPDATED_ITEM(tcp_connect_timeout, 1_s);
  CONFIG_HOT_UPDATED_ITEM(rdma_connect_timeout, 5_s);
  CONFIG_HOT_UPDATED_ITEM(wait_to_retry_send, 100_ms);
  CONFIG_ITEM(num_event_loop, 1u);

  CONFIG_OBJ(ibsocket, IBSocket::Config);
  CONFIG_OBJ(transport_pool, TransportPool::Config);
  CONFIG_OBJ(connect_concurrency_limiter, ConcurrencyLimiterConfig, [](auto &c) { c.set_max_concurrency(4); });
};
```

配置参数的含义：
- `read_write_tcp_in_event_thread`：是否在事件线程中处理TCP读写操作（热更新项）
- `read_write_rdma_in_event_thread`：是否在事件线程中处理RDMA读写操作（热更新项）
- `tcp_connect_timeout`：TCP连接超时时间（热更新项）
- `rdma_connect_timeout`：RDMA连接超时时间（热更新项）
- `wait_to_retry_send`：重试发送的等待时间（热更新项）
- `num_event_loop`：事件循环的数量
- `ibsocket`：IBSocket配置对象
- `transport_pool`：传输池配置对象
- `connect_concurrency_limiter`：连接并发限制器配置（默认最大并发为4）

### 2.2 IOWorker类

`IOWorker`类是处理IO任务的核心类：

```cpp
class IOWorker {
public:
  IOWorker(Processor &processor,
           CPUExecutorGroup &executor,
           folly::IOThreadPoolExecutor &connExecutor,
           const Config &config);
  ~IOWorker() { stopAndJoin(); }

  // 启动和停止
  Result<Void> start(const std::string &name);
  void stopAndJoin();

  // 添加TCP套接字
  Result<TransportPtr> addTcpSocket(folly::NetworkSocket sock, bool isDomainSocket = false);

  // 添加IBSocket
  Result<TransportPtr> addIBSocket(std::unique_ptr<IBSocket> sock);

  // 异步发送
  void sendAsync(Address addr, WriteList list);

  // 异步重试
  void retryAsync(Address addr, WriteList list);

  // 断开连接
  void dropConnections(bool dropAll = true, bool dropIncome = false);
  void dropConnections(Address addr);
  void checkConnections(Address addr, Duration expiredTime);

protected:
  // 获取传输对象
  TransportPtr getTransport(Address addr);
  
  // 移除传输对象
  void remove(TransportPtr transport);

  // 异步连接
  CoTryTask<void> startConnect(TransportPtr transport, Address addr);

  // 等待并重试
  CoTryTask<void> waitAndRetry(Address addr, WriteList list);

  // 启动读取任务
  void startReadTask(Transport *transport, bool error, bool logError = true);
  
  // 启动写入任务
  void startWriteTask(Transport *transport, bool error, bool logError = true);

  // 处理接收到的消息
  void processMsg(MessageWrapper wrapper, TransportPtr tr);

  // 获取连接执行器的弱引用
  std::weak_ptr<folly::Executor::KeepAlive<>> connExecutorWeak();

private:
  Processor &processor_;
  CPUExecutorGroup &executor_;
  folly::IOThreadPoolExecutor &connExecutor_;
  const Config &config_;
  std::atomic<uint32_t> dropConnections_{0};
  folly::atomic_shared_ptr<folly::Executor::KeepAlive<>> connExecutorShared_;
  TransportPool pool_;
  EventLoopPool eventLoopPool_;
  std::unique_ptr<ConfigCallbackGuard> ibsocketConfigGuard_;
  ConcurrencyLimiter<Address> connectConcurrencyLimiter_;
  constexpr static size_t kStopFlag = 1;
  constexpr static size_t kCountInc = 2;
  std::atomic<size_t> flags_{0};
};
```

`IOWorker`类的主要成员：
- `processor_`：处理器，用于处理接收到的消息
- `executor_`：CPU执行器组，用于处理CPU密集型任务
- `connExecutor_`：连接执行器，用于处理连接相关操作
- `config_`：IOWorker配置
- `dropConnections_`：是否丢弃连接的标志
- `connExecutorShared_`：连接执行器的共享引用
- `pool_`：传输池，管理所有的传输对象
- `eventLoopPool_`：事件循环池，监控所有传输对象的IO事件
- `ibsocketConfigGuard_`：IBSocket配置回调守卫
- `connectConcurrencyLimiter_`：连接并发限制器
- `flags_`：标志位，用于停止和计数

## 3. 工作流程

### 3.1 启动和停止

IOWorker的启动和停止过程由`start()`和`stopAndJoin()`方法完成：

```cpp
Result<Void> IOWorker::start(const std::string &name) { 
  return eventLoopPool_.start(name + "EL"); 
}

void IOWorker::stopAndJoin() {
  auto flags = flags_.fetch_or(kStopFlag);
  while (flags > kStopFlag) {
    XLOGF(INFO, "Waiting for {} tasks to finish...", (flags >> 1));
    std::this_thread::sleep_for(100ms);
    flags = flags_.load(std::memory_order_acquire);
  }

  eventLoopPool_.stopAndJoin();
  dropConnections(true, true);
  connExecutorShared_.store(std::shared_ptr<folly::Executor::KeepAlive<>>());
}
```

启动过程的主要步骤：
1. 启动事件循环池，传入名称
2. 返回启动结果

停止过程的主要步骤：
1. 设置停止标志位
2. 等待所有正在进行的任务完成
3. 停止并等待事件循环池
4. 断开所有连接
5. 清除连接执行器的共享引用

### 3.2 添加连接

IOWorker支持添加TCP和RDMA连接，分别由`addTcpSocket()`和`addIBSocket()`方法完成：

```cpp
Result<TransportPtr> IOWorker::addTcpSocket(folly::NetworkSocket sock, bool isDomainSocket /* = false */) {
  auto tcp = std::make_unique<TcpSocket>(sock.toFd());
  RETURN_AND_LOG_ON_ERROR(tcp->init(isDomainSocket));

  auto type = tcp->peerAddr().type;
  auto transport = Transport::create(std::move(tcp), *this, type);
  RETURN_AND_LOG_ON_ERROR(transport->getPeerCredentials());
  RETURN_AND_LOG_ON_ERROR(eventLoopPool_.add(transport, (EPOLLIN | EPOLLOUT | EPOLLET)));

  pool_.add(transport);
  return Result<TransportPtr>(std::move(transport));
}

Result<TransportPtr> IOWorker::addIBSocket(std::unique_ptr<IBSocket> sock) {
  auto transport = Transport::create(std::move(sock), *this, Address::RDMA);
  RETURN_AND_LOG_ON_ERROR(eventLoopPool_.add(transport, (EPOLLIN | EPOLLOUT | EPOLLET)));

  pool_.add(transport);
  return Result<TransportPtr>(std::move(transport));
}
```

添加TCP连接的主要步骤：
1. 创建TcpSocket对象
2. 初始化TcpSocket
3. 创建Transport对象
4. 获取对等凭证
5. 将Transport添加到事件循环池
6. 将Transport添加到传输池
7. 返回Transport指针

添加RDMA连接的主要步骤：
1. 创建Transport对象
2. 将Transport添加到事件循环池
3. 将Transport添加到传输池
4. 返回Transport指针

### 3.3 发送消息

IOWorker的消息发送过程由`sendAsync()`和`retryAsync()`方法完成：

```cpp
void IOWorker::sendAsync(Address addr, WriteList list) {
  auto transport = getTransport(addr);
  auto failList = transport->send(std::move(list));
  if (failList.has_value()) {
    remove(transport);
    if (!failList.value().empty()) {
      retryAsync(addr, std::move(failList.value()));
    }
  }
}

void IOWorker::retryAsync(Address addr, WriteList list) {
  auto flags = flags_ += kCountInc;
  if (flags & kStopFlag) {
    flags_ -= kCountInc;
  } else {
    waitAndRetry(addr, std::move(list)).scheduleOn(&connExecutor_).start();
  }
}
```

发送消息的主要步骤：
1. 获取目标地址的Transport
2. 发送消息列表
3. 如果发送失败，移除Transport并重试发送失败的消息

重试发送的主要步骤：
1. 增加任务计数
2. 如果没有停止标志，调度等待并重试任务
3. 否则减少任务计数

### 3.4 获取传输对象

IOWorker的传输对象获取过程由`getTransport()`方法完成：

```cpp
TransportPtr IOWorker::getTransport(Address addr) {
  auto [transport, doConnect] = pool_.get(addr, *this);
  if (doConnect) {
    // connect asynchronous.
    auto flags = flags_ += kCountInc;
    if (flags & kStopFlag) {
      flags_ -= kCountInc;
    } else {
      startConnect(transport, addr).scheduleOn(&connExecutor_).start();
    }
  }
  return transport;
}
```

获取传输对象的主要步骤：
1. 从传输池获取目标地址的Transport
2. 如果需要连接，增加任务计数并启动异步连接
3. 返回Transport

### 3.5 连接建立

IOWorker的连接建立过程由`startConnect()`方法完成：

```cpp
CoTryTask<void> IOWorker::startConnect(TransportPtr transport, Address addr) {
  auto flagsGuard = folly::makeGuard([this] { flags_ -= kCountInc; });
  auto guard = folly::makeGuard([transport] { transport->invalidate(false); });

  if (transport->isRDMA()) {
    auto guard = co_await connectConcurrencyLimiter_.lock(addr);
    CO_RETURN_AND_LOG_ON_ERROR(co_await transport->connect(addr, config_.rdma_connect_timeout()));
  } else {
    CO_RETURN_AND_LOG_ON_ERROR(co_await transport->connect(addr, config_.tcp_connect_timeout()));
  }
  CO_RETURN_AND_LOG_ON_ERROR(eventLoopPool_.add(transport, (EPOLLIN | EPOLLOUT | EPOLLET)));

  // connect successfully.
  guard.dismiss();
  co_return Void{};
}
```

连接建立的主要步骤：
1. 创建任务计数守卫和Transport无效化守卫
2. 根据Transport类型（RDMA或TCP）进行连接
   - 对于RDMA连接，使用并发限制器限制并发连接数
3. 将Transport添加到事件循环池
4. 如果连接成功，取消Transport无效化守卫
5. 返回连接结果

### 3.6 等待并重试

IOWorker的等待并重试过程由`waitAndRetry()`方法完成：

```cpp
CoTryTask<void> IOWorker::waitAndRetry(Address addr, WriteList list) {
  auto flagsGuard = folly::makeGuard([this] { flags_ -= kCountInc; });
  co_await folly::coro::sleep(config_.wait_to_retry_send().asMs());
  sendAsync(addr, std::move(list));
  co_return Void{};
}
```

等待并重试的主要步骤：
1. 创建任务计数守卫
2. 等待配置的重试时间
3. 重新发送消息列表
4. 返回重试结果

### 3.7 读写任务启动

IOWorker的读写任务启动过程由`startReadTask()`和`startWriteTask()`方法完成：

```cpp
void IOWorker::startReadTask(Transport *transport, bool error, bool logError /* = true */) {
  if ((transport->isTCP() && config_.read_write_tcp_in_event_thread()) ||
      (transport->isRDMA() && config_.read_write_rdma_in_event_thread())) {
    transport->doRead(error, logError);
  } else {
    executor_.pickNextFree().add(
        [tr = transport->shared_from_this(), error, logError] { tr->doRead(error, logError); });
  }
}

void IOWorker::startWriteTask(Transport *transport, bool error, bool logError /* = true */) {
  if ((transport->isTCP() && config_.read_write_tcp_in_event_thread()) ||
      (transport->isRDMA() && config_.read_write_rdma_in_event_thread())) {
    transport->doWrite(error, logError);
  } else {
    executor_.pickNextFree().add(
        [tr = transport->shared_from_this(), error, logError] { tr->doWrite(error, logError); });
  }
}
```

读写任务启动的主要步骤：
1. 根据Transport类型和配置决定在事件线程还是CPU执行器中执行读写操作
2. 如果在事件线程中执行，直接调用Transport的doRead或doWrite方法
3. 如果在CPU执行器中执行，将任务添加到下一个空闲的CPU执行器中

## 4. 连接管理

### 4.1 连接池

IOWorker使用`TransportPool`管理所有的传输对象：

```cpp
TransportPool pool_;
```

`TransportPool`的主要功能：
1. 添加新的传输对象
2. 获取目标地址的传输对象
3. 移除传输对象
4. 检查和断开连接

### 4.2 断开连接

IOWorker支持断开连接的方法包括：

```cpp
void dropConnections(bool dropAll = true, bool dropIncome = false) { 
  pool_.dropConnections(dropAll, dropIncome); 
}

void dropConnections(Address addr) { 
  pool_.dropConnections(addr); 
}

void checkConnections(Address addr, Duration expiredTime) { 
  return pool_.checkConnections(addr, expiredTime); 
}
```

断开连接的方法：
1. `dropConnections(bool, bool)`：断开所有连接或仅断开传出连接
2. `dropConnections(Address)`：断开到指定地址的所有连接
3. `checkConnections(Address, Duration)`：检查到指定地址的连接，断开超过指定时间的连接

### 4.3 IBSocket配置回调

IOWorker使用配置回调机制来处理IBSocket配置变更：

```cpp
std::unique_ptr<ConfigCallbackGuard> ibsocketConfigGuard_;

// 在构造函数中初始化
ibsocketConfigGuard_(config.ibsocket().addCallbackGuard([this] {
  auto newVal = config_.ibsocket().drop_connections();
  if (dropConnections_.exchange(newVal) != newVal) {
    XLOGF(INFO, "ioworker@{} drop all connections", fmt::ptr(this));
    dropConnections();
  }
}))
```

IBSocket配置回调的主要功能：
1. 监听IBSocket配置变更
2. 当`drop_connections`配置变更时，断开所有连接

## 5. 异步处理

### 5.1 协程任务

IOWorker使用协程任务处理异步操作，主要包括：

```cpp
CoTryTask<void> startConnect(TransportPtr transport, Address addr);
CoTryTask<void> waitAndRetry(Address addr, WriteList list);
```

协程任务的特点：
1. 使用`co_await`等待异步操作完成
2. 使用`co_return`返回结果
3. 使用`co_await`等待并发限制器
4. 使用`CO_RETURN_AND_LOG_ON_ERROR`宏处理错误

### 5.2 任务调度

IOWorker使用多种执行器进行任务调度：

```cpp
CPUExecutorGroup &executor_;
folly::IOThreadPoolExecutor &connExecutor_;
```

任务调度的方式：
1. 使用`connExecutor_`调度连接相关的任务（startConnect、waitAndRetry）
2. 使用`executor_`调度CPU密集型任务（doRead、doWrite）
3. 根据配置决定在事件线程还是CPU执行器中执行读写操作

### 5.3 任务计数

IOWorker使用任务计数机制跟踪正在进行的任务：

```cpp
constexpr static size_t kStopFlag = 1;
constexpr static size_t kCountInc = 2;
std::atomic<size_t> flags_{0};
```

任务计数的工作原理：
1. 启动任务时增加计数：`flags_ += kCountInc`
2. 完成任务时减少计数：`flags_ -= kCountInc`
3. 停止时设置停止标志位：`flags_.fetch_or(kStopFlag)`
4. 等待所有任务完成：`while (flags > kStopFlag) { ... }`

## 6. 事件循环

### 6.1 事件循环池

IOWorker使用`EventLoopPool`监控所有传输对象的IO事件：

```cpp
EventLoopPool eventLoopPool_;
```

事件循环池的主要功能：
1. 启动事件循环
2. 添加传输对象到事件循环
3. 监控传输对象的IO事件
4. 停止事件循环

### 6.2 事件处理

当IO事件发生时，事件循环会调用传输对象的回调函数，进而调用IOWorker的`startReadTask()`和`startWriteTask()`方法：

```cpp
void startReadTask(Transport *transport, bool error, bool logError = true);
void startWriteTask(Transport *transport, bool error, bool logError = true);
```

事件处理的流程：
1. 事件循环检测到IO事件
2. 调用传输对象的回调函数
3. 传输对象调用IOWorker的`startReadTask()`或`startWriteTask()`方法
4. IOWorker根据配置在事件线程或CPU执行器中执行读写操作
5. 传输对象执行读写操作并处理结果

## 7. 错误处理

### 7.1 错误返回

IOWorker使用`Result<T>`类型表示可能失败的操作，主要包括：

```cpp
Result<Void> start(const std::string &name);
Result<TransportPtr> addTcpSocket(folly::NetworkSocket sock, bool isDomainSocket = false);
Result<TransportPtr> addIBSocket(std::unique_ptr<IBSocket> sock);
```

错误返回的处理方式：
1. 使用`RETURN_AND_LOG_ON_ERROR`宏检查操作结果并记录错误
2. 使用`CO_RETURN_AND_LOG_ON_ERROR`宏检查协程操作结果并记录错误
3. 返回包含错误信息的`Result<T>`对象

### 7.2 连接错误

当连接发生错误时，IOWorker会采取以下措施：

1. 记录错误信息
2. 关闭连接
3. 移除传输对象
4. 重试发送失败的消息

### 7.3 读写错误

当读写操作发生错误时，IOWorker会采取以下措施：

1. 通过`error`参数标识错误状态
2. 通过`logError`参数控制是否记录错误
3. 通知传输对象处理错误
4. 必要时关闭连接并移除传输对象

## 8. 性能优化

### 8.1 并发限制

IOWorker使用`ConcurrencyLimiter`限制并发连接数：

```cpp
ConcurrencyLimiter<Address> connectConcurrencyLimiter_;
```

并发限制的作用：
1. 防止大量连接同时建立导致系统负载过高
2. 根据地址限制并发连接数，避免对同一目标进行过多的连接尝试
3. 使用协程机制实现非阻塞的并发限制

### 8.2 线程控制

IOWorker提供了控制读写操作执行线程的配置：

```cpp
CONFIG_HOT_UPDATED_ITEM(read_write_tcp_in_event_thread, false);
CONFIG_HOT_UPDATED_ITEM(read_write_rdma_in_event_thread, false);
```

线程控制的优势：
1. 在事件线程中执行可以减少线程切换开销
2. 在CPU执行器中执行可以避免阻塞事件循环
3. 通过配置可以根据实际情况调整策略，实现最佳性能

### 8.3 传输池

IOWorker使用`TransportPool`管理传输对象，实现连接复用：

```cpp
TransportPool pool_;
```

传输池的优势：
1. 避免频繁创建和销毁连接
2. 实现连接的统一管理
3. 支持根据地址查找和复用连接
4. 提供连接的生命周期管理

### 8.4 事件驱动

IOWorker采用事件驱动模型处理IO事件：

```cpp
EventLoopPool eventLoopPool_;
```

事件驱动的优势：
1. 使用epoll监控大量连接，避免轮询造成的CPU浪费
2. 只在有事件时触发回调，提高系统效率
3. 支持边缘触发（EPOLLET）模式，减少事件通知次数
4. 通过事件循环池实现多线程并行处理事件

## 9. 调用规则

### 9.1 创建和初始化IOWorker

```cpp
// 创建配置
IOWorker::Config config;
config.num_event_loop = 4;
config.tcp_connect_timeout = 2_s;
config.rdma_connect_timeout = 10_s;

// 创建IOWorker
IOWorker ioWorker(processor, executorGroup, connExecutor, config);

// 启动IOWorker
auto result = ioWorker.start("worker1");
if (result.hasError()) {
  // 处理启动失败
}
```

### 9.2 添加连接

```cpp
// 添加TCP连接
auto sock = createTcpSocket(); // 假设这是一个创建TCP套接字的函数
auto result1 = ioWorker.addTcpSocket(sock);
if (result1.hasError()) {
  // 处理添加失败
} else {
  auto transport = std::move(result1.value());
  // 使用transport
}

// 添加RDMA连接
auto ibsock = createIBSocket(); // 假设这是一个创建IBSocket的函数
auto result2 = ioWorker.addIBSocket(std::move(ibsock));
if (result2.hasError()) {
  // 处理添加失败
} else {
  auto transport = std::move(result2.value());
  // 使用transport
}
```

### 9.3 发送消息

```cpp
// 创建消息列表
WriteList list;
list.push_back(createWriteItem(data1)); // 假设这是创建写入项的函数
list.push_back(createWriteItem(data2));

// 发送消息
Address addr{"192.168.1.1", 8080, Address::TCP}; // 目标地址
ioWorker.sendAsync(addr, std::move(list));
```

### 9.4 断开连接

```cpp
// 断开所有连接
ioWorker.dropConnections();

// 断开到特定地址的连接
Address addr{"192.168.1.1", 8080, Address::TCP};
ioWorker.dropConnections(addr);

// 检查并断开超时连接
Duration timeout = 60_s;
ioWorker.checkConnections(addr, timeout);
```

### 9.5 停止IOWorker

```cpp
// 停止IOWorker
ioWorker.stopAndJoin();
```

## 10. 总结

IOWorker是网络通信框架中负责处理所有IO任务的核心组件，它提供了以下主要功能：

1. **连接管理**：支持TCP和RDMA连接的添加、管理和释放
2. **消息发送**：提供异步消息发送和重试机制
3. **事件处理**：采用事件驱动模型高效处理IO事件
4. **任务调度**：结合协程和线程池实现异步任务处理
5. **错误处理**：提供完善的错误检测和恢复机制
6. **性能优化**：实现连接复用、并发限制和线程控制等优化策略

IOWorker的设计体现了以下特点：

1. **高效性**：通过事件驱动、连接复用和异步处理提高系统性能
2. **可靠性**：提供完善的错误处理和重试机制
3. **灵活性**：支持多种网络类型和配置选项
4. **可扩展性**：采用模块化设计，可以方便地扩展功能

IOWorker与Listener、Transport和Processor等组件配合，共同构成了完整的网络通信框架，为应用提供高性能、可靠的网络通信能力。 