# 公共 Win32 封装不产生编译产物；宏、语言要求和内联函数所需的系统库随接口传播。
add_library(picolet_win32 INTERFACE)
add_library(Picolet::win32 ALIAS picolet_win32)
target_include_directories(picolet_win32 INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_compile_features(picolet_win32 INTERFACE cxx_std_20)
target_compile_definitions(picolet_win32 INTERFACE
    UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN WINVER=0x0A00 _WIN32_WINNT=0x0A00
)
target_link_libraries(picolet_win32 INTERFACE gdi32 user32)
