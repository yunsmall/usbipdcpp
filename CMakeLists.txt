cmake_minimum_required(VERSION 3.31)
project(usbipcpp)

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

file(GLOB LIBUSB_SRC_FILES src/libusb_handler/*.cpp)
file(GLOB BASE_SRC_FILES src/*.cpp)

add_library(${PROJECT_NAME} STATIC ${BASE_SRC_FILES} ${LIBUSB_SRC_FILES})
target_link_libraries(${PROJECT_NAME} PUBLIC asio::asio)
target_link_libraries(${PROJECT_NAME} PUBLIC spdlog::spdlog)
target_link_libraries(${PROJECT_NAME} PRIVATE PkgConfig::libusb)
target_include_directories(${PROJECT_NAME} PUBLIC include)

if (CMAKE_BUILD_TYPE STREQUAL "Release")
    target_compile_definitions(${PROJECT_NAME} PUBLIC
            SPDLOG_ACTIVE_LEVEL=SPDLOG_LEVEL_INFO
            NDEBUG
    )
else ()
    target_compile_definitions(${PROJECT_NAME} PUBLIC
            SPDLOG_ACTIVE_LEVEL=SPDLOG_LEVEL_TRACE
    )
endif ()

file(GLOB_RECURSE MOCK_MOUSE_SRC example/mock_mouse/*.cpp)
add_executable(mock_mouse ${MOCK_MOUSE_SRC})
target_link_libraries(mock_mouse ${PROJECT_NAME})
target_include_directories(mock_mouse PRIVATE example/mock_mouse)

add_executable(empty_server example/empty_server.cpp)
target_link_libraries(empty_server ${PROJECT_NAME})

add_executable(libusb_server example/libusb_server.cpp)
target_link_libraries(libusb_server ${PROJECT_NAME})

include(GoogleTest)

file(GLOB_RECURSE TEST_PROTOCOL_SRC test/test_protocol/*.cpp)
add_executable(test_protocol ${TEST_PROTOCOL_SRC})
target_include_directories(test_protocol PRIVATE test)
target_link_libraries(test_protocol PRIVATE ${PROJECT_NAME})
target_link_libraries(test_protocol PRIVATE GTest::gtest_main)
gtest_discover_tests(test_protocol)

set(TEST_NETWORK_SRC test/test_network.cpp)
add_executable(test_network ${TEST_NETWORK_SRC})
target_include_directories(test_network PRIVATE test)
target_link_libraries(test_network PRIVATE ${PROJECT_NAME})
target_link_libraries(test_network PRIVATE GTest::gtest_main)
gtest_discover_tests(test_network)

install(TARGETS ${PROJECT_NAME}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
)

install(TARGETS mock_mouse empty_server libusb_server
        RUNTIME DESTINATION example
)

install(DIRECTORY include/ DESTINATION include)