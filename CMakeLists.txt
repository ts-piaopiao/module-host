cmake_minimum_required(VERSION 3.20)

project(ModuleHost C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(BUILD_STAGE2_PLUGINS "Build stage2 plugins" OFF)

add_executable(module_host_core src/core/main.cpp)
set_target_properties(module_host_core PROPERTIES OUTPUT_NAME core)
target_include_directories(module_host_core PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(module_host_core PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)

add_library(stage1_stub_capture SHARED src/stubs/stub_capture.cpp)
target_compile_definitions(stage1_stub_capture PRIVATE CORE_BUILDING_PLUGIN)
target_include_directories(stage1_stub_capture PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(stage1_stub_capture PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
set_target_properties(stage1_stub_capture PROPERTIES
    OUTPUT_NAME capture_plugin
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/$<CONFIG>/stubs
)

add_library(stage1_stub_policy SHARED src/stubs/stub_policy.cpp)
target_compile_definitions(stage1_stub_policy PRIVATE CORE_BUILDING_PLUGIN)
target_include_directories(stage1_stub_policy PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(stage1_stub_policy PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
set_target_properties(stage1_stub_policy PROPERTIES
    OUTPUT_NAME policy_plugin
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/$<CONFIG>/stubs
)

add_library(stage1_stub_input SHARED src/stubs/stub_input.cpp)
target_compile_definitions(stage1_stub_input PRIVATE CORE_BUILDING_PLUGIN)
target_include_directories(stage1_stub_input PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(stage1_stub_input PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
set_target_properties(stage1_stub_input PROPERTIES
    OUTPUT_NAME input_plugin
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/$<CONFIG>/stubs
)

add_library(bad_missing_symbol_capture SHARED src/bad_plugins/missing_symbol_capture.cpp)
target_compile_definitions(bad_missing_symbol_capture PRIVATE CORE_BUILDING_PLUGIN)
target_include_directories(bad_missing_symbol_capture PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(bad_missing_symbol_capture PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
set_target_properties(bad_missing_symbol_capture PROPERTIES
    OUTPUT_NAME capture_plugin
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/$<CONFIG>/bad_plugins/missing_symbol_capture
)

add_library(bad_missing_symbol_policy SHARED src/bad_plugins/missing_symbol_policy.cpp)
target_compile_definitions(bad_missing_symbol_policy PRIVATE CORE_BUILDING_PLUGIN)
target_include_directories(bad_missing_symbol_policy PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(bad_missing_symbol_policy PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
set_target_properties(bad_missing_symbol_policy PROPERTIES
    OUTPUT_NAME policy_plugin
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/$<CONFIG>/bad_plugins/missing_symbol_policy
)

add_library(bad_missing_symbol_input SHARED src/bad_plugins/missing_symbol_input.cpp)
target_compile_definitions(bad_missing_symbol_input PRIVATE CORE_BUILDING_PLUGIN)
target_include_directories(bad_missing_symbol_input PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(bad_missing_symbol_input PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
set_target_properties(bad_missing_symbol_input PROPERTIES
    OUTPUT_NAME input_plugin
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/$<CONFIG>/bad_plugins/missing_symbol_input
)

add_library(bad_abi_mismatch_capture SHARED src/bad_plugins/abi_mismatch_capture.cpp)
target_compile_definitions(bad_abi_mismatch_capture PRIVATE CORE_BUILDING_PLUGIN)
target_include_directories(bad_abi_mismatch_capture PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(bad_abi_mismatch_capture PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
set_target_properties(bad_abi_mismatch_capture PROPERTIES
    OUTPUT_NAME capture_plugin
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/$<CONFIG>/bad_plugins/abi_mismatch_capture
)

add_library(bad_meta_null_capture SHARED src/bad_plugins/meta_null_capture.cpp)
target_compile_definitions(bad_meta_null_capture PRIVATE CORE_BUILDING_PLUGIN)
target_include_directories(bad_meta_null_capture PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(bad_meta_null_capture PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
set_target_properties(bad_meta_null_capture PROPERTIES
    OUTPUT_NAME capture_plugin
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/$<CONFIG>/bad_plugins/meta_null_capture
)

add_library(bad_meta_seg_count_capture SHARED src/bad_plugins/meta_seg_count_capture.cpp)
target_compile_definitions(bad_meta_seg_count_capture PRIVATE CORE_BUILDING_PLUGIN)
target_include_directories(bad_meta_seg_count_capture PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(bad_meta_seg_count_capture PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
set_target_properties(bad_meta_seg_count_capture PROPERTIES
    OUTPUT_NAME capture_plugin
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/$<CONFIG>/bad_plugins/meta_seg_count_capture
)

add_library(bad_meta_abi_not_number_capture SHARED src/bad_plugins/meta_abi_not_number_capture.cpp)
target_compile_definitions(bad_meta_abi_not_number_capture PRIVATE CORE_BUILDING_PLUGIN)
target_include_directories(bad_meta_abi_not_number_capture PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(bad_meta_abi_not_number_capture PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
set_target_properties(bad_meta_abi_not_number_capture PROPERTIES
    OUTPUT_NAME capture_plugin
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/$<CONFIG>/bad_plugins/meta_abi_not_number_capture
)

add_library(bad_meta_kind_mismatch_capture SHARED src/bad_plugins/meta_kind_mismatch_capture.cpp)
target_compile_definitions(bad_meta_kind_mismatch_capture PRIVATE CORE_BUILDING_PLUGIN)
target_include_directories(bad_meta_kind_mismatch_capture PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(bad_meta_kind_mismatch_capture PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
set_target_properties(bad_meta_kind_mismatch_capture PROPERTIES
    OUTPUT_NAME capture_plugin
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/$<CONFIG>/bad_plugins/meta_kind_mismatch_capture
)

add_library(bad_init_fail_capture SHARED src/bad_plugins/init_fail_capture.cpp)
target_compile_definitions(bad_init_fail_capture PRIVATE CORE_BUILDING_PLUGIN)
target_include_directories(bad_init_fail_capture PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(bad_init_fail_capture PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
set_target_properties(bad_init_fail_capture PROPERTIES
    OUTPUT_NAME capture_plugin
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/$<CONFIG>/bad_plugins/init_fail_capture
)
