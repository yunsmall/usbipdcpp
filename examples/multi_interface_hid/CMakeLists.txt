file(GLOB_RECURSE src *.cpp *.c)
add_executable(multi_interface_hid ${src})
target_link_libraries(multi_interface_hid usbipdcpp_virtual_device cxxopts::cxxopts)
target_include_directories(multi_interface_hid PRIVATE .)
