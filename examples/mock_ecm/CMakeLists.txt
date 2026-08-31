file(GLOB_RECURSE src *.cpp *.c)
add_executable(mock_ecm ${src})
target_link_libraries(mock_ecm usbipdcpp_virtual_device cxxopts::cxxopts)
target_include_directories(mock_ecm PRIVATE .)
