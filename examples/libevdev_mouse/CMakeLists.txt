file(GLOB_RECURSE src *.cpp)
add_executable(libevdev_mouse ${src})
target_link_libraries(libevdev_mouse usbipdcpp_virtual_device PkgConfig::libevdev cxxopts::cxxopts)
target_include_directories(libevdev_mouse PRIVATE . include)
