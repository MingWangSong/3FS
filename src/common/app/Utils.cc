#include "Utils.h"

#include "common/logging/LogInit.h"
#include "common/utils/FileUtils.h"
#include "common/utils/RenderConfig.h"

// 命令行参数声明
DECLARE_string(cfg);              // 配置文件路径
DECLARE_bool(dump_cfg);           // 是否打印配置
DECLARE_bool(dump_default_cfg);   // 是否打印默认配置
DECLARE_bool(use_local_cfg);      // 是否使用本地配置
DECLARE_string(cfg_persist_prefix); // 配置持久化前缀

// jemalloc 内存分配器相关函数声明
extern "C" {
__attribute__((weak)) void __attribute__((nothrow))
malloc_stats_print(void (*write_cb)(void *, const char *), void *je_cbopaque, const char *opts);
}

namespace hf3fs::app_detail {
namespace {

// 内存分析相关工具函数
bool profiling_active = false;  // 内存分析是否激活

/**
 * @brief 控制 jemalloc 内存分析功能
 * @param active 是否激活内存分析
 * @param prefix 分析结果文件前缀
 * @return 操作是否成功
 * 
 * 功能：
 * 1. 控制内存分析采样是否激活
 * 2. 设置分析结果文件前缀
 * 3. 导出内存分析结果
 */
bool je_profiling(bool active, const char *prefix) {
  /*
    prof.active (bool) rw [--enable-prof]
    Control whether sampling is currently active. See the opt.prof_active option for additional information, as well
    as the interrelated thread.prof.active mallctl.

    prof.dump (const char *) -w [--enable-prof]
    Dump a memory profile to the specified file, or if NULL is specified, to a file according to the pattern
    <prefix>.<pid>.<seq>.m<mseq>.heap, where <prefix> is controlled by the opt.prof_prefix and prof.prefix options.

    prof.prefix (const char *) -w [--enable-prof]
    Set the filename prefix for profile dumps. See opt.prof_prefix for the default setting. This can be useful to
    differentiate profile dumps such as from forked processes.
  */

  if (profiling_active) {
    // first dump memory profiling

    if (prefix != nullptr) {
      auto err = mallctl("prof.prefix", nullptr, nullptr, &prefix, sizeof(prefix));

      if (err) {
        fprintf(stderr, "Failed to set profiling dump prefix: %s, error: %d\n", prefix, err);
        return false;
      }
    }

    auto err = mallctl("prof.dump", nullptr, nullptr, nullptr, 0);

    if (err) {
      fprintf(stderr, "Failed to dump memory profiling, prefix: %s, error: %d\n", prefix, err);
      return false;
    }

    fprintf(stderr, "Memory profiling result saved with prefix: %s\n", prefix);
  }

  if (profiling_active == active) {
    fprintf(stderr, "Memory profiling already %s\n", active ? "enabled" : "disabled");
    return true;
  }

  auto err = mallctl("prof.active", nullptr, nullptr, (void *)&active, sizeof(active));

  if (err) {
    fprintf(stderr, "Failed to %s memory profiling, error :%d\n", active ? "enable" : "disable", err);
    return false;
  }

  fprintf(stderr,
          "Memory profiling set from '%s' to '%s'\n",
          profiling_active ? "active" : "inactive",
          active ? "active" : "inactive");
  profiling_active = active;

  return true;
}

/**
 * @brief 获取 jemalloc 内存分配器状态
 * @param buf 输出缓冲区
 * @param size 缓冲区大小
 * 
 * 功能：
 * 1. 获取内存分配统计信息
 * 2. 包括已分配、活跃、元数据、常驻、映射、保留等内存数据
 * 3. 记录 jemalloc 配置信息
 */
void je_logstatus(char *buf, size_t size) {
  size_t allocated, active, metadata, resident, mapped, retained, epoch = 1;
  size_t len = sizeof(size_t);

  /*
    stats.allocated (size_t) r- [--enable-stats]
    Total number of bytes allocated by the application.

    stats.active (size_t) r- [--enable-stats]
    Total number of bytes in active pages allocated by the application. This is a multiple of the page size, and
    greater than or equal to stats.allocated. This does not include stats.arenas.<i>.pdirty, stats.arenas.<i>.pmuzzy,
    nor pages entirely devoted to allocator metadata.

    stats.metadata (size_t) r- [--enable-stats]
    Total number of bytes dedicated to metadata, which comprise base allocations used for bootstrap-sensitive
    allocator metadata structures (see stats.arenas.<i>.base) and internal allocations (see
    stats.arenas.<i>.internal). Transparent huge page (enabled with opt.metadata_thp) usage is not considered.

    stats.resident (size_t) r- [--enable-stats]
    Maximum number of bytes in physically resident data pages mapped by the allocator, comprising all pages dedicated
    to allocator metadata, pages backing active allocations, and unused dirty pages. This is a maximum rather than
    precise because pages may not actually be physically resident if they correspond to demand-zeroed virtual memory
    that has not yet been touched. This is a multiple of the page size, and is larger than stats.active.

    stats.mapped (size_t) r- [--enable-stats]
    Total number of bytes in active extents mapped by the allocator. This is larger than stats.active. This does not
    include inactive extents, even those that contain unused dirty pages, which means that there is no strict ordering
    between this and stats.resident.

    stats.retained (size_t) r- [--enable-stats]
    Total number of bytes in virtual memory mappings that were retained rather than being returned to the operating
    system via e.g. munmap(2) or similar. Retained virtual memory is typically untouched, decommitted, or purged, so
    it has no strongly associated physical memory (see extent hooks for details). Retained memory is excluded from
    mapped memory statistics, e.g. stats.mapped.
  */

  if (mallctl("epoch", nullptr, nullptr, &epoch, sizeof(epoch)) == 0 &&
      mallctl("stats.allocated", &allocated, &len, nullptr, 0) == 0 &&
      mallctl("stats.active", &active, &len, nullptr, 0) == 0 &&
      mallctl("stats.metadata", &metadata, &len, nullptr, 0) == 0 &&
      mallctl("stats.resident", &resident, &len, nullptr, 0) == 0 &&
      mallctl("stats.mapped", &mapped, &len, nullptr, 0) == 0 &&
      mallctl("stats.retained", &retained, &len, nullptr, 0) == 0) {
    // Get the jemalloc config string from environment variable (for logging purposes only).
    const char *configStr = std::getenv("JE_MALLOC_CONF");
    std::snprintf(buf,
                  size,
                  "jemalloc enabled (JE_MALLOC_CONF=\"%s\", profiling=%s), "
                  "current allocated=%zu active=%zu metadata=%zu resident=%zu mapped=%zu, retained=%zu",
                  configStr == nullptr ? "(null)" : configStr,
                  profiling_active ? "active" : "inactive",
                  allocated,
                  active,
                  metadata,
                  resident,
                  mapped,
                  retained);
  }
}

}  // namespace

/**
 * @brief 解析命令行参数
 * @param argc 参数数量
 * @param argv 参数数组
 * @param appConfigFlags 应用配置标志
 * @param launcherConfigFlags 启动器配置标志
 * @param configFlags 通用配置标志
 * 
 * 功能：
 * 1. 解析应用配置参数
 * 2. 解析启动器配置参数
 * 3. 解析动态配置参数
 */
Result<Void> parseFlags(int *argc,
                        char ***argv,
                        ApplicationBase::ConfigFlags &appConfigFlags,
                        ApplicationBase::ConfigFlags &launcherConfigFlags,
                        ApplicationBase::ConfigFlags &configFlags) {
  static constexpr std::string_view appConfigPrefix = "--app_config.";
  static constexpr std::string_view launcherConfigPrefix = "--launcher_config.";
  static constexpr std::string_view dynamicConfigPrefix = "--config.";

  RETURN_ON_ERROR(ApplicationBase::parseFlags(appConfigPrefix, argc, argv, appConfigFlags));
  RETURN_ON_ERROR(ApplicationBase::parseFlags(launcherConfigPrefix, argc, argv, launcherConfigFlags));
  RETURN_ON_ERROR(ApplicationBase::parseFlags(dynamicConfigPrefix, argc, argv, configFlags));
  return Void{};
}

/**
 * @brief 初始化日志系统
 * @param cfg 日志配置
 * @param serverName 服务器名称
 * 
 * 功能：
 * 1. 生成日志配置
 * 2. 初始化日志系统
 */
void initLogging(const logging::LogConfig &cfg, const String &serverName) {
  auto logConfigStr = logging::generateLogConfig(cfg, serverName);
  XLOGF(INFO, "LogConfig: {}", logConfigStr);
  logging::initOrDie(logConfigStr);
}

/**
 * @brief 初始化通用组件
 * @param cfg 应用配置
 * @param serverName 服务器名称
 * @param nodeId 节点ID
 * 
 * 功能：
 * 1. 初始化日志系统
 * 2. 打印版本信息
 * 3. 初始化等待器单例
 * 4. 启动监控系统
 */
void initCommonComponents(const ApplicationBase::Config &cfg, const String &serverName, flat::NodeId nodeId) {
  initLogging(cfg.log(), serverName);

  XLOGF(INFO, "{}", VersionInfo::full());

  XLOGF(INFO, "Init waiter singleton {}", fmt::ptr(&net::Waiter::instance()));

  // 启动监控服务
  auto monitorResult =
      monitor::Monitor::start(cfg.monitor(), nodeId == 0 ? "" : fmt::format("Node_{}", nodeId.toUnderType()));
  XLOGF_IF(FATAL, !monitorResult, "Start monitor failed: {}", monitorResult.error());
}

/**
 * @brief 持久化配置到文件
 * @param cfg 配置对象
 * 
 * 功能：
 * 1. 根据时间戳生成文件名
 * 2. 将配置写入文件
 */
void persistConfig(const config::IConfig &cfg) {
  if (FLAGS_cfg_persist_prefix.empty()) {
    XLOGF(INFO, "skip config dumping: cfg_persist_prefix is empty");
    return;
  }
  constexpr auto usPerSecond = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::seconds(1)).count();
  auto now = UtcClock::now();
  auto us = now.toMicroseconds() % usPerSecond;
  auto path = fmt::format("{}_{:%Y%m%d_%H%M%S}_{:06d}", FLAGS_cfg_persist_prefix, now, us); 
  auto storeRes = storeToFile(path, cfg.toString());
  if (storeRes) {
    XLOGF(INFO, "Dump full config to {}", path);
  } else {
    XLOGF(ERR, "Dump full config to {} failed: {}", path, storeRes.error());
  }
}

/**
 * @brief 创建日志配置更新回调
 * @param cfg 日志配置
 * @param serverName 服务器名称
 * @return 配置回调守卫
 * 
 * 功能：
 * 1. 创建日志配置更新回调
 * 2. 确保日志系统随配置更新而更新
 */
std::unique_ptr<ConfigCallbackGuard> makeLogConfigUpdateCallback(logging::LogConfig &cfg, String serverName) {
  return cfg.addCallbackGuard([&cfg, serverName = std::move(serverName)] { initLogging(cfg, serverName); });
}

/**
 * @brief 创建内存配置更新回调
 * @param cfg 内存配置
 * @param hostname 主机名
 * @return 配置回调守卫
 * 
 * 功能：
 * 1. 控制内存分析功能
 * 2. 记录内存分配器状态
 */
std::unique_ptr<ConfigCallbackGuard> makeMemConfigUpdateCallback(memory::MemoryAllocatorConfig &cfg, String hostname) {
  if (malloc_stats_print == nullptr) {
    XLOGF(WARNING, "not dynamically linked jemalloc");
  } else {
    XLOGF(WARNING, "dynamically linked jemalloc");
  }
  return cfg.addCallbackGuard([&cfg, hostname = std::move(hostname)] {
    if (malloc_stats_print == nullptr) {
      return;
    }
    // turn on/off memory profiling
    std::string prefix = fmt::format("{}{}", cfg.prof_prefix(), hostname);
    je_profiling(cfg.prof_active(), prefix.c_str());
    // log memory allocator status
    char logbuf[512] = {0};
    je_logstatus(logbuf, sizeof(logbuf));
    XLOG(WARN, logbuf);
  });
}

String loadConfigFromLauncher(std::function<Result<std::pair<String, String>>()> launcher) {
  XLOGF(INFO, "Start load config from launcher");
  auto retryInterval = std::chrono::milliseconds(10);
  constexpr auto maxRetryInterval = std::chrono::milliseconds(1000);
  for (int i = 0; i < 20; ++i) {
    auto loadConfigRes = launcher();
    if (loadConfigRes) {
      const auto &[content, desc] = *loadConfigRes;
      XLOGF(INFO, "Load config from launcher finished {}", desc);
      return content;
    }

    XLOGF(CRITICAL, "Load config by launcher failed: {}\nretryCount: {}", loadConfigRes.error(), i);
    std::this_thread::sleep_for(retryInterval);
    retryInterval = std::min(2 * retryInterval, maxRetryInterval);
  }
  XLOGF(FATAL, "Load config from launcher failed, stop retrying");
  __builtin_unreachable();
}

/**
 * @brief 从启动器加载应用信息
 * @param launcher 启动器函数
 * @param baseInfo 基础应用信息
 * 
 * 功能：
 * 1. 通过启动器获取应用信息
 * 2. 支持重试机制
 * 3. 记录应用信息日志
 */
void loadAppInfo(std::function<Result<flat::AppInfo>()> launcher, flat::AppInfo &baseInfo) {
  XLOGF(INFO, "Start load AppInfo from launcher");
  auto retryInterval = std::chrono::milliseconds(10);
  constexpr auto maxRetryInterval = std::chrono::milliseconds(1000);
  for (int i = 0; i < 20; ++i) {
    auto loadRes = launcher();
    if (loadRes) {
      baseInfo = *loadRes;
      XLOGF(INFO,
            "Load AppInfo from launcher finished. AppInfo: {}\nclusterId: {}\ntags: {}",
            serde::toJsonString(static_cast<const flat::FbsAppInfo &>(baseInfo)),
            baseInfo.clusterId,
            serde::toJsonString(baseInfo.tags));
      return;
    }

    XLOGF(CRITICAL, "Load AppInfo from launcher failed: {}\nretryCount: {}", loadRes.error(), i);
    std::this_thread::sleep_for(retryInterval);
    retryInterval = std::min(2 * retryInterval, maxRetryInterval);
  }
  XLOGF(FATAL, "Load AppInfo from launcher failed, stop retrying");
  __builtin_unreachable();
}

/**
 * @brief 从文件加载配置
 * @return 配置内容
 * 
 * 功能：
 * 1. 读取配置文件
 * 2. 处理文件读取错误
 */
String loadConfigFromFile() {
  XLOGF(INFO, "Use local server config file: {}", FLAGS_cfg);
  auto res = loadFile(FLAGS_cfg);
  XLOGF_IF(FATAL, !res, "Load config from {} failed: {}", FLAGS_cfg, res.error());
  return *res;
}

/**
 * @brief 加载配置模板
 * @param cfg 配置对象
 * @param launcher 启动器函数
 * @return 配置模板内容
 * 
 * 功能：
 * 1. 支持从文件加载配置
 * 2. 支持使用默认配置
 * 3. 支持从启动器加载配置
 */
String loadConfigTemplate(config::IConfig &cfg, std::function<Result<std::pair<String, String>>()> launcher) {
  if (!FLAGS_cfg.empty()) {
    return loadConfigFromFile();
  } else if (FLAGS_use_local_cfg) {
    XLOGF(INFO, "Use default server config");
    return cfg.toString();
  } else {
    return loadConfigFromLauncher(std::move(launcher));
  }
}

/**
 * @brief 初始化配置
 * @param cfg 配置对象
 * @param updates 配置更新
 * @param appInfo 应用信息
 * @param launcher 启动器函数
 * 
 * 功能：
 * 1. 加载配置模板
 * 2. 渲染配置内容
 * 3. 更新配置
 * 4. 支持配置打印
 */
void initConfig(config::IConfig &cfg,
                const std::vector<config::KeyValue> &updates,
                const flat::AppInfo &appInfo,
                std::function<Result<std::pair<String, String>>()> launcher) {
  // std::move(launcher)将launcher参数转换为右值引用，使其所有权可以被转移
  // 这里使用std::move是因为launcher是一个函数对象，我们不再需要在这个作用域使用它
  // 通过移动语义可以避免不必要的拷贝，提高性能
  auto configTemplate = loadConfigTemplate(cfg, std::move(launcher));
  XLOGF(INFO, "Full Config Template:\n{}", configTemplate);
  auto renderRes = hf3fs::renderConfig(configTemplate, &appInfo);  //直接返回configTemplate
  XLOGF_IF(FATAL, !renderRes, "Render config failed: {}\nTemplate: {}", renderRes.error(), configTemplate);

  auto initRes = cfg.atomicallyUpdate(std::string_view(*renderRes), /*isHotUpdate=*/false);
  XLOGF_IF(FATAL, !initRes, "Init config failed: {}", initRes.error());

  auto res = cfg.update(updates, /*isHotUpdate=*/false);
  XLOGF_IF(FATAL, !res, "Update config failed: {}", res.error());

  if (FLAGS_dump_cfg) {
    fmt::print("{}\n", cfg.toString());
    exit(0);
  }
}

/**
 * @brief 从文件初始化配置
 * @param cfg 配置对象
 * @param filePath 文件路径
 * @param dump 是否打印配置
 * @param updates 配置更新
 * 
 * 功能：
 * 1. 从文件加载配置
 * 2. 渲染配置内容
 * 3. 更新配置
 * 4. 支持配置打印
 */
void initConfigFromFile(config::IConfig &cfg,
                        const String &filePath,
                        bool dump,
                        const std::vector<config::KeyValue> &updates) {
  if (dump) {
    fmt::print("{}\n", cfg.toString());
    exit(0);
    __builtin_unreachable();
  }
  auto loadRes = loadFile(filePath);
  XLOGF_IF(FATAL, !loadRes, "Load launcher config failed: {}. filePath: {}", loadRes.error(), filePath);
  auto renderRes = hf3fs::renderConfig(*loadRes, nullptr);
  XLOGF_IF(FATAL, !renderRes, "Render launcher config failed: {}. filePath: {}", renderRes.error(), filePath);
  auto initRes = cfg.atomicallyUpdate(std::string_view(*renderRes), /*isHotUpdate=*/false);
  XLOGF_IF(FATAL, !initRes, "Init launcher config failed: {}. filePath: {}", initRes.error(), filePath);
  auto updateRes = cfg.update(updates, /*isHotUpdate=*/false);
  XLOGF_IF(FATAL, !updateRes, "Update launcher config failed: {}. filePath: {}", updateRes.error(), filePath);
}
}  // namespace hf3fs::app_detail
