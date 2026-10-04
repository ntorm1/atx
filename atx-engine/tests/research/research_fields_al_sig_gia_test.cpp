// Lane AL-SIG: field gia_13f -- centered ranks, holdings returns and the holders' skill score in
// closed form, the panel's clock (anchor, staleness, no skip-back), and the planted-leak probe with
// teeth: a change planted in one quarter's holdings reaches the field at the first session whose
// previous-session mark passes that quarter's visibility and never earlier, and the same probe
// flags a same-session read.
#include <gtest/gtest.h>

#include <cmath>
#include <optional>
#include <vector>

#include "atx/engine/research/fields/gia_13f_field.hpp"
#include "research/research_fields_al_sig_support.hpp"

namespace fields = atx::engine::research::fields;
namespace support = atx::engine::research::fields::test;
using atx::f64;
using atx::i64;
using atx::u8;
using atx::usize;

namespace {

i64 day(i64 y, atx::u32 m, atx::u32 d) { return support::civil_day(y, m, d); }

i64 mark_of(i64 d) { return d * fields::kDayNs + fields::kSessionMarkNs; }

bool same_bits(f64 a, f64 b) { return support::same_bits(a, b); }

} // namespace

TEST(ResearchFieldsAlSigGia, CenteredRanksClosedForm) {
  const std::vector<f64> values{3.0, 1.0, 1.0, 2.0}; // average ranks 4, 1.5, 1.5, 3; m = 4
  EXPECT_EQ(fields::centered_ranks(values), (std::vector<f64>{0.375, -0.25, -0.25, 0.125}));
  EXPECT_TRUE(fields::centered_ranks(std::vector<f64>{}).empty());
  EXPECT_EQ(fields::centered_ranks(std::vector<f64>{5.0}), (std::vector<f64>{0.0}));
}

TEST(ResearchFieldsAlSigGia, HoldingsReturnsClosedForm) {
  // q-1: every price 10. Filer 1: 7 and 8 (1000 each); filer 2: 7 (3000) and 9 (100); filer 3: 7
  // (100) and 9 (1000). q: 7 at 12, 8 at 9, 9 unpriced (no holder): no return for 9.
  const auto prev = fields::quarter_positions({{1, 7, 100.0, 1000.0},
                                               {1, 8, 100.0, 1000.0},
                                               {2, 7, 300.0, 3000.0},
                                               {2, 9, 10.0, 100.0},
                                               {3, 7, 10.0, 100.0},
                                               {3, 9, 100.0, 1000.0}});
  const auto cur = fields::quarter_positions(
      {{1, 7, 100.0, 1200.0}, {2, 7, 300.0, 3600.0}, {1, 8, 100.0, 900.0}});
  const fields::GiaParams params{};
  const auto r = fields::holdings_returns(prev, cur, params);
  ASSERT_EQ(r.filer.size(), 3U);
  const f64 r7 = 1.0 * 12.0 / 10.0 - 1.0;
  const f64 r8 = 1.0 * 9.0 / 10.0 - 1.0;
  EXPECT_DOUBLE_EQ(r.filer[0], (1000.0 * r7 + 1000.0 * r8) / 2000.0);
  EXPECT_DOUBLE_EQ(r.filer[1], r7); // 9 has no return; 3000 of 3100 covered
  EXPECT_TRUE(std::isnan(r.filer[2])); // 100 of 1100 covered: below half
  const f64 sum = (1000.0 * r7 + 1000.0 * r8) + 3000.0 * r7 + 100.0 * r7;
  EXPECT_DOUBLE_EQ(r.aggregate, sum / 5100.0);
  // A price ratio outside [1/10, 10] gives no return (filer 1 then covers only 8).
  const auto jump = fields::quarter_positions({{1, 7, 100.0, 20000.0}, {1, 8, 100.0, 900.0}});
  const auto j = fields::holdings_returns(prev, jump, params);
  EXPECT_DOUBLE_EQ(j.filer[0], r8);
}

TEST(ResearchFieldsAlSigGia, HolderScoresClosedForm) {
  const auto p = fields::quarter_positions({{1, 7, 10.0, 100.0},
                                            {1, 8, 30.0, 300.0},
                                            {2, 7, 5.0, 50.0},
                                            {3, 8, 1.0, 10.0},
                                            {3, 9, 1.0, 0.0}});
  ASSERT_EQ(p.filers, (std::vector<i64>{1, 2, 3}));
  fields::GiaParams params;
  params.min_holders = 2;
  const std::vector<f64> u{0.25, -0.25, support::nan_value()}; // filer 3 has no skill
  const auto s = fields::holder_scores(p, u, params);
  ASSERT_EQ(s.sid, (std::vector<i64>{7, 8, 9}));
  // 7: w = 100/400 (u .25) and 50/50 (u -.25); 8: one skilled holder; 9: no skilled holder.
  EXPECT_DOUBLE_EQ(s.value[0], (0.25 * 0.25 + 1.0 * -0.25) / (0.25 + 1.0));
  EXPECT_TRUE(std::isnan(s.value[1]));
  EXPECT_TRUE(std::isnan(s.value[2]));
  params.min_holders = 1;
  EXPECT_DOUBLE_EQ(fields::holder_scores(p, u, params).value[1], 0.25);
}

TEST(ResearchFieldsAlSigGia, RowClockAnchorStalenessNoSkipBack) {
  fields::GiaPanel panel;
  panel.columns = 2;
  const f64 nan = support::nan_value();
  const i64 v0 = mark_of(day(2020, 11, 16));
  const i64 v1 = mark_of(day(2021, 2, 16));
  panel.period_day = {day(2020, 9, 30), day(2020, 12, 31), day(2021, 3, 31)};
  panel.visible_ns = {v0, v1, fields::kNeverNs};
  panel.value = {0.1, 0.2, nan, 7.0, nan, nan};
  panel.has = {1, 1, 1, 0, 0, 0};
  std::vector<f64> row(2);
  std::vector<u8> fresh(2);
  // Visible only strictly after V*(P): a mark equal to it sees nothing.
  fields::gia_row(panel, day(2020, 11, 17), v0, row, fresh);
  EXPECT_TRUE(std::isnan(row[0]) && std::isnan(row[1]));
  EXPECT_EQ(fresh, (std::vector<u8>{0, 0}));
  fields::gia_row(panel, day(2020, 11, 17), v0 + 1, row, fresh);
  EXPECT_EQ(row, (std::vector<f64>{0.1, 0.2}));
  // P1 visible: column 0 anchors on P1 and its NaN stays NaN (no skip-back to 0.1); column 1 has
  // no P1 position and keeps P0 (140 days old); P1's value 7.0 is never read for it.
  fields::gia_row(panel, day(2021, 2, 17), v1 + 1, row, fresh);
  EXPECT_TRUE(std::isnan(row[0]));
  EXPECT_EQ(row[1], 0.2);
  EXPECT_EQ(fresh, (std::vector<u8>{1, 1}));
  // 152 days after P0: stale for column 1.
  fields::gia_row(panel, day(2021, 3, 1), v1 + 1, row, fresh);
  EXPECT_TRUE(std::isnan(row[1]));
  EXPECT_EQ(fresh, (std::vector<u8>{1, 0}));
  // A quarter ending after the session is never read, whatever the mark.
  fields::gia_row(panel, day(2020, 9, 29), fields::kNeverNs - 1, row);
  EXPECT_TRUE(std::isnan(row[0]) && std::isnan(row[1]));
}

TEST(ResearchFieldsAlSigGia, AccumulatorVisibilityAndManagerFloor) {
  const std::vector<i64> ids{101, 102, 200};
  fields::GiaAccumulator acc(ids, fields::GiaParams{});
  const std::vector<fields::PositionRow> rows{{1, 101, 1.0, 10.0}, {2, 102, 1.0, 10.0}};
  acc.add(day(2020, 3, 31), 50, fields::quarter_positions(rows));
  acc.add(day(2020, 6, 30), 40, fields::quarter_positions(rows)); // an earlier V: V* keeps 50
  acc.add(day(2020, 9, 30), 90, std::nullopt);                    // not used
  const fields::GiaPanel &p = acc.panel();
  EXPECT_EQ(p.visible_ns, (std::vector<i64>{50, 50, fields::kNeverNs}));
  EXPECT_EQ(p.has, (std::vector<u8>{1, 1, 0, 1, 1, 0, 0, 0, 0}));
  for (const f64 v : p.value) {
    EXPECT_TRUE(std::isnan(v)); // two managers, no skill: no value
  }
  ASSERT_EQ(p.counts.size(), 3U);
  EXPECT_EQ(p.counts[1].filers_with_return, 2U);
  EXPECT_EQ(p.counts[1].managers_ranked, 0U);
  EXPECT_FALSE(p.counts[2].positions);
}

namespace {

// Six quarters of four managers holding 101..103 at common prices; the plant triples manager 1's
// value of 103 in quarter 4 (its weights move; the median price does not).
const std::vector<i64> kQuarters{day(2020, 3, 31), day(2020, 6, 30), day(2020, 9, 30),
                                 day(2020, 12, 31), day(2021, 3, 31), day(2021, 6, 30)};
constexpr usize kPlanted = 4;

i64 visible_of(usize q) { return mark_of(kQuarters[q] + 46); }

fields::GiaPanel probe_panel(bool planted) {
  fields::GiaParams params;
  params.skill_quarters = 2;
  params.skill_min_quarters = 1;
  params.min_holders = 1;
  fields::GiaAccumulator acc(std::vector<i64>{101, 102, 103}, params);
  for (usize q = 0; q < kQuarters.size(); ++q) {
    std::vector<fields::PositionRow> rows;
    for (i64 k = 1; k <= 4; ++k) {
      for (i64 s = 101; s <= 103; ++s) {
        const f64 price = 10.0 + static_cast<f64>(s - 100) + static_cast<f64>(q) *
                                                                   static_cast<f64>(s - 101);
        const f64 shares = 100.0 * static_cast<f64>(k + (s + static_cast<i64>(q)) % 3);
        f64 value = shares * price;
        if (planted && q == kPlanted && k == 1 && s == 103) {
          value *= 3.0;
        }
        rows.push_back({k, s, shares, value});
      }
    }
    acc.add(kQuarters[q], visible_of(q), fields::quarter_positions(std::move(rows)));
  }
  return acc.panel();
}

// The first session whose row differs between the two panels under `marks`.
std::optional<usize> first_changed(const fields::GiaPanel &a, const fields::GiaPanel &b,
                                   const std::vector<i64> &days, const std::vector<i64> &marks) {
  std::vector<f64> ra(a.columns);
  std::vector<f64> rb(b.columns);
  for (usize t = 0; t < days.size(); ++t) {
    fields::gia_row(a, days[t], marks[t], ra);
    fields::gia_row(b, days[t], marks[t], rb);
    for (usize j = 0; j < ra.size(); ++j) {
      if (!same_bits(ra[j], rb[j])) {
        return t;
      }
    }
  }
  return std::nullopt;
}

} // namespace

TEST(ResearchFieldsAlSigGia, PlantedLeakProbeHasTeeth) {
  const fields::GiaPanel base = probe_panel(false);
  const fields::GiaPanel planted = probe_panel(true);
  const std::vector<i64> days = support::consecutive_days(kQuarters[3], 200);
  const std::vector<i64> honest = support::honest_marks(days);
  const i64 visible = planted.visible_ns[kPlanted];
  ASSERT_EQ(visible, visible_of(kPlanted));
  // The house clock: the plant shows at the first session whose previous-session mark passes the
  // quarter's visibility, never before (rows up to there are bit-identical), and it does show (the
  // probe has data to catch).
  const auto changed = first_changed(base, planted, days, honest);
  ASSERT_TRUE(changed.has_value());
  EXPECT_GT(honest[*changed], visible);
  EXPECT_LE(honest[*changed - 1], visible);
  // The planted leak: reading at the session's own mark shows the plant one session early, which
  // the same probe condition flags.
  const auto leaked = first_changed(base, planted, days, support::leaky_marks(days));
  ASSERT_TRUE(leaked.has_value());
  EXPECT_LT(*leaked, *changed);
  EXPECT_LE(honest[*leaked], visible);
}
