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
        scheduler.start();
    }
};

/// 等时提交的便捷封装（响应为 OK + 10 字节）
void submit_iso(TestTransferScheduler &scheduler, Session &session, std::uint8_t ep,
                std::chrono::microseconds interval, int num_packets, std::uint32_t seqnum) {
    scheduler.submit(session, ep, EndpointAttributes::Isochronous, interval, num_packets,
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
    submit_iso(fx.scheduler, fx.session,0x81, 20ms, 1, 7);
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
    submit_iso(fx.scheduler, fx.session,0x81, 10ms, 5, 1);
    ASSERT_TRUE(fx.scheduler.wait_for_response_count(1, 2s));
    EXPECT_GE(std::chrono::steady_clock::now() - t0, 40ms);
    fx.scheduler.stop();
}

TEST(TransferSchedulerTest, DataDurationOverridesInterval) {
    // 显式数据时长优先于 包数×间隔：3 包 × 10ms = 30ms，数据时长 60ms → 按 60ms 延迟
    // （模拟自适应 OUT 跟随主机数据量：响应间隔 = 数据对应的音频时长）
    TestFixture fx;
    fx.start();
    auto t0 = std::chrono::steady_clock::now();
    fx.scheduler.submit(fx.session, 0x81, EndpointAttributes::Isochronous, 10ms, 3,
                        UsbIpResponse::UsbIpRetSubmit::create_ret_submit_ok_without_data(9, 10), 60ms);
    ASSERT_TRUE(fx.scheduler.wait_for_response_count(1, 3s));
    EXPECT_GE(std::chrono::steady_clock::now() - t0, 50ms);
    fx.scheduler.stop();
}

TEST(TransferSchedulerTest, DataDurationShorterThanIntervalProduct) {
    // 数据时长比 包数×间隔 短时同样按数据时长：2 包 × 20ms = 40ms，数据时长 10ms → 约 10ms
    TestFixture fx;
    fx.start();
    auto t0 = std::chrono::steady_clock::now();
    fx.scheduler.submit(fx.session, 0x81, EndpointAttributes::Isochronous, 20ms, 2,
                        UsbIpResponse::UsbIpRetSubmit::create_ret_submit_ok_without_data(11, 10), 10ms);
    ASSERT_TRUE(fx.scheduler.wait_for_response_count(1, 2s));
    auto elapsed = std::chrono::steady_clock::now() - t0;
    EXPECT_GE(elapsed, 8ms);
    EXPECT_LE(elapsed, 150ms); // 上限只需排除按 40ms 延迟（定时器慢的机器也远够）
    fx.scheduler.stop();
}

TEST(TransferSchedulerTest, NegativeDataDurationRespondsImmediately) {
    // 负数据时长 = 提前响应（水位闭环修正主机提交开销用）：deadline 不早于
    // 当前时刻，到点即完成，不等待包数×间隔
    TestFixture fx;
    fx.start();
    auto t0 = std::chrono::steady_clock::now();
    fx.scheduler.submit(fx.session, 0x81, EndpointAttributes::Isochronous, 20ms, 3,
                        UsbIpResponse::UsbIpRetSubmit::create_ret_submit_ok_without_data(12, 10), -500us);
    ASSERT_TRUE(fx.scheduler.wait_for_response_count(1, 1s));
    EXPECT_LE(std::chrono::steady_clock::now() - t0, 200ms);
    EXPECT_EQ(fx.scheduler.take_responses()[0].header.seqnum, 12);
    fx.scheduler.stop();
}

TEST(TransferSchedulerTest, BulkDelayedOneFrame) {
    // bulk 对齐 vudc 帧驱动语义：URB 对齐帧边界完成（不立即响应）。
    // 断言上限放宽到 100ms：Windows 定时器默认 ~15.6ms 粒度，亚毫秒
    // deadline 无法精确触发（见 TransferScheduler.cpp 帧对齐注释）
    TestFixture fx;
    fx.start();
    auto t0 = std::chrono::steady_clock::now();
    fx.scheduler.submit(fx.session, 0x01, EndpointAttributes::Bulk, 0us, 0,
                        UsbIpResponse::UsbIpRetSubmit::create_ret_submit_ok_without_data(13, 10));
    ASSERT_TRUE(fx.scheduler.wait_for_response_count(1, 1s));
    auto elapsed = std::chrono::steady_clock::now() - t0;
    EXPECT_GT(elapsed, 0us);
    EXPECT_LT(elapsed, 100ms);
    EXPECT_EQ(fx.scheduler.take_responses()[0].header.seqnum, 13);
    fx.scheduler.stop();
}

TEST(TransferSchedulerTest, InterruptDelayedOneFrame) {
    // interrupt 同 bulk：对齐帧边界完成（不立即响应），上限放宽同 bulk
    TestFixture fx;
    fx.start();
    auto t0 = std::chrono::steady_clock::now();
    fx.scheduler.submit(fx.session, 0x82, EndpointAttributes::Interrupt, 0us, 0,
                        UsbIpResponse::UsbIpRetSubmit::create_ret_submit_ok_without_data(14, 10));
    ASSERT_TRUE(fx.scheduler.wait_for_response_count(1, 1s));
    auto elapsed = std::chrono::steady_clock::now() - t0;
    EXPECT_GT(elapsed, 0us);
    EXPECT_LT(elapsed, 100ms);
    EXPECT_EQ(fx.scheduler.take_responses()[0].header.seqnum, 14);
    fx.scheduler.stop();
}

TEST(TransferSchedulerTest, BulkSerialCompletionSameEndpoint) {
    // 同端点 2 个 bulk URB：串行完成（同端点不重叠）。间隔断言放宽：
    // Windows 定时器粒度下可能同批到期（平均吞吐仍受限，见帧对齐注释）
    TestFixture fx;
    fx.start();
    fx.scheduler.submit(fx.session, 0x01, EndpointAttributes::Bulk, 0us, 0,
                        UsbIpResponse::UsbIpRetSubmit::create_ret_submit_ok_without_data(16, 10));
    fx.scheduler.submit(fx.session, 0x01, EndpointAttributes::Bulk, 0us, 0,
                        UsbIpResponse::UsbIpRetSubmit::create_ret_submit_ok_without_data(17, 10));
    ASSERT_TRUE(fx.scheduler.wait_for_response_count(2, 2s));
    auto responses = fx.scheduler.take_responses();
    ASSERT_EQ(responses.size(), 2);
    EXPECT_EQ(responses[0].header.seqnum, 16);
    EXPECT_EQ(responses[1].header.seqnum, 17);
    fx.scheduler.stop();
}

TEST(TransferSchedulerTest, ControlRespondsImmediately) {
    // 控制请求不走帧调度：立即响应（对齐 vudc 的 ep0 无延迟）
    TestFixture fx;
    fx.start();
    auto t0 = std::chrono::steady_clock::now();
    fx.scheduler.submit(fx.session, 0x00, EndpointAttributes::Control, 0us, 0,
                        UsbIpResponse::UsbIpRetSubmit::create_ret_submit_ok_without_data(15, 10));
    ASSERT_TRUE(fx.scheduler.wait_for_response_count(1, 1s));
    EXPECT_LE(std::chrono::steady_clock::now() - t0, 200ms);
    EXPECT_EQ(fx.scheduler.take_responses()[0].header.seqnum, 15);
    fx.scheduler.stop();
}

TEST(TransferSchedulerTest, ZeroPacketsRespondImmediately) {
    // 无等时包：不占调度窗口，立即响应
    TestFixture fx;
    fx.start();
    auto t0 = std::chrono::steady_clock::now();
    submit_iso(fx.scheduler, fx.session,0x81, 20ms, 0, 3);
    ASSERT_TRUE(fx.scheduler.wait_for_response_count(1, 1s));
    EXPECT_LE(std::chrono::steady_clock::now() - t0, 200ms);
    EXPECT_EQ(fx.scheduler.take_responses()[0].header.seqnum, 3);
    fx.scheduler.stop();
}

TEST(TransferSchedulerTest, NegativePacketsRespondImmediately) {
    TestFixture fx;
    fx.start();
    submit_iso(fx.scheduler, fx.session,0x81, 20ms, -1, 4);
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
    submit_iso(fx.scheduler, fx.session,0x81, 60ms, 1, 1);
    auto submit2_at = std::chrono::steady_clock::now();
    submit_iso(fx.scheduler, fx.session,0x81, 60ms, 1, 2);
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
        submit_iso(fx.scheduler, fx.session,0x81, 40ms, 1, i);
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
    submit_iso(fx.scheduler, fx.session,0x81, 40ms, 1, 1);
    submit_iso(fx.scheduler, fx.session,0x82, 80ms, 1, 2);
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
    submit_iso(fx.scheduler, fx.session,0x81, 20ms, 1, 9);
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
    submit_iso(fx.scheduler, fx.session,0x81, 60ms, 1, 1); // deadline ≈ t0+60
    submit_iso(fx.scheduler, fx.session,0x81, 60ms, 1, 2); // deadline ≈ t0+120（串行）
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
    submit_iso(fx.scheduler, fx.session,0x81, 20ms, 1, 1);
    fx.scheduler.stop();
    EXPECT_EQ(fx.scheduler.response_count(), 0);
    submit_iso(fx.scheduler, fx.session,0x81, 20ms, 1, 2);
    EXPECT_EQ(fx.scheduler.response_count(), 0);
}

TEST(TransferSchedulerTest, RestartAfterStopWorks) {
    // 重连：stop 后 start 再次可用，状态干净
    TestFixture fx;
    fx.start();
    submit_iso(fx.scheduler, fx.session,0x81, 20ms, 1, 1);
    ASSERT_TRUE(fx.scheduler.wait_for_response_count(1, 2s));
    fx.scheduler.take_responses(); // 清空第一次连接的响应
    fx.scheduler.stop();

    fx.start();
    submit_iso(fx.scheduler, fx.session,0x81, 20ms, 1, 2);
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
        submit_iso(fx.scheduler, fx.session,0x81, 5ms, 1, static_cast<std::uint32_t>(i));
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
                submit_iso(fx.scheduler, fx.session,static_cast<std::uint8_t>(0x81 + t), 30ms, 1,
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

// ==================== 处理器版（通知语义：自发送 + on_urb_done） ====================

TEST(TransferSchedulerTest, IsoProcessorCalledAfterDataDuration) {
    // iso 处理器版：数据时长（30ms）后调度线程通知处理器开始处理，处理器
    // 自行发送（测试里收集）+ on_urb_done 上报。限速靠"延迟服务时机"
    TestFixture fx;
    fx.start();
    std::mutex m;
    std::condition_variable cv;
    std::vector<UsbIpResponse::UsbIpRetSubmit> sent;
    auto t0 = std::chrono::steady_clock::now();
    fx.scheduler.submit(fx.session, UsbEndpoint{.address = 0x81, .interval = 4}, EndpointAttributes::Isochronous,
                        30ms, 21, {}, [&](Session &session, const UsbEndpoint &ep, std::uint32_t seqnum,
                                           TransferHandle &&) {
                            {
                                std::lock_guard lock(m);
                                sent.push_back(UsbIpResponse::UsbIpRetSubmit::create_ret_submit_ok_without_data(
                                        seqnum, 10));
                            }
                            // 处理完上报完成：推进串行点、服务下一个 URB
                            fx.scheduler.on_urb_done(ep.address, seqnum);
                        });
    std::unique_lock lock(m);
    ASSERT_TRUE(cv.wait_for(lock, 2s, [&] { return !sent.empty(); }));
    EXPECT_GE(std::chrono::steady_clock::now() - t0, 25ms);
    EXPECT_EQ(sent[0].header.seqnum, 21);
    fx.scheduler.stop();
}

TEST(TransferSchedulerTest, BulkProcessorCalledAtFrameBoundary) {
    // bulk 处理器版：对齐帧边界后通知处理器。上限放宽同 BulkDelayedOneFrame
    // （Windows 定时器粒度）
    TestFixture fx;
    fx.start();
    std::mutex m;
    std::condition_variable cv;
    bool notified = false;
    auto t0 = std::chrono::steady_clock::now();
    fx.scheduler.submit(fx.session, UsbEndpoint{.address = 0x01, .interval = 0}, EndpointAttributes::Bulk, 0us, 22,
                        {}, [&](Session &session, const UsbEndpoint &ep, std::uint32_t seqnum, TransferHandle &&) {
                            {
                                std::lock_guard lock(m);
                                notified = true;
                            }
                            fx.scheduler.on_urb_done(ep.address, seqnum);
                            cv.notify_all();
                        });
    std::unique_lock lock(m);
    ASSERT_TRUE(cv.wait_for(lock, 1s, [&] { return notified; }));
    EXPECT_LT(std::chrono::steady_clock::now() - t0, 100ms);
    fx.scheduler.stop();
}

TEST(TransferSchedulerTest, ProcessorUrbsSerializeOnCompletion) {
    // 同端点两个 iso 处理器版：服务时刻链在提交时定死（间隔 40ms），
    // 处理耗时/触发延迟不进入节奏；但处理慢（超过链间隔）时第二个 URB
    // 必须等第一个 on_urb_done 放行（串行保护）——处理器 1 sleep 60ms
    // 超过 40ms 链间隔，第二个的通知不早于第一个完成上报
    TestFixture fx;
    fx.start();
    std::mutex m;
    std::condition_variable cv;
    std::vector<std::chrono::steady_clock::time_point> notify_at;
    // 初始化为 max()：处理器 2 若在处理器 1 上报前被通知，断言恒失败（能测出来）
    auto first_done_at = std::chrono::steady_clock::time_point::max();
    fx.scheduler.submit(fx.session, UsbEndpoint{.address = 0x81, .interval = 4}, EndpointAttributes::Isochronous,
                        40ms, 23, {}, [&](Session &session, const UsbEndpoint &ep, std::uint32_t seqnum,
                                           TransferHandle &&) {
                            {
                                std::lock_guard lock(m);
                                notify_at.push_back(std::chrono::steady_clock::now());
                                cv.notify_all();
                            }
                            // 模拟慢处理：上报前卡 60ms（超过链间隔 40ms，
                            // 第二个必须等上报，不能按已过的链时刻提前）
                            std::this_thread::sleep_for(60ms);
                            first_done_at = std::chrono::steady_clock::now();
                            fx.scheduler.on_urb_done(ep.address, seqnum);
                        });
    fx.scheduler.submit(fx.session, UsbEndpoint{.address = 0x81, .interval = 4}, EndpointAttributes::Isochronous,
                        40ms, 24, {}, [&](Session &session, const UsbEndpoint &ep, std::uint32_t seqnum,
                                           TransferHandle &&) {
                            {
                                std::lock_guard lock(m);
                                notify_at.push_back(std::chrono::steady_clock::now());
                                cv.notify_all();
                            }
                            fx.scheduler.on_urb_done(ep.address, seqnum);
                        });
    std::unique_lock lock(m);
    ASSERT_TRUE(cv.wait_for(lock, 3s, [&] { return notify_at.size() >= 2; }));
    ASSERT_EQ(notify_at.size(), 2);
    // 串行保护：第二个的通知不早于第一个的完成上报（处理慢超链间隔时）
    EXPECT_GE(notify_at[1], first_done_at);
    EXPECT_GT(notify_at[1], notify_at[0]);
    fx.scheduler.stop();
}

TEST(TransferSchedulerTest, CancelPendingProcessorUrb) {
    // 处理器版 URB 带 seqnum：未服务前可取消（返回 true，处理器不被调用）；
    // 已通知处理器（处理中）的取消不了（返回 false）
    TestFixture fx;
    fx.start();
    bool notified = false;
    fx.scheduler.submit(fx.session, UsbEndpoint{.address = 0x81, .interval = 4}, EndpointAttributes::Isochronous,
                        30ms, 25, {}, [&](Session &, const UsbEndpoint &, std::uint32_t, TransferHandle &&) {
                            notified = true;
                        });
    EXPECT_TRUE(fx.scheduler.cancel(25));
    // 等待超过原服务时刻，确认处理器未被调用
    std::this_thread::sleep_for(200ms);
    EXPECT_FALSE(notified);

    // 已通知（处理中）的 URB 取消不了
    bool done = false;
    std::mutex m;
    std::condition_variable cv;
    fx.scheduler.submit(fx.session, UsbEndpoint{.address = 0x81, .interval = 4}, EndpointAttributes::Isochronous,
                        30ms, 26, {}, [&](Session &session, const UsbEndpoint &ep, std::uint32_t seqnum,
                                           TransferHandle &&) {
                            {
                                std::lock_guard lock(m);
                                done = true;
                            }
                            cv.notify_all();
                            fx.scheduler.on_urb_done(ep.address, seqnum);
                        });
    std::unique_lock lock(m);
    ASSERT_TRUE(cv.wait_for(lock, 2s, [&] { return done; }));
    EXPECT_FALSE(fx.scheduler.cancel(26));
    fx.scheduler.stop();
}

TEST(TransferSchedulerTest, OnUrbDoneWrongSeqnumIgnored) {
    // on_urb_done 的 seqnum 须匹配处理中的 URB：错误 seqnum 的上报被忽略，
    // 端点不推进（下一个 URB 不被服务）——处理器完成收尾后的误报不会
    // 打乱节奏
    TestFixture fx;
    fx.start();
    std::mutex m;
    std::condition_variable cv;
    std::vector<std::uint32_t> notified_seqnums;
    fx.scheduler.submit(fx.session, UsbEndpoint{.address = 0x81, .interval = 4}, EndpointAttributes::Isochronous,
                        20ms, 31, {}, [&](Session &session, const UsbEndpoint &ep, std::uint32_t seqnum,
                                           TransferHandle &&) {
                            {
                                std::lock_guard lock(m);
                                notified_seqnums.push_back(seqnum);
                            }
                            cv.notify_all();
                            if (seqnum == 31) {
                                // 先误报一个错误 seqnum（不应生效），再正确上报
                                fx.scheduler.on_urb_done(ep.address, 999);
                                fx.scheduler.on_urb_done(ep.address, seqnum);
                            } else {
                                fx.scheduler.on_urb_done(ep.address, seqnum);
                            }
                        });
    fx.scheduler.submit(fx.session, UsbEndpoint{.address = 0x81, .interval = 4}, EndpointAttributes::Isochronous,
                        20ms, 32, {}, [&](Session &session, const UsbEndpoint &ep, std::uint32_t seqnum,
                                           TransferHandle &&) {
                            {
                                std::lock_guard lock(m);
                                notified_seqnums.push_back(seqnum);
                            }
                            fx.scheduler.on_urb_done(ep.address, seqnum);
                            cv.notify_all();
                        });
    std::unique_lock lock(m);
    ASSERT_TRUE(cv.wait_for(lock, 3s, [&] { return notified_seqnums.size() >= 2; }));
    // 顺序保持：31 先 32 后（错误上报没让 32 提前或跳过）
    EXPECT_EQ(notified_seqnums[0], 31);
    EXPECT_EQ(notified_seqnums[1], 32);
    fx.scheduler.stop();
}

TEST(TransferSchedulerTest, OnUrbDoneFromOtherThreadConcurrentSubmit) {
    // 真异步处理：处理器在回调里把 on_urb_done 交给另一个线程延迟上报
    //（模拟慢设备异步完成），同时主线程持续并发 submit——全部完成、
    // 顺序保持、无死锁。detached 线程在 stop 前用计数确保全部结束
    TestFixture fx;
    fx.start();
    std::mutex m;
    std::condition_variable cv;
    std::vector<std::uint32_t> notified_seqnums;
    int pending_done_threads = 0;
    std::atomic<bool> keep_submitting{true};
    // 网络线程：持续提交（处理器回调从调度线程调 on_urb_done 不同线程）
    std::thread submitter([&] {
        std::uint32_t seq = 100;
        while (keep_submitting) {
            fx.scheduler.submit(fx.session, UsbEndpoint{.address = 0x81, .interval = 4},
                                EndpointAttributes::Isochronous, 10ms, seq, {},
                                [&](Session &session, const UsbEndpoint &ep, std::uint32_t seqnum,
                                    TransferHandle &&) {
                                    {
                                        std::lock_guard lock(m);
                                        notified_seqnums.push_back(seqnum);
                                        pending_done_threads++;
                                    }
                                    cv.notify_all();
                                    // 异步上报：新线程延迟 5ms 后调 on_urb_done。
                                    // join 等待线程结束（on_urb_done 确实从别的
                                    // 线程调用），避免 detached 线程生命周期
                                    // 竞态（主线程析构 cv 时线程还在 notify）
                                    std::thread t([&, ep, seqnum] {
                                        std::this_thread::sleep_for(5ms);
                                        fx.scheduler.on_urb_done(ep.address, seqnum);
                                        {
                                            std::lock_guard lock(m);
                                            pending_done_threads--;
                                        }
                                        cv.notify_all();
                                    });
                                    t.join();
                                });
            ++seq;
            std::this_thread::sleep_for(1ms);
        }
    });
    std::unique_lock lock(m);
    // 等 10 个 URB 全部被通知且所有异步上报线程结束
    ASSERT_TRUE(cv.wait_for(lock, 10s, [&] { return notified_seqnums.size() >= 10 && pending_done_threads == 0; }));
    keep_submitting = false;
    // 必须先放锁再 join：submitter 退出前可能又提交了 URB，其处理器
    //（调度线程）需要拿 m 锁——主线程持锁 join 会死锁（Windows 定时器
    // 粒度粗时最后的 URB 会在 stop 前被同批调度，Linux 1ms 粒度下时序
    // 不同不触发——时序依赖的死锁，测试里一律先放锁）
    lock.unlock();
    submitter.join();
    lock.lock();
    // 同端点串行通知（提交顺序），异步上报不破坏顺序
    for (std::size_t i = 1; i < notified_seqnums.size(); ++i)
        EXPECT_GT(notified_seqnums[i], notified_seqnums[i - 1]);
    lock.unlock();
    fx.scheduler.stop();
}

TEST(TransferSchedulerTest, ProcessingEndpointDoesNotBlockOthers) {
    // 端点独立：A（60ms）先提交并进入处理中，B（20ms）的 URB 照常按自己
    // 的节奏被通知——A 的 processing 状态不阻塞 B 的排期（处理器为中断
    // 语义、快速返回，见 UrbProcessCallback 契约；此处验证排期独立）
    TestFixture fx;
    fx.start();
    std::mutex m;
    std::condition_variable cv;
    std::vector<std::uint32_t> notified; // (seqnum)
    // A：60ms 后通知，通知后立即上报（处理快速）
    fx.scheduler.submit(fx.session, UsbEndpoint{.address = 0x81, .interval = 4}, EndpointAttributes::Isochronous,
                        60ms, 51, {}, [&](Session &session, const UsbEndpoint &ep, std::uint32_t seqnum,
                                           TransferHandle &&) {
                            {
                                std::lock_guard lock(m);
                                notified.push_back(seqnum);
                            }
                            cv.notify_all();
                            fx.scheduler.on_urb_done(ep.address, seqnum);
                        });
    auto t0 = std::chrono::steady_clock::now();
    // B：20ms 后通知
    fx.scheduler.submit(fx.session, UsbEndpoint{.address = 0x82, .interval = 4}, EndpointAttributes::Isochronous,
                        20ms, 52, {}, [&](Session &session, const UsbEndpoint &ep, std::uint32_t seqnum,
                                           TransferHandle &&) {
                            {
                                std::lock_guard lock(m);
                                notified.push_back(seqnum);
                            }
                            cv.notify_all();
                            fx.scheduler.on_urb_done(ep.address, seqnum);
                        });
    std::unique_lock lock(m);
    // B 先到（20ms）：A 的 60ms 排期不拖累 B
    ASSERT_TRUE(cv.wait_for(lock, 2s, [&] { return notified.size() >= 1; }));
    EXPECT_EQ(notified[0], 52);
    EXPECT_GE(std::chrono::steady_clock::now() - t0, 15ms);
    // A 随后到
    ASSERT_TRUE(cv.wait_for(lock, 2s, [&] { return notified.size() >= 2; }));
    EXPECT_EQ(notified[1], 51);
    fx.scheduler.stop();
}
