/**
 * @file ApplicationBase.cc
 * @brief ApplicationBase类的实现
 * 
 * 实现了应用程序的基本生命周期管理，包括启动、运行、配置加载和更新、信号处理以及优雅关闭等功能。
 */
#include "common/app/ApplicationBase.h"

#include <folly/init/Init.h>
#include <iostream>
#include <sys/signal.h>

#include "common/app/ConfigManager.h"
#include "common/app/Thread.h"
#include "common/utils/OptionalUtils.h"
#include "common/utils/RenderConfig.h"
#include "common/utils/StringUtils.h"
#include "memory/common/GlobalMemoryAllocator.h"

DECLARE_bool(release_version);

namespace hf3fs {
  /*
  匿名命名空间(anonymous namespace)是C++的一个特性,它可以将变量和函数的作用域限制在当前文件内。
  使用匿名命名空间的好处是:

  1. 实现文件级别的静态变量/函数,避免命名冲突
    - 这里的appMutex、globalApp等变量只在ApplicationBase.cc文件内可见
    - 其他文件即使定义同名变量也不会冲突

  2. 隐藏实现细节,提高封装性
    - 这些变量是ApplicationBase类的内部实现细节
    - 不需要暴露给外部使用者
    - 通过匿名命名空间隐藏起来更安全

  3. 替代static关键字
    - 匿名命名空间是C++推荐的替代static声明的方式
    - 语义更清晰,作用域控制更严格

  所以这里使用匿名命名空间来封装ApplicationBase类的内部状态变量是很好的实践。
  */
  namespace {
    // 全局应用程序互斥锁和指针，用于管理全局唯一的应用程序实例
    std::mutex appMutex;
    ApplicationBase *globalApp = nullptr;

    // 主循环控制变量，用于信号处理和优雅退出
    std::mutex loopMutex;
    std::condition_variable loopCv;
    std::atomic<bool> exitLoop = false;
    std::atomic<int> exitCode = 0;

    /**
     * @brief 获取配置管理器单例
     * @return 配置管理器引用
     */
    app::ConfigManager &getConfigManager() {
      static app::ConfigManager cm;
      return cm;
    }

    /**
     * @brief 记录配置更新结果的宏
     * 
     * 执行配置更新操作并记录结果
     */
    #define RETURN_AND_RECORD_CONFIG_UPDATE(ret, desc)                                                     \
      Result<Void> _r = (ret);                                                                             \
      getConfigManager().updateConfigRecord(_r.hasError() ? _r.error() : Status(StatusCode::kOK), (desc)); \
      return _r;

  }  // namespace

  /**
   * @brief ApplicationBase构造函数
   * 
   * 将当前应用实例注册为全局应用
   */
  ApplicationBase::ApplicationBase() { globalApp = this; }

  /**
   * @brief 信号处理函数
   * @param signum 信号编号
   * 
   * 处理各种信号并触发应用程序退出
   */
  void ApplicationBase::handleSignal(int signum) {
    XLOGF(ERR, "Handle {} signal.", strsignal(signum));
    exitLoop = true;
    if (signum == SIGUSR2) {
      exitCode = 128 + SIGUSR2;
    }
    loopCv.notify_one();
  }

  /**
   * @brief 运行应用程序
   * @param argc 命令行参数数量
   * @param argv 命令行参数数组
   * @return 应用程序退出码
   * 
   * 应用程序的主入口点，负责调用整个应用程序生命周期的各个阶段：
   * 1. 阻塞中断信号
   * 2. 解析命令行参数
   * 3. 初始化folly库
   * 4. 初始化应用程序
   * 5. 进入主循环
   * 6. 清理资源并退出
   */
  int ApplicationBase::run(int argc, char *argv[]) {
    Thread::blockInterruptSignals();

    // 解析命令行参数（实际没用）
    auto parseFlagsRes = parseFlags(&argc, &argv);
    XLOGF_IF(FATAL, !parseFlagsRes, "Parse flags failed: {}", parseFlagsRes.error());

    // 初始化folly库（自带解析命令行参数）
    folly::init(&argc, &argv);

    // 如果指定了release_version标志，则打印版本信息并退出
    if (FLAGS_release_version) {
      fmt::print("{}\n{}\n", VersionInfo::full(), VersionInfo::commitHashFull());
      return 0;
    }

    // 初始化应用程序
    auto initRes = initApplication();
    XLOGF_IF(FATAL, !initRes, "Init application failed: {}", initRes.error());

    // 进入主循环
    auto exitCode = mainLoop();

    // 清理资源
    memory::shutdown();
    stop();

    return exitCode;
  }

  /**
   * @brief 应用程序主循环
   * @return 退出码
   * 
   * 设置信号处理器并等待退出信号
   */
  int ApplicationBase::mainLoop() {
    // 注册信号处理函数
    signal(SIGINT, handleSignal);
    signal(SIGTERM, handleSignal); 
    signal(SIGUSR1, handleSignal);
    signal(SIGUSR2, handleSignal);

    // 解除中断信号阻塞
    Thread::unblockInterruptSignals();

    // 等待退出信号
    {
      auto lock = std::unique_lock(loopMutex);
      loopCv.wait(lock, [] { return exitLoop.load(); });
    }

    return exitCode.load();
  }

  /**
   * @brief 解析命令行参数中的配置标志
   * @param prefix 配置标志前缀
   * @param argc 命令行参数数量指针
   * @param argv 命令行参数数组指针
   * @param flags 解析结果存储容器
   * @return 解析结果
   */
  Result<Void> ApplicationBase::parseFlags(std::string_view prefix, int *argc, char ***argv, ConfigFlags &flags) {
    auto res = config::parseFlags(prefix, *argc, *argv);
    if (!res) {
      XLOGF(ERR, "Parse config flags start with {} failed: {}", prefix, res.error());
      return makeError(StatusCode::kConfigInvalidValue,
                      fmt::format("Parse config flags start with {} failed: {}", prefix, res.error()));
    }
    flags = std::move(*res);
    return Void{};
  }

  /**
   * @brief 初始化配置
   * @param cfg 配置对象
   * @param path 配置文件路径
   * @param dump 是否打印默认配置并退出
   * @param flags 命令行配置标志
   * @return 初始化结果
   * 
   * 如果dump为true，则打印默认配置并退出；否则加载配置
   */
  Result<Void> ApplicationBase::initConfig(config::IConfig &cfg,
                                          const String &path,
                                          bool dump,
                                          const std::vector<config::KeyValue> &flags) {
    if (dump) {
      std::cout << cfg.toString() << std::endl;
      exit(0);
      __builtin_unreachable();
    }
    return loadConfig(cfg, path, flags);
  }

  /**
   * @brief 更新配置
   * @param configContent 新配置内容
   * @param configDesc 配置描述
   * @return 更新结果
   * 
   * 更新应用程序的配置，并在成功时调用onConfigUpdated回调
   */
  Result<Void> ApplicationBase::updateConfig(const String &configContent, const String &configDesc) {
    auto lock = std::unique_lock(appMutex);
    // 检查全局应用和配置是否存在
    if (!globalApp || !globalApp->getConfig()) {
      XLOGF(WARN, "Update config ignored: no app found. desc: {}", configDesc);
      RETURN_AND_RECORD_CONFIG_UPDATE(makeError(StatusCode::kNoApplication), configDesc);
    }
    // 检查配置是否可推送
    if (!globalApp->configPushable()) {
      XLOGF(WARN,
            "Update config ignored: this app declares that its config is not hotupdatable. desc: {}\nConfig content:\n{}",
            configDesc,
            configContent);
      RETURN_AND_RECORD_CONFIG_UPDATE(makeError(StatusCode::kCannotPushConfig), configDesc);
    }
    // 更新配置
    auto res = getConfigManager().updateConfig(configContent, configDesc, *globalApp->getConfig(), globalApp->info());
    if (res) {
      globalApp->onConfigUpdated();
    }
    return res;
  }

  /**
   * @brief 渲染配置
   * @param configContent 配置内容
   * @param testUpdate 是否测试更新
   * @param isHotUpdate 是否为热更新
   * @return 渲染结果和更新结果
   * 
   * 渲染配置内容，如果testUpdate为true，则测试更新并返回结果
   */
  Result<std::pair<String, Result<String>>> ApplicationBase::renderConfig(const String &configContent,
                                                                          bool testUpdate,
                                                                          bool isHotUpdate) {
    auto lock = std::unique_lock(appMutex);
    if (!globalApp) {
      return makeError(StatusCode::kNoApplication);
    }
    // 渲染配置
    auto renderRes = hf3fs::renderConfig(configContent, globalApp->info());
    RETURN_ON_ERROR(renderRes);
    if (!testUpdate) {
      return std::make_pair(std::move(*renderRes), Result<String>(""));
    }
    // 测试更新
    auto *cfg = globalApp->getConfig();
    if (!cfg) {
      return makeError(StatusCode::kNoApplication);
    }
    auto newCfg = cfg->clonePtr();
    auto updateRes = newCfg->atomicallyUpdate(std::string_view(*renderRes), isHotUpdate);
    if (updateRes.hasError()) {
      return std::make_pair(std::move(*renderRes), Result<String>(makeError(updateRes.error())));
    } else {
      return std::make_pair(std::move(*renderRes), Result<String>(newCfg->toString()));
    }
  }

  /**
   * @brief 热更新配置
   * @param update 更新内容
   * @param render 是否需要渲染
   * @return 更新结果
   * 
   * 热更新应用程序的配置，并在成功时调用onConfigUpdated回调
   */
  Result<Void> ApplicationBase::hotUpdateConfig(const String &update, bool render) {
    auto lock = std::unique_lock(appMutex);
    if (!globalApp || !globalApp->getConfig()) {
      return makeError(StatusCode::kNoApplication);
    }
    auto res = getConfigManager().hotUpdateConfig(update, render, *globalApp->getConfig(), globalApp->info());
    if (res) {
      globalApp->onConfigUpdated();
    }
    return res;
  }

  /**
   * @brief 验证配置
   * @param configContent 配置内容
   * @param configDesc 配置描述
   * @return 验证结果
   * 
   * 验证配置内容是否有效，包括渲染和验证两个步骤
   */
  Result<Void> ApplicationBase::validateConfig(const String &configContent, const String &configDesc) {
    auto lock = std::unique_lock(appMutex);
    if (!globalApp) {
      XLOGF(WARN, "Validate config ignored: no app found. desc: {}\nConfig content:\n{}", configDesc, configContent);
      return Void{};
    }
    auto *cfg = globalApp->getConfig();
    if (!cfg) {
      XLOGF(WARN, "Validate config ignored: no config found. desc: {}\nConfig content:\n{}", configDesc, configContent);
      return Void{};
    }
    // 渲染配置
    auto renderRes = hf3fs::renderConfig(configContent, globalApp->info());
    if (renderRes.hasError()) {
      XLOGF(ERR,
            "Validate config failed at rendering: {}. desc: {}\nConfig content:\n{}",
            renderRes.error(),
            configDesc,
            configContent);
      RETURN_ERROR(renderRes);
    }
    // 验证配置
    auto validateRes = cfg->validateUpdate(std::string_view(*renderRes));
    if (validateRes.hasError()) {
      XLOGF(ERR,
            "Validate config failed at validating: {}. desc: {}\nConfig content:\n{}\nRendered content:\n{}",
            validateRes.error(),
            configDesc,
            configContent,
            *renderRes);
      RETURN_ERROR(validateRes);
    }
    XLOGF(INFO,
          "Validate config succeeded. desc: {}\nConfig content:\n{}\nRendered content:\n{}",
          configDesc,
          configContent,
          *renderRes);
    return Void{};
  }

  /**
   * @brief 加载配置
   * @param cfg 配置对象
   * @param path 配置文件路径
   * @param flags 命令行配置标志
   * @return 加载结果
   * 
   * 从文件和命令行参数加载配置
   */
  Result<Void> ApplicationBase::loadConfig(config::IConfig &cfg,
                                          const String &path,
                                          const std::vector<config::KeyValue> &flags) {
    // 从文件加载配置
    if (!path.empty()) {
      XLOGF(INFO, "Try to load config from file [{}]", path);

      toml::parse_result parseResult;
      try {
        parseResult = toml::parse_file(path);
      } catch (const toml::parse_error &e) {
        std::stringstream ss;
        ss << e;
        XLOGF(ERR, "Parse config file [{}] failed: {}", path, ss.str());
        return makeError(StatusCode::kConfigParseError);
      } catch (std::exception &e) {
        XLOGF(ERR, "Parse config file [{}] failed: {}", path, e.what());
        return makeError(StatusCode::kConfigInvalidValue);
      }

      auto updateResult = cfg.update(parseResult, /* isHotUpdate = */ false);
      if (UNLIKELY(!updateResult)) {
        XLOGF(ERR, "Update config from file [{}] failed: {}", path, updateResult.error());
        return makeError(StatusCode::kConfigInvalidValue);
      }
    }

    // 从命令行参数加载配置
    auto updateResult = cfg.update(flags, /* isHotUpdate = */ false);
    if (UNLIKELY(!updateResult)) {
      XLOGF(ERR, "Update config from command-line flags failed: {}", updateResult.error());
      return makeError(StatusCode::kConfigInvalidValue);
    }

    return Void{};
  }

  /**
   * @brief 获取应用信息
   * @return 应用信息
   */
  std::optional<flat::AppInfo> ApplicationBase::getAppInfo() {
    auto lock = std::unique_lock(appMutex);
    if (globalApp) {
      return makeOptional(globalApp->info());
    }
    return std::nullopt;
  }

  /**
   * @brief 获取配置字符串
   * @param configKey 配置键
   * @return 配置字符串
   */
  Result<String> ApplicationBase::getConfigString(std::string_view configKey) {
    auto lock = std::unique_lock(appMutex);
    if (auto *cfg = globalApp ? globalApp->getConfig() : nullptr; cfg) {
      auto toml = cfg->toToml(configKey);
      RETURN_ON_ERROR(toml);
      std::stringstream ss;
      ss << toml::toml_formatter(*toml, toml::toml_formatter::default_flags & ~toml::format_flags::indentation);
      return ss.str();
    }
    return makeError(StatusCode::kNoApplication);
  }

  /**
   * @brief 获取最后一次配置更新记录
   * @return 配置更新记录
   */
  std::optional<app::ConfigUpdateRecord> ApplicationBase::getLastConfigUpdateRecord() {
    auto lock = std::unique_lock(appMutex);
    return getConfigManager().lastConfigUpdateRecord;
  }

  /**
   * @brief 获取配置状态
   * @return 配置状态
   */
  ConfigStatus ApplicationBase::getConfigStatus() {
    auto lock = std::unique_lock(appMutex);
    return getConfigManager().configStatus;
  }

  /**
   * @brief 停止并等待服务器关闭
   * @param server 服务器指针
   * 
   * 停止服务器并等待其完全关闭，同时清理监控和IB管理器
   */
  void stopAndJoin(net::Server *server) {
    XLOGF(INFO, "Stop the server...");
    if (server) {
      server->stopAndJoin();
    }
    XLOGF(INFO, "Stop server finished.");
    monitor::Monitor::stop();
    hf3fs::net::IBManager::stop();
  }
}  // namespace hf3fs
