file(GLOB_RECURSE src *.cpp *.c)
add_executable(mock_cdc_throttle ${src})
target_link_libraries(mock_cdc_throttle usbipdcpp_virtual_device cxxopts::cxxopts)
target_include_directories(mock_cdc_throttle PRIVATE .)