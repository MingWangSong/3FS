#pragma once

#include <fmt/format.h>

#include "Utils.h"
#include "common/app/ApplicationBase.h"
#include "common/utils/LogCommands.h"

DECLARE_string(cfg);
DECLARE_bool(dump_default_cfg);
DECLARE_bool(use_local_cfg);

namespace hf3fs {
/**
 * @class TwoPhaseApplication
 * @brief 两阶段应用程序模板类，提供分阶段初始化的应用程序框架
 * 
 * @tparam Server 服务器类型，必须提供Launcher、Config和CommonConfig类型，以及kName常量
 * 
 * TwoPhaseApplication是ApplicationBase的模板子类，实现了一个两阶段初始化的应用程序框架。
 * 它通过模板参数接收一个服务器类型，并使用该服务器的Launcher组件进行第一阶段初始化，
 * 然后在第二阶段初始化和启动服务器。这种设计适用于复杂的应用程序，其初始化逻辑需要分阶段处理。
 */
  template <typename Server>
  class TwoPhaseApplication : public ApplicationBase {
    /**
     * @brief Launcher类型别名，从Server类型获取（依赖注入）meta的是core::ServerLauncher
     */
    using Launcher = typename Server::Launcher;
    
    /**
     * @brief 服务器配置类型别名，从Server类型获取（依赖注入）
     */
    using ServerConfig = typename Server::Config;

  public:
    /**
     * @brief 构造函数
     * 
     * 创建一个Launcher实例，用于第一阶段初始化
     */
    TwoPhaseApplication()
        : launcher_(std::make_unique<Launcher>()) {}

  private:
    /**
     * @class Config
     * @brief 应用程序配置类
     * 
     * 包含通用配置和服务器特定配置
     */
    struct Config : public ConfigBase<Config> {
      CONFIG_OBJ(common, typename Server::CommonConfig);  ///< 通用配置
      CONFIG_OBJ(server, ServerConfig);                   ///< 服务器特定配置
    };

    /**
     * @brief 解析命令行参数
     * @param argc 命令行参数数量指针
     * @param argv 命令行参数数组指针
     * @return 解析结果
     * 
     * 首先调用launcher的parseFlags方法，然后解析动态配置参数
     */
    Result<Void> parseFlags(int *argc, char ***argv) final {
      RETURN_ON_ERROR(launcher_->parseFlags(argc, argv));

      static constexpr std::string_view dynamicConfigPrefix = "--config.";
      return ApplicationBase::parseFlags(dynamicConfigPrefix, argc, argv, configFlags_);
    }

    /**
     * @brief 初始化应用程序
     * @return 初始化结果
     * 
     * 实现ApplicationBase::initApplication虚函数，
     * 完成应用程序的两阶段初始化：
     * 1. 第一阶段：通过launcher初始化
     * 2. 第二阶段：初始化和启动服务器
     */
    /**
     * @brief 初始化应用程序
     * @return 初始化结果
     * 
     * 使用final关键字修饰此虚函数，表示:
     * 1. 这是对基类ApplicationBase中的纯虚函数initApplication的最终实现
     * 2. 禁止TwoPhaseApplication的子类重写此方法
     * 3. 编译器可以进行优化,因为知道这是最终实现
     */
    Result<Void> initApplication() final {
      // 如果指定了dump_default_cfg标志，则打印默认配置并退出
      if (FLAGS_dump_default_cfg) {
        fmt::print("{}\n", config_.toString());
        exit(0);
      }

      // 第一阶段：通过launcher初始化
      auto firstInitRes = launcher_->init();
      XLOGF_IF(FATAL, !firstInitRes, "Failed to init launcher: {}", firstInitRes.error());

      // 加载应用信息
      app_detail::loadAppInfo([this] { return launcher_->loadAppInfo(); }, appInfo_);
      
      // 初始化配置
      app_detail::initConfig(config_, configFlags_, appInfo_, [this] { return launcher_->loadConfigTemplate(); });
      XLOGF(INFO, "Server config inited");

      // 初始化通用组件
      app_detail::initCommonComponents(config_.common(), Server::kName, appInfo_.nodeId);

      // 设置日志和内存配置更新回调
      onLogConfigUpdated_ = app_detail::makeLogConfigUpdateCallback(config_.common().log(), Server::kName);
      onMemConfigUpdated_ = app_detail::makeMemConfigUpdateCallback(config_.common().memory(), appInfo_.hostname);

      // 记录完整配置信息
      XLOGF(INFO, "Full Config:\n{}", config_.toString());
      
      // 持久化配置
      app_detail::persistConfig(config_);

      // 第二阶段：初始化服务器
      XLOGF(INFO, "Start to init server");
      auto initRes = initServer();
      XLOGF_IF(FATAL, !initRes, "Init server failed: {}", initRes.error());
      XLOGF(INFO, "Init server finished");

      // 启动服务器
      XLOGF(INFO, "Start to start server");
      auto startRes = startServer();
      XLOGF_IF(FATAL, !startRes, "Start server failed: {}", startRes.error());
      XLOGF(INFO, "Start server finished");

      // 释放launcher资源
      launcher_.reset();

      return Void{};
    }

    /**
     * @brief 停止应用程序
     * 
     * 停止launcher和服务器，释放资源
     */
    void stop() final {
      XLOGF(INFO, "Stop TwoPhaseApplication...");
      if (launcher_) {
        launcher_.reset();
      }
      stopAndJoin(server_.get());
      server_.reset();
      XLOGF(INFO, "Stop TwoPhaseApplication finished");
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
    const flat::AppInfo *info() const final { return &appInfo_; }

    /**
     * @brief 判断配置是否可推送
     * @return 如果配置可推送则返回true，否则返回false
     * 
     * 当未指定配置文件且未使用本地配置时，配置可推送
     */
    bool configPushable() const final { return FLAGS_cfg.empty() && !FLAGS_use_local_cfg; }

    /**
     * @brief 配置更新回调
     * 
     * 当配置更新时，持久化配置
     */
    void onConfigUpdated() { app_detail::persistConfig(config_); }

  private:
    /**
     * @brief 初始化服务器
     * @return 初始化结果
     * 
     * 创建服务器实例并设置
     */
    Result<Void> initServer() {
      server_ = std::make_unique<Server>(config_.server());
      RETURN_ON_ERROR(server_->setup());
      XLOGF(INFO, "{}", server_->describe());
      return Void{};
    }

    /**
     * @brief 启动服务器
     * @return 启动结果
     * 
     * 通过launcher启动服务器，并更新应用信息
     */
    Result<Void> startServer() {
      auto startResult = launcher_->startServer(*server_, appInfo_);
      XLOGF_IF(FATAL, !startResult, "Start server failed: {}", startResult.error());
      appInfo_ = server_->appInfo();
      return Void{};
    }

    ConfigFlags configFlags_;  ///< 配置标志

    Config config_;  ///< 应用程序配置
    flat::AppInfo appInfo_;  ///< 应用信息
    std::unique_ptr<Launcher> launcher_;  ///< Launcher实例
    std::unique_ptr<Server> server_;  ///< 服务器实例
    std::unique_ptr<ConfigCallbackGuard> onLogConfigUpdated_;  ///< 日志配置更新回调
    std::unique_ptr<ConfigCallbackGuard> onMemConfigUpdated_;  ///< 内存配置更新回调
  };
}  // namespace hf3fs
