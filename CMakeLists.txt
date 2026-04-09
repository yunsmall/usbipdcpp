cmake_minimum_required(VERSION 3.22)
project(usbipdcpp VERSION 0.0.1)

set(CMAKE_CXX_STANDARD 23)

include(GNUInstallDirs)

if (CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(IS_TOP_LEVEL ON)
else ()
    set(IS_TOP_LEVEL OFF)
endif ()
option(USBIPDCPP_BUILD_EXAMPLES "Build example applications" ${IS_TOP_LEVEL})
option(USBIPDCPP_BUILD_EXAMPLE_LIBUSB_SERVER "Build example libusb server applications" ${IS_TOP_LEVEL})

if ((UNIX AND NOT APPLE) OR ANDROID)
    set(CAN_THIS_SYSTEM_SUPPORT_EVDEV TRUE)
else ()
    set(CAN_THIS_SYSTEM_SUPPORT_EVDEV FALSE)
endif ()

if (IS_TOP_LEVEL AND CAN_THIS_SYSTEM_SUPPORT_EVDEV)
    set(USBIPDCPP_BUILD_EXAMPLE_LIBEVDEV_MOUSE_DEFAULT ON)
else ()
    set(USBIPDCPP_BUILD_EXAMPLE_LIBEVDEV_MOUSE_DEFAULT OFF)
endif ()
option(
        USBIPDCPP_BUILD_EXAMPLE_LIBEVDEV_MOUSE
        "Build example libevdev mouse applications"
        ${USBIPDCPP_BUILD_EXAMPLE_LIBEVDEV_MOUSE_DEFAULT}
)

if (IS_TOP_LEVEL AND ANDROID)
    set(USBIPDCPP_BUILD_EXAMPLE_TERMUX_LIBUSB_SERVER_DEFAULT ON)
else ()
    set(USBIPDCPP_BUILD_EXAMPLE_TERMUX_LIBUSB_SERVER_DEFAULT OFF)
endif ()
option(
        USBIPDCPP_BUILD_EXAMPLE_TERMUX_LIBUSB_SERVER
        "Build example libusb applications that can be run in termux by termux-usb"
        ${USBIPDCPP_BUILD_EXAMPLE_TERMUX_LIBUSB_SERVER_DEFAULT}
)

option(USBIPDCPP_BUILD_EXAMPLE_MOCK_MOUSE "Build example mock mouse applications" ${IS_TOP_LEVEL})
option(USBIPDCPP_BUILD_EXAMPLE_MOCK_KEYBOARD "Build example mock keyboard applications" ${IS_TOP_LEVEL})
option(USBIPDCPP_BUILD_EXAMPLE_MOCK_CDC_ACM "Build example mock CDC ACM (virtual serial port) applications" ${IS_TOP_LEVEL})
option(USBIPDCPP_BUILD_EXAMPLE_EMPTY_SERVER "Build example empty server applications" ${IS_TOP_LEVEL})
option(USBIPDCPP_BUILD_EXAMPLE_MULTI_DEVICES "Build example multi devices applications" ${IS_TOP_LEVEL})
option(USBIPDCPP_BUILD_VIRTUAL_DEVICE "Build virtual device component" ON)
option(USBIPDCPP_BUILD_LIBUSB_COMPONENTS "Build libusb component" ON)
option(USBIPDCPP_BUILD_TESTS "Build tests" ${IS_TOP_LEVEL})

option(USBIPDCPP_USE_COROUTINE "Use coroutine-based implementation" OFF)
option(USBIPDCPP_ENABLE_BUSY_WAIT "Enable busy-wait mode for lower latency (only effective when USBIPDCPP_USE_COROUTINE is OFF)" OFF)
option(USBIPDCPP_FORCE_TRACK_PACKAGE "Force to track package even in Release build" OFF)

find_package(asio CONFIG REQUIRED)
find_package(spdlog CONFIG REQUIRED)

if (MSVC)
    add_compile_options("/Zc:preprocessor")
endif ()

if(USBIPDCPP_USE_COROUTINE)
    message(STATUS "USBIPDCPP: Using coroutine-based implementation")
    add_compile_definitions(USBIPDCPP_USE_COROUTINE)
else()
    message(STATUS "USBIPDCPP: Using non-coroutine implementation")
    if(USBIPDCPP_ENABLE_BUSY_WAIT)
        message(STATUS "USBIPDCPP: Busy-wait mode enabled")
        add_compile_definitions(USBIPDCPP_ENABLE_BUSY_WAIT)
    endif()
endif ()

if (CMAKE_BUILD_TYPE STREQUAL "Release")
    add_compile_definitions(
            SPDLOG_ACTIVE_LEVEL=SPDLOG_LEVEL_INFO
            NDEBUG
    )
elseif (CMAKE_BUILD_TYPE STREQUAL "RelWithDebInfo")
    add_compile_definitions(
#            SPDLOG_ACTIVE_LEVEL=SPDLOG_LEVEL_DEBUG
            SPDLOG_ACTIVE_LEVEL=SPDLOG_LEVEL_INFO
            NDEBUG
    )
else ()
    add_compile_definitions(
            SPDLOG_ACTIVE_LEVEL=SPDLOG_LEVEL_TRACE
            TRANSFER_DELAY_RECORD
    )
endif ()

file(GLOB BASE_SRC_FILES src/*.cpp)
file(GLOB DEVICE_HANDLER_SRC_FILES src/DeviceHandler/*.cpp)
file(GLOB INTERFACE_HANDLER_SRC_FILES src/InterfaceHandler/*.cpp)
file(GLOB VIRTUAL_DEVICE_SRC_FILES src/virtual_device/*.cpp)
file(GLOB LIBUSB_SRC_FILES src/LibusbHandler/*.cpp)

# 核心库（不含虚拟设备）
add_library(${PROJECT_NAME} STATIC ${BASE_SRC_FILES} ${DEVICE_HANDLER_SRC_FILES} ${INTERFACE_HANDLER_SRC_FILES})
target_link_libraries(${PROJECT_NAME} PUBLIC asio::asio)
target_link_libraries(${PROJECT_NAME} PUBLIC spdlog::spdlog)
target_include_directories(${PROJECT_NAME} PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>" "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/${PROJECT_NAME}>")
if(USBIPDCPP_FORCE_TRACK_PACKAGE)
    target_compile_definitions(${PROJECT_NAME} PUBLIC USBIPDCPP_FORCE_TRACK_PACKAGE)
endif()

# 虚拟设备库
if(USBIPDCPP_BUILD_VIRTUAL_DEVICE)
    add_library(${PROJECT_NAME}_virtual_device STATIC ${VIRTUAL_DEVICE_SRC_FILES})
    target_link_libraries(${PROJECT_NAME}_virtual_device PUBLIC ${PROJECT_NAME})
    target_include_directories(${PROJECT_NAME}_virtual_device PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>")
endif()

if (USBIPDCPP_BUILD_LIBUSB_COMPONENTS)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(libusb REQUIRED IMPORTED_TARGET libusb-1.0)

    add_library(${PROJECT_NAME}_libusb STATIC ${LIBUSB_SRC_FILES})
    target_link_libraries(${PROJECT_NAME}_libusb PUBLIC PkgConfig::libusb)
    target_link_libraries(${PROJECT_NAME}_libusb PUBLIC ${PROJECT_NAME})
endif ()

if (USBIPDCPP_BUILD_EXAMPLES)
    if (USBIPDCPP_BUILD_EXAMPLE_LIBEVDEV_MOUSE)
        if (CAN_THIS_SYSTEM_SUPPORT_EVDEV)
            find_package(PkgConfig REQUIRED)
            pkg_check_modules(libevdev REQUIRED IMPORTED_TARGET libevdev)

            file(GLOB_RECURSE LIBEVDEV_MOUSE_SRC examples/libevdev_mouse/*.cpp)
            add_executable(libevdev_mouse ${LIBEVDEV_MOUSE_SRC})
            target_link_libraries(libevdev_mouse PUBLIC ${PROJECT_NAME}_virtual_device)
            target_link_libraries(libevdev_mouse PUBLIC PkgConfig::libevdev)
            target_include_directories(libevdev_mouse PRIVATE examples/libevdev_mouse/include examples/libevdev_mouse)
        else ()
            message(WARNING "evdev is not available in this target")
        endif ()
    endif ()

    if (USBIPDCPP_BUILD_EXAMPLE_MOCK_MOUSE)
        file(GLOB_RECURSE MOCK_MOUSE_SRC examples/mock_mouse/*.cpp)
        add_executable(mock_mouse ${MOCK_MOUSE_SRC})
        target_link_libraries(mock_mouse ${PROJECT_NAME}_virtual_device)
        target_include_directories(mock_mouse PRIVATE examples/mock_mouse)
    endif ()

    if (USBIPDCPP_BUILD_EXAMPLE_MOCK_KEYBOARD)
        file(GLOB_RECURSE MOCK_KEYBOARD_SRC examples/mock_keyboard/*.cpp)
        add_executable(mock_keyboard ${MOCK_KEYBOARD_SRC})
        target_link_libraries(mock_keyboard ${PROJECT_NAME}_virtual_device)
        target_include_directories(mock_keyboard PRIVATE examples/mock_keyboard)
    endif ()

    if (USBIPDCPP_BUILD_EXAMPLE_MOCK_CDC_ACM)
        file(GLOB_RECURSE MOCK_CDC_ACM_SRC examples/mock_cdc_acm/*.cpp)
        add_executable(mock_cdc_acm ${MOCK_CDC_ACM_SRC})
        target_link_libraries(mock_cdc_acm ${PROJECT_NAME}_virtual_device)
        target_include_directories(mock_cdc_acm PRIVATE examples/mock_cdc_acm)
    endif ()

    if (USBIPDCPP_BUILD_EXAMPLE_EMPTY_SERVER)
        add_executable(empty_server examples/empty_server.cpp)
        target_link_libraries(empty_server ${PROJECT_NAME})
    endif ()

    if (USBIPDCPP_BUILD_EXAMPLE_MULTI_DEVICES)
        file(GLOB_RECURSE MULTI_DEVICES_SRC examples/multi_devices/*.cpp)
        add_executable(multi_devices ${MULTI_DEVICES_SRC})
        target_link_libraries(multi_devices ${PROJECT_NAME}_virtual_device)
        target_include_directories(multi_devices PRIVATE examples/multi_devices)
    endif ()

    if (USBIPDCPP_BUILD_LIBUSB_COMPONENTS AND USBIPDCPP_BUILD_EXAMPLE_LIBUSB_SERVER)
        add_executable(libusb_server examples/libusb_server.cpp)
        target_link_libraries(libusb_server ${PROJECT_NAME}_libusb)
    endif ()

    if (USBIPDCPP_BUILD_LIBUSB_COMPONENTS AND USBIPDCPP_BUILD_EXAMPLE_TERMUX_LIBUSB_SERVER)
        add_executable(termux_libusb_server examples/termux_libusb_server.cpp)
        target_link_libraries(termux_libusb_server ${PROJECT_NAME}_libusb)
    endif ()
endif ()

if (USBIPDCPP_BUILD_TESTS)
    enable_testing()
    find_package(GTest CONFIG REQUIRED)
    include(GoogleTest)

    file(GLOB_RECURSE TEST_PROTOCOL_SRC tests/test_protocol/*.cpp)
    add_executable(test_protocol ${TEST_PROTOCOL_SRC})
    target_include_directories(test_protocol PRIVATE tests)
    target_link_libraries(test_protocol PRIVATE ${PROJECT_NAME})
    target_link_libraries(test_protocol PRIVATE GTest::gtest GTest::gtest_main)
    gtest_discover_tests(test_protocol)

    set(TEST_NETWORK_SRC tests/test_network.cpp)
    add_executable(test_network ${TEST_NETWORK_SRC})
    target_include_directories(test_network PRIVATE tests)
    target_link_libraries(test_network PRIVATE ${PROJECT_NAME})
    target_link_libraries(test_network PRIVATE GTest::gtest GTest::gtest_main)
    gtest_discover_tests(test_network)

    add_executable(test_stringpool tests/test_stringpool.cpp)
    target_include_directories(test_stringpool PRIVATE tests)
    target_link_libraries(test_stringpool PRIVATE ${PROJECT_NAME})
    target_link_libraries(test_stringpool PRIVATE GTest::gtest GTest::gtest_main)
    gtest_discover_tests(test_stringpool)

    add_executable(test_usb_device tests/test_usb_device.cpp)
    target_include_directories(test_usb_device PRIVATE tests)
    target_link_libraries(test_usb_device PRIVATE ${PROJECT_NAME})
    target_link_libraries(test_usb_device PRIVATE GTest::gtest GTest::gtest_main)
    gtest_discover_tests(test_usb_device)

    add_executable(test_setup_packet tests/test_setup_packet.cpp)
    target_include_directories(test_setup_packet PRIVATE tests)
    target_link_libraries(test_setup_packet PRIVATE ${PROJECT_NAME})
    target_link_libraries(test_setup_packet PRIVATE GTest::gtest GTest::gtest_main)
    gtest_discover_tests(test_setup_packet)

    add_executable(test_usbip_response tests/test_usbip_response.cpp)
    target_include_directories(test_usbip_response PRIVATE tests)
    target_link_libraries(test_usbip_response PRIVATE ${PROJECT_NAME})
    target_link_libraries(test_usbip_response PRIVATE GTest::gtest GTest::gtest_main)
    gtest_discover_tests(test_usbip_response)
endif ()

if (IS_TOP_LEVEL)
    set(EXPORT_TARGETS ${PROJECT_NAME})
    if (TARGET ${PROJECT_NAME}_virtual_device)
        list(APPEND EXPORT_TARGETS ${PROJECT_NAME}_virtual_device)
    endif ()
    if (TARGET ${PROJECT_NAME}_libusb)
        list(APPEND EXPORT_TARGETS ${PROJECT_NAME}_libusb)
    endif ()

    export(TARGETS ${EXPORT_TARGETS}
            NAMESPACE ${PROJECT_NAME}::
            FILE ${PROJECT_NAME}Targets.cmake
    )

    # 安装头文件
    if(USBIPDCPP_BUILD_VIRTUAL_DEVICE)
        if (USBIPDCPP_BUILD_LIBUSB_COMPONENTS)
            install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/${PROJECT_NAME})
        else()
            install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/${PROJECT_NAME}
                    PATTERN "LibusbHandler" EXCLUDE)
        endif()
    else()
        if (USBIPDCPP_BUILD_LIBUSB_COMPONENTS)
            install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/${PROJECT_NAME}
                    PATTERN "virtual_device" EXCLUDE)
        else()
            install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/${PROJECT_NAME}
                    PATTERN "virtual_device" EXCLUDE
                    PATTERN "LibusbHandler" EXCLUDE)
        endif()
    endif()


    install(EXPORT ${PROJECT_NAME}Targets
            FILE ${PROJECT_NAME}Targets.cmake
            NAMESPACE ${PROJECT_NAME}::
            DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/${PROJECT_NAME}
    )

    install(TARGETS ${PROJECT_NAME}
            EXPORT ${PROJECT_NAME}Targets
            RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
            LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
            ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
            INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/${PROJECT_NAME}
    )
    if (USBIPDCPP_BUILD_VIRTUAL_DEVICE)
        install(TARGETS ${PROJECT_NAME}_virtual_device
                EXPORT ${PROJECT_NAME}Targets
                RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
                LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
                ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
                INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/${PROJECT_NAME}
        )
    endif ()
    if (USBIPDCPP_BUILD_LIBUSB_COMPONENTS)
        install(TARGETS ${PROJECT_NAME}_libusb
                EXPORT ${PROJECT_NAME}Targets
                RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
                LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
                ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
                INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/${PROJECT_NAME}
        )
    endif ()
    #install(TARGETS mock_mouse empty_server libusb_server
    #        RUNTIME DESTINATION example
    #)


    include(CMakePackageConfigHelpers)

    configure_package_config_file(
            "${CMAKE_CURRENT_SOURCE_DIR}/cmake/${PROJECT_NAME}Config.cmake.in"
            "${CMAKE_BINARY_DIR}/${PROJECT_NAME}Config.cmake"
            INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/${PROJECT_NAME}"
    )


    write_basic_package_version_file(
            "${CMAKE_BINARY_DIR}/${PROJECT_NAME}ConfigVersion.cmake"
            VERSION ${PROJECT_VERSION}
            COMPATIBILITY SameMajorVersion
    )

    install(FILES
            "${CMAKE_BINARY_DIR}/${PROJECT_NAME}Config.cmake"
            "${CMAKE_BINARY_DIR}/${PROJECT_NAME}ConfigVersion.cmake"
            DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/${PROJECT_NAME}"
    )
endif ()