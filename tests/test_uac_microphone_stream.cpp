// UAC 麦克风（ISO IN 送流方向）测试：每包 PCM 字节数按端点 bInterval 与设备
// 速度推导（USB 2.0 §9.6.6），全速 UAC 设备不能按每秒 1000 包计算

#include <gtest/gtest.h>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <memory>
#include <optional>
#include <thread>

#include "test_utils.h"

#include "usbipdcpp/Device.h"
#include "usbipdcpp/DeviceHandler/DeviceHandler.h"
#include "usbipdcpp/protocol.h"
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

// 只建 AC/AS 两个音频接口，不装配 handler（供"装配失败"用例预置状态）
std::shared_ptr<UsbDevice> make_mic_device_shell() {
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
    return device;
}

MicFixture make_mic_device_with(std::unique_ptr<AudioSource> source) {
    auto device = make_mic_device_shell();
    EXPECT_FALSE(UacDeviceHelper::setup_microphone(device, 0, std::move(source)));
    auto *dh = dynamic_cast<VirtualDeviceHandler *>(device->handler.get());
    return {device, dh, {}, device->interfaces[1].endpoints[1][0]};
}

MicFixture make_mic_device() {
    return make_mic_device_with(std::make_unique<SineWaveSource>(440, std::vector<std::uint32_t>{48000}, 1, 1.0));
}

/// 所有采样都为固定值的音频源：验证音量缩放的数值结果
struct ConstantSource : AudioSource {
    explicit ConstantSource(std::int16_t sample) : value(sample) {
        buf.assign(384, 0);
        for (std::size_t i = 0; i + 1 < buf.size(); i += 2) {
            buf[i] = static_cast<std::uint8_t>(value & 0xFF);
            buf[i + 1] = static_cast<std::uint8_t>((value >> 8) & 0xFF);
        }
    }
    std::vector<AudioFormatInfo> supported_formats() const override {
        return {{1, 16, 48000}};
    }
    AudioFormatInfo current_format() const override {
        return format;
    }
    bool set_format(std::uint16_t channels, std::uint8_t bits_per_sample, std::uint32_t sample_rate) override {
        if (channels != 1 || bits_per_sample != 16) {
            return false;
        }
        format = {channels, bits_per_sample, sample_rate};
        return true;
    }
    bool get_chunk(AudioChunk &chunk) override {
        chunk.data = buf.data();
        chunk.size = buf.size();
        return true;
    }
    AudioFormatInfo format{1, 16, 48000};
    std::int16_t value;
    std::vector<std::uint8_t> buf;
};

} // namespace

TEST(TestUacMicrophoneStream, FullSpeedPacketSizeFollowsBInterval) {
    // 全速 bInterval=4 → 每 4ms 一包；48kHz 单声道 16bit 每包 192 帧 = 384 字节。
    // 固定按 1000 包/秒算时只填 96 字节，实际送流速率只有标称的 1/4
    auto fx = make_mic_device();
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

TEST(TestUacMicrophoneStream, VolumeControlScalesPcm) {
    // AC Feature Unit 音量（SET_CUR(FU_VOLUME)）作用到麦克风送流：每个 16 位
    // 采样按 Q16 增益缩放（与扬声器收流的处理对称）
    auto fx = make_mic_device_with(std::make_unique<ConstantSource>(1000));
    CaptureResponder stub;
    usbipdcpp::error_code ec;
    fx.dh->on_new_connection(stub, ec);
    ASSERT_FALSE(ec);

    auto *as = dynamic_cast<UacAudioStreamingSourceHandler *>(fx.device->interfaces[1].handler.get());
    ASSERT_NE(as, nullptr);
    std::uint32_t status = 0;
    as->request_set_interface(1, &status);
    ASSERT_EQ(status, 0u);

    // -12dB（1/256 dB 单位），期望增益 ≈ 0.2512
    const std::int16_t target_db = -0x0C00;
    const SetupPacket vol_setup{
            .request_type = 0x21, // Class | Interface | OUT
            .request = 0x01,      // SET_CUR
            .value = static_cast<std::uint16_t>(FU_VOLUME_CONTROL << 8),
            .index = static_cast<std::uint16_t>(UAC_ENTITY_FEATURE_UNIT << 8),
            .length = 2,
    };
    const data_type vol_data = {static_cast<std::uint8_t>(target_db & 0xFF),
                                static_cast<std::uint8_t>((target_db >> 8) & 0xFF)};
    fx.dh->receive_urb(make_cmd_submit(fx.op, 101, 0x00, UsbIpDirection::Out, 2, vol_setup, vol_data),
                       fx.device->ep0_in, std::nullopt, ec);
    ASSERT_FALSE(ec);

    auto submit = make_cmd_submit(fx.op, 2, 0x01, UsbIpDirection::In, 1024, {}, {}, 2);
    auto *trx = GenericTransfer::from_handle(submit.transfer.get());
    trx->iso_descriptors[0] = {.offset = 0, .length = 512, .actual_length = 0, .status = 0};
    trx->iso_descriptors[1] = {.offset = 512, .length = 512, .actual_length = 0, .status = 0};
    fx.dh->receive_urb(std::move(submit), fx.ep_iso_in, fx.device->interfaces[1], ec);
    ASSERT_FALSE(ec);

    // 等调度线程填充完再断连（join 调度线程后才能安全读 stub）
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    fx.dh->on_disconnection(ec);

    const UsbIpResponse::UsbIpRetSubmit *iso_ret = nullptr;
    for (auto &s: stub.submits) {
        if (s.header.seqnum == 2) {
            iso_ret = &s;
        }
    }
    ASSERT_NE(iso_ret, nullptr);
    auto *resp = GenericTransfer::from_handle(iso_ret->transfer.get());
    ASSERT_EQ(resp->iso_descriptors[0].actual_length, 384u);

    // 期望值与 handle_sampling_freq_control 用的公式一致：Q16、四舍五入
    const auto linear = std::pow(10.0, (target_db / 256.0) / 20.0);
    const auto gain = static_cast<std::int32_t>(linear * 65536.0 + 0.5);
    const auto expected = static_cast<std::int16_t>((1000 * gain) >> 16);
    const auto got = static_cast<std::int16_t>(resp->data[0] | (resp->data[1] << 8));
    EXPECT_NE(got, 1000); // 确实缩放了
    EXPECT_EQ(got, expected);

    // 每个采样都按同一增益缩放
    const auto second = static_cast<std::int16_t>(resp->data[2] | (resp->data[3] << 8));
    EXPECT_EQ(second, expected);
}

namespace {

/// 非 VirtualDeviceHandler 的设备处理器：用于验证 UAC/UVC helper 对处理器
/// 类型不符的处理（不能空指针解引用）
class ForeignDeviceHandler : public AbstDeviceHandler {
public:
    using AbstDeviceHandler::AbstDeviceHandler;
    void receive_urb(UsbIpCommand::UsbIpCmdSubmit, UsbEndpoint, std::optional<UsbInterface>,
                     usbipdcpp::error_code &) override {
    }
    void handle_unlink_seqnum(std::uint32_t, std::uint32_t) override {
    }
};

} // namespace

TEST(TestUacMicrophoneStream, SetupRejectsForeignDeviceHandler) {
    // 设备已绑定其它类型的 handler 时，UAC 装配必须报参数错误：
    // dynamic_pointer_cast 得到空指针后直接调用会崩溃
    auto device = make_mic_device_shell();
    device->handler = std::make_shared<ForeignDeviceHandler>(*device);

    auto err = UacDeviceHelper::setup_microphone(device, 0, std::make_unique<ConstantSource>(0));
    EXPECT_EQ(err, make_error_code(ErrorType::INVALID_ARG));
    // 失败发生在装配任何接口之前：不能留下半装配状态
    EXPECT_EQ(device->interfaces[0].handler, nullptr);
    EXPECT_EQ(device->interfaces[1].handler, nullptr);

    auto err_speaker = UacDeviceHelper::setup_speaker(device, 0, nullptr);
    EXPECT_EQ(err_speaker, make_error_code(ErrorType::INVALID_ARG));
}
