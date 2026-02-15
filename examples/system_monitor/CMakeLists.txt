# --------------------------------------------------------------------------
# System monitor demo (YAML config)
# --------------------------------------------------------------------------

add_osp_example(osp_system_monitor_demo main.cpp)

# Copy YAML config to build directory for easy testing
configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/system_monitor.yaml
    ${CMAKE_CURRENT_BINARY_DIR}/system_monitor.yaml
    COPYONLY
)
