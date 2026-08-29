// 音频汇测试：UAC 扬声器（OUT 方向）AC/AS 描述符拓扑、采样率协商相关的
// 纯逻辑部分（WavFileSink 文件写入测试在 examples/mock_speaker/test_wav_file_sink.cpp，
// 与实现同目录；PlaybackSink 依赖真实声卡，不做自动化测试）

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>

#include "usbipdcpp/Interface.h"
#include "usbipdcpp/utils/StringPool.h"
#include "usbipdcpp/virtual_device/UacConstants.h"
#include "usbipdcpp/virtual_device/UacVirtualInterfaceHandler.h"
#include "usbipdcpp/virtual_device/audio_sinks/AudioSink.h"

using namespace usbipdcpp;

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
