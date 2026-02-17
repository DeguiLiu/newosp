# --------------------------------------------------------------------------
# Data Visitor Dispatcher demo (SPMC shared memory)
# --------------------------------------------------------------------------

find_package(Threads REQUIRED)

function(add_dvd_example target source)
    add_executable(${target} ${source})
    target_link_libraries(${target} PRIVATE osp Threads::Threads)
    target_compile_features(${target} PRIVATE cxx_std_17)
    # Link rt for shm_open/shm_unlink on older glibc
    target_link_libraries(${target} PRIVATE rt)
endfunction()

add_dvd_example(osp_dvd_producer         producer.cpp)
add_dvd_example(osp_dvd_visitor_logging  visitor_logging.cpp)
add_dvd_example(osp_dvd_visitor_fusion   visitor_fusion.cpp)
add_dvd_example(osp_dvd_monitor          monitor.cpp)
add_dvd_example(osp_dvd_launcher         launcher.cpp)
