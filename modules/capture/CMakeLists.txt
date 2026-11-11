# 普通截图和滚动长截图共用像素类型及采集实现；标注迁移直接消费拼接输出的首帧映射。
add_library(picolet_capture STATIC
    "src/image.cpp"
    "src/capture.cpp"
    "src/hdr_capture.cpp"
    "src/hdr_color.cpp"
    "src/scroll_stitcher.cpp"
    "src/scroll_tracker.cpp"
    "src/capture_pipeline.cpp"
    "src/capture_edit.cpp"
    "src/png_export.cpp"
)
add_library(Picolet::capture ALIAS picolet_capture)
picolet_configure_target(picolet_capture)
target_include_directories(picolet_capture PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")

# 公开接口使用 Win32 类型和线程类型。图形采集与 WIC 编码只需要在实现中包含对应 SDK 头文件。
target_link_libraries(picolet_capture
    PUBLIC Picolet::win32 Threads::Threads
    PRIVATE d3d11 dxgi ole32 runtimeobject uuid windowscodecs
)
