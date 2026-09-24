// R21-3: unit test of the `panel --asof-field` mechanics (CSV contract + strict
// as-of join). Synthetic by design — it pins mechanics, it is not evidence.

#include <gtest/gtest.h>

#include <cmath>
#include <string>
#include <vector>

#include "asof_field.hpp"

namespace {

using atx::impl::AsofTable;
using atx::impl::build_asof_column;
using atx::impl::is_dsl_identifier;
using atx::impl::parse_asof_csv;

constexpr atx::i64 kDayNs = 86400LL * 1000000000LL;
// 2024-01-02 = day 19724 since 1970-01-01.
constexpr atx::i64 kJan2 = 19724;

std::vector<atx::i64> sessions(std::initializer_list<atx::i64> days) {
    std::vector<atx::i64> out;
    for (const auto d : days) out.push_back(d * kDayNs);
    return out;
}

TEST(AsofField, StrictInequalityNaNBeforeFirstRowAndUnknownIdCounted) {
    // Unsorted on purpose; id 999 is not on the panel axis.
    const std::string csv =
        "security_id,available_at,value\r\n"
        "7,2024-01-04,2.5\r\n"
        "999,2024-01-02,1\r\n"
        "7,2024-01-02,1.5\r\n"
        "11,2024-01-03,nan\r\n";
    auto table = parse_asof_csv(csv);
    ASSERT_TRUE(table.has_value()) << table.error().to_string();
    ASSERT_EQ(table->rows.size(), 4U);

    const auto keys = sessions({kJan2, kJan2 + 1, kJan2 + 2, kJan2 + 3});
    const std::vector<std::string> ids{"7", "11"};
    auto col = build_asof_column(*table, keys, ids, 0);
    ASSERT_TRUE(col.has_value()) << col.error().to_string();
    EXPECT_EQ(col->rows_ignored_unknown_id, 1U);
    EXPECT_EQ(col->rows_matched, 3U);
    const auto at = [&](atx::usize d, atx::usize n) { return col->values[d * 2 + n]; };
    // id 7: row available 01-02 is NOT visible on the 01-02 session (strict <) ...
    EXPECT_TRUE(std::isnan(at(0, 0)));
    // ... and is visible from the next session on.
    EXPECT_DOUBLE_EQ(at(1, 0), 1.5);
    EXPECT_DOUBLE_EQ(at(2, 0), 1.5);   // 01-04 row not yet visible on 01-04
    EXPECT_DOUBLE_EQ(at(3, 0), 2.5);
    // id 11: NaN before its first row, and a NaN-valued row stays NaN.
    for (atx::usize d = 0; d < 4; ++d) EXPECT_TRUE(std::isnan(at(d, 1)));
}

TEST(AsofField, MaxStaleCapTurnsOldValuesNaN) {
    auto table = parse_asof_csv("security_id,available_at,value\n5,2024-01-02,3\n");
    ASSERT_TRUE(table.has_value());
    const auto keys = sessions({kJan2 + 1, kJan2 + 2, kJan2 + 3});
    const std::vector<std::string> ids{"5"};
    auto capped = build_asof_column(*table, keys, ids, 2);
    ASSERT_TRUE(capped.has_value());
    EXPECT_DOUBLE_EQ(capped->values[0], 3.0);        // age 1
    EXPECT_DOUBLE_EQ(capped->values[1], 3.0);        // age 2 == cap, still valid
    EXPECT_TRUE(std::isnan(capped->values[2]));      // age 3 > cap
    auto uncapped = build_asof_column(*table, keys, ids, 0);
    ASSERT_TRUE(uncapped.has_value());
    EXPECT_DOUBLE_EQ(uncapped->values[2], 3.0);
}

TEST(AsofField, RejectsDuplicateBadHeaderAndBadRows) {
    EXPECT_FALSE(parse_asof_csv("security_id,available_at,value\n"
                                "5,2024-01-02,3\n5,2024-01-02,4\n")
                     .has_value());
    EXPECT_FALSE(parse_asof_csv("security_id,date,value\n5,2024-01-02,3\n").has_value());
    EXPECT_FALSE(parse_asof_csv("").has_value());
    EXPECT_FALSE(parse_asof_csv("security_id,available_at,value\n0,2024-01-02,3\n").has_value());
    EXPECT_FALSE(parse_asof_csv("security_id,available_at,value\n5,2024/01/02,3\n").has_value());
    EXPECT_FALSE(parse_asof_csv("security_id,available_at,value\n5,2024-01-02,x\n").has_value());
    EXPECT_FALSE(parse_asof_csv("security_id,available_at,value\n5,2024-01-02\n").has_value());
    EXPECT_FALSE(parse_asof_csv("security_id,available_at,value\n5,2024-01-02,inf\n").has_value());
    // Empty value is NaN, not an error; header-only file is an empty table.
    auto empty_value = parse_asof_csv("security_id,available_at,value\n5,2024-01-02,\n");
    ASSERT_TRUE(empty_value.has_value());
    EXPECT_TRUE(std::isnan(empty_value->rows[0].value));
    EXPECT_TRUE(parse_asof_csv("security_id,available_at,value\n").has_value());
}

TEST(AsofField, DslIdentifierGrammar) {
    EXPECT_TRUE(is_dsl_identifier("si_shares"));
    EXPECT_TRUE(is_dsl_identifier("_x9"));
    EXPECT_FALSE(is_dsl_identifier(""));
    EXPECT_FALSE(is_dsl_identifier("9x"));
    EXPECT_FALSE(is_dsl_identifier("si-dtc"));
    EXPECT_FALSE(is_dsl_identifier("a.b"));
}

} // namespace
