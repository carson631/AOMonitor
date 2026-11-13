cmake_minimum_required(VERSION 3.19)
project(AoMonitor LANGUAGES CXX)

find_package(Qt6 6.5 REQUIRED COMPONENTS Core Widgets SerialPort Charts Sql OpenGL OpenGLWidgets Network LinguistTools Test)

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
    tcpclient.cpp
    tcpclient.h
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
        Qt::Network
)

# uic 生成的 ui_mainwindow.h 在 build 目录，它 include 了项目内的自定义控件头，
# 必须把源码目录显式加进搜索路径，否则报 "waveformglwidget.h: No such file"
target_include_directories(AoMonitor PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

# ---------- Windows：每次构建后把 Qt 运行库拷到 exe 旁 ----------
# 本机系统 PATH 里有 "C:\Program Files\010 Editor"，它自带 Qt 6.4.3 的 Qt6Core.dll，
# 且系统 PATH 恒排在用户 PATH 之前。exe 旁边没有 DLL 时 Windows 会先命中 6.4.3，
# 报 "无法定位程序输入点 _Z11qt_assert_xPKcS0_S0_i"。部署到 exe 旁可彻底规避。
if(WIN32)
    get_target_property(_qmake_exe Qt6::qmake IMPORTED_LOCATION)
    get_filename_component(_qt_bin_dir "${_qmake_exe}" DIRECTORY)
    find_program(WINDEPLOYQT_EXECUTABLE windeployqt HINTS "${_qt_bin_dir}" NO_DEFAULT_PATH)
    if(WINDEPLOYQT_EXECUTABLE)
        add_custom_command(TARGET AoMonitor POST_BUILD
            COMMAND "${WINDEPLOYQT_EXECUTABLE}"
                    --no-translations
                    --no-opengl-sw              # 用系统 OpenGL（本程序跑真实 GL 波形）
                    --no-system-d3d-compiler
                    "$<TARGET_FILE:AoMonitor>"
            COMMENT "windeployqt: 把 Qt 运行库部署到 exe 旁（防止 PATH 上其他软件的 Qt 抢占）"
            VERBATIM)

        # 部署后 app 目录里有 platforms\qwindows.dll，其搜索优先级最高；
        # ctest 的 offscreen 平台会因此找不到 qoffscreen.dll 而 fail-fast。
        # 记录完整插件目录，等 add_test 之后统一设置（顺序不能反）。
        get_filename_component(_qt_plugin_dir "${_qt_bin_dir}/../plugins" REALPATH)
    endif()
endif()

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

# TCP 数据源测试：测试内起一个 QTcpServer 做真实环回（不 mock），
# 验证 连接/断开/收字节/断线重连 与 FrameParser 组合切帧
qt_add_executable(aomonitor_tcp_tests
    tests/test_tcpclient.cpp
    tcpclient.cpp
    tcpclient.h
    frameparser.cpp
    frameparser.h
)
target_link_libraries(aomonitor_tcp_tests PRIVATE Qt6::Core Qt6::Test Qt6::Network)
target_include_directories(aomonitor_tcp_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME aomonitor_tcp_tests COMMAND aomonitor_tcp_tests)

# 两个测试都用 offscreen 平台跑（无需显示器）。注意 windeployqt 部署后，
# app 目录的 platforms\ 搜索优先级最高，里面只有 qwindows.dll，
# 不补 QT_PLUGIN_PATH 就会找不到 qoffscreen.dll 直接 fail-fast。
if(WIN32 AND _qt_plugin_dir)
    set_tests_properties(aomonitor_tests aomonitor_tcp_tests PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_PLUGIN_PATH=${_qt_plugin_dir}")
endif()

# ---------- 覆盖率（gcov/gcovr） ----------
# -DENABLE_COVERAGE=ON 时只给测试目标插桩（--coverage 编译+链接都要加），
# 主程序 AoMonitor 保持干净。跑完测试后在 build 目录会生成 .gcda，
# 再用 gcovr 出 HTML（手顺见 QTest执行手顺.md 第 7 节）。
option(ENABLE_COVERAGE "Enable gcov coverage instrumentation for tests" OFF)
if(ENABLE_COVERAGE)
    target_compile_options(aomonitor_tests PRIVATE --coverage -fno-inline -fno-inline-small-functions)
    target_link_options(aomonitor_tests PRIVATE --coverage)
endif()

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
