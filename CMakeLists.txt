cmake_minimum_required(VERSION 3.20)

project(RemoteMirror LANGUAGES CXX)

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
)

add_executable(RemoteMirrorServer WIN32
    server/main.cpp
)

target_link_libraries(RemoteMirrorServer PRIVATE
    RemoteMirrorCommon
    user32
    gdi32
    windowscodecs
    ole32
)

add_executable(RemoteMirrorViewer WIN32
    viewer/main.cpp
)

target_link_libraries(RemoteMirrorViewer PRIVATE
    RemoteMirrorCommon
    user32
    gdi32
    windowscodecs
    ole32
)