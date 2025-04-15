#pragma once

#include <cstdint>
#include <semaphore.h>

#include "IovTable.h"
#include "UserConfig.h"
#include "client/storage/StorageClient.h"
#include "common/utils/AtomicSharedPtrTable.h"
#include "common/utils/Coroutine.h"
#include "common/utils/Uuid.h"
#include "fbs/meta/Schema.h"
#include "lib/common/Shm.h"

namespace hf3fs::fuse {
  struct RcInode;
  struct IoArgs {
    uint8_t bufId[16];
    size_t bufOff;

    uint64_t fileIid;
    size_t fileOff;

    uint64_t ioLen;

    const void *userdata;
  };

  struct IoSqe {
    int32_t index;
    const void *userdata;
  };

  struct IoCqe {
    int32_t index;
    int32_t reserved;
    int64_t result;
    const void *userdata;
  };

  class IoRing;

  struct IoRingJob {
    std::shared_ptr<IoRing> ior;
    int sqeProcTail;
    int toProc;
  };

  // we allow multiple io workers to process the same ioring, but different ranges
  // so 1 ioring can be used to submit ios processed in parallel
  // howoever, we don't allow multiple threads to prepare ios in the same ioring
  // or batches may be mixed and things may get ugly
  /**
   * IoRing类 - 管理高效的异步IO请求环
   * 
   * 通过共享内存实现进程间IO请求提交和完成通知，类似Linux kernel的io_uring机制
   * 支持多线程并行处理，批量IO请求，优先级调度和超时控制
   */
  class IoRing : public std::enable_shared_from_this<IoRing> {
  public:
    /**
     * 计算环标记所需的内存对齐大小
     * @return 对齐后的标记大小（字节）
     */
    static int ringMarkerSize() {
      auto n = std::atomic_ref<int32_t>::required_alignment;
      return (4 + n - 1) / n * n;
    }
    
    /**
     * 计算给定缓冲区大小能容纳的最大IO请求数量
     * @param bufSize 共享内存缓冲区大小
     * @return 最大可容纳的IO请求条目数（减1是为了检测队列空/满状态）
     */
    static int ioRingEntries(size_t bufSize) {
      auto n = ringMarkerSize();
      // n * 4 用于sqe/cqe的头尾标记
      return (int)std::min((size_t)std::numeric_limits<int>::max(),
                          (bufSize - 4096 - n * 4 - sizeof(sem_t)) / (sizeof(IoArgs) + sizeof(IoCqe) + sizeof(IoSqe))) - 1;
    }
    
    /**
     * 计算指定条目数量所需的共享内存大小
     * @param entries IO请求条目数
     * @return 所需的字节数
     */
    static size_t bytesRequired(int entries) {
      auto n = ringMarkerSize();
      return n * 4 + sizeof(sem_t) + (sizeof(IoArgs) + sizeof(IoCqe) + sizeof(IoSqe)) * (entries + 1) + 4096;
    }

  public:
    /**
     * 构造IoRing对象
     * @param shm 共享内存缓冲区
     * @param nm 环名称
     * @param ui 用户信息
     * @param read 是否为读操作环
     * @param buf 共享内存基地址
     * @param size 缓冲区大小
     * @param iod IO深度（批处理大小）
     * @param prio 优先级（0-高，1-中，2-低）
     * @param to 超时设置
     * @param flags 操作标志
     * @param owner 是否为所有者（负责初始化信号量）
     */
    IoRing(std::shared_ptr<lib::ShmBuf> shm,
          std::string_view nm,
          const meta::UserInfo &ui,
          bool read,
          uint8_t *buf,
          size_t size,
          int iod,
          int prio,
          Duration to,
          uint64_t flags,
          bool owner = true)
        : name(nm),
          entries(ioRingEntries(size) + 1),
          ioDepth(iod),
          priority(prio),
          timeout(to),
          sqeHead_((int32_t *)buf),
          sqeTail_((int32_t *)(buf + ringMarkerSize())),
          cqeHead_((int32_t *)(buf + ringMarkerSize() * 2)),
          cqeTail_((int32_t *)(buf + ringMarkerSize() * 3)),
          sqeHead(*sqeHead_),
          sqeTail(*sqeTail_),
          cqeHead(*cqeHead_),
          cqeTail(*cqeTail_),
          ringSection((IoArgs *)(buf + ringMarkerSize() * 4)),
          cqeSection((IoCqe *)(ringSection + entries)),
          sqeSection((IoSqe *)(cqeSection + entries)),
          slots(entries - 1),
          shm_(std::move(shm)),
          userInfo_(ui),
          forRead_(read),
          flags_(flags) {
      // 确保内存布局正确
      XLOGF_IF(FATAL,
              (uintptr_t)(sqeSection + entries + sizeof(sem_t)) > (uintptr_t)(buf + size),
              "sem has a bad address {}, after whole shm starts at {} with {} bytes",
              (void *)(sqeSection + entries + sizeof(sem_t)),
              (void *)buf,
              size);
      // 初始化信号量
      auto sem = (sem_t *)(sqeSection + entries);
      if (owner) {
        sem_init(sem, 1, 0);  // 进程间共享的信号量
      }
      cqeSem.reset(sem);
    }
    
    /**
     * 获取待处理的作业
     * @param maxJobs 最大获取作业数
     * @return 待处理的作业列表
     */
    std::vector<IoRingJob> jobsToProc(int maxJobs);
    
    /**
     * 获取完成队列中的条目数量
     * @return 完成队列中的条目数
     */
    int cqeCount() const { return (cqeHead.load() + entries - cqeTail.load()) % entries; }
    
    /**
     * 处理IO请求批次
     * @param spt 起始处理位置
     * @param toProc 要处理的请求数量
     * @param storageClient 存储客户端
     * @param storageIo 存储IO选项
     * @param userConfig 用户配置
     * @param lookupFiles 文件查找回调
     * @param lookupBufs 缓冲区查找回调
     * @return 异步任务
     */
    CoTask<void> process(
        int spt,
        int toProc,
        storage::client::StorageClient &storageClient,
        const storage::client::IoOptions &storageIo,
        UserConfig &userConfig,
        std::function<void(std::vector<std::shared_ptr<RcInode>> &, const IoArgs *, const IoSqe *, int)> &&lookupFiles,
        std::function<void(std::vector<Result<lib::ShmBufForIO>> &, const IoArgs *, const IoSqe *, int)> &&lookupBufs);

  public:
    /**
     * 添加提交队列条目
     * @param idx 参数索引
     * @param userdata 用户数据指针
     * @return 添加是否成功
     */
    bool addSqe(int idx, const void *userdata) {
      auto h = sqeHead.load();
      // 检查队列是否已满
      if ((h + 1) % entries == sqeTail.load()) {
        return false;
      }

      // 填充SQE条目
      auto &sqe = sqeSection[h];
      sqe.index = idx;
      sqe.userdata = userdata;

      // 更新头指针
      sqeHead.store((h + 1) % entries);

      return true;
    }
    
    /**
     * 判断两个尾指针的先后顺序
     * @param a 第一个尾指针
     * @param b 第二个尾指针
     * @return a是否在b之后
     */
    bool sqeTailAfter(int a, int b) {
      auto h = sqeHead.load();
      if (a == h) {  // 赶上了头指针，必然是最后的
        return true;
      }
      auto ah = a > h, bh = b > h;
      if (ah == bh) {  // 都在头指针之前或之后，较大的在后
        return a > b;
      } else {  // 在头指针之前的反而在后
        return bh;
      }
    }

  public:
    std::string name;
    std::string mountName;
    int entries;
    int ioDepth;
    int priority;
    Duration timeout;

  private:
    int32_t *sqeHead_;
    int32_t *sqeTail_;
    int32_t *cqeHead_;
    int32_t *cqeTail_;
    std::optional<SteadyTime> lastCheck_;

  public:
    std::atomic_ref<int32_t> sqeHead;
    std::atomic_ref<int32_t> sqeTail;
    std::atomic_ref<int32_t> cqeHead;
    std::atomic_ref<int32_t> cqeTail;
    IoArgs *ringSection;
    IoCqe *cqeSection;
    IoSqe *sqeSection;
    std::unique_ptr<sem_t, std::function<void(sem_t *)>> cqeSem{nullptr, [](sem_t *p) { sem_destroy(p); }};

  public:
    AvailSlots slots;

  private:
    int sqeCount() const { return (sqeHead.load() + entries - sqeProcTail_) % entries; }
    [[nodiscard]] bool addCqe(int idx, ssize_t res, const void *userdata) {
      auto h = cqeHead.load();
      if ((h + 1) % entries == cqeTail.load()) {
        return false;
      }

      auto &cqe = cqeSection[h];
      cqe.index = idx;
      cqe.result = res;
      cqe.userdata = userdata;

      cqeHead.store((h + 1) % entries);
      return true;
    }

  private:  // for fuse
    std::shared_ptr<lib::ShmBuf> shm_;
    meta::UserInfo userInfo_;
    bool forRead_;
    uint64_t flags_;
    std::mutex cqeMtx_;  // when reporting cqes
    int sqeProcTail_{0};
    int processing_{0};
    std::deque<int> sqeProcTails_;  // tails claimed and processing
    std::set<int> sqeDoneTails_;    // tails done processing
  };

  struct IoRingTable {
    void init(int cap) {
      for (int prio = 0; prio <= 2; ++prio) {
        auto sp = "/" + semOpenPath(prio);
        sems.emplace_back(sem_open(sp.c_str(), O_CREAT, 0666, 0), [sp](sem_t *p) {
          sem_close(p);
          sem_unlink(sp.c_str());
        });
        chmod(semPath(prio).c_str(), 0666);
      }
      ioRings = std::make_unique<AtomicSharedPtrTable<IoRing>>(cap);
    }
    Result<int> addIoRing(const Path &mountName,
                          std::shared_ptr<lib::ShmBuf> shm,
                          std::string_view name,
                          const meta::UserInfo &ui,
                          bool forRead,
                          uint8_t *buf,
                          size_t size,
                          int ioDepth,
                          const hf3fs::lib::IorAttrs &attrs) {
      auto idxRes = ioRings->alloc();
      if (!idxRes) {
        return makeError(ClientAgentCode::kTooManyOpenFiles, "too many io rings");
      }

      auto idx = *idxRes;

      auto ior = std::make_shared<IoRing>(std::move(shm), name, ui, forRead, buf, size, ioDepth, attrs.priority, attrs.timeout, attrs.flags);
      ior->mountName = mountName.native();
      ioRings->table[idx].store(ior);

      return idx;
    }
    void rmIoRing(int idx) { ioRings->remove(idx); }
    std::vector<std::unique_ptr<sem_t, std::function<void(sem_t *)>>> sems;
    std::unique_ptr<AtomicSharedPtrTable<IoRing>> ioRings;

  private:
    static std::string semOpenPath(int prio) {
      static std::vector<Uuid> semIds{Uuid::random(), Uuid::random(), Uuid::random()};
      return fmt::format("hf3fs-submit-ios.{}", semIds[prio].toHexString());
    }

  public:
    static std::string semName(int prio) {
      return fmt::format("submit-ios{}", prio == 1 ? "" : prio == 0 ? ".ph" : ".pl");
    }
    static Path semPath(int prio) { return Path("/dev/shm") / ("sem." + semOpenPath(prio)); }
    static meta::Inode lookupSem(int prio) {
      static const std::vector<meta::Inode> inodes{
          {meta::InodeId{meta::InodeId::iovDir().u64() - 1},
          meta::InodeData{meta::Symlink{semPath(0)}, meta::Acl{meta::Uid{0}, meta::Gid{0}, meta::Permission{0666}}}},
          {meta::InodeId{meta::InodeId::iovDir().u64() - 2},
          meta::InodeData{meta::Symlink{semPath(1)}, meta::Acl{meta::Uid{0}, meta::Gid{0}, meta::Permission{0666}}}},
          {meta::InodeId{meta::InodeId::iovDir().u64() - 3},
          meta::InodeData{meta::Symlink{semPath(2)}, meta::Acl{meta::Uid{0}, meta::Gid{0}, meta::Permission{0666}}}}};

      return inodes[prio];
    }
  };
}  // namespace hf3fs::fuse
