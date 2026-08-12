#include <gtest/gtest.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "atx/vol/arb.hpp"
#include "atx/vol/data.hpp"
#include "atx/vol/panel.hpp"
#include "atx/vol/rates_curve.hpp"
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

// ── The SurfaceParityReport construction contract (S4-T19, plan item 4.2) ────

// SurfaceParityReport is a designated-init-only aggregate. It is the one struct
// in this item that really WAS built positionally: `run_surface_parity` filled a
// 12-element prefix and then assigned `expiry_reports` afterwards, which is why
// that field was parked at the end. The compile-time half of the new contract is
// the field-count pin in surface_parity.hpp; this is the runtime half — a named
// initializer lands on the field its name says, and an OMITTED field takes its
// own default member initializer rather than a neighbour's value.
TEST(SurfaceParityReportContract, DesignatedInitBindsByName) {
  auto surface = atx::vol::VolSurface::create(/*uid=*/7u, atx::vol::Parametrization::Essvi,
                                              /*cap_slices=*/2);
  ASSERT_TRUE(surface.has_value()) << surface.error().to_string();

  const SurfaceParityReport rep{
      .surface = std::move(*surface),
      .expiry_T = {0.25, 0.75},
      // ‖ under.chains in CHAIN order — a DIFFERENT alignment from expiry_T, so
      // the two sizes are allowed to disagree. Naming one must not touch the
      // other, which is exactly what the old positional prefix could not promise.
      .expiry_reports = std::vector<atx::vol::ExpiryFitReport>(3),
      .worst_frac_within_bidask = 0.91,
      .calendar_arb_free = true,
      .n_slices = 2,
      .n_carry_skipped = 1,
  };

  EXPECT_EQ(rep.surface.uid(), 7u);
  ASSERT_EQ(rep.expiry_T.size(), std::size_t{2});
  EXPECT_DOUBLE_EQ(rep.expiry_T[0], 0.25);
  EXPECT_DOUBLE_EQ(rep.expiry_T[1], 0.75);
  EXPECT_EQ(rep.expiry_reports.size(), std::size_t{3});
  EXPECT_DOUBLE_EQ(rep.worst_frac_within_bidask, 0.91);
  EXPECT_TRUE(rep.calendar_arb_free);
  EXPECT_EQ(rep.n_slices, std::size_t{2});
  EXPECT_EQ(rep.n_carry_skipped, std::size_t{1});

  EXPECT_TRUE(rep.per_expiry.empty());
  EXPECT_TRUE(rep.context.empty());
  EXPECT_TRUE(rep.carry.empty());
  EXPECT_TRUE(rep.input_certification.empty());
  EXPECT_EQ(rep.n_calendar_viol_pre, std::size_t{0});
  EXPECT_EQ(rep.n_audit_starved, std::size_t{0});
  EXPECT_FALSE(rep.fit_timings.collected);
  EXPECT_DOUBLE_EQ(rep.fit_timings.total_wall_ms, 0.0);
}

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

// FT-C8 (B4): the served eSSVI path (`run_surface_parity`) historically prepared
// its observation population under the permissive LegacyEssviCompatibility
// predicate (strike>0 + quote_valid only), ignoring the configured CalibOpts
// filter cascade — so a stale/flagged quote entered the DEFAULT-family fit while
// every other family filtered it. The flag-guarded rollout
// (`essvi_serve_configured_prep`) routes the served eSSVI path through the
// configured `fit_prep_policy`. A quote flagged Stale must be EXCLUDED under
// Configured prep and KEPT under the (default) Legacy prep.
TEST(SurfaceParity, ConfiguredPrepExcludesFlaggedQuote_LegacyKeepsIt) {
  const PanelBuild pb = make_panel_build();
  const auto panel = make_synthetic_american_panel(pb.spec);
  ASSERT_TRUE(panel.has_value()) << panel.error().to_string();

  Universe u;
  const auto uid = atx::vol::data_install(u, panel->frame);
  ASSERT_TRUE(uid.has_value()) << uid.error().to_string();
  const auto under = u.get_underlying(*uid);
  ASSERT_TRUE(under.has_value());
  ASSERT_EQ((*under)->chains.size(), std::size_t{4});

  // Flag one mid-OTM strike's BOTH legs Stale on the front chain, so that strike
  // contributes no usable OTM leg under a flag-aware (Configured) filter.
  auto& chain0 = (*under)->chains[0];
  const std::uint16_t flagged_strike = 2u;  // K = 78, OTM put vs spot 100
  ASSERT_GT(chain0.strikes.size(), flagged_strike);
  for (const atx::vol::Side side : {atx::vol::Side::Call, atx::vol::Side::Put}) {
    const std::size_t qi = atx::vol::chain_index(flagged_strike, side);
    chain0.flags[qi] = static_cast<std::uint8_t>(atx::vol::QuoteFlag::Stale);
  }

  SurfaceParityInputs base;
  base.S = pb.spec.spot;
  base.r = pb.spec.r;
  base.cash_divs = pb.spec.cash_divs;
  base.now_ts_ns = iso_to_ns(pb.snapshot);
  base.deam.hyb = pb.spec.hyb;
  base.deam.imply_borrow = true;
  base.deam.n_atm = 3;

  // (1) Default (Legacy prep): the stale flag is IGNORED, the quote is kept.
  SurfaceParityInputs legacy_in = base;
  legacy_in.essvi_serve_configured_prep = false;
  const auto legacy = run_surface_parity(**under, legacy_in);
  ASSERT_TRUE(legacy.has_value()) << legacy.error().to_string();
  ASSERT_GT(legacy->context.size(), 0u);
  const std::size_t legacy_n_used = legacy->context.front().n_used;

  // (2) Configured prep (flag on): the stale quote is EXCLUDED by the cascade.
  SurfaceParityInputs configured_in = base;
  configured_in.essvi_serve_configured_prep = true;
  configured_in.fit_prep_policy = atx::vol::PreparedObservationPolicy::Configured;
  const auto configured = run_surface_parity(**under, configured_in);
  ASSERT_TRUE(configured.has_value()) << configured.error().to_string();
  ASSERT_GT(configured->context.size(), 0u);
  const std::size_t configured_n_used = configured->context.front().n_used;

  EXPECT_LT(configured_n_used, legacy_n_used)
      << "Configured prep must exclude the flagged quote the Legacy prep keeps "
         "(legacy n_used=" << legacy_n_used << " configured n_used="
      << configured_n_used << ")";
}

// FT-P (B4): the per-expiry preparation prepass fans out over `fit_workers` while
// the fit/scoring pass stays sequential, so the assembled surface must be
// BIT-IDENTICAL for any worker count. Determinism gate: 1 vs 8 workers must
// produce byte-for-byte identical eSSVI slices and per-expiry metrics.
TEST(SurfaceParity, PrepassParallelIsBitIdenticalAcrossWorkers) {
  const PanelBuild pb = make_panel_build();
  const auto panel = make_synthetic_american_panel(pb.spec);
  ASSERT_TRUE(panel.has_value()) << panel.error().to_string();

  Universe u;
  const auto uid = atx::vol::data_install(u, panel->frame);
  ASSERT_TRUE(uid.has_value()) << uid.error().to_string();
  const auto under = u.get_underlying(*uid);
  ASSERT_TRUE(under.has_value());

  const auto run_with = [&](unsigned workers) {
    SurfaceParityInputs in;
    in.S = pb.spec.spot;
    in.r = pb.spec.r;
    in.cash_divs = pb.spec.cash_divs;
    in.now_ts_ns = iso_to_ns(pb.snapshot);
    in.deam.hyb = pb.spec.hyb;
    in.deam.imply_borrow = true;
    in.deam.n_atm = 3;
    in.fit_workers = workers;
    return run_surface_parity(**under, in);
  };

  const auto r1 = run_with(1u);
  const auto r8 = run_with(8u);
  ASSERT_TRUE(r1.has_value()) << r1.error().to_string();
  ASSERT_TRUE(r8.has_value()) << r8.error().to_string();

  ASSERT_EQ(r1->n_slices, r8->n_slices);
  ASSERT_EQ(r1->expiry_T.size(), r8->expiry_T.size());
  EXPECT_EQ(r1->calendar_arb_free, r8->calendar_arb_free);
  // Byte-for-byte on the reported worst-case bid-ask fraction.
  EXPECT_EQ(r1->worst_frac_within_bidask, r8->worst_frac_within_bidask);

  const auto s1 = r1->surface.essvi_slices();
  const auto s8 = r8->surface.essvi_slices();
  ASSERT_EQ(s1.size(), s8.size());
  for (std::size_t i = 0; i < s1.size(); ++i) {
    EXPECT_EQ(r1->expiry_T[i], r8->expiry_T[i]) << "expiry_T slice " << i;
    // Raw eSSVI parameters must match bit-for-bit (==, not NEAR).
    EXPECT_EQ(s1[i].theta, s8[i].theta) << "theta slice " << i;
    EXPECT_EQ(s1[i].phi, s8[i].phi) << "phi slice " << i;
    EXPECT_EQ(s1[i].rho, s8[i].rho) << "rho slice " << i;
    EXPECT_EQ(s1[i].T, s8[i].T) << "T slice " << i;
    // The served surface at a k-grid must be bit-identical too.
    for (double k = -0.30; k <= 0.30 + 1e-9; k += 0.05) {
      const double iv1 = r1->surface.iv_on_slice(static_cast<std::uint16_t>(i), k);
      const double iv8 = r8->surface.iv_on_slice(static_cast<std::uint16_t>(i), k);
      EXPECT_EQ(iv1, iv8) << "iv_on_slice mismatch slice " << i << " k=" << k;
    }
  }
  // Per-slice used-observation counts identical.
  ASSERT_EQ(r1->context.size(), r8->context.size());
  for (std::size_t i = 0; i < r1->context.size(); ++i) {
    EXPECT_EQ(r1->context[i].n_used, r8->context[i].n_used) << "n_used slice " << i;
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

// ── D4 (T10c): served chi2 dof at the eSSVI-lane scoring site ───────────────

// The per-expiry parity score used to hardcode `n_curve_params = 3` — right for
// a backbone-only eSSVI slice, wrong for a residual-armed one (SPY-like /
// LiquidSingleName profiles keep `residual_disable = false`), whose served
// curve is 3 backbone + 4 HingeQuad coefficients. The dof is now read off the
// SLICE THE FINAL SURFACE SERVES (`essvi_slice_dof`), and the published report
// carries the dof `chain_parity` ACTUALLY consumed (`ParityReport::chi2_dof`,
// stamped inside chain_parity from the ParityInputs it was given) — so this
// test pins the served artifact, not a parallel re-derivation.
//
// Mutation checks (both performed): hardcoding the pin back to 3 reds the
// residual-armed half (chi2_dof reads 3 while the served slice carries 7);
// reading the dof anywhere but off the served slice cannot stay green for both
// the residual-off and residual-armed halves at once.
TEST(SurfaceParity, ServedChiSquareIsScoredAgainstTheServedSlicesOwnDof) {
  const PanelBuild pb = make_panel_build();
  const auto panel = make_synthetic_american_panel(pb.spec);
  ASSERT_TRUE(panel.has_value()) << panel.error().to_string();

  Universe u;
  const auto uid = atx::vol::data_install(u, panel->frame);
  ASSERT_TRUE(uid.has_value()) << uid.error().to_string();
  const auto under = u.get_underlying(*uid);
  ASSERT_TRUE(under.has_value());

  SurfaceParityInputs in;
  in.S = pb.spec.spot;
  in.r = pb.spec.r;
  in.cash_divs = pb.spec.cash_divs;
  in.now_ts_ns = iso_to_ns(pb.snapshot);
  in.deam.hyb = pb.spec.hyb;
  in.deam.imply_borrow = true;
  in.deam.n_atm = 3;

  // Residual OFF (the CalibOpts default): every served slice is backbone-only,
  // dof 3 — the value the old hardcode was accidentally right about. This half
  // is the regression guard that residual-off boards score bit-identically.
  {
    const auto res = run_surface_parity(**under, in);
    ASSERT_TRUE(res.has_value()) << res.error().to_string();
    ASSERT_EQ(res->per_expiry.size(), res->n_slices);
    for (std::size_t i = 0; i < res->per_expiry.size(); ++i) {
      EXPECT_EQ(atx::vol::essvi_slice_dof(res->surface.essvi_slices()[i]), std::size_t{3})
          << "slice " << i;
      EXPECT_EQ(res->per_expiry[i].chi2_dof, std::size_t{3}) << "slice " << i;
      EXPECT_FALSE(res->per_expiry[i].chi2_dof_underdetermined) << "slice " << i;
    }
  }

  // Residual ARMED (the SPY-like / LiquidSingleName production shape): the
  // served curve carries 4 extra fitted HingeQuad coefficients, and the score
  // must say so.
  {
    SurfaceParityInputs armed = in;
    armed.calib.residual_disable = false;
    armed.calib.residual_basis_kind = atx::vol::ResidualBasisKind::HingeQuad;
    armed.calib.residual_n_basis_terms = 5;
    const auto res = run_surface_parity(**under, armed);
    ASSERT_TRUE(res.has_value()) << res.error().to_string();
    ASSERT_EQ(res->per_expiry.size(), res->n_slices);
    std::size_t n_armed = 0;
    for (std::size_t i = 0; i < res->per_expiry.size(); ++i) {
      const atx::vol::EssviParams &served = res->surface.essvi_slices()[i];
      // The served artifact vs the served slice: equality per slice, whatever
      // the residual fit decided for that slice.
      EXPECT_EQ(res->per_expiry[i].chi2_dof, atx::vol::essvi_slice_dof(served)) << "slice " << i;
      EXPECT_FALSE(res->per_expiry[i].chi2_dof_underdetermined) << "slice " << i;
      EXPECT_GT(res->per_expiry[i].chi2_reduced, 0.0) << "slice " << i;  // W3-A: never blanked
      if (served.resid_scale > 0.0) {
        ++n_armed;
        EXPECT_EQ(res->per_expiry[i].chi2_dof, std::size_t{7}) << "slice " << i;
      }
    }
    // FIXTURE GUARD: at least one slice must actually arm the residual, or the
    // armed half degenerates to the dof-3 case and the mutation check above
    // could pass vacuously.
    ASSERT_GT(n_armed, std::size_t{0})
        << "fixture no longer arms the HingeQuad residual on any slice; the "
           "dof correction is not being exercised";
  }
}

// D4 (T10c), under-determined half. A residual-armed slice serves 7 fitted
// parameters; a thin (6-strike) expiry scores ~6 quotes, so N <= dof and a
// true reduced chi-square is undefined. The pre-D4 shape — chain_parity's
// error propagating through ATX_TRY — failed the WHOLE BOARD, discarding band
// evidence that does not depend on dof at all (the D1 defect: no measurement
// laundered as a measured failure). The site now RE-SCORES with dof = 0
// (chi2 per observation), keeps the band evidence, and marks the report
// `chi2_dof_underdetermined` instead of blanking chi2 to a perfect-looking
// 0.0 (W3-A).
//
// Mutation checks (both performed): hardcoding the pin back to 3 reds this
// test (6 > 3 scores "fine", so the flag never sets); removing the dof-0
// re-score reds it harder (run_surface_parity fails outright).
TEST(SurfaceParity, UnderdeterminedChiSquareIsFlaggedAndBandEvidenceSurvives) {
  PanelBuild pb = make_panel_build();
  // Thin the board to 6 strikes per expiry: enough to clear the 5-row prepared
  // fit floor, too few to support 7 fitted parameters at scoring.
  pb.spec.strikes.clear();
  for (double K = 88.0; K <= 108.0 + 1e-9; K += 4.0) {
    pb.spec.strikes.push_back(K);  // 88, 92, 96, 100, 104, 108
  }
  const auto panel = make_synthetic_american_panel(pb.spec);
  ASSERT_TRUE(panel.has_value()) << panel.error().to_string();

  Universe u;
  const auto uid = atx::vol::data_install(u, panel->frame);
  ASSERT_TRUE(uid.has_value()) << uid.error().to_string();
  const auto under = u.get_underlying(*uid);
  ASSERT_TRUE(under.has_value());

  SurfaceParityInputs in;
  in.S = pb.spec.spot;
  in.r = pb.spec.r;
  in.cash_divs = pb.spec.cash_divs;
  in.now_ts_ns = iso_to_ns(pb.snapshot);
  in.deam.hyb = pb.spec.hyb;
  in.deam.imply_borrow = true;
  in.deam.n_atm = 3;
  in.calib.residual_disable = false;
  in.calib.residual_basis_kind = atx::vol::ResidualBasisKind::HingeQuad;
  in.calib.residual_n_basis_terms = 5;

  const auto res = run_surface_parity(**under, in);
  ASSERT_TRUE(res.has_value()) << res.error().to_string();
  ASSERT_GT(res->n_slices, std::size_t{0});
  ASSERT_EQ(res->per_expiry.size(), res->n_slices);

  std::size_t n_underdetermined = 0;
  for (std::size_t i = 0; i < res->per_expiry.size(); ++i) {
    const atx::vol::ParityReport &parity = res->per_expiry[i];
    const std::size_t served_dof = atx::vol::essvi_slice_dof(res->surface.essvi_slices()[i]);
    if (parity.chi2_dof_underdetermined) {
      ++n_underdetermined;
      // FIXTURE COHERENCE: the flag may only fire when the scored population
      // truly cannot support the served dof.
      EXPECT_LE(parity.n, served_dof) << "slice " << i;
      // Re-scored with dof 0 — chi2 is chi2/N, defined, and NOT blanked.
      EXPECT_EQ(parity.chi2_dof, std::size_t{0}) << "slice " << i;
      EXPECT_GT(parity.chi2_reduced, 0.0) << "slice " << i;
    } else {
      EXPECT_EQ(parity.chi2_dof, served_dof) << "slice " << i;
    }
    // The band evidence survives either way: the population scored, and the
    // fraction is a real measurement on this clean synthetic board.
    EXPECT_GT(parity.n, std::size_t{0}) << "slice " << i;
    EXPECT_GT(parity.frac_fv_within_bidask, 0.0) << "slice " << i;
  }
  // FIXTURE GUARD: the thin board must actually exercise the N <= dof path, or
  // this test quietly becomes the well-posed test above.
  ASSERT_GT(n_underdetermined, std::size_t{0})
      << "fixture no longer produces an under-determined slice; thin the board "
         "or re-arm the residual";
}
