file(GLOB_RECURSE src *.cpp *.c)
add_executable(mock_pipe_hid ${src})
target_link_libraries(mock_pipe_hid usbipdcpp_virtual_device cxxopts::cxxopts)
target_include_directories(mock_pipe_hid PRIVATE .)
