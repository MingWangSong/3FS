#pragma once

#include "common/serde/CallContext.h"
#include "common/serde/Serde.h"
#include "common/serde/Service.h"

namespace hf3fs::serde::echo {

struct Message {
  SERDE_STRUCT_FIELD(str, std::string{});
};

SERDE_SERVICE(Service, 10000) {
  // send back which received.
  SERDE_SERVICE_METHOD(echo, 1, Message, Message);
};

// Echo服务的核心功能极其简单：接收客户端发送的消息，然后原样返回，一般用于连通性测试、健康检查
struct ServiceImpl : ServiceWrapper<ServiceImpl, Service> {
  CoTryTask<Message> echo(CallContext &, const Message &req) { co_return req; }
};

}  // namespace hf3fs::serde::echo
