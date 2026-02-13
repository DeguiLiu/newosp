# --------------------------------------------------------------------------
# SHM IPC demo executables
# --------------------------------------------------------------------------

find_package(Threads REQUIRED)

function(add_shm_ipc_example target source)
    add_executable(${target} ${source})
    target_link_libraries(${target} PRIVATE osp Threads::Threads)
    target_compile_features(${target} PRIVATE cxx_std_17)
    # Link rt for shm_open/shm_unlink on older glibc
    target_link_libraries(${target} PRIVATE rt)
endfunction()

add_shm_ipc_example(osp_shm_producer  shm_producer.cpp)
add_shm_ipc_example(osp_shm_consumer  shm_consumer.cpp)
add_shm_ipc_example(osp_shm_monitor   shm_monitor.cpp)
