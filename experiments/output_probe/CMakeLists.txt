cmake_minimum_required(VERSION 3.20)
project(output_probe CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(output_probe main.cpp)
target_include_directories(output_probe PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src/core
    ${CMAKE_CURRENT_SOURCE_DIR}/../..
)
target_sources(output_probe PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src/core/output_manager.cpp
)
target_compile_options(output_probe PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
target_link_libraries(output_probe PRIVATE ws2_32)
