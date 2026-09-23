cmake_minimum_required(VERSION 3.20)

project(CaptureProbe CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(capture_probe main.cpp)
set_target_properties(capture_probe PROPERTIES OUTPUT_NAME capture_probe)
target_compile_options(capture_probe PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
target_link_libraries(capture_probe PRIVATE
    mfplat.lib
    mf.lib
    mfreadwrite.lib
    mfuuid.lib
    ole32.lib
)
