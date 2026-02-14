# --------------------------------------------------------------------------
# inicpp examples (embedded ini library)
# --------------------------------------------------------------------------

# Comprehensive demo showing all inicpp features
add_executable(osp_inicpp_demo inicpp_demo.cpp)
target_link_libraries(osp_inicpp_demo PRIVATE osp)
target_compile_features(osp_inicpp_demo PRIVATE cxx_std_17)

# Custom type conversion example (std::vector support)
add_executable(osp_inicpp_custom_type custom_type_conversion.cpp)
target_link_libraries(osp_inicpp_custom_type PRIVATE osp)
target_compile_features(osp_inicpp_custom_type PRIVATE cxx_std_17)
