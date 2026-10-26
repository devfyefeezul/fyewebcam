cmake_minimum_required(VERSION 3.20)
project(fyewebcam LANGUAGES CXX RC)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(fyewebcam WIN32
    src/main.cpp
    src/stream_client.cpp
    src/h264_decoder.cpp
    src/adb.cpp
    src/app.rc
)

target_compile_definitions(fyewebcam PRIVATE
    UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00 WINVER=0x0A00)

target_link_libraries(fyewebcam PRIVATE
    ws2_32 mfplat mfuuid ole32 comctl32 uxtheme dwmapi gdi32 user32 shell32 advapi32)

if(MSVC)
    target_compile_options(fyewebcam PRIVATE /W4 /utf-8)
    # Ship a single exe without the VC++ runtime installer.
    set_property(TARGET fyewebcam PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    # app.rc embeds our own manifest (common controls v6, per-monitor DPI).
    target_link_options(fyewebcam PRIVATE /MANIFEST:NO)
else()
    target_compile_options(fyewebcam PRIVATE -Wall -Wextra -Wno-missing-field-initializers)
    target_link_options(fyewebcam PRIVATE -static -municode)
endif()

enable_testing()
add_executable(protocol_test tests/protocol_test.cpp)
add_test(NAME protocol_test COMMAND protocol_test)
