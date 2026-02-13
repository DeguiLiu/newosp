cmake_minimum_required(VERSION 3.14)
project(newosp VERSION 0.1.0 LANGUAGES C CXX)

include(FetchContent)

# --- Core: header-only interface library ---
add_library(osp INTERFACE)
target_include_directories(osp INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>)
target_compile_features(osp INTERFACE cxx_std_17)

# --- Configuration options ---
option(OSP_BUILD_TESTS    "Build tests"                                ON)
option(OSP_BUILD_EXAMPLES "Build examples"                             OFF)
option(OSP_CONFIG_INI     "Enable INI config support (inih)"           ON)
option(OSP_CONFIG_JSON    "Enable JSON config support (nlohmann/json)" OFF)
option(OSP_CONFIG_YAML    "Enable YAML config support (fkYAML)"        OFF)

# --- inih (INI parser, used by config.hpp) ---
if(OSP_CONFIG_INI)
    FetchContent_Declare(
        inih
        URL https://ghfast.top/https://github.com/benhoyt/inih/archive/refs/tags/r58.zip
        DOWNLOAD_EXTRACT_TIMESTAMP ON
    )
    FetchContent_MakeAvailable(inih)

    # inih doesn't provide a proper CMake target; create one
    if(NOT TARGET inih)
        add_library(inih STATIC
            ${inih_SOURCE_DIR}/ini.c
        )
        target_include_directories(inih PUBLIC
            $<BUILD_INTERFACE:${inih_SOURCE_DIR}>
            $<INSTALL_INTERFACE:include>)
    endif()

    target_compile_definitions(osp INTERFACE OSP_CONFIG_INI_ENABLED)
    target_link_libraries(osp INTERFACE inih)
endif()

# --- JSON config backend ---
if(OSP_CONFIG_JSON)
    target_compile_definitions(osp INTERFACE OSP_CONFIG_JSON_ENABLED)
endif()

# --- YAML config backend ---
if(OSP_CONFIG_YAML)
    target_compile_definitions(osp INTERFACE OSP_CONFIG_YAML_ENABLED)
endif()

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
if(TARGET inih)
    install(TARGETS inih EXPORT ospTargets)
endif()
install(DIRECTORY include/osp DESTINATION include)
install(EXPORT ospTargets
    FILE ospConfig.cmake
    NAMESPACE osp::
    DESTINATION lib/cmake/osp)
