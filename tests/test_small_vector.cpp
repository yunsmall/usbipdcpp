#include <gtest/gtest.h>
#include <string>
#include <utility>

#include "utils/SmallVector.h"

using namespace usbipdcpp;

// ============== SmallVector Tests ==============

TEST(SmallVector, EmptyVector) {
    SmallVector<int, 4> vec;

    EXPECT_TRUE(vec.empty());
    EXPECT_EQ(vec.size(), 0u);
    EXPECT_EQ(vec.capacity(), 4u);
    EXPECT_FALSE(vec.on_heap());
}

TEST(SmallVector, PushBackStack) {
    SmallVector<int, 4> vec;

    vec.push_back(1);
    vec.push_back(2);
    vec.push_back(3);

    EXPECT_FALSE(vec.empty());
    EXPECT_EQ(vec.size(), 3u);
    EXPECT_EQ(vec.capacity(), 4u);
    EXPECT_FALSE(vec.on_heap());

    EXPECT_EQ(vec[0], 1);
    EXPECT_EQ(vec[1], 2);
    EXPECT_EQ(vec[2], 3);
}

TEST(SmallVector, PushBackHeap) {
    SmallVector<int, 2> vec;

    vec.push_back(1);
    EXPECT_FALSE(vec.on_heap());

    vec.push_back(2);
    EXPECT_FALSE(vec.on_heap());

    vec.push_back(3); // 溢出到堆
    EXPECT_TRUE(vec.on_heap());
    EXPECT_EQ(vec.size(), 3u);

    EXPECT_EQ(vec[0], 1);
    EXPECT_EQ(vec[1], 2);
    EXPECT_EQ(vec[2], 3);
}

TEST(SmallVector, EmplaceBack) {
    SmallVector<std::pair<int, std::string>, 2> vec;

    vec.emplace_back(1, "one");
    vec.emplace_back(2, "two");
    EXPECT_FALSE(vec.on_heap());

    vec.emplace_back(3, "three");
    EXPECT_TRUE(vec.on_heap());

    EXPECT_EQ(vec[0].first, 1);
    EXPECT_EQ(vec[0].second, "one");
    EXPECT_EQ(vec[2].first, 3);
    EXPECT_EQ(vec[2].second, "three");
}

TEST(SmallVector, PopBack) {
    SmallVector<int, 4> vec;

    vec.push_back(1);
    vec.push_back(2);
    vec.push_back(3);

    vec.pop_back();
    EXPECT_EQ(vec.size(), 2u);
    EXPECT_EQ(vec.back(), 2);

    vec.pop_back();
    EXPECT_EQ(vec.size(), 1u);
    EXPECT_EQ(vec.back(), 1);
}

TEST(SmallVector, Clear) {
    SmallVector<int, 2> vec;

    vec.push_back(1);
    vec.push_back(2);
    vec.push_back(3);
    EXPECT_TRUE(vec.on_heap());

    vec.clear();
    EXPECT_TRUE(vec.empty());
    EXPECT_EQ(vec.size(), 0u);
    EXPECT_FALSE(vec.on_heap()); // 清空后回到栈
}

TEST(SmallVector, Resize) {
    SmallVector<int, 4> vec;

    vec.push_back(1);
    vec.push_back(2);

    // 扩展（仍在栈）
    vec.resize(4);
    EXPECT_EQ(vec.size(), 4u);
    EXPECT_FALSE(vec.on_heap());

    // 扩展到堆
    vec.resize(10);
    EXPECT_TRUE(vec.on_heap());
    EXPECT_EQ(vec.size(), 10u);

    // 缩小回栈
    vec.resize(2);
    EXPECT_FALSE(vec.on_heap());
    EXPECT_EQ(vec.size(), 2u);
}

TEST(SmallVector, Iterators) {
    SmallVector<int, 4> vec;
    vec.push_back(1);
    vec.push_back(2);
    vec.push_back(3);

    int sum = 0;
    for (auto& v : vec) {
        sum += v;
    }
    EXPECT_EQ(sum, 6);

    // const iterator
    const auto& cvec = vec;
    sum = 0;
    for (auto it = cvec.begin(); it != cvec.end(); ++it) {
        sum += *it;
    }
    EXPECT_EQ(sum, 6);
}

TEST(SmallVector, At) {
    SmallVector<int, 4> vec;
    vec.push_back(10);
    vec.push_back(20);

    EXPECT_EQ(vec.at(0), 10);
    EXPECT_EQ(vec.at(1), 20);

    EXPECT_THROW((void)vec.at(2), std::out_of_range);
    EXPECT_THROW((void)vec.at(100), std::out_of_range);
}

TEST(SmallVector, FrontBack) {
    SmallVector<int, 4> vec;
    vec.push_back(1);
    vec.push_back(2);
    vec.push_back(3);

    EXPECT_EQ(vec.front(), 1);
    EXPECT_EQ(vec.back(), 3);
}

TEST(SmallVector, Data) {
    SmallVector<int, 4> vec;
    vec.push_back(10);
    vec.push_back(20);

    int* ptr = vec.data();
    EXPECT_EQ(ptr[0], 10);
    EXPECT_EQ(ptr[1], 20);
}

TEST(SmallVector, CopyConstructor) {
    SmallVector<int, 2> vec1;
    vec1.push_back(1);
    vec1.push_back(2);
    vec1.push_back(3); // 堆

    SmallVector<int, 2> vec2(vec1);
    EXPECT_EQ(vec2.size(), 3u);
    EXPECT_TRUE(vec2.on_heap());
    EXPECT_EQ(vec2[0], 1);
    EXPECT_EQ(vec2[1], 2);
    EXPECT_EQ(vec2[2], 3);
}

TEST(SmallVector, MoveConstructor) {
    SmallVector<int, 2> vec1;
    vec1.push_back(1);
    vec1.push_back(2);
    vec1.push_back(3);

    SmallVector<int, 2> vec2(std::move(vec1));
    EXPECT_EQ(vec2.size(), 3u);
    EXPECT_TRUE(vec2.on_heap());
    EXPECT_EQ(vec1.size(), 0u);
    EXPECT_FALSE(vec1.on_heap());
}

TEST(SmallVector, CopyAssignment) {
    SmallVector<int, 2> vec1;
    vec1.push_back(1);
    vec1.push_back(2);
    vec1.push_back(3);

    SmallVector<int, 2> vec2;
    vec2 = vec1;

    EXPECT_EQ(vec2.size(), 3u);
    EXPECT_TRUE(vec2.on_heap());
    EXPECT_EQ(vec2[0], 1);
    EXPECT_EQ(vec2[2], 3);
}

TEST(SmallVector, MoveAssignment) {
    SmallVector<int, 2> vec1;
    vec1.push_back(1);
    vec1.push_back(2);
    vec1.push_back(3);

    SmallVector<int, 2> vec2;
    vec2 = std::move(vec1);

    EXPECT_EQ(vec2.size(), 3u);
    EXPECT_EQ(vec1.size(), 0u);
}

TEST(SmallVector, Reserve) {
    SmallVector<int, 2> vec;

    vec.reserve(10); // 大于 N，迁移到堆
    EXPECT_TRUE(vec.on_heap());
    EXPECT_GE(vec.capacity(), 10u);
    EXPECT_EQ(vec.size(), 0u);

    vec.push_back(1);
    vec.push_back(2);
    vec.push_back(3);
    EXPECT_EQ(vec.size(), 3u);
}

TEST(SmallVector, ComplexType) {
    struct NonTrivial {
        std::string s;
        int* p = nullptr;

        NonTrivial() = default;
        NonTrivial(std::string str, int* ptr) : s(std::move(str)), p(ptr) {}

        // 确保拷贝/移动正确
        NonTrivial(const NonTrivial& other) : s(other.s), p(other.p) {}
        NonTrivial& operator=(const NonTrivial& other) {
            s = other.s;
            p = other.p;
            return *this;
        }
    };

    int x = 42;
    SmallVector<NonTrivial, 2> vec;

    vec.emplace_back("first", &x);
    vec.emplace_back("second", &x);
    EXPECT_FALSE(vec.on_heap());

    vec.emplace_back("third", &x);
    EXPECT_TRUE(vec.on_heap());

    EXPECT_EQ(vec[0].s, "first");
    EXPECT_EQ(vec[0].p, &x);
    EXPECT_EQ(vec[2].s, "third");
}

// ============== 极端情况测试 ==============

TEST(SmallVector, ExactlyNElements) {
    // 刚好在栈容量边界
    SmallVector<int, 4> vec;

    vec.push_back(1);
    vec.push_back(2);
    vec.push_back(3);
    vec.push_back(4); // 刚好满

    EXPECT_EQ(vec.size(), 4u);
    EXPECT_FALSE(vec.on_heap()); // 仍在栈上

    vec.push_back(5); // 溢出
    EXPECT_TRUE(vec.on_heap());
    EXPECT_EQ(vec.size(), 5u);
}

TEST(SmallVector, ResizeToZero) {
    SmallVector<int, 2> vec;

    vec.push_back(1);
    vec.push_back(2);
    vec.push_back(3);
    EXPECT_TRUE(vec.on_heap());

    vec.resize(0);
    EXPECT_EQ(vec.size(), 0u);
    EXPECT_TRUE(vec.empty());
    EXPECT_FALSE(vec.on_heap()); // resize(0) 回到栈

    // 可以重新使用
    vec.push_back(10);
    EXPECT_EQ(vec.size(), 1u);
    EXPECT_EQ(vec[0], 10);
}

TEST(SmallVector, EmptyOperations) {
    SmallVector<int, 4> vec;

    // 空容器操作
    EXPECT_TRUE(vec.empty());
    EXPECT_EQ(vec.size(), 0u);
    EXPECT_EQ(vec.begin(), vec.end());

    vec.pop_back(); // 对空容器 pop_back（未定义行为，但不应崩溃）
    vec.clear();    // 对空容器 clear

    EXPECT_TRUE(vec.empty());
}

TEST(SmallVector, LargeData) {
    SmallVector<int, 4> vec;

    // 大量数据
    for (int i = 0; i < 1000; ++i) {
        vec.push_back(i);
    }

    EXPECT_TRUE(vec.on_heap());
    EXPECT_EQ(vec.size(), 1000u);

    // 验证数据正确
    for (int i = 0; i < 1000; ++i) {
        EXPECT_EQ(vec[i], i);
    }
}

TEST(SmallVector, SelfAssignment) {
    SmallVector<int, 2> vec;
    vec.push_back(1);
    vec.push_back(2);

    // 自赋值保护
    vec = vec;
    EXPECT_EQ(vec.size(), 2u);
    EXPECT_EQ(vec[0], 1);
    EXPECT_EQ(vec[1], 2);
}

TEST(SmallVector, ReserveOnStack) {
    SmallVector<int, 4> vec;

    vec.reserve(2); // 小于 N，仍在栈
    EXPECT_FALSE(vec.on_heap());
    EXPECT_EQ(vec.capacity(), 4u); // 栈容量不变

    vec.reserve(4); // 等于 N，仍在栈
    EXPECT_FALSE(vec.on_heap());
}

TEST(SmallVector, MultipleStackHeapTransitions) {
    SmallVector<int, 2> vec;

    // 栈 -> 堆
    vec.push_back(1);
    vec.push_back(2);
    vec.push_back(3);
    EXPECT_TRUE(vec.on_heap());

    // 堆 -> 栈 (通过 resize)
    vec.resize(1);
    EXPECT_FALSE(vec.on_heap());

    // 栈 -> 堆
    vec.push_back(2);
    vec.push_back(3);
    EXPECT_TRUE(vec.on_heap());

    // 堆 -> 栈 (通过 clear)
    vec.clear();
    EXPECT_FALSE(vec.on_heap());
}

TEST(SmallVector, StressTest) {
    SmallVector<int, 4> vec;

    // 大量 push/pop
    for (int round = 0; round < 100; ++round) {
        for (int i = 0; i < 100; ++i) {
            vec.push_back(i);
        }
        for (int i = 0; i < 50; ++i) {
            vec.pop_back();
        }
    }

    EXPECT_GT(vec.size(), 0u);
}
