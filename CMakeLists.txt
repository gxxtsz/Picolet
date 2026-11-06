cmake_minimum_required(VERSION 3.20)
project(Picolet VERSION 1.0.0 LANGUAGES CXX)

if(NOT WIN32 OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "Picolet 需要 Windows x64 工具链。")
endif()

enable_language(RC)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_library(picolet_core STATIC src/image.cpp src/capture_edit.cpp src/export.cpp src/hotkeys.cpp)
target_include_directories(picolet_core PUBLIC src)
target_compile_definitions(picolet_core PUBLIC UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN
    WINVER=0x0A00 _WIN32_WINNT=0x0A00)
target_link_libraries(picolet_core PUBLIC gdi32 user32 ole32 shell32 uuid windowscodecs advapi32)

add_executable(Picolet WIN32
    src/app.cpp src/overlay.cpp src/pins.cpp src/shortcut_dialog.cpp src/picolet.rc)
set_property(SOURCE src/picolet.rc APPEND PROPERTY OBJECT_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/picolet.manifest;${CMAKE_CURRENT_SOURCE_DIR}/src/picolet.ico")
target_link_libraries(Picolet PRIVATE picolet_core comctl32 dwmapi shcore)

include(CTest)
set(picolet_targets picolet_core Picolet)
if(BUILD_TESTING)
    add_executable(picolet_image_tests tests/image_tests.cpp)
    target_link_libraries(picolet_image_tests PRIVATE picolet_core)
    add_test(NAME picolet.image COMMAND picolet_image_tests)
    list(APPEND picolet_targets picolet_image_tests)
endif()

foreach(target IN LISTS picolet_targets)
    if(MSVC)
        target_compile_options(${target} PRIVATE "$<$<COMPILE_LANGUAGE:CXX>:/W4;/utf-8>")
    elseif(MINGW)
        target_compile_options(${target} PRIVATE "$<$<COMPILE_LANGUAGE:CXX>:-Wall;-Wextra;-Wpedantic>")
        target_link_options(${target} PRIVATE -static)
    endif()
endforeach()

if(MSVC)
    # 资源文件已嵌入清单，避免链接器再生成一份默认清单。
    target_link_options(Picolet PRIVATE /MANIFEST:NO)
elseif(MINGW)
    target_compile_options(Picolet PRIVATE $<$<COMPILE_LANGUAGE:RC>:--codepage=65001>)
    target_link_options(Picolet PRIVATE -municode)
endif()
