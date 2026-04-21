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

// ============== Mock DeviceHandler for Testing ==============

class MockDeviceHandler : public AbstDeviceHandler {
public:
    std::atomic<int> alloc_count{0};
    std::atomic<int> free_count{0};
    std::atomic<void*> last_freed_handle{nullptr};

    explicit MockDeviceHandler(UsbDevice &device)
        : AbstDeviceHandler(device) {}

    void* alloc_transfer_handle(std::size_t buffer_length, int num_iso_packets, const UsbIpHeaderBasic& header, const SetupPacket& setup_packet) override {
        alloc_count++;
        auto* transfer = new GenericTransfer();
        transfer->data.resize(buffer_length);
        transfer->actual_length = buffer_length;
        return transfer;
    }

    void free_transfer_handle(void* transfer_handle) override {
        free_count++;
        last_freed_handle = transfer_handle;
        auto* transfer = GenericTransfer::from_handle(transfer_handle);
        delete transfer;
    }

    void* get_transfer_buffer(void* transfer_handle) override {
        auto* transfer = GenericTransfer::from_handle(transfer_handle);
        return transfer->data.data();
    }

    std::size_t get_actual_length(void* transfer_handle) override {
        auto* transfer = GenericTransfer::from_handle(transfer_handle);
        return transfer->actual_length;
    }

    void handle_unlink_seqnum(std::uint32_t unlink_seqnum, std::uint32_t cmd_seqnum) override {}

protected:
    void handle_control_urb(std::uint32_t seqnum, const UsbEndpoint &ep,
                            std::uint32_t transfer_flags, std::uint32_t transfer_buffer_length,
                            const SetupPacket &setup_packet, TransferHandle transfer,
                            std::error_code &ec) override {}

    void handle_bulk_transfer(std::uint32_t seqnum, const UsbEndpoint &ep,
                              UsbInterface &interface, std::uint32_t transfer_flags,
                              std::uint32_t transfer_buffer_length, TransferHandle transfer,
                              std::error_code &ec) override {}

    void handle_interrupt_transfer(std::uint32_t seqnum, const UsbEndpoint &ep,
                                   UsbInterface &interface, std::uint32_t transfer_flags,
                                   std::uint32_t transfer_buffer_length, TransferHandle transfer,
                                   std::error_code &ec) override {}

    void handle_isochronous_transfer(std::uint32_t seqnum, const UsbEndpoint &ep,
                                     UsbInterface &interface, std::uint32_t transfer_flags,
                                     std::uint32_t transfer_buffer_length, TransferHandle transfer,
                                     int num_iso_packets, std::error_code &ec) override {}
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

TEST_F(TransferHandleTest, DefaultConstructor) {
    TransferHandle handle;
    EXPECT_EQ(handle.get(), nullptr);
    EXPECT_EQ(handle.handler(), nullptr);
    EXPECT_FALSE(handle);
}

TEST_F(TransferHandleTest, ParameterizedConstructor) {
    void* ptr = handler->alloc_transfer_handle(100, 0, test_header, test_setup);
    TransferHandle handle(ptr, handler.get());
    EXPECT_EQ(handle.get(), ptr);
    EXPECT_EQ(handle.handler(), handler.get());
    EXPECT_TRUE(handle);
    // handle 析构时自动释放
}

TEST_F(TransferHandleTest, MoveConstructor) {
    void* ptr = handler->alloc_transfer_handle(100, 0, test_header, test_setup);
    int initial_alloc = handler->alloc_count.load();

    TransferHandle handle1(ptr, handler.get());
    TransferHandle handle2(std::move(handle1));

    // 所有权转移
    EXPECT_EQ(handle1.get(), nullptr);
    EXPECT_EQ(handle1.handler(), nullptr);
    EXPECT_EQ(handle2.get(), ptr);
    EXPECT_EQ(handle2.handler(), handler.get());

    // handle2 析构时释放
    handle2.reset();
    EXPECT_EQ(handler->free_count.load(), 1);
    EXPECT_EQ(handler->last_freed_handle.load(), ptr);
}

TEST_F(TransferHandleTest, MoveAssignment) {
    void* ptr1 = handler->alloc_transfer_handle(100, 0, test_header, test_setup);
    void* ptr2 = handler->alloc_transfer_handle(200, 0, test_header, test_setup);

    TransferHandle handle1(ptr1, handler.get());
    TransferHandle handle2(ptr2, handler.get());

    // 移动赋值，handle1 原有的资源应该被释放
    handle1 = std::move(handle2);

    EXPECT_EQ(handler->free_count.load(), 1);  // ptr1 被释放
    EXPECT_EQ(handler->last_freed_handle.load(), ptr1);

    EXPECT_EQ(handle1.get(), ptr2);
    EXPECT_EQ(handle2.get(), nullptr);
}

TEST_F(TransferHandleTest, SelfMoveAssignment) {
    void* ptr = handler->alloc_transfer_handle(100, 0, test_header, test_setup);
    TransferHandle handle(ptr, handler.get());

    // 自我赋值应该安全
    handle = std::move(handle);
    EXPECT_EQ(handle.get(), ptr);
    EXPECT_EQ(handler->free_count.load(), 0);

    // 正常析构
    handle.reset();
    EXPECT_EQ(handler->free_count.load(), 1);
}

TEST_F(TransferHandleTest, Destructor) {
    void* ptr = handler->alloc_transfer_handle(100, 0, test_header, test_setup);
    int initial_free = handler->free_count.load();

    {
        TransferHandle handle(ptr, handler.get());
    }  // 析构

    EXPECT_EQ(handler->free_count.load(), initial_free + 1);
    EXPECT_EQ(handler->last_freed_handle.load(), ptr);
}

TEST_F(TransferHandleTest, Reset) {
    void* ptr = handler->alloc_transfer_handle(100, 0, test_header, test_setup);
    TransferHandle handle(ptr, handler.get());

    handle.reset();
    EXPECT_EQ(handle.get(), nullptr);
    EXPECT_EQ(handle.handler(), nullptr);
    EXPECT_EQ(handler->free_count.load(), 1);
    EXPECT_EQ(handler->last_freed_handle.load(), ptr);

    // 多次 reset 安全
    handle.reset();
    EXPECT_EQ(handler->free_count.load(), 1);  // 只释放一次
}

TEST_F(TransferHandleTest, Release) {
    void* ptr = handler->alloc_transfer_handle(100, 0, test_header, test_setup);
    TransferHandle handle(ptr, handler.get());

    void* released = handle.release();
    EXPECT_EQ(released, ptr);
    EXPECT_EQ(handle.get(), nullptr);
    EXPECT_EQ(handle.handler(), nullptr);
    EXPECT_EQ(handler->free_count.load(), 0);  // 未释放

    // 需要手动释放
    handler->free_transfer_handle(released);
    EXPECT_EQ(handler->free_count.load(), 1);
}

TEST_F(TransferHandleTest, SetHandlerAndSetHandle) {
    void* ptr = handler->alloc_transfer_handle(100, 0, test_header, test_setup);
    TransferHandle handle;
    handle.set_handler(handler.get());
    handle.set_handle(ptr);

    EXPECT_EQ(handle.handler(), handler.get());
    EXPECT_EQ(handle.get(), ptr);
    // handle 析构时自动释放
}

// ============== 极端情况测试 ==============

TEST_F(TransferHandleTest, NullPointerHandling) {
    TransferHandle handle(nullptr, handler.get());
    EXPECT_FALSE(handle);

    // 重置空指针不应该崩溃
    handle.reset();
    EXPECT_EQ(handler->free_count.load(), 0);  // 空指针不调用 free

    TransferHandle handle2(nullptr, nullptr);
    handle2.reset();
    EXPECT_EQ(handler->free_count.load(), 0);  // handler 也为空
}

TEST_F(TransferHandleTest, NullHandlerHandling) {
    void* ptr = reinterpret_cast<void*>(0x1234);
    TransferHandle handle(ptr, nullptr);

    // handler 为空时 reset 不应该崩溃
    handle.reset();
    EXPECT_EQ(handler->free_count.load(), 0);  // handler 为空不调用 free
}

TEST_F(TransferHandleTest, MoveFromEmpty) {
    TransferHandle empty;
    TransferHandle handle(std::move(empty));

    EXPECT_EQ(handle.get(), nullptr);
    EXPECT_EQ(empty.get(), nullptr);

    // 析构空 handle 不应该崩溃
}

TEST_F(TransferHandleTest, MoveToEmpty) {
    void* ptr = handler->alloc_transfer_handle(100, 0, test_header, test_setup);
    TransferHandle empty;
    TransferHandle handle(ptr, handler.get());

    empty = std::move(handle);

    EXPECT_EQ(empty.get(), ptr);
    EXPECT_EQ(handle.get(), nullptr);
    EXPECT_EQ(handler->free_count.load(), 0);

    empty.reset();
    EXPECT_EQ(handler->free_count.load(), 1);
}

TEST_F(TransferHandleTest, MoveToNonEmpty) {
    void* ptr1 = handler->alloc_transfer_handle(100, 0, test_header, test_setup);
    void* ptr2 = handler->alloc_transfer_handle(200, 0, test_header, test_setup);

    TransferHandle handle1(ptr1, handler.get());
    TransferHandle handle2(ptr2, handler.get());

    // handle1 已有资源，移动后应该释放原资源
    handle1 = std::move(handle2);

    EXPECT_EQ(handler->free_count.load(), 1);
    EXPECT_EQ(handler->last_freed_handle.load(), ptr1);
}

TEST_F(TransferHandleTest, ReleaseThenReset) {
    void* ptr = handler->alloc_transfer_handle(100, 0, test_header, test_setup);
    TransferHandle handle(ptr, handler.get());

    void* released = handle.release();
    handle.reset();  // reset 空 handle，不应崩溃

    EXPECT_EQ(handler->free_count.load(), 0);
    handler->free_transfer_handle(released);
}

TEST_F(TransferHandleTest, DoubleMove) {
    void* ptr = handler->alloc_transfer_handle(100, 0, test_header, test_setup);

    TransferHandle h1(ptr, handler.get());
    TransferHandle h2(std::move(h1));
    TransferHandle h3(std::move(h2));

    EXPECT_EQ(h1.get(), nullptr);
    EXPECT_EQ(h2.get(), nullptr);
    EXPECT_EQ(h3.get(), ptr);

    h3.reset();
    EXPECT_EQ(handler->free_count.load(), 1);
}

// ============== 多线程测试 ==============

TEST_F(TransferHandleTest, ThreadSafeMove) {
    constexpr int num_threads = 8;
    constexpr int iterations = 1000;
    std::atomic<int> total_frees{0};

    std::vector<std::thread> threads;
    for (int t = 0; t < num_threads; ++t) {
        threads.emplace_back([&]() {
            for (int i = 0; i < iterations; ++i) {
                void* ptr = handler->alloc_transfer_handle(64, 0, test_header, test_setup);
                TransferHandle h1(ptr, handler.get());
                TransferHandle h2(std::move(h1));
                h2.reset();
                total_frees++;
            }
        });
    }

    for (auto& th : threads) {
        th.join();
    }

    EXPECT_EQ(total_frees.load(), num_threads * iterations);
    EXPECT_EQ(handler->free_count.load(), num_threads * iterations);
}

TEST_F(TransferHandleTest, ConcurrentAllocFree) {
    constexpr int num_threads = 4;
    std::vector<std::vector<TransferHandle>> handles_per_thread(num_threads);
    std::vector<std::thread> threads;

    for (int t = 0; t < num_threads; ++t) {
        threads.emplace_back([&, t]() {
            for (int i = 0; i < 100; ++i) {
                void* ptr = handler->alloc_transfer_handle(64, 0, test_header, test_setup);
                handles_per_thread[t].emplace_back(ptr, handler.get());
            }
        });
    }

    for (auto& th : threads) {
        th.join();
    }

    // 所有 handle 析构时应该释放
    for (auto& handles : handles_per_thread) {
        handles.clear();
    }

    EXPECT_EQ(handler->free_count.load(), num_threads * 100);
}

// ============== 边界条件测试 ==============

TEST_F(TransferHandleTest, ZeroBufferLength) {
    void* ptr = handler->alloc_transfer_handle(0, 0, test_header, test_setup);
    TransferHandle handle(ptr, handler.get());
    handle.reset();
    EXPECT_EQ(handler->free_count.load(), 1);
}

TEST_F(TransferHandleTest, LargeBufferLength) {
    void* ptr = handler->alloc_transfer_handle(1024 * 1024, 0, test_header, test_setup);  // 1MB
    TransferHandle handle(ptr, handler.get());
    EXPECT_TRUE(handle);
    handle.reset();
    EXPECT_EQ(handler->free_count.load(), 1);
}

TEST_F(TransferHandleTest, WithIsoPackets) {
    void* ptr = handler->alloc_transfer_handle(1024, 8, test_header, test_setup);
    TransferHandle handle(ptr, handler.get());
    handle.reset();
    EXPECT_EQ(handler->free_count.load(), 1);
}

// ============== 与 UsbIpRetSubmit 集成测试 ==============

TEST_F(TransferHandleTest, CreateRetSubmitWithTransfer) {
    void* ptr = handler->alloc_transfer_handle(100, 0, test_header, test_setup);
    TransferHandle transfer(ptr, handler.get());

    auto ret = UsbIpResponse::UsbIpRetSubmit::create_ret_submit_ok_with_no_iso(
        123, 100, std::move(transfer));

    EXPECT_EQ(ret.header.seqnum, 123u);
    EXPECT_EQ(ret.actual_length, 100u);
    EXPECT_TRUE(ret.transfer);
    EXPECT_EQ(transfer.get(), nullptr);  // 已移动

    // ret.transfer 析构时会释放
}

TEST_F(TransferHandleTest, CreateRetSubmitWithoutData) {
    auto ret = UsbIpResponse::UsbIpRetSubmit::create_ret_submit_ok_without_data(456, 0);

    EXPECT_EQ(ret.header.seqnum, 456u);
    EXPECT_FALSE(ret.transfer);  // 无 transfer
    EXPECT_EQ(handler->free_count.load(), 0);  // 未涉及任何释放
}

TEST_F(TransferHandleTest, CreateRetSubmitEpipe) {
    void* ptr = handler->alloc_transfer_handle(100, 0, test_header, test_setup);
    TransferHandle transfer(ptr, handler.get());

    auto ret = UsbIpResponse::UsbIpRetSubmit::create_ret_submit_epipe_no_iso(
        789, 0, std::move(transfer));

    EXPECT_EQ(ret.header.seqnum, 789u);
    EXPECT_TRUE(ret.transfer);
}

// ============== 异常安全性测试 ==============

TEST_F(TransferHandleTest, NoLeakOnException) {
    void* ptr = handler->alloc_transfer_handle(100, 0, test_header, test_setup);
    int initial_free = handler->free_count.load();

    try {
        TransferHandle handle(ptr, handler.get());
        throw std::runtime_error("test exception");
    } catch (...) {
        // 析构应该已经发生
    }

    EXPECT_EQ(handler->free_count.load(), initial_free + 1);
}
