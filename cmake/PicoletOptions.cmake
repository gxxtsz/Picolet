include_guard(GLOBAL)

# 两个业务库及其调用方共用此接口目标。公开头文件直接使用 Windows 类型，
# 因此将 Unicode、最低 API 版本和头文件路径作为使用要求随目标传递。
add_library(picolet_win32 INTERFACE)
add_library(Picolet::win32 ALIAS picolet_win32)
target_include_directories(picolet_win32 INTERFACE "${PROJECT_SOURCE_DIR}/include")
target_compile_features(picolet_win32 INTERFACE cxx_std_20)
target_compile_definitions(picolet_win32 INTERFACE
    UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN WINVER=0x0A00 _WIN32_WINNT=0x0A00
)
# 内联句柄封装会在调用方实例化，释放菜单、位图等句柄所需的系统库也由此提供。
target_link_libraries(picolet_win32 INTERFACE gdi32 user32)

# 配置已经创建的项目库或可执行目标。警告和编码规则只影响当前目标的 C++ 源码，
# 不传递到使用方或资源编译器；所有模块使用相同的运行库以安全地跨模块移交对象。
function(picolet_configure_target target)
    set_target_properties(${target} PROPERTIES
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
    )
    if(MSVC)
        target_compile_options(${target} PRIVATE "$<$<COMPILE_LANGUAGE:CXX>:/W4;/utf-8>")
    elseif(MINGW)
        target_compile_options(${target} PRIVATE "$<$<COMPILE_LANGUAGE:CXX>:-Wall;-Wextra;-Wpedantic>")
        get_target_property(target_type ${target} TYPE)
        if(target_type STREQUAL "EXECUTABLE")
            # 静态链接 MinGW 运行库，保持应用和测试可以单文件运行；系统 DLL 仍由 Windows 提供。
            target_link_options(${target} PRIVATE -static)
        endif()
    endif()
endfunction()

# 仅用于以 wmain 或 wWinMain 为入口的可执行目标；普通 main 测试不应启用此选项。
# MSVC 根据入口自动选择启动代码，MinGW 则需要显式指定 Unicode 启动入口。
function(picolet_use_unicode_entry target)
    if(MINGW)
        target_link_options(${target} PRIVATE -municode)
    endif()
endfunction()
