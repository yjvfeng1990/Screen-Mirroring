# mirrorsdk:对外 SDK(C ABI 共享库)
# 生成 mirror_api.h 声明的导出符号
add_library(mirrorsdk SHARED
    mirror_api.cpp
)

set_target_properties(mirrorsdk PROPERTIES
    OUTPUT_NAME "mirrorsdk"
    VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR}
)

target_include_directories(mirrorsdk PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}          # mirror_api.h
    ${CMAKE_SOURCE_DIR}/core
)

target_link_libraries(mirrorsdk PRIVATE mirrorcore Qt6::Core)

target_compile_definitions(mirrorsdk PRIVATE MIRROR_SDK_BUILD)

if(WIN32)
    target_compile_definitions(mirrorsdk PRIVATE _CRT_SECURE_NO_WARNINGS)
    # MSVC 共享库需要导出符号表;这里用 WINDOWS_EXPORT_ALL_SYMBOLS 自动导出所有全局符号
    set_target_properties(mirrorsdk PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS ON)
else()
    # Linux 下默认隐藏符号,仅导出带 MIRROR_API 标注的接口
    set_target_properties(mirrorsdk PROPERTIES CXX_VISIBILITY_PRESET hidden)
endif()

# 每次 DLL 更新后,同步到演示宿主 exe 目录,保证运行时加载的是最新版
if(WIN32)
    add_custom_command(TARGET mirrorsdk POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/app
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:mirrorsdk>
            ${CMAKE_BINARY_DIR}/app/
        VERBATIM
    )
endif()
