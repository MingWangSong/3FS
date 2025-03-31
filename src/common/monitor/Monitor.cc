#ifdef OVERRIDE_CXX_NEW_DELETE
#undef OVERRIDE_CXX_NEW_DELETE
#endif

#include "common/monitor/Monitor.h"

#include <chrono>
#include <folly/Synchronized.h>
#include <folly/logging/xlog.h>
#include <folly/system/ThreadName.h>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "common/monitor/Recorder.h"
#include "common/monitor/Reporter.h"
#include "common/utils/SysResource.h"
#include "memory/common/OverrideCppNewDelete.h"

namespace hf3fs::monitor {
namespace {

/**
 * @class DummyVariable
 * @brief 虚拟指标记录器，用于保持监控系统活跃
 * 
 * 这个类创建一个不收集任何数据的记录器，主要目的是确保
 * 在报告器线程停止之前，变量保持活跃状态
 */
class DummyVariable : public Recorder {
 public:
  DummyVariable(const std::string &name)
      : Recorder(name, std::nullopt, Monitor::getDefaultInstance()) {
    registerRecorder();
  }
  ~DummyVariable() override { unregisterRecorder(); }
  void collect(std::vector<Sample> &) override {}
} dummyVariable("dummy");  // 保持变量活跃直到报告器线程停止

/**
 * @brief 记录器键类型，由名称和标签集组成
 */
using RecorderKey = std::pair<std::string, TagSet>;

/**
 * @brief 记录器键的哈希函数
 */
struct RecorderKeyHash {
  size_t operator()(const RecorderKey &key) const noexcept { return folly::hash::hash_combine(key.first, key.second); }
};

/**
 * @brief 记录器映射类型，用于存储记录器对象
 */
using RecorderMap = robin_hood::unordered_map<RecorderKey, Recorder *, RecorderKeyHash>;

/**
 * @brief 线程安全的记录器映射类型
 */
using LockableRecorderMap = folly::Synchronized<RecorderMap, std::mutex>;
}  // namespace

/**
 * @brief Collector的实现细节
 * 
 * 包含调整大小的互斥锁和记录器映射数组
 */
struct Collector::Impl {
  std::shared_mutex resizeMutex_;  // 用于保护maps_调整大小操作的互斥锁
  std::vector<std::unique_ptr<LockableRecorderMap>> maps_;  // 记录器映射数组，每个桶对应一个映射
};

/**
 * @brief 构造函数，初始化收集器
 * 
 * 创建一个实现对象并初始化一个记录器映射
 */
Collector::Collector() {
  impl = std::make_unique<Impl>();
  impl->maps_.push_back(std::make_unique<LockableRecorderMap>());
}

/**
 * @brief 析构函数，释放资源
 */
Collector::~Collector() = default;

/**
 * @brief 添加一个指标记录器
 * @param name 指标名称
 * @param var 指标记录器引用
 * 
 * 将指标记录器添加到收集器的注册表中，以便后续收集数据
 * 如果已存在同名且同标签的记录器，则会触发致命错误
 */
void Collector::add(const std::string &name, Recorder &var) {
  auto key = std::make_pair(name, var.tag_);  // 创建记录器键
  auto hash = RecorderKeyHash()(key);  // 计算键的哈希值
  auto lock = std::shared_lock(impl->resizeMutex_);  // 获取共享锁，允许并发读取
  auto map = impl->maps_[hash % impl->maps_.size()]->lock();  // 获取对应桶的映射并锁定
  auto [it, succeed] = map->try_emplace(std::move(key), &var);  // 尝试插入记录器
  XLOGF_IF(FATAL,
           !succeed,
           "Monitor two variables with same name and same tag: {}. tags: {}",
           name,
           serde::toJsonString(var.tag_.asMap()));  // 如果插入失败，记录错误
  return;
}

/**
 * @brief 删除一个指标记录器
 * @param name 指标名称
 * @param var 指标记录器引用
 * 
 * 从收集器的注册表中删除指标记录器
 * 如果找不到对应的记录器，则会记录错误
 */
void Collector::del(const std::string &name, Recorder &var) {
  auto key = std::make_pair(name, var.tag_);  // 创建记录器键
  auto hash = RecorderKeyHash()(key);  // 计算键的哈希值
  auto lock = std::shared_lock(impl->resizeMutex_);  // 获取共享锁，允许并发读取
  auto map = impl->maps_[hash % impl->maps_.size()]->lock();  // 获取对应桶的映射并锁定
  auto it = map->find(key);  // 查找记录器
  if (it != map->end()) {
    map->erase(it);  // 如果找到，则删除
  } else {
    XLOGF(ERR, "Monitor has no variable with name {} and tags {}", name, serde::toJsonString(var.tag_.asMap()));  // 如果找不到，记录错误
  }
}

/**
 * @brief 收集指定桶中的所有指标数据
 * @param bucketIndex 桶索引
 * @param samples 用于存储收集到的样本的向量
 * @param cleanInactive 是否清理不活跃的指标
 * 
 * 遍历指定桶中的所有指标记录器，调用它们的collect方法收集数据
 * 如果桶索引超出范围，则会触发致命错误
 */
void Collector::collectAll(size_t bucketIndex, std::vector<Sample> &samples, bool cleanInactive) {
  XLOGF_IF(FATAL,
           impl->maps_.size() <= bucketIndex,
           "Monitor bucketIndex out of range. buckets: {}. bucketIndex: {}",
           impl->maps_.size(),
           bucketIndex);  // 检查桶索引是否有效
  auto map = impl->maps_[bucketIndex]->lock();  // 获取对应桶的映射并锁定
  for (auto &pair : *map) {
    pair.second->collectAndClean(samples, cleanInactive);  // 收集每个记录器的数据
  }
}

/**
 * @brief 调整收集器的桶数量
 * @param n 新的桶数量
 * 
 * 重新分配桶，并将现有的指标记录器重新分配到新的桶中
 * 如果桶数量无效，则会触发致命错误
 */
void Collector::resize(size_t n) {
  XLOGF_IF(FATAL, n == 0 || n > 1024, "Monitor invalid bucket count: {}", n);  // 检查桶数量是否有效

  auto lock = std::unique_lock(impl->resizeMutex_);  // 获取独占锁，防止并发读写
  if (impl->maps_.size() == n) {
    return;  // 如果桶数量没有变化，则直接返回
  }
  std::vector<std::unique_ptr<LockableRecorderMap>> newMaps(n);  // 创建新的映射数组
  for (size_t i = 0; i < n; ++i) {
    newMaps[i] = std::make_unique<LockableRecorderMap>();  // 初始化每个映射
  }

  for (auto &m : impl->maps_) {
    auto oldmap = m->lock();  // 锁定旧映射
    for (auto &[key, v] : *oldmap) {
      auto hash = RecorderKeyHash()(key);  // 计算键的哈希值
      auto newmap = newMaps[hash % newMaps.size()]->lock();  // 获取对应新桶的映射并锁定
      newmap->try_emplace(key, v);  // 将记录器插入新映射
    }
  }

  impl->maps_.swap(newMaps);  // 交换新旧映射数组
}

/**
 * @brief 定期收集指标数据
 * @param context 收集器上下文
 * @param config 监控系统配置
 * 
 * 收集器线程的主函数，定期从Recorder收集指标数据，放入队列
 * 每隔一段时间会清理不活跃的指标
 */
void MonitorInstance::periodicallyCollect(CollectorContext &context, const Monitor::Config &config) {
  static constexpr auto kCleanPeriod = std::chrono::seconds(300);  // 清理周期，5分钟

  auto cleanTime = std::chrono::steady_clock::now();  // 上次清理时间
  auto currentTime = std::chrono::steady_clock::now();  // 当前时间
  while (!stop_) {  // 循环直到停止标志被设置
    auto samples = SampleBatchPool::get();  // 从对象池获取样本批次

    if (currentTime - cleanTime > kCleanPeriod) {  // 如果超过清理周期
      getCollector().collectAll(context.bucketIndex, *samples, true);  // 收集数据并清理不活跃的指标
      cleanTime = std::chrono::steady_clock::now();  // 更新清理时间
    } else {
      getCollector().collectAll(context.bucketIndex, *samples, false);  // 只收集数据，不清理
    }

    if (LIKELY(!samples->empty())) {  // 如果收集到了数据
      context.samplesQueue_.write(std::move(samples));  // 将样本批次写入队列
      context.reporterCond_.notify_one();  // 通知报告器线程
    }

    auto collect_period = config.collect_period();  // 获取收集周期
    auto nextRunTime = currentTime + collect_period;  // 计算下次运行时间
    currentTime = std::chrono::steady_clock::now();  // 更新当前时间
    if (currentTime <= nextRunTime) {  // 如果当前时间小于下次运行时间
      auto lock = std::unique_lock(context.mutex_);  // 获取互斥锁
      context.collectorCond_.wait_for(lock, nextRunTime - currentTime);  // 等待直到下次运行时间
      currentTime = nextRunTime;  // 更新当前时间
    } else {
      XLOGF(ERR,
            "A report task takes more than {} second: {}ms",
            collect_period,
            std::chrono::duration_cast<std::chrono::milliseconds>(currentTime - nextRunTime).count());  // 记录错误
    }
  }
}

/**
 * @brief 报告样本数据
 * @param context 收集器上下文
 * @param reporters 报告器列表
 * 
 * 报告器线程的主函数，从队列中获取样本数据，发送到后端存储系统
 */
void MonitorInstance::reportSamples(CollectorContext &context, std::vector<std::unique_ptr<Reporter>> reporters) {
  while (!stop_) {  // 循环直到停止标志被设置
    SampleBatchPool::Ptr samples;  // 样本批次指针
    if (!context.samplesQueue_.read(samples)) {  // 尝试从队列读取样本批次
      auto lock = std::unique_lock(context.mutex_);  // 获取互斥锁
      context.reporterCond_.wait(lock);  // 等待通知
      continue;  // 继续循环
    }
    for (auto &reporter : reporters) {
      reporter->commit(*samples);  // 将样本数据提交给每个报告器
    }
  }
}

/**
 * @brief 启动监控系统
 * @param config 监控系统配置
 * @param hostnameExtra 主机名附加信息（可选）
 * @return 成功返回Void对象，失败返回错误信息
 * 
 * 使用指定的配置启动监控系统，设置主机名和创建报告器
 */
Result<Void> Monitor::start(const Config &config, String hostnameExtra) {
  return getDefaultInstance().start(config, std::move(hostnameExtra));  // 调用默认实例的start方法
}

/**
 * @brief 停止监控系统
 * 
 * 停止所有收集器和报告器线程，释放资源
 */
void Monitor::stop() { getDefaultInstance().stop(); }  // 调用默认实例的stop方法

/**
 * @brief 创建一个新的监控实例
 * @return 新创建的监控实例指针
 * 
 * 工厂方法，用于创建MonitorInstance对象
 */
std::unique_ptr<MonitorInstance> Monitor::createMonitorInstance() { return std::make_unique<MonitorInstance>(); }

/**
 * @brief 获取默认的监控实例
 * @return 默认监控实例的引用
 * 
 * 单例模式，返回全局唯一的MonitorInstance对象
 */
MonitorInstance &Monitor::getDefaultInstance() {
  static MonitorInstance defaultInstance;  // 静态局部变量，确保只创建一次
  return defaultInstance;
}

/**
 * @brief 启动监控系统
 * @param config 监控系统配置
 * @param hostnameExtra 主机名附加信息（可选）
 * @return 成功返回Void对象，失败返回错误信息
 * 
 * 初始化监控系统，设置主机名，创建报告器，启动收集器和报告器线程
 */
Result<Void> MonitorInstance::start(const Monitor::Config &config, String hostnameExtra) {
  if (!stop_) return Void{};  // 如果已经启动，则直接返回
  stop_ = false;  // 设置停止标志为false

  getCollector().resize(config.num_collectors());  // 调整收集器的桶数量
  for (int i = 0; i < config.num_collectors(); ++i) {
    collectorContexts_.push_back(std::make_unique<CollectorContext>());  // 创建收集器上下文
    collectorContexts_.back()->bucketIndex = i;  // 设置桶索引
  }

  // 1. 设置主机名
  auto hostnameResult = SysResource::hostname(true);  // 获取主机名
  RETURN_ON_ERROR(hostnameResult);  // 如果获取失败，则返回错误
  auto podnameResult = SysResource::hostname(false);  // 获取Pod名
  RETURN_ON_ERROR(podnameResult);  // 如果获取失败，则返回错误
  if (hostnameExtra.empty()) {
    Recorder::setHostname(hostnameResult.value(), podnameResult.value());  // 设置主机名和Pod名
  } else {
    auto append = [&](const String &hostname) { return fmt::format("{}({})", hostname, hostnameExtra); };  // 附加额外信息
    Recorder::setHostname(append(hostnameResult.value()), append(podnameResult.value()));  // 设置带有额外信息的主机名和Pod名
  }

  for (int i = 0; i < config.num_collectors(); ++i) {
    // 2. 创建报告器
    std::vector<std::unique_ptr<Reporter>> reporters;  // 报告器列表
    for (auto i = 0ul; i < config.reporters_length(); ++i) {
      auto &reporterConfig = config.reporters(i);  // 获取报告器配置
      if (reporterConfig.type() == "clickhouse") {
        reporters.push_back(std::make_unique<ClickHouseClient>(reporterConfig.clickhouse()));  // 创建ClickHouse报告器
      } else if (reporterConfig.type() == "log") {
        reporters.push_back(std::make_unique<LogReporter>(reporterConfig.log()));  // 创建日志报告器
      } else if (reporterConfig.type() == "monitor_collector") {
        reporters.push_back(std::make_unique<MonitorCollectorClient>(reporterConfig.monitor_collector()));  // 创建监控收集器报告器
      }
      RETURN_ON_ERROR(reporters.back()->init());  // 初始化报告器，如果失败则返回错误
    }

    // 3. 启动收集和报告线程
    collectorContexts_[i]->collectThread_ =
        std::jthread(&MonitorInstance::periodicallyCollect, this, std::ref(*collectorContexts_[i]), std::ref(config));  // 创建收集器线程
    folly::setThreadName(collectorContexts_[i]->collectThread_.get_id(), "Collector");  // 设置线程名
    collectorContexts_[i]->reportThread_ =
        std::jthread(&MonitorInstance::reportSamples, this, std::ref(*collectorContexts_[i]), std::move(reporters));  // 创建报告器线程
    folly::setThreadName(collectorContexts_[i]->reportThread_.get_id(), "Reporter");  // 设置线程名
  }
  return Void{};  // 返回成功
}

/**
 * @brief 停止监控系统
 * 
 * 停止所有收集器和报告器线程，释放资源
 */
void MonitorInstance::stop() {
  if (!stop_) {  // 如果未停止
    stop_ = true;  // 设置停止标志为true
    for (auto &context : collectorContexts_) {
      context->reporterCond_.notify_one();  // 通知报告器线程
      context->collectorCond_.notify_one();  // 通知收集器线程
      context->collectThread_ = std::jthread{};  // 重置收集器线程
      context->reportThread_ = std::jthread{};  // 重置报告器线程
    }
  }
}

/**
 * @brief 析构函数，停止监控系统并释放资源
 */
MonitorInstance::~MonitorInstance() { stop(); }  // 调用stop方法

/**
 * @brief 注册记录器
 * 
 * 将记录器添加到收集器的注册表中
 */
void Recorder::registerRecorder() {
  if (!register_) {  // 如果未注册
    monitor_.getCollector().add(this->name(), *this);  // 添加到收集器
    register_ = true;  // 设置注册标志为true
  }
}

/**
 * @brief 注销记录器
 * 
 * 从收集器的注册表中删除记录器
 */
void Recorder::unregisterRecorder() {
  if (register_) {  // 如果已注册
    monitor_.getCollector().del(this->name(), *this);  // 从收集器中删除
    register_ = false;  // 设置注册标志为false
  }
}

}  // namespace hf3fs::monitor
