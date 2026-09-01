// RNDIS 虚拟网卡测试：控制面（封装命令握手、OID 查询/设置、状态机）与
// 数据面（RNDIS_MSG_PACKET 封装/解封装、多包聚合、ZLP 填充）

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <utility>

#include "test_utils.h"

#include "usbipdcpp/Interface.h"
#include "usbipdcpp/utils/StringPool.h"
#include "usbipdcpp/virtual_device/RndisConstants.h"
#include "usbipdcpp/virtual_device/RndisVirtualInterfaceHandler.h"
#include "usbipdcpp/virtual_device/network_backends/NetworkBackend.h"

using namespace usbipdcpp;
using namespace usbipdcpp::test;

namespace {

// 记录收到的帧的测试后端
struct StubBackend : NetworkBackend {
    void send_frame(const std::uint8_t *data, std::size_t size) override {
        frames.emplace_back(data, data + size);
    }
    std::vector<data_type> frames;
};

constexpr std::uint8_t EP_NOTIFY = 0x83;
constexpr std::uint8_t EP_IN = 0x81;
constexpr std::uint8_t EP_OUT = 0x02;
// 对齐 examples/mock_rndis 的默认 MAC
const std::array<std::uint8_t, 6> TEST_MAC = {0x02, 0x10, 0x83, 0x00, 0x00, 0x02};

// 最小可用的 RNDIS 双接口（通信 + 数据）+ handler
struct RndisTestEnv {
    StringPool pool;
    // 传输分配器：必须声明在 stub 之前（成员逆序析构，op 后死）——
    // submits 里的 RetSubmit 持 TransferHandle，析构时要用 op 释放
    GenericTransferOperator op;
    StubBackend backend;
    UsbInterface comm_intf{.interface_class = 0x02,
                           .interface_subclass = 0x02, // ACM（RNDIS 外壳）
                           .interface_protocol = 0xFF, // Vendor
                           .endpoints = {{UsbEndpoint{.address = EP_NOTIFY,
                                                       .attributes = 0x03,
                                                       .max_packet_size = 8,
                                                       .interval = 32}}}};
    UsbInterface data_intf{.interface_class = 0x0A,
                           .interface_subclass = 0x00,
                           .interface_protocol = 0x00,
                           .endpoints = {{UsbEndpoint{.address = EP_IN,
                                                       .attributes = 0x02,
                                                       .max_packet_size = 64,
                                                       .interval = 0},
                                          UsbEndpoint{.address = EP_OUT,
                                                       .attributes = 0x02,
                                                       .max_packet_size = 64,
                                                       .interval = 0}}}};
    RndisCommunicationInterfaceHandler comm;
    RndisDataInterfaceHandler data;
    CaptureResponder stub;
    usbipdcpp::error_code ec;

    explicit RndisTestEnv(UsbSpeed speed = UsbSpeed::Full) :
        comm(comm_intf, pool, TEST_MAC, speed), data(data_intf, pool, &backend, &comm) {
        // 连接失败场景不在本测试范围（on_new_connection 正常路径不报错），
        // 构造函数里不能 ASSERT（非 void 返回），错误留存在 ec 成员
        comm.on_new_connection(stub, ec);
        data.on_new_connection(stub, ec);
    }

    // 找 seqnum 对应的 RET_SUBMIT（找不到返回 nullptr）
    const UsbIpResponse::UsbIpRetSubmit *find_submit(std::uint32_t seqnum) const {
        for (auto &s : stub.submits) {
            if (s.header.seqnum == seqnum) {
                return &s;
            }
        }
        return nullptr;
    }
};

// ========== 控制消息构造（主机→设备，LE 序列化） ==========

data_type make_init_msg(std::uint32_t request_id = 1) {
    data_type m;
    vector_append_to_le(m, rndis_msg(RndisMessageType::Init), 24u, request_id, 1u, 0u, 0u);
    return m;
}

data_type make_query_msg(std::uint32_t oid, std::uint32_t request_id = 1) {
    data_type m;
    // 无输入缓冲：InformationBufferOffset=20（相对第 8 字节）
    vector_append_to_le(m, rndis_msg(RndisMessageType::Query), 28u, request_id, oid, 0u, 20u, 0u);
    return m;
}

data_type make_set_msg(std::uint32_t oid, const data_type &buf, std::uint32_t request_id = 1) {
    data_type m;
    // 注意 28+buf.size() 必须显式转 u32：vector_append_to_le 按 sizeof 序列化，
    // size_t 会写 8 字节导致字段错位
    vector_append_to_le(m, rndis_msg(RndisMessageType::Set), static_cast<std::uint32_t>(28 + buf.size()), request_id,
                        oid, static_cast<std::uint32_t>(buf.size()), 20u, 0u);
    m.insert(m.end(), buf.begin(), buf.end());
    return m;
}

data_type make_keepalive_msg(std::uint32_t request_id = 1) {
    data_type m;
    vector_append_to_le(m, rndis_msg(RndisMessageType::Keepalive), 12u, request_id);
    return m;
}

data_type make_halt_msg() {
    data_type m;
    vector_append_to_le(m, rndis_msg(RndisMessageType::Halt), 12u, 1u);
    return m;
}

data_type make_reset_msg() {
    data_type m;
    vector_append_to_le(m, rndis_msg(RndisMessageType::Reset), 12u, 0u);
    return m;
}

// ========== 控制请求投递 ==========

// SEND_ENCAPSULATED_COMMAND（OUT 类请求，数据阶段 = 整条 RNDIS 消息）
void send_command(RndisTestEnv &env, const data_type &msg, std::uint32_t seqnum = 1) {
    const SetupPacket setup{
            .request_type = 0x21, // Class | Interface | OUT
            .request = 0x00,      // SEND_ENCAPSULATED_COMMAND
            .value = 0,
            .index = 0,
            .length = static_cast<std::uint16_t>(msg.size()),
    };
    env.comm.handle_non_standard_request_type_control_urb(
            seqnum, UsbEndpoint::get_ep0_in(UsbSpeed::Full), 0, msg.size(), setup,
            make_cmd_submit(env.op, seqnum, 0x00, UsbIpDirection::Out, msg.size(), setup, msg).transfer, env.ec);
    ASSERT_FALSE(env.ec);
}

// GET_ENCAPSULATED_RESPONSE（IN 类请求），返回取到的响应字节（EPIPE 时返回空）
data_type get_response(RndisTestEnv &env, std::uint32_t seqnum) {
    const SetupPacket setup{
            .request_type = 0xA1, // Class | Interface | IN
            .request = 0x01,      // GET_ENCAPSULATED_RESPONSE
            .value = 0,
            .index = 0,
            .length = 1558, // 主机缓冲（对齐 RNDIS_MAX_TOTAL_SIZE）
    };
    auto submit = make_cmd_submit(env.op, seqnum, 0x00, UsbIpDirection::In, 1558, setup);
    env.comm.handle_non_standard_request_type_control_urb(
            seqnum, UsbEndpoint::get_ep0_in(UsbSpeed::Full), 0, 1558, setup, std::move(submit.transfer), env.ec);
    EXPECT_FALSE(env.ec);
    if (env.ec) {
        return {};
    }
    const auto *ret = env.find_submit(seqnum);
    if (ret == nullptr) {
        return {};
    }
    return ret_submit_data(*ret);
}

// ========== 响应解析辅助 ==========

// 解析 QUERY_C：返回 (status, payload)
std::pair<std::uint32_t, data_type> parse_query_cmplt(const data_type &resp) {
    EXPECT_GE(resp.size(), 24u);
    auto status = read_le32_at(resp, 12);
    auto payload_len = read_le32_at(resp, 16);
    auto payload_off = read_le32_at(resp, 20);
    EXPECT_EQ(payload_off, 16u); // 数据紧跟 24 字节头
    return {status, data_type(resp.begin() + 24, resp.begin() + 24 + payload_len)};
}

// ========== 数据包构造（主机方向：24 字节头，DataOffset=16） ==========

// 单条 RNDIS_MSG_PACKET（主机→设备布局：头 6 个字段 24 字节，数据从字节 24 起）
data_type make_host_packet(const data_type &frame, std::uint32_t data_offset = 16, std::uint32_t oob_len = 0) {
    data_type p;
    // 同上：24+frame.size() 显式转 u32，size_t 会写 8 字节
    vector_append_to_le(p, 1u, static_cast<std::uint32_t>(24 + frame.size()), data_offset,
                        static_cast<std::uint32_t>(frame.size()), 0u, oob_len);
    p.insert(p.end(), frame.begin(), frame.end());
    return p;
}

} // namespace

// ==================== 控制面 ====================

TEST(TestRndisHandler, SendEncapsulatedInitQueuesInitCmpltAndNotification) {
    // INIT → SEND_ENCAPSULATED_COMMAND 回 OK；响应入队并触发
    // RESPONSE_AVAILABLE 通知（中断 IN 端点 8 字节 {1,0}）
    RndisTestEnv env;

    // 先挂中断 IN 请求：通知应直接应答
    env.comm.handle_interrupt_transfer(9, UsbEndpoint{.address = EP_NOTIFY, .attributes = 0x03,
                                                      .max_packet_size = 8, .interval = 32},
                                       0, 8, make_cmd_submit(env.op, 9, 0x83, UsbIpDirection::In, 8).transfer, env.ec);
    ASSERT_FALSE(env.ec);
    EXPECT_TRUE(env.stub.submits.empty()); // 无通知：请求挂起

    send_command(env, make_init_msg());
    const auto *ret = env.find_submit(1);
    ASSERT_NE(ret, nullptr);
    EXPECT_EQ(ret->status, 0u); // 命令已接收

    // 通知 = 8 字节 {1, 0}（两个 LE32）
    const auto *notify = env.find_submit(9);
    ASSERT_NE(notify, nullptr);
    EXPECT_EQ(notify->status, 0u);
    EXPECT_EQ(ret_submit_data(*notify), (data_type{1, 0, 0, 0, 0, 0, 0, 0}));

    // 取 INIT_C：52 字节，字段对齐内核 rndis_init_cmplt
    auto resp = get_response(env, 2);
    ASSERT_EQ(resp.size(), 52u);
    EXPECT_EQ(read_le32_at(resp, 0), rndis_msg(RndisMessageType::InitComplete));
    EXPECT_EQ(read_le32_at(resp, 4), 52u);
    EXPECT_EQ(read_le32_at(resp, 8), 1u); // RequestID 回显
    EXPECT_EQ(read_le32_at(resp, 12), rndis_status(RndisStatus::Success));
    EXPECT_EQ(read_le32_at(resp, 16), 1u); // MajorVersion
    EXPECT_EQ(read_le32_at(resp, 20), 0u); // MinorVersion
    EXPECT_EQ(read_le32_at(resp, 24), RNDIS_DF_CONNECTIONLESS);
    EXPECT_EQ(read_le32_at(resp, 28), RNDIS_MEDIUM_802_3);
    EXPECT_EQ(read_le32_at(resp, 32), 1u); // MaxPacketsPerTransfer
    EXPECT_EQ(read_le32_at(resp, 36), RNDIS_MAX_TRANSFER_SIZE);
    EXPECT_EQ(read_le32_at(resp, 40), 0u); // PacketAlignmentFactor
}

TEST(TestRndisHandler, InitQueuesMediaConnectIndicate) {
    // INIT 完成后入队 MEDIA_CONNECT 状态通知（对齐内核 rndis_signal_connect：
    // Windows 依赖此通知确认链路 up）
    RndisTestEnv env;
    send_command(env, make_init_msg());

    auto resp = get_response(env, 2); // INIT_C
    ASSERT_GE(resp.size(), 20u);
    resp = get_response(env, 3); // INDICATE MEDIA_CONNECT
    ASSERT_EQ(resp.size(), 20u);
    EXPECT_EQ(read_le32_at(resp, 0), rndis_msg(RndisMessageType::Indicate));
    EXPECT_EQ(read_le32_at(resp, 4), 20u);
    EXPECT_EQ(read_le32_at(resp, 8), rndis_status(RndisStatus::MediaConnect));
    EXPECT_EQ(read_le32_at(resp, 12), 0u); // StatusBufferLength
    EXPECT_EQ(env.comm.get_state(), RndisState::Initialized);
}

TEST(TestRndisHandler, QueryOidReturnsData) {
    // QUERY OID_802_3_PERMANENT_ADDRESS → QUERY_C + 6 字节 MAC
    // （Linux 主机绑定强制查询此 OID，失败则绑定失败）
    RndisTestEnv env;
    send_command(env, make_query_msg(OID_802_3_PERMANENT_ADDRESS));
    auto resp = get_response(env, 2);

    ASSERT_EQ(resp.size(), 24u + 6u);
    EXPECT_EQ(read_le32_at(resp, 0), rndis_msg(RndisMessageType::QueryComplete));
    EXPECT_EQ(read_le32_at(resp, 8), 1u); // RequestID 回显
    auto [status, payload] = parse_query_cmplt(resp);
    EXPECT_EQ(status, rndis_status(RndisStatus::Success));
    EXPECT_EQ(payload, (data_type(TEST_MAC.begin(), TEST_MAC.end())));
}

TEST(TestRndisHandler, QueryUnsupportedOidReturnsNotSupported) {
    // 未知 OID → QUERY_C NOT_SUPPORTED（不能 stall 控制请求，对齐内核）
    RndisTestEnv env;
    send_command(env, make_query_msg(0x12345678));
    auto resp = get_response(env, 2);

    ASSERT_EQ(resp.size(), 24u); // 无数据
    auto [status, payload] = parse_query_cmplt(resp);
    EXPECT_EQ(status, rndis_status(RndisStatus::NotSupported));
    EXPECT_TRUE(payload.empty());
}

TEST(TestRndisHandler, QueryLinkSpeedBySpeed) {
    // LINK_SPEED 按设备速度上报（100bps 单位，对齐内核 bitrate()）
    RndisTestEnv env_full;
    send_command(env_full, make_query_msg(OID_GEN_LINK_SPEED));
    auto [status, payload] = parse_query_cmplt(get_response(env_full, 2));
    EXPECT_EQ(status, rndis_status(RndisStatus::Success));
    ASSERT_EQ(payload.size(), 4u);
    EXPECT_EQ(read_le32_at(payload, 0), 97'280u); // Full speed

    RndisTestEnv env_high(UsbSpeed::High);
    send_command(env_high, make_query_msg(OID_GEN_LINK_SPEED));
    auto [status2, payload2] = parse_query_cmplt(get_response(env_high, 2));
    ASSERT_EQ(payload2.size(), 4u);
    EXPECT_EQ(read_le32_at(payload2, 0), 4'259'840u); // High speed
}

TEST(TestRndisHandler, SetPacketFilterEnablesData) {
    // SET OID_GEN_CURRENT_PACKET_FILTER：非零 → DATA_INITIALIZED（数据闸门开），
    // 零 → 回 INITIALIZED（关）。对齐 rndis.c 的 gen_ndis_set_resp
    RndisTestEnv env;
    send_command(env, make_init_msg());
    get_response(env, 2); // 取走 INIT_C
    get_response(env, 3); // 取走 MEDIA_CONNECT
    EXPECT_FALSE(env.comm.is_data_enabled());

    data_type filter_32{0x2D, 0x00, 0x00, 0x00}; // LE32 0x2D
    send_command(env, make_set_msg(OID_GEN_CURRENT_PACKET_FILTER, filter_32));
    auto resp = get_response(env, 4);
    ASSERT_EQ(resp.size(), 16u);
    EXPECT_EQ(read_le32_at(resp, 0), rndis_msg(RndisMessageType::SetComplete));
    EXPECT_EQ(read_le32_at(resp, 12), rndis_status(RndisStatus::Success));
    EXPECT_TRUE(env.comm.is_data_enabled());
    EXPECT_EQ(env.comm.get_state(), RndisState::DataInitialized);

    // filter=0：数据闸门关闭
    send_command(env, make_set_msg(OID_GEN_CURRENT_PACKET_FILTER, data_type(4, 0)));
    get_response(env, 5);
    EXPECT_FALSE(env.comm.is_data_enabled());
    EXPECT_EQ(env.comm.get_state(), RndisState::Initialized);
}

TEST(TestRndisHandler, KeepaliveRespondsSuccess) {
    // KEEPALIVE → KEEPALIVE_C SUCCESS（Windows 主机每 5 秒发一次保活）
    RndisTestEnv env;
    send_command(env, make_keepalive_msg(7));
    auto resp = get_response(env, 2);

    ASSERT_EQ(resp.size(), 16u);
    EXPECT_EQ(read_le32_at(resp, 0), rndis_msg(RndisMessageType::KeepaliveComplete));
    EXPECT_EQ(read_le32_at(resp, 8), 7u); // RequestID 回显
    EXPECT_EQ(read_le32_at(resp, 12), rndis_status(RndisStatus::Success));
}

TEST(TestRndisHandler, ResetClearsQueueAndResponds) {
    // RESET 清空未取走的响应队列，回 RESET_C（AddressingReset=1）。对齐
    // rndis.c：队列里残留的 INIT_C/MEDIA_CONNECT 应被丢弃
    RndisTestEnv env;
    send_command(env, make_init_msg()); // 入队 INIT_C + MEDIA_CONNECT
    send_command(env, make_reset_msg());

    auto resp = get_response(env, 2);
    ASSERT_EQ(resp.size(), 16u);
    EXPECT_EQ(read_le32_at(resp, 0), rndis_msg(RndisMessageType::ResetComplete));
    EXPECT_EQ(read_le32_at(resp, 8), rndis_status(RndisStatus::Success));
    EXPECT_EQ(read_le32_at(resp, 12), 1u); // AddressingReset

    // 队列只剩 RESET_C：再取应 EPIPE
    send_command(env, make_init_msg()); // 重新入队
    auto resp2 = get_response(env, 3);
    ASSERT_EQ(resp2.size(), 52u); // 新队列从 INIT_C 开始（旧响应已被 RESET 清掉）
}

TEST(TestRndisHandler, HaltResetsStateNoResponse) {
    // HALT 不响应（对齐 rndis.c），状态回 UNINITIALIZED、数据闸门关闭
    RndisTestEnv env;
    send_command(env, make_init_msg());
    get_response(env, 2);
    get_response(env, 3);
    send_command(env, make_set_msg(OID_GEN_CURRENT_PACKET_FILTER, data_type{0x2D, 0, 0, 0}));
    get_response(env, 4);
    EXPECT_TRUE(env.comm.is_data_enabled());

    send_command(env, make_halt_msg());
    // HALT 无响应：取响应应 EPIPE
    const SetupPacket setup{
            .request_type = 0xA1, .request = 0x01, .value = 0, .index = 0, .length = 1558,
    };
    auto submit = make_cmd_submit(env.op, 5, 0x00, UsbIpDirection::In, 1558, setup);
    env.comm.handle_non_standard_request_type_control_urb(
            5, UsbEndpoint::get_ep0_in(UsbSpeed::Full), 0, 1558, setup, std::move(submit.transfer), env.ec);
    ASSERT_FALSE(env.ec);
    const auto *ret = env.find_submit(5);
    ASSERT_NE(ret, nullptr);
    EXPECT_EQ(ret->status, static_cast<std::uint32_t>(UrbStatusType::StatusEPIPE));
    EXPECT_EQ(env.comm.get_state(), RndisState::Uninitialized);
    EXPECT_FALSE(env.comm.is_data_enabled());
}

TEST(TestRndisHandler, GetResponseEmptyQueueStalls) {
    // 队列空时 GET_ENCAPSULATED_RESPONSE 回 EPIPE（对齐内核 STALL：
    // 主机 rndis_command 以 40ms 间隔轮询重试）
    RndisTestEnv env;
    auto resp = get_response(env, 2);
    EXPECT_TRUE(resp.empty());
    const auto *ret = env.find_submit(2);
    ASSERT_NE(ret, nullptr);
    EXPECT_EQ(ret->status, static_cast<std::uint32_t>(UrbStatusType::StatusEPIPE));
}

// ==================== 数据面 ====================

TEST(TestRndisHandler, OutPacketStrippedHeaderDeliveredToBackend) {
    // 主机 bulk OUT：RNDIS_MSG_PACKET（24B 头 DataOffset=16）→ 剥头后
    // 以太网帧交给 backend，应答 actual_length=传输总长
    RndisTestEnv env;

    const data_type frame = {0xAA, 0xBB, 0x00, 0x01, 0x02, 0x03, 0x00, 0x11, 0x22, 0x33,
                             0x44, 0x55, 0x08, 0x00, 0x01, 0x02, 0x03, 0x04};
    auto payload = make_host_packet(frame);
    env.data.handle_bulk_transfer(1, UsbEndpoint{.address = EP_OUT, .attributes = 0x02, .max_packet_size = 64},
                                  0, payload.size(),
                                  make_cmd_submit(env.op, 1, EP_OUT, UsbIpDirection::Out, payload.size(), {}, payload)
                                          .transfer,
                                  env.ec);
    ASSERT_FALSE(env.ec);

    ASSERT_EQ(env.backend.frames.size(), 1u);
    EXPECT_EQ(env.backend.frames[0], frame);

    const auto *ret = env.find_submit(1);
    ASSERT_NE(ret, nullptr);
    EXPECT_EQ(ret->status, 0u);
    EXPECT_EQ(ret->actual_length, payload.size());
}

TEST(TestRndisHandler, OutMultipacketAggregation) {
    // 一条 bulk OUT 传输聚合两条 RNDIS_MSG_PACKET（协议允许，主机一次一包
    // 但设备须能处理）→ backend 收到两帧
    RndisTestEnv env;

    const data_type frame1 = {0xAA, 0xBB, 0x00, 0x01, 0x02, 0x03, 0x00, 0x11, 0x22, 0x33,
                              0x44, 0x55, 0x08, 0x00, 0x01, 0x02, 0x03, 0x04};
    // 帧长必须 ≥ 14（以太网头最小长度，对齐内核 u_ether 的 ETH_HLEN 校验）
    const data_type frame2 = {0xCC, 0xDD, 0x00, 0x02, 0x03, 0x04, 0x00, 0x55, 0x66, 0x77, 0x88, 0x99, 0xAA, 0xBB};
    auto p1 = make_host_packet(frame1);
    auto p2 = make_host_packet(frame2);
    p1.insert(p1.end(), p2.begin(), p2.end());

    env.data.handle_bulk_transfer(1, UsbEndpoint{.address = EP_OUT, .attributes = 0x02, .max_packet_size = 64},
                                  0, p1.size(),
                                  make_cmd_submit(env.op, 1, EP_OUT, UsbIpDirection::Out, p1.size(), {}, p1).transfer,
                                  env.ec);
    ASSERT_FALSE(env.ec);

    ASSERT_EQ(env.backend.frames.size(), 2u);
    EXPECT_EQ(env.backend.frames[0], frame1);
    EXPECT_EQ(env.backend.frames[1], frame2);
}

TEST(TestRndisHandler, OutToleratesOobAndPadding) {
    // 容忍异常主机：OOB 字段非 0、DataOffset 非标准、消息尾部填充——
    // 按 DataOffset/DataLength 定位帧，OOB 直接忽略（对齐内核 rndis_rm_hdr）
    RndisTestEnv env;

    const data_type frame = {0xAA, 0xBB, 0x00, 0x01, 0x02, 0x03, 0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x08, 0x00};
    // DataOffset=20（数据从字节 28 起，头 24B 后有 4 字节填充）、OOB 声明非 0、
    // 消息尾部追加填充字节
    auto payload = make_host_packet(frame, 20, 4);
    payload.insert(payload.begin() + 24, 4, 0xEE); // 头与数据之间的 4 字节填充
    payload.push_back(0xFF);                       // 尾部填充（消息外，无害）

    env.data.handle_bulk_transfer(1, UsbEndpoint{.address = EP_OUT, .attributes = 0x02, .max_packet_size = 64},
                                  0, payload.size(),
                                  make_cmd_submit(env.op, 1, EP_OUT, UsbIpDirection::Out, payload.size(), {}, payload)
                                          .transfer,
                                  env.ec);
    ASSERT_FALSE(env.ec);

    ASSERT_EQ(env.backend.frames.size(), 1u);
    EXPECT_EQ(env.backend.frames[0], frame);
}

TEST(TestRndisHandler, InFrameWrappedWithRndisHeader) {
    // send_frame → 主机 bulk IN 请求取走：数据包 44 字节头
    // （MessageType=1/MessageLength=44+len/DataOffset=36/DataLength=len）+ 帧
    RndisTestEnv env;

    const data_type frame = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x08, 0x00};
    EXPECT_EQ(env.data.send_frame(frame), frame.size());

    env.data.handle_bulk_transfer(2, UsbEndpoint{.address = EP_IN, .attributes = 0x02, .max_packet_size = 64},
                                  0, 1558, make_cmd_submit(env.op, 2, EP_IN, UsbIpDirection::In, 1558).transfer,
                                  env.ec);
    ASSERT_FALSE(env.ec);

    const auto *ret = env.find_submit(2);
    ASSERT_NE(ret, nullptr);
    EXPECT_EQ(ret->status, 0u);
    EXPECT_EQ(ret->actual_length, 44u + frame.size());
    auto resp = ret_submit_data(*ret);
    ASSERT_EQ(resp.size(), 44u + frame.size());
    EXPECT_EQ(read_le32_at(resp, 0), 1u); // RNDIS_MSG_PACKET
    EXPECT_EQ(read_le32_at(resp, 4), 44u + frame.size());
    EXPECT_EQ(read_le32_at(resp, 8), 36u); // 数据从字节 44 起
    EXPECT_EQ(read_le32_at(resp, 12), frame.size());
    EXPECT_EQ(read_le32_at(resp, 16), 0u); // OOB 全 0（不做校验和卸载）
    EXPECT_EQ(data_type(resp.begin() + 44, resp.end()), frame);
}

TEST(TestRndisHandler, InFramePadWhenMultipleOfMaxPacket) {
    // 包后总长恰为 maxpacket(64) 倍数时补 1 字节防 ZLP（对齐内核 u_ether.c：
    // RNDIS 不允许 ZLP；MessageLength 不含填充字节，主机按消息长度拆包后
    // 尾字节被 trim）。44+20=64 → 应发 65 字节
    RndisTestEnv env;

    const data_type frame(20, 0xAB);
    EXPECT_EQ(env.data.send_frame(frame), frame.size());

    env.data.handle_bulk_transfer(1, UsbEndpoint{.address = EP_IN, .attributes = 0x02, .max_packet_size = 64},
                                  0, 1558, make_cmd_submit(env.op, 1, EP_IN, UsbIpDirection::In, 1558).transfer,
                                  env.ec);
    ASSERT_FALSE(env.ec);

    const auto *ret = env.find_submit(1);
    ASSERT_NE(ret, nullptr);
    EXPECT_EQ(ret->actual_length, 65u); // 44 + 20 + 1 填充
    auto resp = ret_submit_data(*ret);
    ASSERT_EQ(resp.size(), 65u);
    EXPECT_EQ(read_le32_at(resp, 4), 64u); // MessageLength 不含填充
    EXPECT_EQ(resp.back(), 0u);
}

TEST(TestRndisHandler, UnlinkPendingInRequest) {
    // 挂起的 IN 请求被 UNLINK 取消：回 RET_UNLINK(-ECONNRESET)
    RndisTestEnv env;

    env.data.handle_bulk_transfer(1, UsbEndpoint{.address = EP_IN, .attributes = 0x02, .max_packet_size = 64},
                                  0, 8, make_cmd_submit(env.op, 1, EP_IN, UsbIpDirection::In, 8).transfer,
                                  env.ec);
    ASSERT_FALSE(env.ec);
    EXPECT_TRUE(env.stub.submits.empty()); // 无帧：请求挂起

    env.data.handle_unlink_seqnum(1, 2);
    ASSERT_EQ(env.stub.unlinks.size(), 1u);
    EXPECT_EQ(env.stub.unlinks[0].header.seqnum, 2u);
    EXPECT_EQ(env.stub.unlinks[0].status, static_cast<std::uint32_t>(UrbStatusType::StatusECONNRESET));
    EXPECT_TRUE(env.stub.submits.empty());
}

TEST(TestRndisHandler, DisconnectResetsStateAndChannels) {
    // 断连：状态机复位（UNINITIALIZED、响应队列清空、数据闸门关），
    // 重连后从干净状态开始
    RndisTestEnv env;
    send_command(env, make_init_msg());
    get_response(env, 2);
    get_response(env, 3);
    send_command(env, make_set_msg(OID_GEN_CURRENT_PACKET_FILTER, data_type{0x2D, 0, 0, 0}));
    get_response(env, 4);
    EXPECT_TRUE(env.comm.is_data_enabled());

    std::error_code disc_ec;
    env.comm.on_disconnection(disc_ec);
    env.data.on_disconnection(disc_ec);
    EXPECT_EQ(env.comm.get_state(), RndisState::Uninitialized);
    EXPECT_FALSE(env.comm.is_data_enabled());

    // 重连：队列干净，无残留响应（INIT 从头开始）
    std::error_code conn_ec;
    env.comm.on_new_connection(env.stub, conn_ec);
    env.data.on_new_connection(env.stub, conn_ec);
    send_command(env, make_init_msg());
    auto resp = get_response(env, 5);
    ASSERT_EQ(resp.size(), 52u); // 第一条就是 INIT_C，无残留
}
