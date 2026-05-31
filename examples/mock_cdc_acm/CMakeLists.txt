file(GLOB_RECURSE src *.cpp *.c)
add_executable(mock_cdc_acm ${src})
target_link_libraries(mock_cdc_acm usbipdcpp_virtual_device cxxopts::cxxopts)
target_include_directories(mock_cdc_acm PRIVATE .)
