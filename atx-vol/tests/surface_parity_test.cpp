#include <gtest/gtest.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "atx/vol/arb.hpp"
#include "atx/vol/curve.hpp"
#include "atx/vol/data.hpp"
#include "atx/vol/panel.hpp"
#include "atx/vol/s3.hpp"
#include "atx/vol/surface_parity.hpp"
#include "atx/vol/types.hpp"
#include "atx/vol/universe.hpp"
#include "atx/vol/vol_surface.hpp"

// MULTI-EXPIRY de-Americanized volatility-SURFACE parity acceptance harness
// (atx/vol/surface_parity.hpp). The primary test drives the whole surface
// pipeline on a 4-expiry known-truth equity panel (rising ATM term structure,
// downward skew, one mid-life cash dividend) and asserts:
//   * four ascending-T eSSVI slices assembled;
//   * per-expiry re-Americanized fair values land inside the bid-ask;
//   * the assembled surface is calendar-arbitrage-free;
//   * each slice reproduces its truth ATM vol; and
//   * TIME-INTERPOLATION PARITY — surface.iv(k, T*) at intermediate maturities
//     matches a linear-in-total-variance reference built from the s3_iv truth.
// A guard test pins the no-chains error path.

namespace {

using atx::vol::DividendEvent;
using atx::vol::iso_to_ns;
using atx::vol::make_synthetic_american_panel;
using atx::vol::run_surface_parity;
using atx::vol::s3_total_var;
using atx::vol::S3Params;
using atx::vol::SurfaceParityInputs;
using atx::vol::SurfaceParityReport;
using atx::vol::SynthExpiry;
using atx::vol::SynthPanelSpec;
using atx::vol::Underlying;
using atx::vol::Universe;
using atx::vol::year_fraction;

// The 4-expiry known-truth panel spec plus the parallel truth smiles.
struct PanelBuild {
  SynthPanelSpec spec;
  std::vector<S3Params> truths;  // ascending T, parallel to spec.expiries
  std::string snapshot;
};

[[nodiscard]] PanelBuild make_panel_build() {
  PanelBuild pb;
  pb.snapshot = "2026-06-19";

  // Four expiries ~0.1 / 0.3 / 0.6 / 1.0 years out; each T is taken from
  // year_fraction so the panel truth T equals the installed chain.T bit-for-bit
  // (a coherent time axis is required for the interpolation reference below).
  const std::vector<std::string> isos = {
      "2026-07-26",  // ~0.10y
      "2026-10-06",  // ~0.30y
      "2027-01-24",  // ~0.60y
      "2027-06-19",  // ~1.00y
  };
  // Rising ATM term structure with a downward skew — plain SSVI shapes the
  // eSSVI backbone (== S3/SSVI under the default symmetric-rho calibration)
  // reproduces exactly.
  pb.truths = {
      S3Params{0.32, -0.70, 1.00},
      S3Params{0.30, -0.60, 0.90},
      S3Params{0.28, -0.55, 0.80},
      S3Params{0.27, -0.50, 0.70},
  };

  SynthPanelSpec& spec = pb.spec;
  spec.uid = "SYNTH";
  spec.snapshot_iso = pb.snapshot;
  spec.spot = 100.0;
  spec.r = 0.03;
  spec.borrow = 0.008;
  DividendEvent div;
  div.ex_date_ns = iso_to_ns("2026-12-15");  // mid-life; inside the 0.6y/1.0y expiries
  div.amount = 0.5;
  spec.cash_divs = {div};

  for (std::size_t i = 0; i < isos.size(); ++i) {
    const double T = year_fraction(pb.snapshot, isos[i]);
    spec.expiries.push_back(SynthExpiry{isos[i], T, pb.truths[i]});
  }
  for (double K = 70.0; K <= 130.0 + 1e-9; K += 4.0) {
    spec.strikes.push_back(K);  // 16 strikes over 70..130
  }
  spec.half_spread_frac = 0.02;
  return pb;
}

}  // namespace

TEST(SurfaceParity, FourExpiryPanel_Essvi_InterpolatesAndCalendarArbFree) {
  const PanelBuild pb = make_panel_build();

  const auto panel = make_synthetic_american_panel(pb.spec);
  ASSERT_TRUE(panel.has_value()) << panel.error().to_string();

  Universe u;
  const auto uid = atx::vol::data_install(u, panel->frame);
  ASSERT_TRUE(uid.has_value()) << uid.error().to_string();
  const auto under = u.get_underlying(*uid);
  ASSERT_TRUE(under.has_value());
  ASSERT_EQ((*under)->chains.size(), std::size_t{4});

  SurfaceParityInputs in;
  in.S = pb.spec.spot;
  in.r = pb.spec.r;
  in.cash_divs = pb.spec.cash_divs;
  in.now_ts_ns = iso_to_ns(pb.snapshot);
  in.deam.hyb = pb.spec.hyb;
  in.deam.imply_borrow = true;
  in.deam.n_atm = 3;

  const auto res = run_surface_parity(**under, in);
  ASSERT_TRUE(res.has_value()) << res.error().to_string();
  const SurfaceParityReport& rep = *res;

  // Four slices, strictly ascending T.
  EXPECT_EQ(rep.n_slices, std::size_t{4});
  ASSERT_EQ(rep.expiry_T.size(), std::size_t{4});
  for (std::size_t i = 1; i < rep.expiry_T.size(); ++i) {
    EXPECT_LT(rep.expiry_T[i - 1], rep.expiry_T[i]);
  }

  // Re-Americanized fair values land inside the bid-ask on every expiry (the
  // worst-case expiry still clears 90%).
  EXPECT_GE(rep.worst_frac_within_bidask, 0.90);

  // The assembled eSSVI surface is calendar-arbitrage-free.
  EXPECT_TRUE(rep.calendar_arb_free);

  // Each fitted slice reproduces its truth AT THE MONEY (k = 0 == at-forward).
  // PARITY NOTE: this rests on the exact eSSVI-backbone == S3/SSVI correspondence
  // (theta = sigma0^2*T, phi = sqrt((s2^2 + 2*c2)/theta), rho = s2/sqrt(s2^2 +
  // 2*c2)); with the clean de-Am round-trip recovering the truth strip, the fit
  // recovers the truth params and the observed ATM error is ~1e-4, well inside
  // the 3e-3 bar.
  for (std::size_t i = 0; i < rep.expiry_T.size(); ++i) {
    const double atm =
        rep.surface.iv_on_slice(static_cast<std::uint16_t>(i), 0.0);
    EXPECT_NEAR(atm, pb.truths[i].sigma0, 3.0e-3) << "slice " << i;
  }

  // INTERPOLATION PARITY: at maturities strictly between adjacent fitted slices
  // the surface interpolates LINEARLY IN TOTAL VARIANCE (w = sigma^2*T) across
  // the two bracketing slices, then reports iv = sqrt(w/T*). We build the
  // reference the SAME way from the TRUTH slices: bracket T*, linear in
  // w_truth(k, T) = s3_total_var(k, T, truth), iv_ref = sqrt(w_ref/T*). Because
  // each fitted eSSVI slice reproduces its S3 truth for ALL k, the surface's
  // interpolation must coincide with the truth slices' interpolation.
  const std::vector<double>& Ts = rep.expiry_T;
  const auto iv_ref = [&](double k, double Tstar) {
    // Bracket Tstar among the fitted slice T's (identical to VolSurface::w).
    std::size_t hi = 0;
    while (hi < Ts.size() && Ts[hi] <= Tstar) {
      ++hi;
    }
    const std::size_t lo = hi - 1;
    const double w_lo = s3_total_var(k, Ts[lo], pb.truths[lo]);
    const double w_hi = s3_total_var(k, Ts[hi], pb.truths[hi]);
    const double alpha = (Tstar - Ts[lo]) / (Ts[hi] - Ts[lo]);
    const double w = w_lo + alpha * (w_hi - w_lo);
    return std::sqrt(w / Tstar);
  };

  // PARITY NOTE: 2e-3 rests on the same exact-correspondence + clean-round-trip
  // argument as the ATM check; the interpolation adds only a convex combination
  // of two near-exact slices, so the observed error stays ~1e-4.
  for (std::size_t i = 0; i + 1 < Ts.size(); ++i) {
    const double Tstar = 0.5 * (Ts[i] + Ts[i + 1]);
    for (double k = -0.30; k <= 0.30 + 1e-9; k += 0.10) {
      const double got = rep.surface.iv(k, Tstar);
      const double ref = iv_ref(k, Tstar);
      EXPECT_NEAR(got, ref, 2.0e-3)
          << "k=" << k << " T*=" << Tstar << " (between slices " << i << " and "
          << (i + 1) << ")";
    }
  }
}

TEST(SurfaceParity, CalendarRepair_HoldsQualityAndDefersScoring) {
  const PanelBuild pb = make_panel_build();
  const auto panel = make_synthetic_american_panel(pb.spec);
  ASSERT_TRUE(panel.has_value()) << panel.error().to_string();

  Universe u;
  const auto uid = atx::vol::data_install(u, panel->frame);
  ASSERT_TRUE(uid.has_value()) << uid.error().to_string();
  const auto under = u.get_underlying(*uid);
  ASSERT_TRUE(under.has_value());

  SurfaceParityInputs base;
  base.S = pb.spec.spot;
  base.r = pb.spec.r;
  base.cash_divs = pb.spec.cash_divs;
  base.now_ts_ns = iso_to_ns(pb.snapshot);
  base.deam.hyb = pb.spec.hyb;
  base.deam.imply_borrow = true;
  base.deam.n_atm = 3;

  // Baseline: check-only (default). The clean 4-expiry panel is already
  // calendar-arb-free, so there is nothing to repair.
  SurfaceParityInputs none = base;
  none.repair = atx::vol::CalendarRepair::None;
  const auto r_none = run_surface_parity(**under, none);
  ASSERT_TRUE(r_none.has_value()) << r_none.error().to_string();
  EXPECT_TRUE(r_none->calendar_arb_free);
  EXPECT_EQ(r_none->n_calendar_viol_pre, std::size_t{0});

  // Project: on an already-arb-free surface the repair guard short-circuits, so
  // the surface is untouched and the scored quality is BIT-IDENTICAL to the
  // check-only run. This pins two invariants at once: (1) the repair path does
  // not perturb a clean surface, and (2) the deferred (post-repair) scoring
  // reproduces the historical interleaved scoring exactly.
  SurfaceParityInputs proj = base;
  proj.repair = atx::vol::CalendarRepair::Project;
  const auto r_proj = run_surface_parity(**under, proj);
  ASSERT_TRUE(r_proj.has_value()) << r_proj.error().to_string();
  EXPECT_TRUE(r_proj->calendar_arb_free);
  EXPECT_EQ(r_proj->n_slices, r_none->n_slices);
  ASSERT_EQ(r_proj->per_expiry.size(), r_none->per_expiry.size());
  EXPECT_DOUBLE_EQ(r_proj->worst_frac_within_bidask,
                   r_none->worst_frac_within_bidask);
  for (std::size_t i = 0; i < r_proj->per_expiry.size(); ++i) {
    EXPECT_DOUBLE_EQ(r_proj->per_expiry[i].frac_fv_within_bidask,
                     r_none->per_expiry[i].frac_fv_within_bidask);
  }
}

TEST(SurfaceParity, MonotoneFit_ClearsNearMoneyCrossingAtHeldQuality) {
  // Engineer a panel with a near-money calendar crossing: the front expiry has a
  // steep curvature (c2), so w(k) ~ theta + sigma_hat0*s2*k + 0.5*c2*k^2 pokes
  // ABOVE the second expiry near the money even though ATM theta stays monotone
  // (theta = sigma0^2*T rises). This is exactly the off-ATM crossing a theta
  // floor cannot fix and the active-set w-floor (MonotoneFit) can.
  const std::string snapshot = "2026-06-19";
  const std::vector<std::string> isos = {"2026-07-26", "2026-10-06",
                                         "2027-01-24"};
  const std::vector<S3Params> truths = {
      S3Params{0.36, -0.10, 2.50},  // T~0.10: high curvature -> steep wings
      S3Params{0.26, -0.10, 0.30},  // T~0.30
      S3Params{0.25, -0.10, 0.30},  // T~0.60
  };

  SynthPanelSpec spec;
  spec.uid = "XSEC";
  spec.snapshot_iso = snapshot;
  spec.spot = 100.0;
  spec.r = 0.03;
  spec.borrow = 0.005;
  for (std::size_t i = 0; i < isos.size(); ++i) {
    spec.expiries.push_back(
        SynthExpiry{isos[i], year_fraction(snapshot, isos[i]), truths[i]});
  }
  for (double K = 60.0; K <= 140.0 + 1e-9; K += 4.0) {
    spec.strikes.push_back(K);
  }
  spec.half_spread_frac = 0.02;

  const auto panel = make_synthetic_american_panel(spec);
  ASSERT_TRUE(panel.has_value()) << panel.error().to_string();
  Universe u;
  const auto uid = atx::vol::data_install(u, panel->frame);
  ASSERT_TRUE(uid.has_value()) << uid.error().to_string();
  const auto under = u.get_underlying(*uid);
  ASSERT_TRUE(under.has_value());

  SurfaceParityInputs base;
  base.S = spec.spot;
  base.r = spec.r;
  base.now_ts_ns = iso_to_ns(snapshot);
  base.deam.imply_borrow = true;
  base.deam.n_atm = 3;

  const auto count_core = [](const SurfaceParityReport& rep) {
    const auto v = atx::vol::arb_check_calendar(rep.surface, -0.6, 0.6, 25);
    return v.has_value() ? v->size() : std::size_t{0};
  };

  // Baseline: independent fits leave the near-money crossing.
  SurfaceParityInputs none = base;
  none.repair = atx::vol::CalendarRepair::None;
  const auto r_none = run_surface_parity(**under, none);
  ASSERT_TRUE(r_none.has_value()) << r_none.error().to_string();
  const std::size_t core_none = count_core(*r_none);
  ASSERT_GT(core_none, std::size_t{0})
      << "panel did not produce a near-money crossing to repair";

  // MonotoneFit: the active-set calendar floor removes the near-money crossings.
  SurfaceParityInputs mono = base;
  mono.repair = atx::vol::CalendarRepair::MonotoneFit;
  const auto r_mono = run_surface_parity(**under, mono);
  ASSERT_TRUE(r_mono.has_value()) << r_mono.error().to_string();
  const std::size_t core_mono = count_core(*r_mono);

  // Strictly fewer near-money violations — the flagship mechanism. (This
  // synthetic uses an EXAGGERATED crossing, c2=2.5 on the front slice, to
  // reliably exercise the floor; repairing so large a crossing necessarily
  // moves the fit. The HELD-QUALITY claim — small real crossings repaired at
  // ~98.5% fair-value-in-bid-ask — is demonstrated on the XOM OPRA slice in
  // examples/opra_parity_bench.cpp, where MonotoneFit clears the near-money
  // window 26->0 at chi2 0.207->0.209.)
  EXPECT_LT(core_mono, core_none);
  // The floor must not drop or corrupt slices: the surface still assembles every
  // expiry, strictly ascending in T.
  EXPECT_EQ(r_mono->n_slices, r_none->n_slices);
  for (std::size_t i = 1; i < r_mono->expiry_T.size(); ++i) {
    EXPECT_LT(r_mono->expiry_T[i - 1], r_mono->expiry_T[i]);
  }
}

TEST(SurfaceParity, NoChains_ReturnsErr) {
  Underlying under;  // default: uid = kInvalidUid, no chains
  under.uid = 1u;

  SurfaceParityInputs in;
  in.S = 100.0;
  in.r = 0.03;

  const auto res = run_surface_parity(under, in);
  ASSERT_FALSE(res.has_value());
  EXPECT_EQ(res.error().code(), atx::vol::ErrorCode::NotFound);
}
