# --------------------------------------------------------------------------
# Data Dispatcher demo
#   - InProc pipeline (intra-process DAG)
#   - ShmStore + ShmNotify (cross-process zero-copy)
#   - Unified API demo (Store-agnostic)
# --------------------------------------------------------------------------

find_package(Threads REQUIRED)

function(add_dd_example target source)
    add_executable(${target} ${source})
    target_link_libraries(${target} PRIVATE osp Threads::Threads)
    target_compile_features(${target} PRIVATE cxx_std_17)
    # Link rt for shm_open/shm_unlink on older glibc
    target_link_libraries(${target} PRIVATE rt)
endfunction()

# Intra-process pipeline (InProcStore)
add_dd_example(osp_dd_pipeline           pipeline_demo.cpp)

# Unified API demo (Store-agnostic template)
add_dd_example(osp_dd_unified            unified_api_demo.cpp)

# Cross-process DataDispatcher<ShmStore, ShmNotify>
add_dd_example(osp_dd_producer           producer.cpp)
add_dd_example(osp_dd_consumer_logging   consumer_logging.cpp)
add_dd_example(osp_dd_consumer_fusion    consumer_fusion.cpp)
add_dd_example(osp_dd_monitor            monitor.cpp)
add_dd_example(osp_dd_launcher           launcher.cpp)
