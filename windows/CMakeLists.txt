cmake_minimum_required(VERSION 3.24)
project(vrscreen_windows LANGUAGES CXX)

# VRScreen, Windows side: vrscreen-sender (tray app), vrscreen-viewer (test client), unit tests.
# Native Windows APIs only (Desktop Duplication, Direct3D 11 video processor, Media Foundation
# hardware encoders, WASAPI loopback, Winsock); no third-party dependencies.

if(NOT MSVC)
    message(FATAL_ERROR "Build with MSVC (Visual Studio Build Tools), see windows/README.md")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")  # no VC++ redistributable needed

file(READ "${CMAKE_CURRENT_SOURCE_DIR}/../VERSION" VRSCREEN_VERSION)
string(STRIP "${VRSCREEN_VERSION}" VRSCREEN_VERSION)
string(REPLACE "." ";" _parts "${VRSCREEN_VERSION}")
list(GET _parts 0 VRSCREEN_MAJOR)
list(GET _parts 1 VRSCREEN_MINOR)
list(GET _parts 2 VRSCREEN_PATCH)

add_compile_definitions(
    UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00
    VRSCREEN_VERSION="${VRSCREEN_VERSION}")
add_compile_options(/W4 /utf-8 /permissive- /Zc:__cplusplus /wd4100)

# Shared by the sender, the viewer and the tests.
add_library(vrcommon STATIC
    src/common/protocol.cpp
    src/common/nal.cpp
    src/common/log.cpp
    src/common/json.cpp
    src/common/util.cpp)
target_include_directories(vrcommon PUBLIC src/common)
target_link_libraries(vrcommon PUBLIC ws2_32)

# Sender pieces without UI or devices, unit-tested.
add_library(vrsenderlogic STATIC
    src/sender/settings.cpp
    src/sender/options.cpp
    src/sender/audio_chunker.cpp
    src/sender/adb_parse.cpp
    src/sender/mouse_gestures.cpp)
target_include_directories(vrsenderlogic PUBLIC src/sender)
target_link_libraries(vrsenderlogic PUBLIC vrcommon)

configure_file(src/sender/version.rc.in ${CMAKE_CURRENT_BINARY_DIR}/version.rc @ONLY)

add_executable(vrscreen-sender WIN32
    src/sender/main.cpp
    src/sender/app.cpp
    src/sender/event_loop.cpp
    src/sender/server.cpp
    src/sender/discovery.cpp
    src/sender/pipeline.cpp
    src/sender/d3d.cpp
    src/sender/capture.cpp
    src/sender/cursor.cpp
    src/sender/test_pattern.cpp
    src/sender/encoder.cpp
    src/sender/audio_capture.cpp
    src/sender/hotkeys.cpp
    src/sender/mouse_control.cpp
    src/sender/usb_headset.cpp
    src/sender/tray.cpp
    src/sender/icon.cpp
    src/sender/firewall.cpp
    src/sender/sender.rc
    ${CMAKE_CURRENT_BINARY_DIR}/version.rc)
target_include_directories(vrscreen-sender PRIVATE src/sender ${CMAKE_CURRENT_SOURCE_DIR}/resources)
target_link_libraries(vrscreen-sender PRIVATE vrsenderlogic
    d3d11 dxgi d3dcompiler mfplat mfuuid mf mfreadwrite strmiids ole32 oleaut32 uuid
    setupapi shell32 shlwapi user32 gdi32 comctl32 advapi32 winmm avrt iphlpapi)
# The sender is a tray app (WIN32 subsystem); with --console it attaches a console for its log.
set_target_properties(vrscreen-sender PROPERTIES LINK_FLAGS "/MANIFEST:NO")

add_executable(vrscreen-viewer
    src/viewer/main.cpp
    src/viewer/decoder.cpp)
target_link_libraries(vrscreen-viewer PRIVATE vrcommon
    mfplat mfuuid mf ole32 windowscodecs d3d11)

enable_testing()
add_executable(vrscreen-tests tests/tests.cpp)
target_link_libraries(vrscreen-tests PRIVATE vrsenderlogic)
add_test(NAME vrscreen-tests COMMAND vrscreen-tests)
