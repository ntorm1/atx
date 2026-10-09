// Task 2 — the populate publish gate: the dormant
// FitAdmissionPolicy::min_worst_frac_within_bidask floor (fit_policy.hpp:128,
// default 0.0 = disabled; enforced at fit_policy.cpp:144-149) is wired to a
// conservative 0.35 for every surface-db populate fit.

#include <gtest/gtest.h>

#include "atx/vol/api/fitting/fit_policy.hpp"
#include "atx/vol/tools/surface_db_populate.hpp"

namespace {

using atx::vol::evaluate_surface_admission;
using atx::vol::fit_admission_consumes_parity;
using atx::vol::FitAdmissionPolicy;
using atx::vol::has_admission_failure;
using atx::vol::kPopulateMinWorstFracInBand;
using atx::vol::ParityDiagnosticState;
using atx::vol::populate_admission_policy;
using atx::vol::SurfaceAdmissionEvidence;
using atx::vol::SurfaceAdmissionReason;

// Evidence for a structurally healthy fitted board; only the in-band figure
// varies per test.
[[nodiscard]] SurfaceAdmissionEvidence healthy_evidence() {
  SurfaceAdmissionEvidence e{};
  e.attempted_expiries = 10u;
  e.fitted_expiries = 10u;
  e.attempted_quotes = 600u;
  e.fitted_quotes = 500u;
  e.front_expiry_fitted = true;
  e.finite_diagnostics = true;
  e.calendar_arb_free = true;
  e.finite_iv_domain = true;
  e.european_price_bounds = true;
  e.strike_monotone = true;
  e.strike_convex = true;
  e.calendar_total_variance = true;
  e.forward_variance_nonnegative = true;
  e.parity_state = ParityDiagnosticState::Valid;
  return e;
}

} // namespace

TEST(SurfaceDbPopulatePolicy, FloorIsWiredAndEverythingElseIsDefault) {
  const FitAdmissionPolicy policy = populate_admission_policy();
  EXPECT_DOUBLE_EQ(policy.min_worst_frac_within_bidask, kPopulateMinWorstFracInBand);
  EXPECT_DOUBLE_EQ(kPopulateMinWorstFracInBand, 0.35);
  // A floored Mark policy consumes the re-Americanized parity diagnostics, so
  // score_parity is forced on and an unscored board fails closed.
  EXPECT_TRUE(fit_admission_consumes_parity(policy));
  // TIGHTENING ONLY: every other admission field keeps its WP12 default.
  const FitAdmissionPolicy dflt{};
  EXPECT_EQ(policy.enabled, dflt.enabled);
  EXPECT_EQ(policy.consumer, dflt.consumer);
  EXPECT_EQ(policy.min_fitted_expiries, dflt.min_fitted_expiries);
  EXPECT_DOUBLE_EQ(policy.min_expiry_coverage, dflt.min_expiry_coverage);
  EXPECT_DOUBLE_EQ(policy.min_quote_coverage, dflt.min_quote_coverage);
  EXPECT_EQ(policy.require_front_expiry, dflt.require_front_expiry);
  EXPECT_EQ(policy.max_consecutive_expiry_gaps, dflt.max_consecutive_expiry_gaps);
  EXPECT_EQ(policy.require_calendar_arb_free, dflt.require_calendar_arb_free);
}

TEST(SurfaceDbPopulatePolicy, FloorRefusesTheAprilTenthSignatureAndAdmitsCalm) {
  SurfaceAdmissionEvidence evidence = healthy_evidence();

  evidence.worst_frac_within_bidask = 0.05; // 2025-04-10 worst long slice
  const auto refused = evaluate_surface_admission(evidence, populate_admission_policy());
  EXPECT_FALSE(refused.admitted);
  EXPECT_TRUE(has_admission_failure(refused, SurfaceAdmissionReason::QualityBelowFloor));

  evidence.worst_frac_within_bidask = 0.55; // calm-day long-end LOWER bound
  const auto admitted = evaluate_surface_admission(evidence, populate_admission_policy());
  EXPECT_TRUE(admitted.admitted);
}
