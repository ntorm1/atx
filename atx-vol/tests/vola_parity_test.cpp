#include <gtest/gtest.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "atx/vol/american.hpp"
#include "atx/vol/curve.hpp"
#include "atx/vol/data.hpp"
#include "atx/vol/deamer.hpp"
#include "atx/vol/dividend.hpp"
#include "atx/vol/panel.hpp"
#include "atx/vol/parity.hpp"
#include "atx/vol/s3.hpp"
#include "atx/vol/types.hpp"
#include "atx/vol/universe.hpp"
#include "atx/vol/vola_parity.hpp"

// CAPSTONE acceptance harness for the Vola Dynamics American-equity parity
// pipeline (atx/vol/vola_parity.hpp). Test A drives the whole pipeline end to
// end on a realistic known-truth synthetic equity panel and asserts the parity
// acceptance criteria. Test B contrasts the two 3-parameter SSVI-family curves
// on an event W-shape neither can represent. Test C pins the input guards.

namespace {

using atx::vol::american_price;
using atx::vol::AmericanMethod;
using atx::vol::Chain;
using atx::vol::chain_index;
using atx::vol::DividendEvent;
using atx::vol::ErrorCode;
using atx::vol::ExpiryParityInputs;
using atx::vol::ExpiryParityReport;
using atx::vol::hybrid_forward;
using atx::vol::HybridDivParams;
using atx::vol::iso_to_ns;
using atx::vol::make_synthetic_american_panel;
using atx::vol::ParityCurve;
using atx::vol::run_expiry_parity;
using atx::vol::S3Params;
using atx::vol::Side;
using atx::vol::SynthExpiry;
using atx::vol::SynthPanelSpec;
using atx::vol::Universe;
using atx::vol::year_fraction;

// Year-fraction -> epoch-ns (365.25-day year, matching hybrid_forward).
constexpr double kYearNs = 365.25 * 86400.0 * 1.0e9;
[[nodiscard]] std::int64_t years_to_ns(double y) {
  return static_cast<std::int64_t>(y * kYearNs);
}

// A downward-skewed equity smile the eSSVI backbone (== S3/SSVI) represents
// exactly: a plain SSVI shape with negative skew and positive curvature.
// (Only used implicitly through the S3 panel truth in Test A.)

// An EVENT (W / frown) smile: a broad parabola with a narrow Gaussian dip at
// the money — a high-frequency ATM feature no 3-parameter SSVI curve can match.
[[nodiscard]] double w_shape_iv(double k) noexcept {
  const double g = std::exp(-(k / 0.05) * (k / 0.05));
  return 0.30 + 0.8 * k * k - 0.6 * g * 0.05;
}

// Build a bespoke American chain from a truth IV(k) function, priced on the
// forward implied by `borrow`, with a symmetric fractional bid-ask (floored).
// Both legs of each strike are priced at the same truth vol, so put-call parity
// holds and the injected borrow is recoverable.
template <class Fn>
[[nodiscard]] Chain build_bespoke_chain(double S, double r, double T, double borrow,
                                        const std::vector<double>& strikes,
                                        double half_spread_frac, Fn&& truth_iv,
                                        std::int64_t now_ns, std::int64_t expiry_ns) {
  const std::vector<DividendEvent> no_divs;
  const HybridDivParams hyb{};  // pure escrowed cash, no proportional yield
  const double F =
      hybrid_forward(S, r, borrow, T, no_divs, expiry_ns, now_ns, hyb);
  const double q_eff = r - std::log(F / S) / T;

  Chain chain;
  chain.T = T;
  chain.expiry_ns = expiry_ns;
  chain.strikes = strikes;
  const std::size_t two_n = 2u * strikes.size();
  chain.bids.assign(two_n, 0.0);
  chain.asks.assign(two_n, 0.0);
  chain.mids.assign(two_n, 0.0);

  for (std::size_t i = 0; i < strikes.size(); ++i) {
    const double K = strikes[i];
    const double sig = truth_iv(std::log(K / F));
    for (const Side side : {Side::Call, Side::Put}) {
      const auto p = american_price(S, K, T, sig, r, q_eff, side,
                                    AmericanMethod::AndersenLake);
      const double mid = p.value_or(std::nan(""));
      const std::size_t idx = chain_index(static_cast<std::uint16_t>(i), side);
      chain.mids[idx] = mid;
      const double hw = std::fmax(half_spread_frac * mid, 0.02);
      chain.bids[idx] = std::fmax(mid - hw, 0.0);
      chain.asks[idx] = mid + hw;
    }
  }
  return chain;
}

}  // namespace

// ── Test A: end-to-end pipeline parity on a realistic equity panel ───────────

TEST(VolaParity, EndToEndPanel_Essvi_RecoversBorrowForwardAndFitsWithinBidAsk) {
  const std::string snapshot = "2026-06-19";
  const std::string expiry = "2026-12-18";  // ~0.5y out
  // Make the chain's ISO-derived year-fraction the SAME T the panel prices at,
  // so de-Americanization and the truth pricing share one coherent T.
  const double T = year_fraction(snapshot, expiry);
  ASSERT_GT(T, 0.0);

  SynthPanelSpec spec;
  spec.uid = "SYNTH";
  spec.snapshot_iso = snapshot;
  spec.spot = 100.0;
  spec.r = 0.03;
  spec.borrow = 0.01;
  spec.hyb = HybridDivParams{/*prop_div_yield=*/0.01, /*blend=*/0.10};
  DividendEvent div;
  div.ex_date_ns = iso_to_ns("2026-09-19");  // quarterly, strictly inside (now, expiry]
  div.amount = 0.5;
  spec.cash_divs = {div};
  // Downward-skewed equity smile (S3 truth is exactly representable by eSSVI).
  spec.expiries = {SynthExpiry{expiry, T, S3Params{0.28, -0.6, 0.9}}};
  for (double K = 70.0; K <= 130.0 + 1e-9; K += 4.0) {
    spec.strikes.push_back(K);  // 16 strikes over 70..130
  }
  spec.half_spread_frac = 0.02;

  const auto panel = make_synthetic_american_panel(spec);
  ASSERT_TRUE(panel.has_value()) << panel.error().to_string();

  Universe u;
  const auto uid = atx::vol::data_install(u, panel->frame);
  ASSERT_TRUE(uid.has_value()) << uid.error().to_string();
  const auto under = u.get_underlying(*uid);
  ASSERT_TRUE(under.has_value());
  ASSERT_EQ((*under)->chains.size(), std::size_t{1});
  const Chain& chain = (*under)->chains[0];

  ExpiryParityInputs in;
  in.S = spec.spot;
  in.r = spec.r;
  in.cash_divs = spec.cash_divs;
  in.now_ts_ns = iso_to_ns(snapshot);
  in.deam.hyb = spec.hyb;
  in.deam.imply_borrow = true;
  in.deam.n_atm = 3;
  in.curve = ParityCurve::Essvi;

  const auto res = run_expiry_parity(chain, in);
  ASSERT_TRUE(res.has_value()) << res.error().to_string();
  const ExpiryParityReport& r = *res;

  // Recovered borrow near the injected 0.01.
  EXPECT_NEAR(r.implied_borrow, spec.borrow, 2.0e-3);
  // Forward matches panel truth within ~1e-2 relative.
  EXPECT_NEAR(r.forward, panel->truth_forward[0],
              1.0e-2 * panel->truth_forward[0]);
  // Re-Americanized fair values sit inside the bid-ask for (nearly) all quotes.
  EXPECT_GE(r.parity.frac_fv_within_bidask, 0.95);
  // eSSVI fits an S3 truth near-exactly (same SSVI shape family).
  EXPECT_LE(r.parity.rmse_mid_vol, 5.0e-3);
  // Error-bar reduced chi-square well under the acceptance ceiling.
  EXPECT_LE(r.fit_chi2_reduced, 2.0);
  // Clean chain: essentially nothing dropped, ample survivors.
  EXPECT_LE(r.n_dropped, std::size_t{2});
  EXPECT_GE(r.n_used, std::size_t{10});
}

// ── Test B: rich-curve vs S3 on an event W-shape neither SSVI curve fits ─────

TEST(VolaParity, EventWShape_EssviAndS3_SimilarlyPoor_NeitherDramaticallyBetter) {
  const double S = 100.0;
  const double r = 0.03;
  const double T = 0.25;
  const double borrow = 0.01;
  const std::int64_t now_ns = 0;
  const std::int64_t expiry_ns = years_to_ns(T);

  std::vector<double> strikes;
  for (double K = 70.0; K <= 130.0 + 1e-9; K += 3.0) {
    strikes.push_back(K);  // 21 strikes over 70..130
  }
  const Chain chain = build_bespoke_chain(S, r, T, borrow, strikes,
                                          /*half_spread_frac=*/0.02, &w_shape_iv,
                                          now_ns, expiry_ns);

  ExpiryParityInputs base;
  base.S = S;
  base.r = r;
  base.now_ts_ns = now_ns;
  base.deam.imply_borrow = false;  // fix the borrow so both fits see one clean strip
  base.deam.borrow_fixed = borrow;

  ExpiryParityInputs in_essvi = base;
  in_essvi.curve = ParityCurve::Essvi;
  ExpiryParityInputs in_s3 = base;
  in_s3.curve = ParityCurve::S3;

  const auto ess = run_expiry_parity(chain, in_essvi);
  const auto s3 = run_expiry_parity(chain, in_s3);
  ASSERT_TRUE(ess.has_value()) << ess.error().to_string();
  ASSERT_TRUE(s3.has_value()) << s3.error().to_string();

  // PARITY NOTE: 3-param SSVI-family curves (S3 == the eSSVI backbone under the
  // default symmetric-rho calibration) cannot represent an event W-shape with a
  // narrow ATM feature (negative-curvature dip). Both therefore leave a NOTABLE
  // vol residual (>> the ~sub-1e-3 they achieve on a plain skew), and — being
  // the same shape family — neither is dramatically better than the other. The
  // nested C8/CStar curves (atx-vol's cstar_calib; see cstar_calib_test.cpp)
  // ARE what fit such W-shapes better, which is exactly Vola's S5 -> C8 -> C12
  // reduced-chi-square improvement story.
  EXPECT_GT(ess->parity.rmse_mid_vol, 3.0e-3);
  EXPECT_GT(s3->parity.rmse_mid_vol, 3.0e-3);

  // Same SSVI manifold, same external error bars in slice_fit_metrics (only the
  // fitted model IV differs), so the two reduced chi-squares are comparable —
  // within a factor of ~3 in either direction (neither an order-of-magnitude
  // better, refuting any >= 10x rich-curve claim for two SSVI-family fits).
  ASSERT_GT(ess->fit_chi2_reduced, 0.0);
  ASSERT_GT(s3->fit_chi2_reduced, 0.0);
  EXPECT_LT(ess->fit_chi2_reduced, 3.0 * s3->fit_chi2_reduced);
  EXPECT_LT(s3->fit_chi2_reduced, 3.0 * ess->fit_chi2_reduced);
}

// ── Test C: input guards ─────────────────────────────────────────────────────

TEST(VolaParity, EmptyChain_ReturnsInvalidArgument) {
  Chain chain;
  chain.T = 0.25;
  chain.expiry_ns = years_to_ns(0.25);  // no strikes

  ExpiryParityInputs in;
  in.S = 100.0;
  in.r = 0.03;

  const auto res = run_expiry_parity(chain, in);
  ASSERT_FALSE(res.has_value());
  EXPECT_EQ(res.error().code(), ErrorCode::InvalidArgument);
}

TEST(VolaParity, NonPositiveChainT_ReturnsInvalidArgument) {
  const std::vector<double> strikes{90.0, 95.0, 100.0, 105.0, 110.0, 115.0};
  Chain chain = build_bespoke_chain(100.0, 0.03, 0.25, 0.01, strikes,
                                    /*half_spread_frac=*/0.02, &w_shape_iv, 0,
                                    years_to_ns(0.25));
  chain.T = 0.0;  // degenerate maturity

  ExpiryParityInputs in;
  in.S = 100.0;
  in.r = 0.03;
  in.deam.imply_borrow = false;
  in.deam.borrow_fixed = 0.01;

  const auto res = run_expiry_parity(chain, in);
  ASSERT_FALSE(res.has_value());
  EXPECT_EQ(res.error().code(), ErrorCode::InvalidArgument);
}

TEST(VolaParity, FewerThanFiveUsableStrikes_ReturnsErr) {
  // Four valid strikes: below the fit's minimum-usable floor.
  const std::vector<double> strikes{92.0, 98.0, 104.0, 110.0};
  const Chain chain = build_bespoke_chain(100.0, 0.03, 0.25, 0.01, strikes,
                                          /*half_spread_frac=*/0.02, &w_shape_iv,
                                          0, years_to_ns(0.25));

  ExpiryParityInputs in;
  in.S = 100.0;
  in.r = 0.03;
  in.now_ts_ns = 0;
  in.deam.imply_borrow = false;  // fixed borrow: de-Am needs no near-ATM pair
  in.deam.borrow_fixed = 0.01;
  in.curve = ParityCurve::Essvi;

  const auto res = run_expiry_parity(chain, in);
  ASSERT_FALSE(res.has_value());
  EXPECT_EQ(res.error().code(), ErrorCode::NotFound);
}
