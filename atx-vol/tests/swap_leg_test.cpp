// atx-vol swap-leg toolkit — module gates.
//
// The reusable pieces every swap-carrying strategy shares, extracted from the
// strangle-varswap comparison strategy:
//
//   1. ContractForLotCarriesResidualTenorAndStagedRv — the SwapLot ->
//      DerivContract transcription is the engine's own convention: residual
//      tenor over kNsPerYear, the caller's accrual staged into rv_spec.
//   2. ProbeReportsNaNBeforeAnyStepAndOnWrongSnapshot — the signal probe's NaN
//      discipline: nothing measured is NaN, never 0.0, and a snapshot other
//      than the as-of one measures nothing.
//   3. ProbeSeedsOneStepLateAndMarksRestoredLotsDesynced — the engine-accrual
//      mirror seeds on the step AFTER first sight (the engine's swap pass sees
//      a new lot one step late) and refuses to fabricate an accrual for a lot
//      it never saw open (a checkpoint restore): desynced forever, NaN forever.
//
// Fixture plumbing (synthetic eSSVI surfaces written as one-symbol archives)
// mirrors backtest_swap_test.cpp.

#include <gtest/gtest.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "atx/core/error.hpp"                  // ErrorCode
#include "atx/vol/api/pricing/american.hpp"                // al_fast_opts, AmericanMethod
#include "atx/vol/api/backtest/backtest.hpp"                // SwapLot, PortfolioState, MarketSnapshot
#include "atx/vol/api/pricing/derivatives.hpp"             // DerivContract, DerivKind, RealizedVarianceSpec
#include "pricing/deriv_ref_bridge.hpp" // detail::deriv_price_on_ref, deriv_greeks_on_ref
#include "atx/vol/api/backtest/portfolio_pricer.hpp" // kNsPerYear
#include "atx/vol/api/backtest/priced_surface.hpp"   // PricedSurface, PricingContext
#include "atx/vol/api/storage/surface_archive.hpp"  // write_surface_archive_v2_file
#include "atx/vol/api/fitting/surface_parity.hpp"   // SliceContext
#include "atx/vol/api/pricing/swap_leg.hpp"         // swap_contract_for_lot, SwapSignalProbe
#include "atx/vol/api/core/types.hpp"            // Result, Status
#include "atx/vol/api/fitting/vol_curve.hpp"        // CurveSurface, EssviCurve
#include "atx/vol/api/fitting/vol_surface.hpp"      // EssviParams

using namespace atx::vol;
namespace fs = std::filesystem;

namespace {

constexpr double kR = 0.043;
constexpr std::int64_t kBaseNow = 1700000000000000000LL;
constexpr std::int64_t kDayNs = 86400LL * 1000000000LL;
constexpr std::int64_t kStepNs = 30LL * kDayNs;
constexpr std::uint32_t kUid = 11;
constexpr const char *kSymbol = "XOM";
constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

// Synthetic eSSVI PricedSurface, the backtest_swap_test recipe verbatim.
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

// One-symbol archive on disk -> a loadable MarketSnapshot for the probe tests.
[[nodiscard]] std::string write_one(const fs::path &dir, const std::string &date,
                                    const PricedSurface &s) {
  std::error_code ec;
  fs::create_directories(dir, ec);
  const std::string path = (dir / (date + ".atxvsa")).string();
  const SurfaceArchiveItem item{kSymbol, &s};
  const std::span<const SurfaceArchiveItem> items(&item, 1);
  const Status st = write_surface_archive_v2_file(path, items);
  EXPECT_TRUE(st.has_value()) << (st.has_value() ? std::string{} : st.error().to_string());
  return path;
}

[[nodiscard]] fs::path fresh_dir(const char *tag) {
  const fs::path dir = fs::temp_directory_path() / (std::string("atx-swapleg-") + tag);
  std::error_code ec;
  fs::remove_all(dir, ec);
  return dir;
}

[[nodiscard]] SwapLot make_lot(std::uint64_t id, std::int64_t open_ts, std::int64_t expiry_ts) {
  SwapLot lot;
  lot.id = id;
  lot.uid = kUid;
  lot.kind = DerivKind::VarSwap;
  lot.strike_dec = 0.04;
  lot.cap_dec = 0.0;
  lot.notional = 1.0;
  lot.qty = 2.0;
  lot.start_ts_ns = open_ts;
  lot.expiry_ts_ns = expiry_ts;
  lot.n_obs_total = 5;
  lot.annualization = 252.0;
  return lot;
}

// The probe's five columns, by name, off a fresh signal vector.
struct Five {
  double delta{kNaN};
  double gamma{kNaN};
  double vega{kNaN};
  double theta{kNaN};
  double rho{kNaN};
  bool complete{false};
};

[[nodiscard]] Five five_of(const SwapSignalProbe &probe, const MarketSnapshot &snap) {
  std::vector<std::pair<std::string, double>> out;
  probe.append_swap_greek_signals(snap, out);
  Five f;
  std::size_t seen = 0;
  for (const auto &[name, value] : out) {
    if (name == "swap_delta") {
      f.delta = value;
      ++seen;
    } else if (name == "swap_gamma") {
      f.gamma = value;
      ++seen;
    } else if (name == "swap_vega") {
      f.vega = value;
      ++seen;
    } else if (name == "swap_theta") {
      f.theta = value;
      ++seen;
    } else if (name == "swap_rho") {
      f.rho = value;
      ++seen;
    }
  }
  f.complete = seen == 5 && out.size() == 5;
  return f;
}

// ── solve_cycle_swap gates ──────────────────────────────────────────────────

// A 5-session grid: open at sessions[0], expiry at sessions[4] leaves 4
// sessions in (open, expiry], and the first of those merely SEEDS the engine's
// fixing series — so the contract observes 3 returns.
[[nodiscard]] CycleSwapRequest make_request(std::span<const std::int64_t> sessions) {
  CycleSwapRequest req;
  req.uid = kUid;
  req.kind = DerivKind::VarSwap;
  req.cap_dec = 0.0;
  req.notional = 1.0;
  req.annualization = 252.0;
  req.open_ts_ns = sessions.front();
  req.expiry_ts_ns = sessions.back();
  req.session_ts = sessions;
  req.deriv_cfg = DerivConfig{};
  return req;
}

TEST(SwapLeg, SolveCycleSwapStrikesFairAndSizesToTargetVega) {
  const fs::path dir = fresh_dir("solve");
  const PricedSurface s0 = make_surface(kUid, 100.0, kBaseNow);
  const Result<MarketSnapshot> snap = MarketSnapshot::load(write_one(dir, "2026-08-01", s0));
  ASSERT_TRUE(snap.has_value());
  const SurfaceRef surface = snap->find(kUid);
  ASSERT_NE(surface, nullptr);

  std::vector<std::int64_t> sessions;
  for (int i = 0; i < 5; ++i) {
    sessions.push_back(kBaseNow + i * kStepNs);
  }
  const CycleSwapRequest req = make_request(sessions);
  constexpr double kTarget = 2500.0;

  const Result<SwapLot> lot = solve_cycle_swap(surface, req, kTarget);
  ASSERT_TRUE(lot.has_value()) << lot.error().to_string();
  EXPECT_EQ(lot->id, 0u); // the caller's watermark, untouched by the solve
  EXPECT_EQ(lot->uid, kUid);
  EXPECT_EQ(lot->kind, DerivKind::VarSwap);
  EXPECT_GT(lot->strike_dec, 0.0);
  EXPECT_EQ(lot->cap_dec, 0.0);
  EXPECT_EQ(lot->n_obs_total, 3u); // sessions in (open, expiry] minus the seed
  EXPECT_EQ(lot->start_ts_ns, sessions.front());
  EXPECT_EQ(lot->expiry_ts_ns, sessions.back());

  // Independent oracle: the very greeks the solve differentiated. qty * entry
  // vega must reproduce the target — that is what "equal vega" MEANS.
  RealizedVarianceSpec staged{};
  staged.annualization = 252.0;
  staged.n_obs_total = lot->n_obs_total;
  const Result<DerivGreeks> g = detail::deriv_greeks_on_ref(
      surface, swap_contract_for_lot(*lot, lot->start_ts_ns, staged), req.deriv_cfg,
      DerivGreekBumps{});
  ASSERT_TRUE(g.has_value());
  EXPECT_NEAR(lot->qty * g->vega, kTarget, 1e-6 * kTarget);

  // ... and the fair strike prices the entry to zero PV under the same bridge.
  const Result<DerivQuote> q = detail::deriv_price_on_ref(
      surface, swap_contract_for_lot(*lot, lot->start_ts_ns, staged), req.deriv_cfg);
  ASSERT_TRUE(q.has_value());
  EXPECT_NEAR(q->pv, 0.0, 1e-12);
}

TEST(SwapLeg, SolveCycleSwapRefusesAOneSessionCycle) {
  const fs::path dir = fresh_dir("short");
  const PricedSurface s0 = make_surface(kUid, 100.0, kBaseNow);
  const Result<MarketSnapshot> snap = MarketSnapshot::load(write_one(dir, "2026-08-01", s0));
  ASSERT_TRUE(snap.has_value());
  const SurfaceRef surface = snap->find(kUid);
  ASSERT_NE(surface, nullptr);

  // Expiry ONE session after open: the single in-window session seeds and the
  // series would observe no return at all — n_obs_total 0, which the engine
  // boundary rejects. The solve refuses instead of fabricating a schedule.
  const std::int64_t sessions[] = {kBaseNow, kBaseNow + kStepNs};
  CycleSwapRequest req = make_request(sessions);
  const Result<SwapLot> lot = solve_cycle_swap(surface, req, 2500.0);
  ASSERT_FALSE(lot.has_value());
  EXPECT_EQ(lot.error().code(), atx::core::ErrorCode::Unavailable);
}

TEST(SwapLeg, SolveCycleSwapRefusesNonFiniteTargetVega) {
  const fs::path dir = fresh_dir("badvega");
  const PricedSurface s0 = make_surface(kUid, 100.0, kBaseNow);
  const Result<MarketSnapshot> snap = MarketSnapshot::load(write_one(dir, "2026-08-01", s0));
  ASSERT_TRUE(snap.has_value());
  const SurfaceRef surface = snap->find(kUid);
  ASSERT_NE(surface, nullptr);

  std::vector<std::int64_t> sessions;
  for (int i = 0; i < 5; ++i) {
    sessions.push_back(kBaseNow + i * kStepNs);
  }
  const CycleSwapRequest req = make_request(sessions);
  EXPECT_FALSE(solve_cycle_swap(surface, req, kNaN).has_value());
  EXPECT_FALSE(solve_cycle_swap(surface, req, 0.0).has_value());
}

TEST(SwapLeg, ContractForLotCarriesResidualTenorAndStagedRv) {
  const SwapLot lot = make_lot(7, kBaseNow, kBaseNow + 91 * kDayNs);
  RealizedVarianceSpec rv{};
  rv.annualization = 252.0;
  rv.n_obs_total = 63;
  rv.n_obs_done = 10;
  rv.sum_sq_log_returns_done = 0.0025;
  rv.rv_done_dec = 252.0 * 0.0025 / 10.0;

  const DerivContract c = swap_contract_for_lot(lot, kBaseNow, rv);
  EXPECT_EQ(c.kind, DerivKind::VarSwap);
  EXPECT_NEAR(c.maturity_t, static_cast<double>(91 * kDayNs) / static_cast<double>(kNsPerYear),
              1e-15);
  EXPECT_EQ(c.strike_dec, 0.04);
  EXPECT_EQ(c.cap_dec, 0.0);
  EXPECT_EQ(c.notional, 1.0);
  EXPECT_EQ(c.rv_spec.n_obs_total, 63u);
  EXPECT_EQ(c.rv_spec.n_obs_done, 10u);
  EXPECT_EQ(c.rv_spec.sum_sq_log_returns_done, 0.0025);

  // An expired lot reports a NEGATIVE residual tenor rather than clamping: the
  // sign is the caller's signal, exactly as the engine's own residual_T is.
  const DerivContract expired = swap_contract_for_lot(lot, kBaseNow + 100 * kDayNs, rv);
  EXPECT_LT(expired.maturity_t, 0.0);
}

TEST(SwapLeg, ProbeReportsNaNBeforeAnyStepAndOnWrongSnapshot) {
  const fs::path dir = fresh_dir("nan");
  const PricedSurface s0 = make_surface(kUid, 100.0, kBaseNow);
  const PricedSurface s1 = make_surface(kUid, 106.0, kBaseNow + kStepNs);
  const Result<MarketSnapshot> snap0 = MarketSnapshot::load(write_one(dir, "2026-08-01", s0));
  const Result<MarketSnapshot> snap1 = MarketSnapshot::load(write_one(dir, "2026-08-02", s1));
  ASSERT_TRUE(snap0.has_value());
  ASSERT_TRUE(snap1.has_value());

  SwapSignalProbe probe;
  EXPECT_FALSE(probe.stepped());
  // Never stepped: all five present, all five NaN.
  const Five before = five_of(probe, *snap0);
  ASSERT_TRUE(before.complete);
  EXPECT_TRUE(std::isnan(before.delta));
  EXPECT_TRUE(std::isnan(before.gamma));
  EXPECT_TRUE(std::isnan(before.vega));
  EXPECT_TRUE(std::isnan(before.theta));
  EXPECT_TRUE(std::isnan(before.rho));

  // A live lot, refreshed as-of snap0 — but interrogated with snap1: the cached
  // accrual is as-of ONE snapshot, and any other measures nothing.
  PortfolioState book;
  probe.capture_pre_step(book);
  book.swap_lots.push_back(make_lot(1, kBaseNow, kBaseNow + 5 * kStepNs));
  probe.refresh(*snap0, book);
  EXPECT_TRUE(probe.stepped());
  const Five as_of = five_of(probe, *snap0);
  ASSERT_TRUE(as_of.complete);
  EXPECT_TRUE(std::isfinite(as_of.vega));
  const Five wrong = five_of(probe, *snap1);
  ASSERT_TRUE(wrong.complete);
  EXPECT_TRUE(std::isnan(wrong.delta));
  EXPECT_TRUE(std::isnan(wrong.vega));
}

TEST(SwapLeg, ProbeSeedsOneStepLateAndMarksRestoredLotsDesynced) {
  const fs::path dir = fresh_dir("seed");
  const PricedSurface s0 = make_surface(kUid, 100.0, kBaseNow);
  const PricedSurface s1 = make_surface(kUid, 106.0, kBaseNow + kStepNs);
  const PricedSurface s2 = make_surface(kUid, 96.0, kBaseNow + 2 * kStepNs);
  const Result<MarketSnapshot> snap0 = MarketSnapshot::load(write_one(dir, "2026-08-01", s0));
  const Result<MarketSnapshot> snap1 = MarketSnapshot::load(write_one(dir, "2026-08-02", s1));
  const Result<MarketSnapshot> snap2 = MarketSnapshot::load(write_one(dir, "2026-08-03", s2));
  ASSERT_TRUE(snap0.has_value());
  ASSERT_TRUE(snap1.has_value());
  ASSERT_TRUE(snap2.has_value());

  // FRESH ADOPTION: the lot appears DURING step 0 (absent from the pre-step
  // capture), so its mirror starts clean — no fixing on sight, seed on step 1,
  // first accrued return on step 2. The greeks stay finite throughout: an
  // accrual mid-flight is a valid contract state.
  {
    SwapSignalProbe probe;
    PortfolioState book;
    probe.capture_pre_step(book); // empty book: nothing carried in
    book.swap_lots.push_back(make_lot(1, kBaseNow, kBaseNow + 5 * kStepNs));
    probe.refresh(*snap0, book);
    EXPECT_TRUE(std::isfinite(five_of(probe, *snap0).vega));

    probe.capture_pre_step(book);
    probe.refresh(*snap1, book); // the seed observation: accrues nothing
    EXPECT_TRUE(std::isfinite(five_of(probe, *snap1).vega));

    probe.capture_pre_step(book);
    probe.refresh(*snap2, book); // first real fixing lands here
    const Five f = five_of(probe, *snap2);
    ASSERT_TRUE(f.complete);
    EXPECT_TRUE(std::isfinite(f.delta));
    EXPECT_TRUE(std::isfinite(f.gamma));
    EXPECT_TRUE(std::isfinite(f.vega));
    EXPECT_TRUE(std::isfinite(f.rho));
  }

  // CHECKPOINT RESTORE: the lot is ALREADY in the book when the step begins and
  // the probe has never seen it — its realized variance is unreachable, and
  // unreachable is reported as unknown (NaN), forever, not as a clean accrual.
  {
    SwapSignalProbe probe;
    PortfolioState book;
    book.swap_lots.push_back(make_lot(9, kBaseNow, kBaseNow + 5 * kStepNs));
    probe.capture_pre_step(book); // carried in: a restore, not an open
    probe.refresh(*snap0, book);
    const Five f = five_of(probe, *snap0);
    ASSERT_TRUE(f.complete);
    EXPECT_TRUE(std::isnan(f.delta));
    EXPECT_TRUE(std::isnan(f.vega));
    // ... and it never recovers: the accrual can only drift further from truth.
    probe.capture_pre_step(book);
    probe.refresh(*snap1, book);
    EXPECT_TRUE(std::isnan(five_of(probe, *snap1).vega));
  }
}

} // namespace
