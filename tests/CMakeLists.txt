set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

find_package(GTest CONFIG REQUIRED)
include(GoogleTest)

function(add_test_file name)
    add_executable(${name} ${name}.cpp)
    target_link_libraries(${name} PRIVATE usbipdcpp GTest::gtest GTest::gtest_main)
    target_include_directories(${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    # PRE_TEST：测试发现移到 ctest 运行时，避免并行构建下多个测试可执行文件
    # 在构建期并发启动（偶发加载动态库失败导致 JSON 解析错误，CI 上出现过）
    gtest_discover_tests(${name} DISCOVERY_MODE PRE_TEST)
endfunction()

function(add_test_dir name)
    file(GLOB_RECURSE src ${name}/*.cpp)
    add_executable(${name} ${src})
    target_link_libraries(${name} PRIVATE usbipdcpp GTest::gtest GTest::gtest_main)
    target_include_directories(${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    gtest_discover_tests(${name} DISCOVERY_MODE PRE_TEST)
endfunction()

add_test_dir(test_protocol)

add_test_file(test_network)
add_test_file(test_stringpool)
add_test_file(test_usb_device)
add_test_file(test_setup_packet)
add_test_file(test_usbip_response)
add_test_file(test_object_pool)
add_test_file(test_small_vector)
add_test_file(test_transfer_handle)
add_test_file(test_generic_transfer)
add_test_file(test_reuse_map)
add_test_file(test_reuse_set)
add_test_file(test_descriptors)
add_test_file(test_ring_buffer)

# 音频源在虚拟设备库中；AudioFileSource 相关用例由文件内 USBIPDCPP_USE_MINIAUDIO 宏条件编译
if (TARGET usbipdcpp_virtual_device)
    add_test_file(test_audio_sources)
    target_link_libraries(test_audio_sources PRIVATE usbipdcpp_virtual_device)

    # 虚拟设备处理器的纯逻辑测试，每种设备一个文件
    add_test_file(test_hid_handler)
    target_link_libraries(test_hid_handler PRIVATE usbipdcpp_virtual_device)

    add_test_file(test_cdc_acm_handler)
    target_link_libraries(test_cdc_acm_handler PRIVATE usbipdcpp_virtual_device)

    # 走网络的虚拟设备测试（import 后 stop 等）
    add_test_file(test_network_vdev)
    target_link_libraries(test_network_vdev PRIVATE usbipdcpp_virtual_device)
endif ()
