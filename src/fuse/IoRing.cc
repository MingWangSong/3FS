#include "IoRing.h"

#include <optional>
#include <type_traits>
#include <utility>

#include "PioV.h"
#include "common/utils/UtcTime.h"
#include "fbs/meta/Schema.h"
#include "fuse/FuseClients.h"
#include "fuse/FuseOps.h"
#include "lib/api/hf3fs_usrbio.h"

namespace hf3fs::fuse {
/**
 * 获取待处理的IO作业
 * 根据当前环的状态、优先级和IO深度，计算应该处理的IO请求批次
 * 
 * @param maxJobs 最大作业数量
 * @return 待处理的作业列表
 */
std::vector<IoRingJob> IoRing::jobsToProc(int maxJobs) {
  std::vector<IoRingJob> jobs;

  std::lock_guard lock(cqeMtx_);  // 加锁保护并发访问
  auto spt = sqeProcTail_;        // 当前处理尾指针
  auto sqes = sqeCount();         // 待处理的提交队列条目数

  // 计算完成队列可用空间
  auto cqeAvail = entries - 1 - processing_ - cqeCount();
  
  while (sqes && (int)jobs.size() < maxJobs) {
    int toProc;
    if (ioDepth > 0) {
      // 固定批处理大小模式
      toProc = ioDepth;
      // 如果请求数或完成队列空间不足，则退出
      if (toProc > sqes || toProc > cqeAvail) {
        break;
      }
    } else {
      // 动态批处理大小模式
      toProc = std::min(sqes, cqeAvail);
      if (ioDepth < 0) {
        // 负IO深度表示期望的批处理大小
        auto iod = -ioDepth;
        if (toProc > iod) {
          // 如果可处理数量超过期望值，限制为期望值
          toProc = iod;
        } else if (toProc < iod && timeout.count()) {
          // 如果可处理数量小于期望值且设置了超时，检查是否超时
          auto now = lastCheck_ = SteadyClock::now();
          if (!lastCheck_) {
            // 首次检查，记录时间并等待
            lastCheck_ = now;
            break;
          } else if (*lastCheck_ + timeout > now) {
            // 未超时，继续等待
            break;
          }
        }

        // 重置超时检查
        lastCheck_ = std::nullopt;
      }
    }

    // 预分配作业容器
    if (jobs.empty()) {
      jobs.reserve(ioDepth ? std::min(maxJobs, sqes / abs(ioDepth) + 1) : 1);
    }

    // 创建作业并添加到列表
    jobs.emplace_back(IoRingJob{shared_from_this(), spt, toProc});

    // 更新处理状态
    spt = (spt + toProc) % entries;
    sqeProcTails_.push_back(spt);
    processing_ += toProc;
    sqes -= toProc;
    cqeAvail -= toProc;
  }

  sqeProcTail_ = spt;  // 更新处理尾指针
  return jobs;
}

/**
 * 处理IO请求批次
 * 执行以下步骤：
 * 1. 查找请求相关的文件和缓冲区
 * 2. 准备IO操作参数和批次
 * 3. 执行IO操作（读或写）
 * 4. 更新完成队列，通知等待线程
 * 
 * @param spt 起始处理位置
 * @param toProc 要处理的请求数量
 * @param storageClient 存储客户端
 * @param storageIo 存储IO选项
 * @param userConfig 用户配置
 * @param lookupFiles 文件查找回调
 * @param lookupBufs 缓冲区查找回调
 * @return 异步任务
 */
CoTask<void> IoRing::process(
    int spt,
    int toProc,
    storage::client::StorageClient &storageClient,
    const storage::client::IoOptions &storageIo,
    UserConfig &userConfig,
    std::function<void(std::vector<std::shared_ptr<RcInode>> &, const IoArgs *, const IoSqe *, int)> &&lookupFiles,
    std::function<void(std::vector<Result<lib::ShmBufForIO>> &, const IoArgs *, const IoSqe *, int)> &&lookupBufs) {
  // 初始化性能监控记录器
  static monitor::LatencyRecorder overallLatency("usrbio.piov.overall", monitor::TagSet{{"mount_name", mountName}});
  static monitor::LatencyRecorder prepareLatency("usrbio.piov.prepare", monitor::TagSet{{"mount_name", mountName}});
  static monitor::LatencyRecorder submitLatency("usrbio.piov.submit", monitor::TagSet{{"mount_name", mountName}});
  static monitor::LatencyRecorder completeLatency("usrbio.piov.complete", monitor::TagSet{{"mount_name", mountName}});
  static monitor::DistributionRecorder ioSizeDist("usrbio.piov.io_size", monitor::TagSet{{"mount_name", mountName}});
  static monitor::DistributionRecorder ioDepthDist("usrbio.piov.io_depth", monitor::TagSet{{"mount_name", mountName}});
  static monitor::DistributionRecorder totalBytesDist("usrbio.piov.total_bytes",
                                                      monitor::TagSet{{"mount_name", mountName}});
  static monitor::DistributionRecorder distinctFilesDist("usrbio.piov.distinct_files",
                                                         monitor::TagSet{{"mount_name", mountName}});
  static monitor::DistributionRecorder distinctBufsDist("usrbio.piov.distinct_bufs",
                                                        monitor::TagSet{{"mount_name", mountName}});
  static monitor::CountRecorder bwCount("usrbio.piov.bw", monitor::TagSet{{"mount_name", mountName}});

  // 记录开始时间和IO类型
  auto start = SteadyClock::now(), overallStart = start;
  std::string ioType = forRead_ ? "read" : "write";
  auto uids = std::to_string(userInfo_.uid.toUnderType());

  // 获取用户配置
  auto &config = userConfig.getConfig(userInfo_);

  // 处理结果数组
  std::vector<ssize_t> res;
  
  // 如果是写操作且系统为只读模式，直接返回错误
  if (!forRead_ && config.readonly()) {
    res = std::vector<ssize_t>(toProc, static_cast<ssize_t>(-StatusCode::kReadOnlyMode));
  } else {
    // 初始化结果数组
    res = std::vector<ssize_t>(toProc, 0);

    // IO统计变量
    size_t iod = 0, totalBytes = 0;
    std::set<uint64_t> distinctFiles;
    std::set<Uuid> distinctBufs;

    // 查找相关的文件inodes
    std::vector<std::shared_ptr<RcInode>> inodes;
    inodes.reserve(toProc);
    lookupFiles(inodes, ringSection, sqeSection + spt, std::min(toProc, entries - spt));
    if ((int)inodes.size() < toProc) {
      lookupFiles(inodes, ringSection, sqeSection, toProc - (int)inodes.size());
    }

    // 查找相关的内存缓冲区
    std::vector<Result<lib::ShmBufForIO>> bufs;
    bufs.reserve(toProc);
    lookupBufs(bufs, ringSection, sqeSection + spt, std::min(toProc, entries - spt));
    if ((int)bufs.size() < toProc) {
      lookupBufs(bufs, ringSection, sqeSection, toProc - (int)bufs.size());
    }

    // 创建IO执行器
    lib::agent::PioV ioExec(storageClient, config.chunk_size_limit(), res);
    
    // 如果是写操作，需要记录截断版本号
    std::vector<uint64_t> truncateVers;
    if (!forRead_) {
      truncateVers.resize(toProc, 0);
    }

    // 为每个IO请求准备参数
    for (int i = 0; i < toProc; ++i) {
      auto idx = (spt + i) % entries;
      auto sqe = sqeSection[idx];
      const auto &args = ringSection[sqe.index];

      // 更新IO统计
      ++iod;
      totalBytes += args.ioLen;
      distinctFiles.insert(args.fileIid);

      Uuid id;
      memcpy(id.data, args.bufId, sizeof(id.data));
      distinctBufs.insert(id);

      // 记录IO大小分布
      ioSizeDist.addSample(args.ioLen, monitor::TagSet{{"io", ioType}, {"uid", uids}});

      // 检查文件是否存在
      if (!inodes[i]) {
        res[i] = -static_cast<ssize_t>(MetaCode::kNotFile);
        continue;
      }

      // 检查缓冲区是否有效
      if (!bufs[i]) {
        res[i] = -static_cast<ssize_t>(bufs[i].error().code());
        continue;
      }

      // 获取内存句柄
      auto memh = co_await bufs[i]->memh(args.ioLen);
      if (!memh) {
        res[i] = -static_cast<ssize_t>(memh.error().code());
        continue;
      } else if (!bufs[i]->ptr() || !*memh) {
        XLOGF(ERR, "{} is null when doing usrbio", *memh ? "buf ptr" : "memh");
        res[i] = -static_cast<ssize_t>(ClientAgentCode::kIovShmFail);
        continue;
      }

      // 写操作前的准备
      if (!forRead_) {
        auto beginWrite =
            co_await inodes[i]->beginWrite(userInfo_, *getFuseClientsInstance().metaClient, args.fileOff, args.ioLen);
        if (beginWrite.hasError()) {
          res[i] = -static_cast<ssize_t>(beginWrite.error().code());
          continue;
        }
        truncateVers[i] = *beginWrite;
      }

      // 添加IO操作到执行器
      auto addRes = forRead_
                        ? ioExec.addRead(i, inodes[i]->inode, 0, args.fileOff, args.ioLen, bufs[i]->ptr(), **memh)
                        : ioExec.addWrite(i, inodes[i]->inode, 0, args.fileOff, args.ioLen, bufs[i]->ptr(), **memh);
      if (!addRes) {
        res[i] = -static_cast<ssize_t>(addRes.error().code());
      }
    }

    // 记录准备阶段的性能指标
    auto now = SteadyClock::now();
    prepareLatency.addSample(now - start, monitor::TagSet{{"io", ioType}, {"uid", uids}});
    start = now;

    // 记录IO批次统计信息
    ioDepthDist.addSample(iod, monitor::TagSet{{"io", ioType}, {"uid", uids}});
    totalBytesDist.addSample(totalBytes, monitor::TagSet{{"io", ioType}, {"uid", uids}});
    distinctFilesDist.addSample(distinctFiles.size(), monitor::TagSet{{"io", ioType}, {"uid", uids}});
    distinctBufsDist.addSample(distinctBufs.size(), monitor::TagSet{{"io", ioType}, {"uid", uids}});

    // 设置读操作选项
    auto readOpt = storageIo.read();
    if (flags_ & HF3FS_IOR_ALLOW_READ_UNCOMMITTED) {
      readOpt.set_allowReadUncommitted(true);
    }
    
    // 执行IO操作
    auto execRes = co_await (forRead_ ? ioExec.executeRead(userInfo_, readOpt)
                                     : ioExec.executeWrite(userInfo_, storageIo.write()));

    // 记录提交阶段的性能指标
    now = SteadyClock::now();
    submitLatency.addSample(now - start, monitor::TagSet{{"io", ioType}, {"uid", uids}});
    start = now;

    // 处理IO执行结果
    if (!execRes) {
      // 执行失败，标记所有未完成的请求为错误
      for (auto &r : res) {
        if (r >= 0) {
          r = -static_cast<ssize_t>(execRes.error().code());
        }
      }
    } else {
      // 完成IO操作
      ioExec.finishIo(!(flags_ & HF3FS_IOR_FORBID_READ_HOLES));
    }

    // 写操作完成处理
    if (!forRead_) {
      for (int i = 0; i < toProc; ++i) {
        auto &inode = inodes[i];
        if (!inode) {
          continue;
        }
        auto sqe = sqeSection[(spt + i) % entries];
        auto off = ringSection[sqe.index].fileOff;
        auto r = res[i];
        // 完成写操作，更新元数据
        inode->finishWrite(userInfo_.uid, truncateVers[i], off, r);
      }
    }
  }

  // 计算新的处理尾指针
  auto newSpt = (spt + toProc) % entries;

  // 收集已处理的SQE
  std::vector<IoSqe> sqes(toProc);
  for (int i = 0; i < toProc; ++i) {
    sqes[i] = sqeSection[(spt + i) % entries];
  }

  {
    // 加锁更新共享状态
    std::lock_guard lock(cqeMtx_);
    if (sqeProcTails_.empty()) {
      XLOGF(FATAL, "bug?! sqeProcTails_ is empty");
    }

    // 更新处理尾指针列表
    if (sqeProcTails_.front() != newSpt) {
      sqeDoneTails_.insert(newSpt);
    } else {
      sqeTail = newSpt;
      sqeProcTails_.pop_front();
      
      // 合并已完成的尾指针
      while (!sqeDoneTails_.empty()) {
        if (sqeProcTails_.empty()) {
          XLOGF(FATAL, "bug?! sqeProcTails_ is empty");
        }
        auto first = sqeProcTails_.front();
        auto it = sqeDoneTails_.find(first);
        if (it == sqeDoneTails_.end()) {
          break;
        } else {
          sqeTail = first;
          sqeProcTails_.pop_front();
          sqeDoneTails_.erase(it);
        }
      }
    }

    // 添加完成队列条目
    for (int i = 0; i < toProc; ++i) {
      auto &sqe = sqes[i];
      auto r = res[i];
      auto addRes = addCqe(sqe.index, r >= 0 ? r : -static_cast<ssize_t>(StatusCode::toErrno(-r)), sqe.userdata);
      if (!addRes) {
        XLOGF(FATAL, "failed to add cqe");
      }
    }

    // 更新处理中计数
    processing_ -= toProc;
  }

  // 发送信号通知等待线程
  sem_post(cqeSem.get());

  // 计算成功完成的字节数
  size_t doneBytes = 0;
  for (auto r : res) {
    if (r > 0) {
      doneBytes += r;
    }
  }
  // 记录带宽统计
  bwCount.addSample(doneBytes, monitor::TagSet{{"io", ioType}, {"uid", uids}});

  // 记录完成阶段和整体性能指标
  auto now = SteadyClock::now();
  completeLatency.addSample(now - start, monitor::TagSet{{"io", ioType}, {"uid", uids}});
  overallLatency.addSample(now - overallStart, monitor::TagSet{{"io", ioType}, {"uid", uids}});
}
}  // namespace hf3fs::fuse
