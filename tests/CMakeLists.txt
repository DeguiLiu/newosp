include(FetchContent)

set(CATCH2_VERSION v3.5.2)
FetchContent_Declare(
    Catch2
    URL https://ghfast.top/https://github.com/catchorg/Catch2/archive/refs/tags/${CATCH2_VERSION}.zip
    DOWNLOAD_EXTRACT_TIMESTAMP ON
)
FetchContent_MakeAvailable(Catch2)
include(Catch)

find_package(Threads REQUIRED)

# Core test sources (no sockpp dependency)
set(OSP_TEST_CORE_SOURCES
    test_platform.cpp
    test_vocabulary.cpp
    test_config.cpp
    test_log.cpp
    test_timer.cpp
    test_shell.cpp
    test_mem_pool.cpp
    test_shutdown.cpp
    test_bus.cpp
    test_node.cpp
    test_lifecycle_node.cpp
    test_worker_pool.cpp
    test_executor.cpp
    test_hsm.cpp
    test_bt.cpp
    test_io_poller.cpp
    test_semaphore.cpp
    test_shm_transport.cpp
    test_data_fusion.cpp
    test_node_manager_hsm.cpp
    test_service_hsm.cpp
    test_discovery_hsm.cpp
    test_serial_transport.cpp
    test_spsc_ringbuffer.cpp
    test_qos.cpp
    test_app.cpp
    test_post.cpp
    test_watchdog.cpp
    test_fault_collector.cpp
    test_shell_commands.cpp
)

# Inicpp test sources (embedded ini library tests)
set(OSP_TEST_INICPP_SOURCES
    inicpp/test_convert.cpp
    inicpp/test_edge_cases.cpp
    inicpp/test_file_io.cpp
    inicpp/test_inifield.cpp
    inicpp/test_inifile.cpp
    inicpp/test_inisection.cpp
)

# Network test sources (depend on sockpp, incompatible with -fno-exceptions)
set(OSP_TEST_NET_SOURCES
    test_socket.cpp
    test_connection.cpp
    test_transport.cpp
    test_discovery.cpp
    test_service.cpp
    test_node_manager.cpp
    test_integration.cpp
    test_transport_factory.cpp
)

if(OSP_NO_EXCEPTIONS)
    # -fno-exceptions mode: only core tests (sockpp uses throw internally)
    add_executable(osp_tests ${OSP_TEST_CORE_SOURCES} ${OSP_TEST_INICPP_SOURCES})
    target_link_libraries(osp_tests Catch2::Catch2WithMain osp Threads::Threads util)
    target_compile_options(osp_tests PRIVATE -fno-exceptions -fno-rtti)
else()
    add_executable(osp_tests ${OSP_TEST_CORE_SOURCES} ${OSP_TEST_INICPP_SOURCES} ${OSP_TEST_NET_SOURCES})
    target_link_libraries(osp_tests Catch2::Catch2WithMain osp Threads::Threads util)
    if(OSP_WITH_SOCKPP)
        target_sources(osp_tests PRIVATE test_net.cpp)
        target_link_libraries(osp_tests sockpp-static)
    endif()
endif()

catch_discover_tests(osp_tests)
