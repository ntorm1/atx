// atx-vol solve-ledger gate tests (WS-V V1) — the deterministic, always-on
// solve-economy baseline the whole solve-wall sprint gates against.
//
// The ledger (`atx::vol::counters::ledger`) is a SECOND counter plane, distinct
// from the gated `ATX_VOL_COUNT*` exact counters: it is compiled into EVERY build
// (Debug / rel / rel-avx2), so these gates run on the shipping binaries L1/L2/C1
// will move — no special `-DATX_VOL_COUNTERS` build required. Its increments are
// per-thread relaxed load/stores merged only at `snapshot()`, so counting a solve
// never perturbs the timing of the solve it counts.
//
// What this file pins (§2 cost model of the solve-wall sprint):
//   * No-churn day  = 6 AL solve-equivs / unique  (0 pnl-base + 1 target mark
//                     + 5 execute/book-greeks bundle) — the base-risk stamp survives.
//   * Expiry day    = 11 AL solve-equivs / unique  (5 pnl-base + 1 mark + 5 bundle)
//                     — a membership change (settlement shrinks the alive set)
//                     kills the stamp, so the pnl-base bundle re-solves.
//   * Duplicate marks: the per-step target/settlement Marks solves L2 drives to 0.
//
// These EXACT numbers are the regression baseline: L1 moves 11 -> <=6 (pnl-base
// reuse across the membership change), L2 drives the duplicate marks to 0. A later
// test re-asserts these same counters with the improved expected values.
//
// The scenarios are the smallest faithful realizations of the model. A SINGLE
// surviving unit (the fixed-book survivor "A") pays 11 on the one expiry step and
// 6 on the following no-churn step of the SAME run, so both headline numbers are
// pinned against identical risk with only the stamp state changed between them.

#include <gtest/gtest.h>

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "atx/vol/american.hpp"         // al_fast_opts, AmericanMethod, american_price
#include "atx/vol/american_iv.hpp"      // american_implied_vol (iv-newton tap)
#include "atx/vol/backtest.hpp"         // Clock, run_backtest, RunConfig, Lot, PortfolioState
#include "atx/vol/corpus.hpp"           // CorpusManifest, CorpusEntry, CorpusFitStatus
#include "atx/vol/counters.hpp"         // counters::ledger — the facility under test
#include "atx/vol/portfolio_pricer.hpp" // OptionContract, kNsPerYear
#include "atx/vol/priced_surface.hpp"   // PricedSurface, PricingContext
#include "atx/vol/strategy.hpp"         // DeclarativeStrategy, StrategySpec, StrikeSelector
#include "atx/vol/surface_archive.hpp"  // write_surface_archive_v2_file, SurfaceArchiveItem
#include "atx/vol/surface_parity.hpp"   // SliceContext
#include "atx/vol/types.hpp"            // Side, Result, Status
#include "atx/vol/vol_curve.hpp"        // CurveSurface, EssviCurve
#include "atx/vol/vol_surface.hpp"      // EssviParams

using namespace atx::vol;
namespace fs = std::filesystem;
namespace led = atx::vol::counters::ledger;

namespace {

constexpr double kR = 0.043;
constexpr std::int64_t kBaseNow = 1700000000000000000LL;
constexpr std::int64_t kDayNs = 86400LL * 1000000000LL;
constexpr std::uint32_t kUid = 7;

// One synthetic eSSVI PricedSurface (mirrors backtest_exec_test.cpp make_surface):
// flat forward, genuine American premium via q_eff=0.02, slices T in [0.05, 1.0].
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

[[nodiscard]] fs::path fresh_dir(const char *tag) {
  const fs::path dir = fs::temp_directory_path() / (std::string("atx-solveledger-") + tag);
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

[[nodiscard]] CorpusManifest make_manifest(
    const std::vector<std::pair<std::string, std::string>> &date_paths, const std::string &symbol) {
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

// A corpus at explicit day offsets from kBaseNow. The date-string labels are
// decorative (ascending); the settlement contract binds on the SURFACE timestamp
// (kBaseNow + offset*kDayNs), so an expiring lot's expiry_ts_ns must equal one of
// these offsets exactly. Spot/vol drift a touch each date so marks actually move.
[[nodiscard]] Clock make_clock(const fs::path &dir, const std::string &symbol,
                               const std::vector<int> &offset_days) {
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

// A single-clip declarative strategy: opens ONE structure at inception and holds it
// (EveryNDays with a cadence far longer than the corpus => opens once). No churn.
[[nodiscard]] StrategySpec single_clip(std::uint32_t uid, double target_T, Side side,
                                       StrikeSelector strike) {
  StrategySpec spec;
  spec.name = "solve-ledger-single-clip";
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
  spec.lifecycle.entry_every_n = 100000; // >> corpus => opens once, at inception
  return spec;
}

[[nodiscard]] RunConfig deterministic_config() {
  RunConfig config;
  config.price.n_threads = 1u;    // boundary-solve counts are thread-invariant; 1 removes
  config.prefetch_snapshots = false; // worker-launch / async-prefetch nondeterminism.
  return config;
}

// Analytic AL greeks bundle = 5 boundary solves; a Marks/target price = 1. So the
// boundary-solve total decomposes as 5*analytic_bundles + 1*(marks) (+ 7*fd, +adjoint).
constexpr std::uint64_t kAnalyticBundleSolves = 5;

[[nodiscard]] std::uint64_t al(const led::Counts &c) noexcept {
  return c.get(led::Solve::AlBoundarySolves);
}
[[nodiscard]] std::uint64_t analytic(const led::Counts &c) noexcept {
  return c.get(led::Solve::GreeksBundlesAnalytic);
}
[[nodiscard]] std::uint64_t fd(const led::Counts &c) noexcept {
  return c.get(led::Solve::GreeksBundlesFd);
}
// Marks/target Price solves implied by the decomposition (each is 1 boundary solve).
[[nodiscard]] std::uint64_t marks(const led::Counts &c) noexcept {
  return al(c) - kAnalyticBundleSolves * analytic(c) - 7u * fd(c) -
         c.get(led::Solve::GreeksBundlesAdjoint);
}

}  // namespace

// ── 0. The ledger facility itself is always-on and contention-free ───────────
TEST(SolveLedger, AlwaysOnAndMergesAcrossThreads) {
  // The ledger compiles into every build; unlike the gated ATX_VOL_COUNT plane it
  // is NOT behind counters_enabled().
  led::reset();
  led::Counts start = led::snapshot();
  EXPECT_EQ(al(start), 0u);

  led::bump(led::Solve::AlBoundarySolves, 3u);
  led::bump(led::Solve::GreeksBundlesAnalytic);
  led::bump(led::Solve::IvNewtonIters, 10u);
  const led::Counts after = led::snapshot();
  EXPECT_EQ(after.get(led::Solve::AlBoundarySolves), 3u);
  EXPECT_EQ(after.get(led::Solve::GreeksBundlesAnalytic), 1u);
  EXPECT_EQ(after.get(led::Solve::IvNewtonIters), 10u);

  // operator- is a saturating per-counter delta.
  const led::Counts d = after - start;
  EXPECT_EQ(d.get(led::Solve::AlBoundarySolves), 3u);

  // A merge across a worker thread: the worker's tally survives its own exit.
  std::thread worker([] { led::bump(led::Solve::AlBoundarySolves, 100u); });
  worker.join();
  EXPECT_EQ(led::snapshot().get(led::Solve::AlBoundarySolves), 103u);

  led::reset();
  EXPECT_EQ(led::snapshot().get(led::Solve::AlBoundarySolves), 0u);
}

// ── 1. iv_newton_iters fires on an American-IV inversion ─────────────────────
TEST(SolveLedger, IvNewtonItersCountsInversionResiduals) {
  const double S = 100.0;
  const double K = 100.0;
  const double T = 0.5;
  const double q = 0.02;
  const double sigma_true = 0.25;
  const Result<double> price = american_price(S, K, T, sigma_true, kR, q, Side::Put,
                                              AmericanMethod::AndersenLake, al_fast_opts());
  ASSERT_TRUE(price.has_value()) << price.error().to_string();

  led::reset();
  const Result<double> iv = american_implied_vol(*price, S, K, T, kR, q, Side::Put);
  ASSERT_TRUE(iv.has_value()) << iv.error().to_string();
  EXPECT_NEAR(*iv, sigma_true, 1e-4);
  // Every bracket/Newton residual evaluation ticks the ledger. A converged
  // inversion of a genuinely American put takes several.
  EXPECT_GT(led::snapshot().get(led::Solve::IvNewtonIters), 1u);
}

// ── 2. No-churn day = 6 solve-equivs / unique ────────────────────────────────
//
// single_clip opens one ATM put at inception and holds it. Every later step the
// book is unchanged, so compute_step's pnl-base reuses the previous step's stamped
// risk (0 bundles); the only work is the 1 target Marks solve + the 5-solve
// book-greeks bundle = 6. Pinned per-step via an armed StepTrace.
TEST(SolveLedger, NoChurnDayIsSixSolvesPerUnit) {
  const fs::path dir = fresh_dir("no-churn");
  const Clock clock = make_clock(dir, "SPX", {0, 1, 2, 3, 4});

  DeclarativeStrategy strategy{
      single_clip(kUid, 0.25, Side::Put, StrikeSelector{StrikeSelector::Kind::AtmForward, 0.0})};
  RunConfig config = deterministic_config();

  led::reset();
  led::StepTrace trace;
  const auto result = run_backtest(clock, strategy, config);
  ASSERT_TRUE(result.has_value()) << result.error().to_string();
  ASSERT_EQ(result->size(), 5u);

  // One StepScope per loop iteration i=1..4 (inception row 0 is not a step scope).
  ASSERT_EQ(trace.size(), 4u);
  for (std::size_t k = 0; k < trace.size(); ++k) {
    const led::Counts &step = trace.steps()[k];
    EXPECT_EQ(al(step), 6u) << "no-churn step " << k << " should be 6 solves/unit";
    EXPECT_EQ(analytic(step), 1u) << "step " << k << ": pnl-base reused, only the book bundle";
    EXPECT_EQ(fd(step), 0u) << "backtest default is analytic greeks";
    EXPECT_EQ(marks(step), 1u) << "step " << k << ": exactly one (duplicate) target mark";
  }
}

// ── 3. Expiry day = 11 solve-equivs / unique (stamp death re-solves pnl-base) ─
//
// Fixed book: a far-dated survivor A (put) + a mid-run expiring B (call). On the
// step where B settles, the alive set shrinks {A,B} -> {A}, so RetainedBookPricer
// mints a fresh pricer id and the base-risk stamp dies: A's pnl-base bundle
// re-solves. The SAME unit A then pays only 6 on the very next (no-churn) step, so
// both headline numbers are pinned against identical risk.
TEST(SolveLedger, ExpiryDayReSolvesPnlBaseStampElevenPerUnit) {
  const fs::path dir = fresh_dir("expiry-day");
  // 20-day spacing keeps every residual T >= the surface's 0.05 min slice.
  const Clock clock = make_clock(dir, "SPX", {0, 20, 40, 60, 80});
  const std::int64_t exp_B = kBaseNow + 60 * kDayNs;  // settles exactly on date index 3
  const std::int64_t exp_A = kBaseNow + 200 * kDayNs; // survives the whole run

  PortfolioState book;
  book.lots.push_back(Lot{1, OptionContract{kUid, 100.0, 0.0, Side::Put}, +1.0, 100.0, exp_A, 0, 0.0});
  book.lots.push_back(Lot{2, OptionContract{kUid, 95.0, 0.0, Side::Call}, +1.0, 100.0, exp_B, 0, 0.0});

  RunConfig config = deterministic_config();

  led::reset();
  led::StepTrace trace;
  const auto result = run_backtest(clock, std::move(book), config);
  ASSERT_TRUE(result.has_value()) << result.error().to_string();
  ASSERT_EQ(result->size(), 5u);
  ASSERT_EQ(trace.size(), 4u); // steps i=1..4

  const led::Counts &warm1 = trace.steps()[0];   // i=1: both alive, no expiry
  const led::Counts &warm2 = trace.steps()[1];   // i=2: both alive, no expiry
  const led::Counts &expiry = trace.steps()[2];  // i=3: B settles, A survives (stamp death)
  const led::Counts &nochurn = trace.steps()[3]; // i=4: A only, stamp survives

  // Two-unit warm steps: pnl-base reused (0 bundles), 2 book-greeks bundles + 2
  // target marks = 12. No stamp death yet.
  EXPECT_EQ(al(warm1), 12u);
  EXPECT_EQ(analytic(warm1), 2u);
  EXPECT_EQ(al(warm2), 12u);
  EXPECT_EQ(analytic(warm2), 2u);

  // No-churn step for the lone survivor A: 6 = 0 pnl-base + 1 target + 5 bundle.
  EXPECT_EQ(al(nochurn), 6u) << "survivor A, stamp intact => no-churn 6/unit";
  EXPECT_EQ(analytic(nochurn), 1u);
  EXPECT_EQ(marks(nochurn), 1u);

  // Expiry step: the alive set is the SAME lone unit A, but the stamp died, so A's
  // pnl-base bundle re-solves. A pays 11 (5 pnl-base + 1 target + 5 book-greeks);
  // the expiring B adds one settlement Marks solve => 12 total, 2 analytic bundles.
  EXPECT_EQ(analytic(expiry), 2u) << "pnl-base re-solve + book bundle for A";
  EXPECT_EQ(al(expiry), 12u) << "A's 11 + B's 1 settlement mark";

  // The A-attributable expiry-day cost is exactly 11 (total minus B's settlement),
  // and it exceeds the no-churn 6 by precisely one analytic bundle (5) — the L1
  // pnl-base re-solve the sprint removes (target: 11 -> <=6).
  const std::uint64_t a_expiry_cost = al(expiry) - 1u; // subtract B's settlement mark
  EXPECT_EQ(a_expiry_cost, 11u) << "expiry-day steady state = 11 solve-equivs/unit";
  EXPECT_EQ(a_expiry_cost - al(nochurn), kAnalyticBundleSolves)
      << "expiry-day pays exactly one extra pnl-base bundle vs no-churn";
  EXPECT_EQ(analytic(expiry) - analytic(nochurn), 1u);

  // Duplicate marks (L2 target): every step pays a target/settlement Marks solve
  // that a per-(contract,date) memo would eliminate. Warm steps: 2 targets; the
  // no-churn step: 1 target; the expiry step: 1 target (A) + 1 settlement (B) = 2.
  EXPECT_EQ(marks(warm1), 2u);
  EXPECT_EQ(marks(expiry), 2u);

  std::printf("[solve-ledger] expiry-day per-step AL solves = [%llu, %llu, %llu, %llu] "
              "(warm,warm,expiry,no-churn); analytic bundles = [%llu, %llu, %llu, %llu]\n",
              static_cast<unsigned long long>(al(warm1)), static_cast<unsigned long long>(al(warm2)),
              static_cast<unsigned long long>(al(expiry)),
              static_cast<unsigned long long>(al(nochurn)),
              static_cast<unsigned long long>(analytic(warm1)),
              static_cast<unsigned long long>(analytic(warm2)),
              static_cast<unsigned long long>(analytic(expiry)),
              static_cast<unsigned long long>(analytic(nochurn)));
}

// ── 4. The always-on ledger agrees with the gated exact counter ──────────────
// When built with -DATX_VOL_COUNTERS the two boundary-solve counters must match
// exactly (same tap site), proving the always-on ledger is faithful.
TEST(SolveLedger, AlwaysOnLedgerMatchesGatedBoundarySolveCounter) {
  if constexpr (!counters::counters_enabled()) {
    GTEST_SKIP() << "gated exact counters are OFF in this build";
  }
  const fs::path dir = fresh_dir("ledger-vs-gated");
  const Clock clock = make_clock(dir, "SPX", {0, 1, 2, 3, 4});
  DeclarativeStrategy strategy{
      single_clip(kUid, 0.25, Side::Put, StrikeSelector{StrikeSelector::Kind::AtmForward, 0.0})};
  RunConfig config = deterministic_config();

  counters::reset();
  led::reset();
  const auto result = run_backtest(clock, strategy, config);
  ASSERT_TRUE(result.has_value()) << result.error().to_string();
  EXPECT_EQ(led::snapshot().get(led::Solve::AlBoundarySolves),
            counters::snapshot().get(counters::Counter::BoundarySolves));
}
