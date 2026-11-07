// UAC 麦克风（ISO IN 送流方向）测试：每包 PCM 字节数按端点 bInterval 与设备
// 速度推导（USB 2.0 §9.6.6），全速 UAC 设备不能按每秒 1000 包计算

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <memory>
#include <thread>

#include "test_utils.h"

#include "usbipdcpp/Device.h"
#include "usbipdcpp/utils/StringPool.h"
#include "usbipdcpp/virtual_device/UacConstants.h"
#include "usbipdcpp/virtual_device/UacVirtualInterfaceHandler.h"
#include "usbipdcpp/virtual_device/VirtualDeviceHandler.h"
#include "usbipdcpp/virtual_device/audio_sources/SineWaveSource.h"

using namespace usbipdcpp;
using namespace usbipdcpp::test;

namespace {

// 构造麦克风设备（对齐 examples/mock_audio）：AC（中断 IN 0x82）+
// AS（alt0 空 / alt1 ISO IN 0x81，bInterval=4）
struct MicFixture {
    std::shared_ptr<UsbDevice> device;
    VirtualDeviceHandler *dh = nullptr;
    // 传输分配器必须活得比 CaptureResponder 久（应答里的 TransferHandle 析构要用它）
    GenericTransferOperator op;
    UsbEndpoint ep_iso_in;
};

MicFixture make_mic_device(StringPool &string_pool) {
    std::vector<UsbInterface> interfaces = {
            UsbInterface{
                    .interface_class = CC_AUDIO,
                    .interface_subclass = SC_AUDIOCONTROL,
                    .interface_protocol = 0x00,
                    .endpoints = {{UsbEndpoint{
                            .address = 0x82,
                            .attributes = 0x03,
                            .max_packet_size = 2,
                            .interval = 4,
                    }}},
            },
            UsbInterface{
                    .interface_class = CC_AUDIO,
                    .interface_subclass = SC_AUDIOSTREAMING,
                    .interface_protocol = 0x00,
                    .endpoints = {{},
                                  {UsbEndpoint{
                                          .address = 0x81,
                                          .attributes = static_cast<std::uint8_t>(EndpointAttributes::Isochronous) |
                                                        static_cast<std::uint8_t>(IsoSyncType::Async),
                                          .max_packet_size = 512,
                                          .interval = 4,
                                  }}},
            },
    };
    auto device = UsbDevice::make("/test/mock_mic", 0x1234, 0x5682, std::move(interfaces), 1, 1, 0,
                                  "/usbipdcpp/mock_mic", UsbSpeed::Full, 0x0100);
    device->assign_interface_numbers();
    EXPECT_FALSE(UacDeviceHelper::setup_microphone(
            device, 0, string_pool,
            std::make_unique<SineWaveSource>(440, std::vector<std::uint32_t>{48000}, 1, 1.0)));
    auto *dh = dynamic_cast<VirtualDeviceHandler *>(device->handler.get());
    return {device, dh, {}, device->interfaces[1].endpoints[1][0]};
}

} // namespace

TEST(TestUacMicrophoneStream, FullSpeedPacketSizeFollowsBInterval) {
    // 全速 bInterval=4 → 每 4ms 一包；48kHz 单声道 16bit 每包 192 帧 = 384 字节。
    // 固定按 1000 包/秒算时只填 96 字节，实际送流速率只有标称的 1/4
    StringPool string_pool;
    auto fx = make_mic_device(string_pool);
    CaptureResponder stub;
    usbipdcpp::error_code ec;
    fx.dh->on_new_connection(stub, ec);
    ASSERT_FALSE(ec);

    auto *as = dynamic_cast<UacAudioStreamingSourceHandler *>(fx.device->interfaces[1].handler.get());
    ASSERT_NE(as, nullptr);
    std::uint32_t status = 0;
    as->request_set_interface(1, &status); // 开流
    ASSERT_EQ(status, 0u);

    // 两包各 512 字节的请求：设备按 384 字节/包填充（iso.length 是上限）
    auto submit = make_cmd_submit(fx.op, 2, 0x01, UsbIpDirection::In, 1024, {}, {}, 2);
    auto *trx = GenericTransfer::from_handle(submit.transfer.get());
    trx->iso_descriptors[0] = {.offset = 0, .length = 512, .actual_length = 0, .status = 0};
    trx->iso_descriptors[1] = {.offset = 512, .length = 512, .actual_length = 0, .status = 0};
    fx.dh->receive_urb(std::move(submit), fx.ep_iso_in, fx.device->interfaces[1], ec);
    ASSERT_FALSE(ec);

    // 调度器按数据时长（1024B / 96000B/s ≈ 10.7ms）延迟应答。等待期间不能
    // 读 stub（调度线程在写）；固定等一小段后断连 join 调度线程再读
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    fx.dh->on_disconnection(ec);

    ASSERT_EQ(stub.submits.size(), 1u);
    EXPECT_EQ(stub.submits[0].status, 0u);
    auto *resp = GenericTransfer::from_handle(stub.submits[0].transfer.get());
    ASSERT_EQ(resp->iso_descriptors.size(), 2u);
    EXPECT_EQ(resp->iso_descriptors[0].actual_length, 384u);
    EXPECT_EQ(resp->iso_descriptors[1].actual_length, 384u);
}
