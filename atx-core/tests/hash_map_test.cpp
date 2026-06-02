// hash_map_test.cpp — TDD tests for atx::core::container::HashMap and HashSet
//
// Covers: insert/overwrite, erase, contains true/false, at() found+NotFound,
// operator[], iteration order (all keys visited), reserve, string+int keys,
// HashSet insert/contains/erase/iteration.
//
// ODR safety: all helper types are suffixed with HM to avoid collisions with
// other translation units in the single atx-core-tests binary.

#include <atx/core/container/hash_map.hpp>

#include <algorithm>
#include <string>
#include <vector>

#include <gtest/gtest.h>

using namespace atx::core::container; // NOLINT(google-build-using-namespace)
using namespace atx::core;            // NOLINT(google-build-using-namespace)

// ============================================================
// HashMap — basic insertion and at()
// ============================================================

TEST(HashMap, InsertAndContains) {
    HashMap<std::string, int> m;
    m.insert_or_assign("AAPL", 42);
    ASSERT_TRUE(m.contains("AAPL"));
}

TEST(HashMap, AtFoundReturnsPointerToValue) {
    HashMap<std::string, int> m;
    m.insert_or_assign("AAPL", 42);
    auto result = m.at("AAPL");
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result.value(), 42);
}

TEST(HashMap, AtMissingReturnsNotFoundError) {
    HashMap<std::string, int> m;
    auto result = m.at("MSFT");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code(), ErrorCode::NotFound);
}

TEST(HashMap, AtConstMissingReturnsNotFoundError) {
    const HashMap<std::string, int> m;
    auto result = m.at("GOOG");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code(), ErrorCode::NotFound);
}

TEST(HashMap, AtConstFoundReturnsConstPointer) {
    HashMap<std::string, int> m;
    m.insert_or_assign("TSLA", 99);
    const HashMap<std::string, int>& cm = m;
    auto result = cm.at("TSLA");
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result.value(), 99);
}

// ============================================================
// HashMap — overwrite via insert_or_assign
// ============================================================

TEST(HashMap, InsertOrAssignOverwritesExistingValue) {
    HashMap<std::string, int> m;
    m.insert_or_assign("AAPL", 42);
    m.insert_or_assign("AAPL", 100);
    ASSERT_TRUE(m.contains("AAPL"));
    EXPECT_EQ(*m.at("AAPL").value(), 100);
}

// ============================================================
// HashMap — erase
// ============================================================

TEST(HashMap, EraseRemovesKey) {
    HashMap<std::string, int> m;
    m.insert_or_assign("AAPL", 42);
    m.erase("AAPL");
    EXPECT_FALSE(m.contains("AAPL"));
    EXPECT_FALSE(m.at("AAPL").has_value());
}

TEST(HashMap, EraseNonexistentKeyIsNoOp) {
    HashMap<std::string, int> m;
    m.insert_or_assign("AAPL", 42);
    m.erase("MSFT"); // must not crash or corrupt
    EXPECT_TRUE(m.contains("AAPL"));
    EXPECT_EQ(m.size(), 1U);
}

// ============================================================
// HashMap — size / empty / clear
// ============================================================

TEST(HashMap, SizeAndEmpty) {
    HashMap<std::string, int> m;
    EXPECT_TRUE(m.empty());
    EXPECT_EQ(m.size(), 0U);
    m.insert_or_assign("A", 1);
    EXPECT_FALSE(m.empty());
    EXPECT_EQ(m.size(), 1U);
    m.insert_or_assign("B", 2);
    EXPECT_EQ(m.size(), 2U);
}

TEST(HashMap, ClearEmptiesMap) {
    HashMap<std::string, int> m;
    m.insert_or_assign("A", 1);
    m.insert_or_assign("B", 2);
    m.clear();
    EXPECT_TRUE(m.empty());
    EXPECT_EQ(m.size(), 0U);
}

// ============================================================
// HashMap — operator[]
// ============================================================

TEST(HashMap, OperatorBracketDefaultInserts) {
    HashMap<std::string, int> m;
    m["X"] = 7;
    EXPECT_EQ(m["X"], 7);
}

TEST(HashMap, OperatorBracketCreatesDefaultOnMissingKey) {
    HashMap<std::string, int> m;
    // operator[] must default-insert (value = 0 for int)
    int val = m["NEW"];
    EXPECT_EQ(val, 0);
    EXPECT_TRUE(m.contains("NEW"));
}

// ============================================================
// HashMap — emplace
// ============================================================

TEST(HashMap, EmplaceInsertsNewKey) {
    HashMap<std::string, int> m;
    m.emplace("SPY", 500);
    ASSERT_TRUE(m.contains("SPY"));
    EXPECT_EQ(*m.at("SPY").value(), 500);
}

// ============================================================
// HashMap — find (raw iterator)
// ============================================================

TEST(HashMap, FindReturnsValidIteratorForPresentKey) {
    HashMap<std::string, int> m;
    m.insert_or_assign("QQQ", 300);
    auto it = m.find("QQQ");
    ASSERT_NE(it, m.end());
    EXPECT_EQ(it->second, 300);
}

TEST(HashMap, FindReturnsEndForMissingKey) {
    HashMap<std::string, int> m;
    EXPECT_EQ(m.find("NONE"), m.end());
}

// ============================================================
// HashMap — reserve (must not throw or corrupt)
// ============================================================

TEST(HashMap, ReserveAllowsInsertionWithoutRehash) {
    HashMap<int, int> m;
    m.reserve(1024U);
    for (int i = 0; i < 100; ++i) {
        m.insert_or_assign(i, i * 2);
    }
    EXPECT_EQ(m.size(), 100U);
    EXPECT_EQ(*m.at(42).value(), 84);
}

// ============================================================
// HashMap — iteration (all keys visited exactly once)
// ============================================================

TEST(HashMap, IterationVisitsAllKeys) {
    HashMap<std::string, int> m;
    m.insert_or_assign("A", 1);
    m.insert_or_assign("B", 2);
    m.insert_or_assign("C", 3);

    std::vector<std::string> keys;
    keys.reserve(m.size());
    for (const auto& [k, v] : m) {
        keys.push_back(k);
    }

    std::sort(keys.begin(), keys.end());
    ASSERT_EQ(keys.size(), 3U);
    EXPECT_EQ(keys[0], "A");
    EXPECT_EQ(keys[1], "B");
    EXPECT_EQ(keys[2], "C");
}

// ============================================================
// HashMap — integer keys
// ============================================================

TEST(HashMap, IntegerKeys) {
    HashMap<int, std::string> m;
    m.insert_or_assign(1, "one");
    m.insert_or_assign(2, "two");
    EXPECT_TRUE(m.contains(1));
    EXPECT_EQ(*m.at(1).value(), "one");
    EXPECT_FALSE(m.contains(3));
}

// ============================================================
// HashSet — insert, contains, erase
// ============================================================

TEST(HashSet, InsertAndContains) {
    HashSet<int> s;
    s.insert(7);
    EXPECT_TRUE(s.contains(7));
    EXPECT_FALSE(s.contains(8));
}

TEST(HashSet, EraseRemovesElement) {
    HashSet<int> s;
    s.insert(7);
    s.erase(7);
    EXPECT_FALSE(s.contains(7));
}

TEST(HashSet, EraseNonexistentIsNoOp) {
    HashSet<int> s;
    s.insert(7);
    s.erase(99); // must not crash
    EXPECT_TRUE(s.contains(7));
}

TEST(HashSet, SizeAndEmpty) {
    HashSet<int> s;
    EXPECT_TRUE(s.empty());
    EXPECT_EQ(s.size(), 0U);
    s.insert(1);
    s.insert(2);
    EXPECT_EQ(s.size(), 2U);
    EXPECT_FALSE(s.empty());
}

TEST(HashSet, ClearEmptiesSet) {
    HashSet<int> s;
    s.insert(1);
    s.insert(2);
    s.clear();
    EXPECT_TRUE(s.empty());
}

TEST(HashSet, ReserveAndInsert) {
    HashSet<int> s;
    s.reserve(256U);
    for (int i = 0; i < 50; ++i) {
        s.insert(i);
    }
    EXPECT_EQ(s.size(), 50U);
    EXPECT_TRUE(s.contains(49));
}

TEST(HashSet, IterationVisitsAllElements) {
    HashSet<int> s;
    s.insert(10);
    s.insert(20);
    s.insert(30);

    std::vector<int> vals;
    vals.reserve(s.size());
    for (const auto& v : s) {
        vals.push_back(v);
    }

    std::sort(vals.begin(), vals.end());
    ASSERT_EQ(vals.size(), 3U);
    EXPECT_EQ(vals[0], 10);
    EXPECT_EQ(vals[1], 20);
    EXPECT_EQ(vals[2], 30);
}

TEST(HashSet, StringKeys) {
    HashSet<std::string> s;
    s.insert("hello");
    s.insert("world");
    EXPECT_TRUE(s.contains("hello"));
    EXPECT_FALSE(s.contains("foo"));
    s.erase("hello");
    EXPECT_FALSE(s.contains("hello"));
    EXPECT_TRUE(s.contains("world"));
}
