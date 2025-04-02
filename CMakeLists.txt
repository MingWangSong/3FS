# 指定CMake最低版本要求，确保使用的功能在该版本中可用
cmake_minimum_required(VERSION 3.12)
# 定义项目名称、版本和使用的编程语言
# 这会设置PROJECT_NAME、PROJECT_VERSION等变量
project(3FS VERSION 0.1.5 LANGUAGES C CXX)

# 设置可用的构建类型并强制缓存这些值
# RelWithDebInfo: 带调试信息的发布版本（-O2 + 调试信息）
# Debug: 调试版本（-O0 + 调试信息）
# Release: 发布版本（-O3，无调试信息）
# MinSizeRel: 最小体积发布版本（-Os，优化体积）
set(CMAKE_CONFIGURATION_TYPES "RelWithDebInfo;Debug;Release;MinSizeRel" CACHE STRING "" FORCE)
# 如果未指定构建类型，默认使用RelWithDebInfo
if (NOT CMAKE_BUILD_TYPE OR CMAKE_BUILD_TYPE STREQUAL "None")
    set (CMAKE_BUILD_TYPE "RelWithDebInfo")
    message (STATUS "CMAKE_BUILD_TYPE is not set, set to default = ${CMAKE_BUILD_TYPE}")
endif ()
message (STATUS "CMAKE_BUILD_TYPE: ${CMAKE_BUILD_TYPE}")

# 将构建类型转换为大写，便于后续条件判断
string (TOUPPER ${CMAKE_BUILD_TYPE} CMAKE_BUILD_TYPE_UC)

# 根据构建类型决定是否启用断言
# 在Debug模式下默认启用断言，其他模式下默认禁用
if(CMAKE_BUILD_TYPE_UC STREQUAL "DEBUG" )
    option(ENABLE_ASSERTIONS "Enable assertions" ON)
else()
    option(ENABLE_ASSERTIONS "Enable assertions" OFF)
endif()
message (STATUS "ENABLE_ASSERTIONS: ${ENABLE_ASSERTIONS}")

# 如果启用断言，定义_DEBUG宏并确保NDEBUG宏被取消定义
# 这影响assert()宏的行为：定义NDEBUG时assert()不执行检查
if(ENABLE_ASSERTIONS)
    add_definitions(-D_DEBUG)
    # 在非Debug构建中，CMake自动定义NDEBUG，所以我们显式取消它
    if(NOT CMAKE_BUILD_TYPE_UC STREQUAL "DEBUG")
        # 使用add_compile_options而不是add_definitions，因为后者不支持生成器表达式
        # 生成器表达式$<...>允许在生成时评估条件
        add_compile_options($<$<OR:$<COMPILE_LANGUAGE:C>,$<COMPILE_LANGUAGE:CXX>>:-UNDEBUG>)
    endif()
endif()

# 定义是否覆盖C++的new/delete操作符的选项
# 这允许自定义内存分配行为，如跟踪内存泄漏
option(OVERRIDE_CXX_NEW_DELETE "Override C++ new/delete operator" OFF)
option(SAVE_ALLOCATE_SIZE "Use more memory to save allocate size" OFF)

# 启用FUSE应用程序构建
# FUSE允许在用户空间实现文件系统
option(ENABLE_FUSE_APPLICATION "" ON)

# 如果定义了SANITIZER，则禁用C++ new/delete操作符覆盖
# 因为sanitizer通常会提供自己的内存分配跟踪机制
if (DEFINED SANITIZER AND SANITIZER)
    set(OVERRIDE_CXX_NEW_DELETE OFF)
endif()
message (STATUS "OVERRIDE_CXX_NEW_DELETE: ${OVERRIDE_CXX_NEW_DELETE}")

# 如果启用了C++ new/delete操作符覆盖，添加相应的编译定义
# 这些宏会在预处理阶段被展开，影响代码的编译
if (OVERRIDE_CXX_NEW_DELETE)
    add_definitions(-DOVERRIDE_CXX_NEW_DELETE)
    if (SAVE_ALLOCATE_SIZE)
        add_definitions(-DSAVE_ALLOCATE_SIZE)
    endif()
endif()
message (STATUS "SAVE_ALLOCATE_SIZE: ${SAVE_ALLOCATE_SIZE}")

# 设置C语言标准为C11，不强制要求，允许扩展
# 这影响编译器如何解释C代码
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED OFF)
set(CMAKE_C_EXTENSIONS ON)
# 设置C++语言标准为C++20，强制要求，不允许扩展
# C++20引入了协程、概念等新特性
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
# 生成位置无关代码，适用于共享库
# 这允许代码在内存中的任何位置执行
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
# 生成compile_commands.json文件，用于代码分析工具
# 这个文件包含了编译每个源文件的完整命令
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# 根据不同的编译器设置特定的编译选项
if (CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    # 对于Clang编译器，启用协程支持
    # 协程允许编写可暂停和恢复的函数，适用于异步编程
    set (CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fcoroutines-ts")
    # 链接原子操作库，提供原子操作支持
    set(CMAKE_CXX_LINK_FLAGS "${CMAKE_CXX_LINK_FLAGS} -latomic")
    # 使用LLVM的lld链接器，比传统链接器更快
    add_link_options(-fuse-ld=lld)
    # 不使用LLVM的C++标准库实现
    set(USE_LIBCXX OFF)
elseif (CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    # 对于GCC编译器，启用协程支持
    set (CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fcoroutines")
endif()

# 从__FILE__宏变量中移除项目根目录，使错误消息更简洁
# 这样错误消息中的文件路径会相对于项目根目录显示
add_compile_options(-fmacro-prefix-map=${CMAKE_SOURCE_DIR}=.)

# 根据处理器架构设置特定的编译选项
if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64")
    # x86_64架构：启用SSE4.2和AVX2指令集优化
    # 这些SIMD指令集可以显著提高向量化操作的性能
    add_compile_options(-msse4.2 -mavx2)
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64")
    # ARM架构：设置特定的编译和链接选项
    # -march=armv8-a启用ARMv8-A架构特定优化
    add_compile_options(-march=armv8-a)
    message(STATUS "ARM architecture detected, linking with compiler-rt and libgcc.")
    # 使用compiler-rt提供运行时支持，libgcc提供异常展开支持
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -rtlib=compiler-rt -unwindlib=libgcc")
endif()

# 为RocksDB定义命名空间，避免与系统安装的RocksDB冲突
# 这样项目内部的RocksDB符号会使用rocksdb_internal命名空间
add_compile_definitions(ROCKSDB_NAMESPACE=rocksdb_internal)

# 包含自定义CMake模块
# Sanitizers.cmake定义了内存和线程检查工具的配置
include(cmake/Sanitizers.cmake)
# CompileFlags.cmake定义了编译标志的存储和恢复函数
include(cmake/CompileFlags.cmake)

# 集成第三方库
# 使用store_compile_flags()和restore_compile_flags()来暂时禁用某些编译标志
# 因为某些第三方库可能与严格的编译标志不兼容

# 集成fmt库（现代C++格式化库）
store_compile_flags()
add_subdirectory("third_party/fmt" EXCLUDE_FROM_ALL)
restore_compile_flags()

# 集成zstd库（快速压缩库）并设置相关变量
# 设置为静态构建，减少运行时依赖
set(ZSTD_BUILD_STATIC ON)
add_subdirectory("third_party/zstd/build/cmake" EXCLUDE_FROM_ALL)
# 设置包含目录和库路径，使主项目能够使用zstd
set(zstd_INCLUDE_DIRS "${PROJECT_SOURCE_DIR}/third_party/zstd/lib")
set(ZSTD_INCLUDE_DIR "${PROJECT_SOURCE_DIR}/third_party/zstd/lib")
set(zstd_FOUND ON)
set(ZSTD_FOUND ON)
set(zstd_LIBRARIES "${PROJECT_BINARY_DIR}/third_party/zstd/build/cmake/lib/libzstd.a")
set(ZSTD_LIBRARY "${PROJECT_BINARY_DIR}/third_party/zstd/build/cmake/lib/libzstd.a")
restore_compile_flags()

# 集成googletest库（单元测试框架）
add_subdirectory("third_party/googletest" EXCLUDE_FROM_ALL)
restore_compile_flags()

# 集成folly库（Facebook的C++工具库）
# 禁用异常跟踪器，减少开销
set(FOLLY_NO_EXCEPTION_TRACER ON)
add_subdirectory("third_party/folly" EXCLUDE_FROM_ALL)
restore_compile_flags()

# 集成leveldb库（键值存储库）并禁用其测试和基准
# 禁用不需要的组件，加快构建速度
set(LEVELDB_BUILD_TESTS OFF CACHE BOOL "Disable LevelDB tests")
set(LEVELDB_BUILD_BENCHMARKS OFF CACHE BOOL "Disable LevelDB benchmarks")
set(LEVELDB_INSTALL OFF CACHE BOOL "Disable LevelDB install")
add_subdirectory("third_party/leveldb" EXCLUDE_FROM_ALL)
restore_compile_flags()

# 设置CMake策略和RocksDB选项
# CMP0077策略控制变量设置行为
set(CMAKE_POLICY_DEFAULT_CMP0077 NEW)
# 启用LZ4和ZSTD压缩支持
set(WITH_LZ4 ON)
set(WITH_ZSTD ON)
# 启用RTTI（运行时类型信息）
set(USE_RTTI ON)
# 禁用不需要的组件，加快构建速度
set(WITH_TESTS OFF)
set(WITH_BENCHMARK_TOOLS OFF)
set(WITH_TOOLS OFF)
set(WITH_ALL_TESTS OFF)

# 集成rocksdb库（高性能键值存储库）
add_subdirectory("third_party/rocksdb" EXCLUDE_FROM_ALL)
restore_compile_flags()

# 集成scnlib库（现代C++扫描/解析库）并禁用其测试和示例
set(SCN_TESTS OFF)
set(SCN_EXAMPLES OFF)
set(SCN_BENCHMARKS OFF)
set(SCN_DOCS OFF)
set(SCN_INSTALL OFF)
set(SCN_PEDANTIC OFF)
add_subdirectory("third_party/scnlib" EXCLUDE_FROM_ALL)
restore_compile_flags()

# 集成pybind11库（C++/Python绑定库）
# 用于创建Python扩展模块
add_subdirectory("third_party/pybind11" EXCLUDE_FROM_ALL)
restore_compile_flags()

# 集成toml11库（TOML配置文件解析库）
add_subdirectory("third_party/toml11" EXCLUDE_FROM_ALL)
restore_compile_flags()

# 集成mimalloc库（高性能内存分配器）
# 禁用全局覆盖，避免与其他内存工具冲突
set (MI_OVERRIDE OFF)
add_subdirectory("third_party/mimalloc" EXCLUDE_FROM_ALL)
restore_compile_flags()

# 集成clickhouse-cpp库（ClickHouse数据库客户端库）
add_subdirectory("third_party/clickhouse-cpp" EXCLUDE_FROM_ALL)
# 设置包含目录，使主项目能够使用clickhouse-cpp
TARGET_INCLUDE_DIRECTORIES(clickhouse-cpp-lib
    PUBLIC ${PROJECT_SOURCE_DIR}/third_party/clickhouse-cpp
)
TARGET_INCLUDE_DIRECTORIES(clickhouse-cpp-lib-static
    PUBLIC ${PROJECT_SOURCE_DIR}/third_party/clickhouse-cpp
)
TARGET_INCLUDE_DIRECTORIES (absl-lib
    PUBLIC ${PROJECT_SOURCE_DIR}/third_party/clickhouse-cpp/contrib
)
restore_compile_flags()

# 集成liburing库（Linux异步IO库）
add_subdirectory("third_party/liburing-cmake" EXCLUDE_FROM_ALL)
restore_compile_flags()

# 设置C++编译标志，启用所有警告并将警告视为错误
# -Wall: 启用所有常见警告
# -Wextra: 启用额外警告
# -Werror: 将警告视为错误，强制修复
# -Wpedantic: 严格遵循语言标准
set (CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra -Werror -Wpedantic")

# 为不同构建类型设置特定的编译标志
# 添加通用编译标志
set (CMAKE_CXX_FLAGS                     "${CMAKE_CXX_FLAGS} ${COMPILER_FLAGS}")
# RelWithDebInfo: 高优化级别(-O3)加调试信息
set (CMAKE_CXX_FLAGS_RELWITHDEBINFO      "${CMAKE_CXX_FLAGS_RELWITHDEBINFO} -O3 ${DEBUG_INFO_FLAGS} ${CMAKE_CXX_FLAGS_ADD}")
# Debug: 无优化(-O0)，禁用内联，加调试信息
set (CMAKE_CXX_FLAGS_DEBUG               "${CMAKE_CXX_FLAGS_DEBUG} -O0 ${DEBUG_INFO_FLAGS} -fno-inline ${CMAKE_CXX_FLAGS_ADD}")

# 为C语言设置类似的编译标志
set (CMAKE_C_FLAGS                       "${CMAKE_C_FLAGS} ${COMPILER_FLAGS} ${CMAKE_C_FLAGS_ADD}")
set (CMAKE_C_FLAGS_RELWITHDEBINFO        "${CMAKE_C_FLAGS_RELWITHDEBINFO} -O3 ${DEBUG_INFO_FLAGS} ${CMAKE_C_FLAGS_ADD}")
set (CMAKE_C_FLAGS_DEBUG                 "${CMAKE_C_FLAGS_DEBUG} -O0 ${DEBUG_INFO_FLAGS} -fno-inline ${CMAKE_C_FLAGS_ADD}")

# 为汇编语言设置类似的编译标志
set (CMAKE_ASM_FLAGS                     "${CMAKE_ASM_FLAGS} ${COMPILER_FLAGS} ${CMAKE_ASM_FLAGS_ADD}")
set (CMAKE_ASM_FLAGS_RELWITHDEBINFO      "${CMAKE_ASM_FLAGS_RELWITHDEBINFO} -O3 ${DEBUG_INFO_FLAGS} ${CMAKE_ASM_FLAGS_ADD}")
set (CMAKE_ASM_FLAGS_DEBUG               "${CMAKE_ASM_FLAGS_DEBUG} -O0 ${DEBUG_INFO_FLAGS} -fno-inline ${CMAKE_ASM_FLAGS_ADD}")

# 使用静态Boost库，减少运行时依赖
set(Boost_USE_STATIC_LIBS ON)
# 设置FoundationDB版本
set(FDB_VERSION 7.1.5-ibe)

# 查找必要的依赖项
# Threads: 线程库，用于多线程编程
find_package(Threads REQUIRED)
# Boost: C++库集合，使用其文件系统、系统和程序选项组件
find_package(Boost REQUIRED COMPONENTS filesystem system program_options)
# libuv: 异步IO库
find_library(LIBUV_LIBRARY NAMES libuv1)

# 启用测试功能
enable_testing()

# 包含其他CMake模块
include(cmake/CodeCoverage.cmake)  # 代码覆盖率分析配置
include(cmake/CLangFormat.cmake)   # 代码格式化配置
include(cmake/CLangTidy.cmake)     # 代码静态分析配置
include(cmake/Target.cmake)        # 目标管理函数
include(cmake/DumpConfig.cmake)    # 配置导出函数
include(cmake/Jemalloc.cmake)      # Jemalloc内存分配器配置
include(cmake/ApacheArrow.cmake)   # Apache Arrow数据处理库配置
include(cmake/AddCrate.cmake)      # Rust集成配置
# 配置CTest自定义设置
configure_file(cmake/CTestCustom.cmake ${CMAKE_BINARY_DIR} @ONLY)

# 添加项目子目录
# src: 源代码目录，包含主要实现
add_subdirectory(src)
# tests: 测试目录，包含单元测试和集成测试
add_subdirectory(tests)
# benchmarks: 基准测试目录，用于性能测试
add_subdirectory(benchmarks)