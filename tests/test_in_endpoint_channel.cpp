#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <deque>
#include <functional>
#include <thread>
#include <vector>

#include "usbipdcpp/DeviceHandler/TransferOperator.h"
#include "usbipdcpp/utils/RingBuffer.h"
#include "usbipdcpp/virtual_device/InEndpointChannel.h"

using namespace usbipdcpp;

namespace {

// 测试通道：记录发出的应答，模拟消息模式缓冲（整条消费）
class RecordingChannel : public InEndpointChannelBase<RecordingChannel> {
public:
    struct Sent {
        std::uint32_t seqnum;
        std::uint32_t length;
        data_type data;
    };

    std::vector<Sent> sent;
    std::deque<data_type> buffer;
    std::function<data_type(std::uint32_t)> pull; // 可选 pull，模拟 try_pull_data
    int pull_calls = 0;
    std::size_t max_pending = 0;

    // 模拟 MessageInChannel::push（持双锁 + 入缓冲 + 匹配挂起请求）
    void push(data_type data) {
        std::lock(channel_mutex, requests_mutex);
        std::lock_guard lock1(channel_mutex, std::adopt_lock);
        std::lock_guard lock2(requests_mutex, std::adopt_lock);
        if (max_pending != 0 && buffer.size() >= max_pending) {
            buffer.pop_front();
        }
        buffer.emplace_back(std::move(data));
        try_send_pending_locked();
    }

    bool buffer_empty() const {
        return buffer.empty();
    }

    void try_send_one(std::uint8_t ep, std::uint32_t seqnum, std::uint32_t length, TransferHandle transfer) {
        // 取最旧一条按长度截断，记录（transfer 由析构释放）
        auto &front = buffer.front();
        auto send_len = std::min(front.size(), static_cast<std::size_t>(length));
        sent.push_back({seqnum, static_cast<std::uint32_t>(send_len),
                        data_type(front.begin(), front.begin() + send_len)});
        buffer.pop_front();
    }

    data_type try_pull_data(std::uint32_t length) {
        pull_calls++;
        return pull ? pull(length) : data_type{};
    }

    void send_pulled_locked(std::uint8_t ep, std::uint32_t seqnum, std::uint32_t length, TransferHandle transfer,
                            data_type pulled) {
        // 与 MessageInChannel 一致：整条入缓冲后由本次请求消费
        push_locked(std::move(pulled));
        try_send_one(ep, seqnum, length, std::move(transfer));
    }

    void push_locked(data_type data) {
        buffer.emplace_back(std::move(data));
    }

    void buffer_clear() {
        buffer.clear();
    }
};

// 字节流分片变体通道：RingBuffer 缓冲，按请求长度分片消费（模拟 ByteStreamInChannel）
class ShardChannel : public InEndpointChannelBase<ShardChannel> {
public:
    struct Sent {
        std::uint32_t seqnum;
        std::uint32_t length;
    };

    std::vector<Sent> sent;
    RingBuffer buffer{64 * 1024};
    std::function<data_type(std::uint32_t)> pull; // 可选 pull，模拟 try_pull_data
    int pull_calls = 0;

    void write(const std::uint8_t *data, std::size_t size) {
        std::lock(channel_mutex, requests_mutex);
        std::lock_guard lock1(channel_mutex, std::adopt_lock);
        std::lock_guard lock2(requests_mutex, std::adopt_lock);
        buffer.write(data, size);
        try_send_pending_locked();
    }

    bool buffer_empty() const {
        return buffer.empty();
    }

    void try_send_one(std::uint8_t ep, std::uint32_t seqnum, std::uint32_t length, TransferHandle transfer) {
        // 按请求长度分片取出，剩余留在缓冲
        std::size_t send_len = std::min(buffer.size(), static_cast<std::size_t>(length));
        data_type data(send_len);
        buffer.read(data.data(), send_len);
        sent.push_back({seqnum, static_cast<std::uint32_t>(send_len)});
    }

    data_type try_pull_data(std::uint32_t length) {
        pull_calls++;
        return pull ? pull(length) : data_type{};
    }

    void send_pulled_locked(std::uint8_t ep, std::uint32_t seqnum, std::uint32_t length, TransferHandle transfer,
                            data_type pulled) {
        // 与 ByteStreamInChannel 一致：本次请求优先发 min 部分，剩余入缓冲
        std::size_t send_len = std::min(pulled.size(), static_cast<std::size_t>(length));
        sent.push_back({seqnum, static_cast<std::uint32_t>(send_len)});
        if (pulled.size() > send_len) {
            buffer.write(pulled.data() + send_len, pulled.size() - send_len);
        }
    }

    void push_locked(data_type data) {
        buffer.write(data.data(), data.size());
    }

    void buffer_clear() {
        buffer.clear();
    }
};

// 构造一个传输句柄（op 必须比 handle 活得久：handle 析构时用 op 释放）
struct TransferMaker {
    GenericTransferOperator op;
    TransferHandle make() {
        return TransferHandle(new GenericTransfer{}, &op);
    }
};

} // namespace

TEST(TestInEndpointChannel, RequestPendsThenPushAnswers) {
    RecordingChannel channel;
    channel.on_new_connection();
    TransferMaker maker;

    // 无数据：请求挂起
    channel.on_in_request(0x81, 10, 8, maker.make());
    EXPECT_TRUE(channel.sent.empty());

    // 数据到达：匹配挂起请求并应答
    channel.push({1, 2, 3, 4, 5});
    ASSERT_EQ(channel.sent.size(), 1u);
    EXPECT_EQ(channel.sent[0].seqnum, 10u);
    EXPECT_EQ(channel.sent[0].length, 5u);
    EXPECT_EQ(channel.sent[0].data, (data_type{1, 2, 3, 4, 5}));
}

TEST(TestInEndpointChannel, BufferedDataServedImmediately) {
    RecordingChannel channel;
    channel.on_new_connection();
    TransferMaker maker;

    channel.push({0xAA, 0xBB});
    channel.on_in_request(0x81, 20, 8, maker.make());

    ASSERT_EQ(channel.sent.size(), 1u);
    EXPECT_EQ(channel.sent[0].seqnum, 20u);
    EXPECT_EQ(channel.sent[0].data, (data_type{0xAA, 0xBB}));
}

TEST(TestInEndpointChannel, MessageTruncatedToRequestLength) {
    RecordingChannel channel;
    channel.on_new_connection();
    TransferMaker maker;

    channel.push({1, 2, 3, 4, 5});
    channel.on_in_request(0x81, 30, 3, maker.make()); // 请求只要 3 字节

    ASSERT_EQ(channel.sent.size(), 1u);
    EXPECT_EQ(channel.sent[0].length, 3u);
    EXPECT_EQ(channel.sent[0].data, (data_type{1, 2, 3}));
    EXPECT_TRUE(channel.buffer.empty()); // 整条消费，剩余丢弃（消息语义）
}

TEST(TestInEndpointChannel, SameEndpointFifoOrder) {
    RecordingChannel channel;
    channel.on_new_connection();
    TransferMaker maker;

    // 同端点两个请求先后挂起，数据逐条到达：先到的请求先应答
    channel.on_in_request(0x81, 1, 8, maker.make());
    channel.on_in_request(0x81, 2, 8, maker.make());
    channel.push({0x01});
    channel.push({0x02});

    ASSERT_EQ(channel.sent.size(), 2u);
    EXPECT_EQ(channel.sent[0].seqnum, 1u);
    EXPECT_EQ(channel.sent[1].seqnum, 2u);
}

TEST(TestInEndpointChannel, PendingRequestBlocksLaterBufferedRequest) {
    RecordingChannel channel;
    channel.on_new_connection();
    TransferMaker maker;

    // 同端点已有挂起请求时，即使缓冲有数据新请求也必须排队（FIFO）
    channel.on_in_request(0x81, 1, 8, maker.make());
    channel.push({0x01});       // 应答请求 1
    channel.push({0x02});       // 无挂起请求了，入缓冲
    channel.on_in_request(0x81, 2, 8, maker.make()); // 缓冲有数据 → 立即应答

    ASSERT_EQ(channel.sent.size(), 2u);
    EXPECT_EQ(channel.sent[0].seqnum, 1u);
    EXPECT_EQ(channel.sent[1].seqnum, 2u);
    EXPECT_EQ(channel.sent[1].data, (data_type{0x02}));
}

TEST(TestInEndpointChannel, MaxPendingDropsOldest) {
    RecordingChannel channel;
    channel.on_new_connection();
    channel.max_pending = 2;
    TransferMaker maker;

    // 无请求时推入 3 条：第 1 条被丢（保持最新消息语义）
    channel.push({0x01});
    channel.push({0x02});
    channel.push({0x03});

    channel.on_in_request(0x81, 40, 8, maker.make());
    ASSERT_EQ(channel.sent.size(), 1u);
    EXPECT_EQ(channel.sent[0].data, (data_type{0x02}));
}

TEST(TestInEndpointChannel, PullProducesDataOnRequest) {
    RecordingChannel channel;
    channel.on_new_connection();
    TransferMaker maker;

    // pull 模型：请求到达且无缓冲数据时，回调现场生成数据
    channel.pull = [](std::uint32_t length) { return data_type{0x42}; };
    channel.on_in_request(0x81, 50, 8, maker.make());

    ASSERT_EQ(channel.sent.size(), 1u);
    EXPECT_EQ(channel.sent[0].seqnum, 50u);
    EXPECT_EQ(channel.sent[0].data, (data_type{0x42}));
    EXPECT_EQ(channel.sent[0].length, 1u);
    EXPECT_EQ(channel.pull_calls, 1);
}

TEST(TestInEndpointChannel, PullDataTruncatedToRequestLength) {
    RecordingChannel channel;
    channel.on_new_connection();
    TransferMaker maker;

    // pull 生成的整包数据经缓冲流转：请求只取前 4 字节，整条消费
    channel.pull = [](std::uint32_t length) { return data_type{1, 2, 3, 4, 5, 6, 7, 8, 9, 10}; };
    channel.on_in_request(0x81, 51, 4, maker.make());

    ASSERT_EQ(channel.sent.size(), 1u);
    EXPECT_EQ(channel.sent[0].length, 4u);
    EXPECT_EQ(channel.sent[0].data, (data_type{1, 2, 3, 4}));
    EXPECT_TRUE(channel.buffer.empty());
}

TEST(TestInEndpointChannel, PullNotCalledWhenSameEndpointHasPending) {
    RecordingChannel channel;
    channel.on_new_connection();
    TransferMaker maker;

    // pull 返回空（无数据可现场生成）
    channel.pull = [](std::uint32_t length) { return data_type{}; };

    // 请求 A：pull 被调但无数据 → 请求挂起
    channel.on_in_request(0x81, 1, 8, maker.make());
    EXPECT_EQ(channel.pull_calls, 1);
    // 同端点请求 B 到达：同端点已有挂起，不应再调 pull
    channel.on_in_request(0x81, 2, 8, maker.make());
    EXPECT_EQ(channel.pull_calls, 1);
    // 不同端点请求 C：可以调 pull
    channel.on_in_request(0x82, 3, 8, maker.make());
    EXPECT_EQ(channel.pull_calls, 2);
    // A 仍是挂起状态（pull 空数据未消费请求）
    EXPECT_TRUE(channel.sent.empty());
}

TEST(TestInEndpointChannel, ByteStreamPullPrefersCurrentRequest) {
    ShardChannel channel;
    channel.on_new_connection();
    TransferMaker maker;

    // pull 生成 10 字节，请求只要 4：本次请求发满 4，剩余 6 留在缓冲
    channel.pull = [](std::uint32_t length) { return data_type{1, 2, 3, 4, 5, 6, 7, 8, 9, 10}; };
    channel.on_in_request(0x81, 70, 4, maker.make());

    ASSERT_EQ(channel.sent.size(), 1u);
    EXPECT_EQ(channel.sent[0].seqnum, 70u);
    EXPECT_EQ(channel.sent[0].length, 4u);
    EXPECT_EQ(channel.buffer.size(), 6u);

    // 后续请求消费剩余部分
    channel.on_in_request(0x81, 71, 8, maker.make());
    ASSERT_EQ(channel.sent.size(), 2u);
    EXPECT_EQ(channel.sent[1].length, 6u);
    EXPECT_TRUE(channel.buffer.empty());
}

TEST(TestInEndpointChannel, ByteStreamPullAllSentWhenFits) {
    ShardChannel channel;
    channel.on_new_connection();
    TransferMaker maker;

    // pull 数据 ≤ 请求长度：全部发走，缓冲不残留
    channel.pull = [](std::uint32_t length) { return data_type{1, 2, 3}; };
    channel.on_in_request(0x81, 72, 8, maker.make());

    ASSERT_EQ(channel.sent.size(), 1u);
    EXPECT_EQ(channel.sent[0].length, 3u);
    EXPECT_TRUE(channel.buffer.empty());
}

TEST(TestInEndpointChannel, ByteStreamChannelLifecycle) {
    // 真实字节流通道：实例化 + 连接生命周期 + 写读，不触发 session 使用
    // （完整挂起-应答路径由端到端测试覆盖）
    ByteStreamInChannel channel;

    channel.set_capacity(1024);
    EXPECT_EQ(channel.capacity(), 1024u);
    EXPECT_EQ(channel.size(), 0u);

    // 初始断连：write_nb 直接拒绝
    const std::uint8_t data[] = {1, 2, 3};
    EXPECT_EQ(channel.write_nb(data, 3), 0u);

    // 连接后可写，查询正确
    channel.on_new_connection();
    EXPECT_EQ(channel.write_nb(data, 3), 3u);
    EXPECT_EQ(channel.size(), 3u);
    EXPECT_EQ(channel.available(), 1021u);

    // 断连清空缓冲
    channel.on_disconnection();
    EXPECT_EQ(channel.size(), 0u);
}

TEST(TestInEndpointChannel, EmptyMessagePushed) {
    RecordingChannel channel;
    channel.on_new_connection();
    TransferMaker maker;

    // 空消息：应答 0 字节（与主机请求语义一致，请求被消费）
    channel.push({});
    channel.on_in_request(0x81, 60, 8, maker.make());

    ASSERT_EQ(channel.sent.size(), 1u);
    EXPECT_EQ(channel.sent[0].seqnum, 60u);
    EXPECT_EQ(channel.sent[0].length, 0u);
    EXPECT_TRUE(channel.buffer.empty());
}

TEST(TestInEndpointChannel, MultipleEndpointsIndependentFifo) {
    RecordingChannel channel;
    channel.on_new_connection();
    TransferMaker maker;

    // 0x81 两个请求、0x82 一个请求：同端点各自 FIFO
    channel.on_in_request(0x81, 1, 8, maker.make());
    channel.on_in_request(0x82, 2, 8, maker.make());
    channel.on_in_request(0x81, 3, 8, maker.make());
    channel.push({0x01});
    channel.push({0x02});
    channel.push({0x03});

    ASSERT_EQ(channel.sent.size(), 3u);
    // 0x81 的两个请求必须按 1 → 3 顺序（可能被 0x82 的请求穿插，但互相不乱序）
    std::vector<std::uint32_t> order;
    for (auto &s: channel.sent) {
        if (s.seqnum == 1 || s.seqnum == 3) {
            order.push_back(s.seqnum);
        }
    }
    ASSERT_EQ(order.size(), 2u);
    EXPECT_EQ(order[0], 1u);
    EXPECT_EQ(order[1], 3u);
    // 0x82 的请求也被应答
    bool ep82_served = false;
    for (auto &s: channel.sent) {
        ep82_served |= (s.seqnum == 2);
    }
    EXPECT_TRUE(ep82_served);
}

TEST(TestInEndpointChannel, ManyPendingRequestsServedInOrder) {
    RecordingChannel channel;
    channel.on_new_connection();
    TransferMaker maker;

    // 10 个请求全部挂起，再逐条 push：必须按入队顺序全部应答
    for (std::uint32_t i = 1; i <= 10; i++) {
        channel.on_in_request(0x81, i, 8, maker.make());
    }
    for (int i = 1; i <= 10; i++) {
        channel.push(data_type{static_cast<std::uint8_t>(i)});
    }

    ASSERT_EQ(channel.sent.size(), 10u);
    for (std::uint32_t i = 0; i < 10; i++) {
        EXPECT_EQ(channel.sent[i].seqnum, i + 1);
        EXPECT_EQ(channel.sent[i].data, (data_type{static_cast<std::uint8_t>(i + 1)}));
    }
}

TEST(TestInEndpointChannel, ByteStreamShardingAcrossRequests) {
    ShardChannel channel;
    channel.on_new_connection();
    TransferMaker maker;

    // 写入 100 字节，请求 64 → 答 64 剩 36；再请求 → 答 36（分片语义）
    data_type payload(100, 0xAB);
    channel.write(payload.data(), payload.size());
    channel.on_in_request(0x81, 1, 64, maker.make());
    channel.on_in_request(0x81, 2, 64, maker.make());

    ASSERT_EQ(channel.sent.size(), 2u);
    EXPECT_EQ(channel.sent[0].seqnum, 1u);
    EXPECT_EQ(channel.sent[0].length, 64u);
    EXPECT_EQ(channel.sent[1].seqnum, 2u);
    EXPECT_EQ(channel.sent[1].length, 36u);
}

TEST(TestInEndpointChannel, LengthZeroRequestDoesNotConsumeStreamData) {
    ShardChannel channel;
    channel.on_new_connection();
    TransferMaker maker;

    // 字节流模式：length=0 的请求应答 0 字节，缓冲数据保留给后续请求
    data_type payload(10, 0xAB);
    channel.write(payload.data(), payload.size());
    channel.on_in_request(0x81, 1, 0, maker.make());
    ASSERT_EQ(channel.sent.size(), 1u);
    EXPECT_EQ(channel.sent[0].length, 0u);
    EXPECT_FALSE(channel.buffer.empty()); // 缓冲数据未被消费

    channel.on_in_request(0x81, 2, 64, maker.make());
    ASSERT_EQ(channel.sent.size(), 2u);
    EXPECT_EQ(channel.sent[1].length, 10u);
    EXPECT_TRUE(channel.buffer.empty());
}

TEST(TestInEndpointChannel, LengthZeroRequestConsumesOneMessage) {
    RecordingChannel channel;
    channel.on_new_connection();
    TransferMaker maker;

    // 消息模式：length=0 的请求应答 0 字节，但仍消费一条消息（消息语义）
    channel.push({1, 2, 3, 4, 5});
    channel.on_in_request(0x81, 1, 0, maker.make());
    ASSERT_EQ(channel.sent.size(), 1u);
    EXPECT_EQ(channel.sent[0].length, 0u);
    EXPECT_TRUE(channel.buffer.empty()); // 消息被消费

    channel.push({0x07});
    channel.on_in_request(0x81, 2, 8, maker.make());
    ASSERT_EQ(channel.sent.size(), 2u);
    EXPECT_EQ(channel.sent[1].data, (data_type{0x07}));
}

TEST(TestInEndpointChannel, RequestsOutnumberMessages) {
    RecordingChannel channel;
    channel.on_new_connection();
    TransferMaker maker;

    // 3 个请求挂起，只来 1 条消息：应答 1 个，其余继续挂起
    channel.on_in_request(0x81, 1, 8, maker.make());
    channel.on_in_request(0x81, 2, 8, maker.make());
    channel.on_in_request(0x81, 3, 8, maker.make());
    channel.push({0x01});

    ASSERT_EQ(channel.sent.size(), 1u);
    EXPECT_EQ(channel.sent[0].seqnum, 1u);
    // 剩余请求可被后续数据应答
    channel.push({0x02});
    channel.push({0x03});
    ASSERT_EQ(channel.sent.size(), 3u);
    EXPECT_EQ(channel.sent[1].seqnum, 2u);
    EXPECT_EQ(channel.sent[2].seqnum, 3u);
}

TEST(TestInEndpointChannel, PullNotCalledWhenBufferHasData) {
    RecordingChannel channel;
    channel.on_new_connection();
    TransferMaker maker;

    // 缓冲有数据时请求直接应答，不调 pull（pull 只在缓冲空时兜底）
    channel.pull = [](std::uint32_t length) { return data_type{0x42}; };
    channel.push({0x01});
    channel.on_in_request(0x81, 1, 8, maker.make());

    ASSERT_EQ(channel.sent.size(), 1u);
    EXPECT_EQ(channel.sent[0].data, (data_type{0x01}));
    EXPECT_EQ(channel.pull_calls, 0);
}

TEST(TestInEndpointChannel, DoubleDisconnectionIdempotent) {
    RecordingChannel channel;
    channel.on_new_connection();
    TransferMaker maker;

    channel.on_in_request(0x81, 1, 8, maker.make());
    channel.on_disconnection();
    channel.on_disconnection(); // 重复断连幂等，不崩溃不清错东西

    EXPECT_FALSE(channel.cancel_pending(1));
    EXPECT_TRUE(channel.buffer.empty());
    channel.on_new_connection();
    channel.on_in_request(0x81, 2, 8, maker.make());
    channel.push({0x01});
    ASSERT_EQ(channel.sent.size(), 1u);
    EXPECT_EQ(channel.sent[0].seqnum, 2u);
}

TEST(TestInEndpointChannel, ByteStreamHangingRequestServedByWrite) {
    ShardChannel channel;
    channel.on_new_connection();
    TransferMaker maker;

    // 请求先挂起，数据后到：write 匹配挂起请求
    channel.on_in_request(0x81, 1, 64, maker.make());
    data_type payload(10, 0xCD);
    channel.write(payload.data(), payload.size());

    ASSERT_EQ(channel.sent.size(), 1u);
    EXPECT_EQ(channel.sent[0].seqnum, 1u);
    EXPECT_EQ(channel.sent[0].length, 10u);
}

TEST(TestInEndpointChannel, CancelPendingRequest) {
    RecordingChannel channel;
    channel.on_new_connection();
    TransferMaker maker;

    channel.on_in_request(0x81, 60, 8, maker.make());
    channel.on_in_request(0x82, 61, 8, maker.make());

    EXPECT_TRUE(channel.cancel_pending(60));
    EXPECT_FALSE(channel.cancel_pending(60)); // 已取消
    EXPECT_FALSE(channel.cancel_pending(999));

    // 剩一个请求仍可被应答
    channel.push({0x01});
    ASSERT_EQ(channel.sent.size(), 1u);
    EXPECT_EQ(channel.sent[0].seqnum, 61u);
}

TEST(TestInEndpointChannel, DisconnectionClearsEverything) {
    RecordingChannel channel;
    channel.on_new_connection();
    TransferMaker maker;

    channel.on_in_request(0x81, 70, 8, maker.make());
    channel.push({0x01});
    ASSERT_EQ(channel.sent.size(), 1u);

    channel.on_disconnection();
    // 队列与缓冲清空
    EXPECT_FALSE(channel.cancel_pending(70));
    EXPECT_TRUE(channel.buffer.empty());
    // 断连后 push 不再匹配（无挂起请求，数据入缓冲但不发送）
    channel.push({0x02});
    EXPECT_EQ(channel.sent.size(), 1u);

    // 重新连接：缓冲清空，恢复可用（请求挂起后可正常应答）
    channel.on_new_connection();
    EXPECT_TRUE(channel.buffer.empty());
    channel.on_in_request(0x81, 71, 8, maker.make());
    channel.push({0x03});
    ASSERT_EQ(channel.sent.size(), 2u);
    EXPECT_EQ(channel.sent[1].seqnum, 71u);
    EXPECT_EQ(channel.sent[1].data, (data_type{0x03}));
}

// ===== 并发场景：请求侧（receiver 线程）与数据侧（业务线程）竞争 =====

TEST(TestInEndpointChannel, ConcurrentPushAndRequestsNoLostRequests) {
    constexpr int kRequests = 500;

    // maker 必须先于 channel 声明（后析构）：队列里可能残留挂起的 transfer，
    // channel 析构时还要用它释放；op 无状态且 delete 线程安全，可跨线程共享
    TransferMaker maker;
    RecordingChannel channel;
    channel.on_new_connection();

    // 请求线程：连续发 IN 请求（模拟 session receiver 线程）
    std::thread requester([&]() {
        for (int i = 0; i < kRequests; i++) {
            channel.on_in_request(0x81, static_cast<std::uint32_t>(i), 8, maker.make());
        }
    });
    // 数据线程：连续推入消息（模拟业务线程）
    std::thread pusher([&]() {
        for (int i = 0; i < kRequests; i++) {
            channel.push(data_type{static_cast<std::uint8_t>(i)});
        }
    });
    requester.join();
    pusher.join();

    // 线程退出先后不定：请求线程可能晚于数据线程结束，最后一批请求可能挂起
    // 等数据。为挂起的请求补数据，验证「数据持续供给时请求不丢、不重复」
    std::vector<int> counts(kRequests, 0);
    for (auto &s: channel.sent) {
        ASSERT_LT(s.seqnum, kRequests);
        counts[s.seqnum]++;
    }
    for (int i = 0; i < kRequests; i++) {
        if (counts[i] == 0) {
            channel.push(data_type{static_cast<std::uint8_t>(i)});
            counts[i] = 1;
        }
    }

    // 每个请求恰好应答一次（组件不得丢请求、不得重复应答）
    ASSERT_EQ(channel.sent.size(), kRequests);
    for (int i = 0; i < kRequests; i++) {
        EXPECT_EQ(counts[i], 1) << "seqnum " << i;
    }
}

TEST(TestInEndpointChannel, ConcurrentByteStreamWriteAndRequests) {
    constexpr int kRequests = 200;
    constexpr std::size_t kBytesPerWrite = 10;

    // maker 必须先于 channel 声明（后析构），理由同 ConcurrentPushAndRequestsNoLostRequests
    TransferMaker maker;
    ShardChannel channel;
    channel.on_new_connection();

    std::thread requester([&]() {
        for (int i = 0; i < kRequests; i++) {
            channel.on_in_request(0x81, static_cast<std::uint32_t>(i), 64, maker.make());
        }
    });
    std::thread writer([&]() {
        for (int i = 0; i < kRequests; i++) {
            data_type payload(kBytesPerWrite, static_cast<std::uint8_t>(i));
            channel.write(payload.data(), payload.size());
        }
    });
    requester.join();
    writer.join();

    // 线程退出先后不定：尾部请求可能挂起等数据。补写数据后验证：
    // 请求不丢、字节流总量守恒（写入 2000 字节全部被消费，缓冲清空）
    std::vector<int> counts(kRequests, 0);
    std::size_t total_sent = 0;
    for (auto &s: channel.sent) {
        ASSERT_LT(s.seqnum, kRequests);
        counts[s.seqnum]++;
        total_sent += s.length;
    }
    // 补充写入后每个请求恰好应答一次
    std::size_t topped_up = 0;
    for (int i = 0; i < kRequests; i++) {
        if (counts[i] == 0) {
            data_type payload(kBytesPerWrite, static_cast<std::uint8_t>(i));
            channel.write(payload.data(), payload.size());
            counts[i] = 1;
            topped_up++;
        }
    }
    ASSERT_EQ(channel.sent.size(), kRequests);
    for (int i = 0; i < kRequests; i++) {
        EXPECT_EQ(counts[i], 1) << "seqnum " << i;
    }
    // 数据守恒：sent 总字节 + 缓冲剩余 == 总写入（原始 + 补写）。
    // 注意补写的数据可能部分滞留缓冲（补写时队列已空），不能断言缓冲清空
    std::size_t total_sent_after = 0;
    for (auto &s: channel.sent) {
        total_sent_after += s.length;
    }
    std::size_t total_written = kRequests * kBytesPerWrite + topped_up * kBytesPerWrite;
    EXPECT_EQ(total_sent_after + channel.buffer.size(), total_written);
}

TEST(TestInEndpointChannel, ConcurrentDisconnectAndPushNoCrash) {
    RecordingChannel channel;
    channel.on_new_connection();

    std::atomic<bool> running{true};
    std::thread pusher([&]() {
        while (running) {
            channel.push({0x01});
        }
    });
    std::thread disconnecter([&]() {
        for (int i = 0; i < 200; i++) {
            channel.on_disconnection();
            channel.on_new_connection();
        }
    });
    disconnecter.join();
    running = false;
    pusher.join();
    // 不崩溃即可；断连后请求不会被应答（数据只进缓冲）
    EXPECT_LE(channel.sent.size(), 1u);
}

TEST(TestInEndpointChannel, ConcurrentRequestsAndDisconnect) {
    // maker 必须先于 channel 声明（后析构），理由同 ConcurrentPushAndRequestsNoLostRequests
    TransferMaker maker;
    RecordingChannel channel;
    channel.on_new_connection();

    std::atomic<bool> running{true};
    std::thread requester([&]() {
        std::uint32_t i = 0;
        while (running) {
            channel.on_in_request(0x81, i++, 8, maker.make());
        }
    });
    std::thread disconnecter([&]() {
        for (int i = 0; i < 200; i++) {
            channel.on_disconnection();
            channel.on_new_connection();
        }
    });
    disconnecter.join();
    running = false;
    requester.join();
    // 不崩溃即可
}

// ===== 真实 ByteStreamInChannel 的阻塞写路径（write 两阶段：等空间 → 写 → 应答） =====
// 这些测试不触发 try_send_one（不产生应答），因此不需要真实 Session：
// "阻塞写被宿主 IN 请求取走唤醒" 依赖 session->submit_ret_submit，由端到端
// （Pipe → ByteStreamInChannel）测试覆盖。

TEST(TestInEndpointChannel, RealByteStreamBlockingWriteSucceedsWhenSpaceAvailable) {
    ByteStreamInChannel channel;
    channel.set_capacity(16);
    channel.on_new_connection();

    // 缓冲空：阻塞写直接填满不阻塞
    std::uint8_t payload[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    auto n = channel.write(payload, 8);
    EXPECT_EQ(n, 8u);
    EXPECT_EQ(channel.size(), 8u);
    EXPECT_EQ(channel.available(), 8u);
}

TEST(TestInEndpointChannel, RealByteStreamBlockingWriteUnblockedByDisconnect) {
    ByteStreamInChannel channel;
    channel.set_capacity(16);
    channel.on_new_connection();

    std::uint8_t payload[32] = {};
    // 先占 8 字节：write(32) 前 8 字节能进缓冲，剩余 24 阻塞等空间
    EXPECT_EQ(channel.write_nb(payload, 8), 8u);

    std::uint32_t written = 0xFFFFFFFF;
    std::thread writer([&]() {
        written = channel.write(payload, 32);
    });
    // 等写者进入等待（已写 8、剩 24 阻塞）
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    // 断连：space_cv 唤醒阻塞写者，返回已写入量（8），不再继续
    channel.on_disconnection();
    writer.join();
    EXPECT_EQ(written, 8u);
}

TEST(TestInEndpointChannel, RealByteStreamBlockingWriteTimesOut) {
    ByteStreamInChannel channel;
    channel.set_capacity(16);
    channel.on_new_connection();

    std::uint8_t payload[32] = {};
    // 占满 16 字节，无宿主取走：阻塞超时返回 0
    EXPECT_EQ(channel.write_nb(payload, 16), 16u);
    EXPECT_EQ(channel.available(), 0u);

    auto t0 = std::chrono::steady_clock::now();
    auto n = channel.write(payload, 32, 30);
    auto waited = std::chrono::steady_clock::now() - t0;
    EXPECT_EQ(n, 0u);
    // 确实阻塞了约 30ms（而非立即返回）
    EXPECT_GE(waited, std::chrono::milliseconds(20));
    EXPECT_EQ(channel.size(), 16u);  // 已占数据未被写者改动
}
