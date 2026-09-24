// atx::engine::data::fundamentals — point-in-time fundamental field tests (lane 10).
//
// Suite: FundamentalFields
//
// Every fixture is SYNTHETIC and hand-sized so each expected cell can be read
// off the arrange block. The contracts pinned here are the ones a look-ahead or
// restatement bug would silently break:
//   * a record is invisible before (first session at/after available_at) + lag;
//   * the visible record with the LATEST FISCAL PERIOD wins, so a late
//     amendment of an older period never regresses a newer figure, while a
//     restatement of the SAME period (later available_at) replaces it;
//   * fields are chosen independently (a filing that omits a field keeps the
//     prior filing's value for that field);
//   * the forward fill stops at both staleness caps;
//   * derived ratios follow their literature definitions and are NaN on a
//     non-positive denominator.

#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "atx/core/types.hpp"
#include "atx/engine/alpha/panel.hpp"
#include "atx/engine/data/fundamental_fields.hpp"

namespace atx_test_l10_fundzoo_fundamental_fields {

using atx::f64;
using atx::i64;
using atx::usize;
namespace fund = atx::engine::data::fundamentals;

constexpr f64 kNaN = std::numeric_limits<f64>::quiet_NaN();
constexpr i64 kDay = fund::kNanosPerDay;

// Session keys: one per calendar day starting at epoch day `first_day`.
std::vector<i64> daily_keys(i64 first_day, usize n) {
  std::vector<i64> keys(n);
  for (usize t = 0; t < n; ++t) keys[t] = (first_day + static_cast<i64>(t)) * kDay;
  return keys;
}

fund::PitRecord record(usize inst, i64 avail_day, i64 period_end_day) {
  fund::PitRecord r{};
  r.instrument = inst;
  r.available_ns = avail_day * kDay;
  r.period_end_ns = period_end_day * kDay;
  r.values.fill(kNaN);
  return r;
}

f64 cell(const fund::AlignedFundamentals &a, fund::RawField f, usize t, usize i) {
  return a.raw[static_cast<usize>(f)][t * a.instruments + i];
}

fund::AlignConfig lenient(usize lag) {
  fund::AlignConfig cfg{};
  cfg.lag_sessions = lag;
  cfg.max_days_since_available = 10'000;
  cfg.max_days_since_period_end = 10'000;
  return cfg;
}

} // namespace atx_test_l10_fundzoo_fundamental_fields

namespace t = atx_test_l10_fundzoo_fundamental_fields;
namespace fund = atx::engine::data::fundamentals;
using fund::RawField;

TEST(FundamentalFields, RawFieldNamesAreUniqueAndComplete) {
  const auto names = fund::kRawFieldNames;
  ASSERT_EQ(names.size(), fund::kRawFieldCount);
  for (atx::usize a = 0; a < names.size(); ++a) {
    EXPECT_FALSE(names[a].empty());
    for (atx::usize b = a + 1; b < names.size(); ++b) EXPECT_NE(names[a], names[b]);
  }
  EXPECT_EQ(fund::raw_field_from_name("book_equity"), RawField::BookEquity);
  EXPECT_EQ(fund::raw_field_from_name("sue"), RawField::Sue);
  EXPECT_FALSE(fund::raw_field_from_name("no_such_field").has_value());
}

TEST(FundamentalFields, Align_NoLookAhead_InvisibleBeforeAvailablePlusLag) {
  const auto keys = t::daily_keys(100, 10);        // days 100..109
  auto r = t::record(0, 103, 90);                   // public on day 103
  r.values[static_cast<atx::usize>(RawField::BookEquity)] = 5.0;
  const std::vector<fund::PitRecord> recs{r};

  auto lag0 = fund::align_pit_records(recs, keys, 1, t::lenient(0));
  ASSERT_TRUE(lag0.has_value());
  EXPECT_TRUE(std::isnan(t::cell(*lag0, RawField::BookEquity, 2, 0))); // day 102
  EXPECT_DOUBLE_EQ(t::cell(*lag0, RawField::BookEquity, 3, 0), 5.0);  // day 103

  auto lag1 = fund::align_pit_records(recs, keys, 1, t::lenient(1));
  ASSERT_TRUE(lag1.has_value());
  EXPECT_TRUE(std::isnan(t::cell(*lag1, RawField::BookEquity, 3, 0)));
  EXPECT_DOUBLE_EQ(t::cell(*lag1, RawField::BookEquity, 4, 0), 5.0);
  EXPECT_DOUBLE_EQ(t::cell(*lag1, RawField::BookEquity, 9, 0), 5.0); // forward-filled
}

TEST(FundamentalFields, Align_AvailableBetweenSessions_UsesNextSession) {
  // Sessions only on even days; a fact available on an odd day becomes visible
  // on the next session, never the previous one.
  std::vector<atx::i64> keys{100 * t::kDay, 102 * t::kDay, 104 * t::kDay};
  auto r = t::record(0, 101, 90);
  r.values[static_cast<atx::usize>(RawField::TotalAssets)] = 7.0;
  auto a = fund::align_pit_records(std::vector<fund::PitRecord>{r}, keys, 1, t::lenient(0));
  ASSERT_TRUE(a.has_value());
  EXPECT_TRUE(std::isnan(t::cell(*a, RawField::TotalAssets, 0, 0)));
  EXPECT_DOUBLE_EQ(t::cell(*a, RawField::TotalAssets, 1, 0), 7.0);
}

TEST(FundamentalFields, Align_LaterPeriodSupersedes_OlderAmendmentDoesNotRegress) {
  const auto keys = t::daily_keys(100, 10);
  constexpr auto f = static_cast<atx::usize>(RawField::NetIncomeTtm);
  auto q1 = t::record(0, 101, 60);
  q1.values[f] = 1.0;
  auto q2 = t::record(0, 104, 90);
  q2.values[f] = 2.0;
  auto amend_q1 = t::record(0, 106, 60); // late 10-Q/A of the OLDER period
  amend_q1.values[f] = 1.5;
  auto restate_q2 = t::record(0, 108, 90); // restatement of the SAME latest period
  restate_q2.values[f] = 2.5;
  // Deliberately unsorted input: the aligner must not depend on record order.
  const std::vector<fund::PitRecord> recs{restate_q2, amend_q1, q2, q1};

  auto a = fund::align_pit_records(recs, keys, 1, t::lenient(0));
  ASSERT_TRUE(a.has_value());
  EXPECT_DOUBLE_EQ(t::cell(*a, RawField::NetIncomeTtm, 1, 0), 1.0);
  EXPECT_DOUBLE_EQ(t::cell(*a, RawField::NetIncomeTtm, 4, 0), 2.0);
  EXPECT_DOUBLE_EQ(t::cell(*a, RawField::NetIncomeTtm, 6, 0), 2.0); // no regression
  EXPECT_DOUBLE_EQ(t::cell(*a, RawField::NetIncomeTtm, 8, 0), 2.5); // same-period restatement
}

TEST(FundamentalFields, Align_FieldsChosenIndependently) {
  const auto keys = t::daily_keys(100, 6);
  constexpr auto be = static_cast<atx::usize>(RawField::BookEquity);
  constexpr auto ta = static_cast<atx::usize>(RawField::TotalAssets);
  auto first = t::record(0, 100, 60);
  first.values[be] = 10.0;
  first.values[ta] = 20.0;
  auto second = t::record(0, 102, 90); // omits book equity
  second.values[ta] = 30.0;
  auto a = fund::align_pit_records(std::vector<fund::PitRecord>{first, second}, keys, 1,
                                   t::lenient(0));
  ASSERT_TRUE(a.has_value());
  EXPECT_DOUBLE_EQ(t::cell(*a, RawField::BookEquity, 3, 0), 10.0);
  EXPECT_DOUBLE_EQ(t::cell(*a, RawField::TotalAssets, 3, 0), 30.0);
}

TEST(FundamentalFields, Align_StalenessCapsStopForwardFill) {
  const auto keys = t::daily_keys(100, 20);
  auto r = t::record(0, 100, 95);
  r.values[static_cast<atx::usize>(RawField::BookEquity)] = 3.0;
  const std::vector<fund::PitRecord> recs{r};

  fund::AlignConfig by_avail = t::lenient(0);
  by_avail.max_days_since_available = 5; // visible days 100..105
  auto a = fund::align_pit_records(recs, keys, 1, by_avail);
  ASSERT_TRUE(a.has_value());
  EXPECT_DOUBLE_EQ(t::cell(*a, RawField::BookEquity, 5, 0), 3.0);
  EXPECT_TRUE(std::isnan(t::cell(*a, RawField::BookEquity, 6, 0)));

  fund::AlignConfig by_period = t::lenient(0);
  by_period.max_days_since_period_end = 8; // period end 95 -> last day 103
  auto b = fund::align_pit_records(recs, keys, 1, by_period);
  ASSERT_TRUE(b.has_value());
  EXPECT_DOUBLE_EQ(t::cell(*b, RawField::BookEquity, 3, 0), 3.0);
  EXPECT_TRUE(std::isnan(t::cell(*b, RawField::BookEquity, 4, 0)));
  EXPECT_EQ(b->stats.stale_cells[static_cast<atx::usize>(RawField::BookEquity)], 16U);
}

TEST(FundamentalFields, Align_InstrumentsIsolated_AndStatsCounted) {
  const auto keys = t::daily_keys(100, 4);
  auto r = t::record(1, 100, 90);
  r.values[static_cast<atx::usize>(RawField::Sue)] = -1.25;
  auto a = fund::align_pit_records(std::vector<fund::PitRecord>{r}, keys, 3, t::lenient(0));
  ASSERT_TRUE(a.has_value());
  for (atx::usize d = 0; d < 4; ++d) {
    EXPECT_TRUE(std::isnan(t::cell(*a, RawField::Sue, d, 0)));
    EXPECT_DOUBLE_EQ(t::cell(*a, RawField::Sue, d, 1), -1.25);
    EXPECT_TRUE(std::isnan(t::cell(*a, RawField::Sue, d, 2)));
  }
  EXPECT_EQ(a->stats.records, 1U);
  EXPECT_EQ(a->stats.filled_cells[static_cast<atx::usize>(RawField::Sue)], 4U);
  EXPECT_EQ(a->stats.records_after_axis, 0U);
}

TEST(FundamentalFields, Align_RecordAfterAxisIsCountedNotPlaced) {
  const auto keys = t::daily_keys(100, 3);
  auto r = t::record(0, 150, 140);
  r.values[static_cast<atx::usize>(RawField::BookEquity)] = 1.0;
  auto a = fund::align_pit_records(std::vector<fund::PitRecord>{r}, keys, 1, t::lenient(0));
  ASSERT_TRUE(a.has_value());
  EXPECT_EQ(a->stats.records_after_axis, 1U);
  for (atx::usize d = 0; d < 3; ++d)
    EXPECT_TRUE(std::isnan(t::cell(*a, RawField::BookEquity, d, 0)));
}

TEST(FundamentalFields, Align_RejectsBadInputs) {
  const auto keys = t::daily_keys(100, 3);
  auto r = t::record(5, 100, 90); // instrument out of range
  EXPECT_FALSE(fund::align_pit_records(std::vector<fund::PitRecord>{r}, keys, 2, t::lenient(0))
                   .has_value());

  std::vector<atx::i64> bad_keys{2 * t::kDay, 1 * t::kDay};
  EXPECT_FALSE(
      fund::align_pit_records(std::span<const fund::PitRecord>{}, bad_keys, 1, t::lenient(0))
          .has_value());

  fund::AlignConfig neg = t::lenient(0);
  neg.max_days_since_available = 0;
  EXPECT_FALSE(
      fund::align_pit_records(std::span<const fund::PitRecord>{}, keys, 1, neg).has_value());

  auto late_period = t::record(0, 100, 120); // period ends after it became public
  EXPECT_FALSE(fund::align_pit_records(std::vector<fund::PitRecord>{late_period}, keys, 1,
                                       t::lenient(0))
                   .has_value());
}

TEST(FundamentalFields, MapSecurityIds_MapsKnownAndLeavesUnknownEmpty) {
  const std::vector<std::string> axis{"33449", "32952", "70652"};
  const std::vector<std::string> ids{"70652", "999", "33449"};
  auto m = fund::map_security_ids(axis, ids);
  ASSERT_TRUE(m.has_value());
  ASSERT_EQ(m->size(), 3U);
  EXPECT_EQ((*m)[0], std::optional<atx::usize>{2});
  EXPECT_FALSE((*m)[1].has_value());
  EXPECT_EQ((*m)[2], std::optional<atx::usize>{0});

  const std::vector<std::string> dup_axis{"1", "1"};
  EXPECT_FALSE(fund::map_security_ids(dup_axis, ids).has_value());
}

TEST(FundamentalFields, Derive_LiteratureDefinitions) {
  fund::AlignedFundamentals a{};
  a.dates = 1;
  a.instruments = 1;
  for (auto &col : a.raw) col.assign(1, t::kNaN);
  auto set = [&](RawField f, atx::f64 v) { a.raw[static_cast<atx::usize>(f)][0] = v; };
  set(RawField::BookEquity, 50.0);
  set(RawField::TotalAssets, 200.0);
  set(RawField::TotalLiabilities, 150.0);
  set(RawField::AssetsLag1y, 160.0);
  set(RawField::NetIncomeTtm, 10.0);
  set(RawField::RevenueTtm, 400.0);
  set(RawField::GrossProfitTtm, 80.0);
  set(RawField::OperatingCashFlowTtm, 16.0);
  set(RawField::OperatingIncomeTtm, 20.0);
  set(RawField::SharesOutstanding, 110.0);
  set(RawField::SharesLag1y, 100.0);
  set(RawField::Sue, 1.5);
  const std::vector<atx::f64> mcap{500.0};

  auto d = fund::derive_fields(a, mcap);
  ASSERT_TRUE(d.has_value());
  auto get = [&](fund::DerivedField f) { return (*d)[static_cast<atx::usize>(f)][0]; };
  EXPECT_DOUBLE_EQ(get(fund::DerivedField::BookToPrice), 0.1);
  EXPECT_DOUBLE_EQ(get(fund::DerivedField::EarningsYield), 0.02);
  EXPECT_DOUBLE_EQ(get(fund::DerivedField::SalesYield), 0.8);
  EXPECT_DOUBLE_EQ(get(fund::DerivedField::CashflowYield), 16.0 / 500.0);
  EXPECT_DOUBLE_EQ(get(fund::DerivedField::Roe), 0.2);
  EXPECT_DOUBLE_EQ(get(fund::DerivedField::Roa), 0.05);
  EXPECT_DOUBLE_EQ(get(fund::DerivedField::GrossProfitability), 0.4);
  EXPECT_DOUBLE_EQ(get(fund::DerivedField::OperatingProfitability), 0.4);
  EXPECT_DOUBLE_EQ(get(fund::DerivedField::Accruals), (10.0 - 16.0) / 180.0);
  EXPECT_DOUBLE_EQ(get(fund::DerivedField::AssetGrowth), 0.25);
  EXPECT_DOUBLE_EQ(get(fund::DerivedField::NetIssuance), std::log(1.1));
  EXPECT_DOUBLE_EQ(get(fund::DerivedField::Leverage), 0.75);
  EXPECT_DOUBLE_EQ(get(fund::DerivedField::Sue), 1.5);
}

TEST(FundamentalFields, Derive_NonPositiveDenominatorsAreNaN) {
  fund::AlignedFundamentals a{};
  a.dates = 1;
  a.instruments = 2;
  for (auto &col : a.raw) col.assign(2, 1.0);
  a.raw[static_cast<atx::usize>(RawField::BookEquity)] = {-5.0, 5.0};
  a.raw[static_cast<atx::usize>(RawField::TotalAssets)] = {0.0, 5.0};
  const std::vector<atx::f64> mcap{100.0, t::kNaN};
  auto d = fund::derive_fields(a, mcap);
  ASSERT_TRUE(d.has_value());
  auto get = [&](fund::DerivedField f, atx::usize i) {
    return (*d)[static_cast<atx::usize>(f)][i];
  };
  EXPECT_TRUE(std::isnan(get(fund::DerivedField::BookToPrice, 0))); // negative book
  EXPECT_TRUE(std::isnan(get(fund::DerivedField::Roe, 0)));
  EXPECT_TRUE(std::isnan(get(fund::DerivedField::Roa, 0)));          // zero assets
  EXPECT_TRUE(std::isnan(get(fund::DerivedField::Leverage, 0)));
  EXPECT_TRUE(std::isnan(get(fund::DerivedField::EarningsYield, 1))); // NaN market cap
  EXPECT_DOUBLE_EQ(get(fund::DerivedField::Roe, 1), 0.2);

  const std::vector<atx::f64> short_mcap{1.0};
  EXPECT_FALSE(fund::derive_fields(a, short_mcap).has_value());
}

TEST(FundamentalFields, WithFundamentalFields_AppendsNamedColumnsAndKeepsMask) {
  using atx::engine::alpha::Panel;
  auto base = Panel::create(2, 2, {"close", "market_cap"},
                            {{1.0, 2.0, 3.0, 4.0}, {10.0, 20.0, 30.0, 40.0}}, {1, 0, 1, 1});
  ASSERT_TRUE(base.has_value());
  fund::AlignedFundamentals a{};
  a.dates = 2;
  a.instruments = 2;
  for (auto &col : a.raw) col.assign(4, 1.0);
  auto d = fund::derive_fields(a, base->field_all(1));
  ASSERT_TRUE(d.has_value());

  auto out = fund::with_fundamental_fields(*base, a, *d);
  ASSERT_TRUE(out.has_value());
  EXPECT_EQ(out->num_fields(), 2U + fund::kRawFieldCount + fund::kDerivedFieldCount);
  EXPECT_FALSE(out->in_universe(0, 1));
  EXPECT_TRUE(out->in_universe(1, 1));
  auto close = out->field_id("close");
  ASSERT_TRUE(close.has_value());
  EXPECT_DOUBLE_EQ(out->field_all(*close)[3], 4.0);
  auto btp = out->field_id("book_to_price");
  ASSERT_TRUE(btp.has_value());
  EXPECT_DOUBLE_EQ(out->field_all(*btp)[2], 1.0 / 30.0);
  EXPECT_TRUE(out->field_id("book_equity").has_value());

  // A name collision with an existing panel field is refused, never shadowed.
  auto clash = Panel::create(2, 2, {"roe"}, {{0.0, 0.0, 0.0, 0.0}}, {});
  ASSERT_TRUE(clash.has_value());
  EXPECT_FALSE(fund::with_fundamental_fields(*clash, a, *d).has_value());
}
