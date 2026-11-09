# 界面、文件属性和清单只从顶层 project() 获取版本；Windows 第四段版本固定为零。
set(PICOLET_WINDOWS_VERSION "${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}.${PROJECT_VERSION_PATCH}.0")
set(PICOLET_ICON_PATH "${CMAKE_CURRENT_SOURCE_DIR}/picolet.ico")
configure_file(picolet_version.h.in "${PICOLET_GENERATED_DIR}/include/picolet/resources/picolet_version.h" @ONLY)
configure_file(picolet.manifest.in "${PICOLET_GENERATED_DIR}/picolet.manifest" @ONLY)

# 只共享资源编号和版本头，不传播 .rc；最终可执行文件必须显式嵌入各自的资源。
add_library(picolet_resources INTERFACE)
add_library(Picolet::resources ALIAS picolet_resources)
target_include_directories(picolet_resources INTERFACE
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${PICOLET_GENERATED_DIR}/include"
)

# 为已经创建的可执行目标加入图标、对话框、版本和清单。
# SOURCE 属性按目录生效，必须在目标所属目录设置依赖，才能覆盖应用和测试两个目录。
function(picolet_add_resources target)
    # 使用函数定义所在目录，不依赖调用方当前位于 app 还是 tests。
    set(resource_source "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/picolet.rc")
    target_sources(${target} PRIVATE "${resource_source}")
    target_link_libraries(${target} PRIVATE Picolet::resources)
    set_property(SOURCE "${resource_source}" TARGET_DIRECTORY ${target} PROPERTY OBJECT_DEPENDS
        "${PICOLET_GENERATED_DIR}/include/picolet/resources/picolet_version.h;${PICOLET_GENERATED_DIR}/picolet.manifest;${CMAKE_CURRENT_FUNCTION_LIST_DIR}/picolet.ico")
    if(MSVC)
        # .rc 已嵌入包含 DPI、权限和控件版本的清单，禁止链接器再生成默认清单。
        target_link_options(${target} PRIVATE /MANIFEST:NO)
    elseif(MINGW)
        target_compile_options(${target} PRIVATE $<$<COMPILE_LANGUAGE:RC>:--codepage=65001>)
    endif()
endfunction()
