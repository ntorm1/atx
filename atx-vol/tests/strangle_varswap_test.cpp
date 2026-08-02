// atx-vol XOM strangle-vs-varswap comparison backtest — strategy gates.
//
// The OPTIONS leg of the comparison: a FIXED-EXPIRY, DAILY-RESTRIKE strangle.
// One cycle picks a single expiry off the run's session grid and holds it; every
// step inside the cycle closes both wings and reopens them at freshly resolved
// +/-40 delta strikes on that step's surface, at the SAME expiry. The engine
// settles the cycle at its expiry session and the next cycle opens on that same
// step. The SWAP leg is one equal-vega uncapped var swap per cycle, and the
// SIGNALS are what the comparison reports per row.
//
// Gates:
//   1. OpensFortyDeltaStrangleAtSnappedFixedExpiry — the inception pair sits at
//      +/-40 delta on the hand-computed snapped session.
//   2. RestrikesDailyAtFixedExpiry               — strikes move every step, the
//      expiry does not, the book never accumulates lots, and `strangle_vega`
//      tracks the restruck pair.
//   3. RollsIntoNewCycleAtExpiry                 — a new pair with a strictly
//      LATER fixed expiry exists after the expiry session.
//   4. HedgeSpecIsDeltaToZeroDaily               — the engine-owned hedge request.
//   5. KeepsStrikesWhenSurfaceCannotServeDelta   — an unusable surface leaves the
//      live strikes untouched (no fabricated strike, no reopen churn).
//   6. FixesTheLastSessionWhenTheTenorOutrunsTheGrid — the short final cycle, and
//      the exhausted grid that opens nothing at all.
//   7. OpensEqualVegaVarSwapAtCycleStart         — one per cycle, fair-struck and
//      sized to the strangle's entry vega.
//   8. SwapLegDisabledMatchesOptionsOnly         — the swap lane is additive.
//   9. SkipsSwapWhenVegaUnavailable              — no vega, no leg, never a
//      fabricated quantity.
//  10. SkipsSwapWhenCycleIsTooShortToAccrue      — the one-session tail cycle.
//  11. SignalsMatchIndependentSwapGreeksOracle   — per-row swap greeks are the
//      engine's own live contract, accrual and all.
//  12. SignalsNaNWhenNoLiveSwap                  — an absent leg reports NOTHING,
//      never 0.0.
//  13. SkippedRestrikesCountsOnlyKeptStrikes     — the attribution counters mean
//      exactly what they say.
//
// Fixture plumbing (synthetic eSSVI surfaces written as one-symbol archives per
// date) mirrors backtest_swap_test.cpp. Per-step book state is read off
// `run_backtest_incremental`'s checkpoint over a PREFIX of the clock, so every
// assertion is on a book the engine itself validated and priced — never on
// strategy internals.

#include <gtest/gtest.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "atx/core/error.hpp"                  // Err, ErrorCode
#include "atx/vol/american.hpp"                // al_fast_opts, AmericanMethod
#include "atx/vol/backtest.hpp"                // Clock, run_backtest_incremental, RunConfig
#include "atx/vol/corpus.hpp"                  // CorpusManifest, CorpusEntry, CorpusFitStatus
#include "atx/vol/derivatives.hpp"             // DerivContract, DerivConfig, DerivGreeks, DerivKind
#include "atx/vol/detail/deriv_ref_bridge.hpp" // detail::deriv_greeks_on_ref
#include "atx/vol/portfolio_pricer.hpp"        // kNsPerYear, SurfaceRef, PriceOptions
#include "atx/vol/priced_surface.hpp"          // PricedSurface, PricingContext, FullGreekSeed
#include "atx/vol/strangle_varswap.hpp"        // StrangleVarswapConfig, StrangleVsVarswapStrategy
#include "atx/vol/strategy.hpp"                // HedgeSpec
#include "atx/vol/surface_archive.hpp"         // write_surface_archive_v2_file
#include "atx/vol/surface_parity.hpp"          // SliceContext
#include "atx/vol/types.hpp"                   // Side, Result, Status
#include "atx/vol/vol_curve.hpp"               // CurveSurface, EssviCurve
#include "atx/vol/vol_surface.hpp"             // EssviParams

using namespace atx::vol;
namespace fs = std::filesystem;

namespace {

constexpr double kR = 0.043;
constexpr std::int64_t kBaseNow = 1700000000000000000LL;
constexpr std::int64_t kDayNs = 86400LL * 1000000000LL;
// 30 calendar days between sessions. Coarse on purpose: it keeps the fixture to
// seven archives while every residual tenor the strategy resolves on stays
// inside the synthetic surface's fitted pillar range [0.05, 1.0].
constexpr std::int64_t kStepNs = 30LL * kDayNs;
constexpr std::uint32_t kUid = 11;
constexpr std::uint32_t kDarkUid = 4243; // written only on the "board went dark" date
constexpr std::size_t kSessions = 7;
constexpr double kTargetDelta = 0.40;
constexpr double kContracts = 100.0;
constexpr const char *kSymbol = "XOM";
// The fixture's EXPLICIT spot path, at namespace scope because the Task 3 signal
// oracle hand-computes the engine's realized-variance accrual off these very
// numbers (r_i = ln(S_i/S_{i-1})) rather than reading it back off anything.
constexpr double kSpots[kSessions] = {100.0, 106.0, 96.0, 103.0, 111.0, 94.0, 101.0};

// A synthetic eSSVI PricedSurface (flat forward, genuine American premium via
// q_eff=0.02), slices T in [0.05, 1.0]. Mirrors backtest_swap_test's make_surface.
[[nodiscard]] PricedSurface make_surface(std::uint32_t uid, double S, std::int64_t now_ts) {
  CurveSurface cs;
  std::vector<SliceContext> ctx;
  const double Ts[] = {0.05, 0.10, 0.20, 0.35, 0.50, 0.75, 1.00};
  int i = 0;
  for (const double T : Ts) {
    const double term_forward = S * std::exp((kR - 0.02) * T);
    EssviParams e{};
    e.theta = 0.04 + 0.005 * static_cast<double>(i);
    e.phi = 1.5 - 0.05 * static_cast<double>(i);
    e.rho = -0.4 + 0.02 * static_cast<double>(i);
    e.psi = 0.5;
    e.p = 0.5;
    e.lambda = 0.5;
    e.T = T;
    e.F = term_forward;
    e.expiry_id = static_cast<std::uint16_t>(i);
    cs.push(std::make_unique<EssviCurve>(e, std::exp(-kR * T)));
    ctx.push_back(SliceContext{T, term_forward, 0.0, 0.02, 250, 7});
    ++i;
  }
  PricingContext pc;
  pc.S = S;
  pc.r = kR;
  pc.now_ts_ns = now_ts;
  pc.method = AmericanMethod::AndersenLake;
  pc.al_opts = al_fast_opts();
  pc.uid = uid;
  auto ps = PricedSurface::create(std::move(cs), std::move(ctx), pc);
  EXPECT_TRUE(ps.has_value()) << (ps.has_value() ? std::string{} : ps.error().to_string());
  return std::move(*ps);
}

[[nodiscard]] fs::path fresh_dir(const char *tag) {
  const fs::path dir = fs::temp_directory_path() / (std::string("atx-strvs-") + tag);
  std::error_code ec;
  fs::remove_all(dir, ec);
  return dir;
}

[[nodiscard]] std::string write_one(const fs::path &dir, const std::string &date,
                                    const std::string &symbol, const PricedSurface &s) {
  std::error_code ec;
  fs::create_directories(dir, ec);
  const std::string path = (dir / (date + ".atxvsa")).string();
  const SurfaceArchiveItem item{symbol, &s};
  const std::span<const SurfaceArchiveItem> items(&item, 1);
  const Status st = write_surface_archive_v2_file(path, items);
  EXPECT_TRUE(st.has_value()) << (st.has_value() ? std::string{} : st.error().to_string());
  return path;
}

[[nodiscard]] CorpusManifest
make_manifest(const std::vector<std::pair<std::string, std::string>> &date_paths) {
  CorpusManifest m;
  for (const auto &[date, path] : date_paths) {
    m.dates.push_back(date);
    CorpusEntry e;
    e.date = date;
    e.symbol = kSymbol;
    e.status = CorpusFitStatus::Ok;
    e.archive_path = path;
    m.entries.push_back(std::move(e));
  }
  return m;
}

struct Corpus {
  CorpusManifest manifest;
  std::vector<std::pair<std::string, std::string>> dp; // (date, path), ascending
  std::vector<std::int64_t> sessions;                  // the run's session grid
};

// Seven sessions kStepNs apart on an EXPLICIT drifting spot path, so each step's
// 40-delta strikes are genuinely different numbers. `dark_at` names the one date
// whose archive is written under a different symbol/uid — the name's board is
// simply absent that session.
[[nodiscard]] Corpus make_corpus(const fs::path &dir,
                                 std::size_t dark_at = static_cast<std::size_t>(-1)) {
  Corpus c;
  for (std::size_t d = 0; d < kSessions; ++d) {
    const std::int64_t now = kBaseNow + static_cast<std::int64_t>(d) * kStepNs;
    const bool dark = d == dark_at;
    const PricedSurface s = make_surface(dark ? kDarkUid : kUid, kSpots[d], now);
    char buf[16];
    std::snprintf(buf, sizeof buf, "2026-08-%02d", static_cast<int>(d) + 1);
    const std::string date = buf;
    c.dp.emplace_back(date, write_one(dir, date, dark ? "OTHER" : kSymbol, s));
    c.sessions.push_back(now);
  }
  c.manifest = make_manifest(c.dp);
  return c;
}

[[nodiscard]] StrangleVarswapConfig make_config(const Corpus &c, double tenor_years) {
  StrangleVarswapConfig cfg;
  cfg.symbol = kSymbol;
  cfg.target_abs_delta = kTargetDelta;
  cfg.tenor_years = tenor_years;
  cfg.contracts = kContracts;
  cfg.session_ts = c.sessions;
  return cfg;
}

// Run `strat` over refs[0..last_index] and hand back the engine's rows plus the
// post-step book at that ref. The strategy carries per-cycle state, so every
// prefix gets its own instance; the run is deterministic, so a prefix reproduces
// the corresponding steps of the full run exactly.
[[nodiscard]] Result<BacktestContinuation> run_prefix(const Clock &clock, const Corpus &c,
                                                      StrangleVsVarswapStrategy &strat,
                                                      std::size_t last_index,
                                                      const RunConfig &rcfg) {
  Result<Clock> sub = clock.between(c.dp.front().first, c.dp[last_index].first);
  if (!sub) {
    return atx::core::Err(sub.error());
  }
  return run_backtest_incremental(*sub, strat, rcfg, nullptr);
}

[[nodiscard]] const Lot *find_side(const PortfolioState &book, Side side) noexcept {
  for (const Lot &lot : book.lots) {
    if (lot.contract.side == side) {
      return &lot;
    }
  }
  return nullptr;
}

// INDEPENDENT delta oracle: reload the date's archive and ask the very surface
// the engine priced against what this lot's delta is. Nothing here reads the
// strategy.
[[nodiscard]] double lot_delta(const std::string &archive_path, const Lot &lot) {
  Result<MarketSnapshot> snap = MarketSnapshot::load(archive_path);
  EXPECT_TRUE(snap.has_value()) << (snap.has_value() ? std::string{} : snap.error().to_string());
  if (!snap) {
    return std::numeric_limits<double>::quiet_NaN();
  }
  const SurfaceRef s = snap->find(lot.contract.uid);
  EXPECT_NE(s, nullptr);
  if (s == nullptr) {
    return std::numeric_limits<double>::quiet_NaN();
  }
  const Result<double> d = s->delta(lot.contract.K, lot.contract.T, lot.contract.side);
  EXPECT_TRUE(d.has_value()) << (d.has_value() ? std::string{} : d.error().to_string());
  return d.has_value() ? *d : std::numeric_limits<double>::quiet_NaN();
}

// ── Task 2 oracles: the swap leg, never read off the strategy ───────────────

[[nodiscard]] bool bits_equal(double a, double b) noexcept {
  std::uint64_t ba = 0;
  std::uint64_t bb = 0;
  std::memcpy(&ba, &a, sizeof ba);
  std::memcpy(&bb, &b, sizeof bb);
  return ba == bb;
}

// The `DerivContract` a swap lot IS on the snapshot at `ts_ns`, rebuilt from the
// lot's own terms under the engine's documented conventions: residual tenor over
// `kNsPerYear`, and an `rv_spec` carrying the lot's schedule with the caller's
// hand-computed accrual staged into it. This is backtest_swap_test's
// `reference_swap_mark` construction; nothing here reads the strategy.
//
// `n_obs_done == 0` with a zero sum IS an entry-day (or seed-day) contract: the
// engine's swap pass first sees a lot on the step AFTER it was opened, and that
// step only seeds the fixing series.
[[nodiscard]] DerivContract live_contract(const SwapLot &lot, std::int64_t ts_ns,
                                          std::uint32_t n_obs_done,
                                          double sum_sq_log_returns_done) noexcept {
  RealizedVarianceSpec rv{};
  rv.annualization = lot.annualization;
  rv.n_obs_total = lot.n_obs_total;
  rv.n_obs_done = n_obs_done;
  rv.sum_sq_log_returns_done = sum_sq_log_returns_done;
  rv.rv_done_dec = n_obs_done == 0u ? 0.0
                                    : lot.annualization * sum_sq_log_returns_done /
                                          static_cast<double>(n_obs_done);

  DerivContract c;
  c.kind = lot.kind;
  c.maturity_t = static_cast<double>(lot.expiry_ts_ns - ts_ns) / kNsPerYear;
  c.strike_dec = lot.strike_dec;
  c.cap_dec = lot.cap_dec;
  c.notional = lot.notional;
  c.rv_spec = rv;
  return c;
}

// Independent greeks for that contract, against the very archive the engine
// priced and through the same `SurfaceRef` bridge the engine's mark lane prices
// through (`step_swap_lots` -> `deriv_price_on_ref`).
[[nodiscard]] Result<DerivGreeks> swap_greeks_at(const std::string &archive_path,
                                                 const SwapLot &lot, std::int64_t ts_ns,
                                                 std::uint32_t n_obs_done,
                                                 double sum_sq_log_returns_done) {
  Result<MarketSnapshot> snap = MarketSnapshot::load(archive_path);
  if (!snap) {
    return atx::core::Err(snap.error());
  }
  const SurfaceRef s = snap->find(lot.uid);
  if (s == nullptr) {
    return atx::core::Err(atx::core::ErrorCode::NotFound,
                          "oracle: the swap lot's uid has no surface on that date");
  }
  return detail::deriv_greeks_on_ref(s,
                                     live_contract(lot, ts_ns, n_obs_done, sum_sq_log_returns_done),
                                     DerivConfig{}, DerivGreekBumps{});
}

[[nodiscard]] Result<DerivGreeks> entry_swap_greeks(const std::string &archive_path,
                                                    const SwapLot &lot, std::int64_t ts_ns) {
  return swap_greeks_at(archive_path, lot, ts_ns, /*n_obs_done=*/0u,
                        /*sum_sq_log_returns_done=*/0.0);
}

// ── Task 3 oracles: the per-step signal columns ─────────────────────────────

[[nodiscard]] const std::vector<double> *signal_series(const BacktestResult &r, const char *name) {
  for (const auto &[series_name, values] : r.signals) {
    if (series_name == name) {
      return &values;
    }
  }
  return nullptr;
}

// Relative agreement with a floor of 1.0 on the denominator, so a genuinely tiny
// greek (a variance swap's delta) is compared on an absolute scale rather than
// against a vanishing divisor.
void expect_close(double got, double want, double rel, const char *what, std::size_t row) {
  EXPECT_LE(std::fabs(got - want), rel * std::max(std::fabs(want), 1.0))
      << what << " row " << row << ": got " << got << ", want " << want;
}

// Every swap-leg signal on `row` is NaN — the documented "no live swap" state.
// EXACTLY NaN, never 0.0: a 0.0 there reads as a measured flat position.
void expect_swap_signals_nan(const BacktestResult &r, std::size_t row) {
  static constexpr const char *kSwapNames[] = {"swap_delta", "swap_gamma", "swap_vega",
                                               "swap_theta", "swap_rho"};
  for (const char *name : kSwapNames) {
    const std::vector<double> *s = signal_series(r, name);
    ASSERT_NE(s, nullptr) << name;
    ASSERT_EQ(s->size(), r.size()) << name;
    EXPECT_TRUE(std::isnan((*s)[row])) << name << " row " << row << ": " << (*s)[row];
  }
}

// The option book's DOLLAR vega per 1.00 of vol: each lot's per-share American
// vega at its OWN (K, T, side) on that date's surface, scaled by qty x
// multiplier exactly as the portfolio pricer scales a position's greeks
// (`out.vega[i] = w * g.vega`, portfolio_pricer.cpp). NaN — never 0.0 — if
// anything fails to price.
[[nodiscard]] double book_option_vega(const std::string &archive_path, const PortfolioState &book,
                                      const PriceOptions &price_options) {
  const double nan = std::numeric_limits<double>::quiet_NaN();
  Result<MarketSnapshot> snap = MarketSnapshot::load(archive_path);
  EXPECT_TRUE(snap.has_value()) << (snap.has_value() ? std::string{} : snap.error().to_string());
  if (!snap) {
    return nan;
  }
  double total = 0.0;
  for (const Lot &lot : book.lots) {
    const SurfaceRef s = snap->find(lot.contract.uid);
    EXPECT_NE(s, nullptr);
    if (s == nullptr) {
      return nan;
    }
    const Result<FullGreekSeed> seed =
        s->full_greek_seed(lot.contract.K, lot.contract.T, lot.contract.side,
                           price_options.analytic_greeks, price_options.query_execution);
    EXPECT_TRUE(seed.has_value()) << (seed.has_value() ? std::string{} : seed.error().to_string());
    if (!seed) {
      return nan;
    }
    total += seed->greeks().vega * lot.qty * lot.multiplier;
  }
  return total;
}

// Every column the OPTION lane owns, compared bit-for-bit. `pnl_total`, `nav`
// and `cash` are deliberately NOT here: the swap lane folds its mark-to-market
// into the step total and its settlement into the ledger, so those three
// legitimately differ between a one-legged run and a two-legged one — which is
// the whole point of the lane, not a violation of its additivity.
void expect_option_columns_equal(const BacktestResult &a, const BacktestResult &b) {
  ASSERT_EQ(a.size(), b.size());
  struct Column {
    const char *name;
    const std::vector<double> BacktestResult::*member;
  };
  static constexpr Column kColumns[] = {
      {"pnl_delta", &BacktestResult::pnl_delta},
      {"pnl_gamma", &BacktestResult::pnl_gamma},
      {"pnl_vega", &BacktestResult::pnl_vega},
      {"pnl_vanna", &BacktestResult::pnl_vanna},
      {"pnl_volga", &BacktestResult::pnl_volga},
      {"pnl_theta", &BacktestResult::pnl_theta},
      {"pnl_rho", &BacktestResult::pnl_rho},
      {"pnl_charm", &BacktestResult::pnl_charm},
      {"pnl_unexplained", &BacktestResult::pnl_unexplained},
      {"pnl_settlement", &BacktestResult::pnl_settlement},
      {"pnl_shares", &BacktestResult::pnl_shares},
      {"financing", &BacktestResult::financing},
      {"cost", &BacktestResult::cost},
      {"gross_delta", &BacktestResult::gross_delta},
      {"gross_gamma", &BacktestResult::gross_gamma},
      {"gross_vega", &BacktestResult::gross_vega},
      {"gross_theta", &BacktestResult::gross_theta},
      {"turnover_notional", &BacktestResult::turnover_notional},
      {"turnover_vega", &BacktestResult::turnover_vega},
      {"n_open_lots", &BacktestResult::n_open_lots},
      {"n_unpriced_lots", &BacktestResult::n_unpriced_lots},
      {"n_unpriced_greeks", &BacktestResult::n_unpriced_greeks},
  };
  for (const Column &col : kColumns) {
    const std::vector<double> &lhs = a.*(col.member);
    const std::vector<double> &rhs = b.*(col.member);
    ASSERT_EQ(lhs.size(), a.size()) << col.name;
    ASSERT_EQ(rhs.size(), b.size()) << col.name;
    for (std::size_t i = 0; i < lhs.size(); ++i) {
      EXPECT_TRUE(bits_equal(lhs[i], rhs[i]))
          << col.name << " row " << i << ": " << lhs[i] << " vs " << rhs[i];
    }
  }
}

// Two option books hold the same POSITIONS, ignoring lot ids: the swap lane
// draws from the same monotonic id watermark, so enabling it shifts every
// subsequent option id without changing a single economic term.
void expect_same_option_positions(const PortfolioState &a, const PortfolioState &b) {
  ASSERT_EQ(a.lots.size(), b.lots.size());
  for (std::size_t i = 0; i < a.lots.size(); ++i) {
    const Lot &x = a.lots[i];
    const Lot &y = b.lots[i];
    EXPECT_EQ(x.contract.uid, y.contract.uid) << i;
    EXPECT_EQ(x.contract.side, y.contract.side) << i;
    EXPECT_TRUE(bits_equal(x.contract.K, y.contract.K)) << i;
    EXPECT_TRUE(bits_equal(x.contract.T, y.contract.T)) << i;
    EXPECT_TRUE(bits_equal(x.qty, y.qty)) << i;
    EXPECT_TRUE(bits_equal(x.multiplier, y.multiplier)) << i;
    EXPECT_TRUE(bits_equal(x.entry_price, y.entry_price)) << i;
    EXPECT_EQ(x.expiry_ts_ns, y.expiry_ts_ns) << i;
    EXPECT_EQ(x.cohort, y.cohort) << i;
  }
}

} // namespace

// ── 1. Inception: a 40-delta pair on the hand-computed snapped expiry ────────
TEST(StrangleVarswap, OpensFortyDeltaStrangleAtSnappedFixedExpiry) {
  const fs::path dir = fresh_dir("open");
  const Corpus c = make_corpus(dir);
  auto clock = Clock::from_manifest(c.manifest);
  ASSERT_TRUE(clock.has_value()) << clock.error().to_string();

  // 0.25y == 91.3125 calendar days on the library's 365.25-day year. The session
  // grid is 30 calendar days apart from kBaseNow, so the FIRST session at or
  // after the raw anchor is day 120 — ref index 4. Hand-computed, not derived
  // from the strategy.
  const std::int64_t expected_expiry = kBaseNow + 4LL * kStepNs;

  StrangleVsVarswapStrategy strat{make_config(c, 0.25)};
  auto out = run_prefix(*clock, c, strat, /*last_index=*/0u, RunConfig{});
  ASSERT_TRUE(out.has_value()) << out.error().to_string();
  EXPECT_EQ(strat.unresolved_strike_steps(), 0u);
  EXPECT_EQ(strat.cycle_expiry_ts_ns(), expected_expiry);
  const PortfolioState &book = out->checkpoint.portfolio;
  ASSERT_EQ(book.lots.size(), 2u);
  // The swap leg opens on this same step; its terms are gated by
  // OpensEqualVegaVarSwapAtCycleStart below.
  EXPECT_EQ(book.swap_lots.size(), 1u);

  const Lot *call = find_side(book, Side::Call);
  const Lot *put = find_side(book, Side::Put);
  ASSERT_NE(call, nullptr);
  ASSERT_NE(put, nullptr);

  EXPECT_EQ(call->expiry_ts_ns, expected_expiry);
  EXPECT_EQ(put->expiry_ts_ns, expected_expiry);
  EXPECT_EQ(call->qty, kContracts);
  EXPECT_EQ(put->qty, kContracts);
  // OTM wings straddle the spot: the call strike is above the put strike.
  EXPECT_GT(call->contract.K, put->contract.K);

  EXPECT_NEAR(lot_delta(c.dp[0].second, *call), kTargetDelta, 0.02);
  EXPECT_NEAR(lot_delta(c.dp[0].second, *put), -kTargetDelta, 0.02);
}

// ── 2. Daily restrike at a FIXED expiry, with no lot accumulation ────────────
TEST(StrangleVarswap, RestrikesDailyAtFixedExpiry) {
  const fs::path dir = fresh_dir("restrike");
  const Corpus c = make_corpus(dir);
  auto clock = Clock::from_manifest(c.manifest);
  ASSERT_TRUE(clock.has_value()) << clock.error().to_string();
  const StrangleVarswapConfig scfg = make_config(c, 0.25);
  const std::int64_t expected_expiry = kBaseNow + 4LL * kStepNs;
  const RunConfig rcfg;

  double prev_call_K = 0.0;
  double prev_put_K = 0.0;
  double prev_strangle_vega = 0.0;
  for (std::size_t step = 0; step < 3u; ++step) {
    StrangleVsVarswapStrategy strat{scfg};
    auto out = run_prefix(*clock, c, strat, step, rcfg);
    ASSERT_TRUE(out.has_value()) << "step " << step << ": " << out.error().to_string();
    EXPECT_EQ(strat.unresolved_strike_steps(), 0u) << "step " << step;
    const PortfolioState &book = out->checkpoint.portfolio;
    // Exactly two option lots every step: the prior pair was CLOSED, not kept.
    ASSERT_EQ(book.lots.size(), 2u) << "step " << step;
    // Two fresh lot ids per step, so no id was reused and none was retained —
    // plus the ONE the cycle-open step spent on this cycle's swap lot (the
    // watermark is shared between the two lanes).
    EXPECT_EQ(out->checkpoint.next_lot_id, 1u + 2u * (step + 1u) + 1u) << "step " << step;

    const Lot *call = find_side(book, Side::Call);
    const Lot *put = find_side(book, Side::Put);
    ASSERT_NE(call, nullptr) << "step " << step;
    ASSERT_NE(put, nullptr) << "step " << step;

    // The expiry is FIXED for the whole cycle; only the strikes move.
    EXPECT_EQ(call->expiry_ts_ns, expected_expiry) << "step " << step;
    EXPECT_EQ(put->expiry_ts_ns, expected_expiry) << "step " << step;
    // Each restrike re-hits the target on THAT step's surface.
    EXPECT_NEAR(lot_delta(c.dp[step].second, *call), kTargetDelta, 0.02) << "step " << step;
    EXPECT_NEAR(lot_delta(c.dp[step].second, *put), -kTargetDelta, 0.02) << "step " << step;

    // Task 3 contract 2: `strangle_vega` describes THIS step's RESTRUCK book,
    // priced on THIS step's surface. The oracle re-prices the engine's own post-
    // step lots off the archive; it never reads the strategy. And because the
    // strikes moved above, the value has to move with them — a signal computed
    // once at cycle open (or carried from the previous step) fails here.
    const std::vector<double> *sv = signal_series(out->rows, "strangle_vega");
    ASSERT_NE(sv, nullptr) << "step " << step;
    ASSERT_EQ(sv->size(), out->rows.size()) << "step " << step;
    const double vega_oracle = book_option_vega(c.dp[step].second, book, rcfg.price);
    ASSERT_TRUE(std::isfinite(vega_oracle)) << "step " << step;
    ASSERT_GT(vega_oracle, 0.0) << "step " << step;
    expect_close((*sv)[step], vega_oracle, 1.0e-9, "strangle_vega", step);

    if (step > 0u) {
      EXPECT_GT(std::fabs(call->contract.K - prev_call_K), 1.0e-6) << "step " << step;
      EXPECT_GT(std::fabs(put->contract.K - prev_put_K), 1.0e-6) << "step " << step;
      EXPECT_GT(std::fabs((*sv)[step] - prev_strangle_vega), 1.0e-6 * prev_strangle_vega)
          << "step " << step;
    }
    prev_call_K = call->contract.K;
    prev_put_K = put->contract.K;
    prev_strangle_vega = (*sv)[step];
  }
}

// ── 3. The cycle settles at its expiry and the next one opens the same step ──
TEST(StrangleVarswap, RollsIntoNewCycleAtExpiry) {
  const fs::path dir = fresh_dir("roll");
  const Corpus c = make_corpus(dir);
  auto clock = Clock::from_manifest(c.manifest);
  ASSERT_TRUE(clock.has_value()) << clock.error().to_string();
  // 50 calendar days: the raw anchor from ref 0 lands between session 1 (day 30)
  // and session 2 (day 60), so cycle 1 expires on ref 2. From ref 2 the anchor is
  // day 110, between session 3 (day 90) and session 4 (day 120) — cycle 2 fixes
  // on ref 4.
  const StrangleVarswapConfig scfg = make_config(c, 50.0 / 365.25);
  const std::int64_t cycle1_expiry = kBaseNow + 2LL * kStepNs;
  const std::int64_t cycle2_expiry = kBaseNow + 4LL * kStepNs;

  StrangleVsVarswapStrategy before_strat{scfg};
  auto before = run_prefix(*clock, c, before_strat, /*last_index=*/1u, RunConfig{});
  ASSERT_TRUE(before.has_value()) << before.error().to_string();
  ASSERT_EQ(before->checkpoint.portfolio.lots.size(), 2u);
  for (const Lot &lot : before->checkpoint.portfolio.lots) {
    EXPECT_EQ(lot.expiry_ts_ns, cycle1_expiry);
  }
  const std::uint64_t ids_before = before->checkpoint.next_lot_id;

  StrangleVsVarswapStrategy after_strat{scfg};
  auto after = run_prefix(*clock, c, after_strat, /*last_index=*/2u, RunConfig{});
  ASSERT_TRUE(after.has_value()) << after.error().to_string();
  const PortfolioState &book = after->checkpoint.portfolio;
  // The expired pair settled (engine-owned) and a NEW pair replaced it.
  ASSERT_EQ(book.lots.size(), 2u);
  const Lot *call = find_side(book, Side::Call);
  const Lot *put = find_side(book, Side::Put);
  ASSERT_NE(call, nullptr);
  ASSERT_NE(put, nullptr);
  EXPECT_EQ(call->expiry_ts_ns, cycle2_expiry);
  EXPECT_EQ(put->expiry_ts_ns, cycle2_expiry);
  EXPECT_GT(cycle2_expiry, cycle1_expiry);
  // Fresh ids: the new pair is not the settled one carried forward.
  EXPECT_GE(call->id, ids_before);
  EXPECT_GE(put->id, ids_before);
  // The 40-delta target is honored on the new cycle's tenor too.
  EXPECT_NEAR(lot_delta(c.dp[2].second, *call), kTargetDelta, 0.02);
  EXPECT_NEAR(lot_delta(c.dp[2].second, *put), -kTargetDelta, 0.02);
}

// ── 4. The engine-owned hedge request ───────────────────────────────────────
TEST(StrangleVarswap, HedgeSpecIsDeltaToZeroDaily) {
  StrangleVsVarswapStrategy strat{StrangleVarswapConfig{}};
  const HedgeSpec hedge = strat.hedge_spec();
  EXPECT_EQ(hedge.kind, HedgeSpec::Kind::DeltaToZero);
  EXPECT_EQ(hedge.cadence, HedgeSpec::Cadence::Daily);
  EXPECT_EQ(hedge.band, 0.0);
}

// ── 5. An unusable surface keeps the live strikes, and never fabricates one ──
TEST(StrangleVarswap, KeepsStrikesWhenSurfaceCannotServeDelta) {
  const fs::path dir = fresh_dir("nodelta");
  // Ref 2's archive carries a different name entirely: there is no surface to
  // resolve a 40-delta strike on that session.
  const Corpus c = make_corpus(dir, /*dark_at=*/2u);
  auto clock = Clock::from_manifest(c.manifest);
  ASSERT_TRUE(clock.has_value()) << clock.error().to_string();
  StrangleVarswapConfig scfg = make_config(c, 0.25);
  // The OPTIONS leg in isolation. The engine's swap lane fails CLOSED on a
  // missing surface — a live swap lot can neither be marked nor take its fixing,
  // and the whole run aborts (`step_swap_lots`, backtest.cpp) — so a fixture
  // whose entire point is a dark session has to run one-legged. The swap leg's
  // own dark-session behaviour is gated by SkipsSwapWhenVegaUnavailable.
  scfg.enable_swap_leg = false;
  RunConfig rcfg;
  // The held pair cannot be valued on the dark session either; that is the
  // documented ExcludeAndReport lane, and it is what keeps the run going so the
  // strategy's own behaviour is observable.
  rcfg.unpriced = UnpricedLotPolicy::ExcludeAndReport;

  StrangleVsVarswapStrategy live_strat{scfg};
  auto live = run_prefix(*clock, c, live_strat, /*last_index=*/1u, rcfg);
  ASSERT_TRUE(live.has_value()) << live.error().to_string();
  EXPECT_EQ(live_strat.unresolved_strike_steps(), 0u);
  ASSERT_EQ(live->checkpoint.portfolio.lots.size(), 2u);
  const Lot *live_call = find_side(live->checkpoint.portfolio, Side::Call);
  const Lot *live_put = find_side(live->checkpoint.portfolio, Side::Put);
  ASSERT_NE(live_call, nullptr);
  ASSERT_NE(live_put, nullptr);
  const double held_call_K = live_call->contract.K;
  const double held_put_K = live_put->contract.K;
  const std::uint64_t ids_before_dark = live->checkpoint.next_lot_id;

  // The dark session: the run CONTINUES and the book is untouched.
  StrangleVsVarswapStrategy dark_strat{scfg};
  auto dark = run_prefix(*clock, c, dark_strat, /*last_index=*/2u, rcfg);
  ASSERT_TRUE(dark.has_value()) << dark.error().to_string();
  // Counted, never silent: exactly the one session whose board was missing.
  EXPECT_EQ(dark_strat.unresolved_strike_steps(), 1u);
  const PortfolioState &dark_book = dark->checkpoint.portfolio;
  ASSERT_EQ(dark_book.lots.size(), 2u);
  const Lot *dark_call = find_side(dark_book, Side::Call);
  const Lot *dark_put = find_side(dark_book, Side::Put);
  ASSERT_NE(dark_call, nullptr);
  ASSERT_NE(dark_put, nullptr);
  // EXACT, not near: the strikes are the previous step's, never re-derived.
  EXPECT_EQ(dark_call->contract.K, held_call_K);
  EXPECT_EQ(dark_put->contract.K, held_put_K);
  EXPECT_EQ(dark_call->id, live_call->id);
  EXPECT_EQ(dark_put->id, live_put->id);
  // No reopen churn: the id watermark did not move on the dark step.
  EXPECT_EQ(dark->checkpoint.next_lot_id, ids_before_dark);
  // The dark row really was unvaluable — the fixture is not vacuously green.
  ASSERT_EQ(dark->rows.size(), 3u);
  EXPECT_GT(dark->rows.n_unpriced_greeks.back(), 0.0);

  // The board comes back on ref 3 and the restrike resumes.
  StrangleVsVarswapStrategy back_strat{scfg};
  auto back = run_prefix(*clock, c, back_strat, /*last_index=*/3u, rcfg);
  ASSERT_TRUE(back.has_value()) << back.error().to_string();
  EXPECT_EQ(back_strat.unresolved_strike_steps(), 1u); // still only the dark session
  ASSERT_EQ(back->checkpoint.portfolio.lots.size(), 2u);
  const Lot *back_call = find_side(back->checkpoint.portfolio, Side::Call);
  const Lot *back_put = find_side(back->checkpoint.portfolio, Side::Put);
  ASSERT_NE(back_call, nullptr);
  ASSERT_NE(back_put, nullptr);
  EXPECT_GT(std::fabs(back_call->contract.K - held_call_K), 1.0e-6);
  EXPECT_GT(std::fabs(back_put->contract.K - held_put_K), 1.0e-6);
  EXPECT_GT(back->checkpoint.next_lot_id, ids_before_dark);
}

// ── 6. The grid boundary: a short final cycle, then nothing ─────────────────
TEST(StrangleVarswap, FixesTheLastSessionWhenTheTenorOutrunsTheGrid) {
  const fs::path dir = fresh_dir("gridend");
  const Corpus c = make_corpus(dir);
  auto clock = Clock::from_manifest(c.manifest);
  ASSERT_TRUE(clock.has_value()) << clock.error().to_string();
  const StrangleVarswapConfig scfg = make_config(c, 0.25);

  // Cycle 1 expires on ref 4 (day 120). From there the raw anchor is day 211.3,
  // past the last session (day 180) — so cycle 2 takes the LAST session rather
  // than an expiry the corpus never reaches.
  StrangleVsVarswapStrategy tail_strat{scfg};
  auto tail = run_prefix(*clock, c, tail_strat, /*last_index=*/4u, RunConfig{});
  ASSERT_TRUE(tail.has_value()) << tail.error().to_string();
  ASSERT_EQ(tail->checkpoint.portfolio.lots.size(), 2u);
  for (const Lot &lot : tail->checkpoint.portfolio.lots) {
    EXPECT_EQ(lot.expiry_ts_ns, kBaseNow + 6LL * kStepNs);
  }

  // On the last session itself there is no session left to expire into, so the
  // settled cycle is not replaced — the book goes flat rather than opening a lot
  // the run can never settle.
  StrangleVsVarswapStrategy end_strat{scfg};
  auto end = run_prefix(*clock, c, end_strat, /*last_index=*/kSessions - 1u, RunConfig{});
  ASSERT_TRUE(end.has_value()) << end.error().to_string();
  EXPECT_TRUE(end->checkpoint.portfolio.lots.empty());
  // The final cycle's swap settled on that same session and was erased with it.
  EXPECT_TRUE(end->checkpoint.portfolio.swap_lots.empty());
  EXPECT_EQ(end_strat.cycle_expiry_ts_ns(), 0);
  EXPECT_EQ(end_strat.unresolved_strike_steps(), 0u);
  EXPECT_EQ(end_strat.skipped_swap_cycles(), 0u);
}

// ── 7. The swap leg: ONE equal-vega uncapped var swap per cycle ─────────────
//
// Contracts 1-3 of the Task 2 brief in one gate, because they are one behaviour:
// the swap is a per-CYCLE instrument. It opens on the step that fixes the cycle
// (never on a restrike), it carries that cycle's own expiry, and it is sized so
// its vega equals the strangle's at that instant. The vega and fair-strike
// oracles are independent `deriv_greeks` / `full_greek_seed` calls against the
// same archives — the strategy's own numbers are never read back.
TEST(StrangleVarswap, OpensEqualVegaVarSwapAtCycleStart) {
  const fs::path dir = fresh_dir("swapopen");
  const Corpus c = make_corpus(dir);
  auto clock = Clock::from_manifest(c.manifest);
  ASSERT_TRUE(clock.has_value()) << clock.error().to_string();
  const StrangleVarswapConfig scfg = make_config(c, 0.25); // the swap leg is on by default
  const std::int64_t cycle1_expiry = kBaseNow + 4LL * kStepNs;
  const RunConfig rcfg;

  StrangleVsVarswapStrategy strat{scfg};
  auto out = run_prefix(*clock, c, strat, /*last_index=*/0u, rcfg);
  ASSERT_TRUE(out.has_value()) << out.error().to_string();
  EXPECT_EQ(strat.skipped_swap_cycles(), 0u);
  const PortfolioState &book = out->checkpoint.portfolio;
  ASSERT_EQ(book.lots.size(), 2u);
  ASSERT_EQ(book.swap_lots.size(), 1u);
  const SwapLot swap = book.swap_lots.front();

  // Terms: an UNCAPPED variance swap on the strangle's own name, unit notional
  // (the leg is sized purely through `qty`), 252-day annualization.
  EXPECT_EQ(swap.kind, DerivKind::VarSwap);
  EXPECT_TRUE(bits_equal(swap.cap_dec, 0.0));
  EXPECT_TRUE(bits_equal(swap.notional, 1.0));
  EXPECT_TRUE(bits_equal(swap.annualization, 252.0));
  EXPECT_EQ(swap.uid, book.lots.front().contract.uid);
  EXPECT_EQ(swap.start_ts_ns, kBaseNow);

  // Contract 3: the SAME snapped expiry as both option wings — the comparison is
  // about vol path, so a calendar difference between the legs would be a confound.
  EXPECT_EQ(swap.expiry_ts_ns, cycle1_expiry);
  for (const Lot &lot : book.lots) {
    EXPECT_EQ(lot.expiry_ts_ns, swap.expiry_ts_ns);
  }
  // The fixing schedule the engine will really observe: sessions in (open,
  // expiry] are refs 1..4, and the FIRST of those only SEEDS the series (a lot
  // needs one step to seed and one more per accrued return), so three returns
  // are actually accrued.
  EXPECT_EQ(swap.n_obs_total, 3u);

  // The strike is FAIR under the very pricer the engine marks with: priced at
  // the lot's own terms on its own open date, the contract is worth exactly
  // nothing. Anything else would be an entry artifact, and the whole of it would
  // land in the first step's `swap_pnl` (swaps open at zero cost).
  const Result<DerivGreeks> g = entry_swap_greeks(c.dp[0].second, swap, kBaseNow);
  ASSERT_TRUE(g.has_value()) << g.error().to_string();
  EXPECT_TRUE(bits_equal(g->pv, 0.0));
  EXPECT_TRUE(bits_equal(swap.strike_dec, g->quote.fair_strike_dec));
  EXPECT_GT(swap.strike_dec, 0.0);

  // Contract 2: EQUAL VEGA AT INCEPTION. Both sides are dollars per 1.00 of
  // parallel vol — the option book's per-share American vega scaled by qty x
  // multiplier, and the swap's notional-scaled finite-difference vega.
  const double strangle_vega = book_option_vega(c.dp[0].second, book, rcfg.price);
  ASSERT_TRUE(std::isfinite(strangle_vega));
  ASSERT_GT(strangle_vega, 0.0);
  ASSERT_TRUE(std::isfinite(g->vega));
  ASSERT_NE(g->vega, 0.0);
  EXPECT_GT(swap.qty, 0.0); // long swap against a long-vega strangle: both long vol
  EXPECT_LE(std::fabs(swap.qty * g->vega - strangle_vega), 1.0e-9 * strangle_vega);

  // Contract 1: opened ONCE. Every restrike step inside the cycle carries the
  // identical lot — same id, same terms, bit for bit (the append-only swap-lot
  // contract makes any other outcome an engine-level error, so this also pins
  // that the strategy never re-sizes or re-strikes the leg intra-cycle).
  for (std::size_t step = 1u; step <= 3u; ++step) {
    StrangleVsVarswapStrategy held{scfg};
    auto r = run_prefix(*clock, c, held, step, rcfg);
    ASSERT_TRUE(r.has_value()) << "step " << step << ": " << r.error().to_string();
    ASSERT_EQ(r->checkpoint.portfolio.swap_lots.size(), 1u) << "step " << step;
    EXPECT_TRUE(r->checkpoint.portfolio.swap_lots.front() == swap) << "step " << step;
    EXPECT_EQ(held.skipped_swap_cycles(), 0u) << "step " << step;
  }

  // At the cycle's expiry the swap SETTLES (engine-owned) and the NEXT cycle
  // opens its own on that same step, at the new expiry, with a fresh id.
  StrangleVsVarswapStrategy roll{scfg};
  auto rolled = run_prefix(*clock, c, roll, /*last_index=*/4u, rcfg);
  ASSERT_TRUE(rolled.has_value()) << rolled.error().to_string();
  ASSERT_EQ(rolled->checkpoint.portfolio.swap_lots.size(), 1u);
  const SwapLot &next = rolled->checkpoint.portfolio.swap_lots.front();
  EXPECT_GT(next.id, swap.id);
  EXPECT_EQ(next.start_ts_ns, kBaseNow + 4LL * kStepNs);
  EXPECT_EQ(next.expiry_ts_ns, kBaseNow + 6LL * kStepNs); // the short final cycle
  EXPECT_EQ(next.n_obs_total, 1u);                        // (ref4, ref6] = 2 sessions, one seeds
  ASSERT_EQ(rolled->checkpoint.portfolio.lots.size(), 2u);
  for (const Lot &lot : rolled->checkpoint.portfolio.lots) {
    EXPECT_EQ(lot.expiry_ts_ns, next.expiry_ts_ns);
  }
  EXPECT_EQ(roll.skipped_swap_cycles(), 0u);
}

// ── 8. The swap leg is ADDITIVE: disabling it reproduces the options leg ────
TEST(StrangleVarswap, SwapLegDisabledMatchesOptionsOnly) {
  const fs::path dir = fresh_dir("swapoff");
  const Corpus c = make_corpus(dir);
  auto clock = Clock::from_manifest(c.manifest);
  ASSERT_TRUE(clock.has_value()) << clock.error().to_string();
  const RunConfig rcfg;

  const StrangleVarswapConfig on_cfg = make_config(c, 0.25);
  StrangleVarswapConfig off_cfg = on_cfg;
  off_cfg.enable_swap_leg = false;

  StrangleVsVarswapStrategy on_strat{on_cfg};
  auto on = run_backtest_incremental(*clock, on_strat, rcfg, nullptr);
  ASSERT_TRUE(on.has_value()) << on.error().to_string();
  StrangleVsVarswapStrategy off_strat{off_cfg};
  auto off = run_backtest_incremental(*clock, off_strat, rcfg, nullptr);
  ASSERT_TRUE(off.has_value()) << off.error().to_string();
  ASSERT_EQ(on->rows.size(), kSessions);
  ASSERT_EQ(off->rows.size(), kSessions);

  // The disabled run books no swap at all: no lot ever, and both swap columns
  // exactly +0.0 on every row (the engine early-outs on an empty swap book, so
  // these are untouched zeros, not marks that rounded to zero).
  EXPECT_TRUE(off->checkpoint.portfolio.swap_lots.empty());
  ASSERT_EQ(off->rows.swap_pv.size(), off->rows.size());
  ASSERT_EQ(off->rows.swap_pnl.size(), off->rows.size());
  for (std::size_t i = 0; i < off->rows.size(); ++i) {
    EXPECT_TRUE(bits_equal(off->rows.swap_pv[i], 0.0)) << "row " << i;
    EXPECT_TRUE(bits_equal(off->rows.swap_pnl[i], 0.0)) << "row " << i;
  }
  EXPECT_EQ(off_strat.skipped_swap_cycles(), 0u); // a leg nobody asked for is not a skip

  // ... while the ENABLED run genuinely did, so this A/B is not vacuous.
  bool any_swap_mark = false;
  for (const double pv : on->rows.swap_pv) {
    any_swap_mark = any_swap_mark || pv != 0.0;
  }
  EXPECT_TRUE(any_swap_mark);

  // And the option lane is bit-for-bit the same run either way.
  expect_option_columns_equal(on->rows, off->rows);
  expect_same_option_positions(on->checkpoint.portfolio, off->checkpoint.portfolio);
  EXPECT_EQ(on_strat.unresolved_strike_steps(), off_strat.unresolved_strike_steps());
  EXPECT_EQ(on_strat.cycle_expiry_ts_ns(), off_strat.cycle_expiry_ts_ns());
}

// ── 9. No vega, no swap — never a fabricated quantity ──────────────────────
TEST(StrangleVarswap, SkipsSwapWhenVegaUnavailable) {
  const fs::path dir = fresh_dir("swapskip");
  const Corpus c = make_corpus(dir);
  auto clock = Clock::from_manifest(c.manifest);
  ASSERT_TRUE(clock.has_value()) << clock.error().to_string();
  const RunConfig rcfg;

  // Arm A — the OPTION surface is perfectly usable but the derivative pricer
  // cannot be reached at all: a reserved `DerivConfig` field is non-zero, which
  // every derivatives entry rejects with NotImplemented. So the entry solve
  // fails at each of the two cycle opens, and no vega exists to size against.
  StrangleVarswapConfig blind_cfg = make_config(c, 0.25);
  blind_cfg.deriv_cfg.abs_price_tol = 1.0;

  StrangleVsVarswapStrategy blind{blind_cfg};
  auto out = run_backtest_incremental(*clock, blind, rcfg, nullptr);
  ASSERT_TRUE(out.has_value()) << out.error().to_string(); // FAIL-SOFT: the run survives
  EXPECT_EQ(blind.skipped_swap_cycles(), 2u);              // counted, never silent
  EXPECT_EQ(blind.unresolved_strike_steps(), 0u);          // the OPTION leg was fine throughout
  EXPECT_TRUE(out->checkpoint.portfolio.swap_lots.empty());
  for (std::size_t i = 0; i < out->rows.size(); ++i) {
    EXPECT_TRUE(bits_equal(out->rows.swap_pv[i], 0.0)) << "row " << i;
    EXPECT_TRUE(bits_equal(out->rows.swap_pnl[i], 0.0)) << "row " << i;
  }

  // Never a garbage qty: the options leg ran exactly as it does with the swap
  // leg switched off, so nothing partial was booked and nothing was disturbed.
  StrangleVarswapConfig off_cfg = make_config(c, 0.25);
  off_cfg.enable_swap_leg = false;
  StrangleVsVarswapStrategy off_strat{off_cfg};
  auto off = run_backtest_incremental(*clock, off_strat, rcfg, nullptr);
  ASSERT_TRUE(off.has_value()) << off.error().to_string();
  expect_option_columns_equal(out->rows, off->rows);
  expect_same_option_positions(out->checkpoint.portfolio, off->checkpoint.portfolio);

  // Arm B — the cycle-open session has no board for the name at all, so there is
  // no surface to strike or size a swap against either. The cycle is fixed all
  // the same (its expiry comes off the calendar, not the surface) and runs
  // one-legged to its end: the swap is a per-cycle instrument, so a later
  // session getting its board back does NOT retro-open it.
  const fs::path dark_dir = fresh_dir("swapdark");
  const Corpus dark_c = make_corpus(dark_dir, /*dark_at=*/0u);
  auto dark_clock = Clock::from_manifest(dark_c.manifest);
  ASSERT_TRUE(dark_clock.has_value()) << dark_clock.error().to_string();

  StrangleVsVarswapStrategy dark_strat{make_config(dark_c, 0.25)};
  auto dark = run_prefix(*dark_clock, dark_c, dark_strat, /*last_index=*/3u, rcfg);
  ASSERT_TRUE(dark.has_value()) << dark.error().to_string();
  EXPECT_EQ(dark_strat.unresolved_strike_steps(), 1u); // the dark inception step
  EXPECT_EQ(dark_strat.skipped_swap_cycles(), 1u);     // and the cycle it left one-legged
  EXPECT_TRUE(dark->checkpoint.portfolio.swap_lots.empty());
  // The option leg came back on ref 1 and has been restriking since, so the
  // fixture is not vacuously green on an empty book.
  EXPECT_EQ(dark->checkpoint.portfolio.lots.size(), 2u);
}

// ── 10. A cycle too short to accrue a return carries no swap ────────────────
//
// The GRID-END case, and it is reachable on any real corpus: once the tenor
// anchor outruns the calendar, `select_cycle_expiry` falls back to the LAST
// session, so a cycle opening on the penultimate one expires on the very next
// session. Its fixing window holds exactly one session — which the engine spends
// SEEDING the series — so it would observe no return at all. That lot is not
// merely uninformative, it is unbookable twice over: `n_obs_total` would be 0,
// which `validate_swap_lot_economics` rejects at the boundary, and the swap pass
// hard-fails a lot that reaches expiry with an empty estimator, taking the whole
// run down with it. The tail cycle therefore runs options-only, and says so.
TEST(StrangleVarswap, SkipsSwapWhenCycleIsTooShortToAccrue) {
  const fs::path dir = fresh_dir("swapshort");
  const Corpus c = make_corpus(dir);
  auto clock = Clock::from_manifest(c.manifest);
  ASSERT_TRUE(clock.has_value()) << clock.error().to_string();
  const RunConfig rcfg;

  // 140 calendar days: the anchor from ref 0 lands strictly between session 4
  // (day 120) and session 5 (day 150), so cycle 1 expires on ref 5. From ref 5
  // the anchor is day 290 — past the last session (day 180) — so cycle 2 takes
  // the LAST session, one step away. Deliberately NOT a tenor that lands exactly
  // on a session: the ns rounding in `validate_config` could then flip the
  // lower_bound to the next session and the fixture would test nothing.
  const StrangleVarswapConfig scfg = make_config(c, 140.0 / 365.25);
  const std::int64_t cycle1_expiry = kBaseNow + 5LL * kStepNs;
  const std::int64_t cycle2_expiry = kBaseNow + 6LL * kStepNs;

  // The LONG cycle first: it does carry a swap, so the skip below is specific to
  // the short cycle rather than a blanket failure of the fixture.
  StrangleVsVarswapStrategy long_strat{scfg};
  auto held = run_prefix(*clock, c, long_strat, /*last_index=*/4u, rcfg);
  ASSERT_TRUE(held.has_value()) << held.error().to_string();
  EXPECT_EQ(long_strat.skipped_swap_cycles(), 0u);
  ASSERT_EQ(held->checkpoint.portfolio.swap_lots.size(), 1u);
  EXPECT_EQ(held->checkpoint.portfolio.swap_lots.front().expiry_ts_ns, cycle1_expiry);
  // Sessions in (ref0, ref5] are refs 1..5; the first only seeds the series.
  EXPECT_EQ(held->checkpoint.portfolio.swap_lots.front().n_obs_total, 4u);

  // Ref 5 — cycle 1's swap settles on a FULLY observed series and cycle 2 opens
  // on that same step with only one session left to expiry. No swap is appended.
  StrangleVsVarswapStrategy tail_strat{scfg};
  auto tail = run_prefix(*clock, c, tail_strat, /*last_index=*/5u, rcfg);
  ASSERT_TRUE(tail.has_value()) << tail.error().to_string();
  EXPECT_EQ(tail_strat.skipped_swap_cycles(), 1u);     // counted, never silent
  EXPECT_EQ(tail_strat.unresolved_strike_steps(), 0u); // the surface was fine
  EXPECT_TRUE(tail->checkpoint.portfolio.swap_lots.empty());
  // The OPTION leg is live on the tail cycle: the strangle rolled into it
  // normally, which is exactly what "runs options-only" has to mean.
  ASSERT_EQ(tail->checkpoint.portfolio.lots.size(), 2u);
  for (const Lot &lot : tail->checkpoint.portfolio.lots) {
    EXPECT_EQ(lot.expiry_ts_ns, cycle2_expiry);
  }
  // Cycle 1's swap really did settle here rather than being dropped: its live
  // mark is gone from the book while its settlement moved that row's swap_pnl.
  ASSERT_EQ(tail->rows.size(), 6u);
  EXPECT_TRUE(bits_equal(tail->rows.swap_pv.back(), 0.0));
  EXPECT_NE(tail->rows.swap_pnl.back(), 0.0);

  // And the run reaches the end of the calendar: the tail cycle settles its
  // options at ref 6, nothing is left open, and no swap ever needed rescuing.
  StrangleVsVarswapStrategy full_strat{scfg};
  auto full = run_backtest_incremental(*clock, full_strat, rcfg, nullptr);
  ASSERT_TRUE(full.has_value()) << full.error().to_string();
  ASSERT_EQ(full->rows.size(), kSessions);
  EXPECT_EQ(full_strat.skipped_swap_cycles(), 1u);
  EXPECT_TRUE(full->checkpoint.portfolio.swap_lots.empty());
  EXPECT_TRUE(full->checkpoint.portfolio.lots.empty());
  EXPECT_EQ(full_strat.cycle_expiry_ts_ns(), 0);
  // The final session carries no swap lane at all — the skipped cycle never
  // booked one, so both columns are untouched zeros on that row.
  EXPECT_TRUE(bits_equal(full->rows.swap_pv.back(), 0.0));
  EXPECT_TRUE(bits_equal(full->rows.swap_pnl.back(), 0.0));
}

// ── 11. Per-step swap greeks are the engine's own live contract ─────────────
//
// Contracts 1 and 4 of the Task 3 brief. The swap greeks are RECOMPUTED every
// step against that step's snapshot and that step's ACCRUAL — not carried from
// the cycle's entry — so the oracle here rebuilds the engine's accrual state by
// hand off the fixture's spot path (the lot is first SEEN by the swap pass one
// step after it is opened, and that step only seeds the series) and prices the
// resulting contract through the same bridge the engine's mark lane uses. A
// signal that staged the accrual one step early, or reused the entry contract,
// or forgot the qty scaling, disagrees on at least one of these four rows.
TEST(StrangleVarswap, SignalsMatchIndependentSwapGreeksOracle) {
  const fs::path dir = fresh_dir("signals");
  const Corpus c = make_corpus(dir);
  auto clock = Clock::from_manifest(c.manifest);
  ASSERT_TRUE(clock.has_value()) << clock.error().to_string();
  const StrangleVarswapConfig scfg = make_config(c, 0.25);
  const RunConfig rcfg;

  StrangleVsVarswapStrategy strat{scfg};
  auto out = run_backtest_incremental(*clock, strat, rcfg, nullptr);
  ASSERT_TRUE(out.has_value()) << out.error().to_string();
  const BacktestResult &r = out->rows;
  ASSERT_EQ(r.size(), kSessions);

  // Contract 4: every column Task 4's renderer reads exists under its exact name
  // and is row-parallel to `date`.
  static constexpr const char *kSignalNames[] = {
      "swap_delta", "swap_gamma",    "swap_vega",         "swap_theta",
      "swap_rho",   "strangle_vega", "skipped_restrikes", "skipped_swaps"};
  for (const char *name : kSignalNames) {
    const std::vector<double> *s = signal_series(r, name);
    ASSERT_NE(s, nullptr) << name;
    EXPECT_EQ(s->size(), r.date.size()) << name;
    EXPECT_EQ(s->size(), r.ts_ns.size()) << name;
  }
  const std::vector<double> &sig_delta = *signal_series(r, "swap_delta");
  const std::vector<double> &sig_gamma = *signal_series(r, "swap_gamma");
  const std::vector<double> &sig_vega = *signal_series(r, "swap_vega");
  const std::vector<double> &sig_theta = *signal_series(r, "swap_theta");
  const std::vector<double> &sig_rho = *signal_series(r, "swap_rho");
  const std::vector<double> &sig_strangle = *signal_series(r, "strangle_vega");

  // Cycle 1's swap lot, read off the ENGINE's own book on its open date.
  StrangleVsVarswapStrategy open_strat{scfg};
  auto opened = run_prefix(*clock, c, open_strat, /*last_index=*/0u, rcfg);
  ASSERT_TRUE(opened.has_value()) << opened.error().to_string();
  ASSERT_EQ(opened->checkpoint.portfolio.swap_lots.size(), 1u);
  const SwapLot swap1 = opened->checkpoint.portfolio.swap_lots.front();
  ASSERT_EQ(swap1.expiry_ts_ns, kBaseNow + 4LL * kStepNs);
  ASSERT_NE(swap1.qty, 0.0);

  // The accrual the engine WILL have taken by each row, hand-computed from the
  // fixture's spot path: row 0 opens the lot (the swap pass has never seen it),
  // row 1 seeds the series at that row's spot and accrues nothing, and every row
  // after that adds exactly one return.
  const double ra = std::log(kSpots[2] / kSpots[1]);
  const double rb = std::log(kSpots[3] / kSpots[2]);
  struct AccrualRow {
    std::size_t row;
    std::uint32_t n_obs_done;
    double sum_sq;
  };
  const AccrualRow kAccrued[] = {
      {0u, 0u, 0.0},
      {1u, 0u, 0.0},
      {2u, 1u, ra * ra},
      {3u, 2u, ra * ra + rb * rb},
  };
  for (const AccrualRow &a : kAccrued) {
    const std::int64_t ts = kBaseNow + static_cast<std::int64_t>(a.row) * kStepNs;
    const Result<DerivGreeks> g =
        swap_greeks_at(c.dp[a.row].second, swap1, ts, a.n_obs_done, a.sum_sq);
    ASSERT_TRUE(g.has_value()) << "row " << a.row << ": " << g.error().to_string();
    // Not vacuously green: this really is a live, non-degenerate position.
    ASSERT_TRUE(std::isfinite(g->vega)) << "row " << a.row;
    ASSERT_NE(g->vega, 0.0) << "row " << a.row;
    expect_close(sig_delta[a.row], swap1.qty * g->delta, 1.0e-12, "swap_delta", a.row);
    expect_close(sig_gamma[a.row], swap1.qty * g->gamma, 1.0e-12, "swap_gamma", a.row);
    expect_close(sig_vega[a.row], swap1.qty * g->vega, 1.0e-12, "swap_vega", a.row);
    expect_close(sig_theta[a.row], swap1.qty * g->theta, 1.0e-12, "swap_theta", a.row);
    expect_close(sig_rho[a.row], swap1.qty * g->rho, 1.0e-12, "swap_rho", a.row);
  }

  // Row 4 settles cycle 1's swap and opens cycle 2's on the SAME step, so the
  // signals there must describe the new lot at its own entry state — not the
  // settled lot's last accrual, and not a stale carry.
  StrangleVsVarswapStrategy roll_strat{scfg};
  auto rolled = run_prefix(*clock, c, roll_strat, /*last_index=*/4u, rcfg);
  ASSERT_TRUE(rolled.has_value()) << rolled.error().to_string();
  ASSERT_EQ(rolled->checkpoint.portfolio.swap_lots.size(), 1u);
  const SwapLot swap2 = rolled->checkpoint.portfolio.swap_lots.front();
  ASSERT_NE(swap2.id, swap1.id);
  const Result<DerivGreeks> g4 = entry_swap_greeks(c.dp[4].second, swap2, kBaseNow + 4LL * kStepNs);
  ASSERT_TRUE(g4.has_value()) << g4.error().to_string();
  expect_close(sig_vega[4], swap2.qty * g4->vega, 1.0e-12, "swap_vega", 4u);
  expect_close(sig_theta[4], swap2.qty * g4->theta, 1.0e-12, "swap_theta", 4u);

  // EQUAL VEGA, now visible in the columns: on a cycle-open row the two legs
  // carry the same first-order exposure by construction, which is what makes
  // every later row of the comparison second-order.
  expect_close(sig_vega[0], sig_strangle[0], 1.0e-12, "swap_vega vs strangle_vega", 0u);
  expect_close(sig_vega[4], sig_strangle[4], 1.0e-12, "swap_vega vs strangle_vega", 4u);

  // Both counters are emitted on every row and clean on this fixture.
  const std::vector<double> &sig_skipped_restrikes = *signal_series(r, "skipped_restrikes");
  const std::vector<double> &sig_skipped_swaps = *signal_series(r, "skipped_swaps");
  for (std::size_t i = 0; i < r.size(); ++i) {
    EXPECT_TRUE(bits_equal(sig_skipped_restrikes[i], 0.0)) << "row " << i;
    EXPECT_TRUE(bits_equal(sig_skipped_swaps[i], 0.0)) << "row " << i;
  }

  // Contract 4 at a STRIDE. Downsampling records every other session, so the
  // series stay parallel to a SHORTER `date` — and the value on a recorded row
  // still carries the fixings taken on the sessions that were not recorded. A
  // signal that advanced its accrual per recorded row instead of per step would
  // read the row below one return light.
  RunConfig strided;
  strided.record_every_n = 2u;
  StrangleVsVarswapStrategy stride_strat{scfg};
  const Result<BacktestResult> strided_rows = run_backtest(*clock, stride_strat, strided);
  ASSERT_TRUE(strided_rows.has_value()) << strided_rows.error().to_string();
  ASSERT_EQ(strided_rows->size(), 4u); // refs 0, 2, 4 and the always-recorded last
  for (const char *name : kSignalNames) {
    const std::vector<double> *s = signal_series(*strided_rows, name);
    ASSERT_NE(s, nullptr) << name;
    EXPECT_EQ(s->size(), strided_rows->date.size()) << name;
  }
  const std::vector<double> *strided_vega = signal_series(*strided_rows, "swap_vega");
  ASSERT_NE(strided_vega, nullptr);
  const Result<DerivGreeks> g2 =
      swap_greeks_at(c.dp[2].second, swap1, kBaseNow + 2LL * kStepNs, /*n_obs_done=*/1u, ra * ra);
  ASSERT_TRUE(g2.has_value()) << g2.error().to_string();
  expect_close((*strided_vega)[1], swap1.qty * g2->vega, 1.0e-12, "strided swap_vega", 1u);
}

// ── 12. No live swap ⇒ NaN, never 0.0 ───────────────────────────────────────
//
// Contract 3. Two arms, because a zero in a swap greek column is indistinguish-
// able from a real flat position and both arms occur on ordinary runs: the leg
// switched off entirely, and the ONE-LEGGED TAIL CYCLE that any corpus whose
// calendar runs out before the tenor ends on.
TEST(StrangleVarswap, SignalsNaNWhenNoLiveSwap) {
  const RunConfig rcfg;

  // Arm A — the swap leg is disabled, so no row ever carries a swap.
  const fs::path off_dir = fresh_dir("signalsoff");
  const Corpus off_c = make_corpus(off_dir);
  auto off_clock = Clock::from_manifest(off_c.manifest);
  ASSERT_TRUE(off_clock.has_value()) << off_clock.error().to_string();
  StrangleVarswapConfig off_cfg = make_config(off_c, 0.25);
  off_cfg.enable_swap_leg = false;

  StrangleVsVarswapStrategy off_strat{off_cfg};
  auto off = run_backtest_incremental(*off_clock, off_strat, rcfg, nullptr);
  ASSERT_TRUE(off.has_value()) << off.error().to_string();
  ASSERT_EQ(off->rows.size(), kSessions);
  const std::vector<double> *off_strangle = signal_series(off->rows, "strangle_vega");
  ASSERT_NE(off_strangle, nullptr);
  for (std::size_t i = 0; i < off->rows.size(); ++i) {
    expect_swap_signals_nan(off->rows, i);
  }
  // ... while the OPTIONS leg is still measured on every row it is live, so the
  // NaNs above are the swap lane's own state and not a blanket "signals off".
  for (std::size_t i = 0; i + 1u < off->rows.size(); ++i) {
    EXPECT_TRUE(std::isfinite((*off_strangle)[i])) << "row " << i;
    EXPECT_GT((*off_strangle)[i], 0.0) << "row " << i;
  }

  // Arm B — the one-legged tail. 140 calendar days: cycle 1 runs ref0 -> ref5 and
  // carries a swap; cycle 2 opens on ref5 with a single session left, which is
  // too short to accrue a return, so it runs options-only to the end. Rows 5 and
  // 6 are therefore NORMAL data with no swap at all, not an error.
  const fs::path dir = fresh_dir("signalstail");
  const Corpus c = make_corpus(dir);
  auto clock = Clock::from_manifest(c.manifest);
  ASSERT_TRUE(clock.has_value()) << clock.error().to_string();
  StrangleVsVarswapStrategy strat{make_config(c, 140.0 / 365.25)};
  auto out = run_backtest_incremental(*clock, strat, rcfg, nullptr);
  ASSERT_TRUE(out.has_value()) << out.error().to_string();
  const BacktestResult &r = out->rows;
  ASSERT_EQ(r.size(), kSessions);
  ASSERT_EQ(strat.skipped_swap_cycles(), 1u);

  const std::vector<double> *vega = signal_series(r, "swap_vega");
  const std::vector<double> *strangle = signal_series(r, "strangle_vega");
  const std::vector<double> *skipped_swaps = signal_series(r, "skipped_swaps");
  const std::vector<double> *skipped_restrikes = signal_series(r, "skipped_restrikes");
  ASSERT_NE(vega, nullptr);
  ASSERT_NE(strangle, nullptr);
  ASSERT_NE(skipped_swaps, nullptr);
  ASSERT_NE(skipped_restrikes, nullptr);

  for (std::size_t i = 0; i <= 4u; ++i) {
    EXPECT_TRUE(std::isfinite((*vega)[i])) << "row " << i;
    EXPECT_NE((*vega)[i], 0.0) << "row " << i;
  }
  expect_swap_signals_nan(r, 5u);
  expect_swap_signals_nan(r, 6u);
  // The strangle is live on every row but the last, where the calendar runs out
  // and the book goes flat — the same NaN convention, one leg over.
  for (std::size_t i = 0; i <= 5u; ++i) {
    EXPECT_TRUE(std::isfinite((*strangle)[i])) << "row " << i;
  }
  EXPECT_TRUE(std::isnan((*strangle)[6u]));
  // The skip is REPORTED on the rows it affects rather than only at the end of
  // the run: the counter steps exactly where the tail cycle opened one-legged.
  for (std::size_t i = 0; i <= 4u; ++i) {
    EXPECT_TRUE(bits_equal((*skipped_swaps)[i], 0.0)) << "row " << i;
  }
  EXPECT_EQ((*skipped_swaps)[5u], 1.0);
  EXPECT_EQ((*skipped_swaps)[6u], 1.0);
  for (std::size_t i = 0; i < r.size(); ++i) {
    EXPECT_TRUE(bits_equal((*skipped_restrikes)[i], 0.0)) << "row " << i;
  }
}

// ── 13. `skipped_restrikes` counts KEPT strikes, and nothing else ───────────
//
// The column has to mean one thing. A step that could not resolve the target
// delta while a pair was LIVE really did skip a restrike — the old strikes were
// kept. A step that could not resolve one while the book was EMPTY (inception,
// or the step a settled cycle rolls on) skipped nothing: there were no strikes
// to keep and the strangle simply does not exist that session. Reporting the
// second as a skipped restrike would make the column a lie on exactly the runs
// it is there to diagnose, so the two are counted apart.
TEST(StrangleVarswap, SkippedRestrikesCountsOnlyKeptStrikes) {
  const RunConfig rcfg;

  // Arm A — the board is dark on INCEPTION: nothing was ever struck, so no
  // restrike was skipped, even though the step is unresolved.
  const fs::path dark_dir = fresh_dir("signalsdark0");
  const Corpus dark_c = make_corpus(dark_dir, /*dark_at=*/0u);
  auto dark_clock = Clock::from_manifest(dark_c.manifest);
  ASSERT_TRUE(dark_clock.has_value()) << dark_clock.error().to_string();
  StrangleVsVarswapStrategy dark_strat{make_config(dark_c, 0.25)};
  auto dark = run_prefix(*dark_clock, dark_c, dark_strat, /*last_index=*/3u, rcfg);
  ASSERT_TRUE(dark.has_value()) << dark.error().to_string();
  ASSERT_EQ(dark->rows.size(), 4u);
  EXPECT_EQ(dark_strat.unresolved_strike_steps(), 1u); // the step IS unresolved ...
  EXPECT_EQ(dark_strat.skipped_restrikes(), 0u);       // ... but nothing was kept
  EXPECT_EQ(dark_strat.unopened_strangle_steps(), 1u);
  const std::vector<double> *dark_skips = signal_series(dark->rows, "skipped_restrikes");
  ASSERT_NE(dark_skips, nullptr);
  for (std::size_t i = 0; i < dark->rows.size(); ++i) {
    EXPECT_TRUE(bits_equal((*dark_skips)[i], 0.0)) << "row " << i;
  }
  // The inception row has no strangle to measure either.
  const std::vector<double> *dark_strangle = signal_series(dark->rows, "strangle_vega");
  ASSERT_NE(dark_strangle, nullptr);
  EXPECT_TRUE(std::isnan((*dark_strangle)[0]));
  EXPECT_TRUE(std::isfinite((*dark_strangle)[1]));

  // Arm B — the board goes dark MID-CYCLE with a live pair: the strikes really
  // were kept, and the column steps on exactly that row.
  const fs::path held_dir = fresh_dir("signalsdark2");
  const Corpus held_c = make_corpus(held_dir, /*dark_at=*/2u);
  auto held_clock = Clock::from_manifest(held_c.manifest);
  ASSERT_TRUE(held_clock.has_value()) << held_clock.error().to_string();
  StrangleVarswapConfig held_cfg = make_config(held_c, 0.25);
  held_cfg.enable_swap_leg = false; // the swap lane fails closed on a dark session
  RunConfig held_rcfg;
  held_rcfg.unpriced = UnpricedLotPolicy::ExcludeAndReport;

  StrangleVsVarswapStrategy held_strat{held_cfg};
  auto held = run_prefix(*held_clock, held_c, held_strat, /*last_index=*/3u, held_rcfg);
  ASSERT_TRUE(held.has_value()) << held.error().to_string();
  ASSERT_EQ(held->rows.size(), 4u);
  EXPECT_EQ(held_strat.unresolved_strike_steps(), 1u);
  EXPECT_EQ(held_strat.skipped_restrikes(), 1u);
  EXPECT_EQ(held_strat.unopened_strangle_steps(), 0u);
  const std::vector<double> *held_skips = signal_series(held->rows, "skipped_restrikes");
  ASSERT_NE(held_skips, nullptr);
  EXPECT_TRUE(bits_equal((*held_skips)[0], 0.0));
  EXPECT_TRUE(bits_equal((*held_skips)[1], 0.0));
  EXPECT_EQ((*held_skips)[2], 1.0);
  EXPECT_EQ((*held_skips)[3], 1.0); // cumulative: the hole stays on the record
  // The pair is held, but it cannot be VALUED on the dark session, so its vega
  // is not reported as anything.
  const std::vector<double> *held_strangle = signal_series(held->rows, "strangle_vega");
  ASSERT_NE(held_strangle, nullptr);
  EXPECT_TRUE(std::isfinite((*held_strangle)[1]));
  EXPECT_TRUE(std::isnan((*held_strangle)[2]));
  EXPECT_TRUE(std::isfinite((*held_strangle)[3]));
}
