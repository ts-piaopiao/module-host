cmake_minimum_required(VERSION 3.20)

project(ScriptReplay CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

get_filename_component(PROJECT_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)

add_executable(script_replay
    main.cpp
    ${PROJECT_ROOT}/src/core/script/cpp_script.cpp
    ${PROJECT_ROOT}/src/core/script/script_config.cpp
    ${PROJECT_ROOT}/src/core/script/script_geometry.cpp
)

set_target_properties(script_replay PROPERTIES OUTPUT_NAME script_replay)

target_include_directories(script_replay PRIVATE
    ${PROJECT_ROOT}
    ${PROJECT_ROOT}/src/core/script
    ${PROJECT_ROOT}/third_party
)

target_compile_options(script_replay PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
target_link_libraries(script_replay PRIVATE ws2_32)
