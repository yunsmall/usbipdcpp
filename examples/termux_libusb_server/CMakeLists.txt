file(GLOB_RECURSE src *.cpp)
add_executable(termux_libusb_server ${src})
target_link_libraries(termux_libusb_server usbipdcpp_libusb)
target_include_directories(termux_libusb_server PRIVATE .)
