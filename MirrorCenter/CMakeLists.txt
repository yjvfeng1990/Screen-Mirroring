cmake_minimum_required(VERSION 3.16)
project(MirrorCenter VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 自动处理 Qt moc/rcc/uic
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

find_package(Qt6 REQUIRED COMPONENTS Core Widgets Network Gui)

# 子目录:core 库 → sdk 库 → app 演示宿主
add_subdirectory(core)
add_subdirectory(sdk)
add_subdirectory(app)
