#include <gtest/gtest.h>
#include <atomic>
#include <memory>
#include <thread>
#include <vector>

#include "protocol.h"
#include "Device.h"
#include "DeviceHandler/DeviceHandler.h"

using namespace usbipdcpp;

// ============== 测试用的常量 header 和 setup_packet ==============

static const UsbIpHeaderBasic test_header = {
    .command = USBIP_CMD_SUBMIT,
    .seqnum = 0,
    .devid = 0,
    .direction = UsbIpDirection::In,
    .ep = 0
};

static const SetupPacket test_setup = {
    .request_type = 0x80,
    .request = 0,
    .value = 0,
    .index = 0,
    .length = 0
};

// ============== Mock TransferOperator ==============

class MockTransferOperator : public GenericTransferOperator {
public:
    std::atomic<int> alloc_count{0};
    std::atomic<int> free_count{0};
    std::atomic<void*> last_freed_handle{nullptr};

    void* alloc_transfer_handle(std::size_t buffer_length, int num_iso_packets,
                                 const UsbIpHeaderBasic& header,
                                 const SetupPacket& setup_packet) override {
        alloc_count++;
        return GenericTransferOperator::alloc_transfer_handle(buffer_length, num_iso_packets, header, setup_packet);
    }

    void free_transfer_handle(void* handle) override {
        free_count++;
        last_freed_handle = handle;
        GenericTransferOperator::free_transfer_handle(handle);
    }
};

// ============== Mock DeviceHandler ==============

class MockDeviceHandler : public AbstDeviceHandler {
public:
    explicit MockDeviceHandler(UsbDevice &device)
        : AbstDeviceHandler(device, std::make_unique<MockTransferOperator>()) {}

    MockTransferOperator* mock_op() {
        return static_cast<MockTransferOperator*>(get_transfer_operator());
    }

    void handle_unlink_seqnum(std::uint32_t unlink_seqnum, std::uint32_t cmd_seqnum) override {}

    void receive_urb(UsbIpCommand::UsbIpCmdSubmit cmd,
                     UsbEndpoint ep,
                     std::optional<UsbInterface> interface,
                     usbipdcpp::error_code &ec) override {}
};

// ============== 基本功能测试 ==============

class TransferHandleTest : public ::testing::Test {
protected:
    void SetUp() override {
        device = std::make_unique<UsbDevice>();
        handler = std::make_unique<MockDeviceHandler>(*device);
    }

    void TearDown() override {
        handler.reset();
        device.reset();
    }

    std::unique_ptr<UsbDevice> device;
    std::unique_ptr<MockDeviceHandler> handler;
};

TEST_F(TransferHandleTest, ConstructorAndDestructor) {
    auto* op = handler->mock_op();
    void* raw_handle = op->alloc_transfer_handle(1024, 0, test_header, test_setup);
    EXPECT_NE(raw_handle, nullptr);
    EXPECT_EQ(op->alloc_count, 1);

    {
        TransferHandle handle(raw_handle, op);
        EXPECT_EQ(handle.get(), raw_handle);
        EXPECT_EQ(handle.get_operator(), op);
    }

    EXPECT_EQ(op->free_count, 1);
    EXPECT_EQ(op->last_freed_handle, raw_handle);
}

TEST_F(TransferHandleTest, MoveConstructor) {
    auto* op = handler->mock_op();
    void* raw_handle = op->alloc_transfer_handle(1024, 0, test_header, test_setup);

    TransferHandle handle1(raw_handle, op);
    TransferHandle handle2(std::move(handle1));

    EXPECT_EQ(handle1.get(), nullptr);
    EXPECT_EQ(handle1.get_operator(), nullptr);
    EXPECT_EQ(handle2.get(), raw_handle);
    EXPECT_EQ(handle2.get_operator(), op);
}

TEST_F(TransferHandleTest, MoveAssignment) {
    auto* op = handler->mock_op();
    void* raw_handle1 = op->alloc_transfer_handle(1024, 0, test_header, test_setup);
    void* raw_handle2 = op->alloc_transfer_handle(2048, 0, test_header, test_setup);

    TransferHandle handle1(raw_handle1, op);
    TransferHandle handle2(raw_handle2, op);

    handle1 = std::move(handle2);

    EXPECT_EQ(handle1.get(), raw_handle2);
    EXPECT_EQ(op->free_count, 1);
    EXPECT_EQ(op->last_freed_handle, raw_handle1);
}

TEST_F(TransferHandleTest, Release) {
    auto* op = handler->mock_op();
    void* raw_handle = op->alloc_transfer_handle(1024, 0, test_header, test_setup);

    TransferHandle handle(raw_handle, op);
    void* released = handle.release();

    EXPECT_EQ(released, raw_handle);
    EXPECT_EQ(handle.get(), nullptr);
    EXPECT_EQ(handle.get_operator(), nullptr);
    EXPECT_EQ(op->free_count, 0);

    op->free_transfer_handle(released);
    EXPECT_EQ(op->free_count, 1);
}

TEST_F(TransferHandleTest, Reset) {
    auto* op = handler->mock_op();
    void* raw_handle = op->alloc_transfer_handle(1024, 0, test_header, test_setup);

    TransferHandle handle(raw_handle, op);
    handle.reset();

    EXPECT_EQ(handle.get(), nullptr);
    EXPECT_EQ(handle.get_operator(), nullptr);
    EXPECT_EQ(op->free_count, 1);
}

// ============== 并发安全测试 ==============

TEST_F(TransferHandleTest, ConcurrentTransferHandles) {
    constexpr int num_threads = 4;
    constexpr int handles_per_thread = 100;
    std::vector<std::thread> threads;

    for (int t = 0; t < num_threads; t++) {
        threads.emplace_back([this]() {
            for (int i = 0; i < handles_per_thread; i++) {
                void* raw = handler->mock_op()->alloc_transfer_handle(1024, 0, test_header, test_setup);
                TransferHandle handle(raw, handler->mock_op());
            }
        });
    }

    for (auto &thread: threads) {
        thread.join();
    }

    EXPECT_EQ(handler->mock_op()->alloc_count, num_threads * handles_per_thread);
    EXPECT_EQ(handler->mock_op()->free_count, num_threads * handles_per_thread);
}
