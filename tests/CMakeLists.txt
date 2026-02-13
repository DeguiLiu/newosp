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
    test_worker_pool.cpp
    test_executor.cpp
    test_hsm.cpp
    test_bt.cpp
    test_socket.cpp
)
target_link_libraries(osp_tests Catch2::Catch2WithMain osp Threads::Threads)
catch_discover_tests(osp_tests)
