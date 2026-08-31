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

// 测试通道：记录发出的应答，模拟消息模式缓冲（整条消费）。
// push/push_blocking/push_locked 的实现与真实 MessageInChannel 逐行一致
// （成员同名 pending/max_pending_，仅 try_send_one/reply_empty 里"用 session
// 提交应答"改为记录到 sent）；debug_front 是测试专用观察窗口（真实无此 API）
class RecordingChannel : public InEndpointChannelBase<RecordingChannel> {
public:
    struct Sent {
        std::uint32_t seqnum;
        std::uint32_t length;
        data_type data;
    };

    std::vector<Sent> sent;
    std::function<data_type(std::uint32_t)> pull; // 可选 pull，模拟派生类 override
    int pull_calls = 0;

    // 与真实 MessageInChannel 同 API：set_max_pending / size（测试只走公开接口）
    void set_max_pending(std::size_t max_pending) {
        std::lock_guard lock(this->channel_mutex);
        max_pending_ = max_pending;
    }

    std::size_t size() const {
        std::lock_guard lock(this->channel_mutex);
        return pending.size();
    }

    /// 测试专用：查看缓冲最旧一条的内容（真实通道无此 API）
    data_type debug_front() const {
        std::lock_guard lock(this->channel_mutex);
        return pending.empty() ? data_type{} : pending.front();
    }

    // ===== 以下与真实 MessageInChannel 的实现逐行一致（仅成员名不需要改，
    // 本来就是同名）=====

    bool push(data_type data, bool drop_oldest = true) {
        std::lock(this->channel_mutex, this->requests_mutex);
        std::lock_guard lock1(this->channel_mutex, std::adopt_lock);
        std::lock_guard lock2(this->requests_mutex, std::adopt_lock);
        // 断连后不再接收：返回 false 告诉调用者本次未发送成功（数据会在下次
        // 连接被清空，白攒；对齐 write_nb 断连返回 0 的语义）
        if (this->disconnected) {
            return false;
        }
        // 满且调用者不丢旧：不入队，告诉调用者满了
        if (!drop_oldest && max_pending_ != 0 && pending.size() >= max_pending_) {
            return false;
        }
        push_locked(std::move(data));
        this->try_send_pending_locked();
        return true;
    }

    bool push_blocking(data_type data, std::uint32_t timeout_ms = 0) {
        std::unique_lock lock(this->channel_mutex);
        while (max_pending_ != 0 && pending.size() >= max_pending_) {
            if (this->disconnected) {
                return false;
            }
            if (timeout_ms == 0) {
                this->space_cv.wait(lock);
            }
            else if (this->space_cv.wait_for(lock, std::chrono::milliseconds(timeout_ms)) == std::cv_status::timeout) {
                break;
            }
        }
        if (this->disconnected) {
            return false;
        }
        if (max_pending_ != 0 && pending.size() >= max_pending_) {
            return false; // 等待超时仍满
        }
        // 空位就绪：补 requests 锁入队并推进挂起请求（与 push 同锁序）
        std::lock_guard lock2(this->requests_mutex);
        push_locked(std::move(data));
        this->try_send_pending_locked();
        return true;
    }

    void push_locked(data_type data) {
        // 缓冲超限丢最旧（保持最新消息语义）
        if (max_pending_ != 0 && pending.size() >= max_pending_) {
            pending.pop_front();
        }
        pending.emplace_back(std::move(data));
    }

    bool buffer_empty() const {
        return pending.empty();
    }

    void buffer_clear() {
        pending.clear();
    }

    void try_send_one(std::uint8_t ep, std::uint32_t seqnum, std::uint32_t length, TransferHandle transfer) {
        // 取最旧一条按长度截断；与真实一致的部分（真实此处 set_transfer_data +
        // session 提交应答，桩改为记录 sent）
        auto &front = pending.front();
        auto send_len = std::min(front.size(), static_cast<std::size_t>(length));
        sent.push_back({seqnum, static_cast<std::uint32_t>(send_len),
                        data_type(front.begin(), front.begin() + send_len)});
        pending.pop_front();
        this->space_cv.notify_one(); // 腾出空位，唤醒阻塞的 push_blocking
    }

    void reply_empty(std::uint32_t seqnum, TransferHandle /*transfer*/) {
        // 挤出的挂起请求：真实此处 session 提交空应答（0 字节），桩改为记录
        sent.push_back({seqnum, 0, {}});
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

private:
    std::deque<data_type> pending;
    std::size_t max_pending_ = 0;
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

    // 与真实 ByteStreamInChannel 同 API：size（缓冲中待发字节数）
    std::size_t size() const {
        std::lock_guard lock(this->channel_mutex);
        return buffer.size();
    }

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
        space_cv.notify_one(); // 腾出空位，唤醒阻塞的写者
    }

    void reply_empty(std::uint32_t seqnum, TransferHandle /*transfer*/) {
        // 挤出的挂起请求：记录空完成（0 字节），transfer 析构释放
        sent.push_back({seqnum, 0});
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

// 构造一个传输句柄（op 为进程级单例：队列中残留请求的 handle 可能在测试结束、
// channel 析构时才销毁，静态 op 保证它始终有效，不依赖局部变量生命周期）
struct TransferMaker {
    static inline GenericTransferOperator op;  // 无状态，多线程并发 free 安全
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
    EXPECT_EQ(channel.size(), 0u); // 整条消费，剩余丢弃（消息语义）
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
    channel.set_max_pending(2);
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
    EXPECT_EQ(channel.size(), 0u);
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
    EXPECT_EQ(channel.size(), 6u);

    // 后续请求消费剩余部分
    channel.on_in_request(0x81, 71, 8, maker.make());
    ASSERT_EQ(channel.sent.size(), 2u);
    EXPECT_EQ(channel.sent[1].length, 6u);
    EXPECT_EQ(channel.size(), 0u);
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
    EXPECT_EQ(channel.size(), 0u);
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
    EXPECT_EQ(channel.size(), 0u);
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
    EXPECT_NE(channel.size(), 0u); // 缓冲数据未被消费

    channel.on_in_request(0x81, 2, 64, maker.make());
    ASSERT_EQ(channel.sent.size(), 2u);
    EXPECT_EQ(channel.sent[1].length, 10u);
    EXPECT_EQ(channel.size(), 0u);
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
    EXPECT_EQ(channel.size(), 0u); // 消息被消费

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
    EXPECT_EQ(channel.size(), 0u);
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
    EXPECT_EQ(channel.size(), 0u);
    // 断连后 push 不再匹配（无挂起请求，数据入缓冲但不发送）
    channel.push({0x02});
    EXPECT_EQ(channel.sent.size(), 1u);

    // 重新连接：缓冲清空，恢复可用（请求挂起后可正常应答）
    channel.on_new_connection();
    EXPECT_EQ(channel.size(), 0u);
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
    EXPECT_EQ(total_sent_after + channel.size(), total_written);
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

TEST(TestInEndpointChannel, PendingLimitEvictsOldestWithEmptyReply) {
    RecordingChannel channel;
    channel.on_new_connection();
    TransferMaker maker;
    channel.set_max_pending_requests(2);

    // 3 个请求全部无数据挂起：第 3 个到达时第 1 个被挤出，应答空完成（0 字节）
    channel.on_in_request(0x81, 1, 8, maker.make());
    channel.on_in_request(0x81, 2, 8, maker.make());
    channel.on_in_request(0x81, 3, 8, maker.make());

    ASSERT_EQ(channel.sent.size(), 1u);
    EXPECT_EQ(channel.sent[0].seqnum, 1u);
    EXPECT_EQ(channel.sent[0].length, 0u);

    // 数据到达：应答剩余的请求 2、3（FIFO 顺序）
    channel.push({0xAA, 0xBB});
    channel.push({0xCC, 0xDD});
    ASSERT_EQ(channel.sent.size(), 3u);
    EXPECT_EQ(channel.sent[1].seqnum, 2u);
    EXPECT_EQ(channel.sent[2].seqnum, 3u);
    EXPECT_EQ(channel.sent[1].data, (data_type{0xAA, 0xBB}));
    EXPECT_EQ(channel.sent[2].data, (data_type{0xCC, 0xDD}));
}

TEST(TestInEndpointChannel, PushRejectsWhenFullIfNotDropping) {
    RecordingChannel channel;
    channel.on_new_connection();
    channel.set_max_pending(1);

    EXPECT_TRUE(channel.push({0x01}));          // 有空位，入队成功
    // 满了且要求不丢旧：返回 false，缓冲不变
    EXPECT_FALSE(channel.push({0x02}, false));
    ASSERT_EQ(channel.size(), 1u);
    EXPECT_EQ(channel.debug_front(), (data_type{0x01}));
    // 丢最旧模式：入队成功，旧消息被挤出
    EXPECT_TRUE(channel.push({0x03}));
    ASSERT_EQ(channel.size(), 1u);
    EXPECT_EQ(channel.debug_front(), (data_type{0x03}));
}

TEST(TestInEndpointChannel, PushBlockingWaitsForSpace) {
    RecordingChannel channel;
    channel.on_new_connection();
    TransferMaker maker;
    channel.set_max_pending(1);
    channel.push({0x01}); // 占满唯一空位

    bool pushed = false;
    std::thread writer([&]() { pushed = channel.push_blocking({0x02}); });

    // 等写者进入阻塞（缓冲仍满），主机请求取走消息腾出空位
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    channel.on_in_request(0x81, 9, 8, maker.make()); // 缓冲有数据 → 立即应答取走

    writer.join();
    EXPECT_TRUE(pushed);
    ASSERT_EQ(channel.size(), 1u);
    EXPECT_EQ(channel.debug_front(), (data_type{0x02}));
}

TEST(TestInEndpointChannel, PushBlockingInterruptedByDisconnection) {
    // 缓冲满时阻塞推入，断连必须打断等待并返回 false（space_cv 唤醒 + disconnected 检查）
    RecordingChannel channel;
    channel.on_new_connection();
    channel.set_max_pending(1);
    channel.push({0x01}); // 占满唯一空位

    bool pushed = true;
    std::thread writer([&]() { pushed = channel.push_blocking({0x02}); });

    std::this_thread::sleep_for(std::chrono::milliseconds(50)); // 确保已阻塞
    channel.on_disconnection();
    writer.join();
    EXPECT_FALSE(pushed);
}

TEST(TestInEndpointChannel, ShardPendingLimitEvictsOldestWithEmptyReply) {
    ShardChannel channel;
    channel.on_new_connection();
    TransferMaker maker;
    channel.set_max_pending_requests(2);

    channel.on_in_request(0x81, 1, 8, maker.make());
    channel.on_in_request(0x81, 2, 8, maker.make());
    channel.on_in_request(0x81, 3, 8, maker.make());

    ASSERT_EQ(channel.sent.size(), 1u);
    EXPECT_EQ(channel.sent[0].seqnum, 1u);
    EXPECT_EQ(channel.sent[0].length, 0u);

    const data_type bytes = {1, 2, 3, 4};
    channel.write(bytes.data(), bytes.size());
    ASSERT_EQ(channel.sent.size(), 2u);
    EXPECT_EQ(channel.sent[1].seqnum, 2u);
    EXPECT_EQ(channel.sent[1].length, 4u);
}

TEST(TestInEndpointChannel, ConcurrentPushAndRequestsMatchAll) {
    // 多线程同时推数据 + 多线程同时发请求：每个请求最终都被应答，应答数据
    // 与对应 push 的消息一致（双锁保证无丢失无错配，TSan 下验证无竞态）
    RecordingChannel channel;
    channel.on_new_connection();
    TransferMaker maker;

    constexpr int PUSHERS = 4;
    constexpr int REQUESTERS = 4;
    constexpr int PER_THREAD = 20;

    // 先挂 16 个请求（每个 64 字节）：设备侧 16 条消息逐条应答
    std::atomic<int> next_seq{1};
    std::vector<std::thread> requesters;
    for (int t = 0; t < REQUESTERS; t++) {
        requesters.emplace_back([&]() {
            for (int i = 0; i < PER_THREAD; i++) {
                channel.on_in_request(0x81, static_cast<std::uint32_t>(next_seq.fetch_add(1)), 64, maker.make());
            }
        });
    }
    // 同时 16 条消息逐个推入（每条内容带序号标签）；t 必须按值捕获
    //（[&] 引用捕获会与主线程循环递增竞态，TSan 实测命中；i 是 lambda 内局部）
    std::vector<std::thread> pushers;
    for (int t = 0; t < PUSHERS; t++) {
        pushers.emplace_back([&, t]() {
            for (int i = 0; i < PER_THREAD; i++) {
                channel.push(data_type{static_cast<std::uint8_t>(t), static_cast<std::uint8_t>(i)});
            }
        });
    }
    for (auto &th: requesters) {
        th.join();
    }
    for (auto &th: pushers) {
        th.join();
    }

    // 80 条消息全部被应答（每请求一条；消息按请求长度截断，长度 ≤ 2 字节）
    ASSERT_EQ(channel.sent.size(), REQUESTERS * PER_THREAD);
    // 每条应答的 seqnum 覆盖 1..80（无丢失无重复）
    std::vector<int> seqs;
    for (auto &s: channel.sent) {
        seqs.push_back(static_cast<int>(s.seqnum));
    }
    std::sort(seqs.begin(), seqs.end());
    for (int i = 0; i < REQUESTERS * PER_THREAD; i++) {
        EXPECT_EQ(seqs[static_cast<std::size_t>(i)], i + 1);
    }
    EXPECT_EQ(channel.size(), 0u); // 全部消费完，无残留
}

TEST(TestInEndpointChannel, ConcurrentPushBlockingWriters) {
    // 上限 1：4 个写者并发阻塞推入，宿主逐个取走腾空位；全部入队成功且顺序
    // 与应答顺序一致（TSan 下验证等待/唤醒无竞态）
    RecordingChannel channel;
    channel.on_new_connection();
    TransferMaker maker;
    channel.set_max_pending(1);

    constexpr int WRITERS = 4;
    std::atomic<bool> ok{true};
    std::vector<std::thread> writers;
    for (int t = 0; t < WRITERS; t++) {
        writers.emplace_back([&, t]() {
            if (!channel.push_blocking(data_type{static_cast<std::uint8_t>(t)})) {
                ok.store(false);
            }
        });
    }
    // 宿主逐个取走（4 条消息需 4 个请求）
    for (int i = 0; i < WRITERS; i++) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        channel.on_in_request(0x81, static_cast<std::uint32_t>(i + 1), 8, maker.make());
    }
    for (auto &th: writers) {
        th.join();
    }

    EXPECT_TRUE(ok.load());
    ASSERT_EQ(channel.sent.size(), WRITERS);
    EXPECT_EQ(channel.size(), 0u);
}

TEST(TestInEndpointChannel, ConcurrentPendingLimitKeepsQueueBounded) {
    // 上限 2：多线程并发发请求，队列始终 ≤2，被挤出的请求全部以空完成（0 字节）
    // 应答（TSan 下验证挤最旧与入队的并发安全）
    RecordingChannel channel;
    channel.on_new_connection();
    TransferMaker maker;
    channel.set_max_pending_requests(2);

    constexpr int THREADS = 4;
    constexpr int PER_THREAD = 25;
    std::atomic<int> next_seq{1};
    std::vector<std::thread> requesters;
    for (int t = 0; t < THREADS; t++) {
        requesters.emplace_back([&]() {
            for (int i = 0; i < PER_THREAD; i++) {
                channel.on_in_request(0x81, static_cast<std::uint32_t>(next_seq.fetch_add(1)), 8, maker.make());
            }
        });
    }
    for (auto &th: requesters) {
        th.join();
    }

    // 100 个请求：98 个被挤出（答 0 长度），2 个仍在挂起
    ASSERT_EQ(channel.sent.size(), THREADS * PER_THREAD - 2);
    for (auto &s: channel.sent) {
        EXPECT_EQ(s.length, 0u);
    }
    // 挂起队列恰为最后两个 seqnum
    channel.push({0x01, 0x02});
    channel.push({0x03, 0x04});
    ASSERT_EQ(channel.sent.size(), THREADS * PER_THREAD);
    EXPECT_EQ(channel.sent[THREADS * PER_THREAD - 2].length, 2u);
    EXPECT_EQ(channel.sent[THREADS * PER_THREAD - 1].length, 2u);
}

TEST(TestInEndpointChannel, ConcurrentPushAndDisconnection) {
    // push 与断连并发：push 要么在断连前入队成功（随后被断连清空），要么
    // 断连检查返回 false——不残留、不崩（TSan 验证）
    RecordingChannel channel;
    channel.on_new_connection();

    std::atomic<bool> started{false};
    std::atomic<bool> stop{false};
    std::thread pusher([&]() {
        int ok = 0;
        for (int i = 0; i < 500; i++) {
            if (channel.push(data_type{static_cast<std::uint8_t>(i)})) {
                ok++;
                if (!started.load()) {
                    started.store(true);
                }
            }
        }
        stop.store(true);
    });
    // 等 pusher 开始入队后断连（与 push 并发竞争）
    while (!started.load() && !stop.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    channel.on_disconnection();
    pusher.join();

    // 断连后无残留（断连前入队的被清空，断连后的 push 返回 false 不入队）
    EXPECT_EQ(channel.size(), 0u);
}

TEST(TestInEndpointChannel, ConcurrentUnlinkAndRequests) {
    // cancel_pending（主机 UNLINK）与 on_in_request 并发：被取消的请求从队列
    // 移除（不再应答），未被取消的继续挂起等数据——不重复应答、不崩（TSan 验证）
    RecordingChannel channel;
    channel.on_new_connection();
    TransferMaker maker;

    std::atomic<int> next_seq{1};
    std::atomic<bool> stop{false};
    std::thread requester([&]() {
        while (!stop.load()) {
            channel.on_in_request(0x81, static_cast<std::uint32_t>(next_seq.fetch_add(1)), 8, maker.make());
        }
    });
    int cancelled = 0;
    for (int i = 0; i < 200; i++) {
        if (channel.cancel_pending(static_cast<std::uint32_t>(i + 1))) {
            cancelled++;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    stop.store(true);
    requester.join();

    // 取消的请求数不超过已发出的请求数；数据到达后剩余请求被应答
    EXPECT_LE(cancelled, next_seq.load() - 1);
    channel.push({0x01});
    EXPECT_FALSE(channel.sent.empty());
    // 每个应答 seqnum 唯一（无重复应答）
    std::vector<int> seqs;
    for (auto &s: channel.sent) {
        seqs.push_back(static_cast<int>(s.seqnum));
    }
    std::sort(seqs.begin(), seqs.end());
    EXPECT_EQ(std::adjacent_find(seqs.begin(), seqs.end()), seqs.end());
}

TEST(TestInEndpointChannel, ConcurrentRequestAndDisconnection) {
    // 请求入队与断连并发：断连清队列时不丢/不崩，请求后到的不残留
    // （requests_mutex 保护，TSan 下验证无竞态）
    RecordingChannel channel;
    channel.on_new_connection();
    TransferMaker maker;

    std::atomic<bool> stop{false};
    std::thread requester([&]() {
        std::uint32_t seq = 1;
        while (!stop.load()) {
            channel.on_in_request(0x81, seq++, 8, maker.make());
        }
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    channel.on_disconnection();
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    channel.on_disconnection();
    stop.store(true);
    requester.join();

    // 断连后队列与缓冲都被清空；再 push 返回 false（断连后不接收），无残留
    EXPECT_FALSE(channel.push({0x01}));
    EXPECT_TRUE(channel.sent.empty());
    EXPECT_EQ(channel.size(), 0u);
}
