# mirrorcore:无 UI 的核心逻辑库(静态库,仅依赖 QtCore)
add_library(mirrorcore STATIC
    mirrorsession.cpp
    mirrorsession.h
    sessionmanager.cpp
    sessionmanager.h
    frameclient.cpp
    frameclient.h
    d3dinterop.cpp
    d3dinterop.h
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
    ${CMAKE_SOURCE_DIR}/sdk
)

target_link_libraries(mirrorcore PUBLIC Qt6::Core Qt6::Network Qt6::Gui)

if(WIN32)
    target_compile_definitions(mirrorcore PRIVATE _CRT_SECURE_NO_WARNINGS)
    # MS-MICE 解码: Windows Media Foundation(MFT)
    target_link_libraries(mirrorcore PUBLIC mfplat mfuuid)
    # GPU 零拷贝: D3D11 共享纹理 OpenSharedResourceByName(d3dinterop.cpp)
    target_link_libraries(mirrorcore PUBLIC d3d11)
    # WGL_NV_DX_interop2: wglGetProcAddress/wglGetCurrentContext(d3dinterop.cpp)
    target_link_libraries(mirrorcore PUBLIC opengl32)
endif()
