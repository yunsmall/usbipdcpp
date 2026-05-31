file(GLOB_RECURSE src *.cpp *.c)
add_executable(mock_mouse ${src})
target_link_libraries(mock_mouse usbipdcpp_virtual_device cxxopts::cxxopts)
target_include_directories(mock_mouse PRIVATE .)
