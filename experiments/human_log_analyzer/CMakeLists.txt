cmake_minimum_required(VERSION 3.20)
project(human_log_analyzer CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(human_log_analyzer main.cpp)
target_include_directories(human_log_analyzer PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../third_party
)
target_compile_options(human_log_analyzer PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
