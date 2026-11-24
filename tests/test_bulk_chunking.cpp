#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <vector>

#include "test_utils.h"

#include "usbipdcpp/Device.h"
#include "usbipdcpp/Server.h"
#include "usbipdcpp/TransferSource.h"
#include "usbipdcpp/network.h"
#include "usbipdcpp/protocol.h"
#include "usbipdcpp/virtual_device/SimpleVirtualDeviceHandler.h"
#include "usbipdcpp/virtual_device/VirtualInterfaceHandler.h"

using namespace usbipdcpp;
using namespace usbipdcpp::test;

namespace {

/**
 * @brief 模拟“固件一次只拿一块”的 bulk 操作器
 *
 * 不按整个 transfer_buffer_length 分配缓冲，只用一块固定大小的 scratch
 * 反复从 TransferSource 拉数据；记录每块长度和内容校验和，供测试断言分块
 * 边界。
 */
class ChunkedOperator : public TransferOperator {
public:
    struct Record {
        std::vector<std::size_t> chunk_sizes;
        std::size_t total = 0;
        std::uint64_t checksum = 0;
    };

    struct Transfer {
        Record record;
        bool is_in = false;
        data_type out_data;
        std::size_t actual_length = 0;
    };

    explicit ChunkedOperator(std::size_t chunk_bytes, std::size_t read_limit = 0) :
        chunk_bytes_(chunk_bytes), read_limit_(read_limit) {
    }

    void *alloc_transfer_handle(std::size_t, int, const UsbIpHeaderBasic &header, const SetupPacket &) override {
        auto *trx = new Transfer{};
        trx->is_in = header.direction == UsbIpDirection::In;
        return trx;
    }

    void free_transfer_handle(void *handle) override { delete static_cast<Transfer *>(handle); }

    std::size_t get_actual_length(void *handle) override { return static_cast<Transfer *>(handle)->actual_length; }

    bool transfer_is_in(void *handle) override { return static_cast<Transfer *>(handle)->is_in; }

    UsbIpIsoPacketDescriptor get_iso_descriptor(void *, int) override { return {}; }
    void set_iso_descriptor(void *, int, const UsbIpIsoPacketDescriptor &) override {}

    std::size_t set_transfer_data(void *handle, const data_type &data, std::size_t max_length) override {
        auto *trx = static_cast<Transfer *>(handle);
        auto len = std::min(data.size(), max_length);
        trx->out_data.assign(data.begin(), data.begin() + static_cast<std::ptrdiff_t>(len));
        trx->actual_length = len;
        return len;
    }

    std::size_t get_transfer_data(void *, data_type &, bool &supported) override {
        supported = false;
        return 0;
    }

    void send_transfer_data(void *handle, asio::ip::tcp::socket &sock, std::size_t length,
                            std::error_code &ec) override {
        auto *trx = static_cast<Transfer *>(handle);
        if (length > 0) {
            asio::write(sock, asio::buffer(trx->out_data.data(), length), ec);
        }
    }

    void recv_transfer_data(void *handle, TransferSource &source, std::error_code &ec) override {
        auto *trx = static_cast<Transfer *>(handle);
        // 只分配一块 scratch，和整个传输大小无关
        std::vector<std::byte> scratch(chunk_bytes_);
        while (source.remaining() > 0) {
            auto want = std::min(chunk_bytes_, source.remaining());
            if (read_limit_ != 0) {
                // 故意少读：达到上限后立即返回，验证框架会把剩余字节丢掉
                auto left = read_limit_ - trx->record.total;
                if (left == 0) {
                    return;
                }
                want = std::min(want, left);
            }
            auto got = source.read({scratch.data(), want}, ec);
            if (ec) {
                return;
            }
            trx->record.chunk_sizes.push_back(got);
            for (std::size_t i = 0; i < got; ++i) {
                trx->record.checksum = trx->record.checksum * 131 +
                                       static_cast<std::uint8_t>(scratch[i]);
            }
            trx->record.total += got;
            trx->actual_length = trx->record.total;
        }
        last_record = trx->record;
    }

    Record last_record;

private:
    std::size_t chunk_bytes_;
    std::size_t read_limit_;
};

/// 收到 bulk URB 后按实际读取量回成功应答
class ChunkedBulkInterface : public VirtualInterfaceHandler {
public:
    using VirtualInterfaceHandler::VirtualInterfaceHandler;

    void handle_bulk_transfer(std::uint32_t seqnum, const UsbEndpoint &, std::uint32_t, std::uint32_t,
                              TransferHandle transfer, std::error_code &) override {
        auto actual = transfer.get_operator()->get_actual_length(transfer.get());
        responder->submit_ret_submit(
                UsbIpResponse::UsbIpRetSubmit::create_ret_submit_ok_without_data(seqnum, actual));
    }
};

struct BulkChunkFixture {
    ChunkedOperator *op = nullptr;
    std::shared_ptr<UsbDevice> device;
    VirtualDeviceHandler *device_handler = nullptr;
    CaptureResponder stub;
    error_code ec;

    explicit BulkChunkFixture(std::size_t chunk_bytes, std::size_t read_limit = 0) {
        std::vector<UsbInterface> interfaces = {
                UsbInterface{
                        .interface_class = 0xFF, // vendor
                        .interface_subclass = 0x00,
                        .interface_protocol = 0x00,
                        .endpoints = {{
                                UsbEndpoint{.address = 0x02, .attributes = 0x02, .max_packet_size = 64, .interval = 0},
                                UsbEndpoint{.address = 0x81, .attributes = 0x02, .max_packet_size = 64, .interval = 0},
                        }},
                },
        };
        device = UsbDevice::make("1-1", 0x1234, 0x5678, std::move(interfaces));
        auto holder = std::make_unique<ChunkedOperator>(chunk_bytes, read_limit);
        op = holder.get();
        device->interfaces[0].with_handler<ChunkedBulkInterface>(std::move(holder));
        device_handler = device->with_handler<SimpleVirtualDeviceHandler>().get();
        device_handler->setup_interface_handlers();
        device_handler->on_new_connection(stub, ec);
    }

    ~BulkChunkFixture() { device_handler->on_disconnection(ec); }

    UsbEndpoint out_endpoint() { return device->interfaces[0].endpoints[0][0]; }

    void send_out(std::uint32_t seqnum, const data_type &payload, std::uint32_t declared_length = 0) {
        auto length = declared_length != 0 ? declared_length : static_cast<std::uint32_t>(payload.size());
        deliver_urb(device_handler,
                    make_cmd_submit(seqnum, 0x02, UsbIpDirection::Out, length, {}, payload),
                    out_endpoint(), device->interfaces[0], ec);
    }
};

std::uint64_t checksum_of(const data_type &data) {
    std::uint64_t sum = 0;
    for (auto byte: data) {
        sum = sum * 131 + byte;
    }
    return sum;
}

std::vector<std::size_t> expected_chunks(std::size_t len, std::size_t chunk) {
    std::vector<std::size_t> sizes;
    while (len > 0) {
        auto n = std::min(len, chunk);
        sizes.push_back(n);
        len -= n;
    }
    return sizes;
}

} // namespace

// ============== 分块边界（设备层，直接喂字节源） ==============

TEST(TestBulkChunking, ChunkBoundariesForAllLengths) {
    const std::size_t chunk = 64; // 与 max_packet_size 对齐
    BulkChunkFixture fixture(chunk);
    ASSERT_FALSE(fixture.ec);

    const std::vector<std::size_t> lengths = {0, 1, 63, 64, 65, 127, 128, 129, 255, 256, 1000, 4097};
    for (const auto len: lengths) {
        data_type payload(len);
        for (std::size_t i = 0; i < len; ++i) {
            payload[i] = static_cast<std::uint8_t>(i * 7 + 3);
        }
        fixture.op->last_record = {};
        fixture.send_out(1, payload);
        ASSERT_FALSE(fixture.ec) << "len=" << len;
        EXPECT_EQ(fixture.op->last_record.chunk_sizes, expected_chunks(len, chunk)) << "len=" << len;
        EXPECT_EQ(fixture.op->last_record.total, len);
        EXPECT_EQ(fixture.op->last_record.checksum, checksum_of(payload));
    }
}

TEST(TestBulkChunking, ChunkSizesAndContentAreCorrect) {
    const std::size_t chunk = 64;
    for (const auto len: std::vector<std::size_t>{1, 63, 64, 65, 128, 1000}) {
        BulkChunkFixture fixture(chunk);
        data_type payload(len);
        for (std::size_t i = 0; i < len; ++i) {
            payload[i] = static_cast<std::uint8_t>(i & 0xFF);
        }
        fixture.send_out(7, payload);
        ASSERT_FALSE(fixture.ec);
        ASSERT_EQ(fixture.stub.submits.size(), 1u);
        EXPECT_EQ(fixture.stub.submits[0].header.seqnum, 7u);
        EXPECT_EQ(fixture.stub.submits[0].actual_length, len);
        EXPECT_EQ(fixture.op->last_record.chunk_sizes, expected_chunks(len, chunk));
    }
}

TEST(TestBulkChunking, RecordsMatchExpectedChunkLayout) {
    const std::size_t chunk = 64;
    BulkChunkFixture fixture(chunk);
    data_type payload(200);
    for (std::size_t i = 0; i < payload.size(); ++i) {
        payload[i] = static_cast<std::uint8_t>(i);
    }

    // 直接在字节源上跑一遍，检查操作器记录（应答只验证不丢字节）
    auto cmd = make_cmd_submit(1, 0x02, UsbIpDirection::Out, payload.size(), {}, payload);
    MemoryTransferSource source(cmd.data, cmd.iso_descriptors);
    auto *handle = fixture.op->alloc_transfer_handle(0, 0, UsbIpHeaderBasic{.direction = UsbIpDirection::Out}, {});
    fixture.op->recv_transfer_data(handle, source, fixture.ec);
    ASSERT_FALSE(fixture.ec);

    auto *trx = static_cast<ChunkedOperator::Transfer *>(handle);
    EXPECT_EQ(trx->record.chunk_sizes, expected_chunks(payload.size(), chunk));
    EXPECT_EQ(trx->record.total, payload.size());
    EXPECT_EQ(trx->record.checksum, checksum_of(payload));
    EXPECT_EQ(trx->actual_length, payload.size());
    EXPECT_EQ(source.remaining(), 0u);
    fixture.op->free_transfer_handle(handle);
}

TEST(TestBulkChunking, ChunkLargerThanTransferReadsSingleChunk) {
    BulkChunkFixture fixture(4096);
    data_type payload(1000, 0x5A);
    auto cmd = make_cmd_submit(1, 0x02, UsbIpDirection::Out, payload.size(), {}, payload);
    MemoryTransferSource source(cmd.data, cmd.iso_descriptors);
    auto *handle = fixture.op->alloc_transfer_handle(0, 0, UsbIpHeaderBasic{.direction = UsbIpDirection::Out}, {});
    fixture.op->recv_transfer_data(handle, source, fixture.ec);
    ASSERT_FALSE(fixture.ec);
    auto *trx = static_cast<ChunkedOperator::Transfer *>(handle);
    EXPECT_EQ(trx->record.chunk_sizes, (std::vector<std::size_t>{1000}));
    fixture.op->free_transfer_handle(handle);
}

TEST(TestBulkChunking, DeclaredLengthLongerThanSourceKeepsFractionalChunks) {
    // 声明 1000 字节但字节源只有 100（模拟截断）：操作器按可用字节读完即止，
    // 不会读越界；应答 actual_length 是实际读到的长度
    BulkChunkFixture fixture(64);
    data_type payload(100, 0x11);
    fixture.send_out(3, payload, 1000);
    ASSERT_FALSE(fixture.ec);
    ASSERT_EQ(fixture.stub.submits.size(), 1u);
    EXPECT_EQ(fixture.stub.submits[0].actual_length, 100u);
}

// ============== 真实 socket 链路：分块 + 流对齐 ==============

namespace {

std::shared_ptr<UsbDevice> make_chunked_device(std::size_t chunk_bytes, ChunkedOperator **out_op = nullptr) {
    std::vector<UsbInterface> interfaces = {
            UsbInterface{
                    .interface_class = 0xFF,
                    .interface_subclass = 0x00,
                    .interface_protocol = 0x00,
                    .endpoints = {{
                            UsbEndpoint{.address = 0x02, .attributes = 0x02, .max_packet_size = 64, .interval = 0},
                            UsbEndpoint{.address = 0x81, .attributes = 0x02, .max_packet_size = 64, .interval = 0},
                    }},
            },
    };
    auto device = UsbDevice::make("1-1", 0x1234, 0x5678, std::move(interfaces));
    auto holder = std::make_unique<ChunkedOperator>(chunk_bytes);
    if (out_op) {
        *out_op = holder.get();
    }
    device->interfaces[0].with_handler<ChunkedBulkInterface>(std::move(holder));
    auto dh = device->with_handler<SimpleVirtualDeviceHandler>();
    dh->setup_interface_handlers();
    return device;
}

} // namespace

TEST(TestBulkChunking, LargeTransferOverSocketStaysAligned) {
    asio::io_context io;
    Server server;
    server.add_device(make_chunked_device(4096));
    ASSERT_FALSE(server.start(asio::ip::tcp::endpoint(asio::ip::address_v4::loopback(), 0)));

    asio::ip::tcp::socket client(io);
    ASSERT_TRUE(connect_with_retry(client, server.endpoint()));
    ASSERT_EQ(import_device(client, "1-1"), 0u);

    // 大传输：100000 字节，分块 4096（最后一块 1696）
    const std::size_t total = 100000;
    data_type payload(total);
    for (std::size_t i = 0; i < total; ++i) {
        payload[i] = static_cast<std::uint8_t>(i * 13 + 1);
    }
    UsbIpCommand::UsbIpCmdSubmit submit{};
    submit.header.command = USBIP_CMD_SUBMIT;
    submit.header.seqnum = 1;
    submit.header.devid = 1;
    submit.header.direction = UsbIpDirection::Out;
    submit.header.ep = 0x02;
    submit.transfer_buffer_length = static_cast<std::uint32_t>(total);
    submit.data = payload;
    error_code send_ec;
    submit.to_socket(client, send_ec);
    ASSERT_FALSE(send_ec);

    auto hdr = read_n(client, 48);
    expect_header(hdr, 1, static_cast<std::uint32_t>(total), 0);

    // 紧接着再发一条：分块/应答没有破坏 TCP 流边界
    UsbIpCommand::UsbIpCmdSubmit second{};
    second.header.command = USBIP_CMD_SUBMIT;
    second.header.seqnum = 2;
    second.header.devid = 1;
    second.header.direction = UsbIpDirection::Out;
    second.header.ep = 0x02;
    second.transfer_buffer_length = 4;
    second.data = {1, 2, 3, 4};
    second.to_socket(client, send_ec);
    ASSERT_FALSE(send_ec);

    auto hdr2 = read_n(client, 48);
    expect_header(hdr2, 2, 4, 0);

    rst_disconnect(client);
    server.stop();
}

TEST(TestBulkChunking, ShortReadByDeviceIsDrainedBeforeNextCommand) {
    // 操作器故意只读 100 字节（声明 5000）：Session 必须把剩余 4900 字节读掉，
    // 否则下一条命令解析从错误位置开始
    asio::io_context io;
    Server server;

    std::vector<UsbInterface> interfaces = {
            UsbInterface{
                    .interface_class = 0xFF,
                    .interface_subclass = 0x00,
                    .interface_protocol = 0x00,
                    .endpoints = {{
                            UsbEndpoint{.address = 0x02, .attributes = 0x02, .max_packet_size = 64, .interval = 0},
                            UsbEndpoint{.address = 0x81, .attributes = 0x02, .max_packet_size = 64, .interval = 0},
                    }},
            },
    };
    auto device = UsbDevice::make("1-1", 0x1234, 0x5678, std::move(interfaces));
    device->interfaces[0].with_handler<ChunkedBulkInterface>(std::make_unique<ChunkedOperator>(512, 100));
    auto dh = device->with_handler<SimpleVirtualDeviceHandler>();
    dh->setup_interface_handlers();
    server.add_device(std::move(device));
    ASSERT_FALSE(server.start(asio::ip::tcp::endpoint(asio::ip::address_v4::loopback(), 0)));

    asio::ip::tcp::socket client(io);
    ASSERT_TRUE(connect_with_retry(client, server.endpoint()));
    ASSERT_EQ(import_device(client, "1-1"), 0u);

    UsbIpCommand::UsbIpCmdSubmit first{};
    first.header.command = USBIP_CMD_SUBMIT;
    first.header.seqnum = 1;
    first.header.devid = 1;
    first.header.direction = UsbIpDirection::Out;
    first.header.ep = 0x02;
    first.transfer_buffer_length = 5000;
    first.data = data_type(5000, 0xAB);
    error_code send_ec;
    first.to_socket(client, send_ec);
    ASSERT_FALSE(send_ec);

    auto hdr = read_n(client, 48);
    expect_header(hdr, 1, 100, 0); // 设备只消费了 100 字节

    // 第二条命令能被正确解析就是 drain 成功的证明
    UsbIpCommand::UsbIpCmdSubmit second{};
    second.header.command = USBIP_CMD_SUBMIT;
    second.header.seqnum = 2;
    second.header.devid = 1;
    second.header.direction = UsbIpDirection::Out;
    second.header.ep = 0x02;
    second.transfer_buffer_length = 8;
    second.data = {8, 7, 6, 5, 4, 3, 2, 1};
    second.to_socket(client, send_ec);
    ASSERT_FALSE(send_ec);

    auto hdr2 = read_n(client, 48);
    expect_header(hdr2, 2, 8, 0);

    rst_disconnect(client);
    server.stop();
}
