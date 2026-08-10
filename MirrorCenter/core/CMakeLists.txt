# mirrorcore:无 UI 的核心逻辑库(静态库,仅依赖 QtCore)
add_library(mirrorcore STATIC
    mirrorsession.cpp
    mirrorsession.h
    sessionmanager.cpp
    sessionmanager.h
    frameclient.cpp
    frameclient.h
    airplaygateway.cpp
    airplaygateway.h
    mdnsbroadcast.cpp
    mdnsbroadcast.h
)

target_include_directories(mirrorcore PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(mirrorcore PUBLIC Qt6::Core Qt6::Network Qt6::Gui)

if(WIN32)
    target_compile_definitions(mirrorcore PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
