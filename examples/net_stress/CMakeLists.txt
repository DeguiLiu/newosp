# --------------------------------------------------------------------------
# net_stress demo executables
# --------------------------------------------------------------------------

find_package(Threads REQUIRED)

function(add_net_stress_example target source)
    add_executable(${target} ${source})
    target_link_libraries(${target} PRIVATE osp Threads::Threads)
    target_compile_features(${target} PRIVATE cxx_std_17)
endfunction()

add_net_stress_example(osp_net_stress_server   server.cpp)
add_net_stress_example(osp_net_stress_client   client.cpp)
add_net_stress_example(osp_net_stress_monitor  monitor.cpp)
add_net_stress_example(osp_net_stress_launcher launcher.cpp)
