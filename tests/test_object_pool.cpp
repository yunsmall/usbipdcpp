#include <gtest/gtest.h>
#include <thread>
#include <vector>
#include <atomic>

#include "utils/ObjectPool.h"

using namespace usbipdcpp;

// ============== ObjectPool Tests (非线程安全) ==============

struct TestObject {
    int value = 0;
    std::string name;

    TestObject() = default;
    TestObject(int v, std::string n) : value(v), name(std::move(n)) {}
};

TEST(ObjectPool, BasicAllocFree) {
    ObjectPool<TestObject, 4> pool;

    // 初始状态
    EXPECT_EQ(pool.available(), 4u);
    EXPECT_EQ(pool.capacity(), 4u);

    // 分配
    auto* obj1 = pool.alloc();
    ASSERT_NE(obj1, nullptr);
    EXPECT_EQ(pool.available(), 3u);

    auto* obj2 = pool.alloc();
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

    auto* obj1 = pool.alloc();
    auto* obj2 = pool.alloc();
    auto* obj3 = pool.alloc(); // 池已空

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

    auto* obj = pool.alloc();
    ASSERT_NE(obj, nullptr);

    EXPECT_TRUE(pool.free(obj));
    EXPECT_FALSE(pool.free(obj)); // 重复 free
}

TEST(ObjectPool, ObjectReuse) {
    ObjectPool<TestObject, 2> pool;

    // 分配并设置值
    auto* obj1 = pool.alloc();
    obj1->value = 42;
    obj1->name = "test";

    // 归还
    pool.free(obj1);

    // 再次分配，应该得到同一个对象（但值可能未重置）
    auto* obj2 = pool.alloc();
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

// ============== ObjectPool Tests (线程安全) ==============

TEST(ObjectPoolThreadSafe, ConcurrentAllocFree) {
    ObjectPool<int, 1000, true> pool;
    std::atomic<int> alloc_count{0};
    std::atomic<int> free_count{0};
    constexpr int num_threads = 8;
    constexpr int ops_per_thread = 1000;

    std::vector<std::thread> threads;
    std::vector<std::vector<int*>> thread_objects(num_threads);

    // 并发分配
    for (int t = 0; t < num_threads; ++t) {
        threads.emplace_back([&, t]() {
            for (int i = 0; i < ops_per_thread; ++i) {
                auto* obj = pool.alloc();
                if (obj) {
                    alloc_count++;
                    thread_objects[t].push_back(obj);
                    *obj = t * ops_per_thread + i;
                }
            }
        });
    }

    for (auto& th : threads) {
        th.join();
    }

    // 并发归还
    threads.clear();
    for (int t = 0; t < num_threads; ++t) {
        threads.emplace_back([&, t]() {
            for (auto* obj : thread_objects[t]) {
                if (pool.free(obj)) {
                    free_count++;
                }
            }
        });
    }

    for (auto& th : threads) {
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
            int* objs[10] = {};
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

    for (auto& th : threads) {
        th.join();
    }

    EXPECT_GT(total_ops.load(), 0);
}

// ============== 极端情况测试 ==============

TEST(ObjectPool, AllocateAllFreeAll) {
    ObjectPool<int, 4> pool;
    std::vector<int*> ptrs;

    // 分配所有
    for (int i = 0; i < 4; ++i) {
        auto* p = pool.alloc();
        ASSERT_NE(p, nullptr);
        ptrs.push_back(p);
    }
    EXPECT_EQ(pool.available(), 0u);

    // 再次分配应失败
    EXPECT_EQ(pool.alloc(), nullptr);

    // 归还所有
    for (auto* p : ptrs) {
        EXPECT_TRUE(pool.free(p));
    }
    EXPECT_EQ(pool.available(), 4u);

    // 可以再次分配
    EXPECT_NE(pool.alloc(), nullptr);
}

TEST(ObjectPool, FreeWrongPointer) {
    ObjectPool<int, 2> pool;

    auto* p1 = pool.alloc();
    auto* p2 = pool.alloc();

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
            std::vector<int*> local;
            while (running.load()) {
                if (auto* p = pool.alloc()) {
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
            for (auto* p : local) {
                pool.free(p);
            }
        });
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    running = false;

    for (auto& th : threads) {
        th.join();
    }

    // 池最终应该恢复到满状态
    EXPECT_EQ(pool.available(), pool.capacity());
    EXPECT_GE(min_available.load(), 0);
}

TEST(ObjectPool, SingleElement) {
    ObjectPool<int, 1> pool;

    EXPECT_EQ(pool.capacity(), 1u);
    EXPECT_EQ(pool.available(), 1u);

    auto* p = pool.alloc();
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(pool.available(), 0u);

    EXPECT_EQ(pool.alloc(), nullptr); // 池空

    EXPECT_TRUE(pool.free(p));
    EXPECT_EQ(pool.available(), 1u);
}
