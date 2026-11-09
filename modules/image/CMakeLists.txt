add_library(picolet_image STATIC
    "src/image.cpp"
    "src/export.cpp"
)
add_library(Picolet::image ALIAS picolet_image)
picolet_configure_target(picolet_image)
target_include_directories(picolet_image PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
# Windows 类型属于公开接口；COM、文件对话框和 WIC 编码库仅供实现使用。
target_link_libraries(picolet_image PUBLIC Picolet::win32 PRIVATE ole32 shell32 uuid windowscodecs)
