// Lane AL-SIG (ruling P18): the pinned stage and the 13F stage reader -- quarter calendar, the
// effective-filing rule, quarter visibility and the reader-side seal, the row screen, quarter
// positions, the share-basis rule, and the sealed-part probe (with teeth: a mis-pinned part that
// is opened is refused, so a part the seal lets through cannot pass unseen).
#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "atx/engine/research/fields/sources/stage.hpp"
#include "atx/engine/research/fields/sources/thirteenf.hpp"
#include "research/research_fields_al_sig_support.hpp"

namespace fields = atx::engine::research::fields;
namespace support = atx::engine::research::fields::test;
using atx::f64;
using atx::i64;
using atx::u8;
using atx::usize;

namespace {

i64 day(i64 y, atx::u32 m, atx::u32 d) { return support::civil_day(y, m, d); }

// 46 h after a filing day's midnight (the stage clock 13f-filed-plus-46h-v1).
i64 filed_plus_46h(i64 filing_day) {
  return filing_day * fields::kDayNs + 46LL * 3'600'000'000'000LL;
}

fields::ThirteenfFiling filing(std::string accession, i64 filer, std::string form,
                               std::string amend, i64 period, i64 filed,
                               std::string part = "2023q3") {
  return fields::ThirteenfFiling{std::move(accession), std::move(form), std::move(amend),
                                 std::move(part),      filer,          period,
                                 filed,                period + 45,    filed_plus_46h(filed)};
}

} // namespace

TEST(ResearchFieldsAlSigThirteenf, QuarterCalendar) {
  EXPECT_EQ(fields::quarter_end_on_or_after(day(2021, 1, 4)), day(2021, 3, 31));
  EXPECT_EQ(fields::quarter_end_on_or_after(day(2021, 3, 31)), day(2021, 3, 31));
  EXPECT_EQ(fields::quarter_end_on_or_after(day(2020, 12, 1)), day(2020, 12, 31));
  EXPECT_EQ(fields::previous_quarter_end(day(2021, 3, 31)), day(2020, 12, 31));
  EXPECT_EQ(fields::previous_quarter_end(day(2020, 12, 31)), day(2020, 9, 30));
  EXPECT_EQ(fields::next_quarter_end(day(2020, 12, 31)), day(2021, 3, 31));
  // Role 2021-01-04..2021-12-31: q0 = the quarter end on or after 2020-08-07, 8 quarters of
  // history before it, then every quarter end before the last session.
  const auto q = fields::thirteenf_quarters(day(2021, 1, 4), day(2021, 12, 31), 8);
  ASSERT_EQ(q.size(), 13U);
  EXPECT_EQ(q.front(), day(2018, 9, 30));
  EXPECT_EQ(q[8], day(2020, 9, 30));
  EXPECT_EQ(q.back(), day(2021, 9, 30));
  EXPECT_EQ(fields::thirteenf_quarters(day(2021, 1, 4), day(2021, 12, 31), 0).front(),
            day(2020, 9, 30));
  // Parts: the filing quarter that begins on or after the seal is sealed; other shapes are not
  // dated.
  EXPECT_FALSE(fields::thirteenf_part_is_sealed("2023q4"));
  EXPECT_TRUE(fields::thirteenf_part_is_sealed("2024q1"));
  EXPECT_TRUE(fields::thirteenf_part_is_sealed("2025q3"));
  EXPECT_FALSE(fields::thirteenf_part_is_sealed("2024Q1"));
  EXPECT_FALSE(fields::thirteenf_part_is_sealed("2024q5"));
  EXPECT_FALSE(fields::thirteenf_part_is_sealed("20x4q1"));
  EXPECT_FALSE(fields::thirteenf_part_is_sealed("2024q1x"));
}

TEST(ResearchFieldsAlSigThirteenf, EffectiveFilingsRule) {
  const i64 p = day(2023, 6, 30);
  const std::vector<fields::ThirteenfFiling> rows{
      filing("acc-1", 1, "13F-HR", "", p, p + 10),               // a base, superseded
      filing("acc-2", 1, "13F-HR/A", "RESTATEMENT", p, p + 12),  // the latest base
      filing("acc-3", 1, "13F-HR/A", "NEW HOLDINGS", p, p + 13), // after the base
      filing("acc-0", 1, "13F-HR/A", "NEW HOLDINGS", p, p + 11), // before the base
      filing("acc-4", 1, "13F-HR/A", "", p, p + 14),             // untyped amendment
      filing("b-1", 2, "13F-HR/A", "NEW HOLDINGS", p, p + 10),   // no base at all
      filing("c-2", 3, "13F-HR", "", p, p + 10),                 // same day: accession order
      filing("c-1", 3, "13F-HR", "", p, p + 10)};
  const std::vector<u8> got = fields::effective_filings(rows);
  EXPECT_EQ(got, (std::vector<u8>{0, 1, 1, 0, 0, 0, 1, 0}));
  EXPECT_TRUE(fields::effective_filings({}).empty());
}

TEST(ResearchFieldsAlSigThirteenf, IndexVisibilityDeadlineAndSeal) {
  const i64 p0 = day(2023, 3, 31);
  const i64 p1 = day(2023, 6, 30);
  const i64 p2 = day(2023, 9, 30);
  const i64 p3 = day(2023, 12, 31); // filed in 2024: available after the seal
  std::vector<fields::ThirteenfFiling> filings{
      filing("a-0", 1, "13F-HR", "", p0, p0 + 20), // a period outside the quarters: not kept
      filing("a-1", 1, "13F-HR", "", p1, p1 + 20),
      filing("n-1", 9, "13F-NT", "", p1, p1 + 44),              // a notice moves V(P)
      filing("a-1r", 1, "13F-HR/A", "RESTATEMENT", p1, p1 + 60), // after the deadline
      filing("a-2", 1, "13F-HR", "", p2, p2 + 20),
      filing("a-3", 1, "13F-HR", "", p3, p3 + 20, "2024q1")};
  const std::vector<i64> quarters{p1, p2, p3};
  const std::vector<fields::UnitFactorRow> factors{{"a", support::nan_value()}, {"a", 2.0},
                                                   {"b", 0.0}, {"c", 0.5}};
  const std::vector<fields::CusipMapRow> cusips{{p1, "X", 11}, {p1, "X", 99}, {p2, "X", 12},
                                                {p0, "X", 13}};
  const fields::ThirteenfIndex index =
      fields::index_thirteenf(quarters, std::move(filings), factors, cusips);
  ASSERT_EQ(index.filings.size(), 5U);
  ASSERT_EQ(index.quarters.size(), 3U);
  const auto &q1 = index.quarters[0];
  EXPECT_TRUE(q1.has_filing);
  EXPECT_FALSE(q1.sealed);
  EXPECT_EQ(q1.visible_ns, filed_plus_46h(p1 + 44)); // the notice: last filing by the deadline
  ASSERT_EQ(q1.effective.size(), 1U);
  EXPECT_EQ(index.filings[q1.effective[0]].accession, "a-1");
  EXPECT_EQ(index.quarters[1].visible_ns, filed_plus_46h(p2 + 20));
  const auto &q3 = index.quarters[2];
  EXPECT_TRUE(q3.has_filing);
  EXPECT_TRUE(q3.sealed);
  EXPECT_EQ(q3.visible_ns, fields::kNeverNs);
  EXPECT_EQ(index.counts.filings_after_deadline_ignored, 1U);
  EXPECT_EQ(index.counts.quarters_sealed, 1U);
  EXPECT_EQ(index.counts.effective_filings, 3U);
  EXPECT_EQ(index.unit_factor_of("a"), 1.0); // the first row wins, and NaN reads as 1
  EXPECT_EQ(index.unit_factor_of("b"), 1.0); // not positive
  EXPECT_EQ(index.unit_factor_of("c"), 0.5);
  EXPECT_EQ(index.unit_factor_of("zzz"), 1.0); // unlisted
  using Map = std::vector<std::pair<std::string, i64>>;
  EXPECT_EQ(q1.cusip_sid, (Map{{"X", 11}})); // the first map row of a CUSIP wins
  EXPECT_EQ(index.quarters[1].cusip_sid, (Map{{"X", 12}}));
  EXPECT_TRUE(q3.cusip_sid.empty());
  const auto checks = fields::thirteenf_checks(index);
  EXPECT_EQ(checks.at("quarters_sealed"), 1);
  EXPECT_TRUE(checks.at("quarter_visible_at").at("2023-12-31").is_null());
}

TEST(ResearchFieldsAlSigThirteenf, RowScreen) {
  fields::QuarterFilers filers;
  filers.accession = {"a", "b"};
  filers.filer = {1, 2};
  filers.factor = {1.0, 0.001};
  fields::ThirteenfQuarter quarter;
  quarter.cusip_sid = {{"C1", 101}, {"C2", 102}};
  const f64 nan = support::nan_value();
  const auto screen = [&](fields::HoldingRow row) {
    return fields::screen_holding(filers, quarter, row);
  };
  const auto kept = screen({"a", "C1", "SH", "", 10.0, 100.0});
  ASSERT_TRUE(kept.has_value());
  EXPECT_EQ(kept->filer, 1);
  EXPECT_EQ(kept->sid, 101);
  EXPECT_EQ(kept->shares, 10.0);
  EXPECT_EQ(kept->value, 100.0);
  const auto scaled = screen({"b", "C2", "SH", "", 5.0, 50000.0});
  ASSERT_TRUE(scaled.has_value());
  EXPECT_EQ(scaled->value, 50000.0 * 0.001); // value x the filing's unit factor
  const auto unmapped = screen({"a", "C9", "SH", "", 1.0, 1.0});
  ASSERT_TRUE(unmapped.has_value());
  EXPECT_EQ(unmapped->sid, -1);
  EXPECT_FALSE(screen({"z", "C1", "SH", "", 1.0, 1.0}).has_value());     // not effective
  EXPECT_FALSE(screen({"a", "C1", "PRN", "", 1.0, 1.0}).has_value());    // principal amount
  EXPECT_FALSE(screen({"a", "C1", "SH", "CALL", 1.0, 1.0}).has_value()); // an option
  EXPECT_FALSE(screen({"a", "C1", "SH", "", 0.0, 1.0}).has_value());
  EXPECT_FALSE(screen({"a", "C1", "SH", "", nan, 1.0}).has_value());
  EXPECT_FALSE(screen({"a", "C1", "SH", "", 1.0, nan}).has_value());
  EXPECT_FALSE(screen({"a", "C1", "SH", "", 1.0, -1.0}).has_value());
  EXPECT_TRUE(screen({"a", "C1", "SH", "", 1.0, 0.0}).has_value()); // a zero value is kept
}

TEST(ResearchFieldsAlSigThirteenf, QuarterPositionsClosedForm) {
  std::vector<fields::PositionRow> rows{
      {1, 101, 10.0, 100.0}, {2, 101, 20.0, 220.0}, {3, 101, 30.0, 300.0},
      {3, 101, 1.0, 500.0},  // implied 500 vs the median 10.5: a price outlier
      {1, 102, 5.0, 0.0},    // unpriced: kept with value 0
      {1, -1, 7.0, 70.0},    // unmapped
      {2, 102, 4.0, 40.0},   {2, 102, 6.0, 66.0}}; // one pair, summed
  const fields::QuarterPositions p = fields::quarter_positions(std::move(rows));
  EXPECT_EQ(p.rows_mapped, 7U);
  EXPECT_EQ(p.rows_price_outlier, 1U);
  EXPECT_EQ(p.filers, (std::vector<i64>{1, 2, 3}));
  EXPECT_EQ(p.filer_value, (std::vector<f64>{100.0, 326.0, 300.0}));
  EXPECT_EQ(p.pair_filer, (std::vector<i64>{1, 1, 2, 2, 3}));
  EXPECT_EQ(p.pair_sid, (std::vector<i64>{101, 102, 101, 102, 101}));
  EXPECT_EQ(p.pair_shares, (std::vector<f64>{10.0, 5.0, 20.0, 10.0, 30.0}));
  EXPECT_EQ(p.pair_value, (std::vector<f64>{100.0, 0.0, 220.0, 106.0, 300.0}));
  EXPECT_EQ(p.price_sid, (std::vector<i64>{101, 102}));
  EXPECT_EQ(fields::price_of(p, 101), 10.0); // median of {10, 10, 11} after the screen
  EXPECT_EQ(fields::price_of(p, 102), 10.5); // median of {10, 11}
  EXPECT_TRUE(std::isnan(fields::price_of(p, 103)));
  const fields::QuarterPositions empty = fields::quarter_positions({});
  EXPECT_TRUE(empty.filers.empty());
  EXPECT_EQ(empty.rows_mapped, 0U);
}

TEST(ResearchFieldsAlSigThirteenf, BasisFactorsDetectASplit) {
  std::vector<fields::PositionRow> before;
  std::vector<fields::PositionRow> after;
  for (i64 k = 1; k <= 5; ++k) {
    before.push_back({k, 7, 100.0, 2000.0}); // price 20
    after.push_back({k, 7, 200.0, 2000.0});  // 2:1 split: price 10
    before.push_back({k, 9, 100.0, 1000.0}); // price 10
    after.push_back({k, 9, 200.0, 2000.0});  // shares doubled, price unchanged: a trade
    if (k <= 4) {
      before.push_back({k, 8, 100.0, 1000.0});
      after.push_back({k, 8, 200.0, 1000.0}); // a split seen by 4 holders only
    }
  }
  const auto prev = fields::quarter_positions(before);
  const auto cur = fields::quarter_positions(after);
  const std::vector<i64> sids{7, 8, 9, 10};
  EXPECT_EQ(fields::basis_factors(prev, cur, sids), (std::vector<f64>{2.0, 1.0, 1.0, 1.0}));
}

TEST(ResearchFieldsAlSigThirteenf, StagePinAndContract) {
  const fields::StageRequest request = support::al_sig_stage("thirteenf");
  auto stage = fields::PinnedStage::open(request, fields::kThirteenfStage);
  ASSERT_TRUE(stage.has_value()) << stage.error().message();
  EXPECT_EQ(stage->manifest_source().sha256, request.manifest_sha256);
  auto upper = request;
  for (char &c : upper.manifest_sha256) {
    c = (c >= 'a' && c <= 'f') ? static_cast<char>(c - 'a' + 'A') : c;
  }
  EXPECT_TRUE(fields::PinnedStage::open(upper, fields::kThirteenfStage).has_value());
  auto wrong = request;
  wrong.manifest_sha256 = std::string(64, '0');
  EXPECT_FALSE(fields::PinnedStage::open(wrong, fields::kThirteenfStage).has_value());
  fields::StageContract other = fields::kThirteenfStage;
  other.schema = "atx.alpha-panel.ftd/v1";
  EXPECT_FALSE(fields::PinnedStage::open(request, other).has_value());
  other = fields::kThirteenfStage;
  other.clock_rule = "13f-filed-plus-0h-v1";
  EXPECT_FALSE(fields::PinnedStage::open(request, other).has_value());
  other = fields::kThirteenfStage;
  other.staleness_days = 60;
  EXPECT_FALSE(fields::PinnedStage::open(request, other).has_value());
  // A verified read is recorded once; an unlisted file and a mis-pinned file are refused.
  constexpr std::array<std::string_view, 2> kColumns{"accession", "unit_factor"};
  EXPECT_TRUE(stage->table("filing_checks.parquet", kColumns).has_value());
  EXPECT_TRUE(stage->table("filing_checks.parquet", kColumns).has_value());
  EXPECT_EQ(stage->sources().size(), 2U);
  EXPECT_EQ(stage->check().at("files_read"), 1);
  EXPECT_FALSE(stage->table("nothing.parquet", kColumns).has_value());
  EXPECT_TRUE(stage->lists("parts/source=2023q4/holdings.parquet"));
  constexpr std::array<std::string_view, 1> kAccession{"accession"};
  EXPECT_FALSE(stage->table("parts/source=2023q4/holdings.parquet", kAccession).has_value());
}

TEST(ResearchFieldsAlSigThirteenf, SealedPartIsNeverOpenedAndTheProbeHasTeeth) {
  // Both poisoned parts are listed with a wrong SHA-256: a read of either is refused. An effective
  // filing in the sealed part 2024q1 must leave the read untouched (counted); the same filing in
  // the unsealed part 2023q4 is opened and refused, so a seal that let 2024q1 through would fail.
  auto stage = fields::PinnedStage::open(support::al_sig_stage("thirteenf"),
                                         fields::kThirteenfStage);
  ASSERT_TRUE(stage.has_value()) << stage.error().message();
  const auto index_in = [](std::string part) {
    fields::ThirteenfIndex index;
    const i64 p = day(2023, 9, 30);
    index.filings = {filing("poison-acc", 1, "13F-HR", "", p, p + 20, std::move(part))};
    fields::ThirteenfQuarter q;
    q.period_day = p;
    q.has_filing = true;
    q.visible_ns = filed_plus_46h(p + 20);
    q.effective = {0};
    q.cusip_sid = {{"C101", 101}};
    index.quarters = {q};
    return index;
  };
  fields::ThirteenfCounts counts;
  const auto sealed = fields::read_quarter_rows(*stage, index_in("2024q1"), 0, counts);
  ASSERT_TRUE(sealed.has_value()) << sealed.error().message();
  EXPECT_TRUE(sealed->empty());
  EXPECT_EQ(counts.holdings_parts_not_read_sealed, 1U);
  EXPECT_FALSE(fields::read_quarter_rows(*stage, index_in("2023q4"), 0, counts).has_value());
  EXPECT_FALSE(fields::read_quarter_rows(*stage, index_in("1999q1"), 0, counts).has_value());
  EXPECT_EQ(counts.holdings_parts_not_read_sealed, 1U);
}
