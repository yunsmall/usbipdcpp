// UAC 扬声器（OUT 收流方向）数据面测试：iso OUT 传输经 TransferScheduler
// 延迟调度后写 AudioSink 并应答。控制面（AC 拓扑、采样率协商）在
// test_audio_sinks.cpp，本文件只测收流路径与流状态切换

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
#include "usbipdcpp/virtual_device/audio_sinks/AudioSink.h"

using namespace usbipdcpp;
using namespace usbipdcpp::test;

namespace {

/// 测试用内存汇：记录写入内容，只支持单声道 16 位 48000。
/// 写端是 TransferScheduler 调度线程，测试线程轮询读快照，需加锁
struct RecordingSink : public AudioSink {
    std::vector<AudioFormatInfo> supported_formats() const override {
        return {{1, 16, 48000}};
    }
    AudioFormatInfo current_format() const override { return format; }
    bool set_format(std::uint16_t channels, std::uint8_t bits_per_sample, std::uint32_t sample_rate) override {
        if (bits_per_sample != 16 || channels != 1) {
            return false;
        }
        format = {channels, bits_per_sample, sample_rate};
        return true;
    }
    void write_pcm(const std::uint8_t *data, std::size_t size) override {
        std::lock_guard lock(mutex);
        pcm.insert(pcm.end(), data, data + size);
    }
    AudioFormatInfo format{1, 16, 48000};
    data_type snapshot() {
        std::lock_guard lock(mutex);
        return pcm;
    }
    std::mutex mutex;
    data_type pcm;
};

// 构造扬声器设备（对齐 examples/mock_speaker）：AC（中断 IN 0x82）+
// AS（alt0 空 / alt1 ISO OUT 0x01），setup_speaker 注册 handler 并启用调度器
struct SpeakerFixture {
    std::shared_ptr<UsbDevice> device;
    VirtualDeviceHandler *dh = nullptr;
    RecordingSink *sink = nullptr;
    UsbEndpoint ep_iso_out;
    // 传输分配器：fixture 成员（fixture 先于测试局部 stub 声明，析构在后）——
    // submits 里的 RetSubmit 持 TransferHandle，析构时要用 op 释放
    GenericTransferOperator op;
};

SpeakerFixture make_speaker_device(StringPool &string_pool) {
    std::vector<UsbInterface> interfaces = {
            UsbInterface{
                    .interface_class = CC_AUDIO,
                    .interface_subclass = SC_AUDIOCONTROL,
                    .interface_protocol = 0x00,
                    .endpoints = {{UsbEndpoint{
                            .address = 0x82, // IN, interrupt for status
                            .attributes = 0x03,
                            .max_packet_size = 2,
                            .interval = 4,
                    }}},
            },
            UsbInterface{
                    .interface_class = CC_AUDIO,
                    .interface_subclass = SC_AUDIOSTREAMING,
                    .interface_protocol = 0x00,
                    .endpoints = {{}, // alt 0: zero bandwidth
                                  {UsbEndpoint{
                                          .address = 0x01, // OUT, endpoint 1
                                          .attributes = static_cast<std::uint8_t>(EndpointAttributes::Isochronous) |
                                                        static_cast<std::uint8_t>(IsoSyncType::Adaptive),
                                          .max_packet_size = 192,
                                          .interval = 4,
                                  }}},
            },
    };
    auto device = UsbDevice::make("/test/mock_speaker", 0x1234, 0x5683, std::move(interfaces), 1, 1, 0,
                                  "/usbipdcpp/mock_speaker", UsbSpeed::High, 0x0100);
    auto sink = std::make_unique<RecordingSink>();
    auto *sink_ptr = sink.get();
    UacDeviceHelper::setup_speaker(device, string_pool, std::move(sink), {});
    auto *dh = dynamic_cast<VirtualDeviceHandler *>(device->handler.get());
    return {device, dh, sink_ptr, device->interfaces[1].endpoints[1][0]};
}

// 发 SET_INTERFACE 控制请求切换 AS 接口 alt（streaming 状态机开关）
void set_as_interface(SpeakerFixture &fx, std::uint16_t alt, CaptureResponder &stub, usbipdcpp::error_code &ec) {
    const SetupPacket setup{
            .request_type = 0x01, // Host | Interface | OUT
            .request = 0x0B,      // SET_INTERFACE
            .value = alt,
            .index = 1, // AS 接口号
            .length = 0,
    };
    fx.dh->receive_urb(make_cmd_submit(fx.op, 100, 0x00, UsbIpDirection::Out, 0, setup), fx.device->ep0_in,
                       std::nullopt, ec);
    ASSERT_FALSE(ec);
}

// 轮询等待调度线程消费（iso OUT 按数据时长延迟调度，毫秒级）
void wait_for_sink_data(SpeakerFixture &fx, std::size_t expected,
                        std::chrono::milliseconds timeout = std::chrono::milliseconds(200)) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (fx.sink->snapshot().size() < expected && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}

} // namespace

TEST(TestUacSpeakerStream, IsoOutBeforeStreamingStalls) {
    // 未 SET_INTERFACE（streaming=false，alt0 空端点）时 iso OUT 回 EPIPE
    StringPool string_pool;
    auto fx = make_speaker_device(string_pool);
    CaptureResponder stub;
    usbipdcpp::error_code ec;
    fx.dh->on_new_connection(stub, ec);
    ASSERT_FALSE(ec);

    fx.dh->receive_urb(make_cmd_submit(fx.op, 1, 0x01, UsbIpDirection::Out, 384, {}, {}, 2), fx.ep_iso_out,
                       fx.device->interfaces[1], ec);
    ASSERT_FALSE(ec);

    ASSERT_EQ(stub.submits.size(), 1u);
    EXPECT_EQ(stub.submits[0].header.seqnum, 1u);
    EXPECT_EQ(stub.submits[0].status, static_cast<std::uint32_t>(UrbStatusType::StatusEPIPE));
    EXPECT_EQ(stub.submits[0].actual_length, 0u);
    EXPECT_EQ(fx.sink->snapshot().size(), 0u); // 未进流，sink 不消费

    fx.dh->on_disconnection(ec);
}

TEST(TestUacSpeakerStream, IsoOutWritesSinkThenResponds) {
    // SET_INTERFACE alt=1 后：iso OUT 经调度器按数据时长延迟（4ms）调度，
    // sink 收到紧凑排列的两包 PCM，应答 status 0、actual_length=总数据量、
    // iso 描述符 status 清零（内核驱动要求）
    StringPool string_pool;
    auto fx = make_speaker_device(string_pool);
    CaptureResponder stub;
    usbipdcpp::error_code ec;
    fx.dh->on_new_connection(stub, ec);
    ASSERT_FALSE(ec);

    set_as_interface(fx, 1, stub, ec); // alt 1：开流

    // 两个 iso 包各 192 字节（48000Hz 16bit 单声道 2ms），数据紧凑排列
    data_type payload(384);
    for (std::size_t i = 0; i < payload.size(); i++) {
        payload[i] = static_cast<std::uint8_t>(i);
    }
    auto submit = make_cmd_submit(fx.op, 2, 0x01, UsbIpDirection::Out, 384, {}, payload, 2);
    auto *trx = GenericTransfer::from_handle(submit.transfer.get());
    trx->iso_descriptors[0] = {.offset = 0, .length = 192, .actual_length = 0, .status = 0};
    trx->iso_descriptors[1] = {.offset = 192, .length = 192, .actual_length = 0, .status = 0};
    fx.dh->receive_urb(std::move(submit), fx.ep_iso_out, fx.device->interfaces[1], ec);
    ASSERT_FALSE(ec);

    // 调度线程在数据时长（384B / 96000B/s = 4ms）后写 sink 并应答
    wait_for_sink_data(fx, payload.size());
    EXPECT_EQ(fx.sink->snapshot(), payload);

    // 断连（join 调度线程）后再读应答：process_iso_out 在调度线程写 submits，
    // 必须先停线程建立 happens-before，否则 TSan 报数据竞争
    fx.dh->on_disconnection(ec);

    // 从应答里找 iso 传输（seqnum=2），SET_INTERFACE 的应答是 seqnum=100
    ASSERT_EQ(stub.submits.size(), 2u);
    const auto *ret = &stub.submits[0];
    if (ret->header.seqnum != 2u) {
        ret = &stub.submits[1];
    }
    ASSERT_EQ(ret->header.seqnum, 2u);
    EXPECT_EQ(ret->status, 0u);
    EXPECT_EQ(ret->actual_length, 384u);
    EXPECT_EQ(ret->number_of_packets, 2u);

    // iso 描述符 status 已清零（初始 -EXDEV，不清理内核认为包失败），
    // actual_length 按实际写入填充
    auto *resp_trx = GenericTransfer::from_handle(ret->transfer.get());
    ASSERT_EQ(resp_trx->iso_descriptors.size(), 2u);
    EXPECT_EQ(resp_trx->iso_descriptors[0].status, 0);
    EXPECT_EQ(resp_trx->iso_descriptors[0].actual_length, 192u);
    EXPECT_EQ(resp_trx->iso_descriptors[1].status, 0);
    EXPECT_EQ(resp_trx->iso_descriptors[1].actual_length, 192u);
}

TEST(TestUacSpeakerStream, SwitchingBackToAlt0StopsStreaming) {
    // 切回 alt0 后 streaming=false：iso OUT 又回 EPIPE（流状态随 SET_INTERFACE 复位）
    StringPool string_pool;
    auto fx = make_speaker_device(string_pool);
    CaptureResponder stub;
    usbipdcpp::error_code ec;
    fx.dh->on_new_connection(stub, ec);
    ASSERT_FALSE(ec);

    set_as_interface(fx, 1, stub, ec);
    set_as_interface(fx, 0, stub, ec); // 关流

    fx.dh->receive_urb(make_cmd_submit(fx.op, 2, 0x01, UsbIpDirection::Out, 192, {}, {}, 1), fx.ep_iso_out,
                       fx.device->interfaces[1], ec);
    ASSERT_FALSE(ec);

    // 最后一条应答是 iso 传输（seqnum=2），EPIPE
    ASSERT_EQ(stub.submits.size(), 3u);
    EXPECT_EQ(stub.submits[2].header.seqnum, 2u);
    EXPECT_EQ(stub.submits[2].status, static_cast<std::uint32_t>(UrbStatusType::StatusEPIPE));
    EXPECT_EQ(fx.sink->snapshot().size(), 0u);

    fx.dh->on_disconnection(ec);
}

TEST(TestUacSpeakerStream, MuteZeroesReceivedPcm) {
    // AC Feature Unit 静音（SET_CUR(FU_MUTE)）后：收流侧把 PCM 清零再写 sink
    // （Windows 音量滑块路径），sink 收到全 0
    StringPool string_pool;
    auto fx = make_speaker_device(string_pool);
    CaptureResponder stub;
    usbipdcpp::error_code ec;
    fx.dh->on_new_connection(stub, ec);
    ASSERT_FALSE(ec);

    set_as_interface(fx, 1, stub, ec); // alt 1：开流

    // SET_CUR(FU_MUTE) 到 AC 接口：wValue 高字节 = 控制选择子（声道 0），
    // wIndex 高字节 = Feature Unit 实体，数据 1 字节 = 静音
    const SetupPacket mute_setup{
            .request_type = 0x21, // Class | Interface | OUT
            .request = 0x01,      // SET_CUR
            .value = static_cast<std::uint16_t>(FU_MUTE_CONTROL << 8),
            .index = static_cast<std::uint16_t>(UAC_ENTITY_FEATURE_UNIT << 8),
            .length = 1,
    };
    fx.dh->receive_urb(make_cmd_submit(fx.op, 101, 0x00, UsbIpDirection::Out, 1, mute_setup, {1}),
                       fx.device->ep0_in, std::nullopt, ec);
    ASSERT_FALSE(ec);

    // 非零 PCM 收流
    auto submit = make_cmd_submit(fx.op, 2, 0x01, UsbIpDirection::Out, 192, {}, data_type(192, 0xAB), 1);
    auto *trx = GenericTransfer::from_handle(submit.transfer.get());
    trx->iso_descriptors[0] = {.offset = 0, .length = 192, .actual_length = 0, .status = 0};
    fx.dh->receive_urb(std::move(submit), fx.ep_iso_out, fx.device->interfaces[1], ec);
    ASSERT_FALSE(ec);

    wait_for_sink_data(fx, 192);
    EXPECT_EQ(fx.sink->snapshot(), data_type(192, 0)); // 全部静音

    fx.dh->on_disconnection(ec);
}

TEST(TestUacSpeakerStream, DeclaredIsoLengthExceedsActualData) {
    // iso 描述符声明 length 大于实际数据（异常主机/截断包）：按实际可用数据
    // 消费（take = min(声明, 剩余)），应答 actual_length = 实际收到字节
    StringPool string_pool;
    auto fx = make_speaker_device(string_pool);
    CaptureResponder stub;
    usbipdcpp::error_code ec;
    fx.dh->on_new_connection(stub, ec);
    ASSERT_FALSE(ec);

    set_as_interface(fx, 1, stub, ec);

    data_type payload(100, 0x11);
    auto submit = make_cmd_submit(fx.op, 2, 0x01, UsbIpDirection::Out, 100, {}, payload, 1);
    auto *trx = GenericTransfer::from_handle(submit.transfer.get());
    trx->iso_descriptors[0] = {.offset = 0, .length = 192, .actual_length = 0, .status = 0}; // 声明 192 > 实际 100
    fx.dh->receive_urb(std::move(submit), fx.ep_iso_out, fx.device->interfaces[1], ec);
    ASSERT_FALSE(ec);

    wait_for_sink_data(fx, 100);
    EXPECT_EQ(fx.sink->snapshot(), payload);

    fx.dh->on_disconnection(ec);
    const auto *ret = &stub.submits[0];
    if (ret->header.seqnum != 2u) {
        ret = &stub.submits[1];
    }
    EXPECT_EQ(ret->header.seqnum, 2u);
    EXPECT_EQ(ret->actual_length, 100u); // 实际字节，非声明值
}
