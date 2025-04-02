#ifdef ENABLE_FUSE_APPLICATION

#include "FuseApplication.h"

#include "FuseMainLoop.h"
#include "FuseOps.h"
#include "common/app/Thread.h"
#include "common/app/Utils.h"

DECLARE_string(cfg);
DECLARE_bool(dump_default_cfg);
DECLARE_bool(use_local_cfg);

namespace hf3fs::fuse {

struct FuseApplication::Impl {
  // 解析命令行参数
  Result<Void> parseFlags(int *argc, char ***argv);
  // 初始化应用程序
  Result<Void> initApplication();
  // 初始化FUSE客户端
  Result<Void> initFuseClients();
  // 停止应用程序
  void stop();
  // 主循环
  int mainLoop();

  Config hf3fsConfig;                                               // FUSE配置
  flat::AppInfo appInfo;                                            // 应用程序信息
  std::unique_ptr<Launcher> launcher_ = std::make_unique<Launcher>(); // 启动器，负责初始化配置

  std::unique_ptr<ConfigCallbackGuard> onLogConfigUpdated_;         // 日志配置更新回调
  std::unique_ptr<ConfigCallbackGuard> onMemConfigUpdated_;         // 内存配置更新回调

  ConfigFlags configFlags_;                                         // 配置标志
  String programName;                                               // 程序名称
  bool allowOther = false;                                          // 是否允许其他用户访问
  String configMountpoint;                                          // 挂载点路径
  size_t configMaxBufSize = 0;                                      // 最大缓冲区大小
  String configClusterId;                                           // 集群ID
};

FuseApplication::FuseApplication()
    : impl_(std::make_unique<Impl>()) {}

FuseApplication::~FuseApplication() = default;

/**
 * 解析命令行参数
 * 处理流程：
 * 1. 首先解析启动器相关的参数
 * 2. 然后解析以"--config."开头的动态配置参数
 * 3. 保存程序名称
 */
Result<Void> FuseApplication::Impl::parseFlags(int *argc, char ***argv) {
  RETURN_ON_ERROR(launcher_->parseFlags(argc, argv));

  static constexpr std::string_view dynamicConfigPrefix = "--config.";
  RETURN_ON_ERROR(ApplicationBase::parseFlags(dynamicConfigPrefix, argc, argv, configFlags_));

  programName = (*argv)[0];
  return Void{};
}

Result<Void> FuseApplication::parseFlags(int *argc, char ***argv) { return impl_->parseFlags(argc, argv); }

/**
 * 初始化应用程序
 * 处理流程：
 * 1. 处理是否需要打印默认配置
 * 2. 初始化启动器
 * 3. 加载应用信息和配置
 * 4. 初始化通用组件
 * 5. 设置日志和内存配置更新回调
 * 6. 初始化FUSE客户端
 * 7. 清理启动器资源
 */
Result<Void> FuseApplication::Impl::initApplication() {
  if (FLAGS_dump_default_cfg) {
    fmt::print("{}\n", hf3fsConfig.toString());
    exit(0);
  }

  auto firstInitRes = launcher_->init();
  XLOGF_IF(FATAL, !firstInitRes, "Failed to init launcher: {}", firstInitRes.error());

  /*Lambda 表达式：
     [this] 是捕获列表，表示这个 Lambda 表达式可以访问当前对象的成员变量和成员函数。
  
  */
  app_detail::loadAppInfo([this] { return launcher_->loadAppInfo(); }, appInfo);
  app_detail::initConfig(hf3fsConfig, configFlags_, appInfo, [this] { return launcher_->loadConfigTemplate(); });
  XLOGF(INFO, "Server config inited");

  app_detail::initCommonComponents(hf3fsConfig.common(), kName, appInfo.nodeId);

  onLogConfigUpdated_ = app_detail::makeLogConfigUpdateCallback(hf3fsConfig.common().log(), kName);
  onMemConfigUpdated_ = app_detail::makeMemConfigUpdateCallback(hf3fsConfig.common().memory(), appInfo.hostname);

  XLOGF(INFO, "Full Config:\n{}", hf3fsConfig.toString());
  app_detail::persistConfig(hf3fsConfig);

  XLOGF(INFO, "Start to init fuse clients");
  auto initRes = initFuseClients();
  XLOGF_IF(FATAL, !initRes, "Init fuse clients failed: {}", initRes.error());
  XLOGF(INFO, "Init fuse clients finished");

  launcher_.reset();

  return Void{};
}

/**
 * 初始化FUSE客户端
 * 处理流程：
 * 1. 从启动器配置中获取必要参数如挂载点、是否允许其他用户访问等
 * 2. 获取FUSE客户端单例实例
 * 3. 使用应用信息、挂载点、令牌文件和配置初始化FUSE客户端
 * 
 * 这里建立了FuseApplication与FuseClients的关联，一个FuseApplication对应一个FuseClients，
 * 而一个FuseClients对应一个挂载点(mountpoint)
 */
Result<Void> FuseApplication::Impl::initFuseClients() {
  const auto &launcherConfig = launcher_->launcherConfig();
  allowOther = launcherConfig.allow_other();
  configMountpoint = launcherConfig.mountpoint();
  configMaxBufSize = hf3fsConfig.io_bufs().max_buf_size();
  configClusterId = launcherConfig.cluster_id();

  auto &d = getFuseClientsInstance();
  RETURN_ON_ERROR(d.init(appInfo, launcherConfig.mountpoint(), launcherConfig.token_file(), hf3fsConfig));
  return Void{};
}

Result<Void> FuseApplication::initApplication() { return impl_->initApplication(); }

/**
 * 停止应用程序
 * 处理流程：
 * 1. 停止FUSE客户端
 * 2. 停止并等待其他组件完成
 */
void FuseApplication::Impl::stop() {
  getFuseClientsInstance().stop();
  hf3fs::stopAndJoin(nullptr);
}

void FuseApplication::stop() { impl_->stop(); }

config::IConfig *FuseApplication::getConfig() { return &impl_->hf3fsConfig; }

const flat::AppInfo *FuseApplication::info() const { return &impl_->appInfo; }

/**
 * 判断配置是否可以动态更新
 * 当命令行参数中没有指定配置文件路径且不使用本地配置时，配置可以动态更新
 */
bool FuseApplication::configPushable() const { return FLAGS_cfg.empty() && !FLAGS_use_local_cfg; }

void FuseApplication::onConfigUpdated() { app_detail::persistConfig(impl_->hf3fsConfig); }

/**
 * 主循环函数
 * 处理流程：
 * 1. 解除对中断信号的阻塞
 * 2. 调用fuseMainLoop函数进入FUSE主循环
 * 
 * fuseMainLoop负责：
 * - 设置FUSE会话和操作处理函数
 * - 挂载文件系统
 * - 启动事件循环处理文件系统请求
 */
int FuseApplication::Impl::mainLoop() {
  Thread::unblockInterruptSignals();

  return fuseMainLoop(programName, allowOther, configMountpoint, configMaxBufSize, configClusterId);
}

int FuseApplication::mainLoop() { return impl_->mainLoop(); }

}  // namespace hf3fs::fuse

#endif
