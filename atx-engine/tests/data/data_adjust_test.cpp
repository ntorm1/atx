// atx::engine::data — split + dividend total-return adjustment tests (p3 S1-3).
//
// Suite: DataAdjust
//
// Covers the 6 named S1-3 tests:
//   1. SplitAdjustedNoDiscontinuityAtKnownAaplSplits — across the AAPL 4:1
//      (2020-08-31) and 7:1 (2014-06-09) ex-dates, S_t = raw_close * cum_adj_factor
//      is continuous: the factor step cancels the price drop, no fabricated jump.
//      Uses the REAL smoke cum_adj_factor (read from the on-disk security master)
//      paired with a raw close synthesized to drop by exactly the split ratio.
//   2. TotalReturnReinvestsDividendOnExDate — a hand fixture with a known prior
//      close, ex-date close, and per-share dividend: r_ex == (close+div)/prev - 1.
//   3. TotalReturnIndexMatchesAdjcloseOracleWithinTolerance — the documented
//      hand-fixture fallback (NOT the --mode adjclose web rebuild): real published
//      AAPL ex-date (prev_close, close, dividend) triples, asserting the engine's
//      TRI ratio across each ex-date matches the published total-return ratio
//      (close+div)/prev, equivalently the Yahoo back-adjustment factor 1 - div/prev
//      applied to prior prices.
//   4. ReturnInvariantUnderConstantPriceRescale — the §0.7 #2 non-leak property:
//      rescaling raw_close AND the price-denominated dividend by an arbitrary
//      constant leaves total_return BYTE-IDENTICAL.
//   5. NanRawCloseDoesNotZeroFill — a NaN raw close yields NaN S/r/TRI for that
//      cell (no silent zero), and the series resumes at the next valid close.
//   6. ZeroDividendSeriesEqualsSplitAdjustedReturns — with no dividends, the TRI
//      daily returns equal the pure split-adjusted price returns.
//
// Test 1 reads the REAL on-disk smoke cum_adj_factor; the data dir is gitignored
// and lives at the (main) repo root, resolved from __FILE__ + the git worktree
// marker exactly as data_corporate_actions_test.cpp does. If the data cannot be
// located the test skips with a clear message (never a false pass). Tests 2-6 are
// self-contained deterministic fixtures.

#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <filesystem>

#include <gtest/gtest.h>

#include "atx/core/types.hpp"
#include "atx/engine/data/adjust.hpp"
#include "atx/engine/data/corporate_actions.hpp"
#include "atx/engine/data/dataset.hpp"
#include "atx/engine/data/dataset_schema.hpp"

namespace atxtest_data_adjust_test {

namespace fs = std::filesystem;
using atx::engine::data::AdjustedSeries;
using atx::engine::data::adjust_total_return;
using atx::engine::data::CorpActionColumns;
using atx::engine::data::corp_action_schema;
using atx::engine::data::Dataset;
using atx::engine::data::extract_symbol;
using atx::engine::data::InstKey;
using atx::engine::data::load_security_master;

namespace {

constexpr atx::f64 kNaN = std::numeric_limits<atx::f64>::quiet_NaN();

// ---------------------------------------------------------------------------
// On-disk smoke-data resolution (mirrors data_corporate_actions_test.cpp).
// ---------------------------------------------------------------------------
constexpr const char *kProbe =
    "data/us_security_master_smoke/security_master/security_master.parquet";

[[nodiscard]] std::optional<fs::path> probe(const fs::path &root) {
  std::error_code ec;
  const fs::path candidate = root / kProbe;
  if (fs::exists(candidate, ec)) {
    return candidate;
  }
  return std::nullopt;
}

[[nodiscard]] std::optional<fs::path> main_root_from_worktree(const fs::path &ancestor) {
  std::error_code ec;
  const fs::path git_marker = ancestor / ".git";
  if (!fs::is_regular_file(git_marker, ec)) {
    return std::nullopt;
  }
  std::ifstream in(git_marker);
  std::string line;
  if (!std::getline(in, line)) {
    return std::nullopt;
  }
  const std::string prefix = "gitdir:";
  if (line.rfind(prefix, 0) != 0) {
    return std::nullopt;
  }
  std::string gitdir = line.substr(prefix.size());
  while (!gitdir.empty() && (gitdir.front() == ' ' || gitdir.front() == '\t')) {
    gitdir.erase(gitdir.begin());
  }
  // gitdir = <mainroot>/.git/worktrees/<name>  ->  mainroot = parent^3.
  fs::path p(gitdir);
  return p.parent_path().parent_path().parent_path();
}

[[nodiscard]] bool read_env(const char *name, std::string &out) {
  char *buf = nullptr;
  size_t len = 0;
  if (_dupenv_s(&buf, &len, name) != 0 || buf == nullptr) {
    return false;
  }
  out.assign(buf);
  std::free(buf);
  return true;
}

[[nodiscard]] std::optional<fs::path> find_master_parquet() {
  if (std::string env; read_env("ATX_DATA_DIR", env)) {
    const fs::path direct =
        fs::path(env) / "us_security_master_smoke" / "security_master" / "security_master.parquet";
    std::error_code ec;
    if (fs::exists(direct, ec)) {
      return direct;
    }
  }
  for (fs::path dir = fs::path(__FILE__).parent_path(); !dir.empty(); dir = dir.parent_path()) {
    if (auto hit = probe(dir)) {
      return hit;
    }
    if (auto root = main_root_from_worktree(dir)) {
      if (auto hit = probe(*root)) {
        return hit;
      }
    }
    if (dir == dir.root_path()) {
      break;
    }
  }
  std::error_code ec;
  for (fs::path dir = fs::current_path(ec); !dir.empty(); dir = dir.parent_path()) {
    if (auto hit = probe(dir)) {
      return hit;
    }
    if (dir == dir.root_path()) {
      break;
    }
  }
  return std::nullopt;
}

// Locate AAPL in the smoke master by its published 2026-05-11 ex-date dividend
// (0.27 per share), the same runtime probe S1-2's tests use.
constexpr atx::i64 kAaplLastExDay = 20584; // 2026-05-11
constexpr atx::f64 kAaplLastExDiv = 0.27;

[[nodiscard]] atx::usize date_row(const Dataset &ds, atx::i64 day) {
  const auto dates = ds.dates();
  for (atx::usize d = 0; d < dates.size(); ++d) {
    if (dates[d] == day) {
      return d;
    }
  }
  return dates.size();
}

[[nodiscard]] std::optional<InstKey> find_aapl(const Dataset &ds) {
  const atx::usize row = date_row(ds, kAaplLastExDay);
  if (row == ds.num_dates()) {
    return std::nullopt;
  }
  const auto div = ds.column(1); // cash_dividend
  const atx::usize ni = ds.num_instruments();
  for (atx::usize i = 0; i < ni; ++i) {
    if (std::abs(div[row * ni + i] - kAaplLastExDiv) < 1e-9) {
      return ds.instruments()[i];
    }
  }
  return std::nullopt;
}

// Index of `day` in an ascending epoch-day vector, or npos.
[[nodiscard]] atx::usize index_of_day(const std::vector<atx::i64> &dates, atx::i64 day) {
  for (atx::usize k = 0; k < dates.size(); ++k) {
    if (dates[k] == day) {
      return k;
    }
  }
  return dates.size();
}

// AAPL split ex-dates (epoch-days) and their integer split ratios. On each
// ex-date the smoke cum_adj_factor steps UP by exactly the ratio.
constexpr atx::i64 kAapl4to1ExDay = 18505; // 2020-08-31, 4:1
constexpr atx::i64 kAapl4to1Prev = 18502;  // 2020-08-28 (prior trading day)
constexpr atx::i64 kAapl7to1ExDay = 16230; // 2014-06-09, 7:1
constexpr atx::i64 kAapl7to1Prev = 16227;  // 2014-06-06 (prior trading day)

} // namespace

// ---------------------------------------------------------------------------
// Test 1 — split-adjusted close has no discontinuity at known AAPL splits.
// ---------------------------------------------------------------------------
TEST(DataAdjust, SplitAdjustedNoDiscontinuityAtKnownAaplSplits) {
  const auto path = find_master_parquet();
  if (!path) {
    GTEST_SKIP() << "smoke security_master.parquet not found (set ATX_DATA_DIR)";
  }
  auto res = load_security_master(path->string(), corp_action_schema());
  ASSERT_TRUE(res.has_value()) << res.error().to_string();
  const Dataset ds = std::move(res).value();
  const auto aapl = find_aapl(ds);
  ASSERT_TRUE(aapl.has_value());
  auto cols_res = extract_symbol(ds, *aapl);
  ASSERT_TRUE(cols_res.has_value()) << cols_res.error().to_string();
  const CorpActionColumns c = std::move(cols_res).value();

  // Real smoke cum_adj_factor. Synthesize a raw close that drops by exactly the
  // split ratio at each ex-date — i.e. raw_close_k = K / caf_k for a constant K —
  // which is how a real unadjusted price behaves (the ex-date drop is the inverse
  // of the cumulative-factor step). Then S_k = raw_close_k * caf_k == K must be
  // CONSTANT across the split: any deviation is a fabricated jump.
  constexpr atx::f64 kBasis = 100.0; // arbitrary split-adjusted level
  const atx::usize n = c.dates.size();
  ASSERT_GT(n, 0U);
  std::vector<atx::f64> raw_close(n, kNaN);
  for (atx::usize k = 0; k < n; ++k) {
    if (std::isfinite(c.cum_adj_factor[k]) && c.cum_adj_factor[k] > 0.0) {
      raw_close[k] = kBasis / c.cum_adj_factor[k];
    }
  }

  const AdjustedSeries adj = adjust_total_return(raw_close, c.cum_adj_factor, c.cash_dividend);
  ASSERT_EQ(adj.split_adj_close.size(), n);

  struct SplitCheck {
    atx::i64 ex_day;
    atx::i64 prev_day;
    atx::f64 ratio; // integer split ratio (factor step)
  };
  const SplitCheck splits[] = {
      {kAapl4to1ExDay, kAapl4to1Prev, 4.0},
      {kAapl7to1ExDay, kAapl7to1Prev, 7.0},
  };
  for (const SplitCheck &sc : splits) {
    const atx::usize ex = index_of_day(c.dates, sc.ex_day);
    const atx::usize prev = index_of_day(c.dates, sc.prev_day);
    ASSERT_LT(ex, n) << "ex-date " << sc.ex_day << " absent from smoke AAPL";
    ASSERT_LT(prev, n) << "prev-date " << sc.prev_day << " absent from smoke AAPL";

    // Real-data sanity: the cumulative factor steps up by exactly the split ratio.
    EXPECT_NEAR(c.cum_adj_factor[ex] / c.cum_adj_factor[prev], sc.ratio, 1e-6)
        << "smoke cum_adj_factor did not step by the split ratio at " << sc.ex_day;

    // The split-adjusted close is continuous: |S_ex / S_prev - 1| ~ 0 (tolerance
    // ABSORBS only the 8-digit rounding of the on-disk factor; a real split jump
    // would be a factor-of-N break, orders of magnitude larger).
    const atx::f64 s_ex = adj.split_adj_close[ex];
    const atx::f64 s_prev = adj.split_adj_close[prev];
    ASSERT_TRUE(std::isfinite(s_ex) && std::isfinite(s_prev));
    EXPECT_NEAR(s_ex / s_prev, 1.0, 1e-6)
        << "fabricated split jump at ex-date " << sc.ex_day << ": S_prev=" << s_prev
        << " S_ex=" << s_ex;
    // And the daily total return across the split is ~0 (no fabricated return).
    EXPECT_NEAR(adj.total_return[ex], 0.0, 1e-6) << "fabricated return at split " << sc.ex_day;
  }
}

// ---------------------------------------------------------------------------
// Test 2 — dividend reinvested on the ex-date (hand fixture).
// ---------------------------------------------------------------------------
TEST(DataAdjust, TotalReturnReinvestsDividendOnExDate) {
  // Three trading days, no splits (factor = 1). Day 1 is an ex-date with a known
  // per-share dividend; days 0 and 2 are dividend-free.
  const std::vector<atx::f64> close = {100.0, 102.0, 101.0};
  const std::vector<atx::f64> factor = {1.0, 1.0, 1.0};
  const std::vector<atx::f64> div = {0.0, 0.50, 0.0};

  const AdjustedSeries adj = adjust_total_return(close, factor, div);

  EXPECT_DOUBLE_EQ(adj.total_return[0], 0.0); // first cell anchors at r = 0
  // Ex-date total return reinvests the dividend in the numerator.
  const atx::f64 expect_r1 = (102.0 + 0.50) / 100.0 - 1.0;
  EXPECT_DOUBLE_EQ(adj.total_return[1], expect_r1);
  // Non-ex day: plain price return.
  EXPECT_DOUBLE_EQ(adj.total_return[2], 101.0 / 102.0 - 1.0);

  // TRI chains the returns off S_0 = 100.
  EXPECT_DOUBLE_EQ(adj.total_return_index[0], 100.0);
  EXPECT_DOUBLE_EQ(adj.total_return_index[1], 100.0 * (1.0 + expect_r1));
  EXPECT_DOUBLE_EQ(adj.total_return_index[2],
                   adj.total_return_index[1] * (1.0 + (101.0 / 102.0 - 1.0)));
}

// ---------------------------------------------------------------------------
// Test 3 — TRI matches the published adjusted-close oracle (hand-fixture
//          fallback; the --mode adjclose web rebuild is intentionally NOT run).
// ---------------------------------------------------------------------------
TEST(DataAdjust, TotalReturnIndexMatchesAdjcloseOracleWithinTolerance) {
  // Real published AAPL ex-dates: {prev_close, ex_close, per-share dividend}.
  // Sources: databento close on the prior trading day + ex-date, smoke-master
  // cash_dividend on the ex-date (all on-disk, post-2020 so factor = 1.0). The
  // published Yahoo back-adjustment multiplies prior prices by (1 - div/prev) on
  // the ex-date; equivalently the forward total-return ratio is (ex+div)/prev.
  // The engine's TRI ratio across the ex-date must equal that within ε_oracle.
  struct ExDate {
    atx::f64 prev_close;
    atx::f64 ex_close;
    atx::f64 dividend;
  };
  const ExDate fixtures[] = {
      {216.24, 217.53, 0.25}, // 2024-08-12
      {227.48, 226.96, 0.25}, // 2024-11-08
      {227.63, 227.65, 0.25}, // 2025-02-10
      {229.35, 227.18, 0.26}, // 2025-08-11
      {268.47, 269.43, 0.26}, // 2025-11-10
  };
  constexpr atx::f64 kEpsOracle = 1e-9; // exact algebraic match (no float drift budget needed)

  for (const ExDate &f : fixtures) {
    const std::vector<atx::f64> close = {f.prev_close, f.ex_close};
    const std::vector<atx::f64> factor = {1.0, 1.0};
    const std::vector<atx::f64> div = {0.0, f.dividend};
    const AdjustedSeries adj = adjust_total_return(close, factor, div);

    // Engine TRI ratio across the ex-date == published total-return ratio.
    const atx::f64 tri_ratio = adj.total_return_index[1] / adj.total_return_index[0];
    const atx::f64 published_ratio = (f.ex_close + f.dividend) / f.prev_close;
    EXPECT_NEAR(tri_ratio, published_ratio, kEpsOracle);

    // Cross-check via the Yahoo back-adjustment factor on PRIOR prices: a prior
    // raw price P is published-adjusted to P * (1 - div/prev). The total-return
    // ratio equals 1 / (that prior factor scaled to the ex close) — i.e. the
    // dividend yield contributes (div/prev) of extra return vs the bare price move.
    const atx::f64 bare_price_ratio = f.ex_close / f.prev_close;
    const atx::f64 dividend_contrib = f.dividend / f.prev_close;
    EXPECT_NEAR(adj.total_return[1], (bare_price_ratio - 1.0) + dividend_contrib, kEpsOracle);
  }
}

// ---------------------------------------------------------------------------
// Test 4 — return invariance under a constant price rescale (the non-leak).
// ---------------------------------------------------------------------------
TEST(DataAdjust, ReturnInvariantUnderConstantPriceRescale) {
  const std::vector<atx::f64> close = {100.0, 102.0, 99.5, 101.25, 103.0};
  const std::vector<atx::f64> factor = {0.25, 0.25, 1.0, 1.0, 1.0}; // a split mid-series
  const std::vector<atx::f64> div = {0.0, 0.22, 0.0, 0.24, 0.0};

  const AdjustedSeries base = adjust_total_return(close, factor, div);

  // Rescale the WHOLE price history (and the price-denominated dividend) by an
  // arbitrary positive constant. Returns must be BYTE-IDENTICAL — back-adjustment
  // moves levels, never returns (§0.7 #2).
  constexpr atx::f64 k = 7.3125;
  std::vector<atx::f64> close_k(close.size());
  std::vector<atx::f64> div_k(div.size());
  for (atx::usize i = 0; i < close.size(); ++i) {
    close_k[i] = close[i] * k;
    div_k[i] = div[i] * k;
  }
  const AdjustedSeries scaled = adjust_total_return(close_k, factor, div_k);

  ASSERT_EQ(base.total_return.size(), scaled.total_return.size());
  for (atx::usize i = 0; i < base.total_return.size(); ++i) {
    // Byte-identical (bit-for-bit), not merely near: a constant cancels exactly.
    EXPECT_EQ(std::bit_cast<atx::u64>(base.total_return[i]),
              std::bit_cast<atx::u64>(scaled.total_return[i]))
        << "total_return diverged under rescale at index " << i;
  }
  // The TRI LEVELS, by contrast, scale by k (levels are basis-dependent).
  for (atx::usize i = 0; i < base.total_return_index.size(); ++i) {
    EXPECT_NEAR(scaled.total_return_index[i], base.total_return_index[i] * k, 1e-9);
  }
}

// ---------------------------------------------------------------------------
// Test 5 — a NaN raw close does not zero-fill; the series resumes after the gap.
// ---------------------------------------------------------------------------
TEST(DataAdjust, NanRawCloseDoesNotZeroFill) {
  const std::vector<atx::f64> close = {100.0, kNaN, 105.0, 106.0};
  const std::vector<atx::f64> factor = {1.0, 1.0, 1.0, 1.0};
  const std::vector<atx::f64> div = {0.0, 0.0, 0.0, 0.0};

  const AdjustedSeries adj = adjust_total_return(close, factor, div);

  // The gap cell is NaN everywhere — never silently 0.0.
  EXPECT_TRUE(std::isnan(adj.split_adj_close[1]));
  EXPECT_TRUE(std::isnan(adj.total_return[1]));
  EXPECT_TRUE(std::isnan(adj.total_return_index[1]));

  // The series resumes at the next valid close, re-anchoring (r = 0, TRI = S);
  // there is no defined return across the gap, so no fabricated jump.
  EXPECT_DOUBLE_EQ(adj.total_return[0], 0.0);
  EXPECT_DOUBLE_EQ(adj.total_return[2], 0.0);
  EXPECT_DOUBLE_EQ(adj.total_return_index[2], 105.0);
  // After resumption the chain continues normally.
  EXPECT_DOUBLE_EQ(adj.total_return[3], 106.0 / 105.0 - 1.0);
  EXPECT_DOUBLE_EQ(adj.total_return_index[3], 105.0 * (106.0 / 105.0));

  // A NaN cum_adj_factor is ALSO a gap (policy: unknown factor != 1.0), never a
  // fabricated un-adjusted return.
  const std::vector<atx::f64> factor_gap = {1.0, kNaN, 1.0, 1.0};
  const AdjustedSeries adj2 =
      adjust_total_return({close.data(), close.size()}, factor_gap, div);
  EXPECT_TRUE(std::isnan(adj2.split_adj_close[1]));
  EXPECT_TRUE(std::isnan(adj2.total_return[1]));
}

// ---------------------------------------------------------------------------
// Test 6 — with zero dividends, TRI returns equal the split-adjusted returns.
// ---------------------------------------------------------------------------
TEST(DataAdjust, ZeroDividendSeriesEqualsSplitAdjustedReturns) {
  const std::vector<atx::f64> close = {50.0, 51.0, 49.5, 52.0, 53.5};
  const std::vector<atx::f64> factor = {0.5, 0.5, 1.0, 1.0, 1.0}; // a split mid-series
  const std::vector<atx::f64> div(close.size(), 0.0);

  const AdjustedSeries adj = adjust_total_return(close, factor, div);

  // With D_t == 0, r_t reduces to the pure split-adjusted price return
  // S_t / S_{t-1} - 1, and TRI reduces to the split-adjusted close (up to the
  // first-cell level anchor S_0).
  EXPECT_DOUBLE_EQ(adj.total_return[0], 0.0);
  for (atx::usize t = 1; t < close.size(); ++t) {
    const atx::f64 s_t = adj.split_adj_close[t];
    const atx::f64 s_prev = adj.split_adj_close[t - 1];
    EXPECT_DOUBLE_EQ(adj.total_return[t], s_t / s_prev - 1.0);
    // TRI == split-adjusted close (the chain telescopes when no dividend); NEAR
    // absorbs only the float rounding of the (1 + (s/prev - 1)) chain, well below
    // any economically meaningful difference.
    EXPECT_NEAR(adj.total_return_index[t], s_t, 1e-9);
  }
  EXPECT_DOUBLE_EQ(adj.total_return_index[0], adj.split_adj_close[0]);
}

} // namespace atxtest_data_adjust_test
