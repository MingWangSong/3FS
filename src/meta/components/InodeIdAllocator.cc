#include "meta/components/InodeIdAllocator.h"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <fmt/core.h>
#include <folly/Likely.h>
#include <folly/ScopeGuard.h>
#include <folly/Unit.h>
#include <folly/experimental/coro/CurrentExecutor.h>
#include <folly/experimental/coro/Promise.h>
#include <folly/experimental/coro/Sleep.h>
#include <folly/experimental/coro/Timeout.h>
#include <folly/io/async/Request.h>
#include <folly/logging/xlog.h>
#include <mutex>
#include <optional>

#include "common/kv/KeyPrefix.h"
#include "common/utils/Coroutine.h"
#include "common/utils/FaultInjection.h"
#include "common/utils/Result.h"

#define FAULT_INJECTION_INODE_ID_ALLOCATOR true

namespace hf3fs::meta::server {

// "SNGL-inode-alloc"
std::string InodeIdAllocator::kAllocatorKeyPrefix = fmt::format("{}-inode-alloc", kv::toStr(kv::KeyPrefix::Single));

CoTryTask<meta::InodeId> InodeIdAllocator::allocateSlow(std::chrono::microseconds timeout) {
  tryStartAllocateTask(co_await folly::coro::co_current_executor);
  auto result = co_await folly::coro::co_awaitTry(folly::coro::timeout(queue_.dequeue(), timeout));
  if (UNLIKELY(result.hasException())) {
    co_return makeError(MetaCode::kInodeIdAllocFailed);
  }
  if (UNLIKELY(queue_.size() < kAllocateBatch / 2)) {
    tryStartAllocateTask(co_await folly::coro::co_current_executor);
  }

  co_return result.value();
}

CoTask<void> InodeIdAllocator::allocateFromDB() {
  
  // 从FoundationDB获取高64位基础值 IdAllocator
  auto result = co_await allocator_.allocate();
  if (UNLIKELY(result.hasError())) {
    XLOGF(CRITICAL, "Failed to allocate InodeId {}", result.error().describe());
    // allocation failed, wait sometime and retry in new task
    startAllocateTask(co_await folly::coro::co_current_executor);
    co_return;
  }

  if (UNLIKELY((result.value() & ~kAllocatorMask) != 0)) {
    XLOGF(FATAL, "64bit InodeId used up, should never happen, {}!!!", result.value());
  }

  // 左移12位，取低52位
  auto first = result.value() << kAllocatorShift;
  XLOGF(DBG,
        "Get {} from IdAllocator, corresponding to InodeId {} - {}",
        result.value(),
        meta::InodeId(first),
        meta::InodeId(first + kAllocateBatch - 1));
  // 低12位依次累加，组成InodeId，递增生成4096个连续ID
  for (uint64_t i = 0; i < kAllocateBatch; i++) {
    meta::InodeId id(first + i);
    co_await queue_.enqueue(id);
  }
  allocating_.store(false);

  if (UNLIKELY(queue_.size() < kAllocateBatch / 2)) {
    tryStartAllocateTask(co_await folly::coro::co_current_executor);
  }
}

}  // namespace hf3fs::meta::server
