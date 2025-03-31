/**
 * @file monitor_collector.cpp
 * @brief 监控收集器应用程序入口点
 * 
 * 该文件实现了监控收集器应用程序的主入口点，
 * 通过OnePhaseApplication模板类实例化一个应用程序，
 * 并将MonitorCollectorServer作为服务器类型传入。
 */
#include "common/app/OnePhaseApplication.h"
#include "memory/common/OverrideCppNewDelete.h"
#include "monitor_collector/service/MonitorCollectorServer.h"

/**
 * @brief 应用程序入口点
 * @param argc 命令行参数数量
 * @param argv 命令行参数数组
 * @return 应用程序退出码
 * 
 * 使用OnePhaseApplication模板类创建应用程序实例，
 * 并调用其run方法启动应用程序。
 * 通过单例模式获取应用程序实例，确保全局唯一性。
 */
int main(int argc, char *argv[]) {
  return hf3fs::OnePhaseApplication<hf3fs::monitor::MonitorCollectorServer>::instance().run(argc, argv);
}
