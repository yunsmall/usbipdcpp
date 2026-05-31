file(GLOB_RECURSE src *.cpp *.c)
add_executable(mock_gamepad ${src})
target_link_libraries(mock_gamepad usbipdcpp_virtual_device cxxopts::cxxopts)
target_include_directories(mock_gamepad PRIVATE .)
