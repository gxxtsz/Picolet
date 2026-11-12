# 遮罩和长图编辑窗口共用 SelectionOverlay，设置窗口与快捷键、自启动共享保存及回滚逻辑。
add_library(picolet_desktop STATIC
    "src/overlay.cpp"
    "src/long_editor.cpp"
    "src/long_capture.cpp"
    "src/gif_recording.cpp"
    "src/toolbar.cpp"
    "src/pins.cpp"
    "src/shortcut_dialog.cpp"
    "src/hotkeys.cpp"
    "src/startup.cpp"
    "src/export.cpp"
    "src/gif_clipboard.cpp"
)
add_library(Picolet::desktop ALIAS picolet_desktop)
picolet_configure_target(picolet_desktop)
target_include_directories(picolet_desktop PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")

# 窗口接口直接接收截图图像、选区和标注历史，因此公开依赖 capture。
# 资源编号和系统控件库用于窗口实现，图标、清单及对话框由最终可执行文件嵌入。
target_link_libraries(picolet_desktop
    PUBLIC Picolet::capture Picolet::win32
    PRIVATE Picolet::resources advapi32 comctl32 dwmapi gdiplus ole32 shcore shell32 uuid
)
