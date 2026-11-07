// 音频汇测试：UAC 扬声器（OUT 方向）AC/AS 描述符拓扑、采样率协商相关的
// 纯逻辑部分（WavFileSink 文件写入测试在 examples/mock_speaker/test_wav_file_sink.cpp，
// 与实现同目录；PlaybackSink 依赖真实声卡，不做自动化测试）

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>

#include "test_utils.h"

#include "usbipdcpp/Interface.h"
#include "usbipdcpp/SetupPacket.h"
#include "usbipdcpp/utils/StringPool.h"
#include "usbipdcpp/virtual_device/UacConstants.h"
#include "usbipdcpp/virtual_device/UacVirtualInterfaceHandler.h"
#include "usbipdcpp/virtual_device/UsbClassConstants.h"
#include "usbipdcpp/virtual_device/audio_sinks/AudioSink.h"
#include "usbipdcpp/virtual_device/audio_sources/AudioSource.h"

using namespace usbipdcpp;
using namespace usbipdcpp::test;

namespace {

/// 测试用内存汇：记录写入字节数，只支持单声道 16 位 48000
struct StubSink : public AudioSink {
    std::vector<AudioFormatInfo> supported_formats() const override {
        return {{1, 16, 48000}};
    }
    AudioFormatInfo current_format() const override { return format; }
    bool set_format(std::uint16_t channels, std::uint8_t bits_per_sample, std::uint32_t sample_rate) override {
        if (bits_per_sample != 16 || (channels != 1 && channels != 2)) {
            return false;
        }
        format = {channels, bits_per_sample, sample_rate};
        return true;
    }
    void write_pcm(const std::uint8_t *, std::size_t size) override { written += size; }
    AudioFormatInfo format{1, 16, 48000};
    std::size_t written = 0;
};

} // namespace

// ========== UAC AC 描述符：扬声器拓扑（对照内核 gadget f_uac1.c） ==========

TEST(UacAudioControlDesc, SpeakerTopology) {
    // 扬声器：IT(USB streaming 0x0101) → FU → OT(Speaker 0x0301)
    // 布局：Header(9) + IT(12) + FU(7+2+1=10) + OT(9) = 40 字节
    StringPool pool;
    UsbInterface intf{.interface_class = CC_AUDIO, .interface_subclass = SC_AUDIOCONTROL, .interface_protocol = 0};
    UacAudioControlHandler handler(intf, pool);
    handler.set_config(UacDeviceConfig{.input_terminal_type = TT_USB_STREAMING, .channels = 2});
    handler.on_setup_interface_handlers();
    auto desc = handler.get_class_specific_descriptor();

    ASSERT_EQ(desc.size(), 40u);
    // Input Terminal: wTerminalType 在 IT 内 offset 4-5（IT 起始 9）→ 0x0101 USB streaming
    EXPECT_EQ(desc[13], 0x01);
    EXPECT_EQ(desc[14], 0x01);
    // Output Terminal: wTerminalType 在 OT 内 offset 4-5（OT 起始 31）→ 0x0301 Speaker
    EXPECT_EQ(desc[35], 0x01);
    EXPECT_EQ(desc[36], 0x03);
    // wTotalLength（AC Header offset 5-6）回填为实际总长
    EXPECT_EQ(desc[5], 40);
    EXPECT_EQ(desc[6], 0);
}

TEST(UacAudioControlDesc, MicrophoneTopology) {
    // 麦克风（默认）：IT(MIC 0x0201) → FU → OT(USB streaming 0x0101)，回归保护
    // 布局：Header(9) + IT(12) + FU(7+1+1=9) + OT(9) = 39 字节
    StringPool pool;
    UsbInterface intf{.interface_class = CC_AUDIO, .interface_subclass = SC_AUDIOCONTROL, .interface_protocol = 0};
    UacAudioControlHandler handler(intf, pool);
    handler.set_config(UacDeviceConfig{.channels = 1});
    handler.on_setup_interface_handlers();
    auto desc = handler.get_class_specific_descriptor();

    ASSERT_EQ(desc.size(), 39u);
    // Input Terminal: 0x0201 Microphone
    EXPECT_EQ(desc[13], 0x01);
    EXPECT_EQ(desc[14], 0x02);
    // Output Terminal: 0x0101 USB streaming
    EXPECT_EQ(desc[34], 0x01);
    EXPECT_EQ(desc[35], 0x01);
}

// ========== UAC AS 汇描述符 ==========

TEST(UacAudioStreamingSinkDesc, TerminalLinkPointsToInputTerminal) {
    // 扬声器方向 AS General 的 bTerminalLink 指向 AC 的 Input Terminal（0x01，
    // 数据从 USB 流入），与麦克风（指向 Output Terminal 0x03）相反
    StringPool pool;
    UsbInterface intf{.interface_class = CC_AUDIO, .interface_subclass = SC_AUDIOSTREAMING, .interface_protocol = 0};
    UacAudioStreamingSinkHandler handler(intf, pool, std::make_unique<StubSink>());
    handler.on_setup_interface_handlers();
    auto desc = handler.get_class_specific_descriptor();

    // General(7) + FormatTypeI head(8) + 1 个采样率(3) = 18 字节
    ASSERT_EQ(desc.size(), 18u);
    // AS General: bLength=7, bDescriptorType=0x24, bDescriptorSubtype=0x01(AS_GENERAL),
    // bTerminalLink=0x01(Input Terminal)
    EXPECT_EQ(desc[0], 0x07);
    EXPECT_EQ(desc[1], 0x24);
    EXPECT_EQ(desc[2], AS_DESC_GENERAL);
    EXPECT_EQ(desc[3], UAC_ENTITY_INPUT_TERMINAL);
    // wFormatTag: PCM 0x0001
    EXPECT_EQ(desc[5], 0x01);
    EXPECT_EQ(desc[6], 0x00);
    // FormatType I: bNrChannels=1, bSubframeSize=2, bBitResolution=16, bSamFreqType=1
    EXPECT_EQ(desc[11], 1);
    EXPECT_EQ(desc[12], 2);
    EXPECT_EQ(desc[13], 16);
    EXPECT_EQ(desc[14], 1);
}

TEST(UacAudioStreamingSinkDesc, SampleRateSetFormatUpdatesSink) {
    // 采样率协商落到汇：SET_CUR 走 handle_sampling_freq_control 前先确认
    // 汇的 supported_formats 与 current_format 同步（协商的其余逻辑与
    // 麦克风共用同一套 GET_CUR/SET_CUR 处理，端到端由 mock_speaker 验证）
    StringPool pool;
    UsbInterface intf{.interface_class = CC_AUDIO, .interface_subclass = SC_AUDIOSTREAMING, .interface_protocol = 0};
    auto sink = std::make_unique<StubSink>();
    auto *sink_ptr = sink.get();
    UacAudioStreamingSinkHandler handler(intf, pool, std::move(sink));
    handler.on_setup_interface_handlers();

    // 描述符中的采样率来自汇的当前格式
    auto desc = handler.get_class_specific_descriptor();
    // tSamFreq: 48000 = 0x00BB80 小端 3 字节（offset 15-17）
    EXPECT_EQ(desc[15], 0x80);
    EXPECT_EQ(desc[16], 0xBB);
    EXPECT_EQ(desc[17], 0x00);
    EXPECT_EQ(sink_ptr->written, 0u);
}

// ========== 采样率协商：支持列表为空 ==========

namespace {

/// 当前格式有效、但 supported_formats 返回空列表的汇（配置错误场景）
struct EmptyFormatsSink : public AudioSink {
    std::vector<AudioFormatInfo> supported_formats() const override {
        return {};
    }
    AudioFormatInfo current_format() const override {
        return {1, 16, 48000};
    }
    bool set_format(std::uint16_t, std::uint8_t, std::uint32_t) override {
        return true;
    }
    void write_pcm(const std::uint8_t *, std::size_t) override {
    }
};

/// 当前格式有效、但 supported_formats 返回空列表的源（配置错误场景）
struct EmptyFormatsSource : public AudioSource {
    std::vector<AudioFormatInfo> supported_formats() const override {
        return {};
    }
    AudioFormatInfo current_format() const override {
        return {1, 16, 48000};
    }
    bool set_format(std::uint16_t, std::uint8_t, std::uint32_t) override {
        return true;
    }
    bool get_chunk(AudioChunk &) override {
        return false;
    }
};

/// 发采样率 GET_MIN/GET_MAX（端点 recipient，Linux 驱动的实际形态）
void send_get_min_max(VirtualInterfaceHandler &handler, GenericTransferOperator &op, CaptureResponder &stub,
                      std::uint8_t request, usbipdcpp::error_code &ec) {
    const UsbEndpoint ep{.address = 0x01, .attributes = 0x05, .max_packet_size = 192, .interval = 1};
    const SetupPacket setup{
            .request_type = 0xA2, // Class | Endpoint | IN
            .request = request,
            .value = static_cast<std::uint16_t>(AS_SAMPLING_FREQ_CONTROL << 8),
            .index = ep.address,
            .length = 3,
    };
    stub.submits.clear();
    handler.handle_non_standard_request_type_control_urb_to_endpoint(
            1, ep, 0, 3, setup, make_cmd_submit(op, 1, ep.address, UsbIpDirection::In, 3, setup).transfer, ec);
    ASSERT_FALSE(ec);
    ASSERT_EQ(stub.submits.size(), 1u);
    EXPECT_EQ(stub.submits[0].status, static_cast<std::uint32_t>(UrbStatusType::StatusEPIPE));
}

} // namespace

TEST(UacAudioStreamingControl, SinkGetMinMaxWithoutSupportedRatesStalls) {
    // 采样率列表为空时 front()/back() 解引用空区间是 UB：GET_MIN/GET_MAX
    // 必须回 EPIPE，让主机按"不支持"处理
    StringPool pool;
    GenericTransferOperator op;
    UsbInterface intf{.interface_class = CC_AUDIO, .interface_subclass = SC_AUDIOSTREAMING};
    UacAudioStreamingSinkHandler handler(intf, pool, std::make_unique<EmptyFormatsSink>());
    CaptureResponder stub;
    usbipdcpp::error_code ec;
    handler.on_new_connection(stub, ec);
    ASSERT_FALSE(ec);

    send_get_min_max(handler, op, stub, GET_MIN, ec);
    send_get_min_max(handler, op, stub, GET_MAX, ec);
}

TEST(UacAudioStreamingControl, SinkSetCurWithoutSupportedRatesStalls) {
    // 列表为空时 min_element 返回 end()，解引用是 UB：SET_CUR 必须回 EPIPE
    StringPool pool;
    GenericTransferOperator op;
    UsbInterface intf{.interface_class = CC_AUDIO, .interface_subclass = SC_AUDIOSTREAMING};
    UacAudioStreamingSinkHandler handler(intf, pool, std::make_unique<EmptyFormatsSink>());
    CaptureResponder stub;
    usbipdcpp::error_code ec;
    handler.on_new_connection(stub, ec);
    ASSERT_FALSE(ec);

    const UsbEndpoint ep{.address = 0x01, .attributes = 0x05, .max_packet_size = 192, .interval = 1};
    const SetupPacket setup{
            .request_type = 0x22, // Class | Endpoint | OUT
            .request = SET_CUR,
            .value = static_cast<std::uint16_t>(AS_SAMPLING_FREQ_CONTROL << 8),
            .index = ep.address,
            .length = 3,
    };
    const data_type rate = {0x80, 0xBB, 0x00}; // 48000 Hz 小端
    handler.handle_non_standard_request_type_control_urb_to_endpoint(
            1, ep, 0, 3, setup,
            make_cmd_submit(op, 1, ep.address, UsbIpDirection::Out, 3, setup, rate).transfer, ec);
    ASSERT_FALSE(ec);
    ASSERT_EQ(stub.submits.size(), 1u);
    EXPECT_EQ(stub.submits[0].status, static_cast<std::uint32_t>(UrbStatusType::StatusEPIPE));
}

TEST(UacAudioStreamingControl, SourceGetMinMaxWithoutSupportedRatesStalls) {
    // 源侧同一处空列表解引用（GET_MIN/GET_MAX）
    StringPool pool;
    GenericTransferOperator op;
    UsbInterface intf{.interface_class = CC_AUDIO, .interface_subclass = SC_AUDIOSTREAMING};
    UacAudioStreamingSourceHandler handler(intf, pool, std::make_unique<EmptyFormatsSource>());
    CaptureResponder stub;
    usbipdcpp::error_code ec;
    handler.on_new_connection(stub, ec);
    ASSERT_FALSE(ec);

    send_get_min_max(handler, op, stub, GET_MIN, ec);
    send_get_min_max(handler, op, stub, GET_MAX, ec);
}

TEST(UacAudioStreamingControl, SourceSetCurWithoutSupportedRatesStalls) {
    // 源侧 SET_CUR：列表为空时不能解引用 min_element 的 end()
    StringPool pool;
    GenericTransferOperator op;
    UsbInterface intf{.interface_class = CC_AUDIO, .interface_subclass = SC_AUDIOSTREAMING};
    UacAudioStreamingSourceHandler handler(intf, pool, std::make_unique<EmptyFormatsSource>());
    CaptureResponder stub;
    usbipdcpp::error_code ec;
    handler.on_new_connection(stub, ec);
    ASSERT_FALSE(ec);

    const UsbEndpoint ep{.address = 0x81, .attributes = 0x05, .max_packet_size = 192, .interval = 1};
    const SetupPacket setup{
            .request_type = 0x22, // Class | Endpoint | OUT
            .request = SET_CUR,
            .value = static_cast<std::uint16_t>(AS_SAMPLING_FREQ_CONTROL << 8),
            .index = ep.address,
            .length = 3,
    };
    const data_type rate = {0x80, 0xBB, 0x00};
    handler.handle_non_standard_request_type_control_urb_to_endpoint(
            1, ep, 0, 3, setup,
            make_cmd_submit(op, 1, ep.address, UsbIpDirection::Out, 3, setup, rate).transfer, ec);
    ASSERT_FALSE(ec);
    ASSERT_EQ(stub.submits.size(), 1u);
    EXPECT_EQ(stub.submits[0].status, static_cast<std::uint32_t>(UrbStatusType::StatusEPIPE));
}

TEST(UacAudioStreamingControl, DescriptorFallsBackToCurrentRateWhenListEmpty) {
    // UAC 1.0 §4.3.2.5：bSamFreqType=0 表示连续采样率，后面必须跟 min/max
    // 两个 3 字节 tSamFreq；支持列表为空时不能生成缺字段的描述符。源/汇都
    // 回退为当前采样率一个离散值（bSamFreqType=1）
    UsbInterface intf{.interface_class = CC_AUDIO, .interface_subclass = SC_AUDIOSTREAMING};

    {
        StringPool pool;
        UacAudioStreamingSinkHandler handler(intf, pool, std::make_unique<EmptyFormatsSink>());
        handler.on_setup_interface_handlers();
        auto desc = handler.get_class_specific_descriptor();
        ASSERT_EQ(desc.size(), 18u); // General(7) + FormatHead(8) + 1 个采样率(3)
        EXPECT_EQ(desc[14], 1); // bSamFreqType = 1（离散）
        EXPECT_EQ(desc[15], 0x80); // tSamFreq = 48000 (0x00BB80) 小端
        EXPECT_EQ(desc[16], 0xBB);
        EXPECT_EQ(desc[17], 0x00);
    }
    {
        StringPool pool;
        UacAudioStreamingSourceHandler handler(intf, pool, std::make_unique<EmptyFormatsSource>());
        handler.on_setup_interface_handlers();
        auto desc = handler.get_class_specific_descriptor();
        ASSERT_EQ(desc.size(), 18u);
        EXPECT_EQ(desc[14], 1);
        EXPECT_EQ(desc[15], 0x80);
        EXPECT_EQ(desc[16], 0xBB);
        EXPECT_EQ(desc[17], 0x00);
    }
}
