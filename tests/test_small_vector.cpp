#include <gtest/gtest.h>

#include <algorithm>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "usbipdcpp/utils/SmallVector.h"

using namespace usbipdcpp;

namespace {

/// 统计存活实例数的类型，用于验证各操作是否真的析构了元素
struct Counted {
    static inline int alive = 0;

    int value = 0;

    Counted() { ++alive; }
    explicit Counted(int v) : value(v) { ++alive; }
    Counted(const Counted &other) : value(other.value) { ++alive; }
    Counted(Counted &&other) noexcept : value(other.value) { ++alive; }
    Counted &operator=(const Counted &) = default;
    Counted &operator=(Counted &&) noexcept = default;
    ~Counted() { --alive; }

    static void reset() { alive = 0; }
};

/// 不可默认构造的类型：验证栈存储不再要求 T 可默认构造
struct NoDefault {
    int value;

    explicit NoDefault(int v) : value(v) {}
};

/// 拷贝到指定次数时抛异常，用于验证插入/扩容的异常安全
struct ThrowingCopy {
    static inline int copies_before_throw = -1;

    int value = 0;

    ThrowingCopy() = default;
    explicit ThrowingCopy(int v) : value(v) {}
    ThrowingCopy(const ThrowingCopy &other) : value(other.value) {
        if (copies_before_throw == 0) {
            throw std::runtime_error("copy failed");
        }
        if (copies_before_throw > 0) {
            --copies_before_throw;
        }
    }
    ThrowingCopy &operator=(const ThrowingCopy &) = default;
    ThrowingCopy(ThrowingCopy &&) noexcept = default;
    ThrowingCopy &operator=(ThrowingCopy &&) noexcept = default;
};

/// 移动到指定次数时抛异常，用于验证栈到堆迁移失败的路径
struct ThrowingMove {
    static inline int moves_before_throw = -1;

    int value = 0;

    ThrowingMove() = default;
    explicit ThrowingMove(int v) : value(v) {}
    ThrowingMove(const ThrowingMove &) = default;
    ThrowingMove &operator=(const ThrowingMove &) = default;
    ThrowingMove(ThrowingMove &&other) : value(other.value) {
        if (moves_before_throw == 0) {
            throw std::runtime_error("move failed");
        }
        if (moves_before_throw > 0) {
            --moves_before_throw;
        }
    }
    ThrowingMove &operator=(ThrowingMove &&) = default;
};

/// 移动赋值到指定次数时抛异常，用于验证中间插入移位的异常安全（不能漏析构）
struct ThrowingMoveAssign {
    static inline int alive = 0;
    static inline int assigns_before_throw = -1;

    int value = 0;

    ThrowingMoveAssign() { ++alive; }
    explicit ThrowingMoveAssign(int v) : value(v) { ++alive; }
    ThrowingMoveAssign(const ThrowingMoveAssign &other) : value(other.value) { ++alive; }
    ThrowingMoveAssign(ThrowingMoveAssign &&other) noexcept : value(other.value) { ++alive; }
    ThrowingMoveAssign &operator=(const ThrowingMoveAssign &) = default;
    ThrowingMoveAssign &operator=(ThrowingMoveAssign &&other) {
        if (assigns_before_throw == 0) {
            throw std::runtime_error("move assign failed");
        }
        if (assigns_before_throw > 0) {
            --assigns_before_throw;
        }
        value = other.value;
        return *this;
    }
    ~ThrowingMoveAssign() { --alive; }
};

} // namespace

// ============== 基本状态 ==============

TEST(SmallVector, EmptyVector) {
    SmallVector<int, 4> vec;

    EXPECT_TRUE(vec.empty());
    EXPECT_EQ(vec.size(), 0u);
    EXPECT_EQ(vec.capacity(), 4u);
    EXPECT_FALSE(vec.on_heap());
    EXPECT_EQ(vec.begin(), vec.end());
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

TEST(SmallVector, ExactlyNElements) {
    SmallVector<int, 4> vec;

    for (int i = 1; i <= 4; ++i) {
        vec.push_back(i);
    }
    EXPECT_EQ(vec.size(), 4u);
    EXPECT_FALSE(vec.on_heap()); // 刚好满，仍在栈上

    vec.push_back(5);
    EXPECT_TRUE(vec.on_heap());
    EXPECT_EQ(vec.size(), 5u);
}

TEST(SmallVector, EmplaceBack) {
    SmallVector<std::pair<int, std::string>, 2> vec;

    vec.emplace_back(1, "one");
    vec.emplace_back(2, "two");
    EXPECT_FALSE(vec.on_heap());

    auto &third = vec.emplace_back(3, "three");
    EXPECT_TRUE(vec.on_heap());
    EXPECT_EQ(third.first, 3);
    EXPECT_EQ(third.second, "three");

    EXPECT_EQ(vec[0].first, 1);
    EXPECT_EQ(vec[0].second, "one");
}

TEST(SmallVector, EmplaceBackReferencesOwnElement) {
    // 尾部溢出时先构造新元素再迁移，参数可以引用本容器元素
    struct FromRef {
        int value;
        explicit FromRef(int v) : value(v) {}
    };
    SmallVector<FromRef, 1> vec;
    vec.emplace_back(1);
    vec.emplace_back(vec[0].value + 1); // 触发迁移，参数来自 vec[0]
    EXPECT_EQ(vec.size(), 2u);
    EXPECT_EQ(vec[0].value, 1);
    EXPECT_EQ(vec[1].value, 2);
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

TEST(SmallVector, PopBackDestroysElement) {
    Counted::reset();
    {
        SmallVector<Counted, 4> vec;
        vec.emplace_back(1);
        vec.emplace_back(2);
        EXPECT_EQ(Counted::alive, 2);

        vec.pop_back();
        EXPECT_EQ(Counted::alive, 1);
        EXPECT_EQ(vec.size(), 1u);
    }
    EXPECT_EQ(Counted::alive, 0);
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

TEST(SmallVector, ClearDestroysAndReleasesHeap) {
    Counted::reset();
    SmallVector<Counted, 2> vec;
    for (int i = 0; i < 5; ++i) {
        vec.emplace_back(i);
    }
    ASSERT_EQ(Counted::alive, 5);
    vec.clear();
    EXPECT_EQ(Counted::alive, 0);
    EXPECT_FALSE(vec.on_heap());
    EXPECT_EQ(vec.capacity(), 2u);
}

TEST(SmallVector, ClearOnEmpty) {
    SmallVector<int, 4> vec;
    vec.clear();
    EXPECT_TRUE(vec.empty());
    EXPECT_EQ(vec.size(), 0u);
}

TEST(SmallVector, DestructorDestroysElements) {
    Counted::reset();
    {
        SmallVector<Counted, 2> vec;
        vec.emplace_back(1);
        vec.emplace_back(2);
        EXPECT_EQ(Counted::alive, 2);
        vec.emplace_back(3); // 堆
        EXPECT_EQ(Counted::alive, 3);
    }
    EXPECT_EQ(Counted::alive, 0);
}

// ============== resize ==============

TEST(SmallVector, Resize) {
    SmallVector<int, 4> vec;

    vec.push_back(1);
    vec.push_back(2);

    vec.resize(4);
    EXPECT_EQ(vec.size(), 4u);
    EXPECT_FALSE(vec.on_heap());

    vec.resize(10);
    EXPECT_TRUE(vec.on_heap());
    EXPECT_EQ(vec.size(), 10u);

    vec.resize(2);
    EXPECT_FALSE(vec.on_heap());
    EXPECT_EQ(vec.size(), 2u);
}

TEST(SmallVector, ResizeValueInitializesNewElements) {
    // 栈路径和堆路径都必须值初始化新增元素
    SmallVector<int, 4> stack_vec;
    stack_vec.push_back(7);
    stack_vec.resize(4);
    EXPECT_EQ(stack_vec[0], 7);
    EXPECT_EQ(stack_vec[1], 0);
    EXPECT_EQ(stack_vec[2], 0);
    EXPECT_EQ(stack_vec[3], 0);

    SmallVector<int, 2> heap_vec;
    heap_vec.push_back(7);
    heap_vec.resize(4); // 迁到堆
    ASSERT_TRUE(heap_vec.on_heap());
    EXPECT_EQ(heap_vec[0], 7);
    EXPECT_EQ(heap_vec[1], 0);
    EXPECT_EQ(heap_vec[2], 0);
    EXPECT_EQ(heap_vec[3], 0);
}

TEST(SmallVector, ResizeWithValue) {
    SmallVector<int, 4> vec;
    vec.push_back(1);
    vec.resize(3, 9);
    EXPECT_EQ(vec.size(), 3u);
    EXPECT_EQ(vec[0], 1);
    EXPECT_EQ(vec[1], 9);
    EXPECT_EQ(vec[2], 9);

    vec.resize(6, 5); // 迁到堆
    ASSERT_TRUE(vec.on_heap());
    EXPECT_EQ(vec[3], 5);
    EXPECT_EQ(vec[5], 5);

    vec.resize(1, 0);
    EXPECT_EQ(vec.size(), 1u);
    EXPECT_EQ(vec[0], 1);
}

TEST(SmallVector, ResizeWithSelfReference) {
    // 填充值引用本容器元素时不能被迁移/析构影响
    SmallVector<std::string, 2> vec;
    vec.push_back("alpha");
    vec.resize(4, vec[0]);
    ASSERT_TRUE(vec.on_heap());
    EXPECT_EQ(vec[0], "alpha");
    EXPECT_EQ(vec[3], "alpha");
}

TEST(SmallVector, ResizeDestroysTruncatedElements) {
    Counted::reset();
    SmallVector<Counted, 2> vec;
    for (int i = 0; i < 5; ++i) {
        vec.emplace_back(i);
    }
    ASSERT_EQ(Counted::alive, 5);

    vec.resize(3);
    EXPECT_EQ(Counted::alive, 3);
    EXPECT_EQ(vec.size(), 3u);

    vec.resize(1); // 缩小到栈容量，迁回栈
    EXPECT_EQ(Counted::alive, 1);
    EXPECT_FALSE(vec.on_heap());

    vec.resize(0);
    EXPECT_EQ(Counted::alive, 0);
}

TEST(SmallVector, ResizeFromHeapToStackWithGrow) {
    // 堆上元素数小于 N 时 resize 扩容并迁回栈：只迁移现有元素，
    // 不能读取堆存储越界部分（回归测试）
    SmallVector<int, 4> vec;
    for (int i = 0; i < 6; i++) {
        vec.push_back(i); // 6 个元素，在堆上
    }
    EXPECT_TRUE(vec.on_heap());
    vec.pop_back();
    vec.pop_back();
    vec.pop_back();
    EXPECT_EQ(vec.size(), 3u);
    EXPECT_TRUE(vec.on_heap());

    vec.resize(4); // 扩容且迁回栈
    EXPECT_FALSE(vec.on_heap());
    EXPECT_EQ(vec.size(), 4u);
    EXPECT_EQ(vec[0], 0);
    EXPECT_EQ(vec[1], 1);
    EXPECT_EQ(vec[2], 2);
    EXPECT_EQ(vec[3], 0); // 新增元素值初始化

    vec.push_back(100);
    EXPECT_EQ(vec.size(), 5u);
    EXPECT_EQ(vec[4], 100);
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
    EXPECT_FALSE(vec.on_heap());

    vec.push_back(10);
    EXPECT_EQ(vec.size(), 1u);
    EXPECT_EQ(vec[0], 10);
}

TEST(SmallVector, ResizeSameSizeKeepsValues) {
    SmallVector<int, 4> vec;
    vec.push_back(1);
    vec.push_back(2);
    vec.resize(2);
    EXPECT_EQ(vec[0], 1);
    EXPECT_EQ(vec[1], 2);
}

// ============== assign ==============

TEST(SmallVector, AssignCountValue) {
    SmallVector<int, 4> vec;
    vec.push_back(1);

    vec.assign(3, 7);
    EXPECT_EQ(vec.size(), 3u);
    EXPECT_FALSE(vec.on_heap());
    EXPECT_EQ(vec[0], 7);
    EXPECT_EQ(vec[2], 7);

    vec.assign(6, 9); // 超过栈容量
    ASSERT_TRUE(vec.on_heap());
    EXPECT_EQ(vec.size(), 6u);
    EXPECT_EQ(vec[5], 9);

    vec.assign(0, 0);
    EXPECT_TRUE(vec.empty());
    EXPECT_FALSE(vec.on_heap());
}

TEST(SmallVector, AssignRange) {
    std::vector<int> source{4, 5, 6, 7, 8};
    SmallVector<int, 2> vec;
    vec.push_back(1);

    vec.assign(source.begin(), source.end());
    ASSERT_TRUE(vec.on_heap());
    EXPECT_EQ(vec.size(), 5u);
    EXPECT_EQ(vec[0], 4);
    EXPECT_EQ(vec[4], 8);

    vec.assign(source.begin(), source.begin());
    EXPECT_TRUE(vec.empty());
}

// ============== insert / emplace / erase ==============

TEST(SmallVector, InsertSingleStack) {
    SmallVector<int, 4> vec;
    vec.push_back(1);
    vec.push_back(3);

    auto it = vec.insert(vec.begin() + 1, 2);
    EXPECT_FALSE(vec.on_heap());
    EXPECT_EQ(vec.size(), 3u);
    EXPECT_EQ(vec[0], 1);
    EXPECT_EQ(vec[1], 2);
    EXPECT_EQ(vec[2], 3);
    EXPECT_EQ(*it, 2);
    EXPECT_EQ(it - vec.begin(), 1);

    vec.insert(vec.begin(), 0);
    EXPECT_EQ(vec[0], 0);
    vec.insert(vec.end(), 4);
    EXPECT_EQ(vec.size(), 5u);
    EXPECT_TRUE(vec.on_heap()); // 插满 4 个后溢出到堆
    EXPECT_EQ(vec.back(), 4);
}

TEST(SmallVector, InsertAtBeginEndStackNotFull) {
    SmallVector<int, 4> vec;
    vec.push_back(2);

    vec.insert(vec.begin(), 1);
    vec.insert(vec.end(), 3);
    EXPECT_FALSE(vec.on_heap());
    ASSERT_EQ(vec.size(), 3u);
    EXPECT_EQ(vec[0], 1);
    EXPECT_EQ(vec[1], 2);
    EXPECT_EQ(vec[2], 3);
}

TEST(SmallVector, InsertAtFullStackBoundary) {
    SmallVector<int, 2> vec;
    vec.push_back(1);
    vec.push_back(2); // 栈满

    vec.insert(vec.begin() + 1, 3); // 触发迁移
    EXPECT_TRUE(vec.on_heap());
    ASSERT_EQ(vec.size(), 3u);
    EXPECT_EQ(vec[0], 1);
    EXPECT_EQ(vec[1], 3);
    EXPECT_EQ(vec[2], 2);
}

TEST(SmallVector, InsertCountValue) {
    SmallVector<int, 2> vec;
    vec.push_back(1);
    vec.push_back(4);

    vec.insert(vec.begin() + 1, 2, 9);
    ASSERT_TRUE(vec.on_heap());
    ASSERT_EQ(vec.size(), 4u);
    EXPECT_EQ(vec[0], 1);
    EXPECT_EQ(vec[1], 9);
    EXPECT_EQ(vec[2], 9);
    EXPECT_EQ(vec[3], 4);
}

TEST(SmallVector, InsertRange) {
    SmallVector<int, 4> vec;
    vec.push_back(1);
    vec.push_back(4);

    std::vector<int> middle{2, 3};
    vec.insert(vec.begin() + 1, middle.begin(), middle.end());
    ASSERT_EQ(vec.size(), 4u);
    EXPECT_FALSE(vec.on_heap());
    EXPECT_EQ(vec[0], 1);
    EXPECT_EQ(vec[1], 2);
    EXPECT_EQ(vec[2], 3);
    EXPECT_EQ(vec[3], 4);

    std::vector<int> tail{5, 6};
    vec.insert(vec.end(), tail.begin(), tail.end());
    EXPECT_TRUE(vec.on_heap());
    EXPECT_EQ(vec.size(), 6u);
    EXPECT_EQ(vec[4], 5);
    EXPECT_EQ(vec[5], 6);
}

TEST(SmallVector, EmplaceAtPosition) {
    SmallVector<std::string, 4> vec;
    vec.push_back("a");
    vec.push_back("c");

    auto it = vec.emplace(vec.begin() + 1, 2, 'b'); // "bb"
    EXPECT_EQ(*it, "bb");
    ASSERT_EQ(vec.size(), 3u);
    EXPECT_EQ(vec[0], "a");
    EXPECT_EQ(vec[1], "bb");
    EXPECT_EQ(vec[2], "c");
}

TEST(SmallVector, EraseSingle) {
    SmallVector<int, 4> vec;
    vec.push_back(1);
    vec.push_back(2);
    vec.push_back(3);

    auto it = vec.erase(vec.begin() + 1);
    ASSERT_EQ(vec.size(), 2u);
    EXPECT_EQ(vec[0], 1);
    EXPECT_EQ(vec[1], 3);
    EXPECT_EQ(*it, 3);
    EXPECT_FALSE(vec.on_heap());
}

TEST(SmallVector, EraseRangeStackAndHeap) {
    SmallVector<int, 2> vec;
    for (int i = 0; i < 6; ++i) {
        vec.push_back(i); // 堆
    }
    ASSERT_TRUE(vec.on_heap());

    vec.erase(vec.begin() + 1, vec.begin() + 4); // 删 1,2,3
    ASSERT_EQ(vec.size(), 3u);
    EXPECT_EQ(vec[0], 0);
    EXPECT_EQ(vec[1], 4);
    EXPECT_EQ(vec[2], 5);

    vec.erase(vec.begin(), vec.begin()); // 空范围
    EXPECT_EQ(vec.size(), 3u);

    vec.erase(vec.begin(), vec.begin() + 2);
    ASSERT_EQ(vec.size(), 1u);
    EXPECT_EQ(vec[0], 5);
}

TEST(SmallVector, EraseDestroysElements) {
    Counted::reset();
    SmallVector<Counted, 4> vec;
    for (int i = 0; i < 4; ++i) {
        vec.emplace_back(i);
    }
    ASSERT_EQ(Counted::alive, 4);

    vec.erase(vec.begin() + 1, vec.begin() + 3);
    EXPECT_EQ(Counted::alive, 2);
    EXPECT_EQ(vec.size(), 2u);
}

TEST(SmallVector, InsertEraseAllStackToHeapTransitions) {
    // 反复在栈/堆之间迁移，数据必须始终正确
    SmallVector<int, 3> vec;
    for (int round = 0; round < 50; ++round) {
        vec.insert(vec.begin(), round);
        ASSERT_EQ(vec.front(), round);
        vec.erase(vec.begin());
    }
    EXPECT_TRUE(vec.empty());

    for (int i = 0; i < 10; ++i) {
        vec.push_back(i);
    }
    vec.insert(vec.begin(), 100);
    ASSERT_EQ(vec.size(), 11u);
    vec.erase(vec.begin(), vec.begin() + 5);
    ASSERT_EQ(vec.size(), 6u);
    EXPECT_EQ(vec[0], 4);
}

// ============== 拷贝 / 移动 / swap ==============

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

TEST(SmallVector, CopyConstructorStack) {
    SmallVector<std::string, 4> vec1;
    vec1.push_back("a");
    vec1.push_back("b");

    SmallVector<std::string, 4> vec2(vec1);
    EXPECT_FALSE(vec2.on_heap());
    EXPECT_EQ(vec2.size(), 2u);
    EXPECT_EQ(vec2[0], "a");
    EXPECT_EQ(vec2[1], "b");

    vec1[0] = "changed";
    EXPECT_EQ(vec2[0], "a");
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

TEST(SmallVector, MoveConstructorFromStack) {
    SmallVector<Counted, 4> vec1;
    vec1.emplace_back(1);
    vec1.emplace_back(2);
    EXPECT_EQ(Counted::alive, 2);

    SmallVector<Counted, 4> vec2(std::move(vec1));
    EXPECT_EQ(vec2.size(), 2u);
    EXPECT_EQ(vec1.size(), 0u);
    // 元素总数不变：源容器里的元素被移动后析构
    EXPECT_EQ(Counted::alive, 2);
    EXPECT_EQ(vec2[0].value, 1);
    EXPECT_EQ(vec2[1].value, 2);
}

TEST(SmallVector, CopyAssignment) {
    SmallVector<int, 2> vec1;
    vec1.push_back(1);
    vec1.push_back(2);
    vec1.push_back(3);

    SmallVector<int, 2> vec2;
    vec2.push_back(9);

    vec2 = vec1;
    EXPECT_EQ(vec2.size(), 3u);
    EXPECT_TRUE(vec2.on_heap());
    EXPECT_EQ(vec2[0], 1);
    EXPECT_EQ(vec2[2], 3);

    vec2 = vec2; // 自赋值
    EXPECT_EQ(vec2.size(), 3u);
    EXPECT_EQ(vec2[0], 1);
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
    EXPECT_EQ(vec2[2], 3);
}

TEST(SmallVector, InitializerListConstructor) {
    SmallVector<int, 4> stack_vec{1, 2, 3};
    EXPECT_FALSE(stack_vec.on_heap());
    ASSERT_EQ(stack_vec.size(), 3u);
    EXPECT_EQ(stack_vec[2], 3);

    SmallVector<int, 2> heap_vec{1, 2, 3, 4};
    EXPECT_TRUE(heap_vec.on_heap());
    ASSERT_EQ(heap_vec.size(), 4u);
    EXPECT_EQ(heap_vec[3], 4);
}

TEST(SmallVector, CountConstructors) {
    SmallVector<int, 4> zeros(3);
    ASSERT_EQ(zeros.size(), 3u);
    EXPECT_EQ(zeros[0], 0);
    EXPECT_EQ(zeros[2], 0);

    SmallVector<int, 2> sevens(5, 7);
    ASSERT_TRUE(sevens.on_heap());
    EXPECT_EQ(sevens.size(), 5u);
    EXPECT_EQ(sevens[4], 7);
}

TEST(SmallVector, RangeConstructor) {
    std::vector<int> source{1, 2, 3, 4, 5};
    SmallVector<int, 2> vec(source.begin(), source.end());
    ASSERT_TRUE(vec.on_heap());
    ASSERT_EQ(vec.size(), 5u);
    EXPECT_EQ(vec[0], 1);
    EXPECT_EQ(vec[4], 5);
}

TEST(SmallVector, SwapStackAndHeap) {
    SmallVector<int, 2> a;
    a.push_back(1);
    a.push_back(2);

    SmallVector<int, 2> b;
    for (int i = 0; i < 5; ++i) {
        b.push_back(i);
    }
    ASSERT_TRUE(b.on_heap());

    swap(a, b);
    EXPECT_TRUE(a.on_heap());
    EXPECT_EQ(a.size(), 5u);
    EXPECT_EQ(a[0], 0);

    EXPECT_FALSE(b.on_heap());
    ASSERT_EQ(b.size(), 2u);
    EXPECT_EQ(b[0], 1);
    EXPECT_EQ(b[1], 2);
}

TEST(SmallVector, SwapSelf) {
    SmallVector<int, 2> a;
    a.push_back(1);
    a.push_back(2);
    a.push_back(3);

    a.swap(a);
    EXPECT_EQ(a.size(), 3u);
    EXPECT_EQ(a[0], 1);
    EXPECT_EQ(a[2], 3);
}

// ============== 容量 ==============

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
    EXPECT_EQ(vec[0], 1);
}

TEST(SmallVector, ReserveOnStack) {
    SmallVector<int, 4> vec;

    vec.reserve(2); // 小于 N，仍在栈
    EXPECT_FALSE(vec.on_heap());
    EXPECT_EQ(vec.capacity(), 4u);

    vec.reserve(4); // 等于 N，仍在栈
    EXPECT_FALSE(vec.on_heap());
}

TEST(SmallVector, ShrinkToFit) {
    SmallVector<int, 2> vec;
    for (int i = 0; i < 5; ++i) {
        vec.push_back(i);
    }
    ASSERT_TRUE(vec.on_heap());

    vec.resize(2); // 已回到栈
    EXPECT_FALSE(vec.on_heap());

    vec.reserve(20);
    ASSERT_TRUE(vec.on_heap());
    vec.shrink_to_fit(); // 元素 2 个，迁回栈
    EXPECT_FALSE(vec.on_heap());
    EXPECT_EQ(vec.size(), 2u);
    EXPECT_EQ(vec[1], 1);

    SmallVector<int, 1> big;
    for (int i = 0; i < 10; ++i) {
        big.push_back(i);
    }
    big.shrink_to_fit(); // 元素 10 个，留在堆但收缩容量
    EXPECT_TRUE(big.on_heap());
    EXPECT_EQ(big.capacity(), big.size());
    EXPECT_EQ(big[9], 9);
}

TEST(SmallVector, MaxSize) {
    SmallVector<int, 4> vec;
    EXPECT_GT(vec.max_size(), 4u);
}

// ============== 迭代器 ==============

TEST(SmallVector, Iterators) {
    SmallVector<int, 4> vec;
    vec.push_back(1);
    vec.push_back(2);
    vec.push_back(3);

    int sum = 0;
    for (auto &v: vec) {
        sum += v;
    }
    EXPECT_EQ(sum, 6);

    const auto &cvec = vec;
    sum = 0;
    for (auto it = cvec.begin(); it != cvec.end(); ++it) {
        sum += *it;
    }
    EXPECT_EQ(sum, 6);

    // 反转迭代器
    std::vector<int> reversed(vec.rbegin(), vec.rend());
    ASSERT_EQ(reversed.size(), 3u);
    EXPECT_EQ(reversed[0], 3);
    EXPECT_EQ(reversed[2], 1);

    std::vector<int> const_reversed(cvec.crbegin(), cvec.crend());
    EXPECT_EQ(const_reversed, reversed);
}

TEST(SmallVector, IteratorsAreContiguousAndStdAlgorithmsWork) {
    static_assert(std::contiguous_iterator<SmallVector<int, 4>::iterator>);
    static_assert(std::contiguous_iterator<SmallVector<int, 4>::const_iterator>);

    SmallVector<int, 2> vec;
    for (int i = 0; i < 5; ++i) {
        vec.push_back(5 - i);
    }
    ASSERT_TRUE(vec.on_heap());

    std::sort(vec.begin(), vec.end());
    EXPECT_EQ(vec[0], 1);
    EXPECT_EQ(vec[4], 5);
    EXPECT_TRUE(std::is_sorted(vec.begin(), vec.end()));

    EXPECT_EQ(*std::find(vec.begin(), vec.end(), 3), 3);
    EXPECT_EQ(std::count_if(vec.begin(), vec.end(), [](int v) { return v % 2 == 1; }), 3);
}

TEST(SmallVector, DataIsContiguous) {
    SmallVector<int, 4> vec;
    vec.push_back(10);
    vec.push_back(20);

    int *ptr = vec.data();
    EXPECT_EQ(ptr[0], 10);
    EXPECT_EQ(ptr[1], 20);
    EXPECT_EQ(ptr + vec.size(), vec.end());
}

// ============== 元素访问 ==============

TEST(SmallVector, At) {
    SmallVector<int, 4> vec;
    vec.push_back(10);
    vec.push_back(20);

    EXPECT_EQ(vec.at(0), 10);
    EXPECT_EQ(vec.at(1), 20);

    EXPECT_THROW((void) vec.at(2), std::out_of_range);
    EXPECT_THROW((void) vec.at(100), std::out_of_range);

    const auto &cvec = vec;
    EXPECT_THROW((void) cvec.at(2), std::out_of_range);
}

TEST(SmallVector, FrontBack) {
    SmallVector<int, 4> vec;
    vec.push_back(1);
    vec.push_back(2);
    vec.push_back(3);

    EXPECT_EQ(vec.front(), 1);
    EXPECT_EQ(vec.back(), 3);

    vec.front() = 10;
    vec.back() = 30;
    EXPECT_EQ(vec[0], 10);
    EXPECT_EQ(vec[2], 30);
}

// ============== 比较 ==============

TEST(SmallVector, ComparisonOperators) {
    SmallVector<int, 2> a{1, 2, 3};
    SmallVector<int, 2> b{1, 2, 3};
    SmallVector<int, 2> c{1, 2, 4};
    SmallVector<int, 2> d{1, 2};

    EXPECT_TRUE(a == b);
    EXPECT_FALSE(a != b);
    EXPECT_TRUE(a < c);
    EXPECT_TRUE(a <= c);
    EXPECT_TRUE(c > a);
    EXPECT_TRUE(c >= a);
    EXPECT_TRUE(d < a);
    EXPECT_FALSE(a < b);

    SmallVector<int, 4> stack_a{1, 2, 3};
    EXPECT_TRUE(stack_a == a); // 栈上与堆上内容相同的容器相等
}

// ============== 极端情况 ==============

TEST(SmallVector, SingleElementCapacity) {
    SmallVector<int, 1> vec;

    vec.push_back(1);
    EXPECT_FALSE(vec.on_heap());
    EXPECT_EQ(vec.size(), 1u);

    vec.push_back(2); // 溢出
    ASSERT_TRUE(vec.on_heap());
    EXPECT_EQ(vec[1], 2);

    vec.pop_back();
    EXPECT_EQ(vec.size(), 1u);
    EXPECT_TRUE(vec.on_heap()); // pop_back 不自动迁回栈

    vec.shrink_to_fit();
    EXPECT_FALSE(vec.on_heap());
    EXPECT_EQ(vec[0], 1);
}

TEST(SmallVector, NonDefaultConstructibleType) {
    SmallVector<NoDefault, 2> vec;
    vec.emplace_back(1);
    vec.emplace_back(2);
    vec.emplace_back(3); // 堆

    vec.insert(vec.begin(), NoDefault(0));
    ASSERT_EQ(vec.size(), 4u);
    EXPECT_EQ(vec[0].value, 0);
    EXPECT_EQ(vec[3].value, 3);

    vec.erase(vec.begin());
    EXPECT_EQ(vec[0].value, 1);
}

TEST(SmallVector, NonTrivialTypeLifetime) {
    Counted::reset();
    {
        SmallVector<Counted, 2> vec;
        vec.emplace_back(1);
        vec.emplace_back(2);
        vec.emplace_back(3); // 堆
        vec.insert(vec.begin(), Counted(0));
        EXPECT_EQ(Counted::alive, 4);
        vec.erase(vec.begin() + 1);
        EXPECT_EQ(Counted::alive, 3);
        vec.resize(1);
        EXPECT_EQ(Counted::alive, 1);
    }
    EXPECT_EQ(Counted::alive, 0);
}

TEST(SmallVector, ThrowingCopyKeepsContainerValid) {
    SmallVector<ThrowingCopy, 2> vec;
    vec.emplace_back(1);
    vec.emplace_back(2); // 栈满
    ASSERT_FALSE(vec.on_heap());

    ThrowingCopy::copies_before_throw = 0;
    ThrowingCopy value(3);
    EXPECT_THROW(vec.push_back(value), std::runtime_error);
    ThrowingCopy::copies_before_throw = -1;

    // 失败后容器保持原状且仍可用
    ASSERT_EQ(vec.size(), 2u);
    EXPECT_FALSE(vec.on_heap());
    EXPECT_EQ(vec[0].value, 1);
    EXPECT_EQ(vec[1].value, 2);

    vec.push_back(ThrowingCopy(3));
    EXPECT_EQ(vec.size(), 3u);
    EXPECT_EQ(vec[2].value, 3);
}

TEST(SmallVector, LargeData) {
    SmallVector<int, 4> vec;

    for (int i = 0; i < 1000; ++i) {
        vec.push_back(i);
    }

    EXPECT_TRUE(vec.on_heap());
    EXPECT_EQ(vec.size(), 1000u);

    for (int i = 0; i < 1000; ++i) {
        EXPECT_EQ(vec[i], i);
    }
}

TEST(SmallVector, MultipleStackHeapTransitions) {
    SmallVector<int, 2> vec;

    vec.push_back(1);
    vec.push_back(2);
    vec.push_back(3);
    EXPECT_TRUE(vec.on_heap());

    vec.resize(1);
    EXPECT_FALSE(vec.on_heap());

    vec.push_back(2);
    vec.push_back(3);
    EXPECT_TRUE(vec.on_heap());

    vec.clear();
    EXPECT_FALSE(vec.on_heap());
}

TEST(SmallVector, StressPushPop) {
    SmallVector<int, 4> vec;

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

// ============== 补充边界 ==============

TEST(SmallVector, EmptyContainerOperations) {
    SmallVector<int, 4> vec;

    EXPECT_EQ(vec.begin(), vec.end());
    EXPECT_EQ(vec.rbegin(), vec.rend());
    EXPECT_NE(vec.data(), nullptr);
    EXPECT_EQ(vec.data(), vec.end());

    vec.shrink_to_fit(); // 栈上无操作
    EXPECT_FALSE(vec.on_heap());
    vec.reserve(0);
    EXPECT_FALSE(vec.on_heap());

    vec.insert(vec.begin(), 1); // 空容器插入
    ASSERT_EQ(vec.size(), 1u);
    EXPECT_EQ(vec[0], 1);
    vec.erase(vec.begin()); // 删到空
    EXPECT_TRUE(vec.empty());

    vec.assign(0, 0);
    EXPECT_TRUE(vec.empty());
}

TEST(SmallVector, SwapHeapHeap) {
    SmallVector<int, 2> a;
    SmallVector<int, 2> b;
    for (int i = 0; i < 5; ++i) {
        a.push_back(i);
        b.push_back(i * 10);
    }
    ASSERT_TRUE(a.on_heap());
    ASSERT_TRUE(b.on_heap());

    a.swap(b);
    EXPECT_EQ(a.size(), 5u);
    EXPECT_EQ(a[0], 0);
    EXPECT_EQ(a[4], 40);
    EXPECT_EQ(b[4], 4);
}

TEST(SmallVector, SwapStackStack) {
    SmallVector<int, 4> a{1, 2};
    SmallVector<int, 4> b{3, 4, 5};

    a.swap(b);
    ASSERT_EQ(a.size(), 3u);
    ASSERT_EQ(b.size(), 2u);
    EXPECT_EQ(a[0], 3);
    EXPECT_EQ(b[0], 1);
    EXPECT_FALSE(a.on_heap());
    EXPECT_FALSE(b.on_heap());
}

TEST(SmallVector, CopyAssignmentAcrossStorageModes) {
    SmallVector<int, 4> stack_source{1, 2};
    SmallVector<int, 4> heap_target;
    for (int i = 0; i < 10; ++i) {
        heap_target.push_back(i);
    }
    ASSERT_TRUE(heap_target.on_heap());
    heap_target = stack_source; // 堆 -> 栈
    EXPECT_FALSE(heap_target.on_heap());
    ASSERT_EQ(heap_target.size(), 2u);
    EXPECT_EQ(heap_target[1], 2);

    SmallVector<int, 2> heap_source;
    for (int i = 0; i < 6; ++i) {
        heap_source.push_back(i);
    }
    SmallVector<int, 2> stack_target{9};
    stack_target = heap_source; // 栈 -> 堆
    ASSERT_TRUE(stack_target.on_heap());
    EXPECT_EQ(stack_target[5], 5);
    EXPECT_EQ(heap_source.size(), 6u); // 源容器不变
}

TEST(SmallVector, MoveAssignmentClearsExistingElements) {
    Counted::reset();
    {
        SmallVector<Counted, 2> source;
        source.emplace_back(1);
        source.emplace_back(2);
        source.emplace_back(3); // 堆

        SmallVector<Counted, 2> target;
        target.emplace_back(9);
        target = std::move(source);

        EXPECT_EQ(Counted::alive, 3); // 旧的 9 被析构
        ASSERT_EQ(target.size(), 3u);
        EXPECT_EQ(target[0].value, 1);
        EXPECT_EQ(source.size(), 0u);
    }
    EXPECT_EQ(Counted::alive, 0);
}

TEST(SmallVector, InitializerListAssignment) {
    SmallVector<int, 2> vec{1};
    vec = {5, 6, 7, 8};
    ASSERT_TRUE(vec.on_heap());
    ASSERT_EQ(vec.size(), 4u);
    EXPECT_EQ(vec[0], 5);
    EXPECT_EQ(vec[3], 8);
}

TEST(SmallVector, InsertEmptyRangeAndZeroCount) {
    SmallVector<int, 4> vec{1, 2};
    std::vector<int> empty;
    vec.insert(vec.begin() + 1, empty.begin(), empty.end());
    EXPECT_EQ(vec.size(), 2u);

    vec.insert(vec.begin(), 0, 9);
    EXPECT_EQ(vec.size(), 2u);
    EXPECT_EQ(vec[0], 1);
}

TEST(SmallVector, InsertValueReferencingOwnElement) {
    // 栈满时插入自身元素：必须先复制再迁移
    SmallVector<int, 2> vec{1, 2};
    ASSERT_FALSE(vec.on_heap());
    vec.insert(vec.begin(), vec[0]);
    ASSERT_TRUE(vec.on_heap());
    ASSERT_EQ(vec.size(), 3u);
    EXPECT_EQ(vec[0], 1);
    EXPECT_EQ(vec[1], 1);
    EXPECT_EQ(vec[2], 2);
}

TEST(SmallVector, InsertCountValueReferencingOwnElement) {
    SmallVector<int, 4> vec{1, 2, 3};
    vec.insert(vec.begin() + 1, 2, vec[2]); // 插入两个 3
    ASSERT_EQ(vec.size(), 5u);
    EXPECT_EQ(vec[0], 1);
    EXPECT_EQ(vec[1], 3);
    EXPECT_EQ(vec[2], 3);
    EXPECT_EQ(vec[3], 2);
    EXPECT_EQ(vec[4], 3);
}

TEST(SmallVector, AssignValueReferencingOwnElement) {
    SmallVector<int, 4> vec{7, 8};
    vec.assign(4, vec[0]);
    ASSERT_EQ(vec.size(), 4u);
    EXPECT_EQ(vec[0], 7);
    EXPECT_EQ(vec[3], 7);
}

TEST(SmallVector, EraseAllElements) {
    SmallVector<int, 2> vec;
    for (int i = 0; i < 6; ++i) {
        vec.push_back(i);
    }
    ASSERT_TRUE(vec.on_heap());

    vec.erase(vec.begin(), vec.end());
    EXPECT_TRUE(vec.empty());
    EXPECT_TRUE(vec.on_heap()); // erase 不主动迁回栈
    EXPECT_EQ(vec.size(), 0u);

    vec.push_back(42);
    ASSERT_EQ(vec.size(), 1u);
    EXPECT_EQ(vec[0], 42);
}

TEST(SmallVector, PopBackToEmptyOnHeap) {
    SmallVector<int, 1> vec;
    vec.push_back(1);
    vec.push_back(2);
    ASSERT_TRUE(vec.on_heap());

    vec.pop_back();
    vec.pop_back();
    EXPECT_TRUE(vec.empty());
    EXPECT_TRUE(vec.on_heap()); // pop_back 不迁移存储

    vec.push_back(3);
    EXPECT_EQ(vec.size(), 1u);
    EXPECT_EQ(vec[0], 3);
}

TEST(SmallVector, PushAfterShrinkToFitOnEmpty) {
    SmallVector<int, 1> vec;
    vec.push_back(1);
    vec.push_back(2);
    ASSERT_TRUE(vec.on_heap());

    vec.pop_back();
    vec.pop_back();
    vec.shrink_to_fit(); // 空容器迁回栈
    EXPECT_FALSE(vec.on_heap());
    EXPECT_EQ(vec.capacity(), 1u);

    vec.push_back(9);
    EXPECT_FALSE(vec.on_heap());
    EXPECT_EQ(vec[0], 9);
}

TEST(SmallVector, ComparisonEmptyAndDifferentCapacities) {
    SmallVector<int, 2> a;
    SmallVector<int, 8> b;
    EXPECT_TRUE(a == b);
    EXPECT_FALSE(a < b);

    a.push_back(1);
    EXPECT_TRUE(b < a);
    EXPECT_TRUE(a > b);
}

TEST(SmallVector, ThrowingMoveDuringMigrationKeepsContainerUsable) {    SmallVector<ThrowingMove, 2> vec;
    vec.emplace_back(1);
    vec.emplace_back(2);
    ASSERT_FALSE(vec.on_heap());

    ThrowingMove::moves_before_throw = 2; // 拷出入参成功、迁移第二个元素时抛
    EXPECT_THROW(vec.push_back(ThrowingMove(3)), std::runtime_error);
    ThrowingMove::moves_before_throw = -1;

    // 迁移失败后容器仍可用，元素个数不变
    EXPECT_EQ(vec.size(), 2u);
    EXPECT_FALSE(vec.on_heap());
    EXPECT_EQ(vec[0].value, 1);
    EXPECT_EQ(vec[1].value, 2);

    vec.push_back(ThrowingMove(3));
    EXPECT_EQ(vec.size(), 3u);
    EXPECT_EQ(vec[2].value, 3);
}

TEST(SmallVector, ThrowingMoveAssignDuringInsertDoesNotLeak) {
    ThrowingMoveAssign::alive = 0;
    {
        SmallVector<ThrowingMoveAssign, 4> vec;
        vec.emplace_back(1);
        vec.emplace_back(2);
        vec.emplace_back(3);
        ASSERT_EQ(ThrowingMoveAssign::alive, 3);

        ThrowingMoveAssign::assigns_before_throw = 1; // 第二次移位时抛
        EXPECT_THROW(vec.insert(vec.begin(), ThrowingMoveAssign(4)), std::runtime_error);
        ThrowingMoveAssign::assigns_before_throw = -1;

        // 尾部已经构造的元素必须计入 size，析构时不能漏掉
        EXPECT_EQ(static_cast<int>(vec.size()), ThrowingMoveAssign::alive);
    }
    EXPECT_EQ(ThrowingMoveAssign::alive, 0);
}
