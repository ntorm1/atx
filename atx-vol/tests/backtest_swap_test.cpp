// atx-vol backtest SWAP LANE gates (Task 10).
//
// The engine's vol-derivative lane is ADDITIVE: a `PortfolioState::swap_lots`
// entry accrues its own realized-variance fixings off the daily snapshot spot,
// marks through `deriv_price` against the shifted surface, and cash-settles from
// the accrual at an exactly-observed expiry — all without touching a single
// number the option lane produces.
//
// Gates:
//   1. VarSwapAccruesAndSettlesExactly       — hand-computed accrual + payoff.
//   1b. DailySwapMarksMatchIndependentDerivPriceOracle — each LIVE daily mark
//                                              against a hand-built contract.
//   2. OptionOnlyBookHasZeroSwapColumns      — zero-swap books: columns exactly
//                                              0.0 and NAV == Sigma pnl_total.
//   3. CheckpointResumeReproducesSwapMarks   — split run == one-shot run, bitwise.
//   4. DuplicateTimestampRefusedNotDoubleCounted — a replayed snapshot ts errors.
//   5. MissingSurfaceForSwapLotErrors        — a live swap lot fails closed.
//
// Fixture plumbing (synthetic eSSVI surfaces written as one-symbol archives per
// date) mirrors backtest_exec_test.cpp; the spot path is EXPLICIT here so the
// realized-variance arithmetic can be hand-computed exactly as
// `RealizedTracker.ObserveBatch_HandComputedThreeReturns` does.

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "atx/core/error.hpp"                  // atx::core::Ok, ErrorCode
#include "atx/vol/american.hpp"                // al_fast_opts, AmericanMethod
#include "atx/vol/backtest.hpp"                // Clock, run_backtest, SwapLot, RunConfig
#include "atx/vol/backtest_db.hpp"             // append_backtest_results
#include "atx/vol/corpus.hpp"                  // CorpusManifest, CorpusEntry, CorpusFitStatus
#include "atx/vol/derivatives.hpp"             // DerivKind, DerivContract, DerivConfig, DerivQuote
#include "atx/vol/detail/deriv_ref_bridge.hpp" // detail::deriv_price_on_ref
#include "atx/vol/portfolio_pricer.hpp"        // kNsPerYear, SurfaceRef
#include "atx/vol/priced_surface.hpp"          // PricedSurface, PricingContext
#include "atx/vol/strategy.hpp"                // IStrategy, DeclarativeStrategy, StrategySpec
#include "atx/vol/surface_archive.hpp"         // write_surface_archive_v2_file
#include "atx/vol/surface_parity.hpp"          // SliceContext
#include "atx/vol/types.hpp"                   // Side, Result, Status
#include "atx/vol/vol_curve.hpp"               // CurveSurface, EssviCurve
#include "atx/vol/vol_surface.hpp"             // EssviParams
#include "atx/vol/vol_time.hpp"                // VolTimeCalendar -- elapsed_weekdays oracle (Task A1)

using namespace atx::vol;
using atx::core::ErrorCode;
namespace fs = std::filesystem;

namespace {

constexpr double kR = 0.043;
constexpr std::int64_t kBaseNow = 1700000000000000000LL;
constexpr std::int64_t kDayNs = 86400LL * 1000000000LL;
// 30 calendar days between snapshots: the swap's residual T stays inside the
// synthetic surface's fitted pillar range [0.05, 1.0] on every marked step, so
// the strip prices real carry rather than a flat extrapolation.
constexpr std::int64_t kStepNs = 30LL * kDayNs;
constexpr std::uint32_t kUid = 7;
constexpr std::uint32_t kMissingUid = 4242; // never written into any archive

[[nodiscard]] bool bits_equal(double a, double b) noexcept {
  std::uint64_t ba = 0;
  std::uint64_t bb = 0;
  std::memcpy(&ba, &a, sizeof ba);
  std::memcpy(&bb, &b, sizeof bb);
  return ba == bb;
}

// Task A1 migration note: this file's fixture spaces snapshots `kStepNs` (30
// CALENDAR days) apart purely so residual T stays inside the synthetic
// surface's fitted pillar range (see `kStepNs` above) -- it was never meant to
// model a genuine daily fixing schedule. Under the new default
// `SwapFixingCadence::RequireEverySession`, every gate below that accrues more
// than one fixing now correctly reports a schedule violation (this IS Task
// A1's fix working: a 30-calendar-day clock step is ~20-22 NYSE weekday
// sessions, exactly the "clock coarser than the fixing schedule" defect this
// task closes). Each such gate opts into `SwapFixingCadence::
// AcceptClockAsSchedule` and recomputes its hand-derived expectations with
// `elapsed_weekdays` below in place of the old "+1 fixing per step" count.

// Independent day-count oracle (Task A1, post-review fixup): sessions elapsed
// in `(prev_ns, ts_ns]`. Mirrors what the engine's OWN `weekday_sessions_
// between` (backtest.cpp, file-local) computes -- including its HYBRID rule
// (real NYSE sessions, via `VolTimeCalendar::us_default()`'s listed closures,
// whenever BOTH endpoints fall inside its 2024-2028 covered window; plain
// Mon-Fri weekdays otherwise) -- but is written FRESH here rather than
// calling into engine internals, the same independent-oracle discipline
// `reference_swap_mark` below documents (built from the documented
// convention, never from the code under test; `VolTimeCalendar` itself is
// the shared, independently-tested data source, not the code under test, so
// consulting its public API here does not defeat that independence -- exactly
// how `reference_swap_mark` calls the real `detail::deriv_price_on_ref`
// rather than re-deriving option pricing from scratch). 1970-01-01 (epoch
// day 0) is a Thursday, so a day's Sun(0)..Sat(6) weekday index is
// `(day + 4) mod 7`.
//
// This matters for THIS file's fixture even though it is nominally
// "2023-dated" (`kBaseNow` = 2023-11-14): steps far enough into a test's
// corpus land in 2024 (e.g. `VarSwapAccruesAndSettlesExactly`'s accrual #2,
// 2024-01-13 -> 2024-02-12, both endpoints in-window) and DO cross a real
// closure (2024 MLK Day, 2024-01-15) -- this oracle must agree with the
// engine there too, not just in the purely-2023 cases.
[[nodiscard]] std::uint32_t elapsed_weekdays(std::int64_t prev_ns, std::int64_t ts_ns) {
  const std::int64_t prev_day = prev_ns / kDayNs;
  const std::int64_t ts_day = ts_ns / kDayNs;
  const VolTimeCalendar &cal = VolTimeCalendar::us_default();
  const bool holiday_aware = cal.covers(static_cast<std::int32_t>(prev_day)) &&
                             cal.covers(static_cast<std::int32_t>(ts_day));
  std::uint32_t sessions = 0;
  for (std::int64_t d = prev_day + 1; d <= ts_day; ++d) {
    const std::int64_t dow = ((d % 7) + 7 + 4) % 7; // 0=Sun .. 6=Sat
    if (dow == 0 || dow == 6) {
      continue;
    }
    if (holiday_aware && cal.is_holiday(static_cast<std::int32_t>(d))) {
      continue;
    }
    ++sessions;
  }
  return sessions;
}

// A fixing-series cap large enough that AcceptClockAsSchedule's
// elapsed-session scaling of `n_obs_done` never prematurely closes a
// migrated gate's fixing series (every migrated gate's genuine accrual count,
// summed via `elapsed_weekdays`, stays far below this over its handful of
// steps) -- so a gate that intends N accrued returns still gets N, exactly as
// the pre-migration test did, just denominated in elapsed sessions instead of
// step count.
constexpr std::uint32_t kHugeObsCap = 1'000'000u;

// A synthetic eSSVI PricedSurface (flat forward, genuine American premium via
// q_eff=0.02), slices T in [0.05, 1.0]. Mirrors backtest_exec_test's make_surface.
[[nodiscard]] PricedSurface make_surface(std::uint32_t uid, double S, double fwd,
                                         std::int64_t now_ts) {
  CurveSurface cs;
  std::vector<SliceContext> ctx;
  const double Ts[] = {0.05, 0.10, 0.20, 0.35, 0.50, 0.75, 1.00};
  int i = 0;
  for (const double T : Ts) {
    const double term_forward = fwd * std::exp((kR - 0.02) * T);
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
  const fs::path dir = fs::temp_directory_path() / (std::string("atx-btswap-") + tag);
  std::error_code ec;
  fs::remove_all(dir, ec);
  return dir;
}

// Write one surface as this date's archive; return its path (creating `dir`).
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

// Hand-build an Ok-only manifest over (date, archive_path) rows (one entry/date).
[[nodiscard]] CorpusManifest
make_manifest(const std::vector<std::pair<std::string, std::string>> &date_paths,
              const std::string &symbol) {
  CorpusManifest m;
  for (const auto &[date, path] : date_paths) {
    m.dates.push_back(date);
    CorpusEntry e;
    e.date = date;
    e.symbol = symbol;
    e.status = CorpusFitStatus::Ok;
    e.archive_path = path;
    m.entries.push_back(std::move(e));
  }
  return m;
}

struct Corpus {
  CorpusManifest manifest;
  std::vector<std::pair<std::string, std::string>> dp; // (date, path), ascending
};

// One archive per entry of `spots`, snapshots kStepNs apart. The spot path is
// the caller's so the realized-variance accrual is hand-computable.
//
// `dark_from` is the first date index whose archive is written under a DIFFERENT
// uid/symbol, i.e. the date the swap's name goes dark. Defaults past the end.
[[nodiscard]] Corpus make_spot_corpus(const fs::path &dir, const std::string &symbol,
                                      const std::vector<double> &spots,
                                      std::size_t dark_from = static_cast<std::size_t>(-1)) {
  std::vector<std::pair<std::string, std::string>> dp;
  for (std::size_t d = 0; d < spots.size(); ++d) {
    const std::int64_t now = kBaseNow + static_cast<std::int64_t>(d) * kStepNs;
    const bool dark = d >= dark_from;
    const PricedSurface s = make_surface(dark ? kMissingUid : kUid, spots[d], spots[d], now);
    char buf[16];
    std::snprintf(buf, sizeof buf, "2026-08-%02d", static_cast<int>(d) + 1);
    const std::string date = buf;
    dp.emplace_back(date, write_one(dir, date, dark ? "OTHER" : symbol, s));
  }
  Corpus c;
  c.dp = std::move(dp);
  c.manifest = make_manifest(c.dp, symbol);
  return c;
}

// ── Task A1: swap fixing-cadence guard fixture ──────────────────────────────
//
// A clock whose steps are NOT uniformly one NYSE weekday session apart: three
// snapshots genuinely one weekday apart (a real daily cadence), then one
// deliberate 42-CALENDAR-day jump. 42 days is exactly 6 full weeks, and any 7
// consecutive calendar days contain exactly 5 weekdays regardless of
// alignment, so that jump is EXACTLY 30 elapsed weekday sessions --
// independently verifiable by hand, without re-deriving the guard's own
// day-counting arithmetic (`weekday_sessions_between`, backtest.cpp).
//
// Deliberately dated in 2023, OUTSIDE `VolTimeCalendar::us_default()`'s
// 2024-2028 covered window (see the "HYBRID RULE" comment on
// `weekday_sessions_between`, backtest.cpp, added in the post-review
// fixup): this fixture exercises the CADENCE GUARD mechanism generically,
// via plain Mon-Fri weekday counting, independent of the holiday-aware path
// -- which gets its own dedicated covering test below
// (`HolidayAwareCoarseClockAcceptedUnderRequireEverySession`). Dates inside
// the window would silently entangle this fixture's hand-computed 30-session
// gap with whichever 2023-era-analogous holidays happen to fall in it.
constexpr std::int64_t kGapD0 = 1696204800000000000LL;  // 2023-10-02 Monday    (inception)
constexpr std::int64_t kGapD1 = 1696291200000000000LL;  // 2023-10-03 Tuesday   (+1 weekday, seed)
constexpr std::int64_t kGapD2 = 1696377600000000000LL;  // 2023-10-04 Wednesday (+1 weekday)
constexpr std::int64_t kGapD3 = kGapD2 + 42LL * kDayNs; // 2023-11-15 Wednesday (+30 weekdays)
constexpr std::int64_t kGapFarExpiry = kGapD0 + 200LL * kDayNs; // far beyond the corpus

struct DatedSpot {
  std::string date;
  std::int64_t ts_ns;
  double spot;
};

// Like `make_spot_corpus`, but with EXPLICIT (date, ts_ns, spot) points
// instead of a uniform `kStepNs` stride -- for tests that need CONTROL over
// irregular inter-snapshot gaps (the fixing-cadence guard above).
[[nodiscard]] Corpus make_spot_corpus_at(const fs::path &dir, const std::string &symbol,
                                         const std::vector<DatedSpot> &points) {
  std::vector<std::pair<std::string, std::string>> dp;
  for (const DatedSpot &p : points) {
    const PricedSurface s = make_surface(kUid, p.spot, p.spot, p.ts_ns);
    dp.emplace_back(p.date, write_one(dir, p.date, symbol, s));
  }
  Corpus c;
  c.dp = std::move(dp);
  c.manifest = make_manifest(c.dp, symbol);
  return c;
}

// Opens exactly ONE swap lot at inception and never trades options. The lot's
// id is drawn from the engine's monotonic watermark, exactly as an option lot's
// would be.
class SwapOnlyStrategy : public IStrategy {
public:
  explicit SwapOnlyStrategy(SwapLot proto) noexcept : proto_{proto} {}

  Status on_step(const MarketSnapshot &base, std::size_t step_index, PortfolioState &book,
                 std::uint64_t &next_lot_id) override {
    if (step_index == 0) {
      SwapLot lot = proto_;
      lot.id = next_lot_id++;
      lot.start_ts_ns = base.ts_ns();
      book.swap_lots.push_back(lot);
    }
    return atx::core::Ok();
  }

private:
  SwapLot proto_;
};

// Opens one swap lot at inception, then MISBEHAVES at `bad_step` in one of the
// ways the transition check must reject. Each mode is a separate gate.
class MisbehavingSwapStrategy : public IStrategy {
public:
  enum class Mode { EraseLot, MutateStrike, IdBelowWatermark };

  MisbehavingSwapStrategy(SwapLot proto, Mode mode, std::size_t bad_step) noexcept
      : proto_{proto}, mode_{mode}, bad_step_{bad_step} {}

  Status on_step(const MarketSnapshot &base, std::size_t step_index, PortfolioState &book,
                 std::uint64_t &next_lot_id) override {
    if (step_index == 0) {
      SwapLot lot = proto_;
      lot.id = next_lot_id++;
      lot.start_ts_ns = base.ts_ns();
      book.swap_lots.push_back(lot);
      return atx::core::Ok();
    }
    if (step_index != bad_step_) {
      return atx::core::Ok();
    }
    switch (mode_) {
    case Mode::EraseLot:
      book.swap_lots.clear(); // an "early close" the engine has no price for
      break;
    case Mode::MutateStrike:
      if (!book.swap_lots.empty()) {
        book.swap_lots.front().strike_dec += 0.01; // restrike in place
      }
      break;
    case Mode::IdBelowWatermark: {
      SwapLot lot = proto_;
      lot.id = 1u; // already issued at inception — a reused id
      lot.start_ts_ns = base.ts_ns();
      book.swap_lots.push_back(lot);
      break;
    }
    }
    return atx::core::Ok();
  }

private:
  SwapLot proto_;
  Mode mode_;
  std::size_t bad_step_;
};

// A single-clip option strategy: opens ONE structure at inception and holds it.
[[nodiscard]] StrategySpec single_clip(std::uint32_t uid, double target_T, Side side) {
  StrategySpec spec;
  spec.name = "single-clip";
  LegSpec leg;
  leg.uid = uid;
  leg.tenor.target_T = target_T;
  leg.structure.kind = StructureSpec::Kind::Single;
  leg.structure.single_side = side;
  leg.strike = StrikeSelector{StrikeSelector::Kind::AtmForward, 0.0};
  leg.size = SizeSpec{SizeSpec::Kind::FixedContracts, 1.0, +1.0};
  spec.legs.push_back(leg);
  spec.lifecycle.entry = LifecycleSpec::Entry::EveryNDays;
  spec.lifecycle.holding = LifecycleSpec::Holding::HoldToExpiry;
  spec.lifecycle.entry_every_n = 100000; // >> corpus => opens once, at inception
  return spec;
}

// The var-swap fixture shared by gates 1 and 3: spots [100, 101, 99, 102] with
// the lot opened at inception. The engine SEEDS the fixing series on the FIRST
// STEP's snapshot (the swap pass runs only inside the step loop), so the seed is
// spots[1] and the accrued returns are ln(spots[2]/spots[1]) and
// ln(spots[3]/spots[2]) — two returns, hence n_obs_total = 2.
constexpr double kAnnualization = 252.0;
constexpr double kStrikeDec = 0.04;
constexpr double kNotional = 1000.0;
constexpr double kQty = 2.0;

// A minimally-valid hand-built BacktestResult covering rows
// [first_index, first_index + n_rows): every mandatory series column sized to
// the row count, with strictly ascending dates AND timestamps derived from the
// absolute row index so two results concatenate in order (which is exactly what
// `append_backtest_results` validates). Optionally carries the swap lane filled
// with `swap_value`.
[[nodiscard]] BacktestResult make_result(std::size_t first_index, std::size_t n_rows, bool swap,
                                         double swap_value = 0.0) {
  BacktestResult r;
  const std::size_t n = n_rows;
  for (std::size_t i = 0; i < n; ++i) {
    const std::size_t idx = first_index + i;
    char buf[16];
    std::snprintf(buf, sizeof buf, "2026-08-%02d", static_cast<int>(idx) + 1);
    r.date.emplace_back(buf);
    r.ts_ns.push_back(kBaseNow + static_cast<std::int64_t>(idx) * kDayNs);
  }
  // The 25 mandatory series columns (kBacktestSeriesColumns order). Spelled with
  // NON-const member pointers so the fill needs no const_cast; a column added to
  // the frozen registry without being added here shows up immediately as
  // `append_backtest_results` rejecting the fixture on a row-count mismatch.
  std::vector<double> BacktestResult::*const mandatory[] = {&BacktestResult::pnl_total,
                                                            &BacktestResult::pnl_delta,
                                                            &BacktestResult::pnl_gamma,
                                                            &BacktestResult::pnl_vega,
                                                            &BacktestResult::pnl_vanna,
                                                            &BacktestResult::pnl_volga,
                                                            &BacktestResult::pnl_theta,
                                                            &BacktestResult::pnl_rho,
                                                            &BacktestResult::pnl_charm,
                                                            &BacktestResult::pnl_unexplained,
                                                            &BacktestResult::pnl_settlement,
                                                            &BacktestResult::pnl_shares,
                                                            &BacktestResult::financing,
                                                            &BacktestResult::cost,
                                                            &BacktestResult::nav,
                                                            &BacktestResult::cash,
                                                            &BacktestResult::gross_delta,
                                                            &BacktestResult::gross_gamma,
                                                            &BacktestResult::gross_vega,
                                                            &BacktestResult::gross_theta,
                                                            &BacktestResult::turnover_notional,
                                                            &BacktestResult::turnover_vega,
                                                            &BacktestResult::n_open_lots,
                                                            &BacktestResult::n_unpriced_lots,
                                                            &BacktestResult::n_unpriced_greeks};
  for (std::vector<double> BacktestResult::*const member : mandatory) {
    (r.*member).assign(n, 0.0);
  }
  if (swap) {
    r.swap_pv.assign(n, swap_value);
    r.swap_pnl.assign(n, swap_value);
  }
  return r;
}

[[nodiscard]] SwapLot var_swap_proto(std::uint32_t uid, std::int64_t expiry_ts_ns,
                                     std::uint32_t n_obs_total) noexcept {
  SwapLot lot{};
  lot.uid = uid;
  lot.kind = DerivKind::VarSwap;
  lot.strike_dec = kStrikeDec;
  lot.cap_dec = 0.0;
  lot.notional = kNotional;
  lot.qty = kQty;
  lot.expiry_ts_ns = expiry_ts_ns;
  lot.n_obs_total = n_obs_total;
  lot.annualization = kAnnualization;
  return lot;
}

// The INDEPENDENT oracle for one live daily swap mark.
//
// Built from the engine's DOCUMENTED conventions, never from `step_swap_lots`:
//   * residual tenor = (expiry_ts_ns - snapshot_ts_ns) / kNsPerYear, in YEARS
//     (portfolio_pricer.hpp's Julian-year constant, the one every residual-T
//     site in the engine divides by);
//   * `rv_spec` = the accrual state AFTER this snapshot's own fixing, per
//     SwapAccrual's transcribed RealizedTracker arithmetic:
//     rv_done_dec = annualization * Sigma r^2 / n_done (0 before any return);
//   * every other field is the lot's own term, unscaled.
//
// It is priced through the SAME entry the mark lane prices through, and the
// mark is qty-scaled the same way, so the ONLY thing this can disagree with the
// engine about is how a `SwapLot` becomes a `DerivContract` — which is exactly
// the divergence a telescoping-sum gate is blind to.
[[nodiscard]] double reference_swap_mark(const PricedSurface &surface, std::int64_t ts_ns,
                                         const SwapLot &lot, std::uint32_t n_obs_done,
                                         double sum_sq_log_returns_done) {
  RealizedVarianceSpec rv{};
  rv.annualization = lot.annualization;
  rv.n_obs_total = lot.n_obs_total;
  rv.n_obs_done = n_obs_done;
  rv.sum_sq_log_returns_done = sum_sq_log_returns_done;
  rv.rv_done_dec = n_obs_done == 0u ? 0.0
                                    : lot.annualization * sum_sq_log_returns_done /
                                          static_cast<double>(n_obs_done);

  DerivContract contract;
  contract.kind = lot.kind;
  contract.maturity_t = static_cast<double>(lot.expiry_ts_ns - ts_ns) / kNsPerYear;
  contract.strike_dec = lot.strike_dec;
  contract.cap_dec = lot.cap_dec;
  contract.notional = lot.notional;
  contract.rv_spec = rv;

  const Result<DerivQuote> quote =
      detail::deriv_price_on_ref(SurfaceRef{&surface}, contract, DerivConfig{});
  EXPECT_TRUE(quote.has_value()) << (quote.has_value() ? std::string{} : quote.error().to_string());
  if (!quote.has_value()) {
    return std::numeric_limits<double>::quiet_NaN();
  }
  return lot.qty * quote->pv;
}

} // namespace

// ── 1. Accrual + settlement are exactly the hand computation ────────────────
TEST(BacktestSwap, VarSwapAccruesAndSettlesExactly) {
  const fs::path dir = fresh_dir("accrue");
  const std::vector<double> spots = {100.0, 101.0, 99.0, 102.0};
  const Corpus c = make_spot_corpus(dir, "SPX", spots);
  auto clock = Clock::from_manifest(c.manifest);
  ASSERT_TRUE(clock.has_value()) << clock.error().to_string();

  const std::int64_t expiry = kBaseNow + 3LL * kStepNs; // exactly the last snapshot
  // Task A1: the 30-calendar-day step is ~21 NYSE weekday sessions, coarser
  // than this lot's implicitly-daily schedule -- opt in and let n_obs_done
  // scale by the elapsed-session count instead of the old "+1 per step".
  // n_obs_total is bumped far above the genuine accrual count so BOTH
  // returns below still accrue (rather than the series closing after the
  // first, scaled-up fixing) -- this is what preserves the original "two
  // accrued returns, hand-computed" gate under the new cadence semantics.
  SwapOnlyStrategy strat{var_swap_proto(kUid, expiry, /*n_obs_total=*/kHugeObsCap)};

  RunConfig cfg;
  cfg.swap_fixing_cadence = SwapFixingCadence::AcceptClockAsSchedule;
  auto result = run_backtest(*clock, strat, cfg);
  ASSERT_TRUE(result.has_value()) << result.error().to_string();
  const BacktestResult &r = *result;
  ASSERT_EQ(r.size(), 4u);
  ASSERT_EQ(r.swap_pv.size(), r.size());
  ASSERT_EQ(r.swap_pnl.size(), r.size());

  // Hand computation over the two accrued returns. n_done is the SUM of
  // elapsed weekday sessions per accrual step, not the step count (2).
  const double ra = std::log(spots[2] / spots[1]);
  const double rb = std::log(spots[3] / spots[2]);
  const double sum_sq = ra * ra + rb * rb;
  const std::uint32_t n1 = elapsed_weekdays(kBaseNow + 1LL * kStepNs, kBaseNow + 2LL * kStepNs);
  const std::uint32_t n2 = elapsed_weekdays(kBaseNow + 2LL * kStepNs, kBaseNow + 3LL * kStepNs);
  const double n_done = static_cast<double>(n1) + static_cast<double>(n2);
  const double rv_dec = kAnnualization * sum_sq / n_done;
  const double payoff = kQty * kNotional * (rv_dec - kStrikeDec);

  // Inception books no swap economics at all (swaps open at zero cost).
  EXPECT_TRUE(bits_equal(r.swap_pv[0], 0.0));
  EXPECT_TRUE(bits_equal(r.swap_pnl[0], 0.0));
  // The first mark carries the whole entry PV (prev_pv starts at zero).
  EXPECT_TRUE(bits_equal(r.swap_pnl[1], r.swap_pv[1]));
  EXPECT_NE(r.swap_pv[1], 0.0);
  EXPECT_NE(r.swap_pv[2], 0.0);
  // The lot settled and was erased: no live mark remains.
  EXPECT_TRUE(bits_equal(r.swap_pv[3], 0.0));

  // Settlement pays qty*notional*(rv_done - K) into the cash ledger, and nothing
  // else in this book touches cash (no option lots, no frictions).
  EXPECT_NEAR(r.cash.back(), payoff, 1.0e-9);
  // swap_pnl telescopes over the marks to exactly the settlement payoff.
  double swap_pnl_sum = 0.0;
  for (const double v : r.swap_pnl) {
    swap_pnl_sum += v;
  }
  EXPECT_NEAR(swap_pnl_sum, payoff, 1.0e-9);
  EXPECT_NEAR(r.nav.back(), payoff, 1.0e-9);

  // The accrual itself, read straight off a checkpoint after the seed + one
  // return: this is the RealizedTracker arithmetic, hand-computed.
  auto sub = clock->between(c.dp[0].first, c.dp[2].first);
  ASSERT_TRUE(sub.has_value()) << sub.error().to_string();
  SwapOnlyStrategy strat_sub{var_swap_proto(kUid, expiry, /*n_obs_total=*/kHugeObsCap)};
  auto cont = run_backtest_incremental(*sub, strat_sub, cfg, nullptr);
  ASSERT_TRUE(cont.has_value()) << cont.error().to_string();
  ASSERT_EQ(cont->checkpoint.swap_accruals.size(), 1u);
  ASSERT_EQ(cont->checkpoint.portfolio.swap_lots.size(), 1u);
  const SwapAccrual &acc = cont->checkpoint.swap_accruals.front();
  EXPECT_EQ(acc.lot_id, cont->checkpoint.portfolio.swap_lots.front().id);
  EXPECT_TRUE(acc.have_prev);
  EXPECT_EQ(acc.prev_ts_ns, kBaseNow + 2LL * kStepNs);
  EXPECT_EQ(acc.prev_spot, spots[2]);
  EXPECT_EQ(acc.rv.n_obs_done, n1); // scaled by elapsed sessions, not +1
  EXPECT_EQ(acc.rv.n_obs_total, kHugeObsCap);
  EXPECT_LT(std::fabs(acc.rv.sum_sq_log_returns_done - ra * ra), 1.0e-15);
  EXPECT_LT(std::fabs(acc.rv.rv_done_dec - kAnnualization * ra * ra / static_cast<double>(n1)),
            1.0e-13);
}

// ── 1b. Every LIVE daily mark is the independently-priced value ─────────────
//
// Gate 1's telescoping identity (Sigma swap_pnl == settlement payoff) is BLIND
// to a wrong daily mark: consecutive marks cancel by construction, so a units
// slip in the residual tenor, a stale/mis-staged `rv_spec` snapshot, or a
// mis-wired `DerivContract` field would leave that sum — and the settlement —
// exactly right while distorting every intermediate NAV row. This pins the
// marks themselves against a contract rebuilt from first principles.
TEST(BacktestSwap, DailySwapMarksMatchIndependentDerivPriceOracle) {
  const fs::path dir = fresh_dir("markoracle");
  const std::vector<double> spots = {100.0, 101.0, 99.0, 102.0};
  const Corpus c = make_spot_corpus(dir, "SPX", spots);
  auto clock = Clock::from_manifest(c.manifest);
  ASSERT_TRUE(clock.has_value()) << clock.error().to_string();

  const std::int64_t expiry = kBaseNow + 3LL * kStepNs; // exactly the last snapshot
  // Task A1: opt in (see the migration note near `elapsed_weekdays`); the
  // huge n_obs_total keeps the fixing series open through both accrual steps.
  const SwapLot proto = var_swap_proto(kUid, expiry, /*n_obs_total=*/kHugeObsCap);
  SwapOnlyStrategy strat{proto};

  RunConfig cfg;
  cfg.swap_fixing_cadence = SwapFixingCadence::AcceptClockAsSchedule;
  auto result = run_backtest(*clock, strat, cfg);
  ASSERT_TRUE(result.has_value()) << result.error().to_string();
  const BacktestResult &r = *result;
  ASSERT_EQ(r.size(), 4u);
  ASSERT_EQ(r.swap_pv.size(), r.size());

  // Row 1 is the SEED step — the swap pass first sees the lot here, so its
  // fixing series is seeded at spots[1] with NOTHING accrued (n_done = 0,
  // Sigma r^2 = 0). The mark is the pure future leg over a 60-day residual.
  const std::int64_t ts1 = kBaseNow + 1LL * kStepNs;
  const PricedSurface surface1 = make_surface(kUid, spots[1], spots[1], ts1);
  const double ref1 = reference_swap_mark(surface1, ts1, proto, /*n_obs_done=*/0u,
                                          /*sum_sq_log_returns_done=*/0.0);

  // Row 2 carries ONE accrued return, so it exercises the aged blend: the mark
  // must read the accrual as of THIS snapshot's own fixing — not the previous
  // step's (stale) and not the terminal one's (look-ahead) — over a 30-day
  // residual, i.e. half of row 1's. A units error in either quantity moves the
  // two rows differently and cannot hide. Task A1: n_obs_done at row 2 is the
  // ELAPSED SESSION count for that step (AcceptClockAsSchedule), not a flat 1.
  const std::int64_t ts2 = kBaseNow + 2LL * kStepNs;
  const double ra = std::log(spots[2] / spots[1]);
  const std::uint32_t n1 = elapsed_weekdays(ts1, ts2);
  const PricedSurface surface2 = make_surface(kUid, spots[2], spots[2], ts2);
  const double ref2 = reference_swap_mark(surface2, ts2, proto, /*n_obs_done=*/n1,
                                          /*sum_sq_log_returns_done=*/ra * ra);

  // A zero reference would make the comparison vacuous; both legs must price.
  ASSERT_NE(ref1, 0.0);
  ASSERT_NE(ref2, 0.0);
  ASSERT_EQ(ref1, ref1); // not NaN: the oracle's own deriv_price call succeeded
  ASSERT_EQ(ref2, ref2);

  // Both sides run the identical strip/quadrature on the identical surface, so
  // this is an EXACT double comparison, not a tolerance.
  EXPECT_EQ(r.swap_pv[1], ref1);
  EXPECT_EQ(r.swap_pv[2], ref2);
  // The first mark carries the whole entry PV, so swap_pnl[1] is pinned too;
  // row 2's increment is then pinned by both marks being pinned.
  EXPECT_EQ(r.swap_pnl[1], ref1);
  EXPECT_EQ(r.swap_pnl[2], ref2 - ref1);
}

// ── 2. Zero-swap books are untouched ───────────────────────────────────────
TEST(BacktestSwap, OptionOnlyBookHasZeroSwapColumns) {
  const fs::path dir = fresh_dir("optiononly");
  const std::vector<double> spots = {100.0, 101.0, 99.0, 102.0, 103.0};
  const Corpus c = make_spot_corpus(dir, "SPX", spots);
  auto clock = Clock::from_manifest(c.manifest);
  ASSERT_TRUE(clock.has_value()) << clock.error().to_string();

  DeclarativeStrategy strat{single_clip(kUid, 0.75, Side::Put)};
  RunConfig cfg;
  auto result = run_backtest(*clock, strat, cfg);
  ASSERT_TRUE(result.has_value()) << result.error().to_string();
  const BacktestResult &r = *result;
  ASSERT_EQ(r.size(), 5u);
  ASSERT_EQ(r.swap_pv.size(), r.size());
  ASSERT_EQ(r.swap_pnl.size(), r.size());
  ASSERT_GT(r.n_open_lots.back(), 0.0); // the option lane really did run

  // The swap pass is skipped entirely on an empty swap book: both columns are
  // exactly +0.0 on every row, never a rounded-to-zero mark.
  for (std::size_t i = 0; i < r.size(); ++i) {
    EXPECT_TRUE(bits_equal(r.swap_pv[i], 0.0)) << i;
    EXPECT_TRUE(bits_equal(r.swap_pnl[i], 0.0)) << i;
  }

  // NAV identity: with a zero swap lane, `nav` is still exactly the running sum
  // of the per-step totals (step_total = pnl_total + settlement + shares +
  // swap + financing - cost, and `pnl_total` IS that sum at stride 1). Exact
  // double equality, not a tolerance — the only latitude is the sign of zero
  // (row 0 books `nav = -cost`, i.e. -0.0 in a frictionless run).
  double running = 0.0;
  for (std::size_t i = 0; i < r.size(); ++i) {
    running += r.pnl_total[i];
    EXPECT_EQ(r.nav[i], running) << i;
  }
  EXPECT_EQ(r.nav.back(), running);
}

// ── 3. Checkpoint resume reproduces the swap marks bit-for-bit ─────────────
TEST(BacktestSwap, CheckpointResumeReproducesSwapMarks) {
  const fs::path dir = fresh_dir("resume");
  const std::vector<double> spots = {100.0, 101.0, 99.0, 102.0, 98.0, 104.0};
  const Corpus c = make_spot_corpus(dir, "SPX", spots);
  auto clock = Clock::from_manifest(c.manifest);
  ASSERT_TRUE(clock.has_value()) << clock.error().to_string();

  // Seed at step 1, four accrued returns (steps 2..5), settling on the last one.
  // Task A1: opt in (huge n_obs_total keeps all four accrual steps genuinely
  // contributing under the elapsed-session scaling) -- see the migration note
  // near `elapsed_weekdays`. Bit-identity between the split and one-shot runs
  // below holds regardless of the cadence policy chosen, as long as both runs
  // share the same `cfg`, which they do.
  const std::int64_t expiry = kBaseNow + 5LL * kStepNs;
  const SwapLot proto = var_swap_proto(kUid, expiry, /*n_obs_total=*/kHugeObsCap);
  RunConfig cfg;
  cfg.swap_fixing_cadence = SwapFixingCadence::AcceptClockAsSchedule;

  SwapOnlyStrategy one_shot_strat{proto};
  auto one_shot = run_backtest_incremental(*clock, one_shot_strat, cfg, nullptr);
  ASSERT_TRUE(one_shot.has_value()) << one_shot.error().to_string();
  const BacktestResult &full = one_shot->rows;
  ASSERT_EQ(full.size(), 6u);

  // Split: [d0..d3] then resume over [d3..d5].
  auto head_clock = clock->between(c.dp[0].first, c.dp[3].first);
  ASSERT_TRUE(head_clock.has_value()) << head_clock.error().to_string();
  auto tail_clock = clock->between(c.dp[3].first, c.dp[5].first);
  ASSERT_TRUE(tail_clock.has_value()) << tail_clock.error().to_string();

  SwapOnlyStrategy head_strat{proto};
  auto head = run_backtest_incremental(*head_clock, head_strat, cfg, nullptr);
  ASSERT_TRUE(head.has_value()) << head.error().to_string();
  ASSERT_EQ(head->rows.size(), 4u);
  ASSERT_EQ(head->checkpoint.swap_accruals.size(), 1u);

  SwapOnlyStrategy tail_strat{proto};
  auto tail = run_backtest_incremental(*tail_clock, tail_strat, cfg, &head->checkpoint);
  ASSERT_TRUE(tail.has_value()) << tail.error().to_string();
  ASSERT_EQ(tail->rows.size(), 2u); // resumed runs omit the anchor row

  // Head rows reproduce the one-shot prefix; the resumed rows reproduce the
  // suffix — swap marks, swap PnL and NAV all bitwise.
  for (std::size_t i = 0; i < head->rows.size(); ++i) {
    EXPECT_TRUE(bits_equal(head->rows.swap_pv[i], full.swap_pv[i])) << i;
    EXPECT_TRUE(bits_equal(head->rows.swap_pnl[i], full.swap_pnl[i])) << i;
    EXPECT_TRUE(bits_equal(head->rows.nav[i], full.nav[i])) << i;
  }
  for (std::size_t i = 0; i < tail->rows.size(); ++i) {
    const std::size_t j = i + 4u;
    EXPECT_TRUE(bits_equal(tail->rows.swap_pv[i], full.swap_pv[j])) << i;
    EXPECT_TRUE(bits_equal(tail->rows.swap_pnl[i], full.swap_pnl[j])) << i;
    EXPECT_TRUE(bits_equal(tail->rows.nav[i], full.nav[j])) << i;
  }
  EXPECT_TRUE(bits_equal(tail->checkpoint.nav, one_shot->checkpoint.nav));
  EXPECT_TRUE(bits_equal(tail->checkpoint.cash, one_shot->checkpoint.cash));
  // The lot settled on the final step in both runs.
  EXPECT_TRUE(tail->checkpoint.portfolio.swap_lots.empty());
  EXPECT_TRUE(tail->checkpoint.swap_accruals.empty());
  EXPECT_TRUE(one_shot->checkpoint.portfolio.swap_lots.empty());
  EXPECT_TRUE(bits_equal(full.swap_pv.back(), 0.0));
}

// ── 4. A replayed snapshot timestamp is refused, never double-counted ──────
TEST(BacktestSwap, DuplicateTimestampRefusedNotDoubleCounted) {
  const fs::path dir = fresh_dir("duplicate");
  const std::vector<double> spots = {100.0, 101.0};
  Corpus c = make_spot_corpus(dir, "SPX", spots);
  // A third clock ref pointing at the SECOND date's archive: a distinct clock
  // date replaying an already-consumed fixing timestamp.
  c.dp.emplace_back("2026-08-03", c.dp[1].second);
  c.manifest = make_manifest(c.dp, "SPX");
  auto clock = Clock::from_manifest(c.manifest);
  ASSERT_TRUE(clock.has_value()) << clock.error().to_string();
  ASSERT_EQ(clock->size(), 3u);

  const std::int64_t expiry = kBaseNow + 9LL * kStepNs; // far beyond the corpus
  SwapOnlyStrategy strat{var_swap_proto(kUid, expiry, /*n_obs_total=*/8u)};

  RunConfig cfg;
  auto result = run_backtest(*clock, strat, cfg);
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code(), ErrorCode::AlreadyExists);
  // Right reason: the refusal is the swap lane's fixing guard, and it aborts the
  // run rather than accruing the replayed date a second time.
  EXPECT_NE(result.error().message().find("duplicate/backdated swap fixing"), std::string::npos)
      << result.error().to_string();
}

// ── 5. A live swap lot with no surface fails closed ────────────────────────
TEST(BacktestSwap, MissingSurfaceForSwapLotErrors) {
  const fs::path dir = fresh_dir("nosurface");
  const std::vector<double> spots = {100.0, 101.0, 99.0};
  const Corpus c = make_spot_corpus(dir, "SPX", spots);
  auto clock = Clock::from_manifest(c.manifest);
  ASSERT_TRUE(clock.has_value()) << clock.error().to_string();

  const std::int64_t expiry = kBaseNow + 9LL * kStepNs; // far beyond the corpus
  SwapOnlyStrategy strat{var_swap_proto(kMissingUid, expiry, /*n_obs_total=*/8u)};

  RunConfig cfg;
  auto result = run_backtest(*clock, strat, cfg);
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code(), ErrorCode::NotFound);
  EXPECT_NE(result.error().message().find("no surface for held swap lot"), std::string::npos)
      << result.error().to_string();
}

// ── 5b. The EXPIRY-DAY surface is required too: the close is the last fixing ──
TEST(BacktestSwap, MissingSurfaceOnExpiryDayErrors) {
  const fs::path dir = fresh_dir("nosurface-expiry");
  // The name goes dark on the LAST date — which is this lot's expiry. The lot
  // marked fine on every earlier step, so only the terminal fixing is missing;
  // settling anyway would divide a short Sigma r^2 by a short denominator.
  const std::vector<double> spots = {100.0, 101.0, 99.0, 102.0};
  const Corpus c = make_spot_corpus(dir, "SPX", spots, /*dark_from=*/3u);
  auto clock = Clock::from_manifest(c.manifest);
  ASSERT_TRUE(clock.has_value()) << clock.error().to_string();

  const std::int64_t expiry = kBaseNow + 3LL * kStepNs; // the dark date
  // Task A1: opt in so the run reaches the dark expiry step at all -- under
  // the default RequireEverySession the 30-calendar-day step (~21 weekday
  // sessions) would refuse one step earlier with SwapFixingScheduleViolation,
  // never reaching the missing-surface check this gate exists to pin. The
  // surface lookup this test cares about runs BEFORE the fixing/cadence
  // check regardless, so this is orthogonal to which cadence policy is set.
  SwapOnlyStrategy strat{var_swap_proto(kUid, expiry, /*n_obs_total=*/kHugeObsCap)};

  RunConfig cfg;
  cfg.swap_fixing_cadence = SwapFixingCadence::AcceptClockAsSchedule;
  auto result = run_backtest(*clock, strat, cfg);
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code(), ErrorCode::NotFound);
  EXPECT_NE(result.error().message().find("no surface for settling swap lot"), std::string::npos)
      << result.error().to_string();
}

// ── 6. A lot that reaches expiry with no observed return must not settle ─────
TEST(BacktestSwap, ZeroObservationSettlementErrors) {
  const fs::path dir = fresh_dir("zeroobs");
  const std::vector<double> spots = {100.0, 101.0, 99.0};
  const Corpus c = make_spot_corpus(dir, "SPX", spots);
  auto clock = Clock::from_manifest(c.manifest);
  ASSERT_TRUE(clock.has_value()) << clock.error().to_string();

  // Expiry on refs[1] — the very step that first SEES the lot, so its fixing
  // series is seeded and immediately terminal with zero accrued returns. The
  // estimator would be a fabricated 0.0 paying away the whole strike.
  const std::int64_t expiry = kBaseNow + 1LL * kStepNs;
  SwapOnlyStrategy strat{var_swap_proto(kUid, expiry, /*n_obs_total=*/1u)};

  RunConfig cfg;
  auto result = run_backtest(*clock, strat, cfg);
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code(), ErrorCode::NotFound);
  EXPECT_NE(result.error().message().find("no observed return"), std::string::npos)
      << result.error().to_string();
}

// ── 7. Transition guards: a strategy may not erase or mutate a swap lot ──────
TEST(BacktestSwap, StrategyErasingSwapLotErrors) {
  const fs::path dir = fresh_dir("erase");
  const std::vector<double> spots = {100.0, 101.0, 99.0, 102.0};
  const Corpus c = make_spot_corpus(dir, "SPX", spots);
  auto clock = Clock::from_manifest(c.manifest);
  ASSERT_TRUE(clock.has_value()) << clock.error().to_string();

  const std::int64_t expiry = kBaseNow + 9LL * kStepNs; // survives the corpus
  // Task A1: opt in so the run survives step 1's ~21-weekday-session gap and
  // reaches step 2, where the deliberate misbehavior under test happens --
  // otherwise the default RequireEverySession cadence guard aborts one step
  // earlier and this gate never exercises the transition check it targets.
  MisbehavingSwapStrategy strat{var_swap_proto(kUid, expiry, /*n_obs_total=*/kHugeObsCap),
                                MisbehavingSwapStrategy::Mode::EraseLot, /*bad_step=*/2u};

  RunConfig cfg;
  cfg.swap_fixing_cadence = SwapFixingCadence::AcceptClockAsSchedule;
  auto result = run_backtest(*clock, strat, cfg);
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code(), ErrorCode::InvalidArgument);
  EXPECT_NE(result.error().message().find("erased by strategy"), std::string::npos)
      << result.error().to_string();
}

TEST(BacktestSwap, StrategyMutatingSurvivingSwapLotErrors) {
  const fs::path dir = fresh_dir("mutate");
  const std::vector<double> spots = {100.0, 101.0, 99.0, 102.0};
  const Corpus c = make_spot_corpus(dir, "SPX", spots);
  auto clock = Clock::from_manifest(c.manifest);
  ASSERT_TRUE(clock.has_value()) << clock.error().to_string();

  const std::int64_t expiry = kBaseNow + 9LL * kStepNs;
  // Task A1: opt in for the same reason as StrategyErasingSwapLotErrors above.
  MisbehavingSwapStrategy strat{var_swap_proto(kUid, expiry, /*n_obs_total=*/kHugeObsCap),
                                MisbehavingSwapStrategy::Mode::MutateStrike, /*bad_step=*/2u};

  RunConfig cfg;
  cfg.swap_fixing_cadence = SwapFixingCadence::AcceptClockAsSchedule;
  auto result = run_backtest(*clock, strat, cfg);
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code(), ErrorCode::InvalidArgument);
  EXPECT_NE(result.error().message().find("mutated surviving swap lot"), std::string::npos)
      << result.error().to_string();
}

TEST(BacktestSwap, ReusedSwapLotIdErrors) {
  const fs::path dir = fresh_dir("reuseid");
  const std::vector<double> spots = {100.0, 101.0, 99.0, 102.0};
  const Corpus c = make_spot_corpus(dir, "SPX", spots);
  auto clock = Clock::from_manifest(c.manifest);
  ASSERT_TRUE(clock.has_value()) << clock.error().to_string();

  const std::int64_t expiry = kBaseNow + 9LL * kStepNs;
  // Task A1: opt in for the same reason as StrategyErasingSwapLotErrors above.
  MisbehavingSwapStrategy strat{var_swap_proto(kUid, expiry, /*n_obs_total=*/kHugeObsCap),
                                MisbehavingSwapStrategy::Mode::IdBelowWatermark, /*bad_step=*/2u};

  RunConfig cfg;
  cfg.swap_fixing_cadence = SwapFixingCadence::AcceptClockAsSchedule;
  auto result = run_backtest(*clock, strat, cfg);
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code(), ErrorCode::InvalidArgument);
  EXPECT_NE(result.error().message().find("duplicate swap lot id"), std::string::npos)
      << result.error().to_string();
}

// ── 8. append_backtest_results must not launder the swap lane away ───────────
TEST(BacktestSwap, AppendRefusesToLaunderSwapLaneAway) {
  // The DB never persists the swap lane, so `append_backtest_results` sees a
  // decoded prefix (lane ABSENT) meeting a fresh continuation (lane PRESENT).
  // Collapsing that to "absent" is only safe when neither side has real swap
  // data — otherwise it would both destroy the history and disarm the store
  // guard, which only ever inspects the combined result.
  const BacktestResult empty_lane_a = make_result(/*first_index=*/0u, /*n_rows=*/2u,
                                                  /*swap=*/false);
  const BacktestResult empty_lane_b = make_result(2u, 1u, /*swap=*/false);
  const BacktestResult zero_lane_b = make_result(2u, 1u, /*swap=*/true, /*swap_value=*/0.0);
  const BacktestResult live_lane_b = make_result(2u, 1u, /*swap=*/true, /*swap_value=*/12.5);

  // (1) Both sides lack the lane => plain append, lane stays absent.
  BacktestResult both_empty = empty_lane_a;
  ASSERT_TRUE(append_backtest_results(both_empty, empty_lane_b).has_value());
  EXPECT_EQ(both_empty.size(), 3u);
  EXPECT_TRUE(both_empty.swap_pv.empty());

  // (2) Shapes differ but the populated side is all-zero (the real DB-extension
  //     case) => collapse to absent, and stay row-consistent.
  BacktestResult mixed_zero = empty_lane_a;
  ASSERT_TRUE(append_backtest_results(mixed_zero, zero_lane_b).has_value());
  EXPECT_EQ(mixed_zero.size(), 3u);
  EXPECT_TRUE(mixed_zero.swap_pv.empty());
  EXPECT_TRUE(mixed_zero.swap_pnl.empty());

  // (3) Shapes differ AND one side carries real swap PnL => refuse.
  BacktestResult mixed_live = empty_lane_a;
  const Status refused = append_backtest_results(mixed_live, live_lane_b);
  ASSERT_FALSE(refused.has_value());
  EXPECT_EQ(refused.error().code(), ErrorCode::InvalidArgument);
  EXPECT_NE(refused.error().message().find("swap-lane shape change"), std::string::npos)
      << refused.error().to_string();

  // (4) Both sides carry the lane => genuine concatenation, nothing dropped.
  BacktestResult both_live = make_result(0u, 1u, /*swap=*/true, /*swap_value=*/3.0);
  ASSERT_TRUE(append_backtest_results(both_live, live_lane_b).has_value());
  ASSERT_EQ(both_live.swap_pnl.size(), 2u);
  EXPECT_EQ(both_live.swap_pnl[0], 3.0);
  EXPECT_EQ(both_live.swap_pnl[1], 12.5);
}

// ── 9. Task A1: the swap fixing-cadence guard ────────────────────────────────
//
// The pre-fix engine booked exactly one fixing per CLOCK STEP regardless of
// how many exchange sessions actually elapsed between steps, so a clock
// coarser than a swap's (implicitly daily) fixing schedule silently misstated
// realized variance. `RunConfig::swap_fixing_cadence` makes that fail closed
// by default and offers an explicit, accrual-scaling opt-in.
TEST(BacktestSwap, CoarseClockRefusedUnderRequireEverySession) {
  const fs::path dir = fresh_dir("cadence-refuse");
  const std::vector<DatedSpot> points = {
      {"2023-10-02", kGapD0, 100.0},
      {"2023-10-03", kGapD1, 101.0},
      {"2023-10-04", kGapD2, 99.0},
      {"2023-11-15", kGapD3, 102.0}, // the 42-calendar-day / 30-weekday-session jump
  };
  const Corpus c = make_spot_corpus_at(dir, "SPX", points);
  auto clock = Clock::from_manifest(c.manifest);
  ASSERT_TRUE(clock.has_value()) << clock.error().to_string();

  // Expiry far beyond the corpus: this run is expected to abort ON the gap
  // step and never reach settlement.
  SwapOnlyStrategy strat{var_swap_proto(kUid, kGapFarExpiry, /*n_obs_total=*/8u)};

  RunConfig cfg; // swap_fixing_cadence defaults to RequireEverySession
  auto result = run_backtest(*clock, strat, cfg);
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code(), ErrorCode::SwapFixingScheduleViolation);
  // Names the offending step (ts) and expected-vs-seen fixing count. This
  // 2023-dated gap is outside VolTimeCalendar's covered window, so the count
  // is plain weekdays (30), not holiday-adjusted -- see
  // HolidayAwareCoarseClockAcceptedUnderRequireEverySession below for the
  // in-window, holiday-aware path.
  EXPECT_NE(result.error().message().find(std::to_string(kGapD3)), std::string::npos)
      << result.error().to_string();
  EXPECT_NE(result.error().message().find("spans 30 session"), std::string::npos)
      << result.error().to_string();
  EXPECT_NE(result.error().message().find("expected 1 session"), std::string::npos)
      << result.error().to_string();
}

// ── 9b. The SAME gap, accepted (and scaled) under AcceptClockAsSchedule ──────
//
// Opting in books the gap's one observed return but scales `n_obs_done` by
// the elapsed weekday-session count (1 for the ordinary accrual, 30 for the
// gapped one) instead of always advancing it by 1 -- so the daily-strike
// convention (`annualization * Sigma r^2 / n_done`) is not overstated ~30x by
// the gap. The lot settles exactly at the gapped snapshot (expiry == the last
// snapshot), and the payoff is hand-computed exactly as gate 1 does, with
// n_done = 1 + 30 = 31 (the SUM of elapsed sessions) in place of the step
// count (2).
TEST(BacktestSwap, GapAcceptedUnderAcceptClockAsSchedule_ScalesAccrual) {
  const fs::path dir = fresh_dir("cadence-accept");
  const std::vector<double> spots = {100.0, 101.0, 99.0, 102.0};
  const std::vector<DatedSpot> points = {
      {"2023-10-02", kGapD0, spots[0]},
      {"2023-10-03", kGapD1, spots[1]},
      {"2023-10-04", kGapD2, spots[2]},
      {"2023-11-15", kGapD3, spots[3]},
  };
  const Corpus c = make_spot_corpus_at(dir, "SPX", points);
  auto clock = Clock::from_manifest(c.manifest);
  ASSERT_TRUE(clock.has_value()) << clock.error().to_string();

  const std::int64_t expiry = kGapD3; // settles exactly at the gapped snapshot
  SwapOnlyStrategy strat{var_swap_proto(kUid, expiry, /*n_obs_total=*/2u)};

  RunConfig cfg;
  cfg.swap_fixing_cadence = SwapFixingCadence::AcceptClockAsSchedule;
  auto result = run_backtest(*clock, strat, cfg);
  ASSERT_TRUE(result.has_value()) << result.error().to_string();
  const BacktestResult &r = *result;
  ASSERT_EQ(r.size(), 4u);

  const double ra = std::log(spots[2] / spots[1]);
  const double rb = std::log(spots[3] / spots[2]);
  const double sum_sq = ra * ra + rb * rb;
  const double n_done = 1.0 + 30.0; // elapsed-session SUM, not the step count
  const double rv_dec = kAnnualization * sum_sq / n_done;
  const double payoff = kQty * kNotional * (rv_dec - kStrikeDec);

  EXPECT_NEAR(r.cash.back(), payoff, 1.0e-9);
  EXPECT_NEAR(r.nav.back(), payoff, 1.0e-9);
}

// ── 9c. Post-review fixup: a real NYSE holiday does NOT trip the guard ──────
//
// Reviewer finding: the original `weekday_sessions_between` counted plain
// Mon-Fri weekdays everywhere, so an ORDINARY daily production clock whose
// step happened to span a single NYSE full closure (Thanksgiving, July 4th,
// Christmas, ...) reported elapsed=2 and refused under the default
// `RequireEverySession`, even though the contract's daily fixing schedule
// was honored exactly -- a false positive on the most common in-window
// production case. Fixed: within `VolTimeCalendar::us_default()`'s covered
// window (2024-2028), `weekday_sessions_between` now subtracts the table's
// listed full closures before counting.
//
// 2024-11-27 (Wed) -> 2024-11-29 (Fri) spans Thanksgiving (2024-11-28, a
// listed closure in vol_time.cpp's table) and NO weekend day. Plain weekday
// counting sees 2 elapsed days (Thu, Fri); holiday-aware counting correctly
// sees 1 elapsed SESSION (Thu is not a trading day at all). Both endpoints
// fall inside the covered window, so this run must succeed under the
// DEFAULT cadence with elapsed == 1, not fail closed.
TEST(BacktestSwap, HolidayAwareCoarseClockAcceptedUnderRequireEverySession) {
  const fs::path dir = fresh_dir("cadence-holiday");
  constexpr std::int64_t kDayBefore = 1732579200000000000LL;   // 2024-11-26 Tuesday
  constexpr std::int64_t kPreHoliday = 1732665600000000000LL;  // 2024-11-27 Wednesday (seed)
  constexpr std::int64_t kPostHoliday = 1732838400000000000LL; // 2024-11-29 Friday (accrual)
  const std::vector<DatedSpot> points = {
      {"2024-11-26", kDayBefore, 100.0},
      {"2024-11-27", kPreHoliday, 101.0},
      {"2024-11-29", kPostHoliday, 99.0}, // steps OVER 2024-11-28 (Thanksgiving)
  };
  const Corpus c = make_spot_corpus_at(dir, "SPX", points);
  auto clock = Clock::from_manifest(c.manifest);
  ASSERT_TRUE(clock.has_value()) << clock.error().to_string();

  // Expiry far beyond the corpus: this test only cares that the run SURVIVES
  // the holiday-spanning step under the default cadence, not settlement.
  const std::int64_t expiry = kDayBefore + 200LL * kDayNs; // 2025-06-14, far beyond
  const SwapLot proto = var_swap_proto(kUid, expiry, /*n_obs_total=*/8u);
  SwapOnlyStrategy strat{proto};

  RunConfig cfg; // swap_fixing_cadence defaults to RequireEverySession
  auto cont = run_backtest_incremental(*clock, strat, cfg, nullptr);
  ASSERT_TRUE(cont.has_value()) << cont.error().to_string();
  ASSERT_EQ(cont->checkpoint.swap_accruals.size(), 1u);
  const SwapAccrual &acc = cont->checkpoint.swap_accruals.front();
  EXPECT_TRUE(acc.have_prev);
  EXPECT_EQ(acc.prev_ts_ns, kPostHoliday);
  // The direct, numeric pin: ONE elapsed session over the holiday-spanning
  // step, not two.
  EXPECT_EQ(acc.rv.n_obs_done, 1u);
  const double r1 = std::log(99.0 / 101.0);
  EXPECT_LT(std::fabs(acc.rv.sum_sq_log_returns_done - r1 * r1), 1.0e-15);
  EXPECT_LT(std::fabs(acc.rv.rv_done_dec - kAnnualization * r1 * r1), 1.0e-13);
}

// ── 9d. Out-of-window fallback still counts pure weekdays (regression pin) ──
//
// `CoarseClockRefusedUnderRequireEverySession` above already exercises the
// out-of-window (2023) fallback for the GAPPED step (30 plain weekdays, no
// holiday table consulted, since 2023 predates the 2024-2028 covered
// window). This test pins the same fallback for the ORDINARY one-session
// case, using the same 2023-dated `kGapD0`/`kGapD1`/`kGapD2` points (one
// genuine weekday apart each) but stopping short of the gap, so the run
// completes successfully rather than erroring -- confirming the fallback
// reads a ROUTINE daily step correctly, not just a coarse one.
TEST(BacktestSwap, OutOfWindowDailyClockStillAcceptedUnderRequireEverySession) {
  const fs::path dir = fresh_dir("cadence-outofwindow");
  const std::vector<DatedSpot> points = {
      {"2023-10-02", kGapD0, 100.0},
      {"2023-10-03", kGapD1, 101.0},
      {"2023-10-04", kGapD2, 99.0},
  };
  const Corpus c = make_spot_corpus_at(dir, "SPX", points);
  auto clock = Clock::from_manifest(c.manifest);
  ASSERT_TRUE(clock.has_value()) << clock.error().to_string();

  const std::int64_t expiry = kGapFarExpiry; // far beyond this 3-entry corpus
  SwapOnlyStrategy strat{var_swap_proto(kUid, expiry, /*n_obs_total=*/8u)};

  RunConfig cfg; // swap_fixing_cadence defaults to RequireEverySession
  auto cont = run_backtest_incremental(*clock, strat, cfg, nullptr);
  ASSERT_TRUE(cont.has_value()) << cont.error().to_string();
  ASSERT_EQ(cont->checkpoint.swap_accruals.size(), 1u);
  EXPECT_EQ(cont->checkpoint.swap_accruals.front().rv.n_obs_done, 1u);
}
