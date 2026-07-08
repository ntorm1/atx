#include <gtest/gtest.h>

#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <limits>
#include <optional>
#include <string>
#include <vector>

#include "atx/vol/american.hpp"        // american_price, al_fast_opts, AlOpts
#include "atx/vol/curve_fit.hpp"       // fit_curve_surface, CurveSurfaceReport
#include "atx/vol/dividend.hpp"        // hybrid_forward, HybridDivParams
#include "atx/vol/opra_panel.hpp"      // load_opra_cbbo_parquet, OpraLoadSpec
#include "atx/vol/surface_parity.hpp"  // SurfaceParityInputs, SliceContext, CalendarRepair
#include "atx/vol/types.hpp"           // Side
#include "atx/vol/universe.hpp"        // Underlying, Chain, chain_index, Universe, data_install
#include "atx/vol/vol_curve.hpp"       // CurveConfig, IVolCurve

// S0-1 gate: `fit_curve_surface`'s per-chain de-Am pre-pass fans out over
// `in.fit_workers` (parallel_for block-partition), but the fit itself stays
// sequential (calendar-floor dependency). The result — the fitted surface AND
// every SliceContext — must be bit-identical for any worker count.
//
// Case 1 (SyntheticBoardBitIdenticalAcrossWorkers) is the hard gate: no data
// dependency, runs everywhere. Case 2 (SpyBoardBitIdenticalAcrossWorkers)
// proves the same identity on the real 13.9k-contract SPY board and reports
// the measured wall-clock speedup; it SKIPs cleanly if the (gitignored) cached
// parquet fixture is absent — it never triggers a Databento pull.

namespace {

using atx::vol::al_fast_opts;
using atx::vol::american_price;
using atx::vol::AmericanMethod;
using atx::vol::CalendarRepair;
using atx::vol::Chain;
using atx::vol::chain_index;
using atx::vol::CurveConfig;
using atx::vol::data_install;
using atx::vol::DividendEvent;
using atx::vol::fit_curve_surface;
using atx::vol::HybridDivParams;
using atx::vol::hybrid_forward;
using atx::vol::load_opra_cbbo_parquet;
using atx::vol::OpraLoadSpec;
using atx::vol::Side;
using atx::vol::SliceContext;
using atx::vol::SurfaceParityInputs;
using atx::vol::Underlying;
using atx::vol::Universe;

constexpr double kSpot = 100.0;
constexpr double kRate = 0.03;

// Convex-ish smile in log-moneyness k = ln(K/F): a parabola with a modest
// downside skew, always positive over the +/-20% moneyness range used below.
[[nodiscard]] double smile_sigma(double k) noexcept {
  return 0.22 - 0.10 * k + 0.35 * k * k;
}

// Build one synthetic chain at year-fraction T with `n_strikes` strikes spanning
// +/-20% log-moneyness around the EXACT forward `fit_curve_surface` will itself
// compute (borrow fixed at 0, no cash divs => `resolve_chain_forward` calls the
// same `hybrid_forward` with the same arguments — calling it here directly keeps
// the test's forward bit-identical to the fit's, with no risk of drift from a
// hand-rolled S*e^{rT} approximation).
//
// Only the OTM leg per strike (chain_index(idx, otm_side(k))) is populated, with
// an American premium generated via the SAME cold Andersen-Lake pricer + ACCURATE
// preset (nullopt) the fit's de-Am inverts, at q = q_eff exactly. Per deamer.hpp,
// that round-trip is self-consistent: build_observations_european recovers (to
// solver tolerance) the same `smile_sigma(k)` for every surviving strike. The
// non-preferred leg is left zeroed, which fails the quote-valid gate and is
// harmlessly ignored — both the observation builder and build_parity_data select
// the OTM leg via the identical prefer-call / otm_side rule.
[[nodiscard]] Chain make_chain(double T, int n_strikes) {
  Chain c;
  c.T = T;
  c.expiry_ns = static_cast<std::int64_t>(T * 3.1536e16);  // ACT/365-ish, ns/yr

  const std::vector<DividendEvent> no_divs;
  const double F = hybrid_forward(kSpot, kRate, /*borrow=*/0.0, T, no_divs,
                                  c.expiry_ns, /*now_ts_ns=*/0, HybridDivParams{});
  const double q_eff = kRate - std::log(F / kSpot) / T;

  constexpr double kLo = -0.20;
  constexpr double kHi = 0.20;
  c.strikes.reserve(static_cast<std::size_t>(n_strikes));
  for (int i = 0; i < n_strikes; ++i) {
    const double frac = static_cast<double>(i) / static_cast<double>(n_strikes - 1);
    const double k = kLo + frac * (kHi - kLo);
    c.strikes.push_back(F * std::exp(k));
  }

  const std::size_t n = c.strikes.size();
  c.bids.assign(2 * n, 0.0);
  c.asks.assign(2 * n, 0.0);
  c.bid_sizes.assign(2 * n, 0);
  c.ask_sizes.assign(2 * n, 0);
  c.mids.assign(2 * n, 0.0);
  c.ivs.assign(2 * n, std::numeric_limits<double>::quiet_NaN());
  c.ts_ns.assign(2 * n, 0);
  c.flags.assign(2 * n, 0);

  for (std::size_t i = 0; i < n; ++i) {
    const double K = c.strikes[i];
    const double k = std::log(K / F);
    const Side side = (k >= 0.0) ? Side::Call : Side::Put;
    const double sigma = smile_sigma(k);
    const auto px_res = american_price(kSpot, K, T, sigma, kRate, q_eff, side,
                                       AmericanMethod::AndersenLake, std::nullopt);
    if (!px_res.has_value()) {
      ADD_FAILURE() << "american_price failed for K=" << K << " T=" << T;
      continue;
    }
    const double px = *px_res;
    const double half = std::max(0.0025 * px, 1.0e-4);
    const std::size_t idx = chain_index(static_cast<std::uint16_t>(i), side);
    c.mids[idx] = px;
    c.bids[idx] = px - half;
    c.asks[idx] = px + half;
    c.bid_sizes[idx] = 1;
    c.ask_sizes[idx] = 1;
  }
  return c;
}

[[nodiscard]] Underlying make_synthetic_underlying() {
  Underlying u;
  u.spot = kSpot;
  for (const double T : {0.10, 0.30, 0.60, 1.00}) {
    u.chains.push_back(make_chain(T, 17));
  }
  return u;
}

[[nodiscard]] SurfaceParityInputs base_inputs(unsigned workers) {
  SurfaceParityInputs in{};
  in.S = kSpot;
  in.r = kRate;
  in.deam.imply_borrow = false;  // borrow fixed at 0 -- see make_chain's comment
  in.deam.borrow_fixed = 0.0;
  in.fit_workers = workers;
  return in;
}

// Assert every SliceContext field-wise equal, and every slice's iv(k) on a
// fixed k-grid bit-exact equal, between two CurveSurfaceReports fit from
// IDENTICAL inputs at (possibly) different worker counts.
void expect_bit_identical(const atx::vol::CurveSurfaceReport& a,
                          const atx::vol::CurveSurfaceReport& b,
                          bool strict_finite) {
  ASSERT_EQ(a.n_slices, b.n_slices);
  ASSERT_EQ(a.context.size(), b.context.size());
  for (std::size_t i = 0; i < a.context.size(); ++i) {
    const SliceContext& ca = a.context[i];
    const SliceContext& cb = b.context[i];
    EXPECT_EQ(ca.T, cb.T) << "slice " << i;
    EXPECT_EQ(ca.forward, cb.forward) << "slice " << i;
    EXPECT_EQ(ca.borrow, cb.borrow) << "slice " << i;
    EXPECT_EQ(ca.q_eff, cb.q_eff) << "slice " << i;
    EXPECT_EQ(ca.n_used, cb.n_used) << "slice " << i;
    EXPECT_EQ(ca.n_dropped, cb.n_dropped) << "slice " << i;
  }

  ASSERT_EQ(a.surface.n_slices(), b.surface.n_slices());
  constexpr double kGrid[] = {-0.10, -0.05, -0.02, 0.0, 0.02, 0.05, 0.10};
  for (std::size_t si = 0; si < a.surface.n_slices(); ++si) {
    const auto* ca = a.surface.slices()[si].get();
    const auto* cb = b.surface.slices()[si].get();
    for (const double k : kGrid) {
      const double iva = ca->iv(k);
      const double ivb = cb->iv(k);
      if (strict_finite) {
        ASSERT_TRUE(std::isfinite(iva)) << "slice " << si << " k=" << k;
        ASSERT_TRUE(std::isfinite(ivb)) << "slice " << si << " k=" << k;
      } else {
        EXPECT_EQ(std::isfinite(iva), std::isfinite(ivb))
            << "slice " << si << " k=" << k;
        if (!std::isfinite(iva) || !std::isfinite(ivb)) {
          continue;  // out-of-domain sample on the real board; skip the value check
        }
      }
      EXPECT_EQ(iva, ivb) << "slice " << si << " k=" << k;
    }
  }
}

// Locate the cached SPY parquet across the paths a test binary might run from.
// (Copied from curve_noarb_test.cpp / spy_real_test.cpp — same gitignored fixture.)
[[nodiscard]] std::string find_spy_parquet() {
  const char* candidates[] = {
      "data/spy_opra_cbbo1m_2026-06-05T1955Z.parquet",
      "../data/spy_opra_cbbo1m_2026-06-05T1955Z.parquet",
      "../../data/spy_opra_cbbo1m_2026-06-05T1955Z.parquet",
      "C:/atx/data/spy_opra_cbbo1m_2026-06-05T1955Z.parquet",
  };
  for (const char* c : candidates) {
    if (std::filesystem::exists(c)) {
      return c;
    }
  }
  return {};
}

}  // namespace

TEST(CurveFitParallel, SyntheticBoardBitIdenticalAcrossWorkers) {
  const Underlying under = make_synthetic_underlying();
  CurveConfig cfg;  // default = ConvexDense

  const SurfaceParityInputs in1 = base_inputs(1);
  auto rep1 = fit_curve_surface(under, in1, cfg);
  ASSERT_TRUE(rep1.has_value()) << rep1.error().to_string();

  const SurfaceParityInputs in8 = base_inputs(8);
  auto rep8 = fit_curve_surface(under, in8, cfg);
  ASSERT_TRUE(rep8.has_value()) << rep8.error().to_string();

  ASSERT_EQ(rep1->n_slices, 4u);
  expect_bit_identical(*rep1, *rep8, /*strict_finite*/ true);
}

TEST(CurveFitParallel, SpyBoardBitIdenticalAcrossWorkers) {
  const std::string path = find_spy_parquet();
  if (path.empty()) {
    GTEST_SKIP() << "cached SPY OPRA parquet not found; run the databento pull + "
                    "opra_dbn_to_parquet to materialise the fixture.";
  }

  OpraLoadSpec spec;
  spec.path = path;
  spec.underlying = "SPY";
  spec.snapshot_iso = "2026-06-05T19:55:00Z";
  spec.r = 0.043;
  const auto panel = load_opra_cbbo_parquet(spec);
  ASSERT_TRUE(panel.has_value()) << panel.error().to_string();

  Universe u;
  const auto uid = data_install(u, panel->frame);
  ASSERT_TRUE(uid.has_value());
  const auto under = u.get_underlying(*uid);
  ASSERT_TRUE(under.has_value());
  const Underlying* U = *under;

  SurfaceParityInputs in{};
  in.S = panel->implied_spot;
  in.r = spec.r;
  in.now_ts_ns = panel->frame.snapshot_ts_ns;
  in.band_k = 1.0;
  in.repair = CalendarRepair::None;
  // Same fast-cold Andersen-Lake preset the served path uses (matches
  // curve_noarb_test.cpp) -- this is what production actually serves.
  in.deam.al_opts = al_fast_opts();
  in.deam.iv_tol = 1.0e-5;
  in.deam.n_atm = 1;

  CurveConfig cfg;  // default = ConvexDense (the served dense surface)

  SurfaceParityInputs in1 = in;
  in1.fit_workers = 1;  // serial -- the pre-S0-1 path
  const auto t0 = std::chrono::steady_clock::now();
  auto rep1 = fit_curve_surface(*U, in1, cfg);
  const auto t1 = std::chrono::steady_clock::now();
  ASSERT_TRUE(rep1.has_value()) << rep1.error().to_string();

  SurfaceParityInputs in0 = in;
  in0.fit_workers = 0;  // auto -- hardware_concurrency
  const auto t2 = std::chrono::steady_clock::now();
  auto rep0 = fit_curve_surface(*U, in0, cfg);
  const auto t3 = std::chrono::steady_clock::now();
  ASSERT_TRUE(rep0.has_value()) << rep0.error().to_string();

  const double ms1 = std::chrono::duration<double, std::milli>(t1 - t0).count();
  const double ms0 = std::chrono::duration<double, std::milli>(t3 - t2).count();
  // Informational only -- NOT a hard timing assert (flaky; stripped elsewhere
  // in this suite). The bit-identity assertions below are the actual gate.
  std::printf(
      "[CurveFitParallel SPY] fit_workers=1 %.1fms, fit_workers=0(auto) %.1fms, "
      "speedup=%.2fx\n",
      ms1, ms0, (ms0 > 0.0) ? (ms1 / ms0) : 0.0);

  expect_bit_identical(*rep1, *rep0, /*strict_finite*/ false);
}
