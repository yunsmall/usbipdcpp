file(GLOB_RECURSE src *.cpp *.c)
add_executable(mock_msc ${src})
target_link_libraries(mock_msc usbipdcpp_virtual_device cxxopts::cxxopts)
target_include_directories(mock_msc PRIVATE .)
