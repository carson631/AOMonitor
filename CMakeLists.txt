cmake_minimum_required(VERSION 3.19)
project(AoMonitor LANGUAGES CXX)

find_package(Qt6 6.5 REQUIRED COMPONENTS Core Widgets SerialPort Charts Sql LinguistTools)

qt_standard_project_setup()

qt_add_executable(AoMonitor
    WIN32 MACOSX_BUNDLE
    main.cpp
    mainwindow.cpp
    mainwindow.h
    mainwindow.ui
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
)

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
