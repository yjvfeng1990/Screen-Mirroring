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
    mice/miceserver.cpp
    mice/miceserver.h
    mice/micertsp.cpp
    mice/micertsp.h
    mice/micertp.cpp
    mice/micertp.h
    mice/micedecoder.cpp
    mice/micedecoder.h
    mice/micebackend.cpp
    mice/micebackend.h
)

target_include_directories(mirrorcore PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(mirrorcore PUBLIC Qt6::Core Qt6::Network Qt6::Gui)

if(WIN32)
    target_compile_definitions(mirrorcore PRIVATE _CRT_SECURE_NO_WARNINGS)
    # MS-MICE 解码: Windows Media Foundation(MFT)
    target_link_libraries(mirrorcore PUBLIC mfplat mfuuid)
endif()
