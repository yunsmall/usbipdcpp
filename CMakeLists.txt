cmake_minimum_required(VERSION 3.20)
project(usbipdcpp VERSION 0.0.1)

set(CMAKE_CXX_STANDARD 26)

find_package(asio CONFIG REQUIRED)
find_package(spdlog CONFIG REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(libusb REQUIRED IMPORTED_TARGET libusb-1.0)

enable_testing()
find_package(GTest CONFIG REQUIRED)

if (MSVC)
    add_compile_options("/Zc:preprocessor")
endif ()

if (CMAKE_BUILD_TYPE STREQUAL "Release")
    add_compile_definitions(
            SPDLOG_ACTIVE_LEVEL=SPDLOG_LEVEL_INFO
            NDEBUG
    )
else ()
    add_compile_definitions(
            SPDLOG_ACTIVE_LEVEL=SPDLOG_LEVEL_TRACE
    )
endif ()

include(GNUInstallDirs)

file(GLOB BASE_SRC_FILES src/*.cpp)
file(GLOB DEVICE_HANDLER_SRC_FILES src/DeviceHandler/*.cpp)
file(GLOB INTERFACE_HANDLER_SRC_FILES src/InterfaceHandler/*.cpp)
file(GLOB LIBUSB_SRC_FILES src/libusb_handler/*.cpp)

add_library(${PROJECT_NAME} STATIC ${BASE_SRC_FILES} ${DEVICE_HANDLER_SRC_FILES} ${INTERFACE_HANDLER_SRC_FILES})
target_link_libraries(${PROJECT_NAME} PUBLIC asio::asio)
target_link_libraries(${PROJECT_NAME} PUBLIC spdlog::spdlog)
target_include_directories(${PROJECT_NAME} PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>" "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")

add_library(${PROJECT_NAME}_libusb STATIC ${LIBUSB_SRC_FILES})
target_link_libraries(${PROJECT_NAME}_libusb PUBLIC PkgConfig::libusb)
target_link_libraries(${PROJECT_NAME}_libusb PUBLIC ${PROJECT_NAME})

if ((UNIX AND NOT APPLE) OR ANDROID)
    pkg_check_modules(libevdev REQUIRED IMPORTED_TARGET libevdev)

    file(GLOB_RECURSE LIBEVDEV_MOUSE_SRC examples/libevdev_mouse/*.cpp)
    add_executable(libevdev_mouse ${LIBEVDEV_MOUSE_SRC})
    target_link_libraries(libevdev_mouse PUBLIC ${PROJECT_NAME})
    target_link_libraries(libevdev_mouse PUBLIC PkgConfig::libevdev)
    target_include_directories(libevdev_mouse PRIVATE examples/libevdev_mouse/include)
endif ()

file(GLOB_RECURSE MOCK_MOUSE_SRC examples/mock_mouse/*.cpp)
add_executable(mock_mouse ${MOCK_MOUSE_SRC})
target_link_libraries(mock_mouse ${PROJECT_NAME})
target_include_directories(mock_mouse PRIVATE examples/mock_mouse)

add_executable(empty_server examples/empty_server.cpp)
target_link_libraries(empty_server ${PROJECT_NAME})

add_executable(libusb_server examples/libusb_server.cpp)
target_link_libraries(libusb_server ${PROJECT_NAME}_libusb)

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


export(TARGETS ${PROJECT_NAME} ${PROJECT_NAME}_libusb
        NAMESPACE ${PROJECT_NAME}::
        FILE ${PROJECT_NAME}Targets.cmake
)

install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(TARGETS ${PROJECT_NAME} ${PROJECT_NAME}_libusb
        EXPORT ${PROJECT_NAME}Targets
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

#install(TARGETS mock_mouse empty_server libusb_server
#        RUNTIME DESTINATION example
#)

install(EXPORT ${PROJECT_NAME}Targets
        FILE ${PROJECT_NAME}Targets.cmake
        NAMESPACE ${PROJECT_NAME}::
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/${PROJECT_NAME}
)

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