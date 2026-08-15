# MirrorCenter 演示宿主:通过 mirrorsdk 对接投屏能力
qt_add_executable(MirrorCenter WIN32
    main.cpp
    desktopwindow.cpp
    desktopwindow.h
    controlpanel.cpp
    controlpanel.h
    sessionview.cpp
    sessionview.h
    sidebar.cpp
    sidebar.h
    topbar.cpp
    topbar.h
    bottombar.cpp
    bottombar.h
    systemstatus.cpp
    systemstatus.h
    volumecontrol.cpp
    volumecontrol.h
    audiocontrol.cpp
    audiocontrol.h
    resources.qrc
)

target_include_directories(MirrorCenter PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/sdk
)

target_link_libraries(MirrorCenter PRIVATE
    mirrorsdk
    Qt6::Widgets
    Qt6::Network
    Qt6::OpenGLWidgets
)

if(WIN32)
    target_compile_definitions(MirrorCenter PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
