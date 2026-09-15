include(FetchContent)

set(CATCH2_VERSION v3.5.2)

# DOWNLOAD_EXTRACT_TIMESTAMP was added in CMake 3.24. On older CMake (3.22 is
# what Ubuntu 22.04 ships) ExternalProject does not recognise the keyword,
# swallows it into the URL list and configure fails with "At least one entry of
# URL is a path (invalid in a list)" — the whole test suite becomes unbuildable.
set(_osp_catch2_ts "")
if(CMAKE_VERSION VERSION_GREATER_EQUAL 3.24)
    set(_osp_catch2_ts DOWNLOAD_EXTRACT_TIMESTAMP ON)
endif()

FetchContent_Declare(
    Catch2
    URL ${OSP_GITHUB_MIRROR}https://github.com/catchorg/Catch2/archive/refs/tags/${CATCH2_VERSION}.zip
    ${_osp_catch2_ts}
)
FetchContent_MakeAvailable(Catch2)
include(Catch)

find_package(Threads REQUIRED)

# --- Header-closure smoke TU (must stay MSVC-clean) ------------------------
# Compiles every header in the Windows consumer set; an OBJECT library so it has
# no main() and its only job is to fail compilation on a non-portable include.
add_library(osp_header_smoke OBJECT msvc_smoke.cpp)
target_link_libraries(osp_header_smoke PRIVATE osp)

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
    test_hsm_table.cpp
    test_serial_ota_parser.cpp
    test_uart_isr.cpp
    test_net_stress_client_sm.cpp
    test_net_stress_ft_sm.cpp
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
    test_thread.cpp
    test_config_toml.cpp
    test_observability.cpp
)

# Windows-branch tests.  Each file guards its body with
# `#if defined(OSP_PLATFORM_WINDOWS)`, so off Windows they compile to an empty
# translation unit instead of being excluded here.
set(OSP_TEST_WINDOWS_SOURCES
    test_frame_channel.cpp
    test_io_poller_win.cpp
    test_process_win.cpp
    test_shm_transport_win.cpp
    test_shutdown_win.cpp
    test_socket_win.cpp
    test_system_monitor_win.cpp
    test_thread_win.cpp
)

# example headers under test (serial_ota parser)
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/../examples)

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
    add_executable(osp_tests ${OSP_TEST_CORE_SOURCES} ${OSP_TEST_WINDOWS_SOURCES})
    if(OSP_WITH_NETWORK)
        target_sources(osp_tests PRIVATE ${OSP_TEST_NETWORK_SOURCES})
    endif()
    target_link_libraries(osp_tests Catch2::Catch2WithMain osp Threads::Threads util)
    target_compile_options(osp_tests PRIVATE -fno-exceptions -fno-rtti)
elseif(NOT OSP_WITH_NETWORK)
    # No-network mode: core tests only (network headers are disabled)
    add_executable(osp_tests ${OSP_TEST_CORE_SOURCES} ${OSP_TEST_WINDOWS_SOURCES})
    target_link_libraries(osp_tests Catch2::Catch2WithMain osp Threads::Threads util)
else()
    # Normal mode: all tests
    add_executable(osp_tests
        ${OSP_TEST_CORE_SOURCES}
        ${OSP_TEST_WINDOWS_SOURCES}
        ${OSP_TEST_NETWORK_SOURCES}
        ${OSP_TEST_SOCKPP_SOURCES}
    )
    target_link_libraries(osp_tests Catch2::Catch2WithMain osp Threads::Threads util)
    if(OSP_WITH_SOCKPP)
        target_sources(osp_tests PRIVATE test_net.cpp)
        target_link_libraries(osp_tests sockpp-static)
    endif()
endif()

# GCC libtsan crashes under high-entropy ASLR (kernel 6.5, mmap_rnd_bits=32).
# For the gcc+TSan combo, run discovery and test execution via setarch -R
# (ASLR off). CI uses clang+TSan, which needs no such workaround.
if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU"
   AND CMAKE_CXX_FLAGS MATCHES "-fsanitize=thread")
    set(OSP_TSAN_EMULATOR "setarch;${CMAKE_HOST_SYSTEM_PROCESSOR};-R")
else()
    set(OSP_TSAN_EMULATOR "")
endif()

# Catch2 SKIP() returns exit code 4; tell CTest to treat it as SKIP, not FAIL
if(OSP_TSAN_EMULATOR)
    set_property(TARGET osp_tests PROPERTY CROSSCOMPILING_EMULATOR "${OSP_TSAN_EMULATOR}")
endif()
catch_discover_tests(osp_tests
    PROPERTIES SKIP_RETURN_CODE 4
)

# --- RT-Thread host-stub tests (hermetic runtime check of the RT-Thread branch) ---
# Compile the same core test sources with OSP_PLATFORM_RTTHREAD=1 so the
# rt_* code paths in thread.hpp/semaphore.hpp/platform.hpp are exercised on
# the host against the pthread-backed stub in rtthread_stub/rtthread.h.
add_executable(osp_rtthread_tests
    test_thread.cpp
    test_semaphore.cpp
)
target_include_directories(osp_rtthread_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/rtthread_stub)
target_compile_definitions(osp_rtthread_tests PRIVATE
    OSP_PLATFORM_RTTHREAD=1
    OSP_NET_BACKEND=2)
target_link_libraries(osp_rtthread_tests Catch2::Catch2WithMain osp Threads::Threads)
if(OSP_TSAN_EMULATOR)
    set_property(TARGET osp_rtthread_tests PROPERTY CROSSCOMPILING_EMULATOR "${OSP_TSAN_EMULATOR}")
endif()
catch_discover_tests(osp_rtthread_tests
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
        test_lwip_modules.cpp
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

# --- Windows/MSVC portable test subset -------------------------------------
# The full core suite depends on the POSIX layer (termios/fork/epoll/...), which
# MSVC does not provide. On MSVC build and run only the tests whose module
# headers are in the portable Windows consumer closure; osp_header_smoke proves
# the rest of that closure compiles.
if(MSVC)
    add_executable(osp_msvc_tests
        test_thread.cpp
        test_hsm.cpp
        test_hsm_table.cpp
        test_vocabulary.cpp
        test_mem_pool.cpp
        test_spsc_ringbuffer.cpp
        test_log.cpp
        test_async_log.cpp
        test_semaphore.cpp
        test_timer.cpp
        test_watchdog.cpp
    )
    target_link_libraries(osp_msvc_tests Catch2::Catch2WithMain osp Threads::Threads)
    catch_discover_tests(osp_msvc_tests
        PROPERTIES SKIP_RETURN_CODE 4
    )
endif()
