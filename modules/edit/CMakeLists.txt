add_library(picolet_edit STATIC "src/capture_edit.cpp")
add_library(Picolet::edit ALIAS picolet_edit)
picolet_configure_target(picolet_edit)
target_include_directories(picolet_edit PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
# 长图标注迁移使用滚动模块的 SeedPlacement，保留真实的单向依赖而不复制共享类型。
target_link_libraries(picolet_edit PUBLIC Picolet::image Picolet::scroll Picolet::win32)
