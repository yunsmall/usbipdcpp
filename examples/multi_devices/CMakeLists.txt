file(GLOB_RECURSE src *.cpp *.c)
add_executable(multi_devices ${src})
target_link_libraries(multi_devices usbipdcpp_virtual_device cxxopts::cxxopts)
target_include_directories(multi_devices PRIVATE .)
