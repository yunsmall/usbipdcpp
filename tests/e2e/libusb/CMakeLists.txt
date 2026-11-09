# libusb 后端的真机 e2e 辅助程序：手动运行，不注册进 ctest
#
# 设备侧是 FunctionFS 守护进程（依赖内核 dummy_hcd + usb_f_fs），
# 客户端是普通的 libusb 程序；运行方式见 run_one.sh / run_all.sh。

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

find_package(Threads REQUIRED)

add_library(usbipdcpp_e2e_ffs STATIC ffs/ffs_helper.cpp)
target_include_directories(usbipdcpp_e2e_ffs PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(usbipdcpp_e2e_ffs PUBLIC Threads::Threads)

foreach(profile control bulk interrupt)
    add_executable(e2e_ffs_${profile} ffs/${profile}_device.cpp)
    target_link_libraries(e2e_ffs_${profile} PRIVATE usbipdcpp_e2e_ffs)

    add_executable(e2e_libusb_${profile}_client clients/${profile}_client.cpp
            clients/client_common.cpp)
    target_include_directories(e2e_libusb_${profile}_client
            PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(e2e_libusb_${profile}_client PRIVATE usbipdcpp_libusb)
endforeach()

# iso：dummy_hcd 不支持等时端点，改用项目自己的虚拟设备框架造设备，
# 链路是“虚拟设备 → vhci → libusb_server → vhci → libusb 客户端”双跳
if (TARGET usbipdcpp_virtual_device)
    add_executable(e2e_vdev_iso vdev/iso_device.cpp)
    target_include_directories(e2e_vdev_iso PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(e2e_vdev_iso PRIVATE usbipdcpp_virtual_device)

    add_executable(e2e_libusb_iso_client clients/iso_client.cpp clients/client_common.cpp)
    target_include_directories(e2e_libusb_iso_client PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(e2e_libusb_iso_client PRIVATE usbipdcpp_libusb)
endif()
