add_library(picolet_scroll STATIC
    "src/scroll_stitcher.cpp"
    "src/scroll_tracker.cpp"
    "src/capture_pipeline.cpp"
)
add_library(Picolet::scroll ALIAS picolet_scroll)
picolet_configure_target(picolet_scroll)
target_include_directories(picolet_scroll PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
# 采集接口及类成员使用图像、Win32 句柄和线程类型，因此向调用方传递其使用要求。
target_link_libraries(picolet_scroll PUBLIC Picolet::image Picolet::win32 Threads::Threads)
