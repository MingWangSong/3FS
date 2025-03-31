/**
 * @file ApplicationBase.h
 * @brief 应用程序基类，提供应用程序生命周期管理和配置处理的基础框架
 * 
 * ApplicationBase是所有应用程序的基类，实现了应用程序的基本生命周期管理，
 * 包括启动、运行、配置加载和更新、信号处理以及优雅关闭等功能。
 * 该类采用模板方法设计模式，定义了应用程序的基本骨架，
 * 而具体的实现细节则由子类通过重写虚函数来提供。
 */
#pragma once

#include "common/app/AppInfo.h"
#include "common/app/ConfigStatus.h"
#include "common/app/ConfigUpdateRecord.h"
#include "common/logging/LogConfig.h"
#include "common/monitor/Monitor.h"
#include "common/net/Server.h"
#include "common/serde/Serde.h"
#include "common/utils/ConfigBase.h"
#include "common/utils/UtcTimeSerde.h"
#include "memory/common/MemoryAllocatorConfig.h"

namespace hf3fs {
/**
 * @class ApplicationBase
 * @brief 应用程序基类，提供应用程序的基本框架和生命周期管理
 * 
 * 该类实现了应用程序的通用功能，包括：
 * 1. 命令行参数解析
 * 2. 配置文件加载和管理
 * 3. 信号处理
 * 4. 应用程序生命周期管理（初始化、运行、关闭）
 * 5. 配置热更新支持
 */
class ApplicationBase {
 public:
  /**
   * @struct Config
   * @brief 应用程序基础配置结构
   * 
   * 包含日志、监控和内存分配器的基本配置项
   */
  struct Config : public ConfigBase<Config> {
    CONFIG_OBJ(log, logging::LogConfig);
    CONFIG_OBJ(monitor, monitor::Monitor::Config);
    CONFIG_OBJ(memory, memory::MemoryAllocatorConfig);
  };
  
  /**
   * @brief 运行应用程序
   * @param argc 命令行参数数量
   * @param argv 命令行参数数组
   * @return 应用程序退出码
   * 
   * 应用程序的主入口点，负责调用整个应用程序生命周期的各个阶段
   */
  int run(int argc, char *argv[]);

  /**
   * @brief 信号处理函数
   * @param signum 信号编号
   * 
   * 处理SIGINT、SIGTERM等信号，实现应用程序的优雅关闭
   */
  static void handleSignal(int signum);

  using ConfigFlags = std::vector<config::KeyValue>;
  
  /**
   * @brief 解析命令行参数中的配置标志
   * @param prefix 配置标志前缀
   * @param argc 命令行参数数量指针
   * @param argv 命令行参数数组指针
   * @param flags 解析结果存储容器
   * @return 解析结果
   */
  static Result<Void> (std::sparseFlagstring_view prefix, int *argc, char ***argv, ConfigFlags &flags);

  /**
   * @brief 初始化配置
   * @param cfg 配置对象
   * @param path 配置文件路径
   * @param dump 是否打印默认配置并退出
   * @param flags 命令行配置标志
   * @return 初始化结果
   */
  static Result<Void> initConfig(config::IConfig &cfg,
                                 const String &path,
                                 bool dump,
                                 const std::vector<config::KeyValue> &flags);

  /**
   * @brief 加载配置
   * @param cfg 配置对象
   * @param path 配置文件路径
   * @param flags 命令行配置标志
   * @return 加载结果
   */
  static Result<Void> loadConfig(config::IConfig &cfg, const String &path, const std::vector<config::KeyValue> &flags);

  /**
   * @brief 更新配置
   * @param configContent 新配置内容
   * @param configDesc 配置描述
   * @return 更新结果
   */
  static Result<Void> updateConfig(const String &configContent, const String &configDesc);

  /**
   * @brief 热更新配置
   * @param update 更新内容
   * @param render 是否需要渲染
   * @return 更新结果
   */
  static Result<Void> hotUpdateConfig(const String &update, bool render);

  /**
   * @brief 验证配置
   * @param configContent 配置内容
   * @param configDesc 配置描述
   * @return 验证结果
   */
  static Result<Void> validateConfig(const String &configContent, const String &configDesc);

  /**
   * @brief 获取配置字符串
   * @param configKey 配置键
   * @return 配置字符串
   */
  static Result<String> getConfigString(std::string_view configKey);

  /**
   * @brief 渲染配置
   * @param configContent 配置内容
   * @param testUpdate 是否测试更新
   * @param isHotUpdate 是否为热更新
   * @return 渲染结果和更新结果
   */
  static Result<std::pair<String, Result<String>>> renderConfig(const String &configContent,
                                                                bool testUpdate,
                                                                bool isHotUpdate);

  /**
   * @brief 获取应用信息
   * @return 应用信息
   */
  static std::optional<flat::AppInfo> getAppInfo();

  /**
   * @brief 获取最后一次配置更新记录
   * @return 配置更新记录
   */
  static std::optional<app::ConfigUpdateRecord> getLastConfigUpdateRecord();

  /**
   * @brief 获取配置状态
   * @return 配置状态
   */
  static ConfigStatus getConfigStatus();

 protected:
  /**
   * @brief 构造函数
   * 
   * 将当前应用实例注册为全局应用
   */
  ApplicationBase();
  
  /**
   * @brief 析构函数
   */
  ~ApplicationBase() = default;

  /**
   * @brief 停止应用程序
   * 
   * 子类必须实现此方法以提供应用程序的清理逻辑
   */
  virtual void stop() = 0;

  /**
   * @brief 应用程序主循环
   * @return 退出码
   * 
   * 设置信号处理并等待退出信号
   */
  virtual int mainLoop();

  /**
   * @brief 解析命令行参数
   * @param argc 命令行参数数量指针
   * @param argv 命令行参数数组指针
   * @return 解析结果
   * 
   * 子类必须实现此方法以提供特定的命令行参数解析逻辑
   */
  virtual Result<Void> parseFlags(int *argc, char ***argv) = 0;

  /**
   * @brief 初始化应用程序
   * @return 初始化结果
   * 
   * 子类必须实现此方法以提供应用程序的初始化逻辑
   */
  virtual Result<Void> initApplication() = 0;

  /**
   * @brief 获取配置对象
   * @return 配置对象指针
   * 
   * 子类必须实现此方法以提供应用程序的配置对象
   */
  virtual config::IConfig *getConfig() = 0;

  /**
   * @brief 获取应用信息
   * @return 应用信息指针
   * 
   * 子类可以重写此方法以提供应用程序的信息
   */
  virtual const flat::AppInfo *info() const { return nullptr; }

  /**
   * @brief 检查配置是否可推送
   * @return 是否可推送
   * 
   * 子类可以重写此方法以控制配置是否可以热更新
   */
  virtual bool configPushable() const { return true; }

  /**
   * @brief 配置更新回调
   * 
   * 子类可以重写此方法以在配置更新后执行特定操作
   */
  virtual void onConfigUpdated() {}
};

/**
 * @brief 停止并等待服务器关闭
 * @param server 服务器指针
 * 
 * 停止服务器并等待其完全关闭，同时清理监控和IB管理器
 */
void stopAndJoin(net::Server *server);
}  // namespace hf3fs
