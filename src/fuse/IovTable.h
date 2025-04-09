#pragma once

#include <string>

#include "common/utils/AtomicSharedPtrTable.h"
#include "fbs/meta/Schema.h"
#include "lib/common/Shm.h"

namespace hf3fs::fuse {
/**
 * IovTable类 - 管理FUSE文件系统的共享内存IO向量
 * 负责创建、查找、删除和列出共享内存缓冲区
 * 支持RDMA注册和普通内存映射两种模式
 */
class IovTable {
public:
  IovTable() = default;
  
  /**
   * 初始化IO向量表
   * @param mount 挂载点路径，用于日志和监控标识
   * @param cap 最大可管理的共享内存缓冲区数量
   */
  void init(const Path &mount, int cap);
  
  /**
   * 添加新的IO向量缓冲区
   * @param key 缓冲区标识键，包含UUID和配置信息
   * @param shmPath 共享内存文件路径
   * @param pid 进程ID
   * @param ui 用户信息，用于权限控制
   * @param exec 执行器，用于异步操作
   * @param sc 存储客户端，用于RDMA注册
   * @return 成功返回inode和共享内存缓冲区指针对，失败返回错误
   */
  Result<std::pair<meta::Inode, std::shared_ptr<lib::ShmBuf>>> addIov(...);
  
  /**
   * 移除IO向量缓冲区
   * @param key 缓冲区标识键
   * @param ui 用户信息，用于权限验证
   * @return 成功返回被移除的共享内存缓冲区指针，失败返回错误
   */
  Result<std::shared_ptr<lib::ShmBuf>> rmIov(const char *key, const meta::UserInfo &ui);
  
  /**
   * 查找IO向量缓冲区对应的inode信息
   * @param key 缓冲区标识键
   * @param ui 用户信息，用于权限验证
   * @return 成功返回inode信息，失败返回错误
   */
  Result<meta::Inode> lookupIov(const char *key, const meta::UserInfo &ui);
  
  /**
   * 根据inode ID获取IO向量描述符
   * @param iid inode ID
   * @return 成功返回描述符索引，失败返回空
   */
  std::optional<int> iovDesc(meta::InodeId iid);
  
  /**
   * 获取IO向量缓冲区状态
   * @param key 描述符索引
   * @param ui 用户信息，用于权限验证
   * @return 成功返回inode信息，失败返回错误
   */
  Result<meta::Inode> statIov(int key, const meta::UserInfo &ui);

public:
  /**
   * 列出所有IO向量缓冲区
   * @param ui 用户信息，用于权限过滤
   * @return 目录项和inode信息对，包含所有用户可见的缓冲区
   */
  std::pair<std::shared_ptr<std::vector<meta::DirEntry>>, std::shared_ptr<std::vector<std::optional<meta::Inode>>>>
  listIovs(const meta::UserInfo &ui);

public:
  std::string mountName;                // 挂载点名称，用于监控和日志
  std::shared_mutex shmLock;            // 共享内存映射表的互斥锁
  robin_hood::unordered_map<Uuid, int> shmsById;  // UUID到描述符的映射
  std::unique_ptr<AtomicSharedPtrTable<lib::ShmBuf>> iovs;  // 原子共享指针表，存储所有共享内存缓冲区

private:
  mutable std::shared_mutex iovdLock_;  // 描述符映射的互斥锁
  robin_hood::unordered_map<std::string, int> iovds_;  // 键到描述符的映射
};
}  // namespace hf3fs::fuse
