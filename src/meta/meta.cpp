#include "common/app/TwoPhaseApplication.h"
#include "memory/common/OverrideCppNewDelete.h"
#include "meta/service/MetaServer.h"

int main(int argc, char *argv[]) {
  // 使用using namespace声明引入hf3fs命名空间
  // 这样后续代码可以直接使用hf3fs命名空间中的类型和函数,无需添加hf3fs::前缀
  // 例如可以直接使用TwoPhaseApplication而不是hf3fs::TwoPhaseApplication
  using namespace hf3fs;
  // 这里使用了C++的临时对象语法
  // TwoPhaseApplication<MetaServer>() 创建了一个临时对象
  // 然后立即调用其run()方法
  // 等价于:
  // TwoPhaseApplication<meta::server::MetaServer> app;
  // return app.run(argc, argv);
  return TwoPhaseApplication<meta::server::MetaServer>().run(argc, argv);
}
