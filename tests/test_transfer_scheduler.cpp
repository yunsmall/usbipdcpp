#include <gtest/gtest.h>

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

#include "usbipdcpp/Server.h"
#include "usbipdcpp/Session.h"
#include "usbipdcpp/constant.h"
#include "usbipdcpp/virtual_device/TransferScheduler.h"

using namespace usbipdcpp;
using namespace std::chrono_literals;

namespace {

/// 收集完成响应的调度器测试子类：override on_urb_completed 拦截响应并记录
/// 完成时刻，供测试断言延迟/节奏/内容
class TestTransferScheduler : public TransferScheduler {
public:
    explicit TestTransferScheduler(UsbSpeed speed) : TransferScheduler(speed) {}

    /// 等待收集到至少 n 个响应，超时返回 false
    bool wait_for_response_count(std::size_t n, std::chrono::milliseconds timeout) {
        std::unique_lock lock(mutex);
        return cv.wait_for(lock, timeout, [&] { return responses.size() >= n; });
    }

    std::size_t response_count() {
        std::lock_guard lock(mutex);
        return responses.size();
    }

    /// 取走全部响应（清空，与 completion_times 一一对应）
    std::vector<UsbIpResponse::UsbIpRetSubmit> take_responses() {
        std::lock_guard lock(mutex);
        return std::move(responses);
    }

    /// 各响应的完成时刻（与响应一一对应）
    std::vector<std::chrono::steady_clock::time_point> completion_times() {
        std::lock_guard lock(mutex);
        return times;
    }

protected:
    void on_urb_completed(Session &, UsbIpResponse::UsbIpRetSubmit &&submit) override {
        {
            std::lock_guard lock(mutex);
            responses.push_back(std::move(submit));
            times.push_back(std::chrono::steady_clock::now());
        }
        cv.notify_all();
    }

private:
    std::mutex mutex;
    std::condition_variable cv;
    std::vector<UsbIpResponse::UsbIpRetSubmit> responses;
    std::vector<std::chrono::steady_clock::time_point> times;
};

/// 测试骨架：Server + Session 仅作为 start 的活引用（响应被子类拦截，不触网）。
/// 析构顺序（后声明先析构）：scheduler（内部线程先停）→ session → server
struct TestFixture {
    Server server;
    Session session{server, 1};
    TestTransferScheduler scheduler{UsbSpeed::High};

    void start() {
        scheduler.start(session);
    }
};

/// 等时提交的便捷封装（响应为 OK + 10 字节）
void submit_iso(TestTransferScheduler &scheduler, std::uint8_t ep, std::chrono::microseconds interval,
                int num_packets, std::uint32_t seqnum) {
    scheduler.submit(ep, EndpointAttributes::Isochronous, interval, num_packets,
                     UsbIpResponse::UsbIpRetSubmit::create_ret_submit_ok_without_data(seqnum, 10));
}

} // namespace

// ==================== 端点间隔换算（纯函数） ====================

TEST(TransferSchedulerTest, EndpointIntervalHighSpeed) {
    // 高速：2^(bInterval-1) × 125µs
    EXPECT_EQ(TransferScheduler::endpoint_interval(UsbEndpoint{.address = 0x81, .interval = 1}, UsbSpeed::High), 125us);
    EXPECT_EQ(TransferScheduler::endpoint_interval(UsbEndpoint{.address = 0x81, .interval = 2}, UsbSpeed::High), 250us);
    EXPECT_EQ(TransferScheduler::endpoint_interval(UsbEndpoint{.address = 0x81, .interval = 3}, UsbSpeed::High), 500us);
    EXPECT_EQ(TransferScheduler::endpoint_interval(UsbEndpoint{.address = 0x81, .interval = 4}, UsbSpeed::High), 1000us);
    EXPECT_EQ(TransferScheduler::endpoint_interval(UsbEndpoint{.address = 0x81, .interval = 5}, UsbSpeed::High), 2000us);
}

TEST(TransferSchedulerTest, EndpointIntervalOtherSpeeds) {
    // 全速/低速：bInterval × 1ms
    EXPECT_EQ(TransferScheduler::endpoint_interval(UsbEndpoint{.address = 0x81, .interval = 4}, UsbSpeed::Full), 4ms);
    // Super：bInterval × 125µs
    EXPECT_EQ(TransferScheduler::endpoint_interval(UsbEndpoint{.address = 0x81, .interval = 4}, UsbSpeed::Super), 500us);
    // bInterval=0 防御：按 1 处理
    EXPECT_EQ(TransferScheduler::endpoint_interval(UsbEndpoint{.address = 0x81, .interval = 0}, UsbSpeed::High), 125us);
}

// ==================== 延迟语义 ====================

TEST(TransferSchedulerTest, DelayedResponse) {
    // 1 个包 × 20ms：响应至少等满一个间隔（定时器粒度粗的平台只验下限）
    TestFixture fx;
    fx.start();
    auto t0 = std::chrono::steady_clock::now();
    submit_iso(fx.scheduler, 0x81, 20ms, 1, 7);
    ASSERT_TRUE(fx.scheduler.wait_for_response_count(1, 2s));
    auto elapsed = std::chrono::steady_clock::now() - t0;
    auto responses = fx.scheduler.take_responses();
    ASSERT_EQ(responses.size(), 1);
    EXPECT_EQ(responses[0].header.seqnum, 7);
    EXPECT_EQ(responses[0].actual_length, 10);
    EXPECT_GE(elapsed, 15ms);
    fx.scheduler.stop();
}

TEST(TransferSchedulerTest, MultiplePacketsExtendDelay) {
    // 5 个包 × 10ms：延迟 50ms（多个包分布在多个间隔）
    TestFixture fx;
    fx.start();
    auto t0 = std::chrono::steady_clock::now();
    submit_iso(fx.scheduler, 0x81, 10ms, 5, 1);
    ASSERT_TRUE(fx.scheduler.wait_for_response_count(1, 2s));
    EXPECT_GE(std::chrono::steady_clock::now() - t0, 40ms);
    fx.scheduler.stop();
}

TEST(TransferSchedulerTest, ZeroPacketsRespondImmediately) {
    // 无等时包：不占调度窗口，立即响应
    TestFixture fx;
    fx.start();
    auto t0 = std::chrono::steady_clock::now();
    submit_iso(fx.scheduler, 0x81, 20ms, 0, 3);
    ASSERT_TRUE(fx.scheduler.wait_for_response_count(1, 1s));
    EXPECT_LE(std::chrono::steady_clock::now() - t0, 200ms);
    EXPECT_EQ(fx.scheduler.take_responses()[0].header.seqnum, 3);
    fx.scheduler.stop();
}

TEST(TransferSchedulerTest, NegativePacketsRespondImmediately) {
    TestFixture fx;
    fx.start();
    submit_iso(fx.scheduler, 0x81, 20ms, -1, 4);
    ASSERT_TRUE(fx.scheduler.wait_for_response_count(1, 1s));
    EXPECT_EQ(fx.scheduler.take_responses()[0].header.seqnum, 4);
    fx.scheduler.stop();
}

// ==================== 同一端点串行 ====================

TEST(TransferSchedulerTest, SerialCompletionSameEndpoint) {
    // 同端点 2 个 URB 各 1 包：后一个完成不早于提交时刻 + 一个间隔（只晚不早），
    // 且顺序保持。不断言相邻间隔：调度延迟会让相邻 URB 的完成时刻靠拢
    // （交付只可能晚于帧边界，不可能提前），间隔下界断言在该场景下不成立
    TestFixture fx;
    fx.start();
    submit_iso(fx.scheduler, 0x81, 60ms, 1, 1);
    auto submit2_at = std::chrono::steady_clock::now();
    submit_iso(fx.scheduler, 0x81, 60ms, 1, 2);
    ASSERT_TRUE(fx.scheduler.wait_for_response_count(2, 3s));
    auto times = fx.scheduler.completion_times();
    ASSERT_EQ(times.size(), 2);
    EXPECT_GE(times[1] - submit2_at, 55ms) << "第二个 URB 提前完成";
    EXPECT_GT(times[1], times[0]);
    auto responses = fx.scheduler.take_responses();
    EXPECT_EQ(responses[0].header.seqnum, 1);
    EXPECT_EQ(responses[1].header.seqnum, 2);
    fx.scheduler.stop();
}

TEST(TransferSchedulerTest, PacingAcrossManyUrbs) {
    // 同端点 4 个 URB 连续提交：每个至少等满一个间隔（提交时刻 + 40ms）
    // 才完成，且完成时刻严格递增。不断言相邻间隔，原因见 SerialCompletionSameEndpoint
    TestFixture fx;
    fx.start();
    std::vector<std::chrono::steady_clock::time_point> submitted_at;
    for (std::uint32_t i = 1; i <= 4; ++i) {
        submitted_at.push_back(std::chrono::steady_clock::now());
        submit_iso(fx.scheduler, 0x81, 40ms, 1, i);
    }
    ASSERT_TRUE(fx.scheduler.wait_for_response_count(4, 4s));
    auto times = fx.scheduler.completion_times();
    ASSERT_EQ(times.size(), 4);
    for (std::size_t i = 0; i < times.size(); ++i) {
        // 定时器只可能晚触发，不可能提前（容差 5ms 与 DelayedResponse 风格一致）
        EXPECT_GE(times[i] - submitted_at[i], 35ms) << "URB " << i + 1 << " 提前完成";
    }
    for (std::size_t i = 1; i < times.size(); ++i) {
        EXPECT_GT(times[i], times[i - 1]) << "URB " << i << " 应晚于前一 URB";
    }
    fx.scheduler.stop();
}

// ==================== 不同端点独立 ====================

TEST(TransferSchedulerTest, DifferentEndpointsRunIndependently) {
    // 端 A 每 40ms、端 B 每 80ms：各自节奏互不拖累
    TestFixture fx;
    fx.start();
    submit_iso(fx.scheduler, 0x81, 40ms, 1, 1);
    submit_iso(fx.scheduler, 0x82, 80ms, 1, 2);
    ASSERT_TRUE(fx.scheduler.wait_for_response_count(2, 3s));
    auto responses = fx.scheduler.take_responses();
    ASSERT_EQ(responses.size(), 2);
    // 先完成的应是更快的 A（seqnum 1）
    EXPECT_EQ(responses[0].header.seqnum, 1);
    EXPECT_EQ(responses[1].header.seqnum, 2);
    fx.scheduler.stop();
}

// ==================== 取消（UNLINK 语义） ====================

TEST(TransferSchedulerTest, CancelPendingUrb) {
    // 取消成功：返回 true，且不再发出 RET_SUBMIT
    TestFixture fx;
    fx.start();
    submit_iso(fx.scheduler, 0x81, 20ms, 1, 9);
    EXPECT_TRUE(fx.scheduler.cancel(9));
    // 等待超过原 deadline，确认无响应
    EXPECT_FALSE(fx.scheduler.wait_for_response_count(1, 200ms));
    EXPECT_EQ(fx.scheduler.response_count(), 0);
    fx.scheduler.stop();
}

TEST(TransferSchedulerTest, CancelMissingUrbReturnsFalse) {
    TestFixture fx;
    fx.start();
    EXPECT_FALSE(fx.scheduler.cancel(123));
    fx.scheduler.stop();
}

TEST(TransferSchedulerTest, CanceledUrbKeepsPacingOfFollowers) {
    // 被取消 URB 占用的总线时间不回收：后继 URB 仍在原 deadline 完成，不提前
    TestFixture fx;
    fx.start();
    auto t0 = std::chrono::steady_clock::now();
    submit_iso(fx.scheduler, 0x81, 60ms, 1, 1); // deadline ≈ t0+60
    submit_iso(fx.scheduler, 0x81, 60ms, 1, 2); // deadline ≈ t0+120（串行）
    ASSERT_TRUE(fx.scheduler.cancel(1));
    ASSERT_TRUE(fx.scheduler.wait_for_response_count(1, 2s));
    EXPECT_EQ(fx.scheduler.take_responses()[0].header.seqnum, 2);
    EXPECT_GE(std::chrono::steady_clock::now() - t0, 90ms);
    fx.scheduler.stop();
}

// ==================== 连接生命周期 ====================

TEST(TransferSchedulerTest, StopDropsPendingUrbs) {
    // 断连：未完成 URB 丢弃（不响应）；stop 后提交的 URB 也丢弃
    TestFixture fx;
    fx.start();
    submit_iso(fx.scheduler, 0x81, 20ms, 1, 1);
    fx.scheduler.stop();
    EXPECT_EQ(fx.scheduler.response_count(), 0);
    submit_iso(fx.scheduler, 0x81, 20ms, 1, 2);
    EXPECT_EQ(fx.scheduler.response_count(), 0);
}

TEST(TransferSchedulerTest, RestartAfterStopWorks) {
    // 重连：stop 后 start 再次可用，状态干净
    TestFixture fx;
    fx.start();
    submit_iso(fx.scheduler, 0x81, 20ms, 1, 1);
    ASSERT_TRUE(fx.scheduler.wait_for_response_count(1, 2s));
    fx.scheduler.take_responses(); // 清空第一次连接的响应
    fx.scheduler.stop();

    fx.start();
    submit_iso(fx.scheduler, 0x81, 20ms, 1, 2);
    ASSERT_TRUE(fx.scheduler.wait_for_response_count(1, 2s));
    EXPECT_EQ(fx.scheduler.take_responses()[0].header.seqnum, 2);
    fx.scheduler.stop();
}

TEST(TransferSchedulerTest, StopIsIdempotent) {
    TestFixture fx;
    fx.start();
    fx.scheduler.stop();
    fx.scheduler.stop(); // 二次 stop 不应崩溃
}

// ==================== 规模与并发 ====================

TEST(TransferSchedulerTest, ManyUrbsAllRespondInOrder) {
    // 同端点连续 30 个 URB：全部响应且按提交顺序（seqnum 递增）
    TestFixture fx;
    fx.start();
    constexpr int kCount = 30;
    for (int i = 1; i <= kCount; ++i)
        submit_iso(fx.scheduler, 0x81, 5ms, 1, static_cast<std::uint32_t>(i));
    ASSERT_TRUE(fx.scheduler.wait_for_response_count(kCount, 5s));
    auto responses = fx.scheduler.take_responses();
    ASSERT_EQ(responses.size(), kCount);
    for (int i = 0; i < kCount; ++i)
        EXPECT_EQ(responses[i].header.seqnum, static_cast<std::uint32_t>(i + 1));
    fx.scheduler.stop();
}

TEST(TransferSchedulerTest, ConcurrentSubmitFromMultipleThreads) {
    // 多线程并发提交（模拟 handler 线程与调度线程并发）：全部响应无丢失
    TestFixture fx;
    fx.start();
    constexpr int kThreads = 2;
    constexpr int kPerThread = 5;
    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&, t] {
            for (int i = 0; i < kPerThread; ++i) {
                // 各线程用不同端点，避免测试时长叠加
                submit_iso(fx.scheduler, static_cast<std::uint8_t>(0x81 + t), 30ms, 1,
                           static_cast<std::uint32_t>(t * kPerThread + i + 1));
            }
        });
    }
    for (auto &th: threads)
        th.join();
    ASSERT_TRUE(fx.scheduler.wait_for_response_count(kThreads * kPerThread, 5s));
    EXPECT_EQ(fx.scheduler.response_count(), kThreads * kPerThread);
    fx.scheduler.stop();
}
