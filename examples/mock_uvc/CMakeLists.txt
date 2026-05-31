file(GLOB_RECURSE src *.cpp *.c)
add_executable(mock_uvc ${src})
target_link_libraries(mock_uvc usbipdcpp_virtual_device cxxopts::cxxopts)
target_include_directories(mock_uvc PRIVATE .)
