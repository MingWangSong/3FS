#include "FuseConfigFetcher.h"

#include <folly/experimental/coro/BlockingWait.h>

#include "common/utils/SysResource.h"

namespace hf3fs::fuse {
// 完成应用信息的函数
Result<Void> FuseConfigFetcher::completeAppInfo(flat::AppInfo &appInfo [[maybe_unused]]) {
  // 获取主机名
  auto hostnameRes = SysResource::hostname(/*physicalMachineName=*/true);
  // 检查主机名获取结果是否出错
  RETURN_ON_ERROR(hostnameRes);
  // 确保客户端已初始化
  RETURN_ON_ERROR(ensureClientInited());
  // 使用协程阻塞等待获取标签
  return folly::coro::blockingWait([&]() -> CoTryTask<void> {
    // 获取通用标签
    auto tagsRes = co_await mgmtdClient_->getUniversalTags(*hostnameRes);
    // 检查标签获取结果是否出错
    CO_RETURN_ON_ERROR(tagsRes);
    // 移动标签到应用信息中
    appInfo.tags = std::move(*tagsRes);
    co_return Void{};
  }());
}
}  // namespace hf3fs::fuse
