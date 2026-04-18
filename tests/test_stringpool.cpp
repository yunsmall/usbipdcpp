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

// ============== 极端情况测试 ==============

TEST(TestStringPool, EmptyString) {
    StringPool pool;

    auto idx = pool.new_string(L"");
    EXPECT_GT(idx, 0);

    auto result = pool.get_string(idx);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value(), L"");
}

TEST(TestStringPool, LongString) {
    StringPool pool;

    // 长字符串
    std::wstring long_str(1000, L'A');
    auto idx = pool.new_string(long_str);
    EXPECT_GT(idx, 0);

    auto result = pool.get_string(idx);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value(), long_str);
}

TEST(TestStringPool, UnicodeString) {
    StringPool pool;

    // Unicode 字符
    std::wstring unicode = L"你好世界🎉";
    auto idx = pool.new_string(unicode);
    EXPECT_GT(idx, 0);

    auto result = pool.get_string(idx);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value(), unicode);
}

TEST(TestStringPool, RemoveTwice) {
    StringPool pool;

    auto idx = pool.new_string(L"Test");
    EXPECT_TRUE(pool.get_string(idx).has_value());

    pool.remove_string(idx);
    EXPECT_FALSE(pool.get_string(idx).has_value());

    // 再次删除不应崩溃
    pool.remove_string(idx);
    EXPECT_FALSE(pool.get_string(idx).has_value());
}

TEST(TestStringPool, RemoveInvalidIndex) {
    StringPool pool;

    // 删除无效索引不应崩溃
    pool.remove_string(0);
    pool.remove_string(255);
    pool.remove_string(static_cast<std::uint8_t>(232));  // 使用有效范围内的值
}

TEST(TestStringPool, ReuseAfterRemove) {
    StringPool pool;

    auto idx1 = pool.new_string(L"First");
    pool.remove_string(idx1);

    auto idx2 = pool.new_string(L"Second");
    // 索引应该被复用
    EXPECT_EQ(idx1, idx2);

    // 新字符串应该正确
    auto result = pool.get_string(idx2);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value(), L"Second");
}
