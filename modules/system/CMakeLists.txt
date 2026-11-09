add_library(picolet_system STATIC
    "src/hotkeys.cpp"
    "src/startup.cpp"
)
add_library(Picolet::system ALIAS picolet_system)
picolet_configure_target(picolet_system)
target_include_directories(picolet_system PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
# 注册表和命令行解析属于实现依赖，不向调用方暴露额外的编译要求。
target_link_libraries(picolet_system PUBLIC Picolet::win32 PRIVATE advapi32 shell32)
