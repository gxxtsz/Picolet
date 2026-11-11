add_library(picolet_image STATIC
    "src/image.cpp"
    "src/capture.cpp"
    "src/export.cpp"
    "src/hdr_capture.cpp"
    "src/hdr_color.cpp"
)
add_library(Picolet::image ALIAS picolet_image)
picolet_configure_target(picolet_image)
target_include_directories(picolet_image PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
# Windows 类型属于公开接口；HDR 采集、文件对话框和 WIC 编码所用系统库仅供实现使用。
target_link_libraries(picolet_image PUBLIC Picolet::win32
    PRIVATE ole32 shell32 uuid windowscodecs d3d11 dxgi runtimeobject)
