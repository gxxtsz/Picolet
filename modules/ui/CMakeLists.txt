# 普通遮罩和长图编辑共同实现 SelectionOverlay；所有生产界面只在此目标中编译一次。
add_library(picolet_ui STATIC
    "src/overlay.cpp"
    "src/long_editor.cpp"
    "src/long_capture.cpp"
    "src/toolbar.cpp"
    "src/pins.cpp"
    "src/shortcut_dialog.cpp"
)
add_library(Picolet::ui ALIAS picolet_ui)
picolet_configure_target(picolet_ui)
target_include_directories(picolet_ui PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(picolet_ui
    PUBLIC Picolet::image Picolet::scroll Picolet::edit Picolet::system Picolet::win32
    PRIVATE Picolet::resources comctl32 dwmapi gdiplus shcore
)
