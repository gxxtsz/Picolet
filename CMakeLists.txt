cmake_minimum_required(VERSION 3.20)
project(Picolet VERSION 1.1.0 LANGUAGES CXX)

if(NOT WIN32 OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "Picolet 需要 Windows x64 工具链。")
endif()
enable_language(RC)

# 所有可执行文件沿用构建根目录；多配置生成器会自动追加 Debug、Release 等目录。
# 版本文件只在当前构建目录生成，使不同工具链与配置能够同时存在。
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/lib")
set(PICOLET_GENERATED_DIR "${PROJECT_BINARY_DIR}/generated")
include(cmake/PicoletOptions.cmake)
find_package(Threads REQUIRED)
include(CTest)

# 先提供资源头，再按依赖顺序注册两个业务库。桌面模块消费采集结果，应用负责协调两者。
add_subdirectory(resources)
add_subdirectory(modules/capture)
add_subdirectory(modules/desktop)
add_subdirectory(app)
if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
