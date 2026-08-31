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
# 连接/停止/竞态类测试（需高频重复才能暴露问题），与纯数据测试分开：
# 高频重复直接 ctest --repeat until-fail:N -R "TestNetworkConnection"
add_test_file(test_network_connection)
add_test_file(test_stringpool)
add_test_file(test_usb_device)
add_test_file(test_setup_packet)
add_test_file(test_usbip_response)
add_test_file(test_object_pool)
add_test_file(test_small_vector)
add_test_file(test_transfer_handle)
add_test_file(test_generic_transfer)
add_test_file(test_transfer_direction)
add_test_file(test_reuse_map)
add_test_file(test_reuse_set)
add_test_file(test_descriptors)
add_test_file(test_ring_buffer)

# 音频源在虚拟设备库中（FourierSource/SineWaveSource，无第三方依赖；
# AudioFileSource 已随实现搬入 examples/mock_audio，其测试由 mock_audio 的 CMakeLists 添加）
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

    # 通用管道虚拟设备（PipeDeviceHandler）的端到端测试
    add_test_file(test_pipe_device_handler)
    target_link_libraries(test_pipe_device_handler PRIVATE usbipdcpp_virtual_device)

    # MSC Bulk-Only（U 盘）BOT 状态机数据面测试
    add_test_file(test_msc_handler)
    target_link_libraries(test_msc_handler PRIVATE usbipdcpp_virtual_device)

    # ECM 网卡数据面测试（bulk 收/发帧）
    add_test_file(test_ecm_handler)
    target_link_libraries(test_ecm_handler PRIVATE usbipdcpp_virtual_device)

    # UVC 摄像头 iso IN 取帧/帧时钟测试
    add_test_file(test_uvc_handler)
    target_link_libraries(test_uvc_handler PRIVATE usbipdcpp_virtual_device)

    # UAC 扬声器（OUT 收流）iso 数据面测试（调度器延迟消费 + 流状态切换）
    add_test_file(test_uac_speaker_stream)
    target_link_libraries(test_uac_speaker_stream PRIVATE usbipdcpp_virtual_device)

    # 传输调度器（vudc 帧调度等价物）测试
    add_test_file(test_transfer_scheduler)
    target_link_libraries(test_transfer_scheduler PRIVATE usbipdcpp_virtual_device)

    # IN 数据通道组件（挂起-应答模式）测试
    add_test_file(test_in_endpoint_channel)
    target_link_libraries(test_in_endpoint_channel PRIVATE usbipdcpp_virtual_device)

    # OUT 数据通道组件（NAK 背压模式）测试
    add_test_file(test_out_endpoint_channel)
    target_link_libraries(test_out_endpoint_channel PRIVATE usbipdcpp_virtual_device)

    # 音频汇（UAC 扬声器方向）AC/AS 描述符拓扑测试
    add_test_file(test_audio_sinks)
    target_link_libraries(test_audio_sinks PRIVATE usbipdcpp_virtual_device)

    # WavFileSink（库内音频汇）WAV 文件写入测试
    add_test_file(test_wav_file_sink)
    target_link_libraries(test_wav_file_sink PRIVATE usbipdcpp_virtual_device)
endif ()
