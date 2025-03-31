#include "IBDevice.h"

#include <algorithm>
#include <asm-generic/errno.h>
#include <bits/types/struct_itimerspec.h>
#include <bits/types/struct_timespec.h>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fmt/core.h>
#include <fmt/format.h>
#include <folly/Conv.h>
#include <folly/File.h>
#include <folly/FileUtil.h>
#include <folly/IPAddress.h>
#include <folly/IPAddressV4.h>
#include <folly/IPAddressV6.h>
#include <folly/Likely.h>
#include <folly/Random.h>
#include <folly/Range.h>
#include <folly/ScopeGuard.h>
#include <folly/String.h>
#include <folly/Subprocess.h>
#include <folly/Synchronized.h>
#include <folly/experimental/TimerFD.h>
#include <folly/functional/Partial.h>
#include <folly/logging/xlog.h>
#include <folly/small_vector.h>
#include <folly/system/Shell.h>
#include <folly/system/ThreadName.h>
#include <ifaddrs.h>
#include <infiniband/verbs.h>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <netinet/in.h>
#include <optional>
#include <set>
#include <shared_mutex>
#include <span>
#include <string>
#include <sys/epoll.h>
#include <sys/timerfd.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <variant>
#include <vector>

#include "common/monitor/Recorder.h"
#include "common/monitor/Sample.h"
#include "common/net/EventLoop.h"
#include "common/net/IfAddrs.h"
#include "common/net/ib/IBSocket.h"
#include "common/utils/Duration.h"
#include "common/utils/FdWrapper.h"
#include "common/utils/MagicEnum.hpp"
#include "common/utils/OptionalUtils.h"
#include "common/utils/Path.h"
#include "common/utils/Result.h"
#include "common/utils/String.h"

using namespace folly::literals::shell_literals;

/* from rdma-core
 * GID types as appear in sysfs, no change is expected as of ABI
 * compatibility.
 */
#define V1_TYPE "IB/RoCE v1"
#define V2_TYPE "RoCE v2"

namespace hf3fs::net {

namespace {
monitor::ValueRecorder deviceCnt("common.ib.devicecnt", {});
monitor::CountRecorder mrcnt("common.ib.mrcnt", {}, false);
monitor::CountRecorder mrSize("common.ib.mrsize", {}, false);
monitor::CountRecorder mrError("common.ib.mrerr");
monitor::LatencyRecorder mrLatency("common.ib.mr_reg_latency");
monitor::CountRecorder events("common.ib.events");

monitor::TagSet getPortTags(const IBDevice &device, uint8_t portNum, const std::set<std::string> &ports) {
  monitor::TagSet tag;
  tag.addTag("instance", fmt::format("{}:{}", device.name(), portNum));
  tag.addTag("tag", fmt::format("{}", fmt::join(ports.begin(), ports.end(), ";")));
  return tag;
}

// Ubuntu 20.04's rdma-core doesn't have ibv_query_gid_ex, and ibv_query_gid_type is a private symbol, so we have to
// implement this.
Result<std::pair<ibv_gid, uint8_t>> queryRoCEv2GID(ibv_context *ctx, uint8_t portNum) {
  auto devname = ibv_get_device_name(ctx->device);
  for (uint8_t index = 0; index < 32; index++) {
    ibv_gid gid;
    auto ret = ibv_query_gid(ctx, portNum, index, &gid);
    if (ret < 0) {
      if (ret == ENODATA) {
        continue;
      }
      XLOGF(CRITICAL, "{}:{} failed to query GID {}", devname, portNum, index);
      return makeError(RPCCode::kIBOpenPortFailed, "query GID failed");
    }
    auto ip = folly::IPAddressV6::fromBinary(folly::ByteRange(gid.raw, sizeof(gid.raw)));
    if (ip.isZero()) {
      continue;
    }
    if (ip.isLinkLocal()) {
      XLOGF(DBG, "{}:{} skip GID {}, ip {}", devname, portNum, index, ip.str());
      continue;
    }
    auto path = Path(ctx->device->ibdev_path) / fmt::format("ports/{}/gid_attrs/types/{}", portNum, index);
    auto gidType = std::string();
    auto ok = folly::readFile(path.c_str(), gidType);
    if (!ok) {
      XLOGF(CRITICAL, "{}:{} failed to query GID type from sysfs {}", devname, portNum, path);
      return makeError(RPCCode::kIBOpenPortFailed, "query GID type failed");
    }
    if (!gidType.starts_with(V2_TYPE)) {
      XLOGF(DBG,
            "{}:{} skip GID {}, type {} != {}",
            devname,
            portNum,
            index,
            gidType.substr(0, gidType.find_first_of('\n')),
            V2_TYPE);
      continue;
    }

    XLOGF(DBG, "{}:{} found RoCE v2 GID, index {}, ip {}", devname, portNum, index, ip.str());
    return std::pair<ibv_gid, uint8_t>(gid, index);
  }

  XLOGF(ERR, "{}:{} RoCE v2 GID not found", devname, portNum);
  return makeError(RPCCode::kIBOpenPortFailed, "RoCE v2 GID not found");
}

}  // namespace

using IBDevPort = std::pair<std::string, uint8_t>;
using NetDev = std::string;

static Result<std::map<IBDevPort, NetDev>> ibdev2netdev() {
  auto command = "env -i bash -l -c /usr/sbin/ibdev2netdev"_shellify();
  auto subprocess = folly::Subprocess(command, folly::Subprocess::Options().pipeStdout());
  String output;
  while (true) {
    char buf[4096];
    ssize_t rsize = ::read(subprocess.stdoutFd(), buf, sizeof(buf));
    if (rsize == 0) {
      break;
    } else if (rsize < 0) {
      XLOGF(ERR, "Failed to run ibdev2netdev, read from stdout failed, errno {}", errno);
      return makeError(RPCCode::kIBInitFailed, "Failed to run ibdev2netdev");
    }
    output.append(&buf[0], &buf[rsize]);
  }
  output = folly::trimWhitespace(output);

  auto ret = subprocess.wait();
  if (ret.exitStatus() != 0) {
    XLOGF(ERR, "Failed to run ibdev2netdev, ret code {}", ret.exitStatus());
    return makeError(RPCCode::kIBInitFailed, "Failed to run ibdev2netdev");
  }

  XLOGF(INFO, "ibdev2netdev: {}", output);
  std::map<IBDevPort, NetDev> map;
  std::vector<String> lines;
  folly::split('\n', output, lines, true);
  for (const auto &line : lines) {
    std::vector<String> fields;
    folly::split(" ", line, fields, true);
    auto devName = fields[0];
    auto port = folly::tryTo<uint8_t>(fields[2]);
    auto netDev = fields[4];
    // todo: test with multiple port devices!
    if (port.hasError()) {
      XLOGF(ERR, "Failed to parse port {} from ibdev2netdev output!", fields[2]);
    } else if (*port != 1) {
      XLOGF(WARN, "IBDevice is not tested with multiple port devices!!!");
    }
    map[{devName, *port}] = netDev;
    XLOGF(INFO, "ibdev2netdev parsed: {} => {}", devName, netDev);
  }

  return map;
}

static bool comparePortAttr(ibv_port_attr a, ibv_port_attr b) {
  return a.link_layer == b.link_layer && a.state == b.state && a.lid == b.lid && a.active_mtu == b.active_mtu;
}

Result<IBConfig::Network> IBConfig::Network::from(std::string_view str) {
  try {
    auto [ip, mask] = folly::IPAddress::createNetwork(str);
    if (ip.isV4()) {
      return IBConfig::Network{folly::CIDRNetworkV4(ip.asV4(), mask)};
    }
    XLOGF(ERR, "Subnet {} is not IPV4", str);
  } catch (std::runtime_error &ex) {
    XLOGF(ERR, "Failed to parse subnet {}, exception: {}", str, ex.what());
  }
  return makeError(StatusCode::kInvalidArg);
}

/* IBDevice */
const std::vector<IBDevice::Ptr> &IBDevice::all() { return IBManager::instance().allDevices(); }

Result<std::vector<IBDevice::Ptr>> IBDevice::openAll(const IBConfig &config) {
  // 获取IB设备与网络设备的映射关系
  // 调用ibdev2netdev命令获取系统中IB设备与网络设备的对应关系
  // 这对于确定IB设备的网络地址和区域非常重要
  auto ib2net = ibdev2netdev();
  if (ib2net.hasError()) {
    // 如果获取失败，可能是在容器环境中运行，记录警告并使用空映射继续
    XLOGF(WARN, "Failed to load ibdev2netdev, maybe running in container.");
    ib2net = std::map<IBDevPort, NetDev>();
  }

  // 加载系统网络接口信息
  // 获取所有网络接口的地址信息，用于后续确定IB设备的网络地址
  auto ifaddrs = IfAddrs::load();
  if (ifaddrs.hasError()) {
    // 如果加载失败，记录错误并返回，因为没有网络接口信息无法继续
    XLOGF(ERR, "Failed to load ifaddrs, error {}", ifaddrs.error());
    return makeError(RPCCode::kIBInitFailed);
  }

  // 获取系统中所有的IB设备列表
  // 调用ibv_get_device_list获取所有可用的RDMA设备
  int deviceCnt = 0;
  auto deviceList = ibv_get_device_list(&deviceCnt);
  if (deviceList == nullptr) {
    // 如果获取设备列表失败，记录错误并返回
    XLOGF(ERR, "Failed to load verbs device list, errno {}", errno);
    return makeError(RPCCode::kIBInitFailed, "Failed to load verbs devices.");
  }
  // 使用SCOPE_EXIT确保在函数退出时释放设备列表资源
  SCOPE_EXIT { ibv_free_device_list(deviceList); };
  if (deviceCnt <= 0) {
    // 如果没有找到RDMA设备，记录警告并返回错误
    XLOGF(WARN, "No RDMA devices!!!");
    return makeError(RPCCode::kIBInitFailed, "No usable RDMA devices.");
  }

  // 遍历设备列表，初始化每个设备
  std::vector<IBDevice::Ptr> devices;
  for (int idx = 0; idx < deviceCnt; idx++) {
    auto dev = deviceList[idx];
    auto devId = devices.size();  // 使用当前设备列表大小作为设备ID
    auto devName = ibv_get_device_name(dev);

    // 打开并初始化设备
    // 调用IBDevice::open方法初始化设备上下文、保护域等资源
    auto device = IBDevice::open(dev, devId, *ib2net, *ifaddrs, config);
    if (device.hasError()) {
      if (config.skip_unusable_device()) {
        // 如果配置允许跳过不可用设备，记录错误并继续处理下一个设备
        XLOGF(CRITICAL, "Failed to open IBDevice {}, error {}, skip it", devName, device.error());
        continue;
      }
      // 如果配置不允许跳过不可用设备，记录错误并返回
      XLOGF(ERR, "IBDevice failed to open device {}, error {}", devName, device.error());
      return makeError(device.error());
    }
    
    // 检查设备是否有可用端口
    if (device.value()->ports().empty()) {
      // 如果设备没有可用端口，记录信息并跳过该设备
      XLOGF(INFO, "IBDevice skip {} because it doesn't have available ports.", devName);
      continue;
    }
    
    // 添加设备到设备列表
    XLOGF(INFO, "IBDevice add {}, id {}, {} available ports", devName, devId, device.value()->ports().size());
    devices.emplace_back(std::move(*device));
  }

  // 检查设备数量是否超过最大限制
  if (devices.size() > kMaxDeviceCnt) {
    // 如果设备数量超过限制，记录错误并返回
    // 这时需要通过device_filter配置项来限制使用的设备
    XLOGF(CRITICAL,
          "too many ibdevices {} > kMaxDeviceCnt {}, please specify device_filter",
          devices.size(),
          kMaxDeviceCnt);
    return makeError(StatusCode::kInvalidArg);
  }

  // 返回初始化好的设备列表
  return devices;
}

Result<IBDevice::Ptr> IBDevice::open(ibv_device *dev,
                                     uint8_t devId,
                                     std::map<std::pair<std::string, uint8_t>, std::string> ib2net,
                                     std::multimap<std::string, IfAddrs::Addr> ifaddrs,
                                     const IBConfig &config) {
  // 创建IBDevice实例
  // 使用智能指针管理设备生命周期
  IBDevice::Ptr device(new IBDevice());
  
  // 设置设备ID和名称
  device->devId_ = devId;
  device->name_ = ibv_get_device_name(dev);
  
  // 打开设备上下文
  // 设备上下文是与IB设备交互的主要接口
  device->context_.reset(ibv_open_device(dev));
  if (!device->context_) {
    // 如果打开设备上下文失败，记录错误并返回
    XLOGF(ERR, "IBDevice failed to open {}, errno {}", device->name_, errno);
    return makeError(RPCCode::kIBInitFailed);
  }
  
  // 分配保护域(Protection Domain)
  // 保护域用于隔离不同应用程序的资源，提高安全性
  device->pd_.reset(ibv_alloc_pd(device->context_.get()));
  if (!device->pd_) {
    // 如果分配保护域失败，记录错误并返回
    XLOGF(ERR, "IBDevice failed to alloc pd for {}, errno {}", device->name_, errno);
    return makeError(RPCCode::kIBInitFailed);
  }
  
  // 查询设备属性
  // 获取设备的能力和限制信息，如最大队列对数、最大内存区域数等
  if (auto ret = ibv_query_device(device->context_.get(), &device->attr_); ret != 0) {
    // 如果查询设备属性失败，记录错误并返回
    XLOGF(ERR, "IBDevice failed to query device {}, errno {}", device->name_, errno);
    return makeError(RPCCode::kIBInitFailed);
  }

  // 创建设备过滤函数
  // 根据配置的device_filter确定是否使用特定设备
  auto filter = [&filter = config.device_filter()](std::string name) {
    return filter.empty() || std::find(filter.begin(), filter.end(), name) != filter.end();
  };

  // 确定要初始化的端口集合
  // 根据设备过滤条件选择要初始化的端口
  std::set<uint8_t> ports;
  for (uint8_t portNum = 1; portNum <= device->attr_.phys_port_cnt; portNum++) {
    // 查找端口对应的网络设备
    auto iter = ib2net.find({device->name_, portNum});
    auto netdev = (iter != ib2net.end()) ? std::optional(iter->second) : std::nullopt;
    
    // 如果设备名称在过滤列表中，或者网络设备名称在过滤列表中，则添加该端口
    if (filter(device->name_) || (netdev && filter(*netdev))) {
      ports.emplace(portNum);
    } else {
      // 否则跳过该端口
      XLOGF(INFO, "Skip device {}, port {} because it's not in device filter.", device->name_, portNum);
    }
  }

  // 设置异步事件文件描述符为非阻塞模式
  // 这样可以在事件循环中处理异步事件而不会阻塞
  auto flags = fcntl(device->context()->async_fd, F_GETFL);
  auto ret = fcntl(device->context()->async_fd, F_SETFL, flags | O_NONBLOCK);
  if (ret < 0) {
    // 如果设置非阻塞模式失败，记录错误并返回
    XLOGF(ERR, "IBDevice {} failed to set async fd to NONBLOCK.", device->name());
    return makeError(RPCCode::kIBInitFailed);
  }

  // 遍历所有选定的端口，初始化每个端口
  for (uint8_t portNum = 1; portNum <= device->attr_.phys_port_cnt; portNum++) {
    // 如果端口不在选定集合中，跳过
    if (!ports.contains(portNum)) {
      continue;
    }

    // 查询端口属性
    // 获取端口的链路层类型、状态、MTU等信息
    ibv_port_attr portAttr;
    if (auto ret = ibv_query_port(device->context_.get(), portNum, &portAttr); ret != 0) {
      // 如果查询端口属性失败，记录错误并返回
      XLOGF(ERR, "IBDevice failed to query port {} of device {}, errno {}", portNum, device->name_, ret);
      return makeError(RPCCode::kIBInitFailed);
    }
    
    // 检查端口链路层类型
    // 只支持以太网(RoCE)和InfiniBand类型的端口
    if (portAttr.link_layer != IBV_LINK_LAYER_ETHERNET && portAttr.link_layer != IBV_LINK_LAYER_INFINIBAND) {
      // 如果链路层类型不支持，记录警告并跳过该端口
      XLOGF(WARN,
            "IBDevice skip port {} of device {}, linklayer {} is not RoCE or INFINIBAND.",
            portNum,
            device->name_,
            portAttr.link_layer);
      continue;
    }
    
    // 检查端口状态
    // 如果端口不活跃且配置要求跳过非活动端口，则跳过该端口
    bool inactive = (portAttr.state != IBV_PORT_ACTIVE && portAttr.state != IBV_PORT_ACTIVE_DEFER);
    XLOGF_IF(WARN,
             inactive,
             "IBDevice {} port {} is not active, state {}, skip {}",
             device->name_,
             portNum,
             magic_enum::enum_name(portAttr.state),
             config.skip_inactive_ports());
    if (inactive && config.skip_inactive_ports()) {
      continue;
    }

    // 创建端口结构并填充信息
    Port port;
    // 获取端口网络地址
    port.addrs = getIBPortAddrs(ib2net, ifaddrs, device->name_, portNum);
    // 根据网络地址确定端口所属的网络区域
    port.zones = getZonesByAddrs(port.addrs, config, device->name_, portNum);
    // 保存端口属性
    port.attr = portAttr;
    
    // 检查网络区域
    // 如果不允许未知区域且端口只属于未知区域，则返回错误
    if (!config.allow_unknown_zone() && port.zones == std::set<std::string>{std::string(IBConfig::kUnknownZone)}) {
      XLOGF(CRITICAL, "IBDevice {}:{}'s zone is unknown!!!", device->name_, portNum);
      return makeError(StatusCode::kInvalidConfig);
    }
    
    // 对于RoCE端口，查询RoCE v2 GID
    std::optional<ibv_gid> rocev2Gid;
    if (portAttr.link_layer == IBV_LINK_LAYER_ETHERNET) {
      auto result = queryRoCEv2GID(device->context(), portNum);
      RETURN_ON_ERROR(result);
      rocev2Gid = result->first;
    }
    
    // 记录端口信息
    XLOGF(INFO,
          "IBDevice {} add active port {}, linklayer {}, addrs {}, zones {}, RoCE v2 GID {}",
          device->name_,
          portNum,
          fmt::ibvLinklayerName(portAttr.link_layer),
          fmt::join(port.addrs.begin(), port.addrs.end(), ";"),
          fmt::join(port.zones.begin(), port.zones.end(), ";"),
          OptionalFmt(rocev2Gid));
    
    // 将端口添加到设备的端口映射中
    device->ports_[portNum] = std::move(port);
  }

  // 返回初始化好的设备
  return device;
}

std::vector<IfAddrs::Addr> IBDevice::getIBPortAddrs(const IB2NetMap &ibdev2netdev,
                                                    const IfAddrs::Map &ifaddrs,
                                                    const std::string &devName,
                                                    uint8_t portNum) {
  auto key = IBDevPort{devName, portNum};
  if (!ibdev2netdev.contains(key)) {
    XLOGF(WARN, "IBDevice {}:{}'s netdev is unknown, maybe running in container.", devName, portNum);
    return {};
  }
  auto netdev = ibdev2netdev.at(key);
  auto [begin, end] = ifaddrs.equal_range(netdev);
  if (begin == end) {
    XLOGF(WARN, "IfAddr of {}:{} -> {} not found, maybe running in container!", devName, portNum, netdev);
    return {};
  }
  XLOGF_IF(WARN, begin == end, "IfAddr of {} not found!!", netdev);
  std::vector<IfAddrs::Addr> addrs;
  addrs.reserve(std::distance(begin, end));
  while (begin != end) {
    addrs.push_back(begin->second);
    begin++;
  }
  XLOGF(INFO, "IBDevice {}:{} IP addrs {}", devName, portNum, fmt::join(addrs.begin(), addrs.end(), ","));
  return addrs;
}

std::set<std::string> IBDevice::getZonesByAddrs(std::vector<IfAddrs::Addr> addrs,
                                                const IBConfig &config,
                                                const std::string &devName,
                                                uint8_t portNum) {
  std::set<std::string> zones;
  for (const auto addr : addrs) {
    for (const auto &subnet : config.subnets()) {
      if (addr.ip.inSubnet(subnet.subnet().ip(), subnet.subnet().mask())) {
        XLOGF(INFO,
              "IBDevice {}:{} addr {} in subnet {}, add network zones {}",
              devName,
              portNum,
              addr,
              subnet.subnet(),
              fmt::join(subnet.network_zones().begin(), subnet.network_zones().end(), ","));
        for (const auto &zone : subnet.network_zones()) {
          zones.insert(zone);
        }
      }
    }
  }

  if (zones.empty()) {
    auto fallback =
        config.default_network_zone().empty() ? std::string(IBConfig::kUnknownZone) : config.default_network_zone();
    if (fallback.starts_with("$")) {
      auto envName = fallback.substr(1);
      auto env = std::getenv(envName.c_str());
      if (!env || strlen(env) == 0) {
        XLOGF(CRITICAL,
              "IBDevice default network zone ENV {} not set, {}:{} fallback to {}!",
              envName,
              devName,
              portNum,
              IBConfig::kUnknownZone);
        return {std::string(IBConfig::kUnknownZone)};
      } else {
        XLOGF(INFO, "IBDevice {}:{} set to default zone {}, specified by ENV {}", devName, portNum, env, envName);
        return {std::string(env)};
      }
    }
    XLOGF(CRITICAL, "IBDevice {}:{} can't set zone by IP, fallback to {}", devName, portNum, fallback);
    return {fallback};
  }
  return zones;
}

Result<IBPort> IBDevice::openPort(size_t portNum) const {
  if (!ports_.contains(portNum)) {
    XLOGF(ERR, "IBDevice {} doesn't have port {}", name_, portNum);
    return makeError(RPCCode::kIBDeviceNotFound, fmt::format("port {}:{} not found", name_, portNum));
  }
  ibv_port_attr attr;
  if (auto ret = ibv_query_port(context(), portNum, &attr); ret != 0) {
    XLOGF(CRITICAL, "IBDevice failed to query port {} of device {}, errno {}", portNum, name_, ret);
    return makeError(RPCCode::kIBOpenPortFailed, fmt::format("query port {}:{} failed, err {}", name_, portNum, ret));
  }
  if (!comparePortAttr(attr, *ports_.at(portNum).attr.rlock())) {
    RETURN_ON_ERROR(updatePort(portNum));
  }

  std::optional<std::pair<ibv_gid, uint8_t>> rocev2Gid;
  if (attr.link_layer == IBV_LINK_LAYER_ETHERNET) {
    auto result = queryRoCEv2GID(context(), portNum);
    RETURN_ON_ERROR(result);
    rocev2Gid = *result;
  }

  return IBPort(shared_from_this(), portNum, attr, rocev2Gid);
}

Result<Void> IBDevice::updatePort(size_t portNum) const {
  if (!ports_.contains(portNum)) {
    XLOGF(ERR, "IBDevice {} doesn't have port {}", name_, portNum);
    return makeError(RPCCode::kIBOpenPortFailed, fmt::format("port {}:{} not found", name_, portNum));
  }

  const auto &port = ports_.at(portNum);
  auto wlock = port.attr.wlock();
  ibv_port_attr attr;
  if (auto ret = ibv_query_port(context(), portNum, &attr); ret != 0) {
    XLOGF(CRITICAL, "IBDevice failed to query port {} of device {}, errno {}", portNum, name_, ret);
    return makeError(RPCCode::kIBOpenPortFailed, fmt::format("query port {}:{} failed, err {}", name_, portNum, ret));
  }

  if (!comparePortAttr(attr, *wlock)) {
    if (attr.state != IBV_PORT_ACTIVE && attr.state != IBV_PORT_ACTIVE_DEFER) {
      XLOGF(WARN, "IBDevice {}:{} port attr update, old {} => new {}", name_, portNum, *wlock, attr);
    } else {
      XLOGF(INFO, "IBDevice {}:{} port attr update, old {} => new {}", name_, portNum, *wlock, attr);
    }
  } else {
    XLOGF(DBG, "IBDevice {}:{} port attr update, old {} => new {}", name_, portNum, *wlock, attr);
  }

  *wlock = attr;
  if (attr.state == IBV_PORT_ACTIVE || attr.state == IBV_PORT_ACTIVE_DEFER) {
    deviceCnt.set(1, getPortTags(*this, portNum, port.zones));
  } else {
    deviceCnt.set(-1, getPortTags(*this, portNum, port.zones));
  }

  return Void{};
}

ibv_mr *IBDevice::regMemory(void *addr, size_t length, int access) const {
  auto begin = SteadyClock::now();
  auto *mr = ibv_reg_mr(pd_.get(), addr, length, access);
  if (UNLIKELY(!mr)) {
    XLOGF(CRITICAL,
          "IBDevice {} failed to reg_mr, addr {}, length {}, access {}, errno {}",
          name(),
          addr,
          length,
          access,
          errno);
    mrError.addSample(1, {{"instance", name()}, {"tag", folly::to<std::string>(errno)}});
    return nullptr;
  }

  XLOGF(DBG, "IBDevice {} reg_mr, addr {}, length {}, access {}, mr {}", name(), addr, length, access, (void *)mr);
  auto tag = monitor::TagSet{{"instance", name()}};
  mrcnt.addSample(1, tag);
  mrSize.addSample(length, tag);
  mrLatency.addSample(SteadyClock::now() - begin, tag);
  return mr;
}

int IBDevice::deregMemory(ibv_mr *mr) const {
  auto length = mr ? mr->length : 0;
  auto ret = ibv_dereg_mr(mr);
  if (UNLIKELY(ret)) {
    XLOGF(CRITICAL, "IBDevice {} failed to dereg_mr {}, ret {}", name(), (void *)mr, ret);
    mrError.addSample(1, {{"instance", name()}, {"tag", folly::to<std::string>(ret)}});
    return ret;
  }

  XLOGF(DBG, "IBDevice {} dereg_mr mr {}", name(), (void *)mr);
  auto tag = monitor::TagSet{{"instance", name()}};
  mrcnt.addSample(-1, tag);
  mrSize.addSample(-length, tag);
  return ret;
}

class IBDevice::BackgroundRunner : public EventLoop::EventHandler,
                                   public std::enable_shared_from_this<BackgroundRunner> {
 public:
  static std::shared_ptr<BackgroundRunner> create() {
    auto fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK);
    if (fd < 0) {
      XLOGF(ERR, "Failed to create timer fd, errno {}", errno);
      return {};
    }

#ifdef NDEBUG
#define IB_RUNNER_INTERVAL 15
#else
#define IB_RUNNER_INTERVAL 5
#endif
    static_assert(IB_RUNNER_INTERVAL > 0);

    itimerspec spec{{IB_RUNNER_INTERVAL, 0}, {IB_RUNNER_INTERVAL, 0}};
    auto ret = timerfd_settime(fd, 0, &spec, nullptr);
    XLOGF_IF(FATAL, ret != 0, "timerfd_settime failed, errno {}", errno);
    return std::make_shared<BackgroundRunner>(fd);
  }

  BackgroundRunner(int fd)
      : timer_(fd) {}

  int fd() const override { return timer_; }
  void handleEvents(uint32_t /* events */) override {
    std::array<char, 64> buf;
    while (true) {
      auto read = ::read(timer_, buf.data(), buf.size());
      if (read < (int)buf.size()) {
        break;
      }
    }

    for (auto &dev : IBDevice::all()) {
      for (auto &[portNum, port] : dev->ports()) {
        dev->updatePort(portNum);
      }
    }
  }

 private:
  FdWrapper timer_;
};

class IBDevice::AsyncEventHandler : public EventLoop::EventHandler {
 public:
  AsyncEventHandler(IBDevice::Ptr dev)
      : dev_(dev) {}

  int fd() const override { return dev_->context()->async_fd; }
  void handleEvents(uint32_t /* epollEvents */) override { return dev_->checkAsyncEvent(); }

 private:
  IBDevice::Ptr dev_;
};

void IBDevice::checkAsyncEvent() const {
  XLOGF(DBG, "IBDevice {} check async event.", name());

  ibv_async_event event;
  auto ret = ibv_get_async_event(context(), &event);
  if (ret != 0) {
    return;
  }
  SCOPE_EXIT { ibv_ack_async_event(&event); };

  auto eventName = magic_enum::enum_name(event.event_type);
  auto instance = std::string(eventName);
  switch (event.event_type) {
    case IBV_EVENT_DEVICE_FATAL:
      events.addSample(1, {{"instance", instance}, {"tag", fmt::format("{}", name())}});
      XLOGF(CRITICAL, "IBDevice {} get event {}", name(), eventName);
      break;
    case IBV_EVENT_PORT_ERR:
      events.addSample(1, {{"instance", instance}, {"tag", fmt::format("{}:{}", name(), event.element.port_num)}});
      XLOGF(CRITICAL, "IBDevice {}:{} get event {}", name(), event.element.port_num, eventName);
      updatePort(event.element.port_num);
      break;
    case IBV_EVENT_PORT_ACTIVE:
    case IBV_EVENT_LID_CHANGE:
    case IBV_EVENT_PKEY_CHANGE:
    case IBV_EVENT_SM_CHANGE:
    case IBV_EVENT_CLIENT_REREGISTER:
    case IBV_EVENT_GID_CHANGE:
      events.addSample(1, {{"instance", instance}, {"tag", fmt::format("{}:{}", name(), event.element.port_num)}});
      XLOGF(INFO, "IBDevice {}:{} get event {}", name(), event.element.port_num, eventName);
      updatePort(event.element.port_num);
      break;
    default:
      XLOGF(DBG, "IBDevice {} get event {}", name(), eventName);
      break;
  }
}

/* IBPort */
IBPort::IBPort(std::shared_ptr<const IBDevice> dev,
               uint8_t portNum,
               ibv_port_attr attr,
               std::optional<std::pair<ibv_gid, uint8_t>> rocev2Gid)
    : dev_(dev),
      portNum_(portNum),
      attr_(attr),
      rocev2Gid_(rocev2Gid) {
  XLOGF_IF(FATAL, (dev && !dev->ports().contains(portNum)), "IBDevice {} doesn't have port {}!", dev->name(), portNum);
  XLOGF_IF(FATAL, (dev && isRoCE() && !rocev2Gid_), "{}:{} doesn't find RoCE v2 GID", dev->name(), portNum);
}

Result<ibv_gid> IBPort::queryGid(uint8_t index) const {
  ibv_gid gid;
  if (auto ret = ibv_query_gid(dev()->context(), portNum_, index, &gid); ret != 0) {
    XLOGF(ERR, "IBDevice {}:{} failed to query gid, errno {}", dev()->name(), portNum_, ret);
    return makeError(RPCCode::kIBOpenPortFailed);
  }
  return gid;
}

/* IBManager */
void IBManager::close(std::unique_ptr<IBSocket> socket) { return instance().closeImpl(std::move(socket)); }

IBManager::IBManager() = default;
IBManager::~IBManager() { reset(); }

Result<Void> IBManager::startImpl(IBConfig config) {
  // 检查是否已经初始化，避免重复初始化
  if (inited_) {
    return Void{};
  }

  // 处理fork安全模式
  // 在多进程环境下，如果子进程会使用IB设备，需要调用ibv_fork_init确保安全
  // 这是一个全局设置，只需要初始化一次
  static bool forkInited = false;
  if (!forkInited && config.fork_safe()) {
    auto ret = ibv_fork_init();
    if (ret < 0) {
      auto msg = fmt::format("ibv_fork_init failed {}", ret);
      XLOG(CRITICAL, msg);
      return makeError(RPCCode::kIBInitFailed, msg);
    }
    forkInited = true;
  }

  // 保存配置
  config_ = config;
  
  // 调用IBDevice::openAll打开所有IB设备
  // 这个过程会发现系统中的IB设备，初始化设备上下文和保护域，并查询设备属性
  auto devices = IBDevice::openAll(config_);
  if (devices.hasError()) {
    // 如果打开设备失败，记录错误并返回
    XLOGF(ERR, "Failed to open all IBDevices, error {}", devices.error());
    return makeError(devices.error());
  } else if (devices->empty()) {
    // 如果没有找到可用设备，根据配置决定是否允许继续
    if (config_.allow_no_usable_devices()) {
      // 如果配置允许没有可用设备，仅记录警告
      XLOGF(WARN, "IBManager can't find available device!");
    } else {
      // 如果配置不允许没有可用设备，记录错误并返回
      XLOGF(ERR, "IBManager can't find available device!");
      return makeError(RPCCode::kIBDeviceNotFound);
    }
  }

  // 保存设备列表
  devices_ = std::move(*devices);
  
  // 创建事件循环
  // 事件循环用于处理异步事件，如设备状态变化、套接字事件等
  eventLoop_ = EventLoop::create();
  auto result = eventLoop_->start("IBManager");
  RETURN_ON_ERROR(result);

  // 创建套接字管理器
  // 套接字管理器负责管理IB套接字的生命周期和资源释放
  socketManager_ = IBSocketManager::create();
  if (!socketManager_) {
    return makeError(RPCCode::kIBInitFailed, "Failed to create IBSocketManager");
  }
  // 将套接字管理器添加到事件循环中，监听读写和边缘触发事件
  eventLoop_->add(socketManager_, (EPOLLIN | EPOLLOUT | EPOLLET));

  // 创建后台运行器
  // 后台运行器负责定期检查设备状态，更新端口属性等
  devBgRunner_ = IBDevice::BackgroundRunner::create();
  if (!devBgRunner_) {
    return makeError(RPCCode::kIBInitFailed, "Failed to create IBDevice::BackgroundRunner");
  }
  // 将后台运行器添加到事件循环中，监听读写和边缘触发事件
  eventLoop_->add(devBgRunner_, (EPOLLIN | EPOLLOUT | EPOLLET));

  // 为每个设备创建异步事件处理器
  // 遍历所有设备，设置事件处理和网络区域映射
  for (const auto &dev : devices_) {
    // 创建异步事件处理器，用于处理设备异步事件（如端口状态变化）
    auto handler = std::make_shared<IBDevice::AsyncEventHandler>(dev);
    // 将事件处理器添加到事件循环中，监听读写和边缘触发事件
    eventLoop_->add(handler, (EPOLLIN | EPOLLOUT | EPOLLET));
    // 保存事件处理器，防止被提前释放
    devEventHandlers_.push_back(handler);
    
    // 遍历设备的所有端口，建立网络区域到端口的映射
    for (const auto &port : dev->ports()) {
      // 确保端口有网络区域，否则终止程序
      XLOGF_IF(FATAL, port.second.zones.empty(), "Zone is empty for port {}:{}", dev->name(), port.first);
      
      // 遍历端口的所有网络区域，建立区域到端口的映射
      for (const auto &zone : port.second.zones) {
        // 跳过未知区域
        if (zone != IBConfig::kUnknownZone) {
          // 将网络区域映射到设备端口
          // 这个映射用于根据网络区域快速找到对应的设备端口
          zone2port_.emplace(zone, std::pair<IBDevice::Ptr, uint8_t>{dev, port.first});
        }
      }
      
      // 设置设备计数监控指标
      // 这用于监控系统中活动设备的数量
      deviceCnt.set(1, getPortTags(*dev, port.first, port.second.zones));
    }
  }

  // 标记初始化完成
  inited_ = true;
  return Void{};
}

void IBManager::reset() {
  // 首先将初始化标志设置为false，表示管理器已不再处于初始化状态
  // 这样可以防止其他线程继续使用已经开始清理的管理器
  inited_ = false;

  // 停止并清理套接字管理器
  // 套接字管理器负责管理所有IB套接字的生命周期
  if (socketManager_) {
    // stopAndJoin会等待所有正在关闭的套接字完成清理
    // 这确保了所有套接字资源都被正确释放
    socketManager_->stopAndJoin();
    // 重置智能指针，释放套接字管理器
    socketManager_.reset();
  }
  
  // 停止并清理事件循环
  // 事件循环负责处理所有异步事件，包括设备状态变化、套接字事件等
  if (eventLoop_) {
    // stopAndJoin会等待事件循环线程退出
    // 这确保了所有事件处理器都不再被调用
    eventLoop_->stopAndJoin();
    // 重置智能指针，释放事件循环
    eventLoop_.reset();
  }

  // 清空设备列表
  // 由于使用了智能指针管理设备，这里的clear会触发设备资源的释放
  // 包括设备上下文、保护域等IB资源
  devices_.clear();
  
  // 清空网络区域到端口的映射
  // 这个映射用于根据网络区域快速找到对应的设备端口
  zone2port_.clear();
}

void IBManager::closeImpl(IBSocket::Ptr socket) {
  // 检查套接字管理器是否存在
  // 如果管理器已经被销毁，则无法关闭套接字
  if (socketManager_) {
    // 将套接字交给套接字管理器关闭
    // 套接字管理器会负责套接字的优雅关闭和资源释放
    // 这包括：
    // 1. 创建一个Drainer对象来处理套接字的关闭过程
    // 2. 将Drainer添加到事件循环中，监听套接字事件
    // 3. 设置超时机制，确保套接字能够在指定时间内关闭
    // 4. 完成关闭后释放套接字资源
    socketManager_->close(std::move(socket));
  }
}

}  // namespace hf3fs::net