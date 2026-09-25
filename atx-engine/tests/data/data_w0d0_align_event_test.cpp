// W0-D0 — event columns, staleness caps and the coverage guard in align_onto (D-05).
//
// Suite: DataAlignEvent_W0d0
//
// The legacy as-of join forward-fills every plug column without limit: a dividend
// is counted again on each following session the plug has no row for, and a split
// factor freezes past the plug's last row. Fixtures are tiny synthetic Datasets.

#include <cmath>
#include <cstdio>
#include <limits>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "atx/core/error.hpp"
#include "atx/core/types.hpp"

#include "atx/engine/data/align.hpp"
#include "atx/engine/data/corporate_actions.hpp"
#include "atx/engine/data/dataset.hpp"
#include "atx/engine/data/dataset_schema.hpp"

namespace atx_test_w0_d0_align_event {

using atx::core::ErrorCode;
using atx::engine::data::align_onto;
using atx::engine::data::AlignColumnRule;
using atx::engine::data::AlignedView;
using atx::engine::data::AlignOptions;
using atx::engine::data::ColumnDType;
using atx::engine::data::corp_action_align_options;
using atx::engine::data::CorpAlignRule;
using atx::engine::data::Dataset;
using atx::engine::data::DatasetProvenance;
using atx::engine::data::DatasetSchema;
using atx::engine::data::DateKey;
using atx::engine::data::InstKey;
using atx::engine::data::kAlignEventSession;
using atx::engine::data::kAlignUnboundedStaleness;
using atx::engine::data::kCorpActionColumnCount;
using atx::engine::data::kCorpMaxStaleSessions;
using atx::engine::data::Role;

namespace {

// Canonical price axis: one instrument, the given dates, a dummy close column.
[[nodiscard]] Dataset price(const std::vector<DateKey> &dates) {
  DatasetSchema s;
  s.columns = {"close"};
  s.dtypes = {ColumnDType::F64};
  s.role = Role::Price;
  std::vector<std::vector<atx::f64>> cols = {std::vector<atx::f64>(dates.size(), 10.0)};
  auto r = Dataset::create(std::move(s), dates, {InstKey{0}}, std::move(cols), {},
                           DatasetProvenance{"test:price", ""});
  EXPECT_TRUE(r.has_value());
  return std::move(r).value();
}

// Plug: one instrument, columns {factor, dividend} on the given (sparse) dates.
[[nodiscard]] Dataset plug(const std::vector<DateKey> &dates, const std::vector<atx::f64> &factor,
                           const std::vector<atx::f64> &dividend) {
  DatasetSchema s;
  s.columns = {"factor", "dividend"};
  s.dtypes = {ColumnDType::F64, ColumnDType::F64};
  s.role = Role::Reference;
  auto r = Dataset::create(std::move(s), dates, {InstKey{0}}, {factor, dividend}, {},
                           DatasetProvenance{"test:plug", ""});
  EXPECT_TRUE(r.has_value());
  return std::move(r).value();
}

[[nodiscard]] AlignOptions event_options(atx::usize factor_cap) {
  AlignOptions o;
  o.column_rules = {AlignColumnRule{factor_cap}, AlignColumnRule{kAlignEventSession}};
  return o;
}

} // namespace

// A dividend joins its own session once; the legacy join counted it again on every
// following session the plug had no row for.
TEST(DataAlignEvent_W0d0, DividendJoinsItsOwnSessionOnlyOnce) {
  const Dataset px = price({0, 1, 2, 3, 4, 5, 6, 7, 8, 9});
  // Plug rows on 0, 3 (0.5 dividend) and 7 only.
  const Dataset pl = plug({0, 3, 7}, {1.0, 1.0, 1.0}, {0.0, 0.5, 0.0});
  auto legacy = align_onto(px, pl);
  auto fixed = align_onto(px, pl, event_options(kAlignUnboundedStaleness));
  ASSERT_TRUE(legacy.has_value() && fixed.has_value());

  atx::f64 legacy_total = 0.0;
  atx::f64 fixed_total = 0.0;
  for (atx::usize d = 0; d < 10; ++d) {
    const atx::f64 lv = legacy->aligned_columns[1][d];
    const atx::f64 fv = fixed->aligned_columns[1][d];
    legacy_total += std::isnan(lv) ? 0.0 : lv;
    fixed_total += std::isnan(fv) ? 0.0 : fv;
  }
  EXPECT_DOUBLE_EQ(legacy_total, 2.0); // 0.5 on sessions 3, 4, 5, 6: counted 4 times
  EXPECT_DOUBLE_EQ(fixed_total, 0.5);  // counted once
  EXPECT_DOUBLE_EQ(fixed->aligned_columns[1][3], 0.5);
  EXPECT_TRUE(std::isnan(fixed->aligned_columns[1][4]));
  EXPECT_DOUBLE_EQ(fixed->aligned_columns[1][7], 0.0);
  std::printf("[align-event] dividend total legacy=%.2f event-join=%.2f (true 0.50)\n",
              legacy_total, fixed_total);
}

// An event dated between two canonical sessions (no price that day) lands on the
// first canonical session after it, once.
TEST(DataAlignEvent_W0d0, OffAxisEventLandsOnTheNextSessionOnce) {
  const Dataset px = price({10, 12, 14, 16});
  const Dataset pl = plug({10, 13}, {1.0, 1.0}, {0.0, 0.25});
  auto fixed = align_onto(px, pl, event_options(kAlignUnboundedStaleness));
  ASSERT_TRUE(fixed.has_value());
  EXPECT_DOUBLE_EQ(fixed->aligned_columns[1][2], 0.25); // session 14
  EXPECT_TRUE(std::isnan(fixed->aligned_columns[1][3]));
}

// A capped as-of column goes NaN once its row is staler than the cap, instead of
// freezing the last value.
TEST(DataAlignEvent_W0d0, StalenessCapStopsAFrozenFactor) {
  const Dataset px = price({0, 1, 2, 3, 4, 5, 6, 7});
  const Dataset pl = plug({0, 1, 2, 3}, {0.5, 0.5, 0.5, 0.5}, {0.0, 0.0, 0.0, 0.0});
  auto legacy = align_onto(px, pl);
  auto capped = align_onto(px, pl, event_options(2));
  ASSERT_TRUE(legacy.has_value() && capped.has_value());
  for (atx::usize d = 0; d < 8; ++d) {
    EXPECT_DOUBLE_EQ(legacy->aligned_columns[0][d], 0.5) << d; // frozen forever
  }
  EXPECT_DOUBLE_EQ(capped->aligned_columns[0][5], 0.5); // staleness 2 (row 3)
  EXPECT_TRUE(std::isnan(capped->aligned_columns[0][6])); // staleness 3 > 2
  EXPECT_TRUE(std::isnan(capped->aligned_columns[0][7]));
}

// A row older than the first canonical date never passes a finite cap (its
// staleness in canonical sessions is unknown) — fail closed.
TEST(DataAlignEvent_W0d0, PreAxisRowNeverPassesAFiniteCap) {
  const Dataset px = price({100, 101});
  const Dataset pl = plug({50}, {0.5}, {1.0});
  auto capped = align_onto(px, pl, event_options(kCorpMaxStaleSessions));
  auto legacy = align_onto(px, pl);
  ASSERT_TRUE(capped.has_value() && legacy.has_value());
  EXPECT_TRUE(std::isnan(capped->aligned_columns[0][0]));
  EXPECT_TRUE(std::isnan(capped->aligned_columns[1][0]));
  EXPECT_DOUBLE_EQ(legacy->aligned_columns[1][1], 1.0); // legacy: a 50-day-old dividend
}

// Price dates past the plug's coverage fail when the guard is on.
TEST(DataAlignEvent_W0d0, PriceDatesPastCoverageFail) {
  const Dataset px = price({0, 1, 2, 3});
  const Dataset short_plug = plug({0, 1, 2}, {1.0, 1.0, 1.0}, {0.0, 0.0, 0.0});
  const Dataset full_plug = plug({0, 3}, {1.0, 1.0}, {0.0, 0.0});
  AlignOptions guarded = event_options(kCorpMaxStaleSessions);
  guarded.require_coverage = true;
  auto bad = align_onto(px, short_plug, guarded);
  ASSERT_FALSE(bad.has_value());
  EXPECT_EQ(bad.error().code(), ErrorCode::OutOfRange);
  EXPECT_TRUE(align_onto(px, full_plug, guarded).has_value());
  EXPECT_TRUE(align_onto(px, short_plug).has_value()) << "legacy join has no guard";
}

// The corporate-action preset: dividend is the event column, the rest are capped,
// coverage is required; the legacy preset is the unlimited join.
TEST(DataAlignEvent_W0d0, CorpActionPresetMarksDividendAsEvent) {
  const AlignOptions v2 = corp_action_align_options();
  ASSERT_EQ(v2.column_rules.size(), kCorpActionColumnCount);
  EXPECT_TRUE(v2.require_coverage);
  for (atx::usize c = 0; c < kCorpActionColumnCount; ++c) {
    const atx::usize expect = (c == 1) ? kAlignEventSession : kCorpMaxStaleSessions;
    EXPECT_EQ(v2.column_rules[c].max_stale_sessions, expect) << c;
  }
  const AlignOptions v1 = corp_action_align_options(CorpAlignRule::AsOfForwardFillV1);
  EXPECT_TRUE(v1.column_rules.empty());
  EXPECT_FALSE(v1.require_coverage);
}

// Rule count must match the plug; empty options are bit-identical to the legacy
// two-argument overload.
TEST(DataAlignEvent_W0d0, RuleCountValidatedAndEmptyOptionsAreLegacy) {
  const Dataset px = price({0, 1, 2, 3, 4});
  const Dataset pl = plug({0, 2}, {0.5, 1.0}, {0.0, 0.3});
  AlignOptions wrong;
  wrong.column_rules = {AlignColumnRule{}};
  auto bad = align_onto(px, pl, wrong);
  ASSERT_FALSE(bad.has_value());
  EXPECT_EQ(bad.error().code(), ErrorCode::InvalidArgument);
  auto a = align_onto(px, pl);
  auto b = align_onto(px, pl, AlignOptions{});
  ASSERT_TRUE(a.has_value() && b.has_value());
  for (atx::usize c = 0; c < 2; ++c) {
    for (atx::usize k = 0; k < 5; ++k) {
      const atx::f64 x = a->aligned_columns[c][k];
      const atx::f64 y = b->aligned_columns[c][k];
      EXPECT_TRUE((std::isnan(x) && std::isnan(y)) || x == y);
    }
  }
}

} // namespace atx_test_w0_d0_align_event
