#pragma once

#include <atomic>
#include <condition_variable>
#include <folly/ProducerConsumerQueue.h>
#include <mutex>
#include <shared_mutex>

#include "common/monitor/ClickHouseClient.h"
#include "common/monitor/LogReporter.h"
#include "common/monitor/MonitorCollectorClient.h"
#include "common/monitor/Recorder.h"
#include "common/monitor/Sample.h"
#include "common/utils/Address.h"
#include "common/utils/ConfigBase.h"
#include "common/utils/Coroutine.h"
#include "common/utils/ObjectPool.h"

namespace hf3fs::monitor {

class MonitorInstance;

/**
 * @class Collector
 * @brief 指标收集器，负责管理和收集所有注册的指标
 * 
 * Collector维护了一个指标注册表，用于存储所有注册的Recorder对象。
 * 它提供了添加、删除和收集指标的方法，支持多线程并发操作。
 */
class Collector {
 public:
  /**
   * @brief 构造函数，初始化收集器
   */
  Collector();
  
  /**
   * @brief 析构函数，释放资源
   */
  ~Collector();

  /**
   * @brief 添加一个指标记录器
   * @param name 指标名称
   * @param var 指标记录器引用
   * 
   * 将指标记录器添加到收集器的注册表中，以便后续收集数据
   */
  void add(const std::string &name, Recorder &var);
  
  /**
   * @brief 删除一个指标记录器
   * @param name 指标名称
   * @param var 指标记录器引用
   * 
   * 从收集器的注册表中删除指标记录器
   */
  void del(const std::string &name, Recorder &var);
  
  /**
   * @brief 收集指定桶中的所有指标数据
   * @param bucketIndex 桶索引
   * @param samples 用于存储收集到的样本的向量
   * @param cleanInactive 是否清理不活跃的指标
   * 
   * 遍历指定桶中的所有指标记录器，调用它们的collect方法收集数据
   */
  void collectAll(size_t bucketIndex, std::vector<Sample> &samples, bool cleanInactive);

 private:
  /**
   * @brief 调整收集器的桶数量
   * @param n 新的桶数量
   * 
   * 重新分配桶，并将现有的指标记录器重新分配到新的桶中
   */
  void resize(size_t n);

  /**
   * @brief 实现细节的封装结构
   */
  struct Impl;
  std::unique_ptr<Impl> impl;

  friend class MonitorInstance;
};

/**
 * @class Monitor
 * @brief 监控系统的主入口，提供静态方法来启动和停止监控系统
 * 
 * Monitor是一个单例类，提供了启动和停止监控系统的静态方法。
 * 它内部维护了一个MonitorInstance对象，用于实际的监控功能。
 */
class Monitor {
 public:
  /**
   * @class ReporterConfig
   * @brief 报告器配置类，支持多种类型的报告器
   * 
   * 使用变体类型来支持不同类型的报告器配置，包括ClickHouse、日志和监控收集器
   */
  class ReporterConfig : public ConfigBase<ReporterConfig> {
    CONFIG_VARIANT_TYPE("clickhouse");
    CONFIG_OBJ(clickhouse, ClickHouseClient::Config);
    CONFIG_OBJ(log, LogReporter::Config);
    CONFIG_OBJ(monitor_collector, MonitorCollectorClient::Config);
  };

  /**
   * @class Config
   * @brief 监控系统配置类
   * 
   * 包含报告器配置数组、收集器线程数量和收集周期等配置项
   */
  class Config : public ConfigBase<Config> {
    CONFIG_OBJ_ARRAY(reporters, ReporterConfig, 4, [](auto &) { return 0; });
    CONFIG_ITEM(num_collectors, 1);
    CONFIG_HOT_UPDATED_ITEM(collect_period, 1_s);
  };

  /**
   * @brief 启动监控系统
   * @param config 监控系统配置
   * @param hostnameExtra 主机名附加信息（可选）
   * @return 成功返回Void对象，失败返回错误信息
   * 
   * 使用指定的配置启动监控系统，设置主机名和创建报告器
   */
  static Result<Void> start(const Config &config, String hostnameExtra = {});
  
  /**
   * @brief 停止监控系统
   * 
   * 停止所有收集器和报告器线程，释放资源
   */
  static void stop();

  /**
   * @brief 创建一个新的监控实例
   * @return 新创建的监控实例指针
   * 
   * 工厂方法，用于创建MonitorInstance对象
   */
  static std::unique_ptr<MonitorInstance> createMonitorInstance();
  
  /**
   * @brief 获取默认的监控实例
   * @return 默认监控实例的引用
   * 
   * 单例模式，返回全局唯一的MonitorInstance对象
   */
  static MonitorInstance &getDefaultInstance();
};

/**
 * @class MonitorInstance
 * @brief 监控系统的实际实现类
 * 
 * MonitorInstance负责管理收集器和报告器线程，协调指标的收集和报告过程
 */
class MonitorInstance {
 public:
  /**
   * @brief 析构函数，停止监控系统并释放资源
   */
  ~MonitorInstance();
  
  /**
   * @brief 启动监控系统
   * @param config 监控系统配置
   * @param hostnameExtra 主机名附加信息（可选）
   * @return 成功返回Void对象，失败返回错误信息
   * 
   * 初始化监控系统，设置主机名，创建报告器，启动收集器和报告器线程
   */
  Result<Void> start(const Monitor::Config &config, String hostnameExtra = {});
  
  /**
   * @brief 停止监控系统
   * 
   * 停止所有收集器和报告器线程，释放资源
   */
  void stop();

  /**
   * @brief 获取收集器线程数量
   * @return 收集器线程数量
   */
  size_t getCollectorCount() const { return collectorContexts_.size(); }

  /**
   * @brief 获取收集器实例
   * @return 收集器实例的引用
   * 
   * 单例模式，返回全局唯一的Collector对象
   */
  Collector &getCollector() {
    static Collector collector;
    return collector;
  }

 private:
  /**
   * @brief 样本批次的最大数量
   * 
   * 限制队列中样本批次的最大数量，防止内存溢出
   */
  static constexpr size_t kMaxNumSampleBatches = 60;
  
  /**
   * @brief 样本批次类型
   */
  using SampleBatch = std::vector<Sample>;
  
  /**
   * @brief 样本批次对象池类型
   * 
   * 使用对象池管理样本批次，减少内存分配和垃圾回收的开销
   */
  using SampleBatchPool = ObjectPool<SampleBatch, kMaxNumSampleBatches, kMaxNumSampleBatches>;

  /**
   * @struct CollectorContext
   * @brief 收集器上下文结构，包含收集器线程的相关信息
   * 
   * 每个收集器线程都有一个对应的CollectorContext，包含队列、互斥锁、条件变量和线程对象
   */
  struct CollectorContext {
    /**
     * @brief 样本批次队列
     * 
     * 生产者-消费者队列，用于在收集器线程和报告器线程之间传递样本批次
     */
    folly::ProducerConsumerQueue<SampleBatchPool::Ptr> samplesQueue_ =
        folly::ProducerConsumerQueue<SampleBatchPool::Ptr>(kMaxNumSampleBatches);

    /**
     * @brief 桶索引
     * 
     * 指定该收集器线程负责的桶索引
     */
    size_t bucketIndex = 0;
    
    /**
     * @brief 互斥锁
     * 
     * 用于保护条件变量
     */
    std::mutex mutex_;
    
    /**
     * @brief 收集器条件变量
     * 
     * 用于通知收集器线程开始收集
     */
    std::condition_variable collectorCond_;
    
    /**
     * @brief 报告器条件变量
     * 
     * 用于通知报告器线程开始报告
     */
    std::condition_variable reporterCond_;
    
    /**
     * @brief 收集器线程
     * 
     * 负责定期收集指标数据
     */
    std::jthread collectThread_;
    
    /**
     * @brief 报告器线程
     * 
     * 负责将收集到的指标数据发送到后端存储系统
     */
    std::jthread reportThread_;
  };

  /**
   * @brief 定期收集指标数据
   * @param context 收集器上下文
   * @param config 监控系统配置
   * 
   * 收集器线程的主函数，定期从Recorder收集指标数据，放入队列
   */
  void periodicallyCollect(CollectorContext &context, const Monitor::Config &config);
  
  /**
   * @brief 报告样本数据
   * @param context 收集器上下文
   * @param reporters 报告器列表
   * 
   * 报告器线程的主函数，从队列中获取样本数据，发送到后端存储系统
   */
  void reportSamples(CollectorContext &context, std::vector<std::unique_ptr<Reporter>> reporters);

  /**
   * @brief 停止标志
   * 
   * 原子变量，用于指示监控系统是否应该停止
   */
  std::atomic<bool> stop_ = true;

  /**
   * @brief 收集器上下文列表
   * 
   * 存储所有收集器线程的上下文信息
   */
  std::vector<std::unique_ptr<CollectorContext>> collectorContexts_;
};

}  // namespace hf3fs::monitor
