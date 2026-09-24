cmake_minimum_required(VERSION 3.20)
project(RemoteClient CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(remote_client main.cpp)

target_compile_options(remote_client PRIVATE /utf-8 /EHsc)

target_link_libraries(remote_client PRIVATE
    ws2_32.lib
    windowscodecs.lib
    shlwapi.lib
    ole32.lib
    user32.lib
    gdi32.lib
)
