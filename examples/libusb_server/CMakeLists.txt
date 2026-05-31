file(GLOB_RECURSE src *.cpp)
add_executable(libusb_server ${src})
target_link_libraries(libusb_server usbipdcpp_libusb cxxopts::cxxopts)
target_include_directories(libusb_server PRIVATE .)
