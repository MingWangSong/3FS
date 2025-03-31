#pragma once

#include "LauncherUtils.h"
#include "client/mgmtd/MgmtdClient.h"
#include "common/net/Client.h"

namespace hf3fs::core::launcher {

/**
 * @brief 管理客户端获取器
 * 
 * 负责从管理服务(Mgmtd)获取配置和应用信息的组件。
 * 主要功能：
 * 1. 管理客户端连接
 * 2. 获取配置模板
 * 3. 完善应用信息
 */
struct MgmtdClientFetcher {
  /**
   * @brief 构造函数
   * @param clusterId 集群ID
   * @param clientCfg 客户端配置
   * @param mgmtdClientCfg 管理客户端配置
   */
  MgmtdClientFetcher(String clusterId,
                     const net::Client::Config &clientCfg,
                     const client::MgmtdClient::Config &mgmtdClientCfg);

  /**
   * @brief 模板构造函数
   * 从配置对象中提取必要的配置信息
   */
  template <typename ConfigT>
  MgmtdClientFetcher(const ConfigT &cfg)
      : MgmtdClientFetcher(cfg.cluster_id(), cfg.client(), cfg.mgmtd_client()) {}

  /**
   * @brief 析构函数
   * 确保客户端正确停止
   */
  virtual ~MgmtdClientFetcher() { stopClient(); }

  /**
   * @brief 加载配置模板
   * @param nodeType 节点类型
   * @return 配置信息
   */
  Result<flat::ConfigInfo> loadConfigTemplate(flat::NodeType nodeType);

  /**
   * @brief 确保客户端已初始化
   * @return 初始化结果
   */
  Result<Void> ensureClientInited();

  /**
   * @brief 停止客户端
   */
  void stopClient();

  /**
   * @brief 完善应用信息
   * @param appInfo 应用信息
   * @return 操作结果
   */
  virtual Result<Void> completeAppInfo(flat::AppInfo &appInfo) = 0;

  const String clusterId_;                                    // 集群ID
  const net::Client::Config &clientCfg_;                     // 客户端配置
  const client::MgmtdClient::Config &mgmtdClientCfg_;       // 管理客户端配置
  std::unique_ptr<net::Client> client_;                      // 网络客户端
  std::shared_ptr<client::MgmtdClient> mgmtdClient_;        // 管理服务客户端
};
}  // namespace hf3fs::core::launcher
