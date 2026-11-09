# 测试保留原有覆盖范围和名称。每个程序只编译自己的入口，生产实现全部链接模块库。
add_executable(picolet_image_tests image_tests.cpp)
picolet_configure_target(picolet_image_tests)
target_link_libraries(picolet_image_tests PRIVATE
    Picolet::image Picolet::edit Picolet::system Picolet::win32 ole32 windowscodecs uuid
)
# 贴图阴影是 UI 内部实现，只为已有白盒测试提供私有访问路径，不扩大 UI 的公开接口。
target_include_directories(picolet_image_tests PRIVATE "${PROJECT_SOURCE_DIR}/modules/ui/src")
add_test(NAME picolet.image COMMAND picolet_image_tests)

add_executable(picolet_scroll_tests scroll_tests.cpp)
picolet_configure_target(picolet_scroll_tests)
target_link_libraries(picolet_scroll_tests PRIVATE
    Picolet::scroll Picolet::edit Picolet::image Picolet::system Picolet::ui Picolet::win32
    ole32 windowscodecs uuid
)
add_test(NAME picolet.scroll COMMAND picolet_scroll_tests)

add_executable(picolet_fast_scroll_tests fast_scroll_tests.cpp)
picolet_configure_target(picolet_fast_scroll_tests)
target_link_libraries(picolet_fast_scroll_tests PRIVATE Picolet::scroll)
add_test(NAME picolet.fast_scroll COMMAND picolet_fast_scroll_tests)

add_executable(picolet_startup_tests startup_tests.cpp)
picolet_configure_target(picolet_startup_tests)
picolet_use_unicode_entry(picolet_startup_tests)
picolet_add_resources(picolet_startup_tests)
target_link_libraries(picolet_startup_tests PRIVATE
    Picolet::system Picolet::ui Picolet::win32 comctl32 advapi32 shell32
)
# 测试会启动真实应用验证单实例行为；只构建本测试时也必须先得到对应配置的应用。
add_dependencies(picolet_startup_tests Picolet)
add_test(NAME picolet.startup COMMAND picolet_startup_tests $<TARGET_FILE:Picolet>)
set_tests_properties(picolet.startup PROPERTIES TIMEOUT 30)

add_executable(picolet_settings_fixture WIN32 startup_tests.cpp)
picolet_configure_target(picolet_settings_fixture)
picolet_use_unicode_entry(picolet_settings_fixture)
picolet_add_resources(picolet_settings_fixture)
target_compile_definitions(picolet_settings_fixture PRIVATE PICOLET_SETTINGS_FIXTURE)
target_link_libraries(picolet_settings_fixture PRIVATE
    Picolet::system Picolet::ui Picolet::win32 comctl32 advapi32 shell32
)

add_executable(picolet_paint_tests paint_tests.cpp)
picolet_configure_target(picolet_paint_tests)
picolet_add_resources(picolet_paint_tests)
target_link_libraries(picolet_paint_tests PRIVATE
    Picolet::ui Picolet::scroll Picolet::win32 comctl32 gdiplus
)
add_test(NAME picolet.paint COMMAND picolet_paint_tests)

add_executable(picolet_scroll_fixture WIN32 scroll_fixture.cpp)
picolet_configure_target(picolet_scroll_fixture)
picolet_use_unicode_entry(picolet_scroll_fixture)
picolet_add_resources(picolet_scroll_fixture)
target_link_libraries(picolet_scroll_fixture PRIVATE Picolet::edit Picolet::image Picolet::win32)

add_executable(picolet_capture_fixture WIN32 capture_fixture.cpp)
picolet_configure_target(picolet_capture_fixture)
picolet_use_unicode_entry(picolet_capture_fixture)
picolet_add_resources(picolet_capture_fixture)
target_link_libraries(picolet_capture_fixture PRIVATE
    Picolet::ui Picolet::image Picolet::scroll Picolet::edit Picolet::win32 comctl32 dwmapi ole32 gdiplus
)

# 原生探针需要交互滚动窗口，保留显式构建入口，不纳入默认构建和无人值守 CTest。
add_executable(picolet_native_scroll_probe EXCLUDE_FROM_ALL native_scroll_probe.cpp)
picolet_configure_target(picolet_native_scroll_probe)
picolet_add_resources(picolet_native_scroll_probe)
target_link_libraries(picolet_native_scroll_probe PRIVATE
    Picolet::image Picolet::edit Picolet::scroll Picolet::win32 ole32
)
