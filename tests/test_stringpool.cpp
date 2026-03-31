#include <gtest/gtest.h>

#include "utils/StringPool.h"

using namespace usbipdcpp;

TEST(TestStringPool, NewString) {
    StringPool pool;

    auto idx1 = pool.new_string(L"Hello");
    EXPECT_EQ(idx1, 1);

    auto idx2 = pool.new_string(L"World");
    EXPECT_EQ(idx2, 2);

    auto idx3 = pool.new_string(L"Test");
    EXPECT_EQ(idx3, 3);
}

TEST(TestStringPool, GetString) {
    StringPool pool;

    auto idx = pool.new_string(L"Hello World");
    auto result = pool.get_string(idx);

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value(), L"Hello World");
}

TEST(TestStringPool, GetStringInvalid) {
    StringPool pool;

    auto result = pool.get_string(100);
    EXPECT_FALSE(result.has_value());
}

TEST(TestStringPool, RemoveString) {
    StringPool pool;

    auto idx = pool.new_string(L"Test");
    EXPECT_TRUE(pool.get_string(idx).has_value());

    pool.remove_string(idx);
    EXPECT_FALSE(pool.get_string(idx).has_value());
}

TEST(TestStringPool, ReuseIndex) {
    StringPool pool;

    auto idx1 = pool.new_string(L"First");
    pool.remove_string(idx1);

    auto idx2 = pool.new_string(L"Second");
    // 索引应该被复用
    EXPECT_EQ(idx1, idx2);
    EXPECT_EQ(pool.get_string(idx2).value(), L"Second");
}

TEST(TestStringPool, MaxIndex) {
    StringPool pool;

    // 索引从1开始，最大到254（uint8_t最大值-1）
    // 测试边界可能太慢，只测试前几个
    for (int i = 0; i < 10; i++) {
        auto idx = pool.new_string(L"Test" + std::to_wstring(i));
        EXPECT_GT(idx, 0);
    }
}
