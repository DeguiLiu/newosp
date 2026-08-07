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
    test_shell_args.cpp
    test_shell_backend.cpp
    test_static_node.cpp
    test_process.cpp
    test_system_monitor.cpp
    test_async_log.cpp
    test_data_dispatcher.cpp
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

# --- lwIP backend tests (optional; link an external lwIP library) ---
# OSP_LWIP_INCLUDE_DIR: semicolon-separated lwIP include dirs, e.g.
#   lwip/src/include;lwip/contrib/ports/unix/port/include;lwip/contrib/ports/unix/lib;lwip/contrib
# OSP_LWIP_LIBRARY: path to the compiled lwIP library (e.g. unixsim liblwip).
# The library must be built with LWIP_COMPAT_SOCKETS=0 and LWIP_NETIF_LOOPBACK=1.
option(OSP_WITH_LWIP "Build lwIP backend tests against an external lwIP library" OFF)
set(OSP_LWIP_INCLUDE_DIR "" CACHE PATH "lwIP include directories (semicolon-separated)")
set(OSP_LWIP_LIBRARY "" CACHE FILEPATH "Path to the lwIP library (e.g. unixsim liblwip)")

if(OSP_WITH_LWIP)
    if(NOT OSP_LWIP_INCLUDE_DIR OR NOT OSP_LWIP_LIBRARY)
        message(FATAL_ERROR "OSP_WITH_LWIP requires OSP_LWIP_INCLUDE_DIR and OSP_LWIP_LIBRARY")
    endif()
    add_executable(osp_lwip_tests
        test_lwip_main.cpp
        test_lwip_backend.cpp
    )
    target_link_libraries(osp_lwip_tests Catch2::Catch2 osp Threads::Threads)
    target_include_directories(osp_lwip_tests PRIVATE ${OSP_LWIP_INCLUDE_DIR})
    target_compile_definitions(osp_lwip_tests PRIVATE
        OSP_NET_BACKEND=1
        LWIP_NETIF_LOOPBACK=1
        LWIP_COMPAT_SOCKETS=0
    )
    get_filename_component(OSP_LWIP_LIB_DIR "${OSP_LWIP_LIBRARY}" DIRECTORY)
    target_link_options(osp_lwip_tests PRIVATE "-Wl,-rpath,${OSP_LWIP_LIB_DIR}")
    target_link_libraries(osp_lwip_tests ${OSP_LWIP_LIBRARY})
    catch_discover_tests(osp_lwip_tests
        PROPERTIES SKIP_RETURN_CODE 4
    )
endif()
