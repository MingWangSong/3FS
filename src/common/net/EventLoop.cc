#include "common/net/EventLoop.h"

#include <folly/Random.h>
#include <folly/logging/xlog.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>

#include "common/net/Network.h"
#include "common/utils/Size.h"

namespace hf3fs::net {

Result<Void> EventLoop::start(const std::string &threadName) {
  // 1. init epoll fd.
  epfd_ = ::epoll_create(16_KB);
  if (UNLIKELY(!epfd_.valid())) {
    XLOGF(ERR, "create epoll failed");
    return makeError(RPCCode::kEpollInitError, "create epoll failed");
  }

  // 2. init event fd for notify.
  eventfd_ = ::eventfd(0, EFD_NONBLOCK);
  if (UNLIKELY(!eventfd_.valid())) {
    XLOGF(ERR, "create eventfd failed");
    return makeError(RPCCode::kEpollInitError, "create eventfd failed");
  }

  // 3. add event fd into epoll. 边缘触发模式
  struct epoll_event evt = {EPOLLIN | EPOLLET, {nullptr}};
  int ret = ::epoll_ctl(epfd_, EPOLL_CTL_ADD, eventfd_, &evt);
  if (UNLIKELY(ret == -1)) {
    auto msg = fmt::format("add eventfd into epoll failed, epoll {}, eventfd {}, errno {}", epfd_, eventfd_, errno);
    XLOG(ERR, msg);
    return makeError(RPCCode::kEpollAddError, std::move(msg));
  }

  // 4. start loop in background thread. 
  // 创建一个新的std::jthread线程来运行EventLoop::loop函数
  thread_ = std::jthread(&EventLoop::loop, this);
  // folly::setThreadName用于设置线程名称,方便调试和监控
  // 通过thread_.get_id()获取线程ID,threadName作为线程名称
  folly::setThreadName(thread_.get_id(), threadName);
  return Void{};
}

Result<Void> EventLoop::wakeUp() {
  uint64_t val = 1;
  int ret = ::write(eventfd_, &val, sizeof(val));
  if (ret == -1) {
    auto msg = fmt::format("wake up epoll loop failed, eventfd {}, errno {}", eventfd_, errno);
    XLOG(ERR, msg);
    return makeError(RPCCode::kEpollWakeUpError, std::move(msg));
  }
  return Void{};
}

void EventLoop::stopAndJoin() {
  if (thread_.joinable()) {
    stop_ = true;
    wakeUp();
    thread_.join();
  }
}

Result<Void> EventLoop::add(const std::shared_ptr<EventHandler> &handler, uint32_t interestEvents) {
  HandlerWrapper *wrapper = nullptr;
  {
    auto lock = std::unique_lock(mutex_);
    wrapperList_.emplace_front(HandlerWrapper{handler});
    handler->it_ = wrapperList_.begin();
    handler->eventLoop_ = weak_from_this();
    wrapper = &wrapperList_.front();
  }

  struct epoll_event event;
  event.events = interestEvents;
  event.data.ptr = wrapper;
  int ret = ::epoll_ctl(epfd_, EPOLL_CTL_ADD, handler->fd(), &event);
  if (ret == 0) {
    return Void{};
  }

  // remove from list if fail to add.
  {
    auto lock = std::unique_lock(mutex_);
    wrapperList_.erase(handler->it_);
  }
  handler->it_ = std::list<HandlerWrapper>::iterator{};
  handler->eventLoop_.reset();
  auto msg = fmt::format("add fd into epoll failed, epoll {}, fd {}, errno {}", epfd_, handler->fd(), errno);
  XLOG(ERR, msg);
  return makeError(RPCCode::kEpollAddError, std::move(msg));
}

Result<Void> EventLoop::remove(EventHandler *handler) {
  if (handler->it_ == std::list<HandlerWrapper>::iterator{}) {
    XLOGF(DBG, "try to remove a invalid event handler, epoll {}, fd {}", epfd_, handler->fd());
    return Void{};
  }

  int ret = ::epoll_ctl(epfd_, EPOLL_CTL_DEL, handler->fd(), nullptr);
  if (ret == -1) {
    auto msg = fmt::format("remove fd from epoll failed, epoll {}, fd {}, errno {}", epfd_, handler->fd(), errno);
    XLOG(ERR, msg);
    return makeError(RPCCode::kEpollDelError, std::move(msg));
  }

  deleteQueue_.enqueue(handler->it_);
  handler->it_ = std::list<HandlerWrapper>::iterator{};

  // wake up event loop if size of delete queue is greater than threshold.
  if (deleteQueue_.size() >= kDeleteQueueWakeUpLoopThreshold) {
    wakeUp();
  }
  return Void{};
}

void EventLoop::loop() {
  XLOGF(INFO, "EventLoop::loop() started.");

  while (true) {
    // 1. wait events.
    constexpr int kMaxEvents = 64;
    struct epoll_event events[kMaxEvents];
    int n = ::epoll_wait(epfd_, events, kMaxEvents, -1);
    if (n == -1) {
      XLOGF(ERR, "epoll_wait failed, errno {}, retry", errno);
      continue;
    }
    if (stop_) {
      break;
    }

    // 2. handle events.
    for (int i = 0; i < n; ++i) {
      auto &evt = events[i];
      // 当 evt.data.ptr 为空时，表示这是一个来自 eventfd 的唤醒事件
      // eventfd 使用边缘触发(EPOLLET)模式,需要完全读取所有数据,否则事件会重复触发
      if (evt.data.ptr == nullptr) {
        // waked up by event fd. read all.
        uint64_t val;
        while (::read(eventfd_, &val, sizeof(val)) > 0) {
        }
        continue;
      }

      // 将事件数据中的指针转换为HandlerWrapper对象指针
      // evt.data.ptr存储了之前add()时设置的处理器包装对象
      auto wrapper = reinterpret_cast<HandlerWrapper *>(evt.data.ptr);

      // 尝试获取处理器的强引用
      // 由于handler存储为weak_ptr,需要先lock()转换为shared_ptr
      // 如果原始处理器已被销毁,lock()会返回空指针
      if (auto handler = wrapper->handler.lock()) {
        // 调用处理器的事件处理函数
        // 将epoll返回的事件标志传入,让处理器知道发生了什么类型的事件
        handler->handleEvents(evt.events);
      }
    }

    // 3. handle remove.
    if (!deleteQueue_.empty()) {
      auto lock = std::unique_lock(mutex_);
      std::list<HandlerWrapper>::iterator it;
      // limit the number of deletions in a single iteration.
      for (auto i = 0ul; i < kDeleteQueueWakeUpLoopThreshold && deleteQueue_.try_dequeue(it); ++i) {
        wrapperList_.erase(it);
      }
    }
  }

  XLOGF(INFO, "EventLoop::loop() stopped.");
}

EventLoopPool::EventLoopPool(size_t numThreads)
    : eventLoops_(numThreads, nullptr) {
  for (auto &eventLoop : eventLoops_) {
    eventLoop = EventLoop::create();
  }
}

Result<Void> EventLoopPool::start(const std::string &threadName) {
  int idx = 0;
  for (auto &eventLoop : eventLoops_) {
    RETURN_AND_LOG_ON_ERROR(eventLoop->start(threadName + std::to_string(idx++)));
  }
  return Void{};
}

void EventLoopPool::stopAndJoin() {
  for (auto &eventLoop : eventLoops_) {
    eventLoop->stopAndJoin();
  }
}

Result<Void> EventLoopPool::add(const std::shared_ptr<EventLoop::EventHandler> &handler, uint32_t interestEvents) {
  auto &eventLoop = eventLoops_[folly::Random::rand32() % eventLoops_.size()];
  return eventLoop->add(handler, interestEvents);
}

}  // namespace hf3fs::net
