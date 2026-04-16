#include <gtest/gtest.h>
#include <thread>
#include <vector>
#include <atomic>

#include "utils/ConcurrentTransferTracker.h"
#include "utils/ConcurrentUnlinkMap.h"

using namespace usbipdcpp;

// ============== ConcurrentTransferTracker Tests ==============

TEST(ConcurrentTransferTracker, BasicOperations) {
    ConcurrentTransferTracker<int*> tracker;

    // 注册传输
    EXPECT_TRUE(tracker.register_transfer(1, (int*)0x100, 0x81));
    EXPECT_TRUE(tracker.register_transfer(2, (int*)0x200, 0x82));
    EXPECT_TRUE(tracker.register_transfer(3, (int*)0x300, 0x83));

    // 重复注册应失败
    EXPECT_FALSE(tracker.register_transfer(1, (int*)0x100, 0x81));

    // 检查存在
    EXPECT_TRUE(tracker.contains(1));
    EXPECT_TRUE(tracker.contains(2));
    EXPECT_FALSE(tracker.contains(999));

    // 获取传输信息
    auto info = tracker.get(1);
    EXPECT_TRUE(info.has_value());
    EXPECT_EQ(info->seqnum, 1u);
    EXPECT_EQ(info->transfer, (int*)0x100);
    EXPECT_EQ(info->endpoint, 0x81);

    // 删除
    EXPECT_TRUE(tracker.remove(1));
    EXPECT_FALSE(tracker.contains(1));
    EXPECT_FALSE(tracker.remove(1)); // 重复删除失败

    // 并发数
    EXPECT_EQ(tracker.concurrent_count(), 2u);
}

TEST(ConcurrentTransferTracker, ConcurrentCount) {
    ConcurrentTransferTracker<int*> tracker;

    EXPECT_EQ(tracker.concurrent_count(), 0u);

    tracker.register_transfer(1, (int*)1, 0);
    EXPECT_EQ(tracker.concurrent_count(), 1u);

    tracker.register_transfer(2, (int*)2, 0);
    EXPECT_EQ(tracker.concurrent_count(), 2u);

    tracker.remove(1);
    EXPECT_EQ(tracker.concurrent_count(), 1u);

    tracker.clear();
    EXPECT_EQ(tracker.concurrent_count(), 0u);
}

TEST(ConcurrentTransferTracker, GetAllTransfers) {
    ConcurrentTransferTracker<int*> tracker;

    tracker.register_transfer(1, (int*)0x100, 0x81);
    tracker.register_transfer(2, (int*)0x200, 0x82);
    tracker.register_transfer(3, (int*)0x300, 0x83);

    auto all = tracker.get_all_transfers();
    EXPECT_EQ(all.size(), 3u);

    // 验证所有传输都被获取
    std::set<std::uint32_t> seqnums;
    for (const auto& info : all) {
        seqnums.insert(info.seqnum);
    }
    EXPECT_TRUE(seqnums.count(1));
    EXPECT_TRUE(seqnums.count(2));
    EXPECT_TRUE(seqnums.count(3));
}

TEST(ConcurrentTransferTracker, ConcurrentAccess) {
    ConcurrentTransferTracker<int*> tracker;
    std::atomic<int> success_count{0};
    constexpr int num_threads = 8;
    constexpr int ops_per_thread = 1000;

    // 并发注册
    std::vector<std::thread> threads;
    for (int t = 0; t < num_threads; ++t) {
        threads.emplace_back([&, t]() {
            for (int i = 0; i < ops_per_thread; ++i) {
                std::uint32_t seqnum = t * ops_per_thread + i + 1;
                if (tracker.register_transfer(seqnum, (int*)(uintptr_t)seqnum, 0)) {
                    success_count++;
                }
            }
        });
    }

    for (auto& th : threads) {
        th.join();
    }

    EXPECT_EQ(success_count.load(), num_threads * ops_per_thread);
    EXPECT_EQ(tracker.concurrent_count(), num_threads * ops_per_thread);

    // 并发删除
    success_count = 0;
    threads.clear();
    for (int t = 0; t < num_threads; ++t) {
        threads.emplace_back([&, t]() {
            for (int i = 0; i < ops_per_thread; ++i) {
                std::uint32_t seqnum = t * ops_per_thread + i + 1;
                if (tracker.remove(seqnum)) {
                    success_count++;
                }
            }
        });
    }

    for (auto& th : threads) {
        th.join();
    }

    EXPECT_EQ(success_count.load(), num_threads * ops_per_thread);
    EXPECT_EQ(tracker.concurrent_count(), 0u);
}

// ============== ConcurrentUnlinkMap Tests ==============

TEST(ConcurrentUnlinkMap, BasicOperations) {
    ConcurrentUnlinkMap<> map;

    // 插入
    map.insert(100, 1000);
    map.insert(200, 2000);
    map.insert(300, 3000);

    // 查询
    auto [found1, val1] = map.get(100);
    EXPECT_TRUE(found1);
    EXPECT_EQ(val1, 1000u);

    auto [found2, val2] = map.get(200);
    EXPECT_TRUE(found2);
    EXPECT_EQ(val2, 2000u);

    auto [found3, val3] = map.get(999);
    EXPECT_FALSE(found3);
    EXPECT_EQ(val3, 0u);

    // contains
    EXPECT_TRUE(map.contains(100));
    EXPECT_FALSE(map.contains(999));

    // 删除
    map.erase(100);
    EXPECT_FALSE(map.contains(100));
}

TEST(ConcurrentUnlinkMap, Size) {
    ConcurrentUnlinkMap<> map;

    EXPECT_EQ(map.size(), 0u);

    map.insert(1, 100);
    EXPECT_EQ(map.size(), 1u);

    map.insert(2, 200);
    EXPECT_EQ(map.size(), 2u);

    map.erase(1);
    EXPECT_EQ(map.size(), 1u);

    map.clear();
    EXPECT_EQ(map.size(), 0u);
}

TEST(ConcurrentUnlinkMap, Overwrite) {
    ConcurrentUnlinkMap<> map;

    map.insert(100, 1000);
    auto [found1, val1] = map.get(100);
    EXPECT_TRUE(found1);
    EXPECT_EQ(val1, 1000u);

    // 覆盖
    map.insert(100, 2000);
    auto [found2, val2] = map.get(100);
    EXPECT_TRUE(found2);
    EXPECT_EQ(val2, 2000u);
}

TEST(ConcurrentUnlinkMap, ConcurrentAccess) {
    ConcurrentUnlinkMap<> map;
    std::atomic<int> insert_count{0};
    std::atomic<int> read_count{0};
    constexpr int num_threads = 8;
    constexpr int ops_per_thread = 1000;

    // 并发插入
    std::vector<std::thread> threads;
    for (int t = 0; t < num_threads; ++t) {
        threads.emplace_back([&, t]() {
            for (int i = 0; i < ops_per_thread; ++i) {
                std::uint32_t key = t * ops_per_thread + i + 1;
                std::uint32_t value = key * 10;
                map.insert(key, value);
                insert_count++;
            }
        });
    }

    for (auto& th : threads) {
        th.join();
    }

    EXPECT_EQ(insert_count.load(), num_threads * ops_per_thread);

    // 并发读取
    threads.clear();
    for (int t = 0; t < num_threads; ++t) {
        threads.emplace_back([&, t]() {
            for (int i = 0; i < ops_per_thread; ++i) {
                std::uint32_t key = t * ops_per_thread + i + 1;
                auto [found, val] = map.get(key);
                if (found && val == key * 10) {
                    read_count++;
                }
            }
        });
    }

    for (auto& th : threads) {
        th.join();
    }

    EXPECT_EQ(read_count.load(), num_threads * ops_per_thread);
}

TEST(ConcurrentUnlinkMap, SegmentDistribution) {
    // 测试分段分布：不同的 seqnum 应该落入不同分段
    ConcurrentUnlinkMap<16> map;

    // 插入多个连续 seqnum
    for (std::uint32_t i = 0; i < 16; ++i) {
        map.insert(i, i * 10);
    }

    // 验证所有都能正确读取
    for (std::uint32_t i = 0; i < 16; ++i) {
        auto [found, val] = map.get(i);
        EXPECT_TRUE(found);
        EXPECT_EQ(val, i * 10);
    }

    EXPECT_EQ(map.size(), 16u);
}

// ============== 极端情况测试 ==============

TEST(ConcurrentTransferTracker, EmptyTracker) {
    ConcurrentTransferTracker<int*> tracker;

    // 空tracker操作
    EXPECT_EQ(tracker.concurrent_count(), 0u);
    EXPECT_FALSE(tracker.contains(1));
    EXPECT_FALSE(tracker.get(1).has_value());
    EXPECT_FALSE(tracker.remove(1)); // 删除不存在的
    EXPECT_TRUE(tracker.get_all_transfers().empty());
}

TEST(ConcurrentTransferTracker, SegmentBoundary) {
    // 测试段边界：16段的边界是 0, 16, 32, 48...
    ConcurrentTransferTracker<int*, 16> tracker;

    // 在段边界上插入
    EXPECT_TRUE(tracker.register_transfer(0, (int*)0x100, 0));
    EXPECT_TRUE(tracker.register_transfer(15, (int*)0x200, 0)); // 段末尾
    EXPECT_TRUE(tracker.register_transfer(16, (int*)0x300, 0)); // 下一段开始
    EXPECT_TRUE(tracker.register_transfer(31, (int*)0x400, 0));
    EXPECT_TRUE(tracker.register_transfer(32, (int*)0x500, 0));

    // 验证都能正确读取
    EXPECT_TRUE(tracker.contains(0));
    EXPECT_TRUE(tracker.contains(15));
    EXPECT_TRUE(tracker.contains(16));
    EXPECT_TRUE(tracker.contains(31));
    EXPECT_TRUE(tracker.contains(32));

    EXPECT_EQ(tracker.concurrent_count(), 5u);
}

TEST(ConcurrentTransferTracker, AfterClear) {
    ConcurrentTransferTracker<int*> tracker;

    tracker.register_transfer(1, (int*)1, 0);
    tracker.register_transfer(2, (int*)2, 0);
    tracker.clear();

    // clear后操作
    EXPECT_EQ(tracker.concurrent_count(), 0u);
    EXPECT_FALSE(tracker.contains(1));
    EXPECT_FALSE(tracker.get(1).has_value());

    // 可以重新注册
    EXPECT_TRUE(tracker.register_transfer(1, (int*)1, 0));
    EXPECT_TRUE(tracker.contains(1));
}

TEST(ConcurrentTransferTracker, ConcurrentReadWrite) {
    ConcurrentTransferTracker<int*> tracker;
    std::atomic<bool> running{true};
    std::atomic<int> read_count{0};
    std::atomic<int> write_count{0};
    constexpr int num_readers = 4;
    constexpr int num_writers = 4;

    // 预先插入一些数据
    for (int i = 0; i < 100; ++i) {
        tracker.register_transfer(i, (int*)(uintptr_t)i, 0);
    }

    std::vector<std::thread> threads;

    // 读线程
    for (int t = 0; t < num_readers; ++t) {
        threads.emplace_back([&]() {
            while (running.load()) {
                for (int i = 0; i < 100; ++i) {
                    tracker.contains(i);
                    tracker.get(i);
                    read_count++;
                }
            }
        });
    }

    // 写线程
    for (int t = 0; t < num_writers; ++t) {
        threads.emplace_back([&, t]() {
            int seqnum = 100 + t * 1000;
            while (running.load()) {
                tracker.register_transfer(seqnum, (int*)(uintptr_t)seqnum, 0);
                tracker.remove(seqnum);
                write_count++;
            }
        });
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    running = false;

    for (auto& th : threads) {
        th.join();
    }

    EXPECT_GT(read_count.load(), 0);
    EXPECT_GT(write_count.load(), 0);
}

TEST(ConcurrentUnlinkMap, EmptyMap) {
    ConcurrentUnlinkMap<> map;

    EXPECT_EQ(map.size(), 0u);
    EXPECT_FALSE(map.contains(1));

    auto [found, val] = map.get(1);
    EXPECT_FALSE(found);
    EXPECT_EQ(val, 0u);

    // 删除不存在的键
    map.erase(999); // 不应崩溃
    EXPECT_EQ(map.size(), 0u);
}

TEST(ConcurrentUnlinkMap, EraseNonExistent) {
    ConcurrentUnlinkMap<> map;

    map.insert(1, 100);
    map.erase(999); // 删除不存在的键
    EXPECT_EQ(map.size(), 1u);
    EXPECT_TRUE(map.contains(1));
}

TEST(ConcurrentUnlinkMap, AfterClear) {
    ConcurrentUnlinkMap<> map;

    map.insert(1, 100);
    map.insert(2, 200);
    map.clear();

    EXPECT_EQ(map.size(), 0u);
    EXPECT_FALSE(map.contains(1));
    EXPECT_FALSE(map.contains(2));

    // 可以重新插入
    map.insert(3, 300);
    EXPECT_TRUE(map.contains(3));
    EXPECT_EQ(map.size(), 1u);
}

TEST(ConcurrentUnlinkMap, SegmentBoundary) {
    ConcurrentUnlinkMap<16> map;

    // 在段边界上插入
    map.insert(0, 0);
    map.insert(15, 150); // 段末尾
    map.insert(16, 160); // 下一段开始
    map.insert(31, 310);
    map.insert(32, 320);

    EXPECT_EQ(map.size(), 5u);
    EXPECT_TRUE(map.contains(0));
    EXPECT_TRUE(map.contains(15));
    EXPECT_TRUE(map.contains(16));
    EXPECT_TRUE(map.contains(31));
    EXPECT_TRUE(map.contains(32));
}

TEST(ConcurrentUnlinkMap, ConcurrentReadWrite) {
    ConcurrentUnlinkMap<> map;
    std::atomic<bool> running{true};
    std::atomic<int> read_count{0};
    std::atomic<int> write_count{0};
    constexpr int num_readers = 4;
    constexpr int num_writers = 4;

    // 预先插入一些数据
    for (int i = 0; i < 100; ++i) {
        map.insert(i, i * 10);
    }

    std::vector<std::thread> threads;

    // 读线程
    for (int t = 0; t < num_readers; ++t) {
        threads.emplace_back([&]() {
            while (running.load()) {
                for (int i = 0; i < 100; ++i) {
                    map.contains(i);
                    map.get(i);
                    read_count++;
                }
            }
        });
    }

    // 写线程
    for (int t = 0; t < num_writers; ++t) {
        threads.emplace_back([&, t]() {
            int key = 100 + t * 1000;
            while (running.load()) {
                map.insert(key, key * 10);
                map.erase(key);
                write_count++;
            }
        });
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    running = false;

    for (auto& th : threads) {
        th.join();
    }

    EXPECT_GT(read_count.load(), 0);
    EXPECT_GT(write_count.load(), 0);
}
