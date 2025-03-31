#pragma once

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <fmt/core.h>
#include <fmt/format.h>
#include <folly/IPAddressV4.h>
#include <folly/Likely.h>
#include <folly/Synchronized.h>
#include <folly/Utility.h>
#include <folly/detail/IPAddressSource.h>
#include <folly/logging/xlog.h>
#include <gtest/gtest_prod.h>
#include <infiniband/verbs.h>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <queue>
#include <set>
#include <shared_mutex>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "common/net/EventLoop.h"
#include "common/net/IfAddrs.h"
#include "common/utils/Address.h"
#include "common/utils/ConfigBase.h"
#include "common/utils/Duration.h"
#include "common/utils/MagicEnum.hpp"
#include "common/utils/Result.h"
#include "common/utils/StrongType.h"

namespace hf3fs::net {

class TestIBNotInitialized;
class TestIBDevice;

/**
 * @class IBConfig
 * @brief IB设备配置类
 * 
 * 定义了IB设备的各种配置选项，包括设备过滤、网络区域配置、子网配置等。
 * 继承自ConfigBase模板类，提供配置项的序列化和反序列化功能。
 */
class IBConfig : public ConfigBase<IBConfig> {
 public:
  /** 未知网络区域的标识符 */
  static constexpr std::string_view kUnknownZone = "UNKNOWN";
  
  /**
   * @class Network
   * @brief 网络配置类
   * 
   * 表示一个IPv4网络，包含IP地址和子网掩码。
   * 继承自folly::CIDRNetworkV4，提供了额外的便捷方法。
   */
  class Network : public folly::CIDRNetworkV4 {
   public:
    /**
     * @brief 从字符串创建Network对象
     * @param str 格式为"ip/mask"的字符串
     * @return 成功时返回Network对象，失败时返回错误
     */
    static Result<Network> from(std::string_view str);
    
    /**
     * @brief 构造函数
     * @param network CIDR网络对象
     */
    Network(folly::CIDRNetworkV4 network)
        : folly::CIDRNetworkV4(network) {}
    
    /**
     * @brief 获取IP地址
     * @return IP地址
     */
    folly::IPAddressV4 ip() const { return first; }
    
    /**
     * @brief 获取子网掩码
     * @return 子网掩码位数
     */
    uint8_t mask() const { return second; }
    
    /**
     * @brief 转换为字符串
     * @return 格式为"ip/mask"的字符串
     */
    std::string toString() const { return fmt::format("{}/{}", ip().str(), mask()); }
  };

  /**
   * @class Subnet
   * @brief 子网配置类
   * 
   * 定义了一个子网及其关联的网络区域。
   * 继承自ConfigBase模板类，提供配置项的序列化和反序列化功能。
   */
  class Subnet : public ConfigBase<Subnet> {
    /** 子网配置项，包含IP地址和掩码 */
    CONFIG_ITEM(subnet, Network({}));
    /** 网络区域配置项，默认为未知区域 */
    CONFIG_ITEM(network_zones, std::vector<std::string>{std::string(kUnknownZone)}, [](auto &v) { return !v.empty(); });
  };

  /** 是否允许没有可用设备 */
  CONFIG_ITEM(allow_no_usable_devices, false);
  /** 是否跳过非活动端口 */
  CONFIG_ITEM(skip_inactive_ports, true);
  /** 是否跳过不可用设备 */
  CONFIG_ITEM(skip_unusable_device, true);

  /** 是否允许未知网络区域 */
  CONFIG_ITEM(allow_unknown_zone, true);
  /** 设备过滤列表 */
  CONFIG_ITEM(device_filter, std::vector<std::string>());
  /** 默认网络区域 */
  CONFIG_ITEM(default_network_zone, std::string(kUnknownZone), [](auto &v) { return !v.empty(); });
  /** 子网配置列表 */
  CONFIG_ITEM(subnets, std::vector<Subnet>());
  /** 是否启用fork安全模式 */
  CONFIG_ITEM(fork_safe, true);
  /** 默认分区键索引 */
  CONFIG_ITEM(default_pkey_index, uint16_t(0));
  /** 默认RoCE分区键索引 */
  CONFIG_ITEM(default_roce_pkey_index, uint16_t(0));  // for RoCE, should just use default value 0
  // CONFIG_ITEM(default_gid_index, uint8_t(0));      // for RoCE
  /** 默认流量类别 */
  CONFIG_ITEM(default_traffic_class, uint8_t(0));  // for RoCE
  /** 是否优先使用IB设备 */
  CONFIG_ITEM(prefer_ibdevice, true);              // prefer use ibdevice instead of RoCE device.
};

class IBPort;
class IBManager;

/**
 * @class IBDevice
 * @brief IB设备类
 * 
 * 表示一个物理IB设备，管理设备资源和状态。
 * 继承自std::enable_shared_from_this，支持安全地创建指向自身的shared_ptr。
 */
class IBDevice : public std::enable_shared_from_this<IBDevice> {
 public:
  /** 设备智能指针类型 */
  using Ptr = std::shared_ptr<IBDevice>;
  /** 设备配置类型 */
  using Config = IBConfig;
  /** IB设备到网络设备映射类型 */
  using IB2NetMap = std::map<std::pair<std::string, uint8_t>, std::string>;

  /**
   * @struct Port
   * @brief 设备端口结构
   * 
   * 包含端口的网络地址、区域和属性信息。
   */
  struct Port {
    /** 端口网络地址列表 */
    std::vector<IfAddrs::Addr> addrs;
    /** 端口所属网络区域集合 */
    std::set<std::string> zones;
    /** 端口属性，使用folly::Synchronized保证线程安全 */
    mutable folly::Synchronized<ibv_port_attr> attr;
  };

  /**
   * @struct Deleter
   * @brief IB资源删除器
   * 
   * 为智能指针提供自定义删除操作，确保IB资源正确释放。
   */
  struct Deleter {
    /** 释放保护域 */
    void operator()(ibv_pd *pd) const {
      auto ret = ibv_dealloc_pd(pd);
      XLOGF_IF(CRITICAL, ret != 0, "ibv_dealloc_pd failed {}", ret);
    }
    /** 关闭设备上下文 */
    void operator()(ibv_context *context) const {
      auto ret = ibv_close_device(context);
      XLOGF_IF(CRITICAL, ret != 0, "ibv_close_device failed {}", ret);
    }
    /** 销毁完成队列 */
    void operator()(ibv_cq *cq) const {
      auto ret = ibv_destroy_cq(cq);
      XLOGF_IF(CRITICAL, ret != 0, "ibv_destroy_cq failed {}", ret);
    }
    /** 销毁队列对 */
    void operator()(ibv_qp *qp) const {
      auto ret = ibv_destroy_qp(qp);
      XLOGF_IF(CRITICAL, ret != 0, "ibv_destroy_qp failed {}", ret);
    }
    /** 销毁完成通道 */
    void operator()(ibv_comp_channel *channel) const {
      auto ret = ibv_destroy_comp_channel(channel);
      XLOGF_IF(CRITICAL, ret != 0, "ibv_destroy_comp_channel failed {}", ret);
    }
  };

  /** 最大设备数量 */
  static constexpr size_t kMaxDeviceCnt = 4;
  
  /**
   * @brief 获取所有设备
   * @return 设备指针向量的常引用
   */
  static const std::vector<IBDevice::Ptr> &all();
  
  /**
   * @brief 根据ID获取设备
   * @param devId 设备ID
   * @return 设备指针，如果不存在则返回nullptr
   */
  static IBDevice::Ptr get(uint8_t devId) {
    if (all().size() < devId) {
      return nullptr;
    }
    return all().at(devId);
  }

  /**
   * @brief 获取设备ID
   * @return 设备ID
   */
  uint8_t id() const { return devId_; }
  
  /**
   * @brief 获取设备名称
   * @return 设备名称的常引用
   */
  const std::string &name() const { return name_; }
  
  /**
   * @brief 获取设备属性
   * @return 设备属性的常引用
   */
  const ibv_device_attr &attr() const { return attr_; }
  
  /**
   * @brief 获取设备上下文
   * @return 设备上下文指针
   */
  ibv_context *context() const { return context_.get(); }
  
  /**
   * @brief 获取保护域
   * @return 保护域指针
   */
  ibv_pd *pd() const { return pd_.get(); }
  
  /**
   * @brief 获取所有端口
   * @return 端口映射的常引用
   */
  const std::map<uint8_t, Port> &ports() const { return ports_; }
  
  /**
   * @brief 打开端口
   * @param portNum 端口号
   * @return 成功时返回IBPort对象，失败时返回错误
   */
  Result<IBPort> openPort(size_t portNum) const;

  /**
   * @brief 注册内存区域
   * @param addr 内存地址
   * @param length 内存长度
   * @param access 访问权限
   * @return 内存区域句柄，失败时返回nullptr
   */
  ibv_mr *regMemory(void *addr, size_t length, int access) const;
  
  /**
   * @brief 注销内存区域
   * @param mr 内存区域句柄
   * @return 成功时返回0，失败时返回错误码
   */
  int deregMemory(ibv_mr *mr) const;

 private:
  friend class IBManager;
  class BackgroundRunner;
  class AsyncEventHandler;

  /**
   * @brief 打开所有设备
   * @param config 设备配置
   * @return 成功时返回设备指针向量，失败时返回错误
   */
  static Result<std::vector<IBDevice::Ptr>> openAll(const IBConfig &config);
  
  /**
   * @brief 打开单个设备
   * @param dev 设备指针
   * @param devId 设备ID
   * @param ib2net IB设备到网络设备的映射
   * @param ifaddrs 网络接口地址映射
   * @param config 设备配置
   * @return 成功时返回设备指针，失败时返回错误
   */
  static Result<IBDevice::Ptr> open(ibv_device *dev,
                                    uint8_t devId,
                                    std::map<std::pair<std::string, uint8_t>, std::string> ib2net,
                                    std::multimap<std::string, IfAddrs::Addr> ifaddrs,
                                    const IBConfig &config);

  FRIEND_TEST(TestIBDevice, IBConfig);
  
  /**
   * @brief 获取IB端口网络地址
   * @param ibdev2netdev IB设备到网络设备的映射
   * @param ifaddrs 网络接口地址映射
   * @param devName 设备名称
   * @param portNum 端口号
   * @return 网络地址向量
   */
  static std::vector<IfAddrs::Addr> getIBPortAddrs(const IB2NetMap &ibdev2netdev,
                                                   const IfAddrs::Map &ifaddrs,
                                                   const std::string &devName,
                                                   uint8_t portNum);
  
  /**
   * @brief 根据地址确定网络区域
   * @param addrs 网络地址向量
   * @param config 设备配置
   * @param devName 设备名称
   * @param portNum 端口号
   * @return 网络区域集合
   */
  static std::set<std::string> getZonesByAddrs(std::vector<IfAddrs::Addr> addrs,
                                               const IBConfig &config,
                                               const std::string &devName,
                                               uint8_t portNum);

  /**
   * @brief 检查异步事件
   */
  void checkAsyncEvent() const;
  
  /**
   * @brief 更新端口属性
   * @param portNum 端口号
   * @return 成功时返回Void对象，失败时返回错误
   */
  Result<Void> updatePort(size_t portNum) const;

  /** 设备ID */
  uint8_t devId_ = 0;
  /** 设备名称 */
  std::string name_;
  /** 设备上下文，使用智能指针管理 */
  std::unique_ptr<ibv_context, Deleter> context_;
  /** 保护域，使用智能指针管理 */
  std::unique_ptr<ibv_pd, Deleter> pd_;
  /** 设备属性 */
  ibv_device_attr attr_;
  /** 端口映射，键为端口号，值为Port结构 */
  std::map<uint8_t, Port> ports_;
};

/**
 * @class IBPort
 * @brief IB端口类
 * 
 * 表示IB设备上的一个端口，管理端口状态和属性。
 */
class IBPort {
 public:
  /**
   * @brief 构造函数
   * @param dev 设备指针
   * @param portNum 端口号
   * @param attr 端口属性
   * @param rocev2Gid RoCE v2 GID信息
   */
  IBPort(std::shared_ptr<const IBDevice> dev = {},
         uint8_t portNum = 0,
         ibv_port_attr attr = {},
         std::optional<std::pair<ibv_gid, uint8_t>> rocev2Gid = {});

  /**
   * @brief 获取端口号
   * @return 端口号
   */
  uint8_t portNum() const { return portNum_; }
  
  /**
   * @brief 获取设备指针
   * @return 设备指针
   */
  const IBDevice *dev() const { return dev_.get(); }
  
  /**
   * @brief 获取端口属性
   * @return 端口属性
   */
  const ibv_port_attr attr() const { return attr_; }
  
  /**
   * @brief 获取端口网络地址
   * @return 网络地址向量的常引用
   */
  const std::vector<IfAddrs::Addr> &addrs() const { return dev_->ports().at(portNum_).addrs; }
  
  /**
   * @brief 获取端口网络区域
   * @return 网络区域集合的常引用
   */
  const std::set<std::string> &zones() const { return dev_->ports().at(portNum_).zones; }

  /**
   * @brief 检查端口是否为RoCE类型
   * @return 如果是RoCE类型则返回true
   */
  bool isRoCE() const { return attr().link_layer == IBV_LINK_LAYER_ETHERNET; }
  
  /**
   * @brief 检查端口是否为InfiniBand类型
   * @return 如果是InfiniBand类型则返回true
   */
  bool isInfiniband() const { return attr().link_layer == IBV_LINK_LAYER_INFINIBAND; }
  
  /**
   * @brief 检查端口是否处于活动状态
   * @return 如果端口活动则返回true
   */
  bool isActive() const { return attr().state == IBV_PORT_ACTIVE || attr().state == IBV_PORT_ACTIVE_DEFER; }
  
  /**
   * @brief 查询GID
   * @param index GID索引
   * @return 成功时返回GID，失败时返回错误
   */
  Result<ibv_gid> queryGid(uint8_t index) const;
  
  /**
   * @brief 获取RoCE v2 GID
   * @return GID和索引的对
   */
  std::pair<ibv_gid, uint8_t> getRoCEv2Gid() const {
    XLOGF_IF(FATAL, !isRoCE(), "port is not RoCE");
    return *rocev2Gid_;
  }

  /**
   * @brief 转换为布尔值
   * @return 如果端口有效则返回true
   */
  operator bool() const { return dev_ && portNum_ != 0; }

 private:
  /** 设备指针 */
  std::shared_ptr<const IBDevice> dev_;
  /** 端口号 */
  uint8_t portNum_;
  /** 端口属性 */
  ibv_port_attr attr_;
  /** RoCE v2 GID信息 */
  std::optional<std::pair<ibv_gid, uint8_t>> rocev2Gid_;
};

class IBSocket;
class IBSocketManager;

/**
 * @class IBManager
 * @brief IB设备管理器类
 * 
 * 全局管理器，负责IB设备的初始化、监控和资源管理。
 * 采用单例模式实现全局管理。
 */
class IBManager {
 public:
  /**
   * @brief 获取单例实例
   * @return 单例实例的引用
   */
  static IBManager &instance() {
    static IBManager instance;
    return instance;
  }
  
  /**
   * @brief 启动IB设备管理器
   * @param config 设备配置
   * @return 成功时返回Void对象，失败时返回错误
   * 
   * 该方法执行以下步骤：
   * 1. 如果已初始化，直接返回成功
   * 2. 如果配置了fork安全模式，调用ibv_fork_init
   * 3. 调用IBDevice::openAll打开所有设备
   * 4. 创建事件循环和套接字管理器
   * 5. 创建后台运行器和事件处理器
   * 6. 建立网络区域到端口的映射
   */
  static Result<Void> start(IBConfig config) { return instance().startImpl(config); }

  /**
   * @brief 停止IB设备管理器
   * 
   * 调用reset方法清理所有资源。
   */
  static void stop() { return instance().reset(); }
  
  /**
   * @brief 关闭套接字
   * @param socket 套接字指针
   * 
   * 将套接字交给套接字管理器关闭。
   */
  static void close(std::unique_ptr<IBSocket> socket);
  
  /**
   * @brief 检查是否已初始化
   * @return 如果已初始化则返回true
   */
  static bool initialized() { return instance().inited_; }

  /**
   * @brief 析构函数
   * 
   * 调用reset方法清理所有资源。
   */
  ~IBManager();

  /**
   * @brief 获取所有设备
   * @return 设备指针向量的常引用
   */
  const std::vector<IBDevice::Ptr> &allDevices() const { return devices_; }
  
  /**
   * @brief 获取网络区域到端口的映射
   * @return 映射的常引用
   */
  const std::multimap<std::string, std::pair<IBDevice::Ptr, uint8_t>> zone2port() const { return zone2port_; }
  
  /**
   * @brief 获取配置
   * @return 配置的常引用
   */
  const auto &config() const { return config_; }

 private:
  /**
   * @brief 私有构造函数
   * 
   * 单例模式要求构造函数私有。
   */
  IBManager();

  /**
   * @brief 重置管理器
   * 
   * 清理所有资源，包括：
   * 1. 停止套接字管理器
   * 2. 停止事件循环
   * 3. 清空设备列表和区域映射
   */
  void reset();

  /**
   * @brief 启动实现
   * @param config 设备配置
   * @return 成功时返回Void对象，失败时返回错误
   */
  Result<Void> startImpl(IBConfig config);
  
  /**
   * @brief 停止实现
   * 
   * 调用reset方法清理所有资源。
   */
  void stopImpl() { return reset(); }
  
  /**
   * @brief 关闭套接字实现
   * @param socket 套接字指针
   */
  void closeImpl(std::unique_ptr<IBSocket> socket);

  friend class IBSocketManager;

  /** 设备配置 */
  IBConfig config_;
  /** 初始化标志 */
  std::atomic<bool> inited_ = false;
  /** 设备列表 */
  std::vector<IBDevice::Ptr> devices_;
  /** 网络区域到端口的映射 */
  std::multimap<std::string, std::pair<IBDevice::Ptr, uint8_t>> zone2port_;
  /** 事件循环 */
  std::shared_ptr<EventLoop> eventLoop_;
  /** 套接字管理器 */
  std::shared_ptr<IBSocketManager> socketManager_;
  /** 后台运行器 */
  std::shared_ptr<IBDevice::BackgroundRunner> devBgRunner_;
  /** 事件处理器列表 */
  std::vector<std::shared_ptr<IBDevice::AsyncEventHandler>> devEventHandlers_;
};
}  // namespace hf3fs::net

FMT_BEGIN_NAMESPACE

/**
 * @brief 获取链路层名称
 * @param linklayer 链路层类型
 * @return 链路层名称
 */
// inline关键字用于建议编译器将函数内联展开,可以减少函数调用开销
// 通常用于短小且频繁调用的函数
inline std::string_view ibvLinklayerName(int linklayer) {
  switch (linklayer) {
    case IBV_LINK_LAYER_INFINIBAND:
      return "INFINIBAND";
    case IBV_LINK_LAYER_ETHERNET:
      return "ETHERNET";
    default:
      return "UNSPECIFIED";
  }
}

/**
 * @brief 获取端口状态名称
 * @param state 端口状态
 * @return 端口状态名称
 */
inline std::string_view ibvPortStateName(ibv_port_state state) {
  switch (state) {
    case IBV_PORT_NOP:
      return "NOP";
    case IBV_PORT_DOWN:
      return "DOWN";
    case IBV_PORT_INIT:
      return "INIT";
    case IBV_PORT_ARMED:
      return "ARMED";
    case IBV_PORT_ACTIVE:
      return "ACTIVE";
    case IBV_PORT_ACTIVE_DEFER:
      return "ACTIVE_DEFER";
    default:
      return "UNKNOWN";
  }
}

/**
 * @brief ibv_gid的格式化器
 */
template <>
struct formatter<ibv_gid> : formatter<std::string_view> {
  template <typename FormatContext>
  auto format(const ibv_gid &gid, FormatContext &ctx) const {
    std::string str = fmt::format("{:x}", fmt::join(&gid.raw[0], &gid.raw[sizeof(gid.raw)], ":"));
    return formatter<std::string_view>::format(str, ctx);
  }
};

/**
 * @brief ibv_port_attr的格式化器
 */
template <>
struct formatter<ibv_port_attr> : formatter<std::string_view> {
  template <typename FormatContext>
  auto format(const ibv_port_attr &attr, FormatContext &ctx) const {
    std::string str = fmt::format("{{linklayer: {}, state: {}, lid: {}, mtu: {}}}",
                                  ibvLinklayerName(attr.link_layer),
                                  ibvPortStateName(attr.state),
                                  attr.lid,
                                  magic_enum::enum_name(attr.active_mtu));
    return formatter<std::string_view>::format(str, ctx);
  }
};

/**
 * @brief IBPort的格式化器
 */
template <>
struct formatter<hf3fs::net::IBPort> : formatter<std::string_view> {
  template <typename FormatContext>
  auto format(const hf3fs::net::IBPort &port, FormatContext &ctx) const {
    return fmt::format_to(ctx.out(),
                          "{{{}:{}, ifaddrs [{}], zones [{}], {}, {}}}",
                          port.dev()->name(),
                          port.portNum(),
                          fmt::join(port.addrs().begin(), port.addrs().end(), ";"),
                          fmt::join(port.zones().begin(), port.zones().end(), ";"),
                          ibvLinklayerName(port.attr().link_layer),
                          ibvPortStateName(port.attr().state));
  }
};

/**
 * @brief IBDevice::Config::Network的格式化器
 */
template <>
struct formatter<hf3fs::net::IBDevice::Config::Network> : formatter<std::string_view> {
  template <typename FormatContext>
  auto format(const hf3fs::net::IBDevice::Config::Network &network, FormatContext &ctx) const {
    return fmt::format_to(ctx.out(), "{}/{}", network.ip().str(), network.mask());
  }
};

FMT_END_NAMESPACE