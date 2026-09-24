cmake_minimum_required(VERSION 3.20)

project(ModuleHost C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(BUILD_STAGE2_PLUGINS "Build stage2 plugins" OFF)

set(ONNXRUNTIME_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/third_party/onnxruntime")

add_executable(module_host_core src/core/main.cpp src/core/config.cpp src/core/remote_server.cpp src/core/jpeg_encoder.cpp src/core/script/script_host.cpp src/core/script/cpp_script.cpp src/core/recorder.cpp)
set_target_properties(module_host_core PROPERTIES OUTPUT_NAME core)
target_include_directories(module_host_core PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
target_compile_options(module_host_core PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
target_link_libraries(module_host_core PRIVATE ws2_32 windowscodecs shlwapi ole32)
if(BUILD_STAGE2_PLUGINS)
    target_compile_definitions(module_host_core PRIVATE BUILD_STAGE2_PLUGINS=1)
endif()
add_custom_command(TARGET module_host_core POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${ONNXRUNTIME_ROOT}/bin/onnxruntime.dll
        ${ONNXRUNTIME_ROOT}/bin/onnxruntime_providers_shared.dll
        $<TARGET_FILE_DIR:module_host_core>
)

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

if(BUILD_STAGE2_PLUGINS)
    add_library(stage2_normal_capture SHARED src/normal/normal_capture.cpp)
    target_compile_definitions(stage2_normal_capture PRIVATE CORE_BUILDING_PLUGIN)
    target_include_directories(stage2_normal_capture PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_options(stage2_normal_capture PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
    set_target_properties(stage2_normal_capture PROPERTIES
        OUTPUT_NAME capture_plugin
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/$<CONFIG>/plugins
    )

    add_library(stage2_normal_policy SHARED src/normal/normal_policy.cpp)
    target_compile_definitions(stage2_normal_policy PRIVATE CORE_BUILDING_PLUGIN)
    target_include_directories(stage2_normal_policy PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_options(stage2_normal_policy PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
    set_target_properties(stage2_normal_policy PROPERTIES
        OUTPUT_NAME policy_plugin
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/$<CONFIG>/plugins
    )

    add_library(stage2_normal_input SHARED src/normal/normal_input.cpp)
    target_compile_definitions(stage2_normal_input PRIVATE CORE_BUILDING_PLUGIN)
    target_include_directories(stage2_normal_input PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_options(stage2_normal_input PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
    set_target_properties(stage2_normal_input PROPERTIES
        OUTPUT_NAME input_plugin
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/$<CONFIG>/plugins
    )

    add_library(rt_capture_fail SHARED src/runtime_errors/capture_fail.cpp)
    target_compile_definitions(rt_capture_fail PRIVATE CORE_BUILDING_PLUGIN)
    target_include_directories(rt_capture_fail PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_options(rt_capture_fail PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
    set_target_properties(rt_capture_fail PROPERTIES
        OUTPUT_NAME capture_plugin
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/$<CONFIG>/runtime_errors/capture_fail
    )

    add_library(rt_decide_fail SHARED src/runtime_errors/decide_fail.cpp)
    target_compile_definitions(rt_decide_fail PRIVATE CORE_BUILDING_PLUGIN)
    target_include_directories(rt_decide_fail PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_options(rt_decide_fail PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
    set_target_properties(rt_decide_fail PROPERTIES
        OUTPUT_NAME policy_plugin
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/$<CONFIG>/runtime_errors/decide_fail
    )

    add_library(rt_decide_over_count SHARED src/runtime_errors/decide_over_count.cpp)
    target_compile_definitions(rt_decide_over_count PRIVATE CORE_BUILDING_PLUGIN)
    target_include_directories(rt_decide_over_count PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_options(rt_decide_over_count PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
    set_target_properties(rt_decide_over_count PROPERTIES
        OUTPUT_NAME policy_plugin
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/$<CONFIG>/runtime_errors/decide_over_count
    )

    add_library(rt_decide_empty SHARED src/runtime_errors/decide_empty.cpp)
    target_compile_definitions(rt_decide_empty PRIVATE CORE_BUILDING_PLUGIN)
    target_include_directories(rt_decide_empty PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_options(rt_decide_empty PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
    set_target_properties(rt_decide_empty PROPERTIES
        OUTPUT_NAME policy_plugin
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/$<CONFIG>/runtime_errors/decide_empty
    )

    add_library(rt_execute_fail SHARED src/runtime_errors/execute_fail.cpp)
    target_compile_definitions(rt_execute_fail PRIVATE CORE_BUILDING_PLUGIN)
    target_include_directories(rt_execute_fail PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_options(rt_execute_fail PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
    set_target_properties(rt_execute_fail PROPERTIES
        OUTPUT_NAME input_plugin
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/$<CONFIG>/runtime_errors/execute_fail
    )

    add_library(stage2_real_capture SHARED src/real/real_capture.cpp)
    target_compile_definitions(stage2_real_capture PRIVATE CORE_BUILDING_PLUGIN)
    target_include_directories(stage2_real_capture PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_options(stage2_real_capture PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
    set_target_properties(stage2_real_capture PROPERTIES
        OUTPUT_NAME capture_plugin
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/$<CONFIG>/plugins_real
    )
    target_link_libraries(stage2_real_capture PRIVATE
        mfplat.lib mf.lib mfreadwrite.lib mfuuid.lib ole32.lib
    )

    add_library(stage2_real_input SHARED src/real/real_input.cpp)
    target_compile_definitions(stage2_real_input PRIVATE CORE_BUILDING_PLUGIN)
    target_include_directories(stage2_real_input PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_options(stage2_real_input PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
    set_target_properties(stage2_real_input PROPERTIES
        OUTPUT_NAME input_plugin
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/$<CONFIG>/plugins_real
    )

    add_library(stage2_test_policy SHARED src/real/test_policy.cpp)
    target_compile_definitions(stage2_test_policy PRIVATE CORE_BUILDING_PLUGIN)
    target_include_directories(stage2_test_policy PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_options(stage2_test_policy PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
    set_target_properties(stage2_test_policy PROPERTIES
        OUTPUT_NAME policy_plugin
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/$<CONFIG>/plugins_real
    )

    add_library(stage2_yolo_policy SHARED src/real/yolo_policy.cpp)
    target_compile_definitions(stage2_yolo_policy PRIVATE CORE_BUILDING_PLUGIN)
    target_include_directories(stage2_yolo_policy PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${ONNXRUNTIME_ROOT}/include
    )
    target_compile_options(stage2_yolo_policy PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
    target_link_libraries(stage2_yolo_policy PRIVATE
        ${ONNXRUNTIME_ROOT}/lib/onnxruntime.lib
    )
    set_target_properties(stage2_yolo_policy PROPERTIES
        OUTPUT_NAME policy_plugin
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/$<CONFIG>/plugins_yolo
    )
endif()
