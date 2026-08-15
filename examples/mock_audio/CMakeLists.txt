file(GLOB_RECURSE src *.cpp *.c)
add_executable(mock_audio ${src})
target_link_libraries(mock_audio usbipdcpp_virtual_device cxxopts::cxxopts)
target_include_directories(mock_audio PRIVATE .)
