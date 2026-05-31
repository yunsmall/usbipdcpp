file(GLOB_RECURSE src *.cpp *.c)
add_executable(mock_keyboard ${src})
target_link_libraries(mock_keyboard usbipdcpp_virtual_device cxxopts::cxxopts)
target_include_directories(mock_keyboard PRIVATE .)
