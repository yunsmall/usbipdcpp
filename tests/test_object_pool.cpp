#include <atomic>
#include <gtest/gtest.h>
#include <thread>
#include <vector>

#include "utils/ObjectPool.h"

using namespace usbipdcpp;

// ============== ObjectPool Tests (非线程安全) ==============

struct TestObject {
    int value = 0;
    std::string name;

    TestObject() = default;
    TestObject(int v, std::string n) : value(v), name(std::move(n)) {
    }
};

TEST(ObjectPool, BasicAllocFree) {
    ObjectPool<TestObject, 4> pool;

    // 初始状态
    EXPECT_EQ(pool.available(), 4u);
    EXPECT_EQ(pool.capacity(), 4u);

    // 分配
    auto *obj1 = pool.alloc();
    ASSERT_NE(obj1, nullptr);
    EXPECT_EQ(pool.available(), 3u);

    auto *obj2 = pool.alloc();
    ASSERT_NE(obj2, nullptr);
    EXPECT_EQ(pool.available(), 2u);

    // 归还
    EXPECT_TRUE(pool.free(obj1));
    EXPECT_EQ(pool.available(), 3u);

    EXPECT_TRUE(pool.free(obj2));
    EXPECT_EQ(pool.available(), 4u);
}

TEST(ObjectPool, PoolExhausted) {
    ObjectPool<int, 2> pool;

    auto *obj1 = pool.alloc();
    auto *obj2 = pool.alloc();
    auto *obj3 = pool.alloc(); // 池已空

    EXPECT_NE(obj1, nullptr);
    EXPECT_NE(obj2, nullptr);
    EXPECT_EQ(obj3, nullptr);
    EXPECT_EQ(pool.available(), 0u);
}

TEST(ObjectPool, InvalidFree) {
    ObjectPool<int, 4> pool;

    int external_value = 0;
    EXPECT_FALSE(pool.free(&external_value)); // 不是池中的对象

    EXPECT_FALSE(pool.free(nullptr)); // 空指针
}

TEST(ObjectPool, DoubleFree) {
    ObjectPool<int, 4> pool;

    auto *obj = pool.alloc();
    ASSERT_NE(obj, nullptr);

    EXPECT_TRUE(pool.free(obj));
    EXPECT_FALSE(pool.free(obj)); // 重复 free
}

TEST(ObjectPool, ObjectReuse) {
    ObjectPool<TestObject, 2> pool;

    // 分配并设置值
    auto *obj1 = pool.alloc();
    obj1->value = 42;
    obj1->name = "test";

    // 归还
    pool.free(obj1);

    // 再次分配，应该得到同一个对象（但值可能未重置）
    auto *obj2 = pool.alloc();
    EXPECT_EQ(obj1, obj2); // 同一个地址
}

TEST(ObjectPool, Clear) {
    ObjectPool<int, 4> pool;

    pool.alloc();
    pool.alloc();
    EXPECT_EQ(pool.available(), 2u);

    pool.clear();
    EXPECT_EQ(pool.available(), 0u);

    // clear 后无法再分配（对象已被删除）
    EXPECT_EQ(pool.alloc(), nullptr);
}

TEST(ObjectPool, Reset) {
    ObjectPool<int, 4> pool;

    // 分配部分对象
    auto *obj1 = pool.alloc();
    auto *obj2 = pool.alloc();
    EXPECT_EQ(pool.available(), 2u);

    // reset 归还所有对象到池
    pool.reset();
    EXPECT_EQ(pool.available(), 4u);

    // reset 后可以重新分配
    auto *obj3 = pool.alloc();
    auto *obj4 = pool.alloc();
    auto *obj5 = pool.alloc();
    auto *obj6 = pool.alloc();
    ASSERT_NE(obj3, nullptr);
    ASSERT_NE(obj4, nullptr);
    ASSERT_NE(obj5, nullptr);
    ASSERT_NE(obj6, nullptr);
    EXPECT_EQ(pool.available(), 0u);

    // 池空后无法分配
    EXPECT_EQ(pool.alloc(), nullptr);

    // 归还所有
    pool.free(obj3);
    pool.free(obj4);
    pool.free(obj5);
    pool.free(obj6);
    EXPECT_EQ(pool.available(), 4u);
}

TEST(ObjectPool, ResetPreservesObjects) {
    ObjectPool<TestObject, 4> pool;

    // 分配并设置值
    auto *obj1 = pool.alloc();
    obj1->value = 100;
    obj1->name = "original";

    // reset 后对象应该还在（地址不变）
    pool.reset();
    EXPECT_EQ(pool.available(), 4u);

    // 再次分配应该能得到原来的对象（值被保留）
    auto *obj2 = pool.alloc();
    EXPECT_EQ(obj1, obj2); // 同一个地址
    EXPECT_EQ(obj2->value, 100); // 值保留
    EXPECT_EQ(obj2->name, "original");
}

TEST(ObjectPool, ResetVsClear) {
    ObjectPool<int, 4> pool;

    // 分配部分
    pool.alloc();
    pool.alloc();

    // reset: 对象保留，可继续使用
    pool.reset();
    EXPECT_EQ(pool.available(), 4u);
    EXPECT_NE(pool.alloc(), nullptr);

    // clear: 对象删除，无法使用
    pool.clear();
    EXPECT_EQ(pool.available(), 0u);
    EXPECT_EQ(pool.alloc(), nullptr);
}

TEST(ObjectPoolThreadSafe, ResetThreadSafe) {
    ObjectPool<int, 100, true> pool;

    // 并发分配
    std::vector<std::thread> threads;
    std::vector<int *> ptrs;
    std::mutex ptrs_mutex;

    for (int t = 0; t < 4; ++t) {
        threads.emplace_back([&]() {
            for (int i = 0; i < 20; ++i) {
                auto *p = pool.alloc();
                if (p) {
                    std::lock_guard<std::mutex> lock(ptrs_mutex);
                    ptrs.push_back(p);
                }
            }
        });
    }

    for (auto &th: threads) {
        th.join();
    }

    EXPECT_LT(pool.available(), pool.capacity());

    // reset 后应该恢复到满状态
    pool.reset();
    EXPECT_EQ(pool.available(), pool.capacity());

    // 可以重新分配
    auto *p = pool.alloc();
    ASSERT_NE(p, nullptr);
    pool.free(p);
}

// ============== ObjectPool Tests (线程安全) ==============

TEST(ObjectPoolThreadSafe, ConcurrentAllocFree) {
    ObjectPool<int, 1000, true> pool;
    std::atomic<int> alloc_count{0};
    std::atomic<int> free_count{0};
    constexpr int num_threads = 8;
    constexpr int ops_per_thread = 1000;

    std::vector<std::thread> threads;
    std::vector<std::vector<int *>> thread_objects(num_threads);

    // 并发分配
    for (int t = 0; t < num_threads; ++t) {
        threads.emplace_back([&, t]() {
            for (int i = 0; i < ops_per_thread; ++i) {
                auto *obj = pool.alloc();
                if (obj) {
                    alloc_count++;
                    thread_objects[t].push_back(obj);
                    *obj = t * ops_per_thread + i;
                }
            }
        });
    }

    for (auto &th: threads) {
        th.join();
    }

    // 并发归还
    threads.clear();
    for (int t = 0; t < num_threads; ++t) {
        threads.emplace_back([&, t]() {
            for (auto *obj: thread_objects[t]) {
                if (pool.free(obj)) {
                    free_count++;
                }
            }
        });
    }

    for (auto &th: threads) {
        th.join();
    }

    EXPECT_EQ(alloc_count.load(), free_count.load());
    EXPECT_EQ(pool.available(), pool.capacity());
}

TEST(ObjectPoolThreadSafe, StressTest) {
    ObjectPool<int, 100, true> pool;
    std::atomic<bool> running{true};
    std::atomic<int> total_ops{0};
    constexpr int num_threads = 4;

    std::vector<std::thread> threads;
    for (int t = 0; t < num_threads; ++t) {
        threads.emplace_back([&]() {
            int *objs[10] = {};
            while (running.load()) {
                for (int i = 0; i < 10; ++i) {
                    if (!objs[i]) {
                        objs[i] = pool.alloc();
                    }
                    if (objs[i]) {
                        *objs[i] = i;
                        if (pool.free(objs[i])) {
                            objs[i] = nullptr;
                            total_ops++;
                        }
                    }
                }
            }
            // 清理
            for (int i = 0; i < 10; ++i) {
                if (objs[i]) {
                    pool.free(objs[i]);
                }
            }
        });
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    running = false;

    for (auto &th: threads) {
        th.join();
    }

    EXPECT_GT(total_ops.load(), 0);
}

// ============== 极端情况测试 ==============

TEST(ObjectPool, AllocateAllFreeAll) {
    ObjectPool<int, 4> pool;
    std::vector<int *> ptrs;

    // 分配所有
    for (int i = 0; i < 4; ++i) {
        auto *p = pool.alloc();
        ASSERT_NE(p, nullptr);
        ptrs.push_back(p);
    }
    EXPECT_EQ(pool.available(), 0u);

    // 再次分配应失败
    EXPECT_EQ(pool.alloc(), nullptr);

    // 归还所有
    for (auto *p: ptrs) {
        EXPECT_TRUE(pool.free(p));
    }
    EXPECT_EQ(pool.available(), 4u);

    // 可以再次分配
    EXPECT_NE(pool.alloc(), nullptr);
}

TEST(ObjectPool, FreeWrongPointer) {
    ObjectPool<int, 2> pool;

    auto *p1 = pool.alloc();
    auto *p2 = pool.alloc();

    // 顺序归还，跨指针归还
    EXPECT_TRUE(pool.free(p1));
    EXPECT_FALSE(pool.free(p1)); // 已归还
    EXPECT_TRUE(pool.free(p2));
    EXPECT_FALSE(pool.free(p2)); // 已归还
}

TEST(ObjectPoolThreadSafe, AvailableThreadSafe) {
    ObjectPool<int, 100, true> pool;
    std::atomic<bool> running{true};
    std::atomic<int> min_available{100};

    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t) {
        threads.emplace_back([&]() {
            std::vector<int *> local;
            while (running.load()) {
                if (auto *p = pool.alloc()) {
                    local.push_back(p);
                }
                size_t avail = pool.available();
                // 记录最小值
                int current_min = min_available.load();
                while (avail < static_cast<size_t>(current_min)) {
                    if (min_available.compare_exchange_weak(current_min, avail)) {
                        break;
                    }
                }
                // 随机归还一些
                if (local.size() > 10) {
                    for (int i = 0; i < 5; ++i) {
                        if (!local.empty()) {
                            pool.free(local.back());
                            local.pop_back();
                        }
                    }
                }
            }
            // 清理
            for (auto *p: local) {
                pool.free(p);
            }
        });
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    running = false;

    for (auto &th: threads) {
        th.join();
    }

    // 池最终应该恢复到满状态
    EXPECT_EQ(pool.available(), pool.capacity());
    EXPECT_GE(min_available.load(), 0);
}

// ============== Custom LifeManager and Reset Tests ==============

struct CountingLM {
    static inline std::atomic<int> create_count{0};
    static inline std::atomic<int> destroy_count{0};

    static int *create() {
        create_count++;
        return new int(0);
    }
    static void destroy(int *p) {
        destroy_count++;
        delete p;
    }
    static void reset_counts() {
        create_count = 0;
        destroy_count = 0;
    }
};

TEST(ObjectPool, CustomLifeManagerCreateDestroy) {
    CountingLM::reset_counts();
    {
        ObjectPool<int, 4, false, CountingLM> pool;
        EXPECT_EQ(CountingLM::create_count.load(), 4);
        EXPECT_EQ(CountingLM::destroy_count.load(), 0);

        auto *p1 = pool.alloc();
        auto *p2 = pool.alloc();
        pool.free(p1);
        pool.free(p2);
    }
    // 析构 destroy 4 个
    EXPECT_EQ(CountingLM::destroy_count.load(), 4);
}

TEST(ObjectPool, CustomLifeManagerClear) {
    CountingLM::reset_counts();
    {
        ObjectPool<int, 4, false, CountingLM> pool;
        CountingLM::reset_counts(); // 排除构造函数里的 create
        pool.clear();
        EXPECT_EQ(CountingLM::destroy_count.load(), 4);
    }
    // 析构时不额外 destroy（已被 clear）
    EXPECT_EQ(CountingLM::destroy_count.load(), 4);
}

struct SetValueReset {
    static void reset(int &obj, int new_value) {
        obj = new_value;
    }
};

TEST(ObjectPool, CustomResetForwardArgs) {
    ObjectPool<int, 4, false, detail::DefaultLM<int>, SetValueReset> pool;

    auto *p = pool.alloc(42);
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(*p, 42);

    auto *p2 = pool.alloc(99);
    ASSERT_NE(p2, nullptr);
    EXPECT_EQ(*p2, 99);
    EXPECT_NE(p, p2);
}

// 模拟 libusb_transfer 的 LifeManager + Reset 用法
struct MockTransfer {
    int num_iso_packets = -1;
    int actual_length = -1;
    int status = -1;
};

struct MockTransferLM {
    static inline std::atomic<int> alloc_count{0};
    static inline std::atomic<int> free_count{0};

    static MockTransfer *create() {
        alloc_count++;
        auto *t = new MockTransfer{};
        t->num_iso_packets = 0;
        return t;
    }
    static void destroy(MockTransfer *p) {
        free_count++;
        delete p;
    }
    static void reset_counts() {
        alloc_count = 0;
        free_count = 0;
    }
};

struct MockTransferReset {
    static void reset(MockTransfer &t) {
        t.actual_length = 0;
        t.status = 0; // LIBUSB_TRANSFER_COMPLETED
    }
};

TEST(ObjectPool, CustomLifeManagerAndReset) {
    MockTransferLM::reset_counts();

    ObjectPool<MockTransfer, 4, false, MockTransferLM, MockTransferReset> pool;
    EXPECT_EQ(MockTransferLM::alloc_count.load(), 4);

    auto *t = pool.alloc();
    ASSERT_NE(t, nullptr);
    EXPECT_EQ(t->actual_length, 0); // Reset 清零
    EXPECT_EQ(t->status, 0);
    EXPECT_EQ(t->num_iso_packets, 0); // LifeManager 设置

    t->actual_length = 100;
    t->status = -5;
    pool.free(t);

    // 再分配同一个对象，应该被 Reset 重置
    auto *t2 = pool.alloc();
    EXPECT_EQ(t, t2);
    EXPECT_EQ(t2->actual_length, 0);
    EXPECT_EQ(t2->status, 0);

    pool.free(t2);
}

TEST(ObjectPool, LifeManagerFallbackPattern) {
    CountingLM::reset_counts();

    ObjectPool<int, 2, false, CountingLM> pool;
    CountingLM::reset_counts(); // 排除构造

    auto *p1 = pool.alloc();
    auto *p2 = pool.alloc();
    auto *p3 = pool.alloc(); // 池空
    EXPECT_NE(p1, nullptr);
    EXPECT_NE(p2, nullptr);
    EXPECT_EQ(p3, nullptr);

    // 回退：直接 create
    p3 = CountingLM::create();
    ASSERT_NE(p3, nullptr);

    pool.free(p1);
    pool.free(p2);
    // p3 不在池中，直接 destroy
    CountingLM::destroy(p3);

    // 池恢复后还能分配
    auto *p4 = pool.alloc();
    EXPECT_NE(p4, nullptr);
    pool.free(p4);
}

// 默认 LifeManager 行为验证
TEST(ObjectPool, DefaultLifeManagerUsesNewDelete) {
    ObjectPool<int, 4> pool;
    auto *p = pool.alloc();
    ASSERT_NE(p, nullptr);
    *p = 123;
    EXPECT_TRUE(pool.free(p));
    // 默认 Reset 不修改值
    auto *p2 = pool.alloc();
    EXPECT_EQ(p, p2);
    // 默认 Reset 是空操作，值保留
    EXPECT_EQ(*p2, 123);
}

TEST(ObjectPool, SingleElement) {
    ObjectPool<int, 1> pool;

    EXPECT_EQ(pool.capacity(), 1u);
    EXPECT_EQ(pool.available(), 1u);

    auto *p = pool.alloc();
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(pool.available(), 0u);

    EXPECT_EQ(pool.alloc(), nullptr); // 池空

    EXPECT_TRUE(pool.free(p));
    EXPECT_EQ(pool.available(), 1u);
}
