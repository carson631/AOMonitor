cmake_minimum_required(VERSION 3.19)
project(AoMonitor LANGUAGES CXX)

find_package(Qt6 6.5 REQUIRED COMPONENTS Core Widgets SerialPort Charts Sql OpenGL OpenGLWidgets LinguistTools Test)

qt_standard_project_setup()

# 界面主题写在外面的 theme.qss 里便于调色，通过 theme.qrc 打进 :/theme.qss
set(CMAKE_AUTORCC ON)

qt_add_executable(AoMonitor
    WIN32 MACOSX_BUNDLE
    main.cpp
    mainwindow.cpp
    mainwindow.h
    mainwindow.ui
    theme.qrc
    waveformglwidget.cpp
    waveformglwidget.h
    frameparser.cpp
    frameparser.h
)

qt_add_translations(
    TARGETS AoMonitor
    TS_FILES AoMonitor_zh_CN.ts
)

target_link_libraries(AoMonitor
    PRIVATE
        Qt::Core
        Qt::Widgets
        Qt::SerialPort
        Qt::Charts
        Qt::Sql
        Qt::OpenGL
        Qt::OpenGLWidgets
)

# uic 生成的 ui_mainwindow.h 在 build 目录，它 include 了项目内的自定义控件头，
# 必须把源码目录显式加进搜索路径，否则报 "waveformglwidget.h: No such file"
target_include_directories(AoMonitor PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

# ---------- 单元测试（Qt Test） ----------
# FrameParser 是纯逻辑类，测试不创建窗口，无显示环境（CI）也能跑：
#   ctest 或 ./aomonitor_tests 直接执行
enable_testing()
qt_add_executable(aomonitor_tests
    tests/test_ao_monitor.cpp
    frameparser.cpp
    frameparser.h
)
target_link_libraries(aomonitor_tests PRIVATE Qt6::Core Qt6::Test)
target_include_directories(aomonitor_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME aomonitor_tests COMMAND aomonitor_tests)

install(TARGETS AoMonitor
    BUNDLE  DESTINATION .
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
)

qt_generate_deploy_app_script(
    TARGET AoMonitor
    OUTPUT_SCRIPT deploy_script
    NO_UNSUPPORTED_PLATFORM_ERROR
)
install(SCRIPT ${deploy_script})
