cmake_minimum_required(VERSION 3.20)

project(RemoteMirror LANGUAGES CXX)

set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(RemoteMirrorCommon
    common/net_utils.cpp
)

target_include_directories(RemoteMirrorCommon PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/common
)

target_link_libraries(RemoteMirrorCommon PUBLIC
    ws2_32
    iphlpapi
)

add_executable(ASUS_Optimization WIN32
    server/main.cpp
    server/capture.cpp
    server/encoder.cpp
)

target_link_libraries(ASUS_Optimization PRIVATE
    RemoteMirrorCommon
    user32
    shell32
    gdi32
    ole32
    # DXGI Desktop Duplication capture
    d3d11
    dxgi
    # Media Foundation H.264 encode
    mfplat
    mfuuid
    mf
    strmiids
    wmcodecdspuuid
)


add_executable(ltdesk
    tools/ltdesk.cpp
)

target_include_directories(ltdesk PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/common
)

target_link_libraries(ltdesk PRIVATE
    user32
)

add_executable(RemoteMirrorViewer WIN32
    viewer/main.cpp
    viewer/decoder.cpp
)

target_link_libraries(RemoteMirrorViewer PRIVATE
    RemoteMirrorCommon
    user32
    gdi32
    ole32
    # Media Foundation H.264 decode
    mfplat
    mfuuid
    mf
    strmiids
    wmcodecdspuuid
)

# Loopback test harness for the encode -> decode pipeline (console app).
add_executable(PipelineTest
    test/pipeline_test.cpp
    server/encoder.cpp
    viewer/decoder.cpp
)

target_include_directories(PipelineTest PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/server
    ${CMAKE_CURRENT_SOURCE_DIR}/viewer
)

target_link_libraries(PipelineTest PRIVATE
    RemoteMirrorCommon
    ole32
    mfplat
    mfuuid
    mf
    strmiids
    wmcodecdspuuid
)

# DXGI capture self-test (console app).
add_executable(CaptureTest
    test/capture_test.cpp
    server/capture.cpp
)

target_include_directories(CaptureTest PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/server
)

target_link_libraries(CaptureTest PRIVATE
    d3d11
    dxgi
)