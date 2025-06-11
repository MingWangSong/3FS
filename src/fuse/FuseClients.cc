#include "FuseClients.h"

#include <folly/Random.h>
#include <folly/ScopeGuard.h>
#include <folly/executors/IOThreadPoolExecutor.h>
#include <folly/experimental/coro/BlockingWait.h>
#include <folly/functional/Partial.h>
#include <folly/logging/xlog.h>
#include <fuse3/fuse_lowlevel.h>
#include <memory>
#include <thread>
#include <utility>

#include "common/app/ApplicationBase.h"
#include "common/monitor/Recorder.h"
#include "common/utils/BackgroundRunner.h"
#include "common/utils/Coroutine.h"
#include "common/utils/Duration.h"
#include "common/utils/FileUtils.h"
#include "common/utils/SysResource.h"
#include "fbs/meta/Common.h"
#include "fbs/mgmtd/Rpc.h"
#include "stubs/MetaService/MetaServiceStub.h"
#include "stubs/common/RealStubFactory.h"
#include "stubs/mgmtd/MgmtdServiceStub.h"

namespace hf3fs::fuse {
namespace {
monitor::ValueRecorder dirtyInodesCnt("fuse.dirty_inodes");

Result<Void> establishClientSession(client::IMgmtdClientForClient &mgmtdClient) {
  return folly::coro::blockingWait([&]() -> CoTryTask<void> {
    auto retryInterval = std::chrono::milliseconds(10);
    constexpr auto maxRetryInterval = std::chrono::milliseconds(1000);
    Result<Void> res = Void{};
    for (int i = 0; i < 40; ++i) {
      res = co_await mgmtdClient.extendClientSession();
      if (res) break;
      XLOGF(CRITICAL, "Try to establish client session failed: {}\nretryCount: {}", res.error(), i);
      co_await folly::coro::sleep(retryInterval);
      retryInterval = std::min(2 * retryInterval, maxRetryInterval);
    }
    co_return res;
  }());
}
}  // namespace

FuseClients::~FuseClients() { stop(); }

/**
 * 初始化FUSE客户端系统，建立与存储和元数据服务的连接
 * @param appInfo 应用信息，包含集群ID和其他元数据
 * @param mountPoint FUSE挂载点路径
 * @param tokenFile 认证令牌文件路径
 * @param fuseConfig FUSE配置对象
 * @return 成功返回Void对象，失败返回错误
 */
Result<Void> FuseClients::init(const flat::AppInfo &appInfo,
                               const String &mountPoint,
                               const String &tokenFile,
                               FuseConfig &fuseConfig) {
  // 保存配置引用，便于后续访问
  config = &fuseConfig;

  // 设置FUSE挂载名称（使用集群ID作为挂载名）
  // FUSE协议限制挂载名必须小于32字符
  fuseMount = appInfo.clusterId;
  XLOGF_IF(FATAL,
           fuseMount.size() >= 32,
           "FUSE only support mount name shorter than 32 characters, but {} got.",
           fuseMount);

  // 规范化挂载点路径，确保路径一致性
  fuseMountpoint = Path(mountPoint).lexically_normal();

  // 处理重挂载前缀设置（如果配置中指定了）
  // 重挂载前缀用于在重挂载时保持原有挂载点信息
  if (fuseConfig.remount_prefix()) {
    fuseRemountPref = Path(*fuseConfig.remount_prefix()).lexically_normal();
  }

  // 获取FUSE认证令牌，优先使用环境变量，其次从配置文件读取
  if (const char *env_p = std::getenv("HF3FS_FUSE_TOKEN")) {
    XLOGF(INFO, "Use token from env var");
    fuseToken = std::string(env_p);
  } else {
    XLOGF(INFO, "Use token from config");
    // 从文件加载令牌并去除两端空白
    auto tokenRes = loadFile(tokenFile);
    RETURN_ON_ERROR(tokenRes); // 文件读取失败则返回错误
    fuseToken = folly::trimWhitespace(*tokenRes);
  }

  // 配置FUSE缓存和IO行为
  enableWritebackCache = fuseConfig.enable_writeback_cache(); // 启用写回缓存
  memsetBeforeRead = fuseConfig.memset_before_read(); // 读取前是否清零内存
  maxIdleThreads = fuseConfig.max_idle_threads(); // 最大空闲线程数

  // 根据系统CPU核心数动态计算最大线程数
  // 通常设置为逻辑核心数的一半（向上取整）
  int logicalCores = std::thread::hardware_concurrency();
  if (logicalCores != 0) {
    maxThreads = std::min(fuseConfig.max_threads(), (logicalCores + 1) / 2);
  } else {
    maxThreads = fuseConfig.max_threads();
  }

  // 创建RDMA缓冲池用于高性能网络IO
  bufPool = net::RDMABufPool::create(fuseConfig.io_bufs().max_buf_size(), fuseConfig.rdma_buf_pool_size());

  // 初始化IO向量和IO环系统
  // iovs用于管理IO缓冲区，iors用于管理IO请求环
  iovs.init(fuseRemountPref.value_or(fuseMountpoint), fuseConfig.iov_limit());
  iors.init(fuseConfig.iov_limit());
  userConfig.init(fuseConfig);

  // 创建网络客户端（如果不存在）
  if (!client) {
    client = std::make_unique<net::Client>(fuseConfig.client());
    RETURN_ON_ERROR(client->start()); // 启动客户端，失败则返回错误
  }
  // 创建网络客户端通信上下文生成器，用于RPC通信
  auto ctxCreator = [this](net::Address addr) { return client->serdeCtx(addr); };

  // 创建管理服务客户端（用于集群管理通信）
  if (!mgmtdClient) {
    mgmtdClient = std::make_shared<client::MgmtdClientForClient>(
        appInfo.clusterId,
        std::make_unique<stubs::RealStubFactory<mgmtd::MgmtdServiceStub>>(ctxCreator),
        fuseConfig.mgmtd());
  }

  // 获取物理主机名（真实物理机名称）和容器主机名
  // 用于客户端身份识别和会话建立
  auto physicalHostnameRes = SysResource::hostname(/*physicalMachineName=*/true);
  RETURN_ON_ERROR(physicalHostnameRes);

  auto containerHostnameRes = SysResource::hostname(/*physicalMachineName=*/false);
  RETURN_ON_ERROR(containerHostnameRes);

  // 基于物理主机名生成随机客户端ID
  auto clientId = ClientId::random(*physicalHostnameRes);

  // 设置客户端会话有效载荷，包含客户端标识和会话数据
  mgmtdClient->setClientSessionPayload({clientId.uuid.toHexString(),
                                        flat::NodeType::FUSE, // 节点类型为FUSE
                                        flat::ClientSessionData::create(
                                            /*universalId=*/*physicalHostnameRes,
                                            /*description=*/fmt::format("fuse: {}", *containerHostnameRes),
                                            appInfo.serviceGroups,
                                            appInfo.releaseVersion),
                                        // TODO: 使用真实用户信息
                                        flat::UserInfo{}});

  // 设置配置监听器，用于动态更新配置
  mgmtdClient->setConfigListener(ApplicationBase::updateConfig);

  // 启动管理服务客户端并等待完成
  folly::coro::blockingWait(mgmtdClient->start(&client->tpg().bgThreadPool().randomPick()));
  
  // 刷新路由信息（非强制）
  folly::coro::blockingWait(mgmtdClient->refreshRoutingInfo(/*force=*/false));
  
  // 建立客户端会话（带重试机制）
  RETURN_ON_ERROR(establishClientSession(*mgmtdClient));

  // 创建存储客户端，用于与存储服务通信
  storageClient = storage::client::StorageClient::create(clientId, fuseConfig.storage(), *mgmtdClient);

  // 创建元数据客户端，用于处理文件系统元数据操作
  metaClient =
      std::make_shared<meta::client::MetaClient>(clientId,
                                                 fuseConfig.meta(),
                                                 std::make_unique<meta::client::MetaClient::StubFactory>(ctxCreator),
                                                 mgmtdClient,
                                                 storageClient,
                                                 true /* dynStripe */); // 启用动态条带化
  
  // 启动元数据客户端
  metaClient->start(client->tpg().bgThreadPool());

  // 初始化IO作业队列系统，使用三个优先级队列（高、中、低）
  iojqs.reserve(3);
  iojqs.emplace_back(new BoundedQueue<IoRingJob>(fuseConfig.io_jobq_sizes().hi())); // 高优先级队列
  iojqs.emplace_back(new BoundedQueue<IoRingJob>(fuseConfig.io_jobq_size())); // 中优先级队列
  iojqs.emplace_back(new BoundedQueue<IoRingJob>(fuseConfig.io_jobq_sizes().lo())); // 低优先级队列

  // 设置提交等待抖动，用于避免请求拥塞
  jitter = fuseConfig.submit_wait_jitter();

  // 创建并启动IO环工作协程，处理IO请求
  auto &tp = client->tpg().bgThreadPool();
  auto coros = fuseConfig.batch_io_coros();
  for (int i = 0; i < coros; ++i) {
    auto exec = &tp.get(i % tp.size()); // 轮询选择执行器
    // 创建协程并绑定取消令牌，以支持优雅关闭
    co_withCancellation(cancelIos.getToken(), ioRingWorker(i, coros)).scheduleOn(exec).start();
  }

  // 创建IO监视线程（每个优先级一个）
  ioWatches.reserve(3);
  for (int i = 0; i < 3; ++i) {
    ioWatches.emplace_back(folly::partial(&FuseClients::watch, this, i));
  }

  // 创建定期同步工作池，用于周期性刷新脏inode
  periodicSyncWorker = std::make_unique<CoroutinesPool<InodeId>>(config->periodic_sync().worker());
  periodicSyncWorker->start(folly::partial(&FuseClients::periodicSync, this), tp);

  // 创建定期同步后台运行器，用于定期扫描脏inode
  // 添加随机因子(0.7-1.3)，避免所有客户端同时扫描
  periodicSyncRunner = std::make_unique<BackgroundRunner>(&tp.pickNextFree());
  periodicSyncRunner->start("PeriodSync", folly::partial(&FuseClients::periodicSyncScan, this), [&]() {
    return config->periodic_sync().interval() * folly::Random::randDouble(0.7, 1.3);
  });

  // 注册配置更新回调，动态更新内存配置
  onFuseConfigUpdated = fuseConfig.addCallbackGuard([&fuseConfig = fuseConfig, this] {
    memsetBeforeRead = fuseConfig.memset_before_read();
    jitter = std::chrono::duration_cast<std::chrono::nanoseconds>(fuseConfig.submit_wait_jitter());
  });

  // 创建通知失效线程池，用于处理缓存失效通知
  notifyInvalExec =
      std::make_unique<folly::IOThreadPoolExecutor>(fuseConfig.notify_inval_threads(),
                                                   std::make_shared<folly::NamedThreadFactory>("NotifyInvalThread"));

  return Void{};
}

void FuseClients::stop() {
  if (notifyInvalExec) {
    notifyInvalExec->stop();
    notifyInvalExec.reset();
  }
  if (onFuseConfigUpdated) {
    onFuseConfigUpdated.reset();
  }

  cancelIos.requestCancellation();

  for (auto &t : ioWatches) {
    t.request_stop();
  }
  if (periodicSyncRunner) {
    folly::coro::blockingWait(periodicSyncRunner->stopAll());
    periodicSyncRunner.reset();
  }
  if (periodicSyncWorker) {
    periodicSyncWorker->stopAndJoin();
    periodicSyncWorker.reset();
  }
  if (metaClient) {
    metaClient->stop();
    metaClient.reset();
  }
  if (storageClient) {
    storageClient->stop();
    storageClient.reset();
  }
  if (mgmtdClient) {
    folly::coro::blockingWait(mgmtdClient->stop());
    mgmtdClient.reset();
  }
  if (client) {
    client->stopAndJoin();
    client.reset();
  }
}

CoTask<void> FuseClients::ioRingWorker(int i, int ths) {
  // a worker thread has its own priority, but it can also execute jobs from queues with a higher priority
  // checkHigher is used to make sure the job queue with the thread's own priority doesn't starve
  bool checkHigher = true;

  while (true) {
    auto res = co_await folly::coro::co_awaitTry([this, &checkHigher, i, ths]() -> CoTask<void> {
      IoRingJob job;
      // 优先级划分
      auto hiThs = config->io_worker_coros().hi(), loThs = config->io_worker_coros().lo();
      auto prio = i < hiThs ? 0 : i < (ths - loThs) ? 1 : 2;
      if (!config->enable_priority()) {  // 1、直接根据优先级获取队列
        job = co_await iojqs[prio]->co_dequeue();
      } else {  // 2、优先级队列智能调度
        bool gotJob = false;

        // if checkHigher, dequeue from a higher job queue if it is full
        while (!gotJob) {
          // 从高优先级已满队列"偷取"任务
          if (checkHigher) {
            for (int nprio = 0; nprio < prio; ++nprio) {
              if (iojqs[nprio]->full()) {
                auto dres = iojqs[nprio]->try_dequeue();
                if (dres) {
                  // got a job from higher priority queue, next time pick a same priority job unless the queue is empty
                  checkHigher = false;
                  gotJob = true;
                  job = std::move(*dres);
                  break;
                }
              }
            }

            if (gotJob) {
              break;
            }
          }

          // 动态优先级扫描
          // if checkHigher, check from higher prio to lower; otherwise, reverse the checking direction
          for (int nprio = checkHigher ? 0 : prio; checkHigher ? nprio <= prio : nprio >= 0;
               nprio += checkHigher ? 1 : -1) {
            auto [sres, dres] =
                co_await folly::coro::collectAnyNoDiscard(folly::coro::sleep(config->io_job_deq_timeout()),
                                                          iojqs[nprio]->co_dequeue());
            if (dres.hasValue()) {
              // if the job is the thread's own priority, next time it can check from higher priority queues
              if (!checkHigher && nprio == prio) {
                checkHigher = true;
              }
              gotJob = true;
              job = std::move(*dres);
              break;
            } else if (sres.hasValue()) {
              continue;
            } else {
              dres.throwUnlessValue();
            }
          }
        }
      }

      while (true) {

        // 查找对应的索引节点
        auto lookupFiles =
            [this](std::vector<std::shared_ptr<RcInode>> &ins, const IoArgs *args, const IoSqe *sqes, int sqec) {
              auto lastIid = 0ull;

              std::lock_guard lock(inodesMutex);
              for (int i = 0; i < sqec; ++i) {
                auto idn = args[sqes[i].index].fileIid;
                if (i && idn == lastIid) {
                  ins.emplace_back(ins.back());
                  continue;
                }

                lastIid = idn;
                auto iid = meta::InodeId(idn);
                auto it = inodes.find(iid);
                ins.push_back(it == inodes.end() ? (std::shared_ptr<RcInode>()) : it->second);
              }
            };
        
        // 查找缓冲区信息
        auto lookupBufs =
            [this](std::vector<Result<lib::ShmBufForIO>> &bufs, const IoArgs *args, const IoSqe *sqe, int sqec) {
              auto lastId = Uuid::zero();
              std::shared_ptr<lib::ShmBuf> lastShm;

              std::lock_guard lock(iovs.shmLock);
              for (int i = 0; i < sqec; ++i) {
                auto &arg = args[sqe[i].index];
                Uuid id;
                memcpy(id.data, arg.bufId, sizeof(id.data));

                std::shared_ptr<lib::ShmBuf> shm;
                if (i && id == lastId) {
                  shm = lastShm;
                } else {
                  auto it = iovs.shmsById.find(id);
                  if (it == iovs.shmsById.end()) {
                    bufs.emplace_back(makeError(StatusCode::kInvalidArg, "buf id not found"));
                    continue;
                  }

                  auto iovd = it->second;
                  shm = iovs.iovs->table[iovd].load();
                  if (!shm) {
                    bufs.emplace_back(makeError(StatusCode::kInvalidArg, "buf id not found"));
                    continue;
                  } else if (shm->size < arg.bufOff + arg.ioLen) {
                    bufs.emplace_back(makeError(StatusCode::kInvalidArg, "invalid buf off and/or io len"));
                    continue;
                  }

                  lastId = id;
                  lastShm = shm;
                }

                bufs.emplace_back(lib::ShmBufForIO(std::move(shm), arg.bufOff));
              }
            };

        // 处理IO
        co_await job.ior->process(job.sqeProcTail,
                                  job.toProc,
                                  *storageClient,
                                  config->storage_io(),
                                  userConfig,
                                  std::move(lookupFiles),
                                  std::move(lookupBufs));

        if (iojqs[0]->full() || job.ior->priority != prio) {
          sem_post(iors.sems[job.ior->priority].get());  // wake the watchers
        } else {
          auto jobs = job.ior->jobsToProc(1);
          if (!jobs.empty()) {
            job = jobs.front();
            if (!iojqs[0]->try_enqueue(job)) {
              continue;
            }
          }
        }

        break;
      }
    }());
    if (UNLIKELY(res.hasException())) {
      XLOGF(INFO, "io worker #{} cancelled", i);
      if (res.hasException<OperationCancelled>()) {
        break;
      } else {
        XLOGF(FATAL, "got exception in io worker #{}", i);
      }
    }
  }
}

void FuseClients::watch(int prio, std::stop_token stop) {
  while (!stop.stop_requested()) {
    // 时间处理与等待机制
    struct timespec ts;
    if (clock_gettime(CLOCK_REALTIME, &ts) < 0) {
      continue;
    }

    auto nsec = ts.tv_nsec + jitter.load().count();
    ts.tv_nsec = nsec % 1000000000;
    ts.tv_sec += nsec / 1000000000;
    if (sem_timedwait(iors.sems[prio].get(), &ts) < 0 && errno == ETIMEDOUT) {
      continue;
    }

    // 任务发现与分发
    auto gotJobs = false;
    do {
      gotJobs = false;

      auto n = iors.ioRings->slots.nextAvail.load();
      for (int i = 0; i < n; ++i) {
        auto ior = iors.ioRings->table[i].load();

        if (ior && ior->priority == prio) {
          auto jobs = ior->jobsToProc(config->max_jobs_per_ioring());
          for (auto &&job : jobs) {
            gotJobs = true;
            iojqs[prio]->enqueue(std::move(job));
          }
        }
      }
    } while (gotJobs);  // loop till we found no more jobs and then block in the next iter
  }
}

CoTask<void> FuseClients::periodicSyncScan() {
  if (!config->periodic_sync().enable() || config->readonly()) {
    co_return;
  }

  XLOGF(INFO, "periodicSyncScan run");
  std::set<InodeId> dirty;
  {
    auto guard = dirtyInodes.lock();
    auto limit = config->periodic_sync().limit();
    dirtyInodesCnt.set(guard->size());
    if (guard->size() <= limit) {
      dirty = std::exchange(*guard, {});
    } else {
      XLOGF(WARN, "dirty inodes {} > limit {}", guard->size(), limit);
      auto iter = guard->find(lastSynced);
      while (dirty.size() < limit) {
        if (iter == guard->end()) {
          iter = guard->begin();
          XLOGF_IF(FATAL, iter == guard->end(), "iter == guard->end() shouldn't happen");
        } else {
          auto inode = *iter;
          lastSynced = inode;
          iter = guard->erase(iter);
          dirty.insert(inode);
        }
      }
    }
  }

  for (auto inode : dirty) {
    co_await periodicSyncWorker->enqueue(inode);
  }

  co_return;
}

}  // namespace hf3fs::fuse
