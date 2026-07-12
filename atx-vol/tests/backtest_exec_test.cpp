// atx-vol backtest engine (Phase B2) execution/ledger/hedge gate tests.
//
// Drives the strategy-aware `run_backtest` with the B2 additions — modeled
// frictions, an engine-internal cash/borrow ledger, and the engine-owned
// delta-hedge overlay — over synthetic single-underlying eSSVI corpora (the
// backtest_test make_surface pattern; analytic, no fitting, runs everywhere).
//
// Five gates:
//   1. ZeroFrictionIdentity — default RunConfig{} (frictionless, financing off,
//      hedge None) is bit-identical to an explicitly-zeroed config, and the
//      cost/pnl_shares/financing columns are exactly 0.0 every row.
//   2. NavReconciliation    — for a delta-hedged put with cash-carry financing,
//      the independently recomputed book-value NAV increment == step_total.
//   3. Financing            — cash grows at exp(r*dt); short shares bleed
//      borrow_rate*|short|*S*dt.
//   4. HedgeOverlay         — post-hedge |net book delta| <= band each row; the
//      hedge PnL neutralizes the option delta axis (pnl_shares ≈ -pnl_delta).
//   5. FrictionMonotonicity — final nav decreases and total cost increases with
//      half_spread_bps.

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "atx/core/error.hpp"            // atx::core::Ok
#include "atx/vol/adjusted_greeks.hpp"   // StickyParams
#include "atx/vol/american.hpp"          // al_fast_opts, AmericanMethod, AmericanGreeks
#include "atx/vol/backtest.hpp"          // Clock, run_backtest, RunConfig, FrictionModel, ...
#include "atx/vol/corpus.hpp"            // CorpusManifest, CorpusEntry, CorpusFitStatus
#include "atx/vol/portfolio_pricer.hpp"  // OptionContract, kNsPerYear
#include "atx/vol/priced_surface.hpp"    // PricedSurface, PricingContext
#include "atx/vol/strategy.hpp"          // DeclarativeStrategy, StrategySpec, HedgeSpec
#include "atx/vol/surface_archive.hpp"   // write_surface_archive_file, SurfaceArchiveItem
#include "atx/vol/surface_parity.hpp"    // SliceContext
#include "atx/vol/types.hpp"             // Side, Result, Status
#include "atx/vol/vol_curve.hpp"         // CurveSurface, EssviCurve
#include "atx/vol/vol_surface.hpp"       // EssviParams

using namespace atx::vol;
namespace fs = std::filesystem;

namespace {

constexpr double kR = 0.043;
constexpr std::int64_t kBaseNow = 1700000000000000000LL;
constexpr std::int64_t kDayNs = 86400LL * 1000000000LL;
constexpr std::uint32_t kUid = 7;

[[nodiscard]] bool bits_equal(double a, double b) noexcept {
  std::uint64_t ba = 0;
  std::uint64_t bb = 0;
  std::memcpy(&ba, &a, sizeof ba);
  std::memcpy(&bb, &b, sizeof bb);
  return ba == bb;
}

// A synthetic eSSVI PricedSurface (flat forward, genuine American premium via
// q_eff=0.02), slices T in [0.05, 1.0]. Mirrors backtest_test's make_surface.
[[nodiscard]] PricedSurface make_surface(std::uint32_t uid, double S, double fwd,
                                         std::int64_t now_ts, double vol_bump = 0.0) {
  CurveSurface cs;
  std::vector<SliceContext> ctx;
  const double Ts[] = {0.05, 0.10, 0.20, 0.35, 0.50, 0.75, 1.00};
  int i = 0;
  for (const double T : Ts) {
    EssviParams e{};
    e.theta = 0.04 + 0.005 * static_cast<double>(i) + vol_bump;
    e.phi = 1.5 - 0.05 * static_cast<double>(i);
    e.rho = -0.4 + 0.02 * static_cast<double>(i);
    e.psi = 0.5;
    e.p = 0.5;
    e.lambda = 0.5;
    e.T = T;
    e.F = fwd;
    e.expiry_id = static_cast<std::uint16_t>(i);
    cs.push(std::make_unique<EssviCurve>(e, std::exp(-kR * T)));
    ctx.push_back(SliceContext{T, fwd, 0.0, 0.02, 250, 7});
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

[[nodiscard]] fs::path fresh_dir(const char* tag) {
  const fs::path dir = fs::temp_directory_path() / (std::string("atx-btexec-") + tag);
  std::error_code ec;
  fs::remove_all(dir, ec);
  return dir;
}

// Write one surface as this date's archive; return its path (creating `dir`).
[[nodiscard]] std::string write_one(const fs::path& dir, const std::string& date,
                                    const std::string& symbol, const PricedSurface& s) {
  std::error_code ec;
  fs::create_directories(dir, ec);
  const std::string path = (dir / (date + ".atxvsa")).string();
  const SurfaceArchiveItem item{symbol, &s};
  const std::span<const SurfaceArchiveItem> items(&item, 1);
  const Status st = write_surface_archive_file(path, items);
  EXPECT_TRUE(st.has_value()) << (st.has_value() ? std::string{} : st.error().to_string());
  return path;
}

// Hand-build an Ok-only manifest over (date, archive_path) rows (one entry/date).
[[nodiscard]] CorpusManifest make_manifest(
    const std::vector<std::pair<std::string, std::string>>& date_paths, const std::string& symbol) {
  CorpusManifest m;
  for (const auto& [date, path] : date_paths) {
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

// A single-underlying evolving corpus: spot drifts, valuation advances one day,
// vol drifts up. Returns the manifest plus per-date archive paths for in-test
// reconstruction (reloading a snapshot to reprice the known book).
struct Corpus {
  CorpusManifest manifest;
  std::vector<std::pair<std::string, std::string>> dp;  // (date, path), ascending
};

[[nodiscard]] Corpus make_corpus(const fs::path& dir, const std::string& symbol, int n_dates,
                                 double s0 = 100.0, double drift = 0.004, double vdrift = 0.001) {
  std::vector<std::pair<std::string, std::string>> dp;
  for (int d = 0; d < n_dates; ++d) {
    const std::int64_t now = kBaseNow + static_cast<std::int64_t>(d) * kDayNs;
    const double S = s0 * (1.0 + drift * static_cast<double>(d));
    const double vb = vdrift * static_cast<double>(d);
    const PricedSurface s = make_surface(kUid, S, S, now, vb);
    char buf[16];
    std::snprintf(buf, sizeof buf, "2026-08-%02d", d + 1);
    const std::string date = buf;
    dp.emplace_back(date, write_one(dir, date, symbol, s));
  }
  Corpus c;
  c.dp = std::move(dp);
  c.manifest = make_manifest(c.dp, symbol);
  return c;
}

[[nodiscard]] double res_T(std::int64_t expiry, std::int64_t now) noexcept {
  return (static_cast<double>(expiry) - static_cast<double>(now)) / kNsPerYear;
}

// Position-scaled option delta of a single (K,T,side) lot on `surf` (qty*mult*delta).
[[nodiscard]] double lot_delta(const PricedSurface& surf, double K, double T, Side side, double qty,
                               double mult) {
  const auto gr = surf.greeks(K, T, side);
  EXPECT_TRUE(gr.has_value()) << (gr.has_value() ? std::string{} : gr.error().to_string());
  return qty * mult * gr->delta;
}

// Independent oracle for the skew-adjusted delta: delta + VegaSlope * vega,
// VegaSlope = (1 - omega) * (-dSigma/dk) / F(T), dSigma/dk a central FD
// (h = 1e-4) on `PricedSurface::total_variance` in log-moneyness space —
// mirrors (does NOT call) portfolio_pricer.cpp's local
// `priced_surface_skew_slope` helper, so this test proves the production
// seam against a from-scratch re-derivation rather than the implementation
// under test.
[[nodiscard]] double adjusted_call_delta(const PricedSurface& surf, double K, double T, Side side,
                                         double omega) {
  const auto gr = surf.greeks(K, T, side);
  EXPECT_TRUE(gr.has_value()) << (gr.has_value() ? std::string{} : gr.error().to_string());
  const double F = surf.forward_at(T);
  const double k_log = std::log(K / F);
  constexpr double h = 1e-4;
  const double w0 = surf.total_variance(K, T);
  const double sigma = std::sqrt(w0 / T);
  const double Kp = F * std::exp(k_log + h);
  const double Km = F * std::exp(k_log - h);
  const double dw_dk = (surf.total_variance(Kp, T) - surf.total_variance(Km, T)) / (2.0 * h);
  const double slope = dw_dk / (2.0 * sigma * T);
  const double vega_slope = (1.0 - omega) * (-slope / F);
  return gr->delta + vega_slope * gr->vega;
}

// A do-nothing strategy (empty book every step) — the flat-book financing seam.
class NoopStrategy : public IStrategy {
 public:
  Status on_step(const MarketSnapshot& /*base*/, std::size_t /*step_index*/,
                 PortfolioState& /*book*/, std::uint64_t& /*next_lot_id*/) override {
    return atx::core::Ok();
  }
};

// A single-clip declarative strategy: opens ONE structure at inception and holds it
// (EveryNDays with a cadence longer than the corpus ⇒ only step 0 opens).
[[nodiscard]] StrategySpec single_clip(std::uint32_t uid, double target_T, Side side,
                                       StrikeSelector strike, HedgeSpec hedge = {}) {
  StrategySpec spec;
  spec.name = "single-clip";
  LegSpec leg;
  leg.uid = uid;
  leg.tenor.target_T = target_T;
  leg.structure.kind = StructureSpec::Kind::Single;
  leg.structure.single_side = side;
  leg.strike = strike;
  leg.size = SizeSpec{SizeSpec::Kind::FixedContracts, 1.0, +1.0};
  spec.legs.push_back(leg);
  spec.lifecycle.entry = LifecycleSpec::Entry::EveryNDays;
  spec.lifecycle.holding = LifecycleSpec::Holding::HoldToExpiry;
  spec.lifecycle.entry_every_n = 100000;  // >> corpus ⇒ opens once, at inception
  spec.hedge = hedge;
  return spec;
}

}  // namespace

// ── 1. Zero-friction / feature-off identity ─────────────────────────────────
TEST(BacktestExec, ZeroFrictionIdentity) {
  const fs::path dir = fresh_dir("identity");
  const Corpus c = make_corpus(dir, "SPX", 6);
  auto clock = Clock::from_manifest(c.manifest);
  ASSERT_TRUE(clock.has_value()) << clock.error().to_string();

  // A single-leg ATM put clip, new cohort every step, held to expiry, NO hedge.
  StrategySpec spec;
  spec.name = "atm-put-clip";
  LegSpec leg;
  leg.uid = kUid;
  leg.tenor.target_T = 0.25;
  leg.structure.kind = StructureSpec::Kind::Single;
  leg.structure.single_side = Side::Put;
  leg.strike = StrikeSelector{StrikeSelector::Kind::AtmForward, 0.0};
  leg.size = SizeSpec{SizeSpec::Kind::FixedContracts, 1.0, +1.0};
  spec.legs.push_back(leg);
  spec.lifecycle.entry = LifecycleSpec::Entry::EveryStep;
  spec.lifecycle.holding = LifecycleSpec::Holding::HoldToExpiry;

  DeclarativeStrategy s_default{spec};
  DeclarativeStrategy s_zeroed{spec};

  RunConfig def{};  // frictions None, financing off, hedge None (all defaults)
  RunConfig zeroed{};
  zeroed.frictions.spread_kind = FrictionModel::SpreadKind::None;
  zeroed.frictions.half_spread_bps = 0.0;
  zeroed.frictions.vol_tick = 0.0;
  zeroed.frictions.per_contract_cost = 0.0;
  zeroed.frictions.hedge_slippage_bps = 0.0;
  zeroed.financing.borrow_rate = 0.0;
  zeroed.financing.finance_premium = false;
  zeroed.financing.shares_carry = false;
  zeroed.financing.initial_cash = 0.0;

  auto rd = run_backtest(*clock, s_default, def);
  auto rz = run_backtest(*clock, s_zeroed, zeroed);
  ASSERT_TRUE(rd.has_value()) << rd.error().to_string();
  ASSERT_TRUE(rz.has_value()) << rz.error().to_string();
  const BacktestResult& a = *rd;
  const BacktestResult& b = *rz;
  ASSERT_EQ(a.size(), b.size());
  ASSERT_GT(a.size(), 1u);

  const std::vector<std::pair<const std::vector<double>*, const std::vector<double>*>> cols = {
      {&a.pnl_total, &b.pnl_total},         {&a.pnl_delta, &b.pnl_delta},
      {&a.pnl_gamma, &b.pnl_gamma},         {&a.pnl_vega, &b.pnl_vega},
      {&a.pnl_vanna, &b.pnl_vanna},         {&a.pnl_volga, &b.pnl_volga},
      {&a.pnl_theta, &b.pnl_theta},         {&a.pnl_rho, &b.pnl_rho},
      {&a.pnl_charm, &b.pnl_charm},         {&a.pnl_unexplained, &b.pnl_unexplained},
      {&a.pnl_settlement, &b.pnl_settlement}, {&a.nav, &b.nav},
      {&a.gross_delta, &b.gross_delta},     {&a.gross_gamma, &b.gross_gamma},
      {&a.gross_vega, &b.gross_vega},       {&a.gross_theta, &b.gross_theta},
      {&a.n_open_lots, &b.n_open_lots},     {&a.n_unpriced_lots, &b.n_unpriced_lots},
      {&a.n_unpriced_greeks, &b.n_unpriced_greeks}};
  for (std::size_t i = 0; i < a.size(); ++i) {
    for (const auto& [va, vb] : cols) {
      EXPECT_TRUE(bits_equal((*va)[i], (*vb)[i])) << "col mismatch at row " << i;
    }
    // The disabled ledger/friction path is exactly zero.
    EXPECT_EQ(a.cost[i], 0.0) << i;
    EXPECT_EQ(a.pnl_shares[i], 0.0) << i;
    EXPECT_EQ(a.financing[i], 0.0) << i;
  }
  std::printf("[btexec] identity rows=%zu (default == explicit-zero, ledger cols 0)\n", a.size());
}

// ── 2. NAV reconciliation (book value increment == step_total) ──────────────
TEST(BacktestExec, NavReconciliation) {
  const fs::path dir = fresh_dir("navrecon");
  const Corpus c = make_corpus(dir, "SPX", 7);
  auto clock = Clock::from_manifest(c.manifest);
  ASSERT_TRUE(clock.has_value()) << clock.error().to_string();

  const double target_T = 0.5;  // expiry beyond the corpus ⇒ no settlement
  const StrategySpec spec =
      single_clip(kUid, target_T, Side::Put, StrikeSelector{StrikeSelector::Kind::AtmForward, 0.0},
                  HedgeSpec{HedgeSpec::Kind::DeltaToZero, HedgeSpec::Cadence::Daily, 1e-6});
  DeclarativeStrategy strat{spec};

  RunConfig cfg;
  cfg.financing.finance_premium = true;   // cash carry on
  cfg.financing.initial_cash = 1'000'000.0;

  auto res = run_backtest(*clock, strat, cfg);
  ASSERT_TRUE(res.has_value()) << res.error().to_string();
  const BacktestResult& r = *res;
  ASSERT_EQ(r.size(), c.dp.size());

  // Reconstruct the single put lot the engine opened at inception.
  auto snap0 = MarketSnapshot::load(c.dp[0].second);
  ASSERT_TRUE(snap0.has_value()) << snap0.error().to_string();
  const double K = snap0->find(kUid)->forward_at(target_T);
  const std::int64_t expiry = snap0->ts_ns() + std::llround(target_T * kNsPerYear);
  const double qty = 1.0;
  const double mult = 100.0;

  // Book value NAV at each row: cash + option MTM + shares*S, with shares backed out
  // of the (net-delta) gross_delta column: shares = gross_delta - option_delta.
  std::vector<double> bv(r.size(), 0.0);
  for (std::size_t i = 0; i < r.size(); ++i) {
    auto snap = MarketSnapshot::load(c.dp[i].second);
    ASSERT_TRUE(snap.has_value()) << snap.error().to_string();
    const PricedSurface* s = snap->find(kUid);
    const double T = res_T(expiry, snap->ts_ns());
    const auto fv = s->fair_value(K, T, Side::Put);
    ASSERT_TRUE(fv.has_value()) << fv.error().to_string();
    const double opt_mtm = qty * mult * (*fv);
    const double opt_delta = lot_delta(*s, K, T, Side::Put, qty, mult);
    const double shares = r.gross_delta[i] - opt_delta;
    const double S = s->pricing().S;
    bv[i] = r.cash[i] + opt_mtm + shares * S;
  }

  double worst = 0.0;
  for (std::size_t i = 1; i < r.size(); ++i) {
    const double d_bv = bv[i] - bv[i - 1];
    const double d_nav = r.nav[i] - r.nav[i - 1];
    worst = std::max(worst, std::fabs(d_bv - d_nav));
    EXPECT_NEAR(d_bv, d_nav, 1e-6 * (std::fabs(d_nav) + 1.0)) << "row " << i;
  }
  std::printf("[btexec] nav-reconciliation worst |dBV - d_step_total| = %.3e\n", worst);
}

// ── 3. Financing: cash carry + short-share borrow ───────────────────────────
TEST(BacktestExec, Financing) {
  // (a) Flat book: cash grows at exp(r*dt) per step, financing == cash-carry.
  {
    const fs::path dir = fresh_dir("fincash");
    const Corpus c = make_corpus(dir, "SPX", 5);
    auto clock = Clock::from_manifest(c.manifest);
    ASSERT_TRUE(clock.has_value()) << clock.error().to_string();

    NoopStrategy noop;
    RunConfig cfg;
    cfg.financing.finance_premium = true;
    cfg.financing.initial_cash = 1'000'000.0;

    auto res = run_backtest(*clock, noop, cfg);
    ASSERT_TRUE(res.has_value()) << res.error().to_string();
    const BacktestResult& r = *res;
    ASSERT_GT(r.size(), 1u);
    EXPECT_EQ(r.cash[0], 1'000'000.0);  // no trades ⇒ inception cash == initial

    for (std::size_t i = 1; i < r.size(); ++i) {
      const double dt = static_cast<double>(r.ts_ns[i] - r.ts_ns[i - 1]) / kNsPerYear;
      const double growth = std::exp(kR * dt);
      EXPECT_NEAR(r.cash[i], r.cash[i - 1] * growth, 1e-6 * r.cash[i - 1]) << "row " << i;
      EXPECT_NEAR(r.financing[i], r.cash[i - 1] * (growth - 1.0), 1e-6 * r.cash[i - 1]) << i;
    }
    std::printf("[btexec] cash-carry: cash[0]=%.2f -> cash[last]=%.2f (r=%.3f)\n", r.cash.front(),
                r.cash.back(), kR);
  }

  // (b) Short-share book: long call, delta-hedged ⇒ short shares that bleed borrow.
  {
    const fs::path dir = fresh_dir("finborrow");
    const Corpus c = make_corpus(dir, "SPX", 6);
    auto clock = Clock::from_manifest(c.manifest);
    ASSERT_TRUE(clock.has_value()) << clock.error().to_string();

    const double target_T = 0.5;
    const StrategySpec spec = single_clip(
        kUid, target_T, Side::Call, StrikeSelector{StrikeSelector::Kind::AtmForward, 0.0},
        HedgeSpec{HedgeSpec::Kind::DeltaToZero, HedgeSpec::Cadence::Daily, 1e-6});
    DeclarativeStrategy strat{spec};

    RunConfig cfg;
    cfg.financing.finance_premium = false;  // isolate the borrow term
    cfg.financing.borrow_rate = 0.05;

    auto res = run_backtest(*clock, strat, cfg);
    ASSERT_TRUE(res.has_value()) << res.error().to_string();
    const BacktestResult& r = *res;
    ASSERT_EQ(r.size(), c.dp.size());

    auto snap0 = MarketSnapshot::load(c.dp[0].second);
    ASSERT_TRUE(snap0.has_value()) << snap0.error().to_string();
    const double K = snap0->find(kUid)->forward_at(target_T);
    const std::int64_t expiry = snap0->ts_ns() + std::llround(target_T * kNsPerYear);

    int short_steps = 0;
    for (std::size_t i = 1; i < r.size(); ++i) {
      // shares held over step i were set at the PREVIOUS base (row i-1).
      auto snap_prev = MarketSnapshot::load(c.dp[i - 1].second);
      ASSERT_TRUE(snap_prev.has_value()) << snap_prev.error().to_string();
      const PricedSurface* sp = snap_prev->find(kUid);
      const double T_prev = res_T(expiry, snap_prev->ts_ns());
      const double opt_delta = lot_delta(*sp, K, T_prev, Side::Call, 1.0, 100.0);
      const double shares_prev = r.gross_delta[i - 1] - opt_delta;
      const double S_prev = sp->pricing().S;
      const double dt = static_cast<double>(r.ts_ns[i] - r.ts_ns[i - 1]) / kNsPerYear;
      const double short_amt = std::max(0.0, -shares_prev);
      const double expected = -0.05 * short_amt * S_prev * dt;

      EXPECT_LT(shares_prev, 0.0) << "hedging a long call should hold SHORT shares, row " << i;
      EXPECT_NEAR(r.financing[i], expected, 1e-6 * (std::fabs(expected) + 1.0)) << "row " << i;
      EXPECT_LT(r.financing[i], 0.0) << "borrow is a cost, row " << i;
      if (shares_prev < 0.0) {
        ++short_steps;
      }
    }
    EXPECT_GT(short_steps, 0);
    std::printf("[btexec] borrow: %d short-share steps bleeding at rate 0.05\n", short_steps);
  }
}

// ── 4. Hedge overlay: net delta banded, hedge PnL offsets the delta axis ─────
TEST(BacktestExec, HedgeOverlay) {
  const fs::path dir = fresh_dir("hedge");
  const Corpus c = make_corpus(dir, "SPX", 8, 100.0, 0.006, 0.001);  // livelier spot moves
  auto clock = Clock::from_manifest(c.manifest);
  ASSERT_TRUE(clock.has_value()) << clock.error().to_string();

  const double band = 1e-6;
  const StrategySpec spec =
      single_clip(kUid, 0.25, Side::Put, StrikeSelector{StrikeSelector::Kind::Delta, 0.25},
                  HedgeSpec{HedgeSpec::Kind::DeltaToZero, HedgeSpec::Cadence::Daily, band});
  DeclarativeStrategy strat{spec};

  auto res = run_backtest(*clock, strat);  // frictionless; hedge only
  ASSERT_TRUE(res.has_value()) << res.error().to_string();
  const BacktestResult& r = *res;
  ASSERT_GT(r.size(), 2u);

  // Post-hedge net book delta (option + shares, folded into gross_delta) is banded.
  double worst_net = 0.0;
  for (std::size_t i = 0; i < r.size(); ++i) {
    worst_net = std::max(worst_net, std::fabs(r.gross_delta[i]));
    EXPECT_LE(std::fabs(r.gross_delta[i]), band + 1e-6) << "row " << i;
  }

  // The share hedge neutralizes the option's delta axis: pnl_shares ≈ -pnl_delta,
  // leaving the long option to run its gamma/theta. (Residual is FP-level because
  // shares held over each step == -option delta at that step's base.)
  double sum_shares = 0.0;
  double sum_delta = 0.0;
  for (std::size_t i = 0; i < r.size(); ++i) {
    sum_shares += r.pnl_shares[i];
    sum_delta += r.pnl_delta[i];
  }
  EXPECT_GT(std::fabs(sum_delta), 0.0);
  EXPECT_LT(sum_shares * sum_delta, 0.0) << "hedge PnL must oppose the option delta axis";
  EXPECT_NEAR(sum_shares + sum_delta, 0.0, 1e-6 * (std::fabs(sum_delta) + 1.0));
  std::printf("[btexec] hedge: worst |net delta|=%.3e  Σpnl_shares=%.4f  Σpnl_delta=%.4f\n",
              worst_net, sum_shares, sum_delta);
}

// ── 4b. Skew-adjusted delta: hedger inherits it via PriceFrame, zero hedger
//        code change (I6) ─────────────────────────────────────────────────
TEST(Backtest, HedgeTradesOnAdjustedDelta) {
  // Deterministic 2-day synthetic backtest, hedge band ~0, on the file's
  // standard put-skewed eSSVI corpus (make_surface's rho = -0.4 + 0.02*i is
  // negative for the near-dated slices AtmForward resolves against). A long
  // ATM-forward call, hedged daily to a ~0 band, held across the one step.
  const fs::path dir = fresh_dir("skewdelta");
  const Corpus c = make_corpus(dir, "SPX", 2);
  auto clock = Clock::from_manifest(c.manifest);
  ASSERT_TRUE(clock.has_value()) << clock.error().to_string();

  const double band = 1e-6;
  const double target_T = 0.25;
  const StrategySpec spec =
      single_clip(kUid, target_T, Side::Call, StrikeSelector{StrikeSelector::Kind::AtmForward, 0.0},
                  HedgeSpec{HedgeSpec::Kind::DeltaToZero, HedgeSpec::Cadence::Daily, band});

  // Off (default) vs on (omega = 0, sticky-delta) vs off-explicit (the
  // full-frame off-flag pin: default-constructed PriceOptions must be
  // bit-identical to an explicitly-zeroed one, matching ZeroFrictionIdentity's
  // pattern one level down the config tree).
  DeclarativeStrategy strat_off{spec};
  auto res_off = run_backtest(*clock, strat_off);
  ASSERT_TRUE(res_off.has_value()) << res_off.error().to_string();

  DeclarativeStrategy strat_off_explicit{spec};
  RunConfig cfg_off_explicit;
  cfg_off_explicit.price.skew_adjusted_delta = false;
  cfg_off_explicit.price.sticky = StickyParams{0.0};
  auto res_off_explicit = run_backtest(*clock, strat_off_explicit, cfg_off_explicit);
  ASSERT_TRUE(res_off_explicit.has_value()) << res_off_explicit.error().to_string();

  DeclarativeStrategy strat_on{spec};
  RunConfig cfg_on;
  cfg_on.price.skew_adjusted_delta = true;
  cfg_on.price.sticky = StickyParams{0.0};
  auto res_on = run_backtest(*clock, strat_on, cfg_on);
  ASSERT_TRUE(res_on.has_value()) << res_on.error().to_string();

  const BacktestResult& roff = *res_off;
  const BacktestResult& rpin = *res_off_explicit;
  const BacktestResult& ron = *res_on;
  ASSERT_EQ(roff.size(), 2u);
  ASSERT_EQ(ron.size(), roff.size());
  ASSERT_EQ(rpin.size(), roff.size());

  // Full-frame off-flag pin: EVERY BacktestResult numeric column, bit-for-bit.
  const std::vector<std::pair<const std::vector<double>*, const std::vector<double>*>> cols = {
      {&roff.pnl_total, &rpin.pnl_total},         {&roff.pnl_delta, &rpin.pnl_delta},
      {&roff.pnl_gamma, &rpin.pnl_gamma},         {&roff.pnl_vega, &rpin.pnl_vega},
      {&roff.pnl_vanna, &rpin.pnl_vanna},         {&roff.pnl_volga, &rpin.pnl_volga},
      {&roff.pnl_theta, &rpin.pnl_theta},         {&roff.pnl_rho, &rpin.pnl_rho},
      {&roff.pnl_charm, &rpin.pnl_charm},         {&roff.pnl_unexplained, &rpin.pnl_unexplained},
      {&roff.pnl_settlement, &rpin.pnl_settlement}, {&roff.pnl_shares, &rpin.pnl_shares},
      {&roff.financing, &rpin.financing},         {&roff.cost, &rpin.cost},
      {&roff.nav, &rpin.nav},                     {&roff.cash, &rpin.cash},
      {&roff.gross_delta, &rpin.gross_delta},     {&roff.gross_gamma, &rpin.gross_gamma},
      {&roff.gross_vega, &rpin.gross_vega},       {&roff.gross_theta, &rpin.gross_theta},
      {&roff.turnover_notional, &rpin.turnover_notional},
      {&roff.turnover_vega, &rpin.turnover_vega}, {&roff.n_open_lots, &rpin.n_open_lots},
      {&roff.n_unpriced_lots, &rpin.n_unpriced_lots},
      {&roff.n_unpriced_greeks, &rpin.n_unpriced_greeks}};
  for (std::size_t i = 0; i < roff.size(); ++i) {
    for (const auto& [va, vb] : cols) {
      ASSERT_EQ(va->size(), vb->size());
      EXPECT_TRUE(bits_equal((*va)[i], (*vb)[i])) << "col mismatch at row " << i;
    }
  }

  // Sign law: reconstruct the hedge SHARE position at each row from the
  // reported net book delta (gross_delta) minus the option delta the run's
  // own convention drove the hedge to (raw for the off runs; the independent
  // adjusted-delta oracle for the on run) -- gross_delta bands to ~0 against
  // WHICHEVER convention was active, so subtracting the WRONG convention's
  // delta would silently cancel the very difference this test exists to
  // detect. Put skew (dSigma/dk < 0) + omega = 0 raises the adjusted call
  // delta above raw (the 07-11 sprint's sign law), so the "on" run must hold
  // a LARGER short-share hedge (more negative) than "off" every row.
  auto snap0 = MarketSnapshot::load(c.dp[0].second);
  ASSERT_TRUE(snap0.has_value()) << snap0.error().to_string();
  const double K = snap0->find(kUid)->forward_at(target_T);
  const std::int64_t expiry = snap0->ts_ns() + std::llround(target_T * kNsPerYear);

  double worst_gap = 0.0;
  for (std::size_t i = 0; i < roff.size(); ++i) {
    auto snap = MarketSnapshot::load(c.dp[i].second);
    ASSERT_TRUE(snap.has_value()) << snap.error().to_string();
    const PricedSurface* s = snap->find(kUid);
    ASSERT_NE(s, nullptr);
    const double T = res_T(expiry, snap->ts_ns());

    const double raw_delta_ps = lot_delta(*s, K, T, Side::Call, 1.0, 100.0);
    const double adj_delta_ps = 100.0 * adjusted_call_delta(*s, K, T, Side::Call, /*omega=*/0.0);
    ASSERT_GT(adj_delta_ps, raw_delta_ps) << "row " << i << ": adjusted call delta must exceed raw "
                                             "under put skew (sign-law precondition)";

    const double shares_off = roff.gross_delta[i] - raw_delta_ps;
    const double shares_on = ron.gross_delta[i] - adj_delta_ps;
    EXPECT_LT(shares_off, 0.0) << "row " << i << ": hedging a long call should hold short shares";
    EXPECT_LT(shares_on, shares_off) << "row " << i
                                     << ": put skew must drive a LARGER short-share hedge "
                                        "when skew_adjusted_delta is on";
    worst_gap = std::max(worst_gap, shares_off - shares_on);
  }
  EXPECT_GT(worst_gap, 0.0);
  std::printf("[btexec] skew-adjusted hedge: worst (shares_off - shares_on) gap = %.4f shares\n",
              worst_gap);
}

// ── 5. Friction monotonicity: more spread ⇒ lower nav, higher cost ──────────
TEST(BacktestExec, FrictionMonotonicity) {
  const fs::path dir = fresh_dir("monotonic");
  const Corpus c = make_corpus(dir, "SPX", 6);
  auto clock = Clock::from_manifest(c.manifest);
  ASSERT_TRUE(clock.has_value()) << clock.error().to_string();

  // A put clip opened every step ⇒ an entry (and thus friction) on every row.
  StrategySpec spec;
  spec.name = "atm-put-clip";
  LegSpec leg;
  leg.uid = kUid;
  leg.tenor.target_T = 0.25;
  leg.structure.kind = StructureSpec::Kind::Single;
  leg.structure.single_side = Side::Put;
  leg.strike = StrikeSelector{StrikeSelector::Kind::AtmForward, 0.0};
  leg.size = SizeSpec{SizeSpec::Kind::FixedContracts, 1.0, +1.0};
  spec.legs.push_back(leg);
  spec.lifecycle.entry = LifecycleSpec::Entry::EveryStep;
  spec.lifecycle.holding = LifecycleSpec::Holding::HoldToExpiry;

  const double spreads[] = {0.0, 5.0, 25.0};
  double navs[3] = {0.0, 0.0, 0.0};
  double costs[3] = {0.0, 0.0, 0.0};
  for (int j = 0; j < 3; ++j) {
    DeclarativeStrategy strat{spec};
    RunConfig cfg;
    cfg.frictions.spread_kind = FrictionModel::SpreadKind::PriceBps;
    cfg.frictions.half_spread_bps = spreads[j];
    auto res = run_backtest(*clock, strat, cfg);
    ASSERT_TRUE(res.has_value()) << res.error().to_string();
    navs[j] = res->nav.back();
    double tc = 0.0;
    for (const double x : res->cost) {
      tc += x;
    }
    costs[j] = tc;
  }

  // More spread ⇒ strictly more total cost and strictly lower final nav.
  EXPECT_EQ(costs[0], 0.0);
  EXPECT_LT(costs[0], costs[1]);
  EXPECT_LT(costs[1], costs[2]);
  EXPECT_GT(navs[0], navs[1]);
  EXPECT_GT(navs[1], navs[2]);
  std::printf("[btexec] monotonicity nav={%.4f,%.4f,%.4f} cost={%.4f,%.4f,%.4f}\n", navs[0], navs[1],
              navs[2], costs[0], costs[1], costs[2]);
}
