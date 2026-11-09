add_executable(Picolet WIN32 app.cpp)
picolet_configure_target(Picolet)
picolet_use_unicode_entry(Picolet)
picolet_add_resources(Picolet)
# 应用负责 COM、GDI+、托盘和窗口协调，直接使用的模块及系统库在这里显式声明。
target_link_libraries(Picolet PRIVATE
    Picolet::image Picolet::scroll Picolet::edit Picolet::system Picolet::ui Picolet::win32
    comctl32 dwmapi gdiplus ole32 shell32
)
