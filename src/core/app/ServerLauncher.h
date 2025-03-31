#pragma once

#include "LauncherUtils.h"
#include "common/app/ApplicationBase.h"
#include "common/utils/ConstructLog.h"
#include "fbs/mgmtd/ConfigInfo.h"

// 应用配置相关命令行参数
DECLARE_string(app_cfg);              // 应用配置文件路径
DECLARE_bool(dump_default_app_cfg);   // 是否打印默认应用配置

// 启动器配置相关命令行参数
DECLARE_string(launcher_cfg);         // 启动器配置文件路径
DECLARE_bool(dump_default_launcher_cfg); // 是否打印默认启动器配置

namespace hf3fs::core {

/**
 * @brief 服务器启动器基类
 * 
 * 提供基础的命令行参数解析功能，用于处理应用配置和启动器配置的命令行参数
 */
class ServerLauncherBase {
 public:
  /**
   * @brief 解析命令行参数
   * @param argc 参数数量
   * @param argv 参数数组
   * @return 解析结果
   * 
   * 功能：
   * 1. 解析应用配置参数（以--app_config.开头）
   * 2. 解析启动器配置参数（以--launcher_config.开头）
   */
  Result<Void> parseFlags(int *argc, char ***argv);

 protected:
  ApplicationBase::ConfigFlags appConfigFlags_;      // 应用配置标志
  ApplicationBase::ConfigFlags launcherConfigFlags_; // 启动器配置标志
};

/**
 * @brief 服务器启动器模板类
 * 
 * 负责服务器的启动流程，包括：
 * 1. 配置管理：应用配置和启动器配置的加载和初始化
 * 2. 资源初始化：网络设备、远程配置获取器等
 * 3. 服务器启动：应用信息加载、服务器实例启动
 * 
 * @tparam Server 服务器类型，必须提供以下类型定义：
 *                - AppConfig：应用配置类型
 *                - LauncherConfig：启动器配置类型
 *                - RemoteConfigFetcher：远程配置获取器类型
 *                以及静态常量 kNodeType
 */
template <typename Server>
class ServerLauncher : public ServerLauncherBase {
 public:
  using AppConfig = typename Server::AppConfig;           // 应用配置类型
  using LauncherConfig = typename Server::LauncherConfig; // 启动器配置类型
  using RemoteConfigFetcher = typename Server::RemoteConfigFetcher; // 远程配置获取器类型
  static constexpr auto kNodeType = Server::kNodeType;    // 节点类型

  ServerLauncher() = default;

  /**
   * @brief 初始化启动器
   * 
   * 功能：
   * 1. 初始化应用配置和启动器配置
   * 2. 启动网络设备管理器
   * 3. 创建远程配置获取器
   * 
   * @return 初始化结果
   */
  Result<Void> init() {
    // 初始化应用配置
    appConfig_.init(FLAGS_app_cfg, FLAGS_dump_default_app_cfg, appConfigFlags_);
    // 初始化启动器配置
    launcherConfig_.init(FLAGS_launcher_cfg, FLAGS_dump_default_launcher_cfg, launcherConfigFlags_);

    // 记录配置信息
    XLOGF(INFO, "Full AppConfig:\n{}", appConfig_.toString());
    XLOGF(INFO, "Full LauncherConfig:\n{}", launcherConfig_.toString());

    // 启动网络设备管理器
    auto ibResult = net::IBManager::start(launcherConfig_.ib_devices());
    XLOGF_IF(FATAL, !ibResult, "Failed to start IBManager: {}", ibResult.error());
    XLOGF(INFO, "IBDevice inited");

    // 创建远程配置获取器（运用函数模板make_unique，创建RemoteConfigFetcher实例）
    fetcher_ = std::make_unique<RemoteConfigFetcher>(launcherConfig_);
    return Void{};
  }

  /**
   * @brief 加载配置模板
   * 
   * 从远程配置服务获取配置模板
   * 
   * @return 配置模板内容和更新描述
   */
  Result<std::pair<String, String>> loadConfigTemplate() {
    auto res = fetcher_->loadConfigTemplate(kNodeType);
    RETURN_ON_ERROR(res);
    return std::make_pair(res->content, res->genUpdateDesc());
  }

  /**
   * @brief 加载应用信息
   * 
   * 构建并完善应用信息，包括：
   * 1. 节点ID
   * 2. 集群ID
   * 3. 其他元数据信息
   * 
   * @return 应用信息
   */
  Result<flat::AppInfo> loadAppInfo() {
    auto appInfo = launcher::buildBasicAppInfo(appConfig_.getNodeId(), launcherConfig_.cluster_id());
    RETURN_ON_ERROR(fetcher_->completeAppInfo(appInfo));
    return appInfo;
  }

  /**
   * @brief 启动服务器
   * 
   * 根据远程配置获取器是否支持服务器启动，选择启动方式：
   * 1. 如果支持，使用远程配置获取器启动
   * 2. 如果不支持，直接启动服务器
   * 
   * @param server 服务器实例
   * @param appInfo 应用信息
   * @return 启动结果
   */
  /**
   * @brief 启动服务器
   * 
   * 这个方法使用了C++20的requires表达式来进行编译期检查:
   * 1. 首先检查fetcher_是否支持startServer方法
   * 2. 如果支持,则调用fetcher_的startServer方法来启动服务器
   * 3. 如果不支持,则直接调用server的start方法
   * 
   * @param server 要启动的服务器实例
   * @param appInfo 应用信息,包含了启动所需的配置
   * @return 启动结果,成功返回Void,失败返回错误信息
   */
  Result<Void> startServer(Server &server, const flat::AppInfo &appInfo) {
    if constexpr (requires { fetcher_->startServer(server, appInfo); }) {
      return fetcher_->startServer(server, appInfo);
    } else {
      return server.start(appInfo);
    }
  }

  // 获取配置的访问器
  const auto &appConfig() const { return appConfig_; }
  const auto &launcherConfig() const { return launcherConfig_; }

 private:
  AppConfig appConfig_;           // 应用配置
  LauncherConfig launcherConfig_; // 启动器配置
  std::unique_ptr<RemoteConfigFetcher> fetcher_; // 远程配置获取器
};

}  // namespace hf3fs::core
