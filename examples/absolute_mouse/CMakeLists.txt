file(GLOB_RECURSE src *.cpp *.c)
add_executable(absolute_mouse ${src})
target_link_libraries(absolute_mouse usbipdcpp_virtual_device cxxopts::cxxopts)
target_include_directories(absolute_mouse PRIVATE .)
