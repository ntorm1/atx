// W0-D0 — PIT shares: split rebase and the same-filed-date tie (D-06, rebase part).
//
// Suite: DataCorpActRebase_W0d0
//
// A synthetic security_master.parquet (written in-process; dates are 2016 epoch
// days) exercises two symbols around a 2:1 split on day kSplit:
//   XREB — one filing (1000 shares, filed on day 0) on the pre-split basis and no
//          later filing. The count visible after the split must double (rebased by
//          the cum_adj_factor ratio), or market cap halves at the split.
//   XTIE — the vendor repeats the SAME filing (filed day 0) on every row, restated
//          on each row's basis (1000 before the split, 2000 after). At a pre-split
//          date the legacy rule resolved the tie to the LAST row — one dated after
//          the split — and showed the post-split count early.
// A null filed_date is written as INT32_MIN (the date32 null sentinel the loader
// maps to "no filing"), because the atx-core writer has no null support.

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <limits>
#include <span>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "atx/core/io/parquet_writer.hpp"
#include "atx/core/types.hpp"

#include "atx/engine/alpha/panel.hpp"
#include "atx/engine/data/corporate_actions.hpp"
#include "atx/engine/data/dataset.hpp"
#include "atx/engine/data/universe.hpp"

namespace atx_test_w0_d0_corpact_rebase {

namespace fs = std::filesystem;
namespace io = atx::core::io;
using atx::engine::data::build_universe;
using atx::engine::data::corp_action_schema;
using atx::engine::data::Dataset;
using atx::engine::data::load_security_master;
using atx::engine::data::SharesPitRule;
using atx::engine::data::UniverseConfig;

namespace {

constexpr atx::i64 kDay0 = 17000;
constexpr atx::usize kDays = 6;
constexpr atx::usize kSplit = 3; // ex-date index: factor 0.5 -> 1.0, raw price 100 -> 50
constexpr atx::i64 kNullDate = std::numeric_limits<atx::i32>::min();

struct MasterRow {
  std::string symbol;
  atx::i64 date;
  atx::f64 factor;
  atx::i64 shares;
  atx::i64 filed;
};

[[nodiscard]] std::vector<MasterRow> master_rows(bool mutate_after_day2) {
  std::vector<MasterRow> rows;
  for (atx::usize k = 0; k < kDays; ++k) {
    const atx::i64 d = kDay0 + static_cast<atx::i64>(k);
    const atx::f64 f = (k < kSplit) ? 0.5 : 1.0;
    // XREB: the one filing sits on day 0; later rows carry no filing.
    rows.push_back({"XREB", d, f, 1000, k == 0 ? kDay0 : kNullDate});
    // XTIE: the same filing (filed day 0) restated on each row's share basis.
    atx::i64 tie_shares = (k < kSplit) ? 1000 : 2000;
    if (mutate_after_day2 && k > 2) {
      tie_shares = 7777; // a future restatement: must not move rows dated <= day 2
    }
    rows.push_back({"XTIE", d, f, tie_shares, kDay0});
  }
  return rows;
}

[[nodiscard]] bool write_master(const fs::path &path, const std::vector<MasterRow> &rows) {
  std::vector<std::string> symbol;
  std::vector<atx::i64> date;
  std::vector<atx::f64> factor;
  std::vector<atx::f64> dividend;
  std::vector<std::string> currency;
  std::vector<atx::i64> shares;
  std::vector<atx::i64> filed;
  std::vector<std::string> sic;
  std::vector<std::string> gics;
  for (const MasterRow &r : rows) {
    symbol.push_back(r.symbol);
    date.push_back(r.date);
    factor.push_back(r.factor);
    dividend.push_back(0.0);
    currency.push_back("USD");
    shares.push_back(r.shares);
    filed.push_back(r.filed);
    sic.push_back("3571");
    gics.push_back("45");
  }
  const std::vector<io::WriteColumn> cols = {
      {"symbol", std::span<const std::string>(symbol)},
      {"date", std::span<const atx::i64>(date)},
      {"cumulative_adjustment_factor", std::span<const atx::f64>(factor)},
      {"cash_dividend", std::span<const atx::f64>(dividend)},
      {"dividend_currency", std::span<const std::string>(currency)},
      {"shares_outstanding", std::span<const atx::i64>(shares)},
      {"shares_filed_date", std::span<const atx::i64>(filed)},
      {"sec_sic", std::span<const std::string>(sic)},
      {"gics_sector_code", std::span<const std::string>(gics)},
  };
  return io::write_parquet(cols, path.string()).has_value();
}

[[nodiscard]] Dataset load(const fs::path &path, SharesPitRule rule) {
  auto ds = load_security_master(path.string(), corp_action_schema(), rule);
  EXPECT_TRUE(ds.has_value()) << (ds ? "" : ds.error().to_string());
  return std::move(ds).value();
}

// shares_outstanding (column 2) for instrument `inst` on date row `d`.
[[nodiscard]] atx::f64 shares_at(const Dataset &ds, atx::usize d, atx::usize inst) {
  return ds.column(2)[d * ds.num_instruments() + inst];
}
[[nodiscard]] atx::f64 filed_at(const Dataset &ds, atx::usize d, atx::usize inst) {
  return ds.column(3)[d * ds.num_instruments() + inst];
}

struct TempFile {
  fs::path path;
  explicit TempFile(const std::string &name) : path{fs::temp_directory_path() / name} {}
  ~TempFile() {
    std::error_code ec;
    fs::remove(path, ec);
  }
  TempFile(const TempFile &) = delete;
  TempFile &operator=(const TempFile &) = delete;
};

constexpr atx::usize kReb = 0; // intern order: XREB first-seen, then XTIE
constexpr atx::usize kTie = 1;

} // namespace

// The single pre-split filing is restated on the post-split basis.
TEST(DataCorpActRebase_W0d0, SharesAreRebasedAcrossASplit) {
  TempFile f("atx_w0d0_rebase_master.parquet");
  ASSERT_TRUE(write_master(f.path, master_rows(false)));
  const Dataset v2 = load(f.path, SharesPitRule::RebasedRowDatedV2);
  const Dataset v1 = load(f.path, SharesPitRule::AsFiledV1);
  ASSERT_EQ(v2.num_dates(), kDays);
  for (atx::usize d = 0; d < kDays; ++d) {
    const atx::f64 expect = (d < kSplit) ? 1000.0 : 2000.0;
    EXPECT_DOUBLE_EQ(shares_at(v2, d, kReb), expect) << "day " << d;
    EXPECT_DOUBLE_EQ(shares_at(v1, d, kReb), 1000.0) << "legacy day " << d;
    EXPECT_DOUBLE_EQ(filed_at(v2, d, kReb), static_cast<atx::f64>(kDay0));
  }
}

// The same-filed-date tie resolves only to rows dated <= d.
TEST(DataCorpActRebase_W0d0, SameFiledDateTieUsesOnlyRowsDatedOnOrBeforeD) {
  TempFile f("atx_w0d0_tie_master.parquet");
  ASSERT_TRUE(write_master(f.path, master_rows(false)));
  const Dataset v2 = load(f.path, SharesPitRule::RebasedRowDatedV2);
  const Dataset v1 = load(f.path, SharesPitRule::AsFiledV1);
  atx::usize v1_leaks = 0;
  for (atx::usize d = 0; d < kDays; ++d) {
    const atx::f64 expect = (d < kSplit) ? 1000.0 : 2000.0;
    EXPECT_DOUBLE_EQ(shares_at(v2, d, kTie), expect) << "day " << d;
    // Legacy: every date resolves to the last tied row (dated day 5): 2000.
    EXPECT_DOUBLE_EQ(shares_at(v1, d, kTie), 2000.0) << "legacy day " << d;
    v1_leaks += (d < kSplit && shares_at(v1, d, kTie) != expect) ? 1U : 0U;
  }
  EXPECT_EQ(v1_leaks, kSplit) << "legacy tie showed the post-split count before the split";
  std::printf("[corp-tie] legacy leaked post-split shares on %zu pre-split days; V2 on 0\n",
              static_cast<std::size_t>(v1_leaks));
}

// Point-in-time: restating every row dated after day 2 leaves shares on days <= 2
// unchanged under the V2 rule (the legacy rule moves them).
TEST(DataCorpActRebase_W0d0, FutureRowRestatementDoesNotMovePastShares) {
  TempFile a("atx_w0d0_pit_a.parquet");
  TempFile b("atx_w0d0_pit_b.parquet");
  ASSERT_TRUE(write_master(a.path, master_rows(false)));
  ASSERT_TRUE(write_master(b.path, master_rows(true)));
  const Dataset v2a = load(a.path, SharesPitRule::RebasedRowDatedV2);
  const Dataset v2b = load(b.path, SharesPitRule::RebasedRowDatedV2);
  const Dataset v1a = load(a.path, SharesPitRule::AsFiledV1);
  const Dataset v1b = load(b.path, SharesPitRule::AsFiledV1);
  atx::usize v1_moved = 0;
  for (atx::usize d = 0; d <= 2; ++d) {
    for (atx::usize i = 0; i < 2; ++i) {
      EXPECT_DOUBLE_EQ(shares_at(v2a, d, i), shares_at(v2b, d, i)) << d << "/" << i;
      v1_moved += (shares_at(v1a, d, i) != shares_at(v1b, d, i)) ? 1U : 0U;
    }
  }
  EXPECT_EQ(v1_moved, 3U) << "legacy XTIE rows 0..2 took the future restatement";
}

// Market cap (shares x raw close, through build_universe) is continuous across the
// split under V2 and halves under the legacy rule.
TEST(DataCorpActRebase_W0d0, MarketCapIsContinuousAcrossTheSplit) {
  TempFile f("atx_w0d0_mcap_master.parquet");
  ASSERT_TRUE(write_master(f.path, master_rows(false)));
  const Dataset v2 = load(f.path, SharesPitRule::RebasedRowDatedV2);
  const Dataset v1 = load(f.path, SharesPitRule::AsFiledV1);
  // Raw price panel on the same axis: 100 before the split, 50 after.
  std::vector<atx::f64> close(kDays * 2);
  std::vector<atx::f64> volume(kDays * 2, 1.0e6);
  for (atx::usize d = 0; d < kDays; ++d) {
    close[d * 2 + 0] = close[d * 2 + 1] = (d < kSplit) ? 100.0 : 50.0;
  }
  auto panel =
      atx::engine::alpha::Panel::create(kDays, 2, {"close", "volume"}, {close, volume}, {});
  ASSERT_TRUE(panel.has_value());
  UniverseConfig cfg;
  cfg.adv_window = 1;
  cfg.min_adv_usd = 0.0;
  auto u2 = build_universe(*panel, v2, cfg);
  auto u1 = build_universe(*panel, v1, cfg);
  ASSERT_TRUE(u2.has_value() && u1.has_value());
  const atx::f64 before = u2->market_cap[(kSplit - 1) * 2 + kReb];
  const atx::f64 after = u2->market_cap[kSplit * 2 + kReb];
  const atx::f64 before_v1 = u1->market_cap[(kSplit - 1) * 2 + kReb];
  const atx::f64 after_v1 = u1->market_cap[kSplit * 2 + kReb];
  EXPECT_DOUBLE_EQ(after / before, 1.0);
  EXPECT_DOUBLE_EQ(after_v1 / before_v1, 0.5);
  std::printf("[corp-rebase] market-cap step at split: V2=%.3f legacy=%.3f\n",
              after / before - 1.0, after_v1 / before_v1 - 1.0);
}

// A basis that cannot be established (differing factors, one not finite) is NaN,
// never the stale count.
TEST(DataCorpActRebase_W0d0, UnknownBasisGivesNaNNotAStaleCount) {
  std::vector<MasterRow> rows = {
      {"XNAN", kDay0, 0.5, 1000, kDay0},
      {"XNAN", kDay0 + 1, std::numeric_limits<atx::f64>::quiet_NaN(), 1000, kNullDate},
      {"XNAN", kDay0 + 2, 0.5, 1000, kNullDate},
  };
  TempFile f("atx_w0d0_nan_master.parquet");
  ASSERT_TRUE(write_master(f.path, rows));
  const Dataset v2 = load(f.path, SharesPitRule::RebasedRowDatedV2);
  EXPECT_DOUBLE_EQ(shares_at(v2, 0, 0), 1000.0);
  EXPECT_TRUE(std::isnan(shares_at(v2, 1, 0)));
  EXPECT_DOUBLE_EQ(shares_at(v2, 2, 0), 1000.0); // same factor as the filing row
  EXPECT_EQ(static_cast<atx::i64>(filed_at(v2, 1, 0)), kDay0);
}

} // namespace atx_test_w0_d0_corpact_rebase
