// R1 (2026-08-23 top-of-book fit-refusal diagnosis, §4) — a board whose RISK
// surface the independent geometry oracle refuses must still be able to serve a
// MARK-grade surface (fair value / fair vol / greeks), and must be visibly
// labelled a mark when it does.
//
// Three claims, each pinned here:
//
//   1. THE CONTRACT. `symbol_config_from_preset` on a Risk-purpose preset asks
//      for BOTH outputs (`SurfaceOutputs::MarketMarkAndRisk`), not Risk alone,
//      so `PricerFitter::fit` actually builds the mark arm
//      (pricer_fitter.cpp: `if (has_output(requested_outputs,
//      SurfacePurpose::MarketMark))`). `risk_admission` stays `Required` — the
//      risk contract is UNCHANGED; the request is widened, never weakened.
//      The widened value round-trips through the 256-byte `DbSymbolRecord`
//      with NO on-disk format change: `outputs` already lives in the
//      `DbSurfacePolicyRecord` embedded in that record's reserved region and
//      its validator already admits 1..3 (surface_db.cpp).
//
//   2. THE RESCUE. `fit_board` no longer discards a healthy mark when
//      `PricerFitter::fit` returns the risk `Err` (corpus_board_fit.cpp). The
//      slot is served, tagged `SurfacePurpose::MarketMark` in its provenance —
//      which is what reaches the archive — and flagged
//      `mark_after_risk_refusal`, with the risk refusal text retained verbatim.
//
//   3. NO SILENT DOWNGRADE. A caller that asks for Risk ONLY still gets the
//      refusal: no mark is built, so there is nothing to substitute.
//
// The refusal is produced the same way pricer_fitter_test's
// `MarkBidAskFloorRetainsParityScoringByDefault` produces one: squeeze every
// quote's band to a hair around its OWN mid. The mids — and therefore the board
// the fitter sees — are untouched, so the fit is unchanged; only the band the
// publish-floor diagnostic measures against becomes unreachably tight. That is
// exactly the shape §4.3 of the diagnosis measured on MSFT/GOOGL: a risk
// candidate refused `QualityBelowFloor` over a board that is perfectly
// serviceable as a mark.

#include <gtest/gtest.h>

#include <cmath>
#include <cstddef>
#include <string>
#include <vector>

#include "atx/vol/api/backtest/panel.hpp"        // SynthPanelSpec, make_synthetic_american_panel
#include "atx/vol/api/backtest/priced_surface.hpp" // PricedSurface
#include "atx/vol/api/core/market_env.hpp"      // MarketEnv
#include "atx/vol/api/fitting/pricer_fitter.hpp"  // PricerConfig, PricerFitter
#include "atx/vol/api/fitting/s3.hpp"           // S3Params
#include "atx/vol/api/fitting/surface_policy.hpp" // SurfaceOutputs, SurfacePurpose
#include "atx/vol/api/marketdata/corpus.hpp"    // CorpusBoard, CorpusFitStatus
#include "atx/vol/api/marketdata/data.hpp"      // iso_to_ns, year_fraction
#include "atx/vol/api/storage/surface_db.hpp"   // SymbolFitConfig, symbol_config_from_preset
#include "atx/vol/tools/surface_db_populate.hpp"  // populate_admission_policy
#include "marketdata/corpus_board_fit.hpp"      // FitSlot, fit_board (src-private)

namespace {

using namespace atx::vol;

// A small three-expiry board: enough for a real eSSVI/calendar fit, small
// enough that this suite stays in the fast lane.
[[nodiscard]] SynthPanelSpec make_small_spec() {
  SynthPanelSpec spec;
  spec.uid = "SYN";
  spec.snapshot_iso = "2026-06-19";
  spec.spot = 100.0;
  spec.r = 0.043;
  spec.borrow = 0.0;
  struct Row {
    const char *iso;
    double sigma0;
    double skew_k;
    double c2;
  };
  const Row rows[] = {
      {"2026-07-17", 0.22, -0.55, 0.30},
      {"2026-08-21", 0.24, -0.52, 0.35},
      {"2026-09-18", 0.26, -0.50, 0.40},
  };
  for (const Row &r : rows) {
    SynthExpiry e;
    e.expiry_iso = r.iso;
    e.T = year_fraction(spec.snapshot_iso, r.iso);
    e.truth = S3Params{r.sigma0, 2.0 * std::sqrt(e.T) * r.skew_k, r.c2};
    spec.expiries.push_back(e);
  }
  for (double K = 88.0; K <= 112.0 + 1e-9; K += 3.0) {
    spec.strikes.push_back(K);
  }
  spec.half_spread_frac = 0.02;
  spec.min_half_spread = 0.02;
  return spec;
}

// `squeeze`: collapse every band onto its own mid. Mids untouched => the fitter
// sees the same board; the publish floor's in-band evidence collapses.
[[nodiscard]] CorpusBoard make_board(bool squeeze) {
  const SynthPanelSpec spec = make_small_spec();
  auto panel = make_synthetic_american_panel(spec);
  EXPECT_TRUE(panel.has_value()) << (panel ? "" : panel.error().to_string());
  CorpusBoard b;
  b.date = "2026-06-19";
  b.symbol = "SYN";
  if (panel.has_value()) {
    b.frame = panel->frame;
  }
  if (squeeze) {
    for (QuoteRow &row : b.frame.rows) {
      const double mid = 0.5 * (row.bid + row.ask);
      row.bid = mid * (1.0 - 1.0e-6);
      row.ask = mid * (1.0 + 1.0e-6);
    }
  }
  b.env = MarketEnv::flat(spec.spot, spec.r, iso_to_ns(spec.snapshot_iso), spec.cash_divs);
  return b;
}

// Populate's own fit contract, resolved through the same seam the production
// path uses (`symbol_config_from_preset` -> `pricer_config_for_symbol`), so
// this test cannot drift from what the pipeline actually requests.
[[nodiscard]] PricerConfig populate_style_config(SurfaceOutputs outputs) {
  const SymbolFitConfig cfg = symbol_config_from_preset(FitPreset::Populate);
  PricerConfig out;
  out.preset = cfg.preset;
  out.quality_mode = cfg.surface_policy.quality_mode;
  out.outputs = outputs;
  out.risk_admission = cfg.surface_policy.risk_admission;
  out.fallback = cfg.surface_policy.fallback;
  out.use_correction_cache = cfg.use_correction_cache;
  out.score_parity = true; // the floor must be able to read its own evidence
  out.enforce_calendar_floor = cfg.enforce_calendar_floor;
  out.use_deam_cache_for_fit = cfg.use_deam_cache_for_fit;
  out.admission = populate_admission_policy();
  return out;
}

[[nodiscard]] FitSlot fit(const CorpusBoard &board, const PricerConfig &cfg, unsigned fit_workers) {
  return fit_board(board, cfg, /*admission=*/nullptr, [fit_workers](SessionInputs &in) {
    in.fit_workers = fit_workers;
  });
}

} // namespace

// ── 1. The contract ─────────────────────────────────────────────────────────

TEST(MarkRescue, PopulatePresetRequestsBothOutputsAndKeepsRiskAdmissionRequired) {
  const SymbolFitConfig cfg = symbol_config_from_preset(FitPreset::Populate);
  EXPECT_EQ(cfg.surface_policy.outputs, SurfaceOutputs::MarketMarkAndRisk);
  EXPECT_TRUE(cfg.surface_policy.requests(SurfacePurpose::MarketMark));
  EXPECT_TRUE(cfg.surface_policy.requests(SurfacePurpose::Risk));
  // The risk half of the contract is untouched: still mandatory admission,
  // still the last-known-good fallback.
  EXPECT_EQ(cfg.surface_policy.risk_admission, RiskAdmission::Required);
  EXPECT_EQ(cfg.surface_policy.fallback, SurfaceFallback::LastKnownGood);
}

TEST(MarkRescue, EveryRiskPurposePresetRequestsTheMarkToo) {
  for (const FitPreset preset : {FitPreset::Fast, FitPreset::Accurate, FitPreset::Robust,
                                 FitPreset::Bulk, FitPreset::Populate}) {
    const SymbolFitConfig cfg = symbol_config_from_preset(preset);
    EXPECT_EQ(cfg.surface_policy.outputs, SurfaceOutputs::MarketMarkAndRisk)
        << "preset " << static_cast<int>(preset);
    EXPECT_EQ(cfg.surface_policy.risk_admission, RiskAdmission::Required)
        << "preset " << static_cast<int>(preset);
  }
  // Hft is a MARK request and stays one — widening a risk request must not
  // promote a mark-only consumer into a risk one.
  const SymbolFitConfig hft = symbol_config_from_preset(FitPreset::Hft);
  EXPECT_EQ(hft.surface_policy.outputs, SurfaceOutputs::MarketMark);
  EXPECT_EQ(hft.surface_policy.risk_admission, RiskAdmission::NotApplicable);
}

// ── 2. The rescue ───────────────────────────────────────────────────────────

TEST(MarkRescue, RiskRefusedBoardStillYieldsAMarkSurface) {
  const CorpusBoard board = make_board(/*squeeze=*/true);
  const FitSlot slot = fit(board, populate_style_config(SurfaceOutputs::MarketMarkAndRisk), 1u);

  ASSERT_EQ(slot.status, CorpusFitStatus::Ok)
      << "code=" << static_cast<int>(slot.error_code) << " msg=" << slot.error_message;
  EXPECT_TRUE(slot.mark_after_risk_refusal);
  ASSERT_TRUE(slot.surface.has_value());
  EXPECT_GT(slot.surface->n_slices(), 0u);

  // The label. `provenance` is what reaches the archive, so this is the field a
  // downstream consumer reads to know it is NOT holding a risk surface.
  ASSERT_TRUE(slot.provenance.has_value());
  EXPECT_EQ(slot.provenance->purpose, SurfacePurpose::MarketMark);
  EXPECT_EQ(slot.provenance->state, SurfaceState::Healthy);

  // The refusal is retained, not swallowed: an operator must still be able to
  // read WHY the risk candidate was rejected.
  EXPECT_NE(slot.error_message.find("risk surface rejected"), std::string::npos)
      << slot.error_message;
  EXPECT_NE(slot.error_message.find("QualityBelowFloor"), std::string::npos)
      << slot.error_message;
}

TEST(MarkRescue, AnAdmittedRiskBoardIsStillServedAsRisk) {
  const CorpusBoard board = make_board(/*squeeze=*/false);
  const FitSlot slot = fit(board, populate_style_config(SurfaceOutputs::MarketMarkAndRisk), 1u);

  ASSERT_EQ(slot.status, CorpusFitStatus::Ok)
      << "code=" << static_cast<int>(slot.error_code) << " msg=" << slot.error_message;
  EXPECT_FALSE(slot.mark_after_risk_refusal);
  ASSERT_TRUE(slot.provenance.has_value());
  EXPECT_EQ(slot.provenance->purpose, SurfacePurpose::Risk);
  EXPECT_TRUE(slot.error_message.empty()) << slot.error_message;
}

// ── 3. No silent downgrade ──────────────────────────────────────────────────

TEST(MarkRescue, RiskOnlyRequestKeepsItsRefusal) {
  const CorpusBoard board = make_board(/*squeeze=*/true);
  const FitSlot slot = fit(board, populate_style_config(SurfaceOutputs::Risk), 1u);

  EXPECT_EQ(slot.status, CorpusFitStatus::Failed);
  EXPECT_FALSE(slot.mark_after_risk_refusal);
  EXPECT_FALSE(slot.surface.has_value());
  EXPECT_NE(slot.error_message.find("risk surface rejected"), std::string::npos)
      << slot.error_message;
}

// ── 4. Determinism ──────────────────────────────────────────────────────────

TEST(MarkRescue, MarkRescueIsBitIdenticalAcrossFitWorkerCounts) {
  const CorpusBoard board = make_board(/*squeeze=*/true);
  const PricerConfig cfg = populate_style_config(SurfaceOutputs::MarketMarkAndRisk);
  const FitSlot serial = fit(board, cfg, 1u);
  const FitSlot parallel = fit(board, cfg, 8u);

  ASSERT_EQ(serial.status, CorpusFitStatus::Ok) << serial.error_message;
  ASSERT_EQ(parallel.status, CorpusFitStatus::Ok) << parallel.error_message;
  ASSERT_TRUE(serial.mark_after_risk_refusal);
  ASSERT_TRUE(parallel.mark_after_risk_refusal);
  ASSERT_TRUE(serial.surface.has_value());
  ASSERT_TRUE(parallel.surface.has_value());
  ASSERT_EQ(serial.surface->n_slices(), parallel.surface->n_slices());
  ASSERT_TRUE(serial.provenance.has_value());
  ASSERT_TRUE(parallel.provenance.has_value());
  EXPECT_EQ(serial.provenance->purpose, parallel.provenance->purpose);
  EXPECT_EQ(serial.provenance->state, parallel.provenance->state);
  EXPECT_EQ(serial.error_message, parallel.error_message);

  // BIT-identical, not merely close: the same query grid on both surfaces.
  ASSERT_EQ(serial.surface->context().size(), parallel.surface->context().size());
  for (std::size_t s = 0; s < serial.surface->n_slices(); ++s) {
    const double T = serial.surface->context()[s].T;
    EXPECT_DOUBLE_EQ(T, parallel.surface->context()[s].T);
    for (const double m : {0.90, 0.95, 1.00, 1.05, 1.10}) {
      const double K = 100.0 * m;
      EXPECT_DOUBLE_EQ(serial.surface->iv(K, T), parallel.surface->iv(K, T))
          << "slice " << s << " K " << K;
    }
  }
}

// ── 5. A malformed REQUEST is not a refused board ───────────────────────────

TEST(MarkRescue, AnInvalidRiskPolicyStillFailsAndIsNotPaperedOverWithAMark) {
  // A LinearVariance pin under a mandatory risk admission is a REQUEST defect:
  // `PricerFitter::fit` refuses it in input validation with
  // Err(InvalidArgument, "invalid correctness policy for requested risk
  // surface") — after publishing a perfectly healthy mark. The mark is real, but
  // serving it would hide an operator misconfiguration behind a green cell, so
  // the rescue is gated on `ErrorCode::Unavailable` (a REFUSED CANDIDATE) and
  // this must still fail.
  const CorpusBoard board = make_board(/*squeeze=*/false);
  PricerConfig cfg = populate_style_config(SurfaceOutputs::MarketMarkAndRisk);
  cfg.curve = CurveConfig{VolCurveKind::LinearVariance};
  const FitSlot slot = fit(board, cfg, 1u);

  EXPECT_EQ(slot.status, CorpusFitStatus::Failed);
  EXPECT_FALSE(slot.mark_after_risk_refusal);
  EXPECT_FALSE(slot.surface.has_value());
  EXPECT_EQ(slot.error_code, ErrorCode::InvalidArgument);
  EXPECT_EQ(slot.error_message, "invalid correctness policy for requested risk surface")
      << slot.error_message;
}
