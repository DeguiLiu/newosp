include(FetchContent)

set(CATCH2_VERSION v3.5.2)
FetchContent_Declare(
    Catch2
    URL ${OSP_GITHUB_MIRROR}https://github.com/catchorg/Catch2/archive/refs/tags/${CATCH2_VERSION}.zip
    DOWNLOAD_EXTRACT_TIMESTAMP ON
)
FetchContent_MakeAvailable(Catch2)
include(Catch)

find_package(Threads REQUIRED)

# Core test sources (no network, no sockpp dependency)
set(OSP_TEST_CORE_SOURCES
    test_platform.cpp
    test_vocabulary.cpp
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
    test_semaphore.cpp
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
    test_shell_backend.cpp
    test_static_node.cpp
    test_process.cpp
    test_system_monitor.cpp
    test_async_log.cpp
)

# Network test sources (require BSD sockets; excluded when OSP_WITH_NETWORK=OFF)
set(OSP_TEST_NETWORK_SOURCES
    test_io_poller.cpp
    test_shm_transport.cpp
)

# Sockpp-dependent test sources (also require exceptions; excluded when
# OSP_NO_EXCEPTIONS=ON or OSP_WITH_NETWORK=OFF)
set(OSP_TEST_SOCKPP_SOURCES
    test_config.cpp
    test_inicpp.cpp
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
    # -fno-exceptions mode: core tests only (sockpp uses throw internally)
    add_executable(osp_tests ${OSP_TEST_CORE_SOURCES})
    if(OSP_WITH_NETWORK)
        target_sources(osp_tests PRIVATE ${OSP_TEST_NETWORK_SOURCES})
    endif()
    target_link_libraries(osp_tests Catch2::Catch2WithMain osp Threads::Threads util)
    target_compile_options(osp_tests PRIVATE -fno-exceptions -fno-rtti)
elseif(NOT OSP_WITH_NETWORK)
    # No-network mode: core tests only (network headers are disabled)
    add_executable(osp_tests ${OSP_TEST_CORE_SOURCES})
    target_link_libraries(osp_tests Catch2::Catch2WithMain osp Threads::Threads util)
else()
    # Normal mode: all tests
    add_executable(osp_tests
        ${OSP_TEST_CORE_SOURCES}
        ${OSP_TEST_NETWORK_SOURCES}
        ${OSP_TEST_SOCKPP_SOURCES}
    )
    target_link_libraries(osp_tests Catch2::Catch2WithMain osp Threads::Threads util)
    if(OSP_WITH_SOCKPP)
        target_sources(osp_tests PRIVATE test_net.cpp)
        target_link_libraries(osp_tests sockpp-static)
    endif()
endif()

# Catch2 SKIP() returns exit code 4; tell CTest to treat it as SKIP, not FAIL
catch_discover_tests(osp_tests
    PROPERTIES SKIP_RETURN_CODE 4
)
