cmake_minimum_required(VERSION 3.14)
project(newosp VERSION 0.4.2 LANGUAGES C CXX)

include(FetchContent)

# GitHub mirror prefix for China mainland acceleration (CI sets to empty)
set(OSP_GITHUB_MIRROR "" CACHE STRING "GitHub download mirror prefix (e.g. https://ghfast.top/ for China)")

# --- Core: header-only interface library ---
add_library(osp INTERFACE)
target_include_directories(osp INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>)
target_compile_features(osp INTERFACE cxx_std_17)

# --- Configuration options ---
option(OSP_BUILD_TESTS    "Build tests"                                ON)
option(OSP_BUILD_EXAMPLES "Build examples"                             OFF)
option(OSP_NO_EXCEPTIONS  "Build tests with -fno-exceptions -fno-rtti" OFF)
option(OSP_CONFIG_INI     "Enable INI config support (inicpp)"         ON)
option(OSP_CONFIG_JSON    "Enable JSON config support (nlohmann/json)" OFF)
option(OSP_CONFIG_YAML    "Enable YAML config support (fkYAML)"        OFF)
option(OSP_WITH_SOCKPP    "Use sockpp for network transport"           ON)
option(OSP_WITH_NETWORK   "Enable BSD-socket network modules"          ON)

# --- INI Config (using embedded inicpp.hpp) ---
if(OSP_CONFIG_INI)
    target_compile_definitions(osp INTERFACE OSP_CONFIG_INI_ENABLED)
endif()

# --- nlohmann/json (JSON parser, used by config.hpp) ---
if(OSP_CONFIG_JSON)
    FetchContent_Declare(
        json
        URL ${OSP_GITHUB_MIRROR}https://github.com/nlohmann/json/archive/refs/tags/v3.11.3.zip
        DOWNLOAD_EXTRACT_TIMESTAMP ON
    )
    set(JSON_BuildTests OFF CACHE INTERNAL "")
    FetchContent_MakeAvailable(json)

    target_compile_definitions(osp INTERFACE OSP_CONFIG_JSON_ENABLED)
    target_compile_definitions(osp INTERFACE JSON_NOEXCEPTION=1)
    target_link_libraries(osp INTERFACE nlohmann_json::nlohmann_json)
endif()

# --- fkYAML (YAML parser, used by config.hpp) ---
if(OSP_CONFIG_YAML)
    FetchContent_Declare(
        fkyaml
        URL ${OSP_GITHUB_MIRROR}https://github.com/fktn-k/fkYAML/archive/refs/tags/v0.4.0.zip
        DOWNLOAD_EXTRACT_TIMESTAMP ON
    )
    set(FK_YAML_BUILD_TEST OFF CACHE INTERNAL "")
    FetchContent_MakeAvailable(fkyaml)

    target_compile_definitions(osp INTERFACE OSP_CONFIG_YAML_ENABLED)
    target_link_libraries(osp INTERFACE fkYAML::fkYAML)
endif()

# --- Network stack control ---
if(NOT OSP_WITH_NETWORK)
    target_compile_definitions(osp INTERFACE OSP_HAS_NETWORK=0)
endif()

# --- sockpp (Network library, used by net.hpp) ---
if(OSP_WITH_SOCKPP)
    FetchContent_Declare(
        sockpp
        URL ${OSP_GITHUB_MIRROR}https://github.com/DeguiLiu/sockpp/archive/refs/heads/master.zip
        DOWNLOAD_EXTRACT_TIMESTAMP ON
    )
    set(SOCKPP_BUILD_SHARED OFF CACHE BOOL "" FORCE)
    set(SOCKPP_BUILD_STATIC ON CACHE BOOL "" FORCE)
    set(SOCKPP_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(SOCKPP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(sockpp)

    target_compile_definitions(osp INTERFACE OSP_HAS_SOCKPP=1)
    # Note: sockpp-static needs to be linked by consumers (tests, examples)
endif()

# --- Code generation (YAML -> C++ headers) ---
option(OSP_CODEGEN "Enable code generation from YAML definitions" OFF)

if(OSP_CODEGEN)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    file(GLOB OSP_DEFS "${CMAKE_SOURCE_DIR}/defs/*.yaml")

    set(OSP_GEN_HDRS "")
    foreach(def_file ${OSP_DEFS})
        get_filename_component(def_name ${def_file} NAME_WE)
        set(out "${CMAKE_BINARY_DIR}/generated/osp/${def_name}.hpp")
        add_custom_command(OUTPUT ${out}
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/ospgen.py
                    --input ${def_file} --output ${out}
            DEPENDS ${def_file}
                    ${CMAKE_SOURCE_DIR}/tools/ospgen.py
            COMMENT "ospgen: ${def_name}.yaml -> ${def_name}.hpp")
        list(APPEND OSP_GEN_HDRS ${out})
    endforeach()

    add_custom_target(osp_codegen ALL DEPENDS ${OSP_GEN_HDRS})
    target_include_directories(osp INTERFACE
        $<BUILD_INTERFACE:${CMAKE_BINARY_DIR}/generated>)
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
if(TARGET nlohmann_json)
    install(TARGETS nlohmann_json EXPORT ospTargets)
endif()
if(TARGET fkYAML)
    install(TARGETS fkYAML EXPORT ospTargets)
endif()
if(TARGET sockpp-static)
    install(TARGETS sockpp-static EXPORT ospTargets)
endif()
install(DIRECTORY include/osp DESTINATION include)
install(EXPORT ospTargets
    FILE ospConfig.cmake
    NAMESPACE osp::
    DESTINATION lib/cmake/osp)
