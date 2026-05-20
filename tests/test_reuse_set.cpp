#include <gtest/gtest.h>
#include <string>

#include "utils/ReuseSet.h"

using namespace usbipdcpp;

TEST(ReuseSet, InsertAndContains) {
    ReuseSet<int> set;
    EXPECT_TRUE(set.insert(1));
    EXPECT_TRUE(set.insert(2));
    EXPECT_TRUE(set.contains(1));
    EXPECT_TRUE(set.contains(2));
    EXPECT_FALSE(set.contains(3));
    EXPECT_EQ(set.size(), 2u);
}

TEST(ReuseSet, InsertDuplicate) {
    ReuseSet<int> set;
    EXPECT_TRUE(set.insert(1));
    EXPECT_FALSE(set.insert(1));
    EXPECT_EQ(set.size(), 1u);
}

TEST(ReuseSet, EraseAndReuse) {
    ReuseSet<int> set;
    set.reserve(4);
    set.insert(1);
    set.insert(2);
    EXPECT_TRUE(set.erase(1));
    EXPECT_FALSE(set.contains(1));
    EXPECT_EQ(set.size(), 1u);
    EXPECT_TRUE(set.insert(3));  // 复用空槽
}

TEST(ReuseSet, EraseNotFound) {
    ReuseSet<int> set;
    EXPECT_FALSE(set.erase(42));
    set.insert(1);
    EXPECT_FALSE(set.erase(42));
    EXPECT_EQ(set.size(), 1u);
}

TEST(ReuseSet, Empty) {
    ReuseSet<int> set;
    EXPECT_TRUE(set.empty());
    EXPECT_FALSE(set.contains(1));
    EXPECT_FALSE(set.erase(1));
}

TEST(ReuseSet, ForEach) {
    ReuseSet<int> set;
    set.insert(1);
    set.insert(2);
    set.erase(1);
    set.insert(3);

    int sum = 0;
    set.for_each([&](int k) { sum += k; });
    EXPECT_EQ(sum, 5);  // 2 + 3
}

TEST(ReuseSet, ClearAndReinsert) {
    ReuseSet<int> set;
    for (int i = 0; i < 5; i++) set.insert(i);
    EXPECT_EQ(set.size(), 5u);
    set.clear();
    EXPECT_EQ(set.size(), 0u);
    EXPECT_TRUE(set.empty());
    EXPECT_TRUE(set.insert(1));
    EXPECT_TRUE(set.contains(1));
}

TEST(ReuseSet, ConstForEach) {
    ReuseSet<int> set;
    set.insert(1);
    set.insert(2);
    const auto &cset = set;
    int count = 0;
    cset.for_each([&](int) { count++; });
    EXPECT_EQ(count, 2);
}

TEST(ReuseSet, Reserve) {
    ReuseSet<int> set;
    set.reserve(64);
    for (int i = 0; i < 32; i++) set.insert(i);
    EXPECT_EQ(set.size(), 32u);
    for (int i = 0; i < 32; i++) EXPECT_TRUE(set.contains(i));
}

TEST(ReuseSet, StringKey) {
    ReuseSet<std::string> set;
    set.insert("hello");
    set.insert("world");
    EXPECT_TRUE(set.contains("hello"));
    EXPECT_FALSE(set.contains("foo"));
    set.erase("hello");
    EXPECT_FALSE(set.contains("hello"));
}

TEST(ReuseSet, ManyInserts) {
    ReuseSet<int> set;
    for (int i = 0; i < 100; i++) set.insert(i);
    EXPECT_EQ(set.size(), 100u);
    for (int i = 0; i < 100; i++) EXPECT_TRUE(set.contains(i));
}
