/**
 * IovTable实现文件
 * 提供共享内存缓冲区的创建、注册、查找和释放功能
 */

#include "IovTable.h"

#include <folly/experimental/coro/BlockingWait.h>

#include "IoRing.h"
#include "fbs/meta/Common.h"

namespace hf3fs::fuse {

using hf3fs::lib::IorAttrs;

// 共享内存文件的基础路径前缀
const Path linkPref = "/dev/shm";

/**
 * 初始化IO向量表
 * @param mount 挂载点路径，用于标识当前实例
 * @param cap 最大容量，决定可同时管理的共享内存缓冲区数量
 */
void IovTable::init(const Path &mount, int cap) {
  mountName = mount.native();
  // 创建原子共享指针表，存储ShmBuf对象
  iovs = std::make_unique<AtomicSharedPtrTable<lib::ShmBuf>>(cap);
}

/**
 * IO向量属性结构，存储共享内存缓冲区的配置信息
 */
struct IovAttrs {
  Uuid id;                   // 唯一标识符
  size_t blockSize = 0;      // 块大小，用于IO操作
  bool isIoRing = false;     // 是否为IO环缓冲区
  bool forRead = true;       // 是否用于读操作
  int ioDepth = 0;           // IO深度，适用于IO环
  std::optional<IorAttrs> iora;  // IO环属性，可选
};

/**
 * 解析键字符串，提取配置信息
 * 键格式: UUID.配置1.配置2...
 * 支持的配置:
 *   b* - 块大小(字节)
 *   r*w* - IO环读/写模式及深度
 *   t* - 超时设置(毫秒)
 *   f* - 标志位(二进制)
 *   p[l/h/n] - 优先级(低/高/正常)
 * 
 * @param key 键字符串
 * @return 解析后的IO向量属性
 */
static Result<IovAttrs> parseKey(const char *key) {
  IovAttrs iova;

  // 按点分割键字符串
  std::vector<std::string> fnParts;
  folly::split('.', key, fnParts);

  // 第一部分必须是有效的UUID
  auto idRes = Uuid::fromHexString(fnParts[0]);
  RETURN_ON_ERROR(idRes);
  iova.id = *idRes;

  // 解析剩余的配置部分
  for (size_t i = 1; i < fnParts.size(); ++i) {
    auto dec = fnParts[i];
    switch (dec[0]) {
      case 'b': {  // 块大小设置
        auto i = atoll(dec.c_str() + 1);
        if (i <= 0) {
          return makeError(StatusCode::kInvalidArg, "invalid block size set in shm key");
        }
        iova.blockSize = (size_t)i;
        break;
      }

      case 'r':
      case 'w': {  // IO环配置
        auto i = atoll(dec.c_str() + 1);
        iova.isIoRing = true;
        iova.forRead = dec[0] == 'r';  // r表示读模式，w表示写模式
        iova.ioDepth = i;
        break;
      }

      case 't': {  // 超时设置
        if (!iova.iora) {
          iova.iora = IorAttrs{};
        }
        auto i = atoi(dec.c_str() + 1);
        if (i < 0) {
          return makeError(StatusCode::kInvalidArg, "invalid io job check timeout {}", dec.c_str() + 1);
        }
        // 转换为纳秒单位的Duration
        iova.iora->timeout = Duration(std::chrono::nanoseconds((uint64_t)i * 1000000));
        break;
      }

      case 'f': {  // 标志位设置
        if (!iova.iora) {
          iova.iora = IorAttrs{};
        }
        char *ep;
        auto i = strtoull(dec.c_str() + 1, &ep, 2);
        if (*ep != 0 || i < 0) {
          return makeError(StatusCode::kInvalidArg, "invalid io exec flags {}", dec.c_str() + 1);
        }
        iova.iora->flags = i;
        break;
      }

      case 'p':  // 优先级设置
        if (!iova.iora) {
          iova.iora = IorAttrs{};
        }
        switch (dec.c_str()[1]) {
          case 'l':  // 低优先级
            iova.iora->priority = 2;
            break;
          case 'h':  // 高优先级
            iova.iora->priority = 0;
            break;
          case 'n':  // 正常优先级
          case '\0':
            iova.iora->priority = 1;
            break;
          default:
            return makeError(StatusCode::kInvalidArg, "invalid priority set in shm key");
        }
        break;
    }
  }

  // 验证配置的一致性
  if (!iova.isIoRing && iova.iora) {
    return makeError(StatusCode::kInvalidArg, "ioring attrs set for non-ioring");
  }

  return iova;
}

// inode ID的起始偏移量
constexpr int iovIidStart = meta::InodeId::iovIidStart;

/**
 * 将inode ID转换为IO向量描述符
 * @param iid inode ID
 * @return 成功返回描述符索引，失败返回空
 */
std::optional<int> IovTable::iovDesc(meta::InodeId iid) {
  auto iidn = (ssize_t)iid.u64();
  auto diid = (ssize_t)meta::InodeId::iovDir().u64();
  // 检查inode ID是否在有效范围内
  if (iidn >= 0 || iidn > diid - iovIidStart || iidn < diid - std::numeric_limits<int>::max()) {
    return std::nullopt;
  }
  // 计算描述符索引
  return diid - iidn - iovIidStart;
}

/**
 * 添加新的IO向量缓冲区
 * @param key 键字符串 包含配置信息
 * @param shmPath 共享内存文件路径
 * @param pid 进程ID
 * @param ui 用户信息
 * @param exec 执行器
 * @param sc 存储客户端
 * @return 成功返回inode和共享内存缓冲区指针对，失败返回错误
 */
Result<std::pair<meta::Inode, std::shared_ptr<lib::ShmBuf>>> IovTable::addIov(const char *key,
                                                                              const Path &shmPath,
                                                                              pid_t pid,
                                                                              const meta::UserInfo &ui,
                                                                              folly::Executor::KeepAlive<> exec,
                                                                              storage::client::StorageClient &sc) {
  // 性能监控计数器
  static monitor::DistributionRecorder mapTimesCount("fuse.iov.times", monitor::TagSet{{"mount_name", mountName}});
  static monitor::DistributionRecorder mapBytesDist("fuse.iov.bytes", monitor::TagSet{{"mount_name", mountName}});
  static monitor::CountRecorder shmSizeCount("fuse.iov.total_bytes", monitor::TagSet{{"mount_name", mountName}}, false);
  static monitor::LatencyRecorder allocLatency("fuse.iov.latency.map", monitor::TagSet{{"mount_name", mountName}});
  static monitor::DistributionRecorder ibRegBytesDist("fuse.iov.bytes.ib_reg",
                                                      monitor::TagSet{{"mount_name", mountName}});
  static monitor::LatencyRecorder ibRegLatency("fuse.iov.latency.ib_reg", monitor::TagSet{{"mount_name", mountName}});

  // 解析键字符串，获取IO向量属性
  auto iovaRes = parseKey(key);
  RETURN_ON_ERROR(iovaRes);

  // 构建共享内存文件的路径
  Path shmOpenPath("/");
  shmOpenPath /= shmPath.lexically_relative(linkPref);

  // 检查共享内存文件状态
  struct stat st;
  if (stat(shmPath.c_str(), &st) == -1 || !S_ISREG(st.st_mode)) {
    return makeError(StatusCode::kInvalidArg, "failed to stat shm path or it's not a regular file");
  }

  // 验证配置参数的合法性
  if (iovaRes->blockSize > (size_t)st.st_size) {
    return makeError(StatusCode::kInvalidArg, "invalid block size set in shm key");
  } else if (iovaRes->isIoRing && iovaRes->ioDepth > IoRing::ioRingEntries((size_t)st.st_size)) {
    return makeError(StatusCode::kInvalidArg, "invalid io batch size set in shm key");
  }

  // 分配并初始化共享内存缓冲区
  while (true) {
    // 分配新的描述符
    auto iovdRes = iovs->alloc();
    if (!iovdRes) {
      return makeError(ClientAgentCode::kTooManyOpenFiles, "too many iovs allocated");
    }
    auto iovd = *iovdRes;
    bool dealloc = true;
    SCOPE_EXIT {
      if (dealloc) {
        iovs->dealloc(iovd);  // 失败时自动释放描述符
      }
    };

    auto start = SteadyClock::now();
    auto uids = std::to_string(ui.uid.toUnderType());

    // 创建共享内存缓冲区对象
    std::shared_ptr<lib::ShmBuf> shm;
    try {
      // 设置共享内存缓冲区及其自定义析构器
      shm.reset(
          new lib::ShmBuf(shmOpenPath, 0, st.st_size, iovaRes->blockSize, iovaRes->id),
          [uids,
           &shmSizeCount = shmSizeCount,
           &mapTimesCount = mapTimesCount,
           &mapBytesDist = mapBytesDist,
           &allocLatency = allocLatency,
           &ibRegLatency = ibRegLatency](auto p) {
            // 析构时取消RDMA注册
            auto start = SteadyClock::now();
            folly::coro::blockingWait(p->deregisterForIO());
            auto now = SteadyClock::now();
            ibRegLatency.addSample(now - start, monitor::TagSet{{"instance", "dereg"}, {"uid", uids}});

            // 解除内存映射
            start = now;
            p->unmapBuf();
            allocLatency.addSample(SteadyClock::now() - start, monitor::TagSet{{"instance", "free"}, {"uid", uids}});

            // 更新监控指标
            mapTimesCount.addSample(1, monitor::TagSet{{"instance", "free"}, {"uid", uids}});
            mapBytesDist.addSample(p->size, monitor::TagSet{{"instance", "free"}, {"uid", uids}});
            shmSizeCount.addSample(-p->size);

            delete p;
          });
    } catch (const std::runtime_error &e) {
      return makeError(ClientAgentCode::kIovShmFail, std::string("failed to open/map shm for iov ") + e.what());
    }

    // 记录分配性能指标
    allocLatency.addSample(SteadyClock::now() - start, monitor::TagSet{{"instance", "alloc"}, {"uid", uids}});
    mapTimesCount.addSample(1, monitor::TagSet{{"instance", "alloc"}, {"uid", uids}});
    mapBytesDist.addSample(shm->size, monitor::TagSet{{"instance", "alloc"}, {"uid", uids}});
    shmSizeCount.addSample(shm->size, monitor::TagSet{{"uid", uids}});

    // 设置共享内存缓冲区属性
    shm->key = key;
    shm->user = ui.uid;
    shm->pid = pid;
    shm->isIoRing = iovaRes->isIoRing;
    shm->forRead = iovaRes->forRead;
    shm->ioDepth = iovaRes->ioDepth;
    shm->iora = iovaRes->iora;

    // 存储共享内存缓冲区指针
    iovs->table[iovd].store(shm);

    start = SteadyClock::now();
    auto recordMetrics = [blockSize = shm->blockSize, start, uids]() mutable {
      ibRegBytesDist.addSample(blockSize, monitor::TagSet{{"instance", "reg"}, {"uid", uids}});
      ibRegLatency.addSample(SteadyClock::now() - start, monitor::TagSet{{"instance", "reg"}, {"uid", uids}});
    };

    // 对于非IO环缓冲区，注册RDMA IO
    if (!iovaRes->isIoRing) {
      folly::coro::blockingWait(shm->registerForIO(exec, sc, recordMetrics));
    }

    // 更新键到描述符的映射
    {
      std::unique_lock lock(iovdLock_);
      iovds_[key] = iovd;
    }

    // 更新UUID到描述符的映射
    {
      std::unique_lock lock(shmLock);
      shmsById[iovaRes->id] = iovd;
    }

    // 获取inode信息
    auto statRes = statIov(iovd, ui);
    RETURN_ON_ERROR(statRes);

    dealloc = false;
    // 返回inode和共享内存缓冲区指针对
    return std::make_pair(*statRes, iovaRes->isIoRing ? shm : std::shared_ptr<lib::ShmBuf>());
  }
}