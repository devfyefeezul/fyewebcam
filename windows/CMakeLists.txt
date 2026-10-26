cmake_minimum_required(VERSION 3.20)
project(fyewebcam LANGUAGES CXX RC)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(fyewebcam WIN32
    src/main.cpp
    src/stream_client.cpp
    src/h264_decoder.cpp
    src/adb.cpp
    src/vcam.cpp
    src/app.rc
)

target_compile_definitions(fyewebcam PRIVATE
    UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00 WINVER=0x0A00)

target_link_libraries(fyewebcam PRIVATE
    ws2_32 mfplat mfuuid ole32 comctl32 uxtheme dwmapi gdi32 user32 shell32 advapi32)

# Virtual camera media source, loaded by the Windows Camera Frame Server. Needs the Windows 11 SDK
# headers, so it is built with MSVC only.
if(MSVC)
    add_library(fyewebcam_vcam SHARED vcam/dllmain.cpp vcam/media_source.cpp vcam/vcam.def)
    target_compile_definitions(fyewebcam_vcam PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_compile_options(fyewebcam_vcam PRIVATE /W4 /utf-8)
    set_property(TARGET fyewebcam_vcam PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    target_link_libraries(fyewebcam_vcam PRIVATE mfplat mfuuid ole32 advapi32)
    add_dependencies(fyewebcam fyewebcam_vcam)
endif()

# Background blur runs a person-segmentation model with ONNX Runtime, taken from its NuGet package at
# configure time. onnxruntime.dll is copied next to the exe and loaded at run time.
if(MSVC)
    set(ORT_VERSION 1.29.0)
    set(ORT_SHA256 4557021fec4da3924e2e12ff44b1cc50b176de0d2c5c7daa89ab11c208b3a454)
    set(ORT_DIR ${CMAKE_BINARY_DIR}/onnxruntime-${ORT_VERSION})
    if(NOT EXISTS ${ORT_DIR}/build/native/include/onnxruntime_c_api.h)
        set(ORT_PACKAGE ${CMAKE_BINARY_DIR}/onnxruntime-${ORT_VERSION}.zip)
        file(DOWNLOAD
            https://api.nuget.org/v3-flatcontainer/microsoft.ml.onnxruntime/${ORT_VERSION}/microsoft.ml.onnxruntime.${ORT_VERSION}.nupkg
            ${ORT_PACKAGE} EXPECTED_HASH SHA256=${ORT_SHA256})
        file(ARCHIVE_EXTRACT INPUT ${ORT_PACKAGE} DESTINATION ${ORT_DIR}
            PATTERNS "build/native/include/*" "runtimes/win-x64/native/onnxruntime.dll")
        file(REMOVE ${ORT_PACKAGE})
    endif()
    target_sources(fyewebcam PRIVATE src/background_blur.cpp)
    target_include_directories(fyewebcam PRIVATE ${ORT_DIR}/build/native/include)
    target_compile_definitions(fyewebcam PRIVATE FYE_HAVE_BLUR)
    add_custom_command(TARGET fyewebcam POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${ORT_DIR}/runtimes/win-x64/native/onnxruntime.dll
                $<TARGET_FILE_DIR:fyewebcam>)
endif()

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
