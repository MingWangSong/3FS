#pragma once

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <folly/MPMCQueue.h>
#include <folly/Math.h>
#include <folly/Synchronized.h>
#include <folly/Utility.h>
#include <folly/executors/IOThreadPoolExecutor.h>
#include <folly/experimental/coro/Mutex.h>
#include <folly/fibers/Semaphore.h>
#include <folly/logging/xlog.h>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <sys/types.h>
#include <thread>
#include <utility>

#include "common/utils/BackgroundRunner.h"
#include "common/utils/CoroutinesPool.h"
#include "common/utils/Result.h"
#include "common/utils/Semaphore.h"
#include "common/utils/UtcTime.h"
#include "fbs/core/user/User.h"
#include "fbs/meta/Common.h"
#define FUSE_USE_VERSION 312
#define OP_LOG_LEVEL DBG

#include <folly/concurrency/AtomicSharedPtr.h>
#include <fuse3/fuse_lowlevel.h>

#include "FuseConfig.h"
#include "IoRing.h"
#include "IovTable.h"
#include "PioV.h"
#include "UserConfig.h"
#include "client/meta/MetaClient.h"
#include "client/mgmtd/MgmtdClientForClient.h"
#include "client/storage/StorageClient.h"
#include "fbs/meta/Schema.h"

namespace hf3fs::fuse {
using flat::Gid;
using flat::Uid;
using flat::UserInfo;
using lib::agent::PioV;
using meta::Acl;
using meta::Directory;
using meta::DirEntry;
using meta::Inode;
using meta::InodeData;
using meta::InodeId;
using meta::Permission;
using storage::client::IOBuffer;

struct InodeWriteBuf {
  std::vector<uint8_t> buf;
  std::unique_ptr<storage::client::IOBuffer> memh;
  off_t off{0};
  size_t len{0};
};

struct RcInode {
  struct DynamicAttr {
    uint64_t written = 0;
    uint64_t synced = 0;   // period sync
    uint64_t fsynced = 0;  // fsync, close, truncate, etc...
    flat::Uid writer = flat::Uid(0);

    uint32_t dynStripe = 1;  // dynamic stripe

    uint64_t truncateVer = 0;                         // largest known truncate version.
    std::optional<meta::VersionedLength> hintLength;  // local hint length
    std::optional<UtcTime> atime;                     // local read time, but only update for write open
    std::optional<UtcTime> mtime;                     // local write time

    void update(const Inode &inode, uint64_t syncver = 0, bool fsync = false) {
      if (!inode.isFile()) {
        return;
      }

      synced = std::max(synced, syncver);
      if (written == synced) {
        // clear local hint, since not write happens after sync
        hintLength = meta::VersionedLength{0, 0};
      }
      if (fsync) {
        fsynced = std::max(fsynced, syncver);
      }
      truncateVer = std::max(truncateVer, inode.asFile().truncateVer);
      dynStripe = inode.asFile().dynStripe;
    }
  };

  Inode inode;
  int refcount;
  std::atomic<int> opened;

  std::mutex wbMtx;
  std::shared_ptr<InodeWriteBuf> writeBuf;

  folly::Synchronized<DynamicAttr> dynamicAttr;
  folly::coro::Mutex extendStripeLock;

  RcInode(Inode inode, int refcount = 1)
      : inode(inode),
        refcount(refcount),
        extendStripeLock() {
    if (inode.isFile()) {
      auto guard = dynamicAttr.wlock();
      guard->truncateVer = inode.asFile().truncateVer;
      guard->hintLength = meta::VersionedLength{0, guard->truncateVer};
      guard->dynStripe = inode.asFile().dynStripe;
    }
  }

  uint64_t getTruncateVer() const { return dynamicAttr.rlock()->truncateVer; }

  void update(const Inode &inode, uint64_t syncver = 0, bool fsync = false) {
    if (!inode.isFile()) {
      return;
    } else {
      auto guard = dynamicAttr.wlock();
      return guard->update(inode, syncver, fsync);
    }
  }

  // clear hint length, force calculate length on next sync
  void clearHintLength() {
    auto guard = dynamicAttr.wlock();
    guard->hintLength = std::nullopt;
  }

  CoTryTask<uint64_t> beginWrite(flat::UserInfo userInfo,
                                 meta::client::MetaClient &meta,
                                 uint64_t offset,
                                 uint64_t length);

  void finishWrite(flat::UserInfo userInfo, uint64_t truncateVer, uint64_t offset, ssize_t ret);
};

struct FileHandle {
  std::shared_ptr<RcInode> rcinode;
  bool oDirect;
  Uuid sessionId;

  /* FileHandle(std::shared_ptr<RcInode> rcinode, bool oDirect, Uuid sessionId) */
  /*       : rcinode(rcinode), */
  /*         sessionId(sessionId) {} */
};

struct DirHandle {
  size_t dirId;
  pid_t pid;
  bool iovDir;
};

struct DirEntryVector {
  std::shared_ptr<std::vector<DirEntry>> dirEntries;

  DirEntryVector(std::shared_ptr<std::vector<DirEntry>> &&dirEntries)
      : dirEntries(std::move(dirEntries)) {}
};

struct DirEntryInodeVector {
  std::shared_ptr<std::vector<DirEntry>> dirEntries;
  std::shared_ptr<std::vector<std::optional<Inode>>> inodes;

  DirEntryInodeVector(std::shared_ptr<std::vector<DirEntry>> dirEntries,
                      std::shared_ptr<std::vector<std::optional<Inode>>> inodes)
      : dirEntries(std::move(dirEntries)),
        inodes(std::move(inodes)) {}
};

/**
 * FuseClients 结构体是FUSE文件系统客户端的核心实现
 * 
 * 功能职责：
 * 1. 管理与一个挂载点相关的所有FUSE操作
 * 2. 维护与各种服务的连接（元数据服务、存储服务、管理服务）
 * 3. 处理文件系统操作（读、写、创建、删除等）
 * 4. 管理inode缓存和IO任务队列
 */
struct FuseClients {
  FuseClients() = default;
  ~FuseClients();

  /**
   * 初始化FUSE客户端
   * 
   * @param appInfo 应用程序信息，包含集群ID、主机名等
   * @param mountPoint 文件系统挂载点路径
   * @param tokenFile 认证令牌文件路径
   * @param fuseConfig FUSE配置
   * 
   * 初始化流程：
   * 1. 保存配置和挂载点信息
   * 2. 加载认证令牌
   * 3. 初始化网络客户端(Client)
   * 4. 初始化管理服务客户端(MgmtdClient)
   * 5. 初始化存储服务客户端(StorageClient)
   * 6. 初始化元数据服务客户端(MetaClient)
   * 7. 初始化IO任务队列和处理协程
   * 8. 启动定期同步任务
   * 
   * 每个FuseClients实例与一个具体的挂载点关联，负责处理该挂载点的所有文件系统操作
   */
  Result<Void> init(const flat::AppInfo &appInfo,
                    const String &mountPoint,
                    const String &tokenFile,
                    FuseConfig &fuseConfig);
  void stop();

  CoTask<void> ioRingWorker(int i, int ths);
  void watch(int prio, std::stop_token stop);

  CoTask<void> periodicSyncScan();
  CoTask<void> periodicSync(InodeId inodeId);

  std::unique_ptr<net::Client> client;                               // 网络客户端
  std::shared_ptr<client::MgmtdClientForClient> mgmtdClient;         // 管理服务客户端
  std::shared_ptr<storage::client::StorageClient> storageClient;     // 存储服务客户端
  std::shared_ptr<meta::client::MetaClient> metaClient;              // 元数据服务客户端

  std::string fuseToken;                                             // FUSE认证令牌
  std::string fuseMount;                                             // FUSE挂载名称
  Path fuseMountpoint;                                               // FUSE挂载点路径
  std::optional<Path> fuseRemountPref;                               // FUSE重挂载前缀
  std::atomic<bool> memsetBeforeRead = false;                        // 读取前是否需要清零内存
  int maxIdleThreads = 0;                                            // 最大空闲线程数
  int maxThreads = 0;                                                // 最大线程数
  bool enableWritebackCache = false;                                 // 是否启用回写缓存

  std::unique_ptr<ConfigCallbackGuard> onFuseConfigUpdated;          // 配置更新回调

  // inode缓存，维护文件系统中活跃的inode，根目录的inode总是存在
  std::unordered_map<InodeId, std::shared_ptr<RcInode>> inodes = {
      {InodeId::root(), std::make_shared<RcInode>(Inode{}, 2)}};
  std::mutex inodesMutex;                                            // inode缓存互斥锁

  // readdir-plus操作的结果缓存
  std::unordered_map<uint64_t, DirEntryInodeVector> readdirplusResults;
  std::mutex readdirplusResultsMutex;                                // readdir-plus结果缓存互斥锁

  std::atomic_uint64_t dirHandle{0};                                 // 目录句柄计数器

  std::shared_ptr<net::RDMABufPool> bufPool;                         // RDMA缓冲区池
  int maxBufsize = 0;                                                // 最大缓冲区大小

  fuse_session *se = nullptr;                                        // FUSE会话

  std::atomic<std::chrono::nanoseconds> jitter;

  IovTable iovs;
  IoRingTable iors;
  std::vector<std::unique_ptr<BoundedQueue<IoRingJob>>> iojqs;  // job queues
  std::vector<std::jthread> ioWatches;
  folly::CancellationSource cancelIos;

  UserConfig userConfig;

  folly::Synchronized<std::set<InodeId>, std::mutex> dirtyInodes;
  std::atomic<InodeId> lastSynced;
  std::unique_ptr<BackgroundRunner> periodicSyncRunner;
  std::unique_ptr<CoroutinesPool<InodeId>> periodicSyncWorker;

  std::unique_ptr<folly::IOThreadPoolExecutor> notifyInvalExec;
  const FuseConfig *config;
};
}  // namespace hf3fs::fuse
