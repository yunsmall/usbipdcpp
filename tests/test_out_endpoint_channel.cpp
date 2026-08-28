#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

#include "usbipdcpp/DeviceHandler/TransferOperator.h"
#include "usbipdcpp/virtual_device/OutEndpointChannel.h"

using namespace usbipdcpp;

namespace {

// 记录应答的测试通道：override reply 记录（不真的发 socket）
class RecordingOutChannel : public OutEndpointChannel {
public:
    struct Replied {
        std::uint32_t seqnum;
        std::uint32_t length;
        std::uint32_t status;
    };
    std::vector<Replied> replied;

protected:
    void reply(std::uint32_t seqnum, std::uint32_t length, std::uint32_t status = 0) override {
        replied.push_back({seqnum, length, status});
    }
};

// 构造带数据的传输句柄（op 必须比 handle 活得久：handle 析构时用 op 释放）
struct TransferMaker {
    GenericTransferOperator op;
    TransferHandle make(const data_type &data) {
        auto *trx = new GenericTransfer{};
        trx->data = data;
        return TransferHandle(trx, &op);
    }
};

} // namespace

TEST(TestOutEndpointChannel, RequestPendsUntilTaken) {
    RecordingOutChannel channel;
    channel.on_new_connection();
    TransferMaker maker;

    // 请求挂起：不应答（NAK 背压：主机 URB 挂着直到业务侧取走）
    channel.on_out_request(0x02, 10, maker.make({1, 2, 3}));
    EXPECT_TRUE(channel.replied.empty());
    ASSERT_EQ(channel.size(), 1u);

    // 业务侧取走：读出数据并应答
    auto p = channel.try_take();
    ASSERT_TRUE(p.has_value());
    EXPECT_EQ(p->ep, 0x02);
    EXPECT_EQ(p->data, (data_type{1, 2, 3}));
    ASSERT_EQ(channel.replied.size(), 1u);
    EXPECT_EQ(channel.replied[0].seqnum, 10u);
    EXPECT_EQ(channel.replied[0].length, 3u);
    EXPECT_EQ(channel.replied[0].status, 0u);
    EXPECT_EQ(channel.size(), 0u);
}

TEST(TestOutEndpointChannel, FifoOrderPreserved) {
    RecordingOutChannel channel;
    channel.on_new_connection();
    TransferMaker maker;

    // 多个请求按入队顺序取出，应答顺序一致
    channel.on_out_request(0x02, 1, maker.make({1}));
    channel.on_out_request(0x02, 2, maker.make({2, 2}));
    channel.on_out_request(0x02, 3, maker.make({3, 3, 3}));

    auto p1 = channel.try_take();
    auto p2 = channel.try_take();
    auto p3 = channel.try_take();
    ASSERT_TRUE(p1 && p2 && p3);
    EXPECT_EQ(p1->data, (data_type{1}));
    EXPECT_EQ(p2->data, (data_type{2, 2}));
    EXPECT_EQ(p3->data, (data_type{3, 3, 3}));
    ASSERT_EQ(channel.replied.size(), 3u);
    EXPECT_EQ(channel.replied[0].seqnum, 1u);
    EXPECT_EQ(channel.replied[1].seqnum, 2u);
    EXPECT_EQ(channel.replied[2].seqnum, 3u);
}

TEST(TestOutEndpointChannel, TakeBlocksUntilData) {
    RecordingOutChannel channel;
    channel.on_new_connection();
    TransferMaker maker;

    // 先阻塞取，另一个线程稍后入队：take 被唤醒
    std::thread producer([&]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        channel.on_out_request(0x02, 7, maker.make({9}));
    });
    auto p = channel.take();
    producer.join();
    ASSERT_TRUE(p.has_value());
    EXPECT_EQ(p->data, (data_type{9}));
}

TEST(TestOutEndpointChannel, TakeTimeoutReturnsNullopt) {
    RecordingOutChannel channel;
    channel.on_new_connection();
    // 无数据且未断连：超时返回 nullopt
    auto p = channel.take(30);
    EXPECT_FALSE(p.has_value());
}

TEST(TestOutEndpointChannel, InitialDisconnectedTakeReturnsNull) {
    RecordingOutChannel channel;
    // 未 on_new_connection（初始断连）：take 立即返回 nullopt
    auto p = channel.take(0);
    EXPECT_FALSE(p.has_value());
    // 断连时 on_out_request 直接释放请求，不入队
    TransferMaker maker;
    channel.on_out_request(0x02, 1, maker.make({1}));
    EXPECT_EQ(channel.size(), 0u);
}

TEST(TestOutEndpointChannel, EmptyDataRequestAnsweredWithZero) {
    RecordingOutChannel channel;
    channel.on_new_connection();
    TransferMaker maker;

    // 空数据请求：应答 0 长度，请求被消费
    channel.on_out_request(0x02, 60, maker.make({}));
    auto p = channel.try_take();
    ASSERT_TRUE(p.has_value());
    EXPECT_TRUE(p->data.empty());
    ASSERT_EQ(channel.replied.size(), 1u);
    EXPECT_EQ(channel.replied[0].seqnum, 60u);
    EXPECT_EQ(channel.replied[0].length, 0u);
}

TEST(TestOutEndpointChannel, NotificationWithoutTransfer) {
    RecordingOutChannel channel;
    channel.on_new_connection();

    // 纯通知（控制 IN 的 setup 透出）：transfer 为空，take 只返回 setup 不应答
    SetupPacket setup;
    setup.request = 0xAA;
    channel.on_out_request(0, 5, TransferHandle{}, setup);

    auto p = channel.try_take();
    ASSERT_TRUE(p.has_value());
    EXPECT_EQ(p->ep, 0);
    ASSERT_TRUE(p->setup_req.has_value());
    EXPECT_EQ(p->setup_req->request, 0xAA);
    EXPECT_TRUE(p->data.empty());
    EXPECT_TRUE(channel.replied.empty());
}

TEST(TestOutEndpointChannel, NotificationAndDataMixedOrder) {
    RecordingOutChannel channel;
    channel.on_new_connection();
    TransferMaker maker;

    // 通知与数据请求混排：按入队顺序消费
    SetupPacket setup;
    setup.request = 0xAA;
    channel.on_out_request(0x02, 1, maker.make({1}));
    channel.on_out_request(0, 2, TransferHandle{}, setup);
    channel.on_out_request(0x02, 3, maker.make({3}));

    auto p1 = channel.try_take();
    auto p2 = channel.try_take();
    auto p3 = channel.try_take();
    ASSERT_TRUE(p1 && p2 && p3);
    EXPECT_EQ(p1->data, (data_type{1}));
    EXPECT_TRUE(p2->setup_req.has_value());
    EXPECT_TRUE(p2->data.empty());
    EXPECT_EQ(p3->data, (data_type{3}));
    // 只有数据请求应答（seqnum 1、3），通知不应答
    ASSERT_EQ(channel.replied.size(), 2u);
    EXPECT_EQ(channel.replied[0].seqnum, 1u);
    EXPECT_EQ(channel.replied[1].seqnum, 3u);
}

TEST(TestOutEndpointChannel, MultipleEndpointsEpPreserved) {
    RecordingOutChannel channel;
    channel.on_new_connection();
    TransferMaker maker;

    // 不同端点请求：take 带回各自的 ep（队列全局 FIFO，ep 随请求走）
    channel.on_out_request(0x02, 1, maker.make({1}));
    channel.on_out_request(0x83, 2, maker.make({2}));
    channel.on_out_request(0x04, 3, maker.make({3}));

    auto p1 = channel.try_take();
    auto p2 = channel.try_take();
    auto p3 = channel.try_take();
    ASSERT_TRUE(p1 && p2 && p3);
    EXPECT_EQ(p1->ep, 0x02);
    EXPECT_EQ(p2->ep, 0x83);
    EXPECT_EQ(p3->ep, 0x04);
}

TEST(TestOutEndpointChannel, ManyPendingRequestsTakenInOrder) {
    RecordingOutChannel channel;
    channel.on_new_connection();
    TransferMaker maker;

    // 10 个请求全部挂起，再逐个 take：按入队顺序消费 + 应答
    for (std::uint32_t i = 1; i <= 10; i++) {
        channel.on_out_request(0x02, i, maker.make({static_cast<std::uint8_t>(i)}));
    }
    ASSERT_EQ(channel.size(), 10u);

    for (std::uint32_t i = 1; i <= 10; i++) {
        auto p = channel.try_take();
        ASSERT_TRUE(p.has_value());
        EXPECT_EQ(p->data, (data_type{static_cast<std::uint8_t>(i)}));
    }
    ASSERT_EQ(channel.replied.size(), 10u);
    for (std::uint32_t i = 0; i < 10; i++) {
        EXPECT_EQ(channel.replied[i].seqnum, i + 1);
        EXPECT_EQ(channel.replied[i].length, 1u);
    }
}

TEST(TestOutEndpointChannel, CancelPending) {
    RecordingOutChannel channel;
    channel.on_new_connection();
    TransferMaker maker;

    channel.on_out_request(0x02, 10, maker.make({1}));
    channel.on_out_request(0x02, 11, maker.make({2}));

    // 命中：请求移除（transfer 析构释放），不再应答
    EXPECT_TRUE(channel.cancel_pending(10));
    // 未命中
    EXPECT_FALSE(channel.cancel_pending(99));

    auto p = channel.try_take();
    ASSERT_TRUE(p.has_value());
    EXPECT_EQ(p->data, (data_type{2}));
    ASSERT_EQ(channel.replied.size(), 1u);
    EXPECT_EQ(channel.replied[0].seqnum, 11u);
}

TEST(TestOutEndpointChannel, DisconnectClearsAndWakes) {
    RecordingOutChannel channel;
    channel.on_new_connection();
    TransferMaker maker;

    channel.on_out_request(0x02, 1, maker.make({1}));
    channel.on_disconnection();

    // 队列已清：take 立即返回 nullopt（断连）
    auto p = channel.take(10);
    EXPECT_FALSE(p.has_value());
    EXPECT_EQ(channel.size(), 0u);
}

TEST(TestOutEndpointChannel, DisconnectWakesBlockedTaker) {
    RecordingOutChannel channel;
    channel.on_new_connection();

    // take 阻塞中断连：被唤醒并返回 nullopt
    std::thread disconnector([&]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        channel.on_disconnection();
    });
    auto p = channel.take();
    disconnector.join();
    EXPECT_FALSE(p.has_value());
}

TEST(TestOutEndpointChannel, MaxPendingRejectsWithBusy) {
    RecordingOutChannel channel;
    channel.on_new_connection();
    channel.set_max_pending(2);
    TransferMaker maker;

    channel.on_out_request(0x02, 1, maker.make({1}));
    channel.on_out_request(0x02, 2, maker.make({2}));
    // 超上限：EPIPE 拒绝，不入队
    channel.on_out_request(0x02, 3, maker.make({3}));

    ASSERT_EQ(channel.replied.size(), 1u);
    EXPECT_EQ(channel.replied[0].seqnum, 3u);
    EXPECT_EQ(channel.replied[0].status,
              static_cast<std::uint32_t>(UrbStatusType::StatusEPIPE));
    EXPECT_EQ(channel.size(), 2u);

    // 取走一条后空位释放，新请求可入队
    EXPECT_TRUE(channel.try_take().has_value());
    channel.on_out_request(0x02, 4, maker.make({4}));
    EXPECT_EQ(channel.size(), 2u);
}

TEST(TestOutEndpointChannel, GetTransferDataAppends) {
    // op 接口语义：数据追加到 out 末尾，返回追加字节数
    TransferMaker maker;
    auto handle = maker.make({1, 2, 3});

    bool supported = false;
    data_type out{0xAA};
    auto n = maker.op.get_transfer_data(handle.get(), out, supported);
    EXPECT_TRUE(supported);
    EXPECT_EQ(n, 3u);
    EXPECT_EQ(out, (data_type{0xAA, 1, 2, 3}));

    // 再读一次（handle 数据不变）：继续追加
    out.clear();
    n = maker.op.get_transfer_data(handle.get(), out, supported);
    EXPECT_EQ(n, 3u);
    EXPECT_EQ(out, (data_type{1, 2, 3}));

    // 默认实现：不支持（supported=false，返回 0）
    class DefaultOp : public TransferOperator {
    public:
        void *alloc_transfer_handle(std::size_t, int, const UsbIpHeaderBasic &, const SetupPacket &) override {
            return nullptr;
        }
        void free_transfer_handle(void *handle) override {}
        std::size_t get_actual_length(void *handle) override { return 0; }
        UsbIpIsoPacketDescriptor get_iso_descriptor(void *handle, int index) override { return {}; }
        void set_iso_descriptor(void *handle, int index, const UsbIpIsoPacketDescriptor &desc) override {}
        void send_transfer_data(void *handle, asio::ip::tcp::socket &sock, std::size_t length,
                                std::error_code &ec) override {}
        void recv_transfer_data(void *handle, asio::ip::tcp::socket &sock, std::size_t length,
                                std::error_code &ec) override {}
    };
    DefaultOp def_op;
    data_type buf;
    bool supported2 = true;
    EXPECT_EQ(def_op.get_transfer_data(nullptr, buf, supported2), 0u);
    EXPECT_FALSE(supported2);
}

TEST(TestOutEndpointChannel, ConcurrentPushAndTakeNoLoss) {
    RecordingOutChannel channel;
    channel.on_new_connection();
    TransferMaker maker;
    constexpr int N = 1000;

    std::thread producer([&]() {
        for (int i = 0; i < N; i++) {
            channel.on_out_request(0x02, i, maker.make({static_cast<std::uint8_t>(i)}));
        }
    });
    std::thread consumer([&]() {
        for (int i = 0; i < N; i++) {
            while (!channel.try_take().has_value()) {
                std::this_thread::yield();
            }
        }
    });
    producer.join();
    consumer.join();

    EXPECT_EQ(channel.size(), 0u);
    ASSERT_EQ(channel.replied.size(), N);
    // 应答 seqnum 覆盖 0..N-1 无重复（FIFO 无丢失）
    std::vector<bool> seen(N, false);
    for (auto &r: channel.replied) {
        ASSERT_LT(r.seqnum, N);
        seen[r.seqnum] = true;
    }
    for (int i = 0; i < N; i++) {
        EXPECT_TRUE(seen[i]);
    }
}

TEST(TestOutEndpointChannel, ConcurrentBlockingTakeAndPush) {
    RecordingOutChannel channel;
    channel.on_new_connection();
    TransferMaker maker;
    constexpr int N = 500;

    // 多个线程反复阻塞 take（每个消费一条），一个线程持续入队
    std::atomic<int> consumed{0};
    std::vector<std::thread> takers;
    for (int t = 0; t < 4; t++) {
        takers.emplace_back([&]() {
            for (int i = 0; i < N / 4; i++) {
                auto p = channel.take();
                if (p) {
                    consumed++;
                }
            }
        });
    }
    std::thread producer([&]() {
        for (int i = 0; i < N; i++) {
            channel.on_out_request(0x02, i, maker.make({}));
        }
    });
    for (auto &t: takers) {
        t.join();
    }
    producer.join();

    EXPECT_EQ(consumed, N);
    EXPECT_EQ(channel.size(), 0u);
    EXPECT_EQ(channel.replied.size(), N);
}

TEST(TestOutEndpointChannel, ConcurrentDisconnectAndPushNoCrash) {
    // 断连与入队并发：不崩溃，最终状态一致（断连后请求不再入队）
    RecordingOutChannel channel;
    channel.on_new_connection();
    TransferMaker maker;

    std::thread disconnector([&]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        channel.on_disconnection();
    });
    std::thread pusher([&]() {
        for (int i = 0; i < 500; i++) {
            channel.on_out_request(0x02, i, maker.make({}));
        }
    });
    disconnector.join();
    pusher.join();

    // 断连标志生效后请求被直接释放：队列最终为空
    EXPECT_EQ(channel.size(), 0u);
    auto p = channel.take(10);
    EXPECT_FALSE(p.has_value());
}

TEST(TestOutEndpointChannel, ConcurrentSubmitAndUnlink) {
    // UNLINK（cancel_pending）与请求入队（on_out_request）并发：
    // 每条请求最终恰好一种结局（被应答或被取消），不丢不重。
    // 竞态在两个独立锁内（入队持 mutex_，cancel 也持同一个锁），合法结果唯一。
    RecordingOutChannel channel;
    channel.on_new_connection();
    TransferMaker maker;
    constexpr int N = 2000;

    std::thread submitter([&]() {
        for (int i = 0; i < N; i++) {
            channel.on_out_request(0x02, i, maker.make({static_cast<std::uint8_t>(i)}));
        }
    });
    std::atomic<int> cancelled{0};
    std::thread unlinker([&]() {
        for (int i = 0; i < N; i++) {
            if (channel.cancel_pending(static_cast<std::uint32_t>(i))) {
                cancelled++;
            }
        }
    });
    submitter.join();
    unlinker.join();

    // 未被取消的请求仍留在队列，全部取出应答（Pending 无 seqnum，验证用 replied）
    while (channel.try_take()) {
    }

    // 每条请求两种结局互斥且完备：应答数 + 取消数 == N（竞态在锁内，不重不丢）
    EXPECT_EQ(channel.replied.size() + static_cast<std::size_t>(cancelled),
              static_cast<std::size_t>(N));

    // 应答的 seqnum 无重复且在范围内
    std::vector<bool> replied_seen(N, false);
    for (auto &r: channel.replied) {
        ASSERT_LT(r.seqnum, static_cast<std::uint32_t>(N));
        ASSERT_FALSE(replied_seen[r.seqnum]);
        replied_seen[r.seqnum] = true;
    }
    // 没应答的一定被取消：取消数 == 未应答数
    std::size_t not_replied = N - channel.replied.size();
    EXPECT_EQ(static_cast<std::size_t>(cancelled), not_replied);
}
