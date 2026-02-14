# --------------------------------------------------------------------------
# osp examples
# --------------------------------------------------------------------------

find_package(Threads REQUIRED)

# Helper: add an example executable that links to osp + Threads::Threads
function(add_osp_example target source)
    add_executable(${target} ${source})
    target_link_libraries(${target} PRIVATE osp Threads::Threads)
    target_compile_features(${target} PRIVATE cxx_std_17)
endfunction()

# --- demo executables -----------------------------------------------------
add_osp_example(osp_basic_demo              basic_demo.cpp)
add_osp_example(osp_protocol_demo           streaming_protocol/main.cpp)
add_osp_example(osp_client_demo             client_gateway/main.cpp)
add_osp_example(osp_priority_demo           priority_demo.cpp)
add_osp_example(osp_benchmark               benchmark.cpp)
add_osp_example(osp_serial_demo              serial_demo.cpp)
add_osp_example(osp_serial_ota_demo          serial_ota/main.cpp)
add_osp_example(osp_bt_patrol_demo          bt_patrol_demo.cpp)
add_osp_example(osp_node_manager_hsm_demo   node_manager_hsm_demo.cpp)
add_osp_example(osp_realtime_demo           realtime_executor_demo.cpp)
add_osp_example(osp_hsm_bt_combo_demo       hsm_bt_combo_demo.cpp)
add_osp_example(osp_hsm_protocol_demo       hsm_protocol_demo.cpp)

# Benchmark builds with -O3 regardless of the global build type
target_compile_options(osp_benchmark PRIVATE -O3)

# --- codegen demo (requires OSP_CODEGEN=ON) --------------------------------
if(OSP_CODEGEN)
    add_osp_example(osp_codegen_demo codegen_demo.cpp)
    add_dependencies(osp_codegen_demo osp_codegen)
endif()

# --- SHM IPC demo (subdirectory) -------------------------------------------
add_subdirectory(shm_ipc)

# --- Net stress demo (subdirectory) ----------------------------------------
add_subdirectory(net_stress)
