# RDMABuf实现原理

## 1. 概述

RDMABuf是IB通信模块中负责管理RDMA内存缓冲区的核心组件，它提供了一套完整的机制来实现RDMA内存的分配、注册、管理和回收。本文档主要分析RDMABuf相关文件的设计原理和调用规则，包括：

- `RDMABuf.h`：定义了RDMA缓冲区相关的数据结构和接口
- `RDMABuf.cc`：实现了RDMA缓冲区的核心逻辑

RDMA（Remote Direct Memory Access）技术允许网络适配器直接访问应用程序内存，无需操作系统和CPU的参与，从而实现高性能、低延迟的数据传输。RDMABuf模块为RDMA操作提供了必要的内存管理支持，是IBSocket进行RDMA通信的基础。

## 2. 核心数据结构

### 2.1 RDMARemoteBuf

`RDMARemoteBuf`类用于表示远程节点上的RDMA内存缓冲区，包含远程内存的地址、大小和访问密钥（rkey）：

```cpp
class RDMARemoteBuf {
  struct Rkey {
    uint32_t rkey = 0;
    int devId = -1;
  };

public:
  RDMARemoteBuf()
      : addr_(0),
        length_(0),
        rkeys_() {}

  RDMARemoteBuf(uint64_t addr, size_t length, std::array<Rkey, IBDevice::kMaxDeviceCnt> rkeys)
      : addr_(addr),
        length_(length),
        rkeys_(rkeys) {}

  uint64_t addr() const { return addr_; }
  size_t size() const { return length_; }

  std::optional<uint32_t> getRkey(int devId) const;
  
  // 其他方法...

private:
  uint64_t addr_;
  uint64_t length_;
  std::array<Rkey, IBDevice::kMaxDeviceCnt> rkeys_;
};
```

`RDMARemoteBuf`提供了一系列方法来操作远程缓冲区：

1. **基本访问方法**：
   - `addr()`：获取远程内存地址
   - `size()`：获取缓冲区大小
   - `getRkey(int devId)`：获取特定设备的远程访问密钥

2. **缓冲区操作方法**：
   - `advance(size_t len)`：前移缓冲区指针
   - `subtract(size_t n)`：减少缓冲区大小
   - `subrange(size_t offset, size_t len)`：获取子范围
   - `first(size_t len)`/`takeFirst(size_t len)`：获取/截取前部分
   - `last(size_t len)`/`takeLast(size_t len)`：获取/截取后部分

这些方法使得`RDMARemoteBuf`可以灵活地表示和操作远程内存区域，支持分段访问和处理。

### 2.2 RDMABuf

`RDMABuf`类用于表示本地RDMA内存缓冲区，它负责内存的分配、注册和管理：

```cpp
class RDMABuf {
public:
  RDMABuf();
  
  // 构造和赋值操作
  RDMABuf(const RDMABuf &) = default;
  RDMABuf(RDMABuf &&) = default;
  RDMABuf &operator=(const RDMABuf &) = default;
  RDMABuf &operator=(RDMABuf &&) = default;
  
  // 静态分配方法
  static RDMABuf allocate(size_t size);
  static RDMABuf createFromUserBuffer(uint8_t *buf, size_t len);
  
  // 基本访问方法
  bool valid() const;
  const uint8_t *ptr() const;
  uint8_t *ptr();
  size_t capacity() const;
  size_t size() const;
  
  // 缓冲区操作方法
  void resetRange();
  bool advance(size_t n);
  bool subtract(size_t n);
  RDMABuf subrange(size_t offset, size_t length) const;
  RDMABuf first(size_t length) const;
  RDMABuf takeFirst(size_t length);
  RDMABuf last(size_t length) const;
  RDMABuf takeLast(size_t length);
  
  // RDMA相关方法
  RDMABufMR getMR(int dev) const;
  RDMARemoteBuf toRemoteBuf() const;
  
  // 类型转换操作符
  operator RDMARemoteBuf();
  operator std::span<const uint8_t>() const;
  operator std::span<uint8_t>();
  operator folly::MutableByteRange();
  operator folly::ByteRange() const;

private:
  class Inner;  // 内部实现类
  
  std::shared_ptr<Inner> buf_;
  uint8_t *begin_;
  size_t length_;
};
```

`RDMABuf`的核心功能包括：

1. **内存分配和注册**：
   - `allocate(size_t size)`：分配指定大小的内存并注册到IB设备
   - `createFromUserBuffer(uint8_t *buf, size_t len)`：使用用户提供的内存创建RDMA缓冲区

2. **内存访问和操作**：
   - 与`RDMARemoteBuf`类似的缓冲区操作方法
   - 支持转换为多种内存表示形式（span、ByteRange等）

3. **RDMA操作支持**：
   - `getMR(int dev)`：获取特定设备的内存注册句柄
   - `toRemoteBuf()`：转换为远程缓冲区表示

### 2.3 RDMABuf::Inner

`RDMABuf::Inner`是`RDMABuf`的内部实现类，负责实际的内存管理和设备注册：

```cpp
class RDMABuf::Inner : folly::MoveOnly {
public:
  static constexpr int kAccessFlags =
      IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_READ | IBV_ACCESS_RELAXED_ORDERING;

  Inner(size_t capacity);
  Inner(std::weak_ptr<RDMABufPool> pool, size_t capacity);
  Inner(uint8_t *buf, size_t len);
  Inner(Inner &&o);
  ~Inner();

  static void deallocate(Inner *ptr);

  int init();
  int allocateMemory();
  int registerMemory();

  RDMABufMR getMR(int dev) const;
  bool getRkeys(std::array<RDMARemoteBuf::Rkey, IBDevice::kMaxDeviceCnt> &rkeys) const;

  const uint8_t *ptr() const;
  uint8_t *ptr();
  size_t capacity() const;

private:
  std::weak_ptr<RDMABufPool> pool_;
  uint8_t *ptr_;
  size_t capacity_;
  std::array<RDMABufMR, IBDevice::kMaxDeviceCnt> mrs_;
  bool userBuffer_;
};
```

`Inner`类的主要职责包括：

1. **内存管理**：
   - 分配和释放内存
   - 支持用户提供的外部内存

2. **设备注册**：
   - 将内存注册到所有IB设备
   - 维护每个设备的内存注册句柄（MR）

3. **资源回收**：
   - 在析构时注销内存注册并释放内存
   - 支持通过内存池回收缓冲区

### 2.4 RDMABufPool

`RDMABufPool`类实现了RDMA缓冲区的池化管理，通过重用缓冲区减少内存分配和注册的开销：

```cpp
class RDMABufPool : public std::enable_shared_from_this<RDMABufPool>, folly::MoveOnly {
public:
  static std::shared_ptr<RDMABufPool> create(size_t bufSize, size_t bufCnt);
  
  CoTask<RDMABuf> allocate(std::optional<folly::Duration> timeout = std::nullopt);
  void deallocate(RDMABuf::Inner *buf);
  
  size_t bufSize() const;
  size_t freeCnt() const;
  size_t totalCnt() const;

private:
  RDMABufPool(PrivateTag, size_t bufSize, size_t bufCnt);
  ~RDMABufPool();
  
  size_t bufSize_;
  std::atomic<size_t> bufAllocated_;
  
  folly::fibers::Semaphore sem_;
  std::mutex mutex_;
  std::deque<RDMABuf::Inner *> freeList_;
};
```

`RDMABufPool`的核心功能包括：

1. **缓冲区分配**：
   - `allocate()`：分配一个RDMA缓冲区，优先从空闲列表中获取
   - 支持超时机制，避免长时间等待

2. **缓冲区回收**：
   - `deallocate()`：回收RDMA缓冲区到空闲列表
   - 使用信号量控制并发分配

3. **资源管理**：
   - 维护固定大小的缓冲区池
   - 提供缓冲区使用情况的统计信息

## 3. 内存管理机制

### 3.1 内存分配和注册

RDMA缓冲区的创建涉及两个关键步骤：内存分配和设备注册。

#### 3.1.1 内存分配

`RDMABuf::Inner::allocateMemory()`方法负责分配内存：

```cpp
int RDMABuf::Inner::allocateMemory() {
  static size_t kPageSize = sysconf(_SC_PAGESIZE);

  ptr_ = (uint8_t *)hf3fs::memory::memalign(kPageSize, capacity_);
  if (!ptr_) {
    XLOGF(CRITICAL, "RDMABuf failed to allocate memory, len {}.", capacity_);
    return -ENOMEM;
  }
  rdmaBufMem.addSample(capacity_);

  XLOGF(DBG, "RDMABuf allocated, ptr {}, capacity {}", (void *)ptr_, capacity_);
  return 0;
}
```

内存分配的特点：
1. 使用页对齐的内存（`memalign`），这是RDMA操作的要求
2. 记录内存使用情况，用于监控
3. 分配失败时返回错误码

#### 3.1.2 设备注册

`RDMABuf::Inner::registerMemory()`方法负责将内存注册到所有IB设备：

```cpp
int RDMABuf::Inner::registerMemory() {
  size_t devs = 0;
  for (auto &dev : IBDevice::all()) {
    XLOGF_IF(FATAL,
             UNLIKELY(dev->id() >= IBDevice::kMaxDeviceCnt || (devs++) >= IBDevice::kMaxDeviceCnt),
             "{} >= {} || {} >= {}, {}",
             dev->id(),
             IBDevice::kMaxDeviceCnt,
             devs,
             IBDevice::kMaxDeviceCnt,
             IBDevice::all().size());
    auto mr = dev->regMemory(ptr_, capacity_, kAccessFlags);
    if (!mr) {
      return -1;
    }
    mrs_[dev->id()] = mr;
  }

  XLOGF(DBG, "RDMABuf registered, ptr {}, capacity {}", (void *)ptr_, capacity_);
  return 0;
}
```

设备注册的特点：
1. 将内存注册到系统中的所有IB设备
2. 使用特定的访问标志（`kAccessFlags`）
3. 保存每个设备的内存注册句柄（MR）
4. 任何设备注册失败都会导致整个操作失败

### 3.2 内存池化管理

`RDMABufPool`实现了RDMA缓冲区的池化管理，通过重用缓冲区减少内存分配和注册的开销。

#### 3.2.1 缓冲区分配

`RDMABufPool::allocate()`方法实现了缓冲区的分配逻辑：

```cpp
CoTask<RDMABuf> RDMABufPool::allocate(std::optional<folly::Duration> timeout) {
  // 等待可用的RDMA缓冲区
  if (UNLIKELY(!sem_.try_wait())) {
    if (timeout.has_value()) {
      auto result = co_await folly::coro::co_awaitTry(folly::coro::timeout(sem_.co_wait(), timeout.value()));
      if (result.hasException()) {
        co_return RDMABuf();
      }
    } else {
      co_await sem_.co_wait();
    }
  }

  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!freeList_.empty()) {
      auto *buf = freeList_.back();
      freeList_.pop_back();
      co_return RDMABuf(buf);
    }
  }

  co_return RDMABuf::allocate(bufSize(), shared_from_this());
}
```

缓冲区分配的特点：
1. 使用信号量控制并发分配，确保不超过池的容量
2. 支持超时机制，避免长时间等待
3. 优先从空闲列表中获取缓冲区
4. 如果没有空闲缓冲区，则创建新的缓冲区

#### 3.2.2 缓冲区回收

`RDMABufPool::deallocate()`方法实现了缓冲区的回收逻辑：

```cpp
void RDMABufPool::deallocate(RDMABuf::Inner *buf) {
  std::lock_guard<std::mutex> lock(mutex_);
  freeList_.push_back(buf);
  sem_.signal();
}
```

缓冲区回收的特点：
1. 将缓冲区放回空闲列表
2. 增加信号量，允许更多的分配操作

### 3.3 资源释放机制

`RDMABuf::Inner`的析构函数负责释放RDMA缓冲区的资源：

```cpp
RDMABuf::Inner::~Inner() {
  XLOGF(DBG, "RDMABuf free and deregister, ptr {}", (void *)ptr_);
  for (auto &dev : IBDevice::all()) {
    XLOGF_IF(FATAL, UNLIKELY(dev->id() >= IBDevice::kMaxDeviceCnt), "{} > {}", dev->id(), IBDevice::kMaxDeviceCnt);
    auto mr = mrs_.at(dev->id());
    if (mr) {
      dev->deregMemory(mr);
    }
  }
  if (ptr_ && !userBuffer_) {
    rdmaBufMem.addSample(-capacity_);
    hf3fs::memory::deallocate(ptr_);
  }
}
```

资源释放的特点：
1. 从所有IB设备注销内存注册
2. 只有非用户提供的缓冲区才会释放内存
3. 更新内存使用统计信息

## 4. 缓冲区操作

### 4.1 基本操作

RDMABuf和RDMARemoteBuf都提供了一系列方法来操作缓冲区：

#### 4.1.1 范围操作

```cpp
// 前移缓冲区指针
bool advance(size_t n);

// 减少缓冲区大小
bool subtract(size_t n);

// 获取子范围
RDMABuf subrange(size_t offset, size_t length) const;

// 获取前部分
RDMABuf first(size_t length) const;

// 截取前部分
RDMABuf takeFirst(size_t length);

// 获取后部分
RDMABuf last(size_t length) const;

// 截取后部分
RDMABuf takeLast(size_t length);
```

这些方法使得可以灵活地操作缓冲区，支持分段处理和数据切片。

#### 4.1.2 类型转换

RDMABuf提供了多种类型转换操作符，方便与其他内存表示形式互操作：

```cpp
// 转换为远程缓冲区
operator RDMARemoteBuf();

// 转换为std::span
operator std::span<const uint8_t>() const;
operator std::span<uint8_t>();

// 转换为folly::ByteRange
operator folly::MutableByteRange();
operator folly::ByteRange() const;
```

这些转换操作符使得RDMABuf可以无缝地与标准库和Folly库的数据结构配合使用。

### 4.2 RDMA操作支持

#### 4.2.1 获取内存注册句柄

```cpp
RDMABufMR RDMABuf::getMR(int dev) const {
  if (UNLIKELY(buf_ == nullptr)) {
    return RDMABufMR();
  }
  return buf_->getMR(dev);
}
```

这个方法用于获取特定设备的内存注册句柄，是执行RDMA操作的必要条件。

#### 4.2.2 转换为远程缓冲区

```cpp
RDMARemoteBuf RDMABuf::toRemoteBuf() const {
  std::array<RDMARemoteBuf::Rkey, IBDevice::kMaxDeviceCnt> rkeys;
  if (UNLIKELY(!buf_ || !buf_->getRkeys(rkeys))) {
    return RDMARemoteBuf();
  }
  return RDMARemoteBuf((uint64_t)begin_, length_, rkeys);
}
```

这个方法将本地缓冲区转换为远程缓冲区表示，包含了远程访问所需的所有信息（地址、大小和访问密钥）。

## 5. 序列化支持

RDMARemoteBuf实现了序列化和反序列化支持，使其可以通过网络传输：

```cpp
template <>
struct ::hf3fs::serde::SerdeMethod<::hf3fs::net::RDMARemoteBuf> {
  static constexpr auto serialize(const net::RDMARemoteBuf &buf, auto &out) {
    // 序列化实现...
  }

  static Result<Void> deserialize(net::RDMARemoteBuf &buf, auto &&in) {
    // 反序列化实现...
  }

  static auto serializeReadable(const net::RDMARemoteBuf &buf, auto &out) {
    // 可读序列化实现...
  }

  static std::string serdeToReadable(const net::RDMARemoteBuf &rdmabuf) {
    return serde::toJsonString(rdmabuf);
  }

  static Result<net::RDMARemoteBuf> serdeFromReadable(const std::string &str) {
    net::RDMARemoteBuf rdmabuf;
    auto result = serde::fromJsonString(rdmabuf, str);
    if (result) return rdmabuf;
    return makeError(result.error());
  }
};
```

序列化支持的特点：
1. 支持二进制序列化和反序列化
2. 支持可读的JSON格式序列化和反序列化
3. 序列化包含远程访问所需的所有信息

## 6. 调用规则

### 6.1 创建和使用RDMABuf

#### 6.1.1 直接分配

```cpp
// 分配指定大小的RDMA缓冲区
RDMABuf buf = RDMABuf::allocate(1024);  // 分配1KB的缓冲区

// 检查分配是否成功
if (!buf.valid()) {
  // 处理分配失败
}

// 使用缓冲区
memcpy(buf.ptr(), data, dataSize);

// 执行RDMA操作
socket->rdmaWrite(remoteBuf, buf);
```

#### 6.1.2 使用用户提供的内存

```cpp
// 使用已有的内存创建RDMA缓冲区
uint8_t* data = new uint8_t[1024];
RDMABuf buf = RDMABuf::createFromUserBuffer(data, 1024);

// 检查创建是否成功
if (!buf.valid()) {
  // 处理创建失败
  delete[] data;
}

// 使用缓冲区
// 注意：内存由用户负责管理，RDMABuf不会释放它
socket->rdmaWrite(remoteBuf, buf);

// 在不再需要RDMA访问时释放内存
delete[] data;
```

#### 6.1.3 使用内存池

```cpp
// 创建内存池
auto pool = RDMABufPool::create(1024, 10);  // 10个1KB的缓冲区

// 分配缓冲区
RDMABuf buf = co_await pool->allocate();

// 检查分配是否成功
if (!buf.valid()) {
  // 处理分配失败
}

// 使用缓冲区
memcpy(buf.ptr(), data, dataSize);
socket->rdmaWrite(remoteBuf, buf);

// 缓冲区会自动返回到池中
```

### 6.2 创建和使用RDMARemoteBuf

#### 6.2.1 从本地缓冲区创建

```cpp
// 从本地缓冲区创建远程缓冲区表示
RDMABuf localBuf = RDMABuf::allocate(1024);
RDMARemoteBuf remoteBuf = localBuf.toRemoteBuf();

// 或者使用转换操作符
RDMARemoteBuf remoteBuf = localBuf;
```

#### 6.2.2 通过网络接收

```cpp
// 反序列化远程缓冲区
RDMARemoteBuf remoteBuf;
auto result = serde::fromJsonString(remoteBuf, jsonStr);
if (!result) {
  // 处理反序列化失败
}

// 使用远程缓冲区
RDMABuf localBuf = RDMABuf::allocate(remoteBuf.size());
socket->rdmaRead(remoteBuf, localBuf);
```

#### 6.2.3 操作远程缓冲区

```cpp
// 获取子范围
RDMARemoteBuf subBuf = remoteBuf.subrange(offset, length);

// 截取前部分
RDMARemoteBuf firstPart = remoteBuf.takeFirst(length);

// 检查是否有效
if (!subBuf.valid()) {
  // 处理无效缓冲区
}
```

### 6.3 RDMA操作

```cpp
// RDMA读操作
socket->rdmaRead(remoteBuf, localBuf);

// RDMA写操作
socket->rdmaWrite(remoteBuf, localBuf);

// 批量RDMA操作
auto batch = socket->rdmaReadBatch();
batch.add(remoteBuf1, localBuf1);
batch.add(remoteBuf2, localBuf2);
co_await batch.post();
```

## 7. 性能优化

### 7.1 内存池化

RDMABufPool通过重用缓冲区减少内存分配和注册的开销，这是一个重要的性能优化：

1. **减少内存分配**：内存分配是一个相对昂贵的操作，特别是对于大块内存
2. **减少设备注册**：内存注册到IB设备是一个更加昂贵的操作，涉及到DMA映射和页表更新
3. **提高并发性能**：使用信号量控制并发分配，避免资源竞争

### 7.2 页对齐内存

RDMABuf使用页对齐的内存，这有几个好处：

1. **提高DMA效率**：页对齐的内存更适合DMA操作
2. **减少内存拷贝**：某些RDMA操作要求内存对齐，使用页对齐内存可以避免额外的拷贝
3. **提高缓存效率**：对齐的内存访问通常有更好的缓存性能

### 7.3 缓冲区操作优化

RDMABuf和RDMARemoteBuf提供了丰富的缓冲区操作方法，这些方法都经过优化，尽量避免不必要的内存拷贝：

1. **子范围操作**：创建子缓冲区时不会复制内存，而是共享底层内存
2. **移动语义**：支持移动构造和移动赋值，避免不必要的引用计数操作
3. **引用计数**：使用`std::shared_ptr`管理内存生命周期，避免内存泄漏和过早释放

## 8. 错误处理

RDMABuf模块使用多种机制进行错误处理：

1. **返回值检查**：内存分配和注册方法返回错误码，调用者需要检查这些错误码
2. **有效性检查**：提供`valid()`方法检查缓冲区是否有效
3. **断言**：使用断言检查内部不变量，特别是在调试模式下
4. **日志记录**：使用XLOG记录关键操作和错误情况
5. **异常处理**：协程操作支持异常处理，如超时异常

## 9. 监控和统计

RDMABuf模块使用监控和统计机制跟踪资源使用情况：

```cpp
monitor::CountRecorder rdmaBufMem("common.ib.rdma_buf_mem", {}, false);
```

这个计数器记录了RDMA缓冲区使用的内存总量，可以用于监控系统性能和资源使用情况。

## 10. 总结

RDMABuf模块是IB通信框架中负责RDMA内存管理的核心组件，它提供了一套完整的机制来实现RDMA内存的分配、注册、管理和回收。主要特点包括：

1. **灵活的内存管理**：支持直接分配、用户提供内存和内存池化
2. **高效的缓冲区操作**：丰富的缓冲区操作方法，支持分段处理和数据切片
3. **完善的RDMA支持**：提供获取内存注册句柄和转换为远程缓冲区的功能
4. **序列化支持**：支持远程缓冲区的序列化和反序列化，便于网络传输
5. **性能优化**：内存池化、页对齐内存和缓冲区操作优化
6. **错误处理**：多种错误处理机制，提高系统可靠性
7. **监控和统计**：跟踪资源使用情况，便于性能调优

RDMABuf模块与IBSocket和IBConnect模块紧密配合，共同构成了完整的IB通信框架，为应用提供高性能的RDMA通信能力。 