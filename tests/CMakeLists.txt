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

add_executable(osp_tests
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
    test_socket.cpp
    test_connection.cpp
    test_io_poller.cpp
    test_transport.cpp
    test_semaphore.cpp
    test_shm_transport.cpp
    test_data_fusion.cpp
    test_discovery.cpp
    test_service.cpp
    test_node_manager.cpp
    test_node_manager_hsm.cpp
    test_integration.cpp
    test_serial_transport.cpp
    test_qos.cpp
    test_app.cpp
    test_post.cpp
    test_transport_factory.cpp
)
target_link_libraries(osp_tests Catch2::Catch2WithMain osp Threads::Threads util)

if(OSP_WITH_SOCKPP)
    target_sources(osp_tests PRIVATE test_net.cpp)
    target_link_libraries(osp_tests sockpp-static)
endif()

# Apply -fno-exceptions -fno-rtti only to osp_tests, not to dependencies
if(OSP_NO_EXCEPTIONS)
    target_compile_options(osp_tests PRIVATE -fno-exceptions -fno-rtti)
endif()

catch_discover_tests(osp_tests)
