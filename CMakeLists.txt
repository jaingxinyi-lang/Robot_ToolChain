cmake_minimum_required(VERSION 3.14)
project(Robot_ToolChain LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

# ── Windows/MinGW：添加 MSYS2 库搜索路径 ─────────────────────────
list(APPEND CMAKE_PREFIX_PATH "D:/msys2/mingw64")

find_package(Qt5 5.15 REQUIRED COMPONENTS Core Widgets SerialPort Network)
find_package(libssh REQUIRED)

add_executable(Robot_ToolChain
    src/main.cpp
    src/mainwindow.cpp
    src/mainwindow.h
    src/mainwindow.ui
    src/config_dialog.cpp
    src/config_dialog.h
    src/script_panel.cpp
    src/script_panel.h
    src/script_runner.cpp
    src/script_runner.h
    src/process_watcher.cpp
    src/process_watcher.h
    src/logger.cpp
    src/logger.h
    src/communication_manager.cpp
    src/communication_manager.h
    src/serial_manager.cpp
    src/serial_manager.h
    src/file_deployer.cpp
    src/file_deployer.h
    src/test_result_card.cpp
    src/test_result_card.h
    src/test_result_manager.cpp
    src/test_result_manager.h
    src/ssh_manager.cpp
    src/ssh_manager.h
    src/ssh_file_browser.cpp
    src/ssh_file_browser.h
    src/mjpeg_stream_receiver.cpp
    src/mjpeg_stream_receiver.h
    src/video_view.cpp
    src/video_view.h
    src/resources/app.rc
    src/resources/icon.qrc
)

set_target_properties(Robot_ToolChain PROPERTIES
    WIN32_EXECUTABLE TRUE
    MACOSX_BUNDLE TRUE
)

target_link_libraries(Robot_ToolChain
    PRIVATE
        Qt5::Core
        Qt5::Widgets
        Qt5::SerialPort
        Qt5::Network
        ssh
)

# 设置目标的编译选项
target_compile_options(Robot_ToolChain PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic>
    $<$<CXX_COMPILER_ID:MSVC>:/W4>
)

install(TARGETS Robot_ToolChain
    BUNDLE  DESTINATION .
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
)
