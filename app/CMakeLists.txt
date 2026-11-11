add_executable(Picolet WIN32 app.cpp)
picolet_configure_target(Picolet)
picolet_use_unicode_entry(Picolet)
picolet_add_resources(Picolet)
# 应用负责 COM、GDI+、托盘和会话切换，直接调用 capture 获取冻结帧，再交由 desktop 显示和输出。
target_link_libraries(Picolet PRIVATE
    Picolet::desktop Picolet::capture Picolet::win32
    advapi32 comctl32 dwmapi gdiplus ole32 shell32
)
