/**
 * @file OnePhaseApplication.h
 * @brief 单阶段应用程序模板类，提供简化的应用程序实现
 * 
 * OnePhaseApplication是ApplicationBase的模板子类，实现了一个简单的单阶段应用程序框架。
 * 它通过模板参数接收一个服务器类型，并提供了标准化的配置管理和应用程序生命周期管理。
 * 该类使用单例模式，确保应用程序实例的全局唯一性。
 */
#pragma once

#include <iostream>

#include "AppInfo.h"
#include "ApplicationBase.h"
#include "Utils.h" 
#include "common/logging/LogInit.h"
#include "common/net/Server.h"
#include "common/net/ib/IBDevice.h"
#include "common/utils/LogCommands.h"
#include "common/utils/SysResource.h"

DECLARE_string(app_cfg);
DECLARE_bool(dump_default_app_cfg);

DECLARE_string(cfg);
DECLARE_bool(dump_cfg);
DECLARE_bool(dump_default_cfg);

namespace hf3fs {
  /**
   * @class OnePhaseApplication
   * @brief 单阶段应用程序模板类
   * 
   * @tparam T 服务器类型，必须提供Config类型和kName常量
   * 
   * 该类实现了一个简单的单阶段应用程序框架，通过模板参数接收一个服务器类型，
   * 并提供了标准化的配置管理和应用程序生命周期管理。
   * 使用单例模式确保应用程序实例的全局唯一性。
   */
  template <class T>
  requires requires {
    typename T::Config;
    std::string_view(T::kName);
  }
  class OnePhaseApplication : public ApplicationBase {
  public:
    /**
     * @class CommonConfig
     * @brief 通用配置类
     * 
     * 包含集群ID、日志、监控和IB设备的配置项
     */
    class CommonConfig : public ConfigBase<CommonConfig> {
      CONFIG_ITEM(cluster_id, "");
      CONFIG_OBJ(log, logging::LogConfig);
      CONFIG_OBJ(monitor, monitor::Monitor::Config);
      CONFIG_OBJ(ib_devices, net::IBDevice::Config);
    };

    /**
     * @class Config
     * @brief 应用程序配置类
     * 
     * 包含通用配置和服务器特定配置
     */
    class Config : public ConfigBase<Config> {
    public:
      CONFIG_OBJ(common, CommonConfig);
      CONFIG_OBJ(server, typename T::Config);
    };

    /**
     * @struct AppConfig
     * @brief 应用程序基础配置结构
     * 
     * 包含节点ID和是否允许空节点ID的配置项
     */
    struct AppConfig : public ConfigBase<AppConfig> {
      CONFIG_ITEM(node_id, 0);
      CONFIG_ITEM(allow_empty_node_id, true);
    };

    /**
     * @brief 构造函数
     * @param appConfig 应用程序基础配置
     * @param config 应用程序配置
     */
    OnePhaseApplication(AppConfig &appConfig, Config &config)
        : appConfig_(appConfig),
          config_(config) {}

    /**
     * @brief 获取单例实例
     * @return 应用程序单例引用
     * 
     * 使用静态局部变量实现线程安全的单例模式
     */
    static OnePhaseApplication &instance() {
      static AppConfig appConfig;
      static Config config;
      static OnePhaseApplication app(appConfig, config);
      return app;
    }

    /**
     * @brief 解析命令行参数
     * @param argc 命令行参数数量指针
     * @param argv 命令行参数数组指针
     * @return 解析结果
     * 
     * 解析应用程序配置和服务器配置的命令行参数
     */
    Result<Void> parseFlags(int *argc, char ***argv) final {
      static constexpr std::string_view appConfigPrefix = "--app_config.";
      static constexpr std::string_view configPrefix = "--config.";

      RETURN_ON_ERROR(ApplicationBase::parseFlags(appConfigPrefix, argc, argv, appConfigFlags_));
      RETURN_ON_ERROR(ApplicationBase::parseFlags(configPrefix, argc, argv, configFlags_));
      return Void{};
    }

    /**
     * @brief 初始化应用程序
     * @return 初始化结果
     * 
     * 实现ApplicationBase::initApplication虚函数，
     * 完成应用程序的初始化，包括：
     * 1. 加载应用程序配置
     * 2. 初始化IB设备
     * 3. 初始化日志系统
     * 4. 初始化监控系统
     * 5. 创建并启动服务器
     */
    Result<Void> initApplication() final {  
      // 第一阶段：配置加载
      // 从文件加载应用程序基础配置(如节点ID等)
      if (!FLAGS_app_cfg.empty()) {
        app_detail::initConfigFromFile(appConfig_, FLAGS_app_cfg, FLAGS_dump_default_app_cfg, appConfigFlags_);
      }
      // 验证节点ID配置，如果不允许空节点ID但节点ID为0，则终止程序
      XLOGF_IF(FATAL, !appConfig_.allow_empty_node_id() && appConfig_.node_id() == 0, "node_id is not allowed to be 0");

      // 从文件加载服务器配置
      if (!FLAGS_cfg.empty()) {
        app_detail::initConfigFromFile(config_, FLAGS_cfg, FLAGS_dump_default_cfg, configFlags_);
      }

      // 如果指定了dump_cfg标志，则打印配置并退出
      // 这通常用于调试和配置验证
      if (FLAGS_dump_cfg) {
        std::cout << config_.toString() << std::endl;
        exit(0);
      }

      // 第二阶段：基础组件初始化
      // 初始化IB(InfiniBand)设备管理器
      // IB是一种高性能网络技术，通常用于高性能计算和数据中心
      auto ibResult = net::IBManager::start(config_.common().ib_devices());
      // 如果初始化失败，记录错误并终止程序
      XLOGF_IF(FATAL, !ibResult, "Failed to start IBManager: {}", ibResult.error());

      // 初始化日志系统
      // 根据配置生成日志配置字符串，并使用服务器名称作为日志标识
      auto logConfigStr = logging::generateLogConfig(config_.common().log(), String(T::kName));
      XLOGF(INFO, "LogConfig: {}", logConfigStr);
      // 初始化日志系统，如果失败则终止程序
      logging::initOrDie(logConfigStr);
      // 记录版本信息
      XLOGF(INFO, "{}", VersionInfo::full());
      // 记录完整配置信息，便于调试和问题排查
      XLOGF(INFO, "Full AppConfig:\n{}", config_.toString());
      XLOGF(INFO, "Full Config:\n{}", config_.toString());

      // 初始化网络等待器单例
      // Waiter用于异步网络操作的事件处理
      XLOGF(INFO, "Init waiter singleton {}", fmt::ptr(&net::Waiter::instance()));

      // 初始化监控系统
      // 监控系统用于收集和报告应用程序的运行状态和性能指标
      auto monitorResult = monitor::Monitor::start(config_.common().monitor());
      XLOGF_IF(FATAL, !monitorResult, "Parse config file from flags failed: {}", monitorResult.error());

      // 第三阶段：服务器初始化
      // 创建服务器实例，使用模板参数T指定的服务器类型
      // 这里体现了工厂方法模式，由子类决定创建哪种具体的服务器
      server_ = std::make_unique<T>(config_.server());
      // 设置服务器，包括初始化服务器的各种组件和资源
      auto setupResult = server_->setup();
      XLOGF_IF(FATAL, !setupResult, "Setup server failed: {}", setupResult.error());

      // 第四阶段：应用信息填充
      // 获取物理机主机名
      auto hostnameResult = SysResource::hostname(/*physicalMachineName=*/true);
      XLOGF_IF(FATAL, !hostnameResult, "Get hostname failed: {}", hostnameResult.error());

      // 获取Pod名称(容器环境中的主机名)
      auto podnameResult = SysResource::hostname(/*physicalMachineName=*/false);
      XLOGF_IF(FATAL, !podnameResult, "Get podname failed: {}", podnameResult.error());

      // 填充应用信息对象，包括节点ID、集群ID、主机名、进程ID等
      // 这些信息用于服务注册、监控和日志记录
      info_.nodeId = flat::NodeId(appConfig_.node_id());
      info_.clusterId = config_.common().cluster_id();
      info_.hostname = *hostnameResult;
      info_.podname = *podnameResult;
      info_.pid = SysResource::pid();
      info_.releaseVersion = flat::ReleaseVersion::fromVersionInfo();
      // 添加服务组信息，包括服务名称和地址列表
      for (auto &group : server_->groups()) {
        info_.serviceGroups.emplace_back(group->serviceNameList(), group->addressList());
      }
      // 记录服务器描述信息
      XLOGF(INFO, "{}", server_->describe());

      // 第五阶段：服务器启动
      // 启动服务器，传入应用信息
      // 这将启动所有服务组和监听器，使服务器开始接受请求
      auto startResult = server_->start(info_);
      XLOGF_IF(FATAL, !startResult, "Start server failed: {}", startResult.error());

      // 初始化成功，返回空值
      return Void{};
    }

    /**
     * @brief 获取配置对象
     * @return 配置对象指针
     */
    config::IConfig *getConfig() final { return &config_; }

    /**
     * @brief 获取应用信息
     * @return 应用信息指针
     */
    const flat::AppInfo *info() const final { return &info_; }

    /**
     * @brief 停止应用程序
     * 
     * 停止服务器并等待其完全关闭
     */
    void stop() final { stopAndJoin(server_.get()); }

  private:
    /**
     * @brief 私有默认构造函数
     * 
     * 防止外部直接创建实例
     */
    OnePhaseApplication() = default;

    ConfigFlags appConfigFlags_;  ///< 应用程序配置标志
    ConfigFlags configFlags_;     ///< 服务器配置标志

    AppConfig &appConfig_;        ///< 应用程序基础配置引用
    Config &config_;              ///< 应用程序配置引用
    flat::AppInfo info_;          ///< 应用信息
    std::unique_ptr<net::Server> server_;  ///< 服务器实例
  };

}  // namespace hf3fs
