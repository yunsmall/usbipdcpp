file(GLOB_RECURSE src *.cpp *.c)
add_executable(mock_rndis ${src})
target_link_libraries(mock_rndis usbipdcpp_virtual_device cxxopts::cxxopts)
