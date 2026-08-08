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
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <limits>
#include <memory>
#include <new>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "atx/core/error.hpp"            // atx::core::Ok
#include "atx/vol/adjusted_greeks.hpp"   // StickyParams
#include "atx/vol/american.hpp"          // al_fast_opts, AmericanMethod, AmericanGreeks
#include "atx/vol/backtest.hpp"          // Clock, run_backtest, RunConfig, FrictionModel, ...
#include "atx/vol/corpus.hpp"            // CorpusManifest, CorpusEntry, CorpusFitStatus
#include "atx/vol/detail/counters.hpp"          // counters::ledger — L1 solve-economy gate
#include "atx/vol/portfolio_pricer.hpp"  // OptionContract, kNsPerYear
#include "atx/vol/priced_surface.hpp"    // PricedSurface, PricingContext
#include "atx/vol/strategy.hpp"          // DeclarativeStrategy, StrategySpec, HedgeSpec
#include "atx/vol/surface_archive.hpp"   // write_surface_archive_v2_file, SurfaceArchiveItem
#include "atx/vol/surface_parity.hpp"    // SliceContext
#include "atx/vol/types.hpp"             // Side, Result, Status
#include "atx/vol/vol_curve.hpp"         // CurveSurface, EssviCurve
#include "atx/vol/vol_surface.hpp"       // EssviParams

#include "../src/step_mark_memo.hpp"  // detail::StepMarkMemo — L2 memo admission gate

using namespace atx::vol;
namespace fs = std::filesystem;

// B3 zero-alloc proof: a counting passthrough for the global allocator, armed only
// around the measured run_backtest calls (see HedgeLedgerAllocationIsStepInvariant).
// Replacing global operator new here affects only this test binary; the count is a
// deterministic tally of ::operator new calls while armed. Aligned/over-aligned
// allocations route to their own operators (not replaced) — irrelevant here because
// the hedge ledger/scratch are plain std::vector / std::unordered_map allocations.
namespace atx_b3_alloc {
std::atomic<std::uint64_t> g_count{0};
std::atomic<bool> g_armed{false};
}  // namespace atx_b3_alloc

void* operator new(std::size_t sz) {
  if (atx_b3_alloc::g_armed.load(std::memory_order_relaxed)) {
    atx_b3_alloc::g_count.fetch_add(1, std::memory_order_relaxed);
  }
  void* p = std::malloc(sz != 0 ? sz : 1);
  if (p == nullptr) {
    throw std::bad_alloc();
  }
  return p;
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void* operator new[](std::size_t sz) { return ::operator new(sz); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

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
    const double term_forward = fwd * std::exp((kR - 0.02) * T);
    EssviParams e{};
    e.theta = 0.04 + 0.005 * static_cast<double>(i) + vol_bump;
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
  const Status st = write_surface_archive_v2_file(path, items);
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
[[nodiscard]] double lot_delta(const SurfaceRef& surf, double K, double T, Side side, double qty,
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
[[nodiscard]] double adjusted_call_delta(const SurfaceRef& surf, double K, double T, Side side,
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
// `sign` (B1) defaults to +1.0 (long/buy) so every pre-existing call site is
// unaffected; a QuoteSide sell-side test passes -1.0.
[[nodiscard]] StrategySpec single_clip(std::uint32_t uid, double target_T, Side side,
                                       StrikeSelector strike, HedgeSpec hedge = {},
                                       double sign = +1.0) {
  StrategySpec spec;
  spec.name = "single-clip";
  LegSpec leg;
  leg.uid = uid;
  leg.tenor.target_T = target_T;
  leg.structure.kind = StructureSpec::Kind::Single;
  leg.structure.single_side = side;
  leg.strike = strike;
  leg.size = SizeSpec{SizeSpec::Kind::FixedContracts, 1.0, sign};
  spec.legs.push_back(leg);
  spec.lifecycle.entry = LifecycleSpec::Entry::EveryNDays;
  spec.lifecycle.holding = LifecycleSpec::Holding::HoldToExpiry;
  spec.lifecycle.entry_every_n = 100000;  // >> corpus ⇒ opens once, at inception
  spec.hedge = hedge;
  return spec;
}

// B1: a single-clip STRADDLE (call+put at the same strike/expiry, ONE cohort,
// TWO legs) — `SizeSpec::Kind::FixedContracts` applies `sign*value` to EVERY
// leg of the structure (strategy.cpp's `expand_and_size_leg`), so both legs
// open qty=+1 (or -1) here, the multi-leg QuoteSide crossing-fraction fixture.
[[nodiscard]] StrategySpec single_clip_straddle(std::uint32_t uid, double target_T,
                                                StrikeSelector strike, double sign = +1.0) {
  StrategySpec spec;
  spec.name = "single-clip-straddle";
  LegSpec leg;
  leg.uid = uid;
  leg.tenor.target_T = target_T;
  leg.structure.kind = StructureSpec::Kind::Straddle;
  leg.strike = strike;
  leg.size = SizeSpec{SizeSpec::Kind::FixedContracts, 1.0, sign};
  spec.legs.push_back(leg);
  spec.lifecycle.entry = LifecycleSpec::Entry::EveryNDays;
  spec.lifecycle.holding = LifecycleSpec::Holding::HoldToExpiry;
  spec.lifecycle.entry_every_n = 100000;  // >> corpus ⇒ opens once, at inception
  return spec;
}

// B1: opens ONE lot at inception (a synthetic model contract, priced straight
// off the surface — no DeclarativeStrategy resolution machinery needed) and
// closes it with NO re-entry on the very next step, isolating execute()'s
// ROLL-CLOSE loop from its entry loop.
class OpenThenCloseStrategy : public IStrategy {
 public:
  OpenThenCloseStrategy(std::uint32_t uid, double target_T, Side side, double sign)
      : uid_(uid), target_T_(target_T), side_(side), sign_(sign) {}

  Status on_step(const MarketSnapshot& base, std::size_t step_index, PortfolioState& book,
                 std::uint64_t& next_lot_id) override {
    if (step_index == 0) {
      const SurfaceRef s = base.find(uid_);
      if (s == nullptr) {
        return atx::core::Err(ErrorCode::NotFound, "OpenThenCloseStrategy: no surface");
      }
      const double K = s->forward_at(target_T_);
      const auto fv = s->fair_value(K, target_T_, side_);
      if (!fv.has_value()) {
        return atx::core::Err(fv.error());
      }
      Lot lot;
      lot.id = next_lot_id++;
      lot.contract = OptionContract{uid_, K, target_T_, side_};
      lot.qty = sign_;
      lot.multiplier = 100.0;
      // Well beyond this fixture's tiny corpus, so the lot never settles —
      // the only close this test exercises is the strategy-driven one below.
      lot.expiry_ts_ns = base.ts_ns() + std::llround(target_T_ * kNsPerYear) + 30 * kDayNs;
      lot.cohort = 1;
      lot.entry_price = *fv;
      book.lots.push_back(lot);
    } else {
      book.lots.clear();  // roll-close: engine diffs before_lots vs. the now-empty book
    }
    return atx::core::Ok();
  }

 private:
  std::uint32_t uid_;
  double target_T_;
  Side side_;
  double sign_;
};

// B1: fixture for the QuoteSide friction-model tests. A tiny 2-date
// single-underlying corpus (`make_corpus`'s pattern) plus a `RunConfig` whose
// `quote_lookup` returns the SAME fixed (bid, ask) for every contract.
//
// `BacktestResult` carries no per-lot fill-price column, so `entry_fill_price`
// recovers the engine's actual fill from the inception cash ledger instead: an
// entry's net cash effect collapses to exactly `-Σ qty*multiplier*fill` (see
// backtest.cpp's entry loop — `cash -= qty*mult*model_mark` then
// `cash -= ex.cost`, and `ex.cost`'s `fill_slippage` term is
// `qty*mult*(fill-model_mark)`, so the `model_mark` terms cancel) whenever
// `per_contract_cost == 0` (this fixture's default) and every opened leg fills
// at the SAME price (true here: the lookup ignores its argument). `n_legs`
// divides out a straddle's two identically-priced legs.
class ExecFixture {
 public:
  [[nodiscard]] static ExecFixture listed_quotes(double bid, double ask) {
    ExecFixture fx;
    fx.dir_ = fresh_dir("quoteside");
    fx.corpus_ = make_corpus(fx.dir_, "SPX", 2);
    fx.bid_ = bid;
    fx.ask_ = ask;
    return fx;
  }

  [[nodiscard]] const Corpus& corpus() const noexcept { return corpus_; }

  [[nodiscard]] RunConfig config() const {
    RunConfig cfg;
    cfg.frictions.quote_lookup = [bid = bid_, ask = ask_](const OptionContract&)
        -> std::optional<FrictionModel::RawQuote> {
      return FrictionModel::RawQuote{bid, ask};
    };
    return cfg;
  }

  [[nodiscard]] static double entry_fill_price(const BacktestResult& r, int n_legs = 1) {
    return -r.cash.front() / (static_cast<double>(n_legs) * 100.0);
  }

 private:
  fs::path dir_;
  Corpus corpus_;
  double bid_{0.0};
  double ask_{0.0};
};

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

TEST(BacktestExec, OpeningCostReducesInceptionNavAndReturn) {
  const fs::path dir = fresh_dir("opening-cost");
  const Corpus c = make_corpus(dir, "SPX", 3);
  auto clock = Clock::from_manifest(c.manifest);
  ASSERT_TRUE(clock.has_value()) << clock.error().to_string();

  const StrategySpec spec =
      single_clip(kUid, 0.5, Side::Put,
                  StrikeSelector{StrikeSelector::Kind::AtmForward, 0.0});
  DeclarativeStrategy strat{spec};
  RunConfig cfg;
  cfg.frictions.per_contract_cost = 2.5;

  auto result = run_backtest(*clock, strat, cfg);
  ASSERT_TRUE(result.has_value()) << result.error().to_string();
  const BacktestResult &r = *result;
  ASSERT_EQ(r.size(), 3u);
  ASSERT_EQ(r.cost.front(), 2.5);
  EXPECT_EQ(r.pnl_total.front(), -r.cost.front());
  EXPECT_EQ(r.nav.front(), -r.cost.front());

  double summed_pnl = 0.0;
  for (const double pnl : r.pnl_total) {
    summed_pnl += pnl;
  }
  EXPECT_EQ(r.nav.back(), summed_pnl);
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
    const SurfaceRef s = snap->find(kUid);
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
      const SurfaceRef sp = snap_prev->find(kUid);
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

TEST(BacktestExec, ShareCarryAndCashFinancingComposeWithoutDoubleFunding) {
  const fs::path dir = fresh_dir("share-carry-composition");
  const Corpus c = make_corpus(dir, "SPX", 3, 100.0, 0.0, 0.0);
  auto clock = Clock::from_manifest(c.manifest);
  ASSERT_TRUE(clock.has_value()) << clock.error().to_string();

  constexpr double target_T = 0.5;
  constexpr double q_eff = 0.02;
  const StrategySpec spec = single_clip(
      kUid, target_T, Side::Call, StrikeSelector{StrikeSelector::Kind::AtmForward, 0.0},
      HedgeSpec{HedgeSpec::Kind::DeltaToZero, HedgeSpec::Cadence::Daily, 1e-6});

  auto snap0 = MarketSnapshot::load(c.dp.front().second);
  ASSERT_TRUE(snap0.has_value()) << snap0.error().to_string();
  const SurfaceRef surface0 = snap0->find(kUid);
  ASSERT_NE(surface0, nullptr);
  const double K = surface0->forward_at(target_T);
  const std::int64_t expiry = snap0->ts_ns() + std::llround(target_T * kNsPerYear);

  for (const bool finance_premium : {false, true}) {
    for (const bool shares_carry : {false, true}) {
      DeclarativeStrategy strat{spec};
      RunConfig cfg;
      cfg.financing.finance_premium = finance_premium;
      cfg.financing.shares_carry = shares_carry;
      cfg.financing.initial_cash = 1'000'000.0;

      auto result = run_backtest(*clock, strat, cfg);
      ASSERT_TRUE(result.has_value()) << result.error().to_string();
      const BacktestResult &r = *result;
      ASSERT_EQ(r.size(), c.dp.size());

      for (std::size_t i = 1; i < r.size(); ++i) {
        auto previous = MarketSnapshot::load(c.dp[i - 1].second);
        ASSERT_TRUE(previous.has_value()) << previous.error().to_string();
        const SurfaceRef surface = previous->find(kUid);
        ASSERT_NE(surface, nullptr);
        const double T = res_T(expiry, previous->ts_ns());
        const double option_delta = lot_delta(*surface, K, T, Side::Call, 1.0, 100.0);
        const double shares = r.gross_delta[i - 1] - option_delta;
        const double dt = static_cast<double>(r.ts_ns[i] - r.ts_ns[i - 1]) / kNsPerYear;
        const double spot = surface->pricing().S;

        double expected = 0.0;
        if (finance_premium) {
          expected += r.cash[i - 1] * (std::exp(kR * dt) - 1.0);
        }
        if (shares_carry) {
          const double carry_rate = finance_premium ? q_eff : q_eff - kR;
          expected += shares * carry_rate * spot * dt;
        }
        EXPECT_NEAR(r.financing[i], expected, 1e-8 * (std::fabs(expected) + 1.0))
            << "finance_premium=" << finance_premium << " shares_carry=" << shares_carry
            << " row=" << i;
      }
    }
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
    const SurfaceRef s = snap->find(kUid);
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

// ── B3. Daily delta-hedge + O(1) share ledger is allocation-free in steady state ──
//
// The hedge overlay's HEAP footprint must not grow with the step count: after
// warm-up (every hedged uid resident in the ledger) the pass reuses its per-uid
// delta aggregate, uid-order, and dedup scratch and touches the O(1) share index,
// so it allocates nothing per step. Proof by isolation: hedge-on vs hedge-off runs
// are identical except for the hedge (same entries, same book growth, same output,
// same snapshot loads), so (armed alloc count)_on - _off is exactly the hedge's
// heap cost. We measure it at D and 2D dates and assert it does NOT grow with the
// step count. Pre-B3 the overlay heap-allocated a fresh `uids` vector on EVERY
// hedge step, so the isolated count grew with D; this test is that regression's gate.
//
// n_threads=1 + prefetch off ⇒ the whole run is single-threaded, so the armed
// global-new tally is deterministic (no worker-pool / async-prefetch allocations).
TEST(BacktestExec, HedgeLedgerAllocationIsStepInvariant) {
  const fs::path dir = fresh_dir("allocsteady");

  // A daily ATM-put clip, held to expiry (cohorts accumulate), hedged daily to a
  // tight band so a trade fires every step over the drifting spot.
  const auto make_daily_hedged = [](bool hedge_on) {
    StrategySpec spec;
    spec.name = "daily-put-htx";
    LegSpec leg;
    leg.uid = kUid;
    leg.tenor.target_T = 0.25;  // 3M: no expiries over the short run (book only grows)
    leg.structure.kind = StructureSpec::Kind::Single;
    leg.structure.single_side = Side::Put;
    leg.strike = StrikeSelector{StrikeSelector::Kind::AtmForward, 0.0};
    leg.size = SizeSpec{SizeSpec::Kind::FixedContracts, 1.0, +1.0};
    spec.legs.push_back(leg);
    spec.lifecycle.entry = LifecycleSpec::Entry::EveryStep;
    spec.lifecycle.holding = LifecycleSpec::Holding::HoldToExpiry;
    if (hedge_on) {
      spec.hedge = HedgeSpec{HedgeSpec::Kind::DeltaToZero, HedgeSpec::Cadence::Daily, 1e-9};
    }
    return spec;
  };

  // Single-threaded, no async prefetch ⇒ the armed tally counts only this thread.
  const auto run_counted = [&](const StrategySpec& spec, int n_dates) -> std::uint64_t {
    const Corpus c = make_corpus(dir, "SPX", n_dates, 100.0, 0.006, 0.001);
    auto clock = Clock::from_manifest(c.manifest);
    EXPECT_TRUE(clock.has_value()) << (clock.has_value() ? std::string{} : clock.error().to_string());
    DeclarativeStrategy strat{spec};
    RunConfig cfg;
    cfg.price.n_threads = 1;
    cfg.prefetch_snapshots = false;
    atx_b3_alloc::g_count.store(0, std::memory_order_relaxed);
    atx_b3_alloc::g_armed.store(true, std::memory_order_relaxed);
    auto res = run_backtest(*clock, strat, cfg);
    atx_b3_alloc::g_armed.store(false, std::memory_order_relaxed);
    EXPECT_TRUE(res.has_value()) << (res.has_value() ? std::string{} : res.error().to_string());
    return atx_b3_alloc::g_count.load(std::memory_order_relaxed);
  };

  constexpr int kD = 8;
  const StrategySpec on = make_daily_hedged(true);
  const StrategySpec off = make_daily_hedged(false);

  const std::uint64_t on_d = run_counted(on, kD);
  const std::uint64_t off_d = run_counted(off, kD);
  const std::uint64_t on_2d = run_counted(on, 2 * kD);
  const std::uint64_t off_2d = run_counted(off, 2 * kD);

  ASSERT_GE(on_d, off_d);
  ASSERT_GE(on_2d, off_2d);
  const std::uint64_t hedge_extra_d = on_d - off_d;
  const std::uint64_t hedge_extra_2d = on_2d - off_2d;

  // The hedge/ledger heap cost is one-time (ledger + scratch grow to the resident
  // uid set during warm-up) and does NOT grow with the step count: doubling the
  // dates does not increase the hedge-attributable allocation count.
  EXPECT_LE(hedge_extra_2d, hedge_extra_d)
      << "hedge allocation grew with step count — a per-step heap allocation regressed the "
         "zero-alloc steady state (hedge_extra_d=" << hedge_extra_d
      << " hedge_extra_2d=" << hedge_extra_2d << ")";
  std::printf("[btexec] B3 zero-alloc: hedge-attributable allocs D=%llu 2D=%llu (on_d=%llu off_d=%llu "
              "on_2d=%llu off_2d=%llu)\n",
              static_cast<unsigned long long>(hedge_extra_d),
              static_cast<unsigned long long>(hedge_extra_2d),
              static_cast<unsigned long long>(on_d), static_cast<unsigned long long>(off_d),
              static_cast<unsigned long long>(on_2d), static_cast<unsigned long long>(off_2d));
}

// ── B4. Held-to-expiry daily overlapping cohorts compose + settle at scale ───────
//
// EveryStep entry + HoldToExpiry over kNames 40Δ strangles builds many overlapping
// daily cohorts; a ~20-clock-day tenor (target_T = 20*kDayNs/kNsPerYear, which
// rounds to exactly 20 clock-days so expiries land ON clock dates) makes cohorts
// reach expiry MID-RUN — exercising the expiry sweep + engine settlement (B2
// batched marks) at scale, not just accumulation. Verifies the composition is
// correct (overlapping cohorts accumulate, cohorts settle, the book never empties)
// and bit-identical across thread counts (determinism at scale).
TEST(BacktestExec, HeldToExpiryDailyCohortsComposeAtScale) {
  const fs::path dir = fresh_dir("b4-cohorts");
  constexpr int kNames = 4;
  constexpr int kDates = 24;
  static const char* kSyms[] = {"AAA", "BBB", "CCC", "DDD"};

  std::vector<std::pair<std::string, std::string>> dp;
  for (int d = 0; d < kDates; ++d) {
    const std::int64_t now = kBaseNow + static_cast<std::int64_t>(d) * kDayNs;
    std::vector<PricedSurface> surfaces;
    surfaces.reserve(kNames);
    for (int u = 0; u < kNames; ++u) {
      const double S = (100.0 + 10.0 * static_cast<double>(u)) * (1.0 + 0.003 * static_cast<double>(d));
      surfaces.push_back(make_surface(kUid + static_cast<std::uint32_t>(u), S, S, now,
                                      0.001 * static_cast<double>(d) + 0.002 * static_cast<double>(u)));
    }
    std::vector<SurfaceArchiveItem> items;
    for (int u = 0; u < kNames; ++u) {
      items.push_back(SurfaceArchiveItem{kSyms[u], &surfaces[u]});
    }
    std::error_code ec;
    fs::create_directories(dir, ec);
    char buf[16];
    std::snprintf(buf, sizeof buf, "2026-08-%02d", d + 1);
    const std::string path = (dir / (std::string(buf) + ".atxvsa")).string();
    ASSERT_TRUE(write_surface_archive_v2_file(path, items).has_value());
    dp.emplace_back(buf, path);
  }
  auto clock = Clock::from_manifest(make_manifest(dp, "AAA"));
  ASSERT_TRUE(clock.has_value()) << clock.error().to_string();

  const double tenor_T = static_cast<double>(20 * kDayNs) / kNsPerYear; // -> exactly 20 clock-days
  StrategySpec spec;
  spec.name = "b4-daily-strangle-htx";
  for (int u = 0; u < kNames; ++u) {
    LegSpec leg;
    leg.uid = kUid + static_cast<std::uint32_t>(u);
    leg.tenor.target_T = tenor_T;
    leg.tenor.snap_to_listed = false;
    leg.structure.kind = StructureSpec::Kind::Strangle;
    leg.structure.call_leg = StrikeSelector{StrikeSelector::Kind::Delta, 0.40};
    leg.structure.put_leg = StrikeSelector{StrikeSelector::Kind::Delta, 0.40};
    leg.size = SizeSpec{SizeSpec::Kind::FixedContracts, 1.0, +1.0};
    spec.legs.push_back(leg);
  }
  spec.lifecycle.entry = LifecycleSpec::Entry::EveryStep;
  spec.lifecycle.holding = LifecycleSpec::Holding::HoldToExpiry;

  const auto run = [&](unsigned n_threads) {
    DeclarativeStrategy strat{spec};
    RunConfig cfg;
    cfg.price.n_threads = n_threads;
    return run_backtest(*clock, strat, cfg);
  };

  auto r1 = run(1);
  ASSERT_TRUE(r1.has_value()) << r1.error().to_string();
  ASSERT_EQ(r1->size(), static_cast<std::size_t>(kDates));

  double max_lots = 0.0;
  int settle_rows = 0;
  for (std::size_t i = 0; i < r1->size(); ++i) {
    max_lots = std::max(max_lots, r1->n_open_lots[i]);
    if (std::fabs(r1->pnl_settlement[i]) > 0.0) {
      ++settle_rows;
    }
    EXPECT_GT(r1->n_open_lots[i], 0.0) << "book emptied at row " << i;
  }
  // Overlapping cohorts accumulate to many legs; cohorts reach expiry mid-run.
  EXPECT_GT(max_lots, static_cast<double>(kNames * 2 * 10)) << "cohorts did not overlap at scale";
  EXPECT_GT(settle_rows, 0) << "no cohort reached expiry/settlement within the run";

  // Determinism at scale: bit-identical across thread counts.
  auto r4 = run(4);
  ASSERT_TRUE(r4.has_value()) << r4.error().to_string();
  ASSERT_EQ(r1->size(), r4->size());
  for (std::size_t i = 0; i < r1->size(); ++i) {
    EXPECT_TRUE(bits_equal(r1->nav[i], r4->nav[i])) << "nav row " << i;
    EXPECT_TRUE(bits_equal(r1->pnl_total[i], r4->pnl_total[i])) << "pnl_total row " << i;
    EXPECT_TRUE(bits_equal(r1->pnl_settlement[i], r4->pnl_settlement[i])) << "settle row " << i;
    EXPECT_TRUE(bits_equal(r1->gross_vega[i], r4->gross_vega[i])) << "gross_vega row " << i;
    EXPECT_EQ(r1->n_open_lots[i], r4->n_open_lots[i]) << "n_open_lots row " << i;
  }
  std::printf("[btexec] B4 cohorts: %d names, %d dates, max_open_lots=%.0f, settle_rows=%d, "
              "det(1 vs 4 threads)=OK\n",
              kNames, kDates, max_lots, settle_rows);
}

// ── B5a. Strategy-overload hedge + cohort determinism (B3 + B4) ───────────────
//
// EXACT coverage: the STRATEGY overload on the M2 universe shape (kNames 40Δ
// strangles, EveryStep entry, HoldToExpiry, DeltaToZero DAILY hedge). This exercises
// B3 (the O(1) daily delta-hedge ledger, band 0.0 so it fires every step) and B4
// (daily overlapping cohort accumulation). It does NOT exercise B1 subset-deser (the
// strategy overload's private cache carries no referenced_uids => whole-board load)
// nor B2 batched settlement (target_T=0.25 over kDates => no mid-run expiries,
// pnl_settlement == 0 every row); those composed paths are gated by
// FixedBookComposedSubsetAndSettlement_Deterministic (B1+B2) below and by B4's
// HeldToExpiryDailyCohortsComposeAtScale (settlement at scale). Asserts:
//   (a) bit-identical across two runs (determinism), and
//   (b) bit-identical across thread counts (thread-invariant reductions).
TEST(BacktestExec, StrategyLoopHedgeAndCohorts_Deterministic) {
  const fs::path dir = fresh_dir("b5-universe");
  constexpr int kNames = 4;
  constexpr int kDates = 12;
  static const char* kSyms[] = {"AAA", "BBB", "CCC", "DDD"};

  std::vector<std::pair<std::string, std::string>> dp;
  for (int d = 0; d < kDates; ++d) {
    const std::int64_t now = kBaseNow + static_cast<std::int64_t>(d) * kDayNs;
    std::vector<PricedSurface> surfaces;
    surfaces.reserve(kNames);
    for (int u = 0; u < kNames; ++u) {
      const double S =
          (100.0 + 10.0 * static_cast<double>(u)) * (1.0 + 0.004 * static_cast<double>(d));
      surfaces.push_back(make_surface(kUid + static_cast<std::uint32_t>(u), S, S, now,
                                      0.001 * static_cast<double>(d) + 0.002 * static_cast<double>(u)));
    }
    std::vector<SurfaceArchiveItem> items;
    for (int u = 0; u < kNames; ++u) {
      items.push_back(SurfaceArchiveItem{kSyms[u], &surfaces[u]});
    }
    std::error_code ec;
    fs::create_directories(dir, ec);
    char buf[16];
    std::snprintf(buf, sizeof buf, "2026-08-%02d", d + 1);
    const std::string path = (dir / (std::string(buf) + ".atxvsa")).string();
    ASSERT_TRUE(write_surface_archive_v2_file(path, items).has_value());
    dp.emplace_back(buf, path);
  }
  auto clock = Clock::from_manifest(make_manifest(dp, "AAA"));
  ASSERT_TRUE(clock.has_value()) << clock.error().to_string();

  StrategySpec spec;
  spec.name = "b5-universe-strangle-hedged";
  for (int u = 0; u < kNames; ++u) {
    LegSpec leg;
    leg.uid = kUid + static_cast<std::uint32_t>(u);
    leg.tenor.target_T = 0.25; // 3M, in-grid across the run
    leg.tenor.snap_to_listed = false;
    leg.structure.kind = StructureSpec::Kind::Strangle;
    leg.structure.call_leg = StrikeSelector{StrikeSelector::Kind::Delta, 0.40};
    leg.structure.put_leg = StrikeSelector{StrikeSelector::Kind::Delta, 0.40};
    leg.size = SizeSpec{SizeSpec::Kind::FixedContracts, 1.0, +1.0};
    spec.legs.push_back(leg);
  }
  spec.lifecycle.entry = LifecycleSpec::Entry::EveryStep;
  spec.lifecycle.holding = LifecycleSpec::Holding::HoldToExpiry;
  spec.hedge = HedgeSpec{HedgeSpec::Kind::DeltaToZero, HedgeSpec::Cadence::Daily, 0.0};

  const auto run = [&](unsigned n_threads) {
    DeclarativeStrategy strat{spec};
    RunConfig cfg;
    cfg.price.n_threads = n_threads;
    return run_backtest(*clock, strat, cfg);
  };

  auto a = run(1);
  auto b = run(1); // same config, second run — determinism
  auto c = run(4); // different thread count — thread-invariance
  ASSERT_TRUE(a.has_value()) << a.error().to_string();
  ASSERT_TRUE(b.has_value()) << b.error().to_string();
  ASSERT_TRUE(c.has_value()) << c.error().to_string();
  ASSERT_EQ(a->size(), static_cast<std::size_t>(kDates));

  const auto all_cols = [](const BacktestResult& r, std::size_t i) {
    return std::array<double, 8>{r.nav[i],        r.pnl_total[i], r.pnl_vega[i],  r.pnl_settlement[i],
                                 r.pnl_shares[i],  r.cost[i],      r.cash[i],      r.gross_delta[i]};
  };
  for (std::size_t i = 0; i < a->size(); ++i) {
    const auto ca = all_cols(*a, i);
    const auto cb = all_cols(*b, i);
    const auto cc = all_cols(*c, i);
    for (std::size_t k = 0; k < ca.size(); ++k) {
      EXPECT_TRUE(bits_equal(ca[k], cb[k])) << "two-run determinism col " << k << " row " << i;
      EXPECT_TRUE(bits_equal(ca[k], cc[k])) << "thread-invariance col " << k << " row " << i;
    }
  }
  std::printf("[btexec] B5a strategy hedge+cohorts (B3+B4): %d names x %d dates hedged; bit-identical "
              "over 2 runs and 1-vs-4 threads\n",
              kNames, kDates);
}

// ── B5b. Fixed-book composed subset-deser + batched settlement determinism (B1 + B2) ─
//
// The strategy gate above cannot exercise B1 (strategy overload = whole board) nor
// B2 (no expiries), so this companion gates the composed FIXED-BOOK path where BOTH
// run. The archive holds 4 names; the fixed book references only 2 (book ⊂ archive),
// so the fixed-book overload's private cache subset-deserializes. To PROVE the
// subset path is load-bearing (not merely present), the 2 UNREFERENCED surfaces are
// written with a MISMATCHED now_ts_ns: a whole-board load then fails the
// surfaces-agree-on-ts check (asserted as a positive control), while the subset load
// of the 2 referenced (matching-ts) names succeeds — so a run that succeeds MUST have
// taken the subset path. One book lot expires exactly on a mid-window clock date, so
// B2 batched settlement fires (asserted: exactly one settling row). Then 2-run +
// 1-vs-4-thread bit-identity over the composed run.
TEST(BacktestExec, FixedBookComposedSubsetAndSettlement_Deterministic) {
  const fs::path dir = fresh_dir("b5-composed");
  constexpr int kDates = 6;
  const std::int64_t kExpiry = kBaseNow + 3 * kDayNs; // settles at date index 3
  static const char* kSyms[] = {"AAA", "BBB", "CCC", "DDD"};
  const std::uint32_t ref_uids[] = {kUid, kUid + 2}; // referenced (AAA, CCC)

  std::vector<std::pair<std::string, std::string>> dp;
  for (int d = 0; d < kDates; ++d) {
    const std::int64_t now = kBaseNow + static_cast<std::int64_t>(d) * kDayNs;
    std::vector<PricedSurface> surfaces;
    surfaces.reserve(4);
    for (int u = 0; u < 4; ++u) {
      const bool referenced = (u == 0 || u == 2);
      // Unreferenced surfaces carry a DIFFERENT now_ts so a WHOLE-board load fails.
      const std::int64_t ts = referenced ? now : now + 1000000;
      const double S = (100.0 + 10.0 * static_cast<double>(u)) * (1.0 + 0.003 * static_cast<double>(d));
      surfaces.push_back(make_surface(kUid + static_cast<std::uint32_t>(u), S, S, ts,
                                      0.001 * static_cast<double>(d) + 0.002 * static_cast<double>(u)));
    }
    std::vector<SurfaceArchiveItem> items;
    for (int u = 0; u < 4; ++u) {
      items.push_back(SurfaceArchiveItem{kSyms[u], &surfaces[u]});
    }
    std::error_code ec;
    fs::create_directories(dir, ec);
    char buf[16];
    std::snprintf(buf, sizeof buf, "2026-08-%02d", d + 1);
    const std::string path = (dir / (std::string(buf) + ".atxvsa")).string();
    ASSERT_TRUE(write_surface_archive_v2_file(path, items).has_value());
    dp.emplace_back(buf, path);
  }
  auto clock = Clock::from_manifest(make_manifest(dp, "AAA"));
  ASSERT_TRUE(clock.has_value()) << clock.error().to_string();

  // Positive control: a WHOLE-BOARD load fails (mixed now_ts across the 4 surfaces).
  auto whole = MarketSnapshot::load(dp.front().second);
  EXPECT_FALSE(whole.has_value()) << "whole-board load should fail on mismatched now_ts_ns — the "
                                     "subset path is what makes the run below succeed";
  // The subset load of the 2 referenced (matching-ts) names succeeds with 2 surfaces.
  auto sub = MarketSnapshot::load(dp.front().second, QueryPricingTier::LegacyCompatible,
                                  std::span<const std::uint32_t>{ref_uids});
  ASSERT_TRUE(sub.has_value()) << sub.error().to_string();
  EXPECT_EQ(sub->n_surfaces(), 2u);

  const auto make_book = [&]() {
    PortfolioState b;
    // kUid lot expires mid-window (settles at date 3); kUid+2 lot held past the window.
    b.lots.push_back(
        Lot{1, OptionContract{kUid, 100.0, 0.0, Side::Call}, +2.0, 100.0, kExpiry, 0, 0.0});
    b.lots.push_back(Lot{2, OptionContract{kUid + 2, 120.0, 0.0, Side::Put}, -1.0, 100.0,
                         kBaseNow + 120 * kDayNs, 0, 0.0});
    return b;
  };
  const auto run = [&](unsigned n_threads) {
    RunConfig cfg;
    cfg.price.n_threads = n_threads; // default (null) cache => private subset cache (B1)
    return run_backtest(*clock, make_book(), cfg);
  };

  auto a = run(1);
  ASSERT_TRUE(a.has_value()) << (a.has_value() ? std::string{} : a.error().to_string())
                             << " (the fixed-book subset run must succeed even though the "
                                "whole-board load fails — proof the B1 subset path ran)";
  // B2 batched settlement ran: exactly one lot expired mid-window and settled.
  int settle_rows = 0;
  for (std::size_t i = 0; i < a->size(); ++i) {
    if (std::fabs(a->pnl_settlement[i]) > 0.0) {
      ++settle_rows;
    }
  }
  EXPECT_EQ(settle_rows, 1) << "B2 batched settlement did not fire in the composed run";

  auto b = run(1); // determinism (second run)
  auto c = run(4); // thread-invariance
  ASSERT_TRUE(b.has_value()) << b.error().to_string();
  ASSERT_TRUE(c.has_value()) << c.error().to_string();
  ASSERT_EQ(a->size(), b->size());
  ASSERT_EQ(a->size(), c->size());
  const auto cols = [](const BacktestResult& r, std::size_t i) {
    return std::array<double, 6>{r.nav[i],   r.pnl_total[i],  r.pnl_settlement[i],
                                 r.pnl_delta[i], r.gross_vega[i], r.gross_delta[i]};
  };
  for (std::size_t i = 0; i < a->size(); ++i) {
    const auto ca = cols(*a, i);
    const auto cb = cols(*b, i);
    const auto cc = cols(*c, i);
    for (std::size_t k = 0; k < ca.size(); ++k) {
      EXPECT_TRUE(bits_equal(ca[k], cb[k])) << "two-run determinism col " << k << " row " << i;
      EXPECT_TRUE(bits_equal(ca[k], cc[k])) << "thread-invariance col " << k << " row " << i;
    }
  }
  std::printf("[btexec] B5b composed fixed-book (B1 subset + B2 settle): whole-board load fails, "
              "subset run succeeds, settle_rows=%d, bit-identical over 2 runs and 1-vs-4 threads\n",
              settle_rows);
}

// ─────────────────────────────────────────────────────────────────────────────
// L1 (AL-solve-wall sprint, fewer-solves): base-risk stamp survives a membership
// change. See portfolio_pricer.cpp PortfolioPricer::carry_base_risk_subset and
// backtest.cpp RetainedBookPricer::prepare. The solve-economy gate below is the
// POST-L1 counterpart of solve_ledger_test's ExpiryDayReSolvesPnlBaseStampEleven-
// PerUnit (which pins the PRE-L1 11 s/u expiry-day cost): after L1 the survivor's
// expiry-day pnl-base bundle is REUSED across the settlement shrink, dropping the
// A-attributable expiry-day cost 11 -> 6 (== the no-churn day).
// ─────────────────────────────────────────────────────────────────────────────
namespace led = atx::vol::counters::ledger;

namespace {

// A clock at explicit day offsets from kBaseNow (mirrors solve_ledger_test's
// make_clock): a settling lot's expiry_ts_ns must equal kBaseNow + offset*kDayNs
// exactly. Spot/vol drift each date so marks actually move.
[[nodiscard]] Clock offset_clock(const fs::path& dir, const std::string& symbol,
                                 const std::vector<int>& offset_days) {
  std::vector<std::pair<std::string, std::string>> dp;
  for (std::size_t d = 0; d < offset_days.size(); ++d) {
    const std::int64_t now = kBaseNow + static_cast<std::int64_t>(offset_days[d]) * kDayNs;
    const double S = 100.0 * (1.0 + 0.004 * static_cast<double>(d));
    const double vb = 0.001 * static_cast<double>(d);
    const PricedSurface s = make_surface(kUid, S, S, now, vb);
    char buf[16];
    std::snprintf(buf, sizeof buf, "2026-08-%02d", static_cast<int>(d) + 1);
    dp.emplace_back(buf, write_one(dir, buf, symbol, s));
  }
  auto clock = Clock::from_manifest(make_manifest(dp, symbol));
  EXPECT_TRUE(clock.has_value()) << (clock.has_value() ? std::string{} : clock.error().to_string());
  return std::move(*clock);
}

[[nodiscard]] std::uint64_t al(const led::Counts& c) noexcept {
  return c.get(led::Solve::AlBoundarySolves);
}
[[nodiscard]] std::uint64_t analytic(const led::Counts& c) noexcept {
  return c.get(led::Solve::GreeksBundlesAnalytic);
}

[[nodiscard]] RunConfig det_config() {
  RunConfig cfg;
  cfg.price.n_threads = 1u;         // boundary-solve counts are thread-invariant
  cfg.prefetch_snapshots = false;   // remove async-prefetch nondeterminism
  return cfg;
}

// Trades nothing and names a uid the corpus does not hold, so the engine's private
// snapshot cache subsets on it and EVERY date loads through the subset-miss path
// into a legal zero-surface snapshot.
class AbsentNameStrategy final : public IStrategy {
 public:
  explicit AbsentNameStrategy(std::uint32_t uid) noexcept : uids_{uid} {}
  Status on_step(const MarketSnapshot&, std::size_t, PortfolioState&, std::uint64_t&) override {
    return atx::core::Ok();
  }
  [[nodiscard]] std::span<const std::uint32_t> referenced_uids() const noexcept override {
    return uids_;
  }

 private:
  std::array<std::uint32_t, 1> uids_;
};

}  // namespace

// ─────────────────────────────────────────────────────────────────────────────
// Zero-surface snapshot + finance_premium (plan 1.12).
//
// `MarketSnapshot::load`'s subset-miss path LEGALLY yields a snapshot owning no
// surfaces (a book/strategy naming only uids absent from this partition — the load
// deliberately keeps an empty SurfaceSet instead of falling back to a whole-board
// read). The cash-carry accrual then sourced its base-date rate from
// `base->surface_at(0)`, an unbounded index into an empty backing, and the null
// `SurfaceRef` that produced dereferenced null in `pricing()`.
//
// There is no rate to accrue at on such a date, so the engine follows the step
// loop's own missing-data convention: a FLAT balance carries no economics (the
// hedge-share ledger's `n != 0.0` rule), a LIVE one is a valuation failure that
// must fail closed rather than silently drop a step of financing out of NAV.
// ─────────────────────────────────────────────────────────────────────────────
TEST(BacktestExec, ZeroSurfaceSnapshotWithLiveCashAndFinancePremiumFailsClosed) {
  const fs::path dir = fresh_dir("zero-surface-live-cash");
  const Clock clock = offset_clock(dir, "SPX", {0, 20});
  AbsentNameStrategy strat{kUid + 1000u};  // no such uid in the archive

  RunConfig cfg;
  cfg.price.n_threads = 1u;
  cfg.prefetch_snapshots = false;
  cfg.financing.finance_premium = true;
  cfg.financing.initial_cash = 1.0e6;  // a LIVE balance: the accrual has economics

  const auto r = run_backtest(clock, strat, cfg);
  ASSERT_FALSE(r.has_value()) << "a live cash balance carried across a date with no base surface "
                                 "must fail closed, not read surface_at(0) out of bounds";
  EXPECT_EQ(r.error().code(), ErrorCode::NotFound);
}

TEST(BacktestExec, ZeroSurfaceSnapshotWithZeroCashAndFinancePremiumCarriesNothing) {
  const fs::path dir = fresh_dir("zero-surface-flat-cash");
  const Clock clock = offset_clock(dir, "SPX", {0, 20});
  AbsentNameStrategy strat{kUid + 1000u};

  RunConfig cfg;
  cfg.price.n_threads = 1u;
  cfg.prefetch_snapshots = false;
  cfg.financing.finance_premium = true;
  cfg.financing.initial_cash = 0.0;  // flat: `cash * (growth - 1)` is 0.0 at any rate

  const auto r = run_backtest(clock, strat, cfg);
  ASSERT_TRUE(r.has_value()) << r.error().to_string();
  ASSERT_EQ(r->size(), 2u);
  for (std::size_t i = 0; i < r->size(); ++i) {
    EXPECT_TRUE(bits_equal(r->financing[i], 0.0)) << "financing row " << i;
    EXPECT_TRUE(bits_equal(r->cash[i], 0.0)) << "cash row " << i;
    // `==`, not bit-equality: row 0's NAV is `-ex->cost` over a zero cost, i.e. a
    // NEGATIVE zero. The sign of zero carries no economics.
    EXPECT_EQ(r->nav[i], 0.0) << "nav row " << i;
  }
}

// ─────────────────────────────────────────────────────────────────────────────
// Non-monotone snapshot timestamps (plan 2.4).
//
// A `Clock` is ordered by DATE STRING, but every economic quantity in a step is
// driven by `dt = (shifted.ts_ns - base.ts_ns) / kNsPerYear`, and that timestamp
// comes from the ARCHIVE's `now_ts_ns` — a different thing entirely. A
// mislabelled or misordered partition therefore hands the step loop a negative
// dt, and nothing checked it: `exp(r*dt)` shrinks the cash balance instead of
// growing it, the borrow bleed becomes a rebate, and the shares-carry term flips
// sign. Every one of those is a plausible number, so the run reports a full set
// of rows and no error at all.
//
// It must fail closed with a clear error instead. Both overloads validate it:
// the fixed-book run has no financing, but it derives residual T, settlement and
// expiry drops from the same timestamps.
// ─────────────────────────────────────────────────────────────────────────────
TEST(BacktestExec, BackwardsSnapshotTimestampsFailClosed) {
  const fs::path dir = fresh_dir("backwards-ts-strategy");
  // Date strings ascend (2026-08-01, 2026-08-02); their archives' valuation
  // stamps do NOT — day 20 then day 0.
  const Clock clock = offset_clock(dir, "SPX", {20, 0});
  NoopStrategy strat;

  RunConfig cfg = det_config();
  cfg.financing.finance_premium = true;
  cfg.financing.initial_cash = 1.0e6; // a live balance: the accrual has economics

  const auto r = run_backtest(clock, strat, cfg);
  ASSERT_FALSE(r.has_value()) << "a step whose snapshot timestamps run backwards must fail "
                                 "closed, not accrue a sign-flipped carry";
  EXPECT_EQ(r.error().code(), ErrorCode::InvalidArgument);
}

TEST(BacktestExec, BackwardsSnapshotTimestampsFailClosedOnTheFixedBookRun) {
  const fs::path dir = fresh_dir("backwards-ts-fixed");
  const Clock clock = offset_clock(dir, "SPX", {20, 0});

  const auto r = run_backtest(clock, PortfolioState{}, det_config());
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error().code(), ErrorCode::InvalidArgument);
}

// Non-vacuity: the SAME fixture in the right order runs, so the rejections above
// are attributable to the ordering and not to the corpus.
TEST(BacktestExec, ForwardSnapshotTimestampsStillRun) {
  const fs::path dir = fresh_dir("forward-ts");
  const Clock clock = offset_clock(dir, "SPX", {0, 20});
  NoopStrategy strat;

  RunConfig cfg = det_config();
  cfg.financing.finance_premium = true;
  cfg.financing.initial_cash = 1.0e6;

  const auto r = run_backtest(clock, strat, cfg);
  ASSERT_TRUE(r.has_value()) << r.error().to_string();
  EXPECT_EQ(r->size(), 2u);
  EXPECT_GT(r->financing[1], 0.0) << "a forward step accrues a POSITIVE carry on a long balance";
}

// The boundary is deliberate: only a BACKWARDS step is refused. Two snapshots
// sharing a timestamp give dt == 0, and every term the step scales by dt is then
// an exact no-op (`cash * (exp(0) - 1)` is 0.0 for every finite r), so there is
// no sign to flip and nothing to protect the caller from. Rejecting it would
// turn a degenerate-but-harmless corpus into a hard failure.
TEST(BacktestExec, EqualSnapshotTimestampsAreAcceptedAndCarryNothing) {
  const fs::path dir = fresh_dir("equal-ts");
  const Clock clock = offset_clock(dir, "SPX", {0, 0});
  NoopStrategy strat;

  RunConfig cfg = det_config();
  cfg.financing.finance_premium = true;
  cfg.financing.initial_cash = 1.0e6;

  const auto r = run_backtest(clock, strat, cfg);
  ASSERT_TRUE(r.has_value()) << r.error().to_string();
  ASSERT_EQ(r->size(), 2u);
  EXPECT_TRUE(bits_equal(r->financing[1], 0.0));
}

// Gate: the survivor's expiry-day cost drops 11 -> 6 s/u; warm and no-churn steps
// are unchanged. Same fixed book as solve_ledger_test's expiry-day scenario.
TEST(BacktestExec, L1ExpiryDaySurvivorReusesBaseRiskAcrossSettlement) {
  const fs::path dir = fresh_dir("l1-expiry");
  const Clock clock = offset_clock(dir, "SPX", {0, 20, 40, 60, 80});
  const std::int64_t exp_B = kBaseNow + 60 * kDayNs;   // settles on date index 3
  const std::int64_t exp_A = kBaseNow + 200 * kDayNs;  // survives the whole run

  PortfolioState book;
  book.lots.push_back(
      Lot{1, OptionContract{kUid, 100.0, 0.0, Side::Put}, +1.0, 100.0, exp_A, 0, 0.0});
  book.lots.push_back(
      Lot{2, OptionContract{kUid, 95.0, 0.0, Side::Call}, +1.0, 100.0, exp_B, 0, 0.0});

  led::reset();
  led::StepTrace trace;
  // Isolate L1: disable the L2 settlement-mark memo so the expiry step still SOLVES
  // B's settlement mark (the [12,12,7,6] pre-L2 economy). L2's further 7->6 drop is
  // pinned by L2SettlementMarkMemoDropsExpirySolve below.
  RunConfig config = det_config();
  config.settlement_mark_memo = false;
  const auto result = run_backtest(clock, std::move(book), config);
  ASSERT_TRUE(result.has_value()) << result.error().to_string();
  ASSERT_EQ(result->size(), 5u);
  ASSERT_EQ(trace.size(), 4u);

  const led::Counts& warm1 = trace.steps()[0];
  const led::Counts& warm2 = trace.steps()[1];
  const led::Counts& expiry = trace.steps()[2];   // B settles, A survives (membership shrink)
  const led::Counts& nochurn = trace.steps()[3];  // A only, stamp intact

  // Warm two-unit steps are unchanged by L1 (no membership change): 2 book bundles
  // + 2 target marks = 12, pnl-base reused.
  EXPECT_EQ(al(warm1), 12u);
  EXPECT_EQ(analytic(warm1), 2u);
  EXPECT_EQ(al(warm2), 12u);
  EXPECT_EQ(analytic(warm2), 2u);

  // No-churn survivor step: 0 pnl-base + 1 target + 5 book bundle = 6.
  EXPECT_EQ(al(nochurn), 6u);
  EXPECT_EQ(analytic(nochurn), 1u);

  // POST-L1 expiry step: A's pnl-base bundle is now REUSED across the shrink, so the
  // step costs 1 (B settlement mark) + 1 (A target mark) + 5 (A book bundle) = 7,
  // with only ONE analytic bundle (the book-greeks pass), down from 2 pre-L1.
  EXPECT_EQ(al(expiry), 7u) << "expiry step: B settle(1) + A target(1) + A book bundle(5)";
  EXPECT_EQ(analytic(expiry), 1u) << "pnl-base re-solve removed by L1 (was 2 bundles)";

  // The A-attributable expiry-day cost (total minus B's one settlement mark) is now 6
  // == the no-churn day — the 11 -> 6 solve-economy win, and it exceeds no-churn by
  // exactly B's settlement mark, no extra pnl-base bundle.
  const std::uint64_t a_expiry_cost = al(expiry) - 1u;
  EXPECT_EQ(a_expiry_cost, 6u) << "expiry-day steady state 11 -> 6 solve-equivs/unit (L1 gate)";
  EXPECT_EQ(al(expiry) - al(nochurn), 1u) << "only B's settlement mark separates expiry from no-churn";

  std::printf("[btexec] L1 expiry-day AL solves = [%llu,%llu,%llu,%llu] "
              "(warm,warm,expiry,no-churn); A-attributable expiry cost 11 -> %llu\n",
              static_cast<unsigned long long>(al(warm1)), static_cast<unsigned long long>(al(warm2)),
              static_cast<unsigned long long>(al(expiry)),
              static_cast<unsigned long long>(al(nochurn)),
              static_cast<unsigned long long>(a_expiry_cost));
}

// Bit-identity (B3 style, by construction, now empirically): the survivor A's PnL
// after B's settlement is BIT-IDENTICAL whether B was ever in the book. Run {A} vs
// {A,B} over the SAME clock; for every date from B's expiry onward the alive set is
// {A} in both runs, so A's Taylor PnL and the book greeks must match to the bit.
// This is the exact invariant the carry-over relies on: A's per-lane solve is
// independent of the book's composition.
TEST(BacktestExec, L1SurvivorPnlBitIdenticalAcrossMembershipChange) {
  const fs::path dir = fresh_dir("l1-diff");
  const Clock clock = offset_clock(dir, "SPX", {0, 20, 40, 60, 80});
  const std::int64_t exp_B = kBaseNow + 60 * kDayNs;   // settles on date index 3
  const std::int64_t exp_A = kBaseNow + 200 * kDayNs;

  const auto book_A = [&] {
    PortfolioState b;
    b.lots.push_back(
        Lot{1, OptionContract{kUid, 100.0, 0.0, Side::Put}, +1.0, 100.0, exp_A, 0, 0.0});
    return b;
  };
  const auto book_AB = [&] {
    PortfolioState b = book_A();
    b.lots.push_back(
        Lot{2, OptionContract{kUid, 95.0, 0.0, Side::Call}, +1.0, 100.0, exp_B, 0, 0.0});
    return b;
  };

  const auto r_a = run_backtest(clock, book_A(), det_config());
  const auto r_ab = run_backtest(clock, book_AB(), det_config());
  ASSERT_TRUE(r_a.has_value()) << r_a.error().to_string();
  ASSERT_TRUE(r_ab.has_value()) << r_ab.error().to_string();
  ASSERT_EQ(r_a->size(), 5u);
  ASSERT_EQ(r_ab->size(), 5u);

  // Confirm the scenario: B settles on row 3 of the {A,B} run only.
  EXPECT_NE(r_ab->pnl_settlement[3], 0.0) << "B should settle on row 3 of the {A,B} run";
  EXPECT_EQ(r_a->pnl_settlement[3], 0.0) << "no settlement in the {A}-only run";

  // Rows 3 (B's expiry day, where the carry fires) and 4 (a following no-churn day)
  // have alive == {A} in BOTH runs: A's Taylor PnL COMPONENTS + book greeks must be
  // bit-equal. (`pnl_total` is EXCLUDED: the fixed-book overload folds B's settlement
  // into row 3's aggregate — step_total = t.pnl_total + settlement — so the aggregate
  // legitimately differs; every per-axis component below is settlement-free pure-A.)
  const auto a_cols = [](const BacktestResult& r, std::size_t i) {
    return std::array<double, 13>{
        r.pnl_delta[i], r.pnl_gamma[i],  r.pnl_vega[i],   r.pnl_vanna[i],   r.pnl_volga[i],
        r.pnl_theta[i], r.pnl_rho[i],    r.pnl_charm[i],  r.pnl_unexplained[i],
        r.gross_delta[i], r.gross_gamma[i], r.gross_vega[i], r.gross_theta[i]};
  };
  for (const std::size_t i : {std::size_t{3}, std::size_t{4}}) {
    const auto ca = a_cols(*r_a, i);
    const auto cb = a_cols(*r_ab, i);
    for (std::size_t k = 0; k < ca.size(); ++k) {
      EXPECT_TRUE(bits_equal(ca[k], cb[k]))
          << "survivor A PnL/greeks diverged at row " << i << " col " << k
          << " ({A}=" << ca[k] << " vs {A,B}=" << cb[k] << ") — the carry-over is NOT bit-identical";
    }
  }
  // Row 4 has NO settlement in either run, so the aggregate pnl_total is pure-A too.
  EXPECT_TRUE(bits_equal(r_a->pnl_total[4], r_ab->pnl_total[4]))
      << "survivor A aggregate PnL diverged on the post-expiry no-churn row";
  std::printf("[btexec] L1 survivor bit-identity: {A} vs {A,B} match to the bit on rows 3-4\n");
}

// Positive control (Trap 3): the carry re-homes the stamp, but it must NOT weaken the
// pnl reuse's base-surface guard. Stamp base risk at surface v1, carry onto the
// subset book, then run pnl against a DIFFERENT base surface v2 — the guard must
// REFUSE the (now surface-mismatched) reuse and RE-SOLVE. The re-solve is observable
// on the ledger AND its result must equal a cold pnl on the subset at v2 (it would be
// the WRONG v1 risk if the carry silently defeated the guard).
TEST(BacktestExec, L1CarryStillHonorsSurfaceGuard) {
  const std::int64_t ts = kBaseNow;
  const PricedSurface sv1 = make_surface(kUid, 100.0, 100.0, ts, 0.000);
  const PricedSurface sv2 = make_surface(kUid, 103.0, 103.0, ts, 0.010);  // different base surface
  const PricedSurface sv3 = make_surface(kUid, 104.0, 104.0, ts, 0.012);  // shifted surface
  const auto make_set = [](const PricedSurface& s) {
    const PricedSurface* ptrs[] = {&s};
    auto set = SurfaceSet::create(std::span<const PricedSurface* const>{ptrs});
    EXPECT_TRUE(set.has_value());
    return std::move(*set);
  };
  const SurfaceSet set_v1 = make_set(sv1);
  const SurfaceSet set_v2 = make_set(sv2);
  const SurfaceSet set_v3 = make_set(sv3);

  const double T = 0.35;
  const Position pa{1, OptionContract{kUid, 100.0, T, Side::Put}, +1.0, 100.0};
  const Position pb{2, OptionContract{kUid, 95.0, T, Side::Call}, +1.0, 100.0};

  PriceOptions opts;
  opts.n_threads = 1u;
  opts.analytic_greeks = true;

  // prev {A,B} stamps base risk at v1.
  auto prev_pf = Portfolio::create(std::array<Position, 2>{pa, pb});
  ASSERT_TRUE(prev_pf.has_value());
  PortfolioPricer prev(std::move(*prev_pf));
  PortfolioWorkspace ws;
  ws.reserve(2, 2);
  ASSERT_TRUE(prev.price_totals(set_v1, PriceFieldMask::FullGreeks, ws, opts).has_value());

  // Subset {A}: the carry succeeds (same-surface subset), re-homing the stamp onto next.
  auto next_pf = Portfolio::create(std::array<Position, 1>{pa});
  ASSERT_TRUE(next_pf.has_value());
  PortfolioPricer next(std::move(*next_pf));
  EXPECT_TRUE(next.carry_base_risk_subset(prev, ws)) << "same-surface subset carry should succeed";

  // pnl at a DIFFERENT base surface v2: the guard must refuse the carried stamp.
  led::reset();
  const auto got = next.pnl_totals(set_v2, set_v3, ws, opts);
  ASSERT_TRUE(got.has_value()) << got.error().to_string();
  EXPECT_GE(analytic(led::snapshot()), 1u)
      << "base-surface guard failed to refuse a surface-mismatched carried stamp (no re-solve)";

  // Cold reference: a fresh pricer with no carry, same surfaces.
  auto cold_pf = Portfolio::create(std::array<Position, 1>{pa});
  ASSERT_TRUE(cold_pf.has_value());
  PortfolioPricer cold(std::move(*cold_pf));
  PortfolioWorkspace ws_cold;
  ws_cold.reserve(1, 1);
  const auto ref = cold.pnl_totals(set_v2, set_v3, ws_cold, opts);
  ASSERT_TRUE(ref.has_value()) << ref.error().to_string();

  EXPECT_TRUE(bits_equal(got->pv_base, ref->pv_base))
      << "guard-refused re-solve did not match the cold v2 base price (stale v1 risk served?)";
  EXPECT_TRUE(bits_equal(got->pnl_total, ref->pnl_total));
  EXPECT_TRUE(bits_equal(got->pnl_vega, ref->pnl_vega));
  std::printf("[btexec] L1 positive control: surface-mismatched carry refused + re-solved to the "
              "cold v2 value (pv_base=%.10g)\n", got->pv_base);
}

// Positive control: a survivor whose INPUT changed (tenor T) is a different
// (uid,K,T,side) key, so carry_base_risk_subset must REFUSE (return false) — reuse
// would serve stale risk. The subsequent solve then recomputes at the new tenor.
TEST(BacktestExec, L1SubsetCarryRefusesOnChangedInput) {
  const std::int64_t ts = kBaseNow;
  const PricedSurface sv1 = make_surface(kUid, 100.0, 100.0, ts, 0.0);
  const PricedSurface* ptrs[] = {&sv1};
  auto set_v1 = SurfaceSet::create(std::span<const PricedSurface* const>{ptrs});
  ASSERT_TRUE(set_v1.has_value());

  PriceOptions opts;
  opts.n_threads = 1u;
  opts.analytic_greeks = true;

  const Position pa1{1, OptionContract{kUid, 100.0, 0.35, Side::Put}, +1.0, 100.0};
  const Position pb{2, OptionContract{kUid, 95.0, 0.35, Side::Call}, +1.0, 100.0};
  auto prev_pf = Portfolio::create(std::array<Position, 2>{pa1, pb});
  ASSERT_TRUE(prev_pf.has_value());
  PortfolioPricer prev(std::move(*prev_pf));
  PortfolioWorkspace ws;
  ws.reserve(2, 2);
  ASSERT_TRUE(prev.price_totals(*set_v1, PriceFieldMask::FullGreeks, ws, opts).has_value());

  // Survivor A's tenor changed 0.35 -> 0.30: NOT a bit-exact subset key.
  const Position pa2{1, OptionContract{kUid, 100.0, 0.30, Side::Put}, +1.0, 100.0};
  auto next_pf = Portfolio::create(std::array<Position, 1>{pa2});
  ASSERT_TRUE(next_pf.has_value());
  PortfolioPricer next(std::move(*next_pf));
  EXPECT_FALSE(next.carry_base_risk_subset(prev, ws))
      << "carry must refuse a survivor whose (uid,K,T,side) changed";

  // The refusal is load-bearing: the following pnl re-solves at T=0.30 and matches a
  // cold reference (a wrongly-reused T=0.35 bundle would give a different base price).
  const auto got = next.pnl_totals(*set_v1, *set_v1, ws, opts);  // dt=0: pure base reprice
  ASSERT_TRUE(got.has_value()) << got.error().to_string();
  PortfolioPricer cold(std::move(*Portfolio::create(std::array<Position, 1>{pa2})));
  PortfolioWorkspace ws_cold;
  ws_cold.reserve(1, 1);
  const auto ref = cold.pnl_totals(*set_v1, *set_v1, ws_cold, opts);
  ASSERT_TRUE(ref.has_value()) << ref.error().to_string();
  EXPECT_TRUE(bits_equal(got->pv_base, ref->pv_base))
      << "changed-tenor refusal did not recompute at the new T (stale T=0.35 risk served?)";
  std::printf("[btexec] L1 positive control: changed-tenor survivor carry refused, re-solved at "
              "the new tenor\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// L2 (AL-solve-wall sprint) — per-(contract,date) settlement-mark memo.
//
// CRUX (PM adjudication): the settlement-mark memo serves the settlement path a
// mark that was computed by the FullGreeks/analytic book-greeks pass, in place of
// the Marks-mask solve the settle path would otherwise run. The two paths must
// agree to within economic nil for the memo to be sound.
//
// With the AVX2 American marks ship gate ON (kShipAvx2Boundary=true, PM 2026-07-19)
// the Marks-mask path routes the 4-wide AVX2 boundary kernel while the FullGreeks
// analytic mark stays scalar, so the two are no longer bit-identical: they diverge
// by the AVX2 economic-parity residual (measured ~1e-13 USD absolute, ~1e-14
// relative on this population — FMA/reduction-order, not a model difference). We
// therefore hold the crux to a <=1e-10 RELATIVE tolerance instead of bit-identity:
// ~1000x above float noise, ~1e7x below the 1-tick economic gate — i.e. ~10 orders
// of magnitude below a settlement tick, so the memo substitution stays economically
// exact. Documented under the PM epsilon license (algorithmically correct — same
// AL solve, SIMD-reassociated — and economically meaningless). If the divergence
// ever EXCEEDS this relative tolerance, that is a real model split: STOP and report,
// do NOT widen the tolerance.
// ─────────────────────────────────────────────────────────────────────────────
TEST(BacktestExec, L2MarkMemoCruxFullGreeksMarkEqualsMarksMark) {
  const PricedSurface s = make_surface(kUid, 100.0, 100.0, kBaseNow, 0.0);
  const PricedSurface* ptrs[] = {&s};
  auto set = SurfaceSet::create(std::span<const PricedSurface* const>{ptrs});
  ASSERT_TRUE(set.has_value());

  // A spread of strikes/sides/tenors — a representative settling-lot population.
  std::vector<Position> pos;
  std::uint64_t id = 1;
  for (const double K : {80.0, 95.0, 100.0, 105.0, 120.0}) {
    for (const Side side : {Side::Call, Side::Put}) {
      for (const double T : {0.10, 0.35, 0.75}) {
        pos.push_back(Position{id++, OptionContract{kUid, K, T, side}, +1.0, 100.0});
      }
    }
  }
  auto pf = Portfolio::create(pos);
  ASSERT_TRUE(pf.has_value());
  PortfolioPricer pricer(std::move(*pf));

  PriceOptions marks_opts;
  marks_opts.n_threads = 1u;
  marks_opts.prices_only = true;  // Marks mask == the settlement solve
  PriceOptions greeks_opts;
  greeks_opts.n_threads = 1u;
  greeks_opts.analytic_greeks = true;  // FullGreeks analytic == the book-greeks memo source

  const auto marks = pricer.price(*set, marks_opts);
  const auto greeks = pricer.price(*set, greeks_opts);
  ASSERT_TRUE(marks.has_value()) << marks.error().to_string();
  ASSERT_TRUE(greeks.has_value()) << greeks.error().to_string();
  ASSERT_EQ(marks->size(), greeks->size());
  std::size_t n_ok = 0;
  for (std::size_t i = 0; i < marks->size(); ++i) {
    ASSERT_EQ(marks->status[i], greeks->status[i]);
    if (marks->status[i] != PriceStatus::Ok) {
      continue;
    }
    ++n_ok;
    // AVX2 economic-parity tolerance, not bit-identity: the Marks path (AVX2 boundary)
    // and the FullGreeks-analytic mark (scalar) differ only by the SIMD reassociation
    // residual (~1e-13 USD, ~1e-14 relative). <=1e-10 relative is ~10 orders below a
    // tick — economic nil (PM epsilon license). A denominator floor of 1.0 keeps the
    // bound sane for deep-OTM marks near zero without ever loosening it economically.
    const double marks_px = marks->price[i];
    const double greeks_px = greeks->price[i];
    const double rel = std::fabs(marks_px - greeks_px) / std::max(std::fabs(greeks_px), 1.0);
    EXPECT_LE(rel, 1e-10)
        << "CRUX FAIL: Marks mark diverges from FullGreeks-analytic mark ABOVE the AVX2 "
           "economic-parity tolerance for lot "
        << marks->id[i] << " (Marks=" << marks_px << " FullGreeks=" << greeks_px
        << " rel=" << rel << ") — this exceeds ~10 orders below a tick, so it is a real "
           "model split, NOT a SIMD epsilon; STOP + report, do NOT widen the tolerance.";
  }
  ASSERT_GT(n_ok, 20u) << "crux population too small to be meaningful";
  std::printf("[btexec] L2 crux: FullGreeks-analytic mark == Marks mark within <=1e-10 relative "
              "(AVX2 economic parity) on %zu lots\n",
              n_ok);
}

// L2 gate: the settlement-mark memo serves B's expiry-day base mark from the prior
// step's book-greeks pass instead of re-solving it — expiry al 7 -> 6, and the new
// DuplicateMarkSolves counter goes 0 (memo on) / >=1 (memo off, proving the counter
// sees the duplication). Memo ON is BIT-IDENTICAL to memo OFF (the pre-L2 behavior).
TEST(BacktestExec, L2SettlementMarkMemoDropsExpirySolve) {
  const fs::path dir = fresh_dir("l2-settle");
  const Clock clock = offset_clock(dir, "SPX", {0, 20, 40, 60, 80});
  const std::int64_t exp_B = kBaseNow + 60 * kDayNs;   // settles on date index 3
  const std::int64_t exp_A = kBaseNow + 200 * kDayNs;  // survives
  const auto make_book = [&] {
    PortfolioState b;
    b.lots.push_back(
        Lot{1, OptionContract{kUid, 100.0, 0.0, Side::Put}, +1.0, 100.0, exp_A, 0, 0.0});
    b.lots.push_back(
        Lot{2, OptionContract{kUid, 95.0, 0.0, Side::Call}, +1.0, 100.0, exp_B, 0, 0.0});
    return b;
  };
  const auto dup = [](const led::Counts& c) { return c.get(led::Solve::DuplicateMarkSolves); };

  // Memo OFF: pre-L2 economy — the expiry step still solves B's settlement (al 7), and
  // the counter observes that duplicate (B's mark was already in the memo).
  RunConfig off = det_config();
  off.settlement_mark_memo = false;
  led::reset();
  led::StepTrace trace_off;
  const auto r_off = run_backtest(clock, make_book(), off);
  ASSERT_TRUE(r_off.has_value()) << r_off.error().to_string();
  const led::Counts total_off = led::snapshot();
  ASSERT_EQ(trace_off.size(), 4u);
  EXPECT_EQ(al(trace_off.steps()[2]), 7u) << "memo off: expiry step still solves B's settlement";
  EXPECT_EQ(dup(total_off), 1u) << "counter must see the one duplicate settlement solve (memo off)";

  // Memo ON (default): B's settlement served from the memo -> expiry al 7->6, 0 dups.
  RunConfig on = det_config();  // settlement_mark_memo defaults true
  led::reset();
  led::StepTrace trace_on;
  const auto r_on = run_backtest(clock, make_book(), on);
  ASSERT_TRUE(r_on.has_value()) << r_on.error().to_string();
  const led::Counts total_on = led::snapshot();
  ASSERT_EQ(trace_on.size(), 4u);
  EXPECT_EQ(al(trace_on.steps()[2]), 6u) << "L2: expiry step 7->6 (B settlement served from memo)";
  EXPECT_EQ(analytic(trace_on.steps()[2]), 1u);
  EXPECT_EQ(dup(total_on), 0u) << "L2 gate: DuplicateMarkSolves == 0 with the memo on";
  EXPECT_EQ(al(trace_on.steps()[0]), 12u) << "warm steps unchanged";
  EXPECT_EQ(al(trace_on.steps()[1]), 12u);
  EXPECT_EQ(al(trace_on.steps()[3]), 6u) << "no-churn step unchanged";

  // Bit-identity: memo ON == memo OFF, full result (settlement mark served == solved).
  ASSERT_EQ(r_on->size(), r_off->size());
  const auto cols = [](const BacktestResult& r, std::size_t i) {
    return std::array<double, 8>{r.nav[i],      r.pnl_total[i],   r.pnl_settlement[i],
                                 r.pnl_delta[i], r.pnl_vega[i],    r.gross_vega[i],
                                 r.gross_delta[i], r.gross_theta[i]};
  };
  for (std::size_t i = 0; i < r_on->size(); ++i) {
    const auto con = cols(*r_on, i);
    const auto cof = cols(*r_off, i);
    for (std::size_t k = 0; k < con.size(); ++k) {
      EXPECT_TRUE(bits_equal(con[k], cof[k]))
          << "L2 memo ON != OFF at row " << i << " col " << k
          << " — the settlement memo is NOT bit-identical to the solve";
    }
  }
  std::printf("[btexec] L2 settlement memo: expiry al 7->6, dup=%llu (on)/%llu (off), memo on==off\n",
              static_cast<unsigned long long>(dup(total_on)),
              static_cast<unsigned long long>(dup(total_off)));
}

// ─────────────────────────────────────────────────────────────────────────────
// L2 memo admission gate (plan 1.11) — NaN is the settlement path's IN-BAND
// "must solve" sentinel, so a NaN mark must never be admitted to the memo.
//
// compute_step's L2 settlement branch seeds `served[i]` with NaN, serves a memo
// hit into it, and then treats `std::isnan(served[i])` as "this lot was a memo
// MISS, take its mark from the batched solve frame". A memo that admitted a
// non-finite Ok-status mark collides with that sentinel: the lot is served (so it
// is NEVER pushed into `to_solve`) yet reads as a miss, which dereferences the
// null `sf_ptr` when it is the only expiring lot, and desyncs `solve_ix` against
// the solve frame when it is not.
//
// The marks that trip this cannot be produced through the public pricing stack —
// every Ok-stamp in portfolio_pricer.cpp sweeps `isfinite(price)` first — so the
// gate is driven directly through `populate_from_marks` (see step_mark_memo.hpp).
// ─────────────────────────────────────────────────────────────────────────────
TEST(BacktestExec, L2MarkMemoNonFiniteOkMarkIsNotServed) {
  const PricedSurface s = make_surface(kUid, 100.0, 100.0, kBaseNow, 0.0);
  const PricedSurface* ptrs[] = {&s};
  auto set = SurfaceSet::create(std::span<const PricedSurface* const>{ptrs});
  ASSERT_TRUE(set.has_value()) << set.error().to_string();
  const std::uint64_t inst = set->find(kUid)->instance_id();

  constexpr double kQNaN = std::numeric_limits<double>::quiet_NaN();
  constexpr double kInf = std::numeric_limits<double>::infinity();
  const std::vector<RetainedMark> marks{
      // A finite Ok mark — the positive control: still served.
      RetainedMark{kUid, 100.0, 0.25, Side::Call, 3.25, PriceStatus::Ok},
      // Ok-status but non-finite: must be a memo MISS, not a served NaN/Inf.
      RetainedMark{kUid, 95.0, 0.25, Side::Put, kQNaN, PriceStatus::Ok},
      RetainedMark{kUid, 90.0, 0.25, Side::Put, kInf, PriceStatus::Ok},
      RetainedMark{kUid, 85.0, 0.25, Side::Put, -kInf, PriceStatus::Ok},
      // Already excluded by status; pinned so the new filter did not replace it.
      RetainedMark{kUid, 105.0, 0.25, Side::Call, 4.0, PriceStatus::NumericError},
  };

  detail::StepMarkMemo memo;
  memo.populate_from_marks(marks, *set);

  const auto served = [&](double K, Side side) { return memo.find(kUid, K, 0.25, side, inst); };
  ASSERT_TRUE(served(100.0, Side::Call).has_value())
      << "positive control: a finite Ok mark must still be served from the memo";
  EXPECT_TRUE(bits_equal(*served(100.0, Side::Call), 3.25));
  EXPECT_FALSE(served(95.0, Side::Put).has_value())
      << "a NaN Ok mark was admitted — it collides with the settlement miss sentinel "
         "(null sf_ptr deref / desynced solve_ix)";
  EXPECT_FALSE(served(90.0, Side::Put).has_value()) << "+Inf Ok mark admitted";
  EXPECT_FALSE(served(85.0, Side::Put).has_value()) << "-Inf Ok mark admitted";
  EXPECT_FALSE(served(105.0, Side::Call).has_value()) << "non-Ok mark admitted";
}

// L2 strategy-path bit-identity: over a daily-cohort strangle that settles cohorts
// mid-run (the B4 shape), memo ON == memo OFF, full result bit-for-bit — proving the
// settlement memo is bit-identical in the STRATEGY overload too (where it is
// populated only from the non-seeded book-greeks pass and falls closed on stale/
// execute-fired steps).
TEST(BacktestExec, L2StrategyCohortSettlementMemoBitIdentical) {
  const fs::path dir = fresh_dir("l2-strat-cohorts");
  constexpr int kNames = 4;
  constexpr int kDates = 24;
  static const char* kSyms[] = {"AAA", "BBB", "CCC", "DDD"};
  std::vector<std::pair<std::string, std::string>> dp;
  for (int d = 0; d < kDates; ++d) {
    const std::int64_t now = kBaseNow + static_cast<std::int64_t>(d) * kDayNs;
    std::vector<PricedSurface> surfaces;
    surfaces.reserve(kNames);
    for (int u = 0; u < kNames; ++u) {
      const double S = (100.0 + 10.0 * static_cast<double>(u)) * (1.0 + 0.003 * static_cast<double>(d));
      surfaces.push_back(make_surface(kUid + static_cast<std::uint32_t>(u), S, S, now,
                                      0.001 * static_cast<double>(d) + 0.002 * static_cast<double>(u)));
    }
    std::vector<SurfaceArchiveItem> items;
    for (int u = 0; u < kNames; ++u) {
      items.push_back(SurfaceArchiveItem{kSyms[u], &surfaces[u]});
    }
    std::error_code ec;
    fs::create_directories(dir, ec);
    char buf[16];
    std::snprintf(buf, sizeof buf, "2026-08-%02d", d + 1);
    const std::string path = (dir / (std::string(buf) + ".atxvsa")).string();
    ASSERT_TRUE(write_surface_archive_v2_file(path, items).has_value());
    dp.emplace_back(buf, path);
  }
  auto clock = Clock::from_manifest(make_manifest(dp, "AAA"));
  ASSERT_TRUE(clock.has_value()) << clock.error().to_string();

  const double tenor_T = static_cast<double>(20 * kDayNs) / kNsPerYear;  // -> settles at 20 days
  StrategySpec spec;
  spec.name = "l2-daily-strangle-htx";
  for (int u = 0; u < kNames; ++u) {
    LegSpec leg;
    leg.uid = kUid + static_cast<std::uint32_t>(u);
    leg.tenor.target_T = tenor_T;
    leg.tenor.snap_to_listed = false;
    leg.structure.kind = StructureSpec::Kind::Strangle;
    leg.structure.call_leg = StrikeSelector{StrikeSelector::Kind::Delta, 0.40};
    leg.structure.put_leg = StrikeSelector{StrikeSelector::Kind::Delta, 0.40};
    leg.size = SizeSpec{SizeSpec::Kind::FixedContracts, 1.0, +1.0};
    spec.legs.push_back(leg);
  }
  spec.lifecycle.entry = LifecycleSpec::Entry::EveryStep;
  spec.lifecycle.holding = LifecycleSpec::Holding::HoldToExpiry;

  const auto run = [&](bool memo) {
    DeclarativeStrategy strat{spec};
    RunConfig cfg;
    cfg.price.n_threads = 1u;
    cfg.settlement_mark_memo = memo;
    return run_backtest(*clock, strat, cfg);
  };
  const auto r_on = run(true);
  const auto r_off = run(false);
  ASSERT_TRUE(r_on.has_value()) << r_on.error().to_string();
  ASSERT_TRUE(r_off.has_value()) << r_off.error().to_string();
  ASSERT_EQ(r_on->size(), r_off->size());

  int settle_rows = 0;
  for (std::size_t i = 0; i < r_off->size(); ++i) {
    if (std::fabs(r_off->pnl_settlement[i]) > 0.0) {
      ++settle_rows;
    }
  }
  EXPECT_GT(settle_rows, 0) << "no cohort settled — the strategy memo path is not exercised";
  // C1 (backtest-lakehouse sprint): before C1, this strategy's memo was NEVER
  // actually consulted -- `Entry::EveryStep` keeps `ExecResult::book_greeks` set on
  // every step (entry_happened is always true), so the standalone `book_greeks()`
  // call that was the ONLY pre-C1 memo-populate site never ran, and every
  // settlement always re-solved regardless of `settlement_mark_memo`. That made
  // "memo on == memo off" trivially bit-identical: both arms always solved.
  //
  // C1 populates the memo from execute()'s own FullGreeks pass, so a settlement
  // here can now genuinely be SERVED from the memo for the first time. The served
  // mark comes from a FullGreeks AVX2 batch over the WHOLE overlapping-cohort book
  // (potentially dozens of lots); the solved mark comes from a Marks-only AVX2
  // batch over just that day's few expiring lots -- a DIFFERENT SIMD lane
  // composition for the identical contract. `L2MarkMemoCruxFullGreeksMarkEqualsMarks
  // Mark` above already documents that FullGreeks and Marks marks are an ECONOMIC
  // parity guarantee (<=1e-10 relative), not a bit-identity one; this is that same
  // AVX2 reassociation residual, one order of magnitude smaller (~1e-16 relative,
  // 1-2 ULP) because both routes are AVX2 here (vs. AVX2-vs-scalar in the crux
  // test). Only `pnl_settlement`/`pnl_total`/`nav` can see it (they subtract the
  // served/solved mark); `cash` does not (settlement credits CASH from the plain
  // intrinsic, never the mark) and `gross_vega`/`n_open_lots` are untouched by
  // settlement at all -- so those four stay bit-identical below.
  constexpr double kSettlementMarkParityTol = 1e-9; // >>1-2 ULP, <<1 cent on this book
  for (std::size_t i = 0; i < r_on->size(); ++i) {
    EXPECT_NEAR(r_on->nav[i], r_off->nav[i], kSettlementMarkParityTol) << "nav row " << i;
    EXPECT_NEAR(r_on->pnl_total[i], r_off->pnl_total[i], kSettlementMarkParityTol)
        << "pnl_total row " << i;
    EXPECT_NEAR(r_on->pnl_settlement[i], r_off->pnl_settlement[i], kSettlementMarkParityTol)
        << "settle row " << i;
    EXPECT_TRUE(bits_equal(r_on->gross_vega[i], r_off->gross_vega[i])) << "gross_vega row " << i;
    EXPECT_TRUE(bits_equal(r_on->cash[i], r_off->cash[i])) << "cash row " << i;
    EXPECT_EQ(r_on->n_open_lots[i], r_off->n_open_lots[i]) << "n_open_lots row " << i;
  }
  std::printf("[btexec] L2 strategy cohort settlement: memo on==off over %d settle rows, "
              "bit-identical\n",
              settle_rows);
}

// ─────────────────────────────────────────────────────────────────────────────
// C1 (backtest-lakehouse sprint) — mark-memo repopulation on the execute() path.
//
// `L2StrategyCohortSettlementMemoBitIdentical` above already documents the defect:
// its strategy re-enters EVERY step (`Entry::EveryStep`), so `ExecResult::book_greeks`
// has a value on every step and the standalone `book_greeks()` call that is the ONLY
// place the step-mark memo gets populated (pre-C1) never runs — the memo is
// permanently cold, and every settlement re-solves.
//
// A DAILY-HEDGED strategy hits the identical starvation for a different reason:
// `HedgeSpec::Cadence::Daily` makes `execute()`'s `hedge_fires` true on EVERY step
// regardless of `entry_happened`, so `ExecResult::book_greeks` again always has a
// value and the memo again never populates. C1 fixes this at the root — execute()'s
// own FullGreeks `price_into` pass now populates the memo itself — so BOTH shapes
// benefit; this fixture isolates the daily-hedge trigger with a single cohort held
// to an EXACT expiry so the solve-ledger counts are unambiguous (`n_expiring_lots`
// lots, one settlement event, nothing else).
// ─────────────────────────────────────────────────────────────────────────────

namespace {

// A daily-hedged strangle (call+put, ONE cohort opened at inception via a huge
// `entry_every_n`, `HoldToExpiry`) whose expiry lands EXACTLY on a later clock
// date — `expiry_day < n_dates` settles it inside the run; `expiry_day >= n_dates`
// leaves it open at run end (the bit-identity/no-expiry variant below). "ExecFixture
// ::daily_hedged_strangle_over_expiry() or equivalent" from the C1 brief: kept as a
// standalone fixture rather than folded into `ExecFixture` above (that class is
// QuoteSide-specific — its `.corpus()`/`.config()` return QuoteSide-only types/
// configs — so bolting an unrelated daily-hedge/expiry mode onto it would trade one
// coupling for another).
struct DailyHedgedExpiryFixture {
  Clock clock;
  StrategySpec spec;
  std::size_t n_expiring_lots{0};

  [[nodiscard]] DeclarativeStrategy strategy() const { return DeclarativeStrategy{spec}; }
  [[nodiscard]] static RunConfig config() {
    RunConfig cfg;
    cfg.price.n_threads = 1u;       // solve-ledger counts are thread-invariant
    cfg.prefetch_snapshots = false; // remove async-prefetch nondeterminism
    return cfg;                    // settlement_mark_memo defaults true
  }
};

[[nodiscard]] DailyHedgedExpiryFixture
make_daily_hedged_strangle_fixture(const fs::path& dir, int n_dates, int expiry_day) {
  const Corpus c = make_corpus(dir, "SPX", n_dates);
  auto clock = Clock::from_manifest(c.manifest);
  EXPECT_TRUE(clock.has_value()) << (clock.has_value() ? std::string{} : clock.error().to_string());

  StrategySpec spec;
  spec.name = "c1-daily-hedged-strangle";
  LegSpec leg;
  leg.uid = kUid;
  leg.tenor.target_T = static_cast<double>(expiry_day * kDayNs) / kNsPerYear;
  leg.tenor.snap_to_listed = false;
  leg.structure.kind = StructureSpec::Kind::Strangle;
  leg.structure.call_leg = StrikeSelector{StrikeSelector::Kind::Delta, 0.40};
  leg.structure.put_leg = StrikeSelector{StrikeSelector::Kind::Delta, 0.40};
  leg.size = SizeSpec{SizeSpec::Kind::FixedContracts, 1.0, +1.0};
  spec.legs.push_back(leg);
  spec.lifecycle.entry = LifecycleSpec::Entry::EveryNDays;
  spec.lifecycle.entry_every_n = 100000; // >> corpus => opens once, at inception
  spec.lifecycle.holding = LifecycleSpec::Holding::HoldToExpiry;
  spec.hedge = HedgeSpec{HedgeSpec::Kind::DeltaToZero, HedgeSpec::Cadence::Daily, 0.0};

  const std::size_t n_expiring = expiry_day < n_dates ? 2u : 0u; // call + put, same cohort
  return DailyHedgedExpiryFixture{std::move(*clock), std::move(spec), n_expiring};
}

} // namespace

// The pinned solve-ledger gate: expiry-day settlement marks must be memo hits, not
// fresh solves, on the daily-hedge route. Pre-C1 this fails with
// settlement_full_solves == 2 (both legs re-solved) and settlement_memo_hits == 0
// (the memo was never populated) — the bug's exact count signature.
TEST(BacktestSolveLedger, ExpiryMarksMemoHitOnDailyHedgeRoute) {
  const fs::path dir = fresh_dir("c1-solve-ledger");
  const DailyHedgedExpiryFixture fx =
      make_daily_hedged_strangle_fixture(dir, /*n_dates=*/8, /*expiry_day=*/5);
  ASSERT_EQ(fx.n_expiring_lots, 2u);

  auto strat = fx.strategy();
  auto r = run_backtest(fx.clock, strat, fx.config());
  ASSERT_TRUE(r.has_value()) << r.error().to_string();
  EXPECT_EQ(0u, r->solve_ledger.settlement_full_solves)
      << "expiry settlement should be served from the L2 mark memo on the daily-hedge "
         "route, not re-solved";
  EXPECT_GE(r->solve_ledger.settlement_memo_hits, fx.n_expiring_lots);
  std::printf("[btexec] C1 daily-hedge settlement memo: hits=%llu full_solves=%llu "
              "(n_expiring_lots=%zu)\n",
              static_cast<unsigned long long>(r->solve_ledger.settlement_memo_hits),
              static_cast<unsigned long long>(r->solve_ledger.settlement_full_solves),
              fx.n_expiring_lots);
}

// Bit-identity (KEY INVARIANT): the memo changes SOLVE COUNT, never VALUES. On a
// non-expiry corpus (the cohort's expiry falls well beyond the run window, so
// nothing ever settles) memo ON and memo OFF must produce a bit-identical result —
// proving execute()'s new populate_from call (which now runs on EVERY daily-hedged
// step, whether or not anything ever consumes the memo) is inert on the economics.
// Both solve-ledger counters are also asserted at exactly 0 in both runs: with no
// expiring lot, compute_step's L2 settlement branch never executes at all.
TEST(BacktestSolveLedger, DailyHedgeNoExpiryMemoOnOffBitIdentical) {
  const fs::path dir = fresh_dir("c1-solve-ledger-noexpiry");
  const DailyHedgedExpiryFixture fx =
      make_daily_hedged_strangle_fixture(dir, /*n_dates=*/6, /*expiry_day=*/60);
  ASSERT_EQ(fx.n_expiring_lots, 0u) << "fixture must not settle within this corpus";

  const auto run = [&](bool memo) {
    auto strat = fx.strategy();
    RunConfig cfg = fx.config();
    cfg.settlement_mark_memo = memo;
    return run_backtest(fx.clock, strat, cfg);
  };
  const auto r_on = run(true);
  const auto r_off = run(false);
  ASSERT_TRUE(r_on.has_value()) << r_on.error().to_string();
  ASSERT_TRUE(r_off.has_value()) << r_off.error().to_string();
  ASSERT_EQ(r_on->size(), r_off->size());

  for (std::size_t i = 0; i < r_on->size(); ++i) {
    EXPECT_TRUE(bits_equal(r_on->nav[i], r_off->nav[i])) << "nav row " << i;
    EXPECT_TRUE(bits_equal(r_on->pnl_total[i], r_off->pnl_total[i])) << "pnl_total row " << i;
    EXPECT_TRUE(bits_equal(r_on->cash[i], r_off->cash[i])) << "cash row " << i;
    EXPECT_TRUE(bits_equal(r_on->gross_vega[i], r_off->gross_vega[i])) << "gross_vega row " << i;
    EXPECT_TRUE(bits_equal(r_on->gross_delta[i], r_off->gross_delta[i])) << "gross_delta row " << i;
  }
  EXPECT_EQ(0u, r_on->solve_ledger.settlement_memo_hits);
  EXPECT_EQ(0u, r_on->solve_ledger.settlement_full_solves);
  EXPECT_EQ(0u, r_off->solve_ledger.settlement_memo_hits);
  EXPECT_EQ(0u, r_off->solve_ledger.settlement_full_solves);
  std::printf("[btexec] C1 daily-hedge no-expiry: memo on==off bit-identical over %zu rows\n",
              r_on->size());
}

// ─────────────────────────────────────────────────────────────────────────────
// L4 (AL-solve-wall sprint) — first-order tier wiring (the K4 last-mile seam).
//
// Edit B threads `PricedSurface::GreekNeeds` from evaluate/evaluate_batch/
// greeks_analytic down into `american_greeks_al(..., need_vega, need_rho, need_charm)`,
// so a reduced Greek request skips whole boundary solves. Edit A carries it on
// `PriceOptions::greek_needs` through PortfolioPricer::price_* -> solve_uniques, and
// adds the base-risk stamp guard (`base_greek_needs.full()`) so a narrowed frame is
// never reused for a full P&L attribution.
//
// The tests below are the L4 gates: (1) the bit-identity proof (default = full = the
// pre-L4 maskless bundle; a reduced request returns its columns BIT-IDENTICAL and the
// unrequested greeks 0), (2) the per-role ledger economy (full=5, risk=3, hedge=1
// boundary solves/unique), and (3) the stamp-guard safety (a narrowed base is
// re-solved, never served, by a following pnl).
// ─────────────────────────────────────────────────────────────────────────────

// (1) Edit-B bit-identity / K4 guarantee. `greeks_analytic` with the DEFAULT
// GreekNeeds{} reproduces the pre-L4 maskless bundle bit-for-bit; a reduced request
// returns its requested columns BIT-IDENTICAL to the full bundle (same base boundary +
// sigma+/- stencils) with the unrequested greeks left exactly 0 on the native put AL
// route. (Calls / degenerate corners defer to american_greeks_fd, the full oracle,
// which ignores the mask — a correctness-preserving superset; the "==0" drop is
// asserted only on puts, which take the analytic route this fixture is built for.)
TEST(BacktestExec, L4EditBReducedColumnsBitIdenticalToFullBundle) {
  const PricedSurface s = make_surface(kUid, 100.0, 100.0, kBaseNow, 0.0);
  using GN = PricedSurface::GreekNeeds;
  const GN full{};                                                // pre-L4 maskless bundle
  const GN risk{/*vega=*/true, /*rho=*/false, /*charm=*/false};   // delta+vega (3 solves)
  const GN hedge{/*vega=*/false, /*rho=*/false, /*charm=*/false}; // delta only (1 solve)

  std::size_t n_ok = 0;
  std::size_t n_put_masked = 0;
  for (const double K : {90.0, 95.0, 100.0, 105.0, 110.0}) {
    for (const Side side : {Side::Call, Side::Put}) {
      for (const double T : {0.10, 0.35, 0.75}) {
        const auto gf = s.greeks_analytic(K, T, side);  // old signature -> default {}
        const auto ge = s.greeks_analytic(K, T, side, QueryExecution::Configured, full);
        const auto gr = s.greeks_analytic(K, T, side, QueryExecution::Configured, risk);
        const auto gh = s.greeks_analytic(K, T, side, QueryExecution::Configured, hedge);
        ASSERT_EQ(gf.has_value(), ge.has_value());
        if (!gf.has_value()) {
          continue;
        }
        ++n_ok;
        // Default arg == explicit full, to the bit (proves GreekNeeds{} is the maskless path).
        EXPECT_TRUE(bits_equal(gf->price, ge->price));
        EXPECT_TRUE(bits_equal(gf->delta, ge->delta));
        EXPECT_TRUE(bits_equal(gf->vega, ge->vega));
        EXPECT_TRUE(bits_equal(gf->rho, ge->rho));
        EXPECT_TRUE(bits_equal(gf->charm, ge->charm));
        // Requested columns BIT-IDENTICAL to the full bundle on BOTH routes.
        ASSERT_TRUE(gr.has_value());
        ASSERT_TRUE(gh.has_value());
        EXPECT_TRUE(bits_equal(gr->price, gf->price)) << "risk price K=" << K << " T=" << T;
        EXPECT_TRUE(bits_equal(gr->delta, gf->delta));
        EXPECT_TRUE(bits_equal(gr->gamma, gf->gamma));
        EXPECT_TRUE(bits_equal(gr->theta, gf->theta));
        EXPECT_TRUE(bits_equal(gr->vega, gf->vega)) << "risk vega K=" << K << " T=" << T;
        EXPECT_TRUE(bits_equal(gr->vanna, gf->vanna));
        EXPECT_TRUE(bits_equal(gr->volga, gf->volga));
        EXPECT_TRUE(bits_equal(gh->price, gf->price)) << "hedge price K=" << K << " T=" << T;
        EXPECT_TRUE(bits_equal(gh->delta, gf->delta)) << "hedge delta K=" << K << " T=" << T;
        EXPECT_TRUE(bits_equal(gh->gamma, gf->gamma));
        EXPECT_TRUE(bits_equal(gh->theta, gf->theta));
        // Unrequested greeks == 0 on the native put AL route (the mask skipped their solves).
        if (side == Side::Put && gr->rho == 0.0) {
          ++n_put_masked;
          EXPECT_EQ(gr->rho, 0.0);
          EXPECT_EQ(gr->charm, 0.0);
          EXPECT_EQ(gh->vega, 0.0);
          EXPECT_EQ(gh->rho, 0.0);
          EXPECT_EQ(gh->charm, 0.0);
          EXPECT_EQ(gh->vanna, 0.0);
          EXPECT_EQ(gh->volga, 0.0);
        }
      }
    }
  }
  ASSERT_GT(n_ok, 20u);
  EXPECT_GT(n_put_masked, 5u) << "no puts took the masked analytic AL route — fixture invalid";
  std::printf("[btexec] L4 Edit-B: reduced columns bit-identical to full on %zu lots; "
              "%zu puts masked rho/charm/vega to 0\n",
              n_ok, n_put_masked);
}

// (2) Per-role ledger economy: a single-unique FullGreeks/analytic price spends 5
// boundary solves at full needs, 3 at the risk role ({delta,vega}, r+/- skipped), 1 at
// the hedge role ({delta}, sigma+/- and r+/- skipped) — the K4 tier, on the SCALAR
// analytic production route (american_greeks_al), independent of any dark AVX2 flag.
TEST(BacktestExec, L4PerRoleBundleSolveEconomy) {
  const PricedSurface s = make_surface(kUid, 100.0, 100.0, kBaseNow, 0.0);
  const PricedSurface* ptrs[] = {&s};
  auto set = SurfaceSet::create(std::span<const PricedSurface* const>{ptrs});
  ASSERT_TRUE(set.has_value());

  // One genuinely-early-exercise put -> the analytic AL bundle route.
  const Position p{1, OptionContract{kUid, 100.0, 0.35, Side::Put}, +1.0, 100.0};
  auto pf = Portfolio::create(std::array<Position, 1>{p});
  ASSERT_TRUE(pf.has_value());
  PortfolioPricer pricer(std::move(*pf));

  using GN = PricedSurface::GreekNeeds;
  const auto count = [&](GN needs) -> std::pair<std::uint64_t, std::uint64_t> {
    PriceOptions opts;
    opts.n_threads = 1u;
    opts.analytic_greeks = true;
    opts.greek_needs = needs;
    PortfolioWorkspace ws;
    ws.reserve(1, 1);
    led::reset();
    const auto t = pricer.price_totals(*set, PriceFieldMask::FullGreeks, ws, opts);
    EXPECT_TRUE(t.has_value()) << (t.has_value() ? std::string{} : t.error().to_string());
    const led::Counts c = led::snapshot();
    return {al(c), analytic(c)};
  };

  const auto [al_full, an_full] = count(GN{});                        // 5
  const auto [al_risk, an_risk] = count(GN{true, false, false});     // 3
  const auto [al_hedge, an_hedge] = count(GN{false, false, false});  // 1

  EXPECT_EQ(an_full, 1u) << "one analytic bundle per unique (the AL route fired)";
  EXPECT_EQ(an_risk, 1u);
  EXPECT_EQ(an_hedge, 1u);
  EXPECT_EQ(al_full, 5u) << "full bundle = base + sigma+/- + r+/- = 5 boundary solves";
  EXPECT_EQ(al_risk, 3u) << "risk {delta,vega} = base + sigma+/- = 3 (r+/- skipped)";
  EXPECT_EQ(al_hedge, 1u) << "hedge {delta} = base only = 1 (sigma+/-, r+/- skipped)";
  std::printf("[btexec] L4 per-role bundle solves/unique: full=%llu risk=%llu hedge=%llu\n",
              static_cast<unsigned long long>(al_full), static_cast<unsigned long long>(al_risk),
              static_cast<unsigned long long>(al_hedge));
}

// (3) Stamp-guard safety (correctness). A P&L Taylor decomposition reads ALL EIGHT base
// greeks, so a base bundle computed under a REDUCED greek_needs (missing rho/charm) must
// NEVER be reused for it. The guard (`base_greek_needs.full()`) forces a fresh full
// solve; a FULL base is still reused (no fresh bundle). This is L4's "stamp support so
// L1 composes": the narrowed frame cannot silently corrupt a downstream P&L.
TEST(BacktestExec, L4NarrowedBaseNeverReusedByPnlStamp) {
  const std::int64_t ts = kBaseNow;
  const PricedSurface s_base = make_surface(kUid, 100.0, 100.0, ts, 0.000);
  const PricedSurface s_shift = make_surface(kUid, 102.0, 102.0, ts, 0.010);  // spot+vol moved
  const auto make_set = [](const PricedSurface& s) {
    const PricedSurface* ptrs[] = {&s};
    auto set = SurfaceSet::create(std::span<const PricedSurface* const>{ptrs});
    EXPECT_TRUE(set.has_value());
    return std::move(*set);
  };
  const SurfaceSet set_base = make_set(s_base);
  const SurfaceSet set_shift = make_set(s_shift);
  const Position pa{1, OptionContract{kUid, 100.0, 0.35, Side::Put}, +1.0, 100.0};

  using GN = PricedSurface::GreekNeeds;
  PriceOptions opts_full;
  opts_full.n_threads = 1u;
  opts_full.analytic_greeks = true;  // greek_needs default {} = full
  PriceOptions opts_risk = opts_full;
  opts_risk.greek_needs = GN{true, false, false};  // narrowed base (rho/charm dropped)

  // Cold full reference.
  PortfolioPricer cold(std::move(*Portfolio::create(std::array<Position, 1>{pa})));
  PortfolioWorkspace ws_cold;
  ws_cold.reserve(1, 1);
  const auto ref = cold.pnl_totals(set_base, set_shift, ws_cold, opts_full);
  ASSERT_TRUE(ref.has_value()) << ref.error().to_string();

  // (a) Narrowed base -> pnl MUST refuse reuse (guard) and re-solve full.
  PortfolioPricer p_narrow(std::move(*Portfolio::create(std::array<Position, 1>{pa})));
  PortfolioWorkspace ws;
  ws.reserve(1, 1);
  ASSERT_TRUE(p_narrow.price_totals(set_base, PriceFieldMask::FullGreeks, ws, opts_risk).has_value());
  led::reset();
  const auto got = p_narrow.pnl_totals(set_base, set_shift, ws, opts_full);
  ASSERT_TRUE(got.has_value()) << got.error().to_string();
  EXPECT_GE(analytic(led::snapshot()), 1u)
      << "L4 STAMP GUARD FAILED: a narrowed (risk-role) base bundle was reused for a full "
         "P&L attribution — pnl_rho/pnl_charm would be silently wrong. STOP + report.";
  EXPECT_TRUE(bits_equal(got->pnl_total, ref->pnl_total)) << "guard re-solve != cold full pnl";
  EXPECT_TRUE(bits_equal(got->pv_base, ref->pv_base));
  EXPECT_TRUE(bits_equal(got->pnl_vega, ref->pnl_vega));

  // (b) Positive control: a FULL base IS reused (guard allows it) — no fresh bundle.
  PortfolioPricer p_full(std::move(*Portfolio::create(std::array<Position, 1>{pa})));
  PortfolioWorkspace ws2;
  ws2.reserve(1, 1);
  ASSERT_TRUE(p_full.price_totals(set_base, PriceFieldMask::FullGreeks, ws2, opts_full).has_value());
  led::reset();
  const auto reuse = p_full.pnl_totals(set_base, set_shift, ws2, opts_full);
  ASSERT_TRUE(reuse.has_value()) << reuse.error().to_string();
  EXPECT_EQ(analytic(led::snapshot()), 0u)
      << "a FULL base bundle should be reused by pnl (no fresh analytic bundle)";
  EXPECT_TRUE(bits_equal(reuse->pnl_total, ref->pnl_total));
  std::printf("[btexec] L4 stamp guard: narrowed base re-solved (analytic>=1), full base reused "
              "(analytic==0); both pnl bit-match the cold full reference\n");
}

// ── B1: SpreadKind::QuoteSide fills ──────────────────────────────────────────
//
// (a) a recorded quote crosses at mid ± f·half-spread, f picked by the
//     committed cohort's leg count (single vs. complex); (b) an absent quote
//     falls back to the modeled PriceBps half-spread; (c) every
//     `BacktestResult` carries its `friction_regime`.

TEST(BacktestExec, QuoteSideFillCrossesRecordedSpread) {
  auto fx = ExecFixture::listed_quotes(/*bid=*/1.00, /*ask=*/1.10);
  RunConfig cfg = fx.config();
  cfg.frictions.spread_kind = FrictionModel::SpreadKind::QuoteSide;

  auto clock = Clock::from_manifest(fx.corpus().manifest);
  ASSERT_TRUE(clock.has_value()) << clock.error().to_string();
  DeclarativeStrategy strat{single_clip(kUid, 0.5, Side::Put,
                                       StrikeSelector{StrikeSelector::Kind::AtmForward, 0.0})};
  auto r = run_backtest(*clock, strat, cfg);
  ASSERT_TRUE(r.has_value()) << r.error().to_string();

  // buy fills at 1.05 + 0.75*0.05 = 1.0875 (single leg -> crossing_fraction_single).
  EXPECT_NEAR(1.0875, ExecFixture::entry_fill_price(*r), 1e-9);
  EXPECT_EQ(FrictionRegime::QuoteSide, r->friction_regime);
}

TEST(BacktestExec, QuoteSideFillUsesComplexFractionForMultiLegCohort) {
  auto fx = ExecFixture::listed_quotes(/*bid=*/1.00, /*ask=*/1.10);
  RunConfig cfg = fx.config();
  cfg.frictions.spread_kind = FrictionModel::SpreadKind::QuoteSide;

  auto clock = Clock::from_manifest(fx.corpus().manifest);
  ASSERT_TRUE(clock.has_value()) << clock.error().to_string();
  // A straddle: call+put share ONE cohort, so both legs price with
  // crossing_fraction_complex, not crossing_fraction_single.
  DeclarativeStrategy strat{single_clip_straddle(
      kUid, 0.5, StrikeSelector{StrikeSelector::Kind::AtmForward, 0.0})};
  auto r = run_backtest(*clock, strat, cfg);
  ASSERT_TRUE(r.has_value()) << r.error().to_string();

  // buy fills at 1.05 + 0.53*0.05 = 1.0765 (complex, both legs identical).
  EXPECT_NEAR(1.0765, ExecFixture::entry_fill_price(*r, /*n_legs=*/2), 1e-9);
}

TEST(BacktestExec, QuoteSideSellFillsBelowMid) {
  auto fx = ExecFixture::listed_quotes(/*bid=*/1.00, /*ask=*/1.10);
  RunConfig cfg = fx.config();
  cfg.frictions.spread_kind = FrictionModel::SpreadKind::QuoteSide;

  auto clock = Clock::from_manifest(fx.corpus().manifest);
  ASSERT_TRUE(clock.has_value()) << clock.error().to_string();
  DeclarativeStrategy strat{single_clip(kUid, 0.5, Side::Put,
                                       StrikeSelector{StrikeSelector::Kind::AtmForward, 0.0},
                                       /*hedge=*/HedgeSpec{}, /*sign=*/-1.0)};
  auto r = run_backtest(*clock, strat, cfg);
  ASSERT_TRUE(r.has_value()) << r.error().to_string();

  // A short entry SELLS: fills at 1.05 - 0.75*0.05 = 1.0125. qty<0 flips the
  // cash-recovery sign from ExecFixture::entry_fill_price's (qty>0 buy)
  // convention, so this test recovers it directly.
  EXPECT_NEAR(1.0125, r->cash.front() / 100.0, 1e-9);
}

TEST(BacktestExec, QuoteSideRollCloseCrossesRecordedSpread) {
  auto fx = ExecFixture::listed_quotes(/*bid=*/1.00, /*ask=*/1.10);
  RunConfig cfg = fx.config();
  cfg.frictions.spread_kind = FrictionModel::SpreadKind::QuoteSide;

  OpenThenCloseStrategy strat(kUid, 0.5, Side::Put, /*sign=*/+1.0);
  auto clock = Clock::from_manifest(fx.corpus().manifest);
  ASSERT_TRUE(clock.has_value()) << clock.error().to_string();
  auto r = run_backtest(*clock, strat, cfg);
  ASSERT_TRUE(r.has_value()) << r.error().to_string();
  ASSERT_GE(r->cash.size(), 2u);

  // Row 0 opens (buy, entry loop, crosses UP); row 1 closes with no
  // re-entry (roll-close loop only) -- closing a long put SELLS it, crossing
  // DOWN: 1.05 - 0.75*0.05 = 1.0125.
  const double close_proceeds = r->cash[1] - r->cash[0];
  EXPECT_NEAR(1.0125, close_proceeds / 100.0, 1e-9);
}

TEST(BacktestExec, QuoteSideFallsBackToModeledPriceBpsHalfSpreadWhenQuoteAbsent) {
  const fs::path dir = fresh_dir("quoteside-fallback");
  const Corpus c = make_corpus(dir, "SPX", 2);
  auto clock = Clock::from_manifest(c.manifest);
  ASSERT_TRUE(clock.has_value()) << clock.error().to_string();
  const StrategySpec spec = single_clip(kUid, 0.5, Side::Put,
                                        StrikeSelector{StrikeSelector::Kind::AtmForward, 0.0});

  // PriceBps: an established, well-tested modeled half-spread charged ON TOP
  // of a model-mid fill.
  RunConfig price_bps_cfg;
  price_bps_cfg.frictions.spread_kind = FrictionModel::SpreadKind::PriceBps;
  price_bps_cfg.frictions.half_spread_bps = 40.0;
  DeclarativeStrategy strat_bps{spec};
  auto r_bps = run_backtest(*clock, strat_bps, price_bps_cfg);
  ASSERT_TRUE(r_bps.has_value()) << r_bps.error().to_string();

  // QuoteSide with NO quote_lookup wired and crossing_fraction_single=1.0 (a
  // full cross) must reduce to EXACTLY the same fill/cost/nav as PriceBps
  // above: `mid + 1.0*(mid*bps/1e4)` is the identical formula, just reached
  // via the fallback branch instead of the additive half_spread() lane.
  RunConfig quote_cfg;
  quote_cfg.frictions.spread_kind = FrictionModel::SpreadKind::QuoteSide;
  quote_cfg.frictions.half_spread_bps = 40.0;
  quote_cfg.frictions.crossing_fraction_single = 1.0;
  // quote_lookup left unset -> every fill falls back to the modeled half-spread.
  DeclarativeStrategy strat_quote{spec};
  auto r_quote = run_backtest(*clock, strat_quote, quote_cfg);
  ASSERT_TRUE(r_quote.has_value()) << r_quote.error().to_string();

  EXPECT_NEAR(r_bps->cost.front(), r_quote->cost.front(), 1e-6);
  EXPECT_NEAR(r_bps->nav.front(), r_quote->nav.front(), 1e-6);
  EXPECT_NE(r_bps->cost.front(), 0.0) << "the fixture must actually charge something";
  EXPECT_EQ(FrictionRegime::Modeled, r_bps->friction_regime);
  EXPECT_EQ(FrictionRegime::QuoteSide, r_quote->friction_regime);
}

TEST(BacktestExec, FrictionRegimeClassifiesFrictionlessModeledAndQuoteSide) {
  const fs::path dir = fresh_dir("frictionregime");
  const Corpus c = make_corpus(dir, "SPX", 2);
  auto clock = Clock::from_manifest(c.manifest);
  ASSERT_TRUE(clock.has_value()) << clock.error().to_string();
  const StrategySpec spec = single_clip(kUid, 0.5, Side::Put,
                                        StrikeSelector{StrikeSelector::Kind::AtmForward, 0.0});

  {
    DeclarativeStrategy strat{spec};
    RunConfig cfg;  // default: SpreadKind::None, every other cost knob 0
    auto r = run_backtest(*clock, strat, cfg);
    ASSERT_TRUE(r.has_value()) << r.error().to_string();
    EXPECT_EQ(FrictionRegime::Frictionless, r->friction_regime);
  }
  {
    DeclarativeStrategy strat{spec};
    RunConfig cfg;
    cfg.frictions.spread_kind = FrictionModel::SpreadKind::PriceBps;
    cfg.frictions.half_spread_bps = 25.0;
    auto r = run_backtest(*clock, strat, cfg);
    ASSERT_TRUE(r.has_value()) << r.error().to_string();
    EXPECT_EQ(FrictionRegime::Modeled, r->friction_regime);
  }
  {
    DeclarativeStrategy strat{spec};
    RunConfig cfg;
    cfg.frictions.spread_kind = FrictionModel::SpreadKind::QuoteSide;
    auto r = run_backtest(*clock, strat, cfg);
    ASSERT_TRUE(r.has_value()) << r.error().to_string();
    EXPECT_EQ(FrictionRegime::QuoteSide, r->friction_regime);
  }
}

// Invariant I3: a frictionless replay (SpreadKind::None) must stay
// bit-identical to a run that never heard of QuoteSide. Reuses the existing
// ZeroFrictionIdentity fixture pattern (default vs. explicit-zero RunConfig)
// but additionally proves the NEW crossing-fraction fields are inert at their
// ORATS defaults under None -- widening FrictionModel must not move a single
// bit of the frictionless golden.
TEST(BacktestExec, QuoteSideFieldsAreInertUnderNoneSpreadKind) {
  const fs::path dir = fresh_dir("quoteside-inert");
  const Corpus c = make_corpus(dir, "SPX", 6);
  auto clock = Clock::from_manifest(c.manifest);
  ASSERT_TRUE(clock.has_value()) << clock.error().to_string();
  const StrategySpec spec = single_clip(kUid, 0.25, Side::Put,
                                        StrikeSelector{StrikeSelector::Kind::AtmForward, 0.0});

  DeclarativeStrategy s_default{spec};
  DeclarativeStrategy s_new_fields{spec};

  RunConfig def{};  // SpreadKind::None (the default) -- crossing fractions untouched
  RunConfig with_fields{};
  with_fields.frictions.crossing_fraction_single = 0.11;   // != the 0.75 default
  with_fields.frictions.crossing_fraction_complex = 0.22;  // != the 0.53 default
  with_fields.frictions.quote_lookup = [](const OptionContract&) {
    return std::optional<FrictionModel::RawQuote>{FrictionModel::RawQuote{1.0, 2.0}};
  };

  auto rd = run_backtest(*clock, s_default, def);
  auto rf = run_backtest(*clock, s_new_fields, with_fields);
  ASSERT_TRUE(rd.has_value()) << rd.error().to_string();
  ASSERT_TRUE(rf.has_value()) << rf.error().to_string();
  const BacktestResult& a = *rd;
  const BacktestResult& b = *rf;
  ASSERT_EQ(a.size(), b.size());
  ASSERT_GT(a.size(), 1u);
  for (std::size_t i = 0; i < a.size(); ++i) {
    EXPECT_TRUE(bits_equal(a.nav[i], b.nav[i])) << "nav row " << i;
    EXPECT_TRUE(bits_equal(a.cash[i], b.cash[i])) << "cash row " << i;
    EXPECT_TRUE(bits_equal(a.cost[i], b.cost[i])) << "cost row " << i;
  }
  EXPECT_EQ(FrictionRegime::Frictionless, a.friction_regime);
  EXPECT_EQ(FrictionRegime::Frictionless, b.friction_regime);
}
