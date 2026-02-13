cmake_minimum_required(VERSION 3.14)
project(newosp VERSION 0.1.0 LANGUAGES CXX)

# --- Core: header-only interface library ---
add_library(osp INTERFACE)
target_include_directories(osp INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>)
target_compile_features(osp INTERFACE cxx_std_14)

# --- Configuration options ---
option(OSP_BUILD_TESTS    "Build tests"                              ON)
option(OSP_BUILD_EXAMPLES "Build examples"                           OFF)
option(OSP_CONFIG_INI     "Enable INI config support (inih)"         ON)
option(OSP_CONFIG_JSON    "Enable JSON config support (nlohmann/json)" OFF)
option(OSP_CONFIG_YAML    "Enable YAML config support (fkYAML)"      OFF)

# --- Examples ---
if(OSP_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

# --- Tests ---
if(OSP_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# --- Install ---
include(CMakePackageConfigHelpers)
install(TARGETS osp EXPORT ospTargets)
install(DIRECTORY include/osp DESTINATION include)
install(EXPORT ospTargets
    FILE ospConfig.cmake
    NAMESPACE osp::
    DESTINATION lib/cmake/osp)
