// UVC 视频流接口（VS）数据面测试：iso IN 取帧拆包、帧时钟空包门控、
// 流状态（SET_INTERFACE + VS_COMMIT）控制。控制面（描述符、PROBE 协商）
// 不在本文件

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>

#include "test_utils.h"

#include "usbipdcpp/Interface.h"
#include "usbipdcpp/utils/StringPool.h"
#include "usbipdcpp/virtual_device/UvcConstants.h"
#include "usbipdcpp/virtual_device/UvcVirtualInterfaceHandler.h"
#include "usbipdcpp/virtual_device/video_sources/VideoSource.h"

using namespace usbipdcpp;
using namespace usbipdcpp::test;

namespace {

// 固定帧大小的测试视频源（YUY2 640x480，帧间隔 1/30s = 333333×100ns）。
// frame_size 可配（测跨 URB 大帧），fail_next 模拟取帧失败
struct StubSource : VideoSource {
    explicit StubSource(std::size_t frame_size = 64) : data(frame_size) {
        for (std::size_t i = 0; i < data.size(); i++) {
            data[i] = static_cast<std::uint8_t>(i);
        }
    }

    std::vector<VideoFormatInfo> supported_formats() const override {
        return {{.fourcc = 0x34363259, // YUY2
                 .width = 640,
                 .height = 480,
                 .max_frame_size = 64,
                 .default_frame_interval = 333333,
                 .min_frame_interval = 333333,
                 .max_frame_interval = 400000,
                 .bits_per_pixel = 16}};
    }
    VideoFormatInfo current_format() const override { return fmt; }
    bool set_format(std::uint32_t fourcc, std::uint16_t width, std::uint16_t height,
                    std::uint32_t frame_interval) override {
        fmt = {.fourcc = fourcc,
               .width = width,
               .height = height,
               .max_frame_size = 64,
               .default_frame_interval = frame_interval,
               .min_frame_interval = 333333,
               .max_frame_interval = 400000,
               .bits_per_pixel = 16};
        interval = frame_interval;
        return true;
    }
    bool get_frame(VideoFrame &frame) override {
        if (fail_next) {
            fail_next = false;
            return false;
        }
        frame = {data.data(), data.size(), true};
        return true;
    }
    std::size_t max_frame_size() const override { return data.size(); }
    std::uint32_t frame_interval() const override { return interval; }

    VideoFormatInfo fmt{0x34363259, 640, 480, 64, 333333, 333333, 400000, 16};
    std::uint32_t interval = 333333;
    data_type data;
    bool fail_next = false;
};

struct UvcTestEnv {
    StringPool pool;
    // 传输分配器：必须声明在 stub 之前（成员逆序析构，op 后死）——
    // submits 里的 RetSubmit 持 TransferHandle，析构时要用 op 释放
    GenericTransferOperator op;
    UsbInterface intf{.interface_class = 0x0E, .interface_subclass = 0x02, .interface_protocol = 0x00};
    StubSource *source = nullptr;
    UvcVideoStreamingHandler handler{intf, pool, std::unique_ptr<StubSource>()};
    CaptureResponder stub;
    usbipdcpp::error_code ec;

    explicit UvcTestEnv(std::size_t frame_size = 64) :
        source(new StubSource(frame_size)), handler(intf, pool, std::unique_ptr<StubSource>(source)) {
        handler.on_new_connection(stub, ec);
    }

    // 走 Linux 驱动的开流顺序：SET_INTERFACE(1) → VS_COMMIT(SET_CUR) →
    // SET_INTERFACE(1)（第二次才真正 streaming）
    void start_streaming() {
        std::uint32_t status = 0;
        handler.request_set_interface(1, &status);
        EXPECT_EQ(status, 0u);

        // VS_COMMIT 数据：bFormatIndex=1, bFrameIndex=1, dwFrameInterval=333333
        data_type commit(26, 0);
        commit[2] = 1;
        commit[3] = 1;
        commit[4] = 0x15; // 333333 = 0x00051615 小端
        commit[5] = 0x16;
        commit[6] = 0x05;
        const SetupPacket setup{
                .request_type = 0x21, // Class | Interface | OUT
                .request = 0x01,      // SET_CUR
                .value = static_cast<std::uint16_t>(VS_COMMIT_CONTROL << 8), // wValue 高字节 = 控制选择子
                .index = 0,
                .length = 26,
        };
        handler.handle_non_standard_request_type_control_urb(
                1, UsbEndpoint::get_ep0_in(UsbSpeed::High), 0, 26, setup,
                make_cmd_submit(op, 1, 0x00, UsbIpDirection::Out, 26, setup, commit).transfer, ec);
        EXPECT_FALSE(ec);
        EXPECT_EQ(stub.submits.back().header.seqnum, 1u); // commit ack

        handler.request_set_interface(1, &status);
        EXPECT_EQ(status, 0u);
    }

    // 发一个 iso IN URB（默认 4 包 × 512 字节）
    void deliver_iso_in(std::uint32_t seqnum, int num_packets = 4) {
        auto submit = make_cmd_submit(op, seqnum, 0x01, UsbIpDirection::In, num_packets * 512, {}, {}, num_packets);
        auto *trx = GenericTransfer::from_handle(submit.transfer.get());
        for (int i = 0; i < num_packets; i++) {
            trx->iso_descriptors[i] = {.offset = static_cast<std::uint32_t>(i * 512),
                                       .length = 512,
                                       .actual_length = 0,
                                       .status = 0};
        }
        handler.handle_isochronous_transfer(seqnum,
                                            UsbEndpoint{.address = 0x81, .attributes = 0x05, .max_packet_size = 512},
                                            0, num_packets * 512, std::move(submit.transfer), num_packets, ec);
        EXPECT_FALSE(ec);
    }
};

} // namespace

TEST(TestUvcHandler, IsoInBeforeCommitStalls) {
    // 未开流（未 SET_INTERFACE / 未 COMMIT）时 iso IN 回 EPIPE
    UvcTestEnv env;

    env.deliver_iso_in(2);

    ASSERT_GE(env.stub.submits.size(), 1u);
    EXPECT_EQ(env.stub.submits.back().header.seqnum, 2u);
    EXPECT_EQ(env.stub.submits.back().status, static_cast<std::uint32_t>(UrbStatusType::StatusEPIPE));
    EXPECT_EQ(env.stub.submits.back().actual_length, 0u);
}

TEST(TestUvcHandler, IsoInServesFrameWithHeaders) {
    // 开流后 iso IN 从源取帧拆包：每包 2 字节 payload header + 数据，
    // 首包带 FID 位、末包带 EOF 位（单包传完整帧），iso 描述符 status 清零
    UvcTestEnv env;
    env.start_streaming();

    env.deliver_iso_in(2);

    // 应答：seqnum 2，总发送 = 2 字节 header + 64 字节帧
    const auto *ret = &env.stub.submits.back();
    ASSERT_EQ(ret->header.seqnum, 2u);
    EXPECT_EQ(ret->status, 0u);
    EXPECT_EQ(ret->actual_length, 66u);
    EXPECT_EQ(ret->number_of_packets, 4u);

    auto *trx = GenericTransfer::from_handle(ret->transfer.get());
    ASSERT_EQ(trx->iso_descriptors.size(), 4u);
    // 第一包：header(2) + 全部帧数据，FID(0x01) + EOF(0x80)
    EXPECT_EQ(trx->iso_descriptors[0].actual_length, 66u);
    EXPECT_EQ(trx->iso_descriptors[0].status, 0); // 初始 -EXDEV，已清零
    EXPECT_EQ(trx->data[0], UVC_PAYLOAD_HEADER_SIZE);
    EXPECT_EQ(trx->data[1], UVC_PAYLOAD_HEADER_FID | UVC_PAYLOAD_HEADER_EOF);
    data_type expected_frame(64);
    for (std::size_t i = 0; i < 64; i++) {
        expected_frame[i] = static_cast<std::uint8_t>(i);
    }
    EXPECT_EQ(data_type(trx->data.begin() + UVC_PAYLOAD_HEADER_SIZE, trx->data.begin() + 66), expected_frame);
    // 其余包无数据
    for (int i = 1; i < 4; i++) {
        EXPECT_EQ(trx->iso_descriptors[i].actual_length, 0u);
        EXPECT_EQ(trx->iso_descriptors[i].status, 0);
    }
}

TEST(TestUvcHandler, FrameIntervalGateEmitsEmptyPacket) {
    // 第一帧传完立即再发 iso IN：帧间隔（33ms）未到 → 空包（actual_length=0），
    // 避免主机拉得快时快放
    UvcTestEnv env;
    env.start_streaming();

    env.deliver_iso_in(2); // 传完第一帧
    env.deliver_iso_in(3); // 立即第二帧：帧间隔未到

    ASSERT_GE(env.stub.submits.size(), 2u);
    const auto *ret = &env.stub.submits.back();
    ASSERT_EQ(ret->header.seqnum, 3u);
    EXPECT_EQ(ret->status, 0u);
    EXPECT_EQ(ret->actual_length, 0u); // 空包
}

TEST(TestUvcHandler, LargeFrameSpansMultipleUrbs) {
    // 大帧（3000 字节）一 URB 装不下：跨多个 URB 续传（frame_offset_ 累积），
    // 帧时钟检查在帧未传完时跳过，EOF 只出现在最后一包的末段，各 URB 数据
    // 拼接后等于整帧
    UvcTestEnv env(3000);
    env.start_streaming();

    // 2 包 URB 容量 = 2×510 = 1020 字节/次；3000 = 1020 + 1020 + 960
    env.deliver_iso_in(2, 2);
    env.deliver_iso_in(3, 2);
    env.deliver_iso_in(4, 2);

    // 三个 URB 的数据按序拼接 = 整帧
    data_type assembled;
    std::size_t eof_found_at = 0;
    for (std::uint32_t seq : {2u, 3u, 4u}) {
        const auto *ret = &env.stub.submits[seq - 1]; // submits[0] 是 commit ack
        ASSERT_EQ(ret->header.seqnum, seq);
        EXPECT_EQ(ret->status, 0u);
        auto *trx = GenericTransfer::from_handle(ret->transfer.get());
        // 两包各 2 字节头 + 数据
        ASSERT_EQ(trx->iso_descriptors.size(), 2u);
        for (auto &iso : trx->iso_descriptors) {
            ASSERT_GE(iso.actual_length, UVC_PAYLOAD_HEADER_SIZE);
            auto &d = trx->data;
            if (d[static_cast<std::size_t>(iso.offset) + 1] & UVC_PAYLOAD_HEADER_EOF) {
                eof_found_at = assembled.size() + iso.actual_length - UVC_PAYLOAD_HEADER_SIZE;
            }
            assembled.insert(assembled.end(), d.begin() + iso.offset + UVC_PAYLOAD_HEADER_SIZE,
                             d.begin() + iso.offset + iso.actual_length);
        }
    }
    EXPECT_EQ(assembled.size(), 3000u);
    EXPECT_EQ(eof_found_at, 3000u); // EOF 只在整帧末尾

    data_type expected(3000);
    for (std::size_t i = 0; i < expected.size(); i++) {
        expected[i] = static_cast<std::uint8_t>(i);
    }
    EXPECT_EQ(assembled, expected);
}

TEST(TestUvcHandler, GetFrameFailureStalls) {
    // 源取帧失败（get_frame 返回 false）：iso IN 回 EPIPE（驱动重试）
    UvcTestEnv env;
    env.source->fail_next = true;
    env.start_streaming();

    env.deliver_iso_in(2);

    ASSERT_GE(env.stub.submits.size(), 1u);
    EXPECT_EQ(env.stub.submits.back().header.seqnum, 2u);
    EXPECT_EQ(env.stub.submits.back().status, static_cast<std::uint32_t>(UrbStatusType::StatusEPIPE));
}

TEST(TestUvcHandler, ShortCommitFallsBackToDefaults) {
    // VS_COMMIT 数据不足 26 字节（最小解析长度）：不拒绝（容错不完整的主机
    // 提交），格式保持默认（probe_data_ 初值），流照常可开
    UvcTestEnv env;

    std::uint32_t status = 0;
    env.handler.request_set_interface(1, &status);
    EXPECT_EQ(status, 0u);

    data_type commit(10, 0); // 过短
    const SetupPacket setup{
            .request_type = 0x21,
            .request = 0x01,
            .value = static_cast<std::uint16_t>(VS_COMMIT_CONTROL << 8),
            .index = 0,
            .length = 10,
    };
    env.handler.handle_non_standard_request_type_control_urb(
            1, UsbEndpoint::get_ep0_in(UsbSpeed::High), 0, 10, setup,
            make_cmd_submit(env.op, 1, 0x00, UsbIpDirection::Out, 10, setup, commit).transfer, env.ec);
    EXPECT_FALSE(env.ec);
    EXPECT_EQ(env.stub.submits.back().status, 0u); // commit ack 成功

    env.handler.request_set_interface(1, &status);
    env.deliver_iso_in(2);

    // 流正常取帧（默认格式）
    ASSERT_GE(env.stub.submits.size(), 2u);
    EXPECT_EQ(env.stub.submits.back().header.seqnum, 2u);
    EXPECT_EQ(env.stub.submits.back().status, 0u);
    EXPECT_GT(env.stub.submits.back().actual_length, 0u);
}
