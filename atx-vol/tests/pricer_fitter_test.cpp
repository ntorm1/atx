#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "atx/core/error.hpp"
#include "atx/vol/chain.hpp"
#include "atx/vol/counters.hpp"
#include "atx/vol/data.hpp"
#include "atx/vol/panel.hpp"
#include "atx/vol/parallel_for.hpp" // atx_auto_worker_count (ATX_VOL_FIT_WORKERS gate)
#include "atx/vol/pricer_fitter.hpp"
#include "atx/vol/pricing_executor.hpp"
#include "atx/vol/risk_surface_validation.hpp"
#include "atx/vol/session.hpp"
#include "atx/vol/spy_fixture.hpp"
#include "atx/vol/surface_archive.hpp"
#include "atx/vol/types.hpp"
#include "atx/vol/vol_curve.hpp"

// Acceptance harness for the unified library layer (chain.hpp + pricer_fitter.hpp):
// the OptionChain -> PricerFitter -> parallel value_chain lifecycle over the SPY
// known-truth fixture. Proves the id-addressed chain enumerate/decode/update, that
// fit stores the surface, that value_chain fields equal the VolaSession scalar
// queries, and that the parallel evaluator is bit-identical across thread counts.

namespace {

using atx::vol::ChainValuation;
using atx::vol::FitPreset;
using atx::vol::FittedSurface;
using atx::vol::make_spy_synthetic_spec;
using atx::vol::make_synthetic_american_panel;
using atx::vol::OptionChain;
using atx::vol::OptionId;
using atx::vol::OutputField;
using atx::vol::PricerConfig;
using atx::vol::PricerFitter;
using atx::vol::QueryPricingTier;
using atx::vol::Side;
using atx::vol::SynthPanelSpec;
using atx::vol::VolaSession;

PricerConfig essvi_config() {
  PricerConfig config;
  config.preset = FitPreset::Fast;
  config.curve = atx::vol::CurveConfig{atx::vol::VolCurveKind::Essvi};
  return config;
}

void replace_expiry_quotes(OptionChain &destination, const OptionChain &source,
                           atx::vol::ExpiryId expiry_id) {
  std::vector<OptionId> ids;
  std::vector<double> bids;
  std::vector<double> asks;
  for (const OptionId id : destination.ids()) {
    if (atx::vol::cid_expiry(id) != expiry_id) {
      continue;
    }
    const auto quote = source.at(id);
    ASSERT_TRUE(quote.has_value()) << quote.error().to_string();
    ids.push_back(id);
    bids.push_back(quote->bid);
    asks.push_back(quote->ask);
  }
  ASSERT_FALSE(ids.empty());
  ASSERT_TRUE(destination.update_quotes(ids, bids, asks).has_value());
}

atx::vol::Result<OptionChain> make_chain_from_spec(const SynthPanelSpec &spec) {
  auto panel = make_synthetic_american_panel(spec);
  if (!panel.has_value()) {
    return atx::core::Err(panel.error());
  }
  return OptionChain::from_frame(panel->frame, spec.r, spec.spot);
}

// NaN-aware equality: two failed/absent cells (both NaN) compare equal, so the
// determinism check does not trip on the (deterministic) NaN slots.
bool same(double a, double b) {
  if (std::isnan(a) && std::isnan(b)) {
    return true;
  }
  return a == b;
}

// rfx task 5a (perf C1 dedup): portable ATX_VOL_FIT_WORKERS env set/unset, the
// same mechanism curve_fit_parallel_test.cpp's S0-4' gate uses to force the
// de-Am prepass worker count without a session-level knob (VolaSession::build
// always drives fit_curve_surface with fit_workers=0 / auto).
#if defined(_MSC_VER)
void set_fit_workers_env(const char *value) { ::_putenv_s("ATX_VOL_FIT_WORKERS", value); }
void unset_fit_workers_env() { ::_putenv_s("ATX_VOL_FIT_WORKERS", ""); }
#else
void set_fit_workers_env(const char *value) { ::setenv("ATX_VOL_FIT_WORKERS", value, 1); }
void unset_fit_workers_env() { ::unsetenv("ATX_VOL_FIT_WORKERS"); }
#endif

// RAII guard: restores the prior ATX_VOL_FIT_WORKERS value on scope exit, even
// if an ASSERT_* below exits the test early.
class FitWorkersEnvGuard {
public:
  FitWorkersEnvGuard() {
#if defined(_MSC_VER)
    char *prev = nullptr;
    std::size_t prev_n = 0;
    had_prev_ = (::_dupenv_s(&prev, &prev_n, "ATX_VOL_FIT_WORKERS") == 0) && (prev != nullptr);
    if (prev != nullptr) {
      prev_val_ = prev;
      std::free(prev);
    }
#else
    const char *prev = std::getenv("ATX_VOL_FIT_WORKERS");
    had_prev_ = prev != nullptr;
    if (prev != nullptr) {
      prev_val_ = prev;
    }
#endif
  }
  FitWorkersEnvGuard(const FitWorkersEnvGuard &) = delete;
  FitWorkersEnvGuard &operator=(const FitWorkersEnvGuard &) = delete;
  ~FitWorkersEnvGuard() {
    if (had_prev_) {
      set_fit_workers_env(prev_val_.c_str());
    } else {
      unset_fit_workers_env();
    }
  }

private:
  bool had_prev_ = false;
  std::string prev_val_;
};

// rfx task 5a: field-wise bit-identical checks for the certification
// diagnostics the perf-C1 dedup consumes from the parallel de-Am prepass
// instead of a serial re-derivation. Mirrors curve_fit_parallel_test.cpp's
// expect_per_expiry_bit_identical (EXPECT_EQ throughout -- bit-exact, not a
// tolerance check).
void expect_route_bit_identical(const atx::vol::InversionRouteDiagnostics &a,
                                const atx::vol::InversionRouteDiagnostics &b, const char *label) {
  EXPECT_EQ(a.n_proposed, b.n_proposed) << label;
  EXPECT_EQ(a.n_audited, b.n_audited) << label;
  EXPECT_EQ(a.n_reference_reprices, b.n_reference_reprices) << label;
  EXPECT_EQ(a.n_accepted, b.n_accepted) << label;
  EXPECT_EQ(a.n_fallback, b.n_fallback) << label;
  EXPECT_EQ(a.p50_residual_half_spreads, b.p50_residual_half_spreads) << label;
  EXPECT_EQ(a.p95_residual_half_spreads, b.p95_residual_half_spreads) << label;
  EXPECT_EQ(a.max_residual_half_spreads, b.max_residual_half_spreads) << label;
}

void expect_inversion_bit_identical(const atx::vol::DeAmAuditDiagnostics &a,
                                    const atx::vol::DeAmAuditDiagnostics &b) {
  expect_route_bit_identical(a.shortcut, b.shortcut, "shortcut");
  expect_route_bit_identical(a.cache, b.cache, "cache");
  expect_route_bit_identical(a.fast, b.fast, "fast");
  expect_route_bit_identical(a.accurate, b.accurate, "accurate");
  EXPECT_EQ(a.n_forced_short_tenor, b.n_forced_short_tenor);
  EXPECT_EQ(a.n_forced_low_vega, b.n_forced_low_vega);
  EXPECT_EQ(a.n_forced_far_wing, b.n_forced_far_wing);
  EXPECT_EQ(a.n_accurate_fallback, b.n_accurate_fallback);
  EXPECT_EQ(a.n_rejected_residual, b.n_rejected_residual);
  EXPECT_EQ(a.n_deam_rows, b.n_deam_rows);
  EXPECT_EQ(a.n_deam_accepted, b.n_deam_accepted);
  EXPECT_EQ(a.n_shared_boundary_lanes, b.n_shared_boundary_lanes);
  EXPECT_EQ(a.n_shared_call_lanes, b.n_shared_call_lanes);
  EXPECT_EQ(a.n_shared_put_lanes, b.n_shared_put_lanes);
  EXPECT_EQ(a.n_shared_boundary_solves, b.n_shared_boundary_solves);
  EXPECT_EQ(a.n_shared_sentinel_reprices, b.n_shared_sentinel_reprices);
  EXPECT_EQ(a.n_shared_scalar_fallback_lanes, b.n_shared_scalar_fallback_lanes);
}

void expect_carry_bit_identical(const atx::vol::SessionCarryDiagnostics &a,
                                const atx::vol::SessionCarryDiagnostics &b) {
  EXPECT_EQ(a.n_candidates, b.n_candidates);
  EXPECT_EQ(a.n_attempted, b.n_attempted);
  EXPECT_EQ(a.n_solved, b.n_solved);
  EXPECT_EQ(a.n_retained, b.n_retained);
  EXPECT_EQ(a.effective_pair_count, b.effective_pair_count);
  EXPECT_EQ(a.dispersion, b.dispersion);
  EXPECT_EQ(a.max_leave_one_out_shift, b.max_leave_one_out_shift);
  EXPECT_EQ(a.confidence_half_width, b.confidence_half_width);
  EXPECT_EQ(a.max_pcp_residual, b.max_pcp_residual);
  EXPECT_EQ(a.available, b.available);
  EXPECT_EQ(a.confident, b.confident);
}

void expect_slice_diagnostics_bit_identical(std::span<const atx::vol::SessionSliceDiagnostics> a,
                                            std::span<const atx::vol::SessionSliceDiagnostics> b) {
  ASSERT_EQ(a.size(), b.size());
  for (std::size_t i = 0; i < a.size(); ++i) {
    SCOPED_TRACE("slice " + std::to_string(i));
    EXPECT_EQ(a[i].T, b[i].T);
    expect_carry_bit_identical(a[i].carry, b[i].carry);
    expect_inversion_bit_identical(a[i].inversion, b[i].inversion);
    EXPECT_EQ(a[i].inversion_available, b[i].inversion_available);
    EXPECT_EQ(a[i].inversion_certified, b[i].inversion_certified);
  }
}

void expect_session_diagnostics_bit_identical(const atx::vol::SessionDiagnostics &a,
                                              const atx::vol::SessionDiagnostics &b) {
  EXPECT_EQ(a.worst_frac_within_bidask, b.worst_frac_within_bidask);
  EXPECT_EQ(a.mean_frac_within_bidask, b.mean_frac_within_bidask);
  EXPECT_EQ(a.mean_chi2_reduced, b.mean_chi2_reduced);
  EXPECT_EQ(a.mean_rmse_vol, b.mean_rmse_vol);
  EXPECT_EQ(a.calendar_arb_free, b.calendar_arb_free);
  EXPECT_EQ(a.n_calendar_viol_pre, b.n_calendar_viol_pre);
  EXPECT_EQ(a.n_slices, b.n_slices);
  EXPECT_EQ(a.n_quotes, b.n_quotes);
  EXPECT_EQ(a.n_carry_slices, b.n_carry_slices);
  EXPECT_EQ(a.n_carry_confident, b.n_carry_confident);
  EXPECT_EQ(a.n_carry_skipped_expiries, b.n_carry_skipped_expiries);
  EXPECT_EQ(a.n_audit_starved_expiries, b.n_audit_starved_expiries);
  EXPECT_EQ(a.n_price_bound_violations, b.n_price_bound_violations);
  EXPECT_EQ(a.min_carry_effective_pairs, b.min_carry_effective_pairs);
  EXPECT_EQ(a.max_carry_dispersion, b.max_carry_dispersion);
  EXPECT_EQ(a.max_carry_leave_one_out, b.max_carry_leave_one_out);
  EXPECT_EQ(a.n_inversion_slices, b.n_inversion_slices);
  EXPECT_EQ(a.n_iv_proposed, b.n_iv_proposed);
  EXPECT_EQ(a.n_iv_audited, b.n_iv_audited);
  EXPECT_EQ(a.n_iv_fallback, b.n_iv_fallback);
  EXPECT_EQ(a.n_iv_rejected_residual, b.n_iv_rejected_residual);
  EXPECT_EQ(a.max_iv_proposal_residual_half_spreads, b.max_iv_proposal_residual_half_spreads);
  EXPECT_EQ(a.carry_confident, b.carry_confident);
  EXPECT_EQ(a.inversion_certified, b.inversion_certified);
}

class PricerFitterTest : public ::testing::Test {
protected:
  void SetUp() override {
    const SynthPanelSpec spec = make_spy_synthetic_spec();
    r_ = spec.r;
    spot_ = spec.spot;
    auto panel = make_synthetic_american_panel(spec);
    ASSERT_TRUE(panel.has_value()) << panel.error().message();
    auto ch = OptionChain::from_frame(panel->frame, spec.r, spec.spot);
    ASSERT_TRUE(ch.has_value()) << ch.error().message();
    chain_.emplace(std::move(*ch));
  }

  std::optional<OptionChain> chain_;
  double r_{0.0};
  double spot_{0.0};
};

TEST(PricerFitterPolicy, ProductionSelectorDefaultsToBroadCoverageEssviOnly) {
  const PricerConfig config;
  ASSERT_EQ(config.selector.candidates.size(), 1u);
  EXPECT_EQ(config.selector.candidates.front().kind, atx::vol::VolCurveKind::Essvi);
}

TEST_F(PricerFitterTest, ChainEnumerateDecodeAndSnapshot) {
  const OptionChain &chain = *chain_;
  EXPECT_EQ(chain.spot(), spot_);
  EXPECT_EQ(chain.rate(), r_);

  const std::vector<OptionId> ids = chain.ids();
  ASSERT_FALSE(ids.empty());
  EXPECT_EQ(ids.size(), chain.size());

  bool saw_call = false;
  bool saw_put = false;
  for (const OptionId id : ids) {
    const auto ref = chain.at(id);
    ASSERT_TRUE(ref.has_value());
    EXPECT_GT(ref->strike, 0.0);
    EXPECT_GT(ref->T, 0.0);
    saw_call = saw_call || (ref->side == Side::Call);
    saw_put = saw_put || (ref->side == Side::Put);
  }
  EXPECT_TRUE(saw_call);
  EXPECT_TRUE(saw_put);

  // An id that does not decode to a known leg is rejected.
  EXPECT_FALSE(chain.at(OptionId{0}).has_value());
}

TEST_F(PricerFitterTest, ChainSnapshotMatchesStableIdDecodeOrder) {
  const OptionChain &chain = *chain_;
  const atx::vol::ChainSnapshot snap = chain.snapshot();
  ASSERT_EQ(snap.size(), chain.size());
  ASSERT_EQ(snap.ids, chain.ids());
  for (std::size_t i = 0; i < snap.size(); ++i) {
    const auto ref = chain.at(snap.ids[i]);
    ASSERT_TRUE(ref.has_value());
    EXPECT_DOUBLE_EQ(snap.T[i], ref->T);
    EXPECT_DOUBLE_EQ(snap.strike[i], ref->strike);
    EXPECT_DOUBLE_EQ(snap.bid[i], ref->bid);
    EXPECT_DOUBLE_EQ(snap.ask[i], ref->ask);
    EXPECT_DOUBLE_EQ(snap.mid[i], ref->mid);
    EXPECT_EQ(snap.side[i], ref->side);
  }
}

TEST_F(PricerFitterTest, UpdateQuotesReplacesBidAsk) {
  OptionChain &chain = *chain_;
  const OptionId id = chain.ids().front();
  const double nb = 12.34;
  const double na = 12.78;
  std::vector<OptionId> ids{id};
  std::vector<double> bids{nb};
  std::vector<double> asks{na};
  ASSERT_TRUE(chain
                  .update_quotes(std::span<const OptionId>(ids), std::span<const double>(bids),
                                 std::span<const double>(asks))
                  .has_value());
  const auto ref = chain.at(id);
  ASSERT_TRUE(ref.has_value());
  EXPECT_DOUBLE_EQ(ref->bid, nb);
  EXPECT_DOUBLE_EQ(ref->ask, na);
  EXPECT_DOUBLE_EQ(ref->mid, 0.5 * (nb + na));
}

TEST_F(PricerFitterTest, QuoteRevisionsAdvanceOnlyForValidTouchedExpiries) {
  OptionChain &chain = *chain_;
  ASSERT_NE(chain.instance_id(), 0u);
  ASSERT_FALSE(chain.expiry_quote_revisions().empty());
  EXPECT_EQ(chain.quote_revision(), 0u);

  const OptionId first = chain.ids().front();
  const auto quote = chain.at(first);
  ASSERT_TRUE(quote.has_value());
  ASSERT_TRUE(chain
                  .update_quotes(std::span<const OptionId>{&first, 1u},
                                 std::span<const double>{&quote->bid, 1u},
                                 std::span<const double>{&quote->ask, 1u})
                  .has_value());
  EXPECT_EQ(chain.quote_revision(), 1u);
  const atx::vol::ExpiryId touched = atx::vol::cid_expiry(first);
  for (std::size_t index = 0u; index < chain.expiry_quote_revisions().size(); ++index) {
    EXPECT_EQ(chain.expiry_quote_revisions()[index], index == touched ? 1u : 0u);
  }

  const OptionId invalid{0u};
  const double bid = 1.0;
  const double ask = 1.1;
  ASSERT_TRUE(chain
                  .update_quotes(std::span<const OptionId>{&invalid, 1u},
                                 std::span<const double>{&bid, 1u},
                                 std::span<const double>{&ask, 1u})
                  .has_value());
  EXPECT_EQ(chain.quote_revision(), 1u);
}

TEST_F(PricerFitterTest, QuoteUpdateChangesModelIvOnlyAfterAdmittedExpiryRefit) {
  SynthPanelSpec changed_spec = make_spy_synthetic_spec();
  changed_spec.expiries.back().truth.sigma0 *= 1.01;
  auto changed_chain = make_chain_from_spec(changed_spec);
  ASSERT_TRUE(changed_chain.has_value()) << changed_chain.error().to_string();

  PricerFitter fitter{essvi_config()};
  ASSERT_TRUE(fitter.fit(*chain_).has_value());
  const atx::vol::ExpiryId target =
      static_cast<atx::vol::ExpiryId>(chain_->underlying().chains.size() - 1u);
  const double maturity = chain_->underlying().chains[target].T;
  const double before = fitter.surface()->iv(spot_, maturity);
  replace_expiry_quotes(*chain_, *changed_chain, target);
  EXPECT_DOUBLE_EQ(fitter.surface()->iv(spot_, maturity), before);

  const auto refitted = fitter.refit_expiry(*chain_, target);
  ASSERT_TRUE(refitted.has_value()) << refitted.error().to_string();
  EXPECT_TRUE(refitted->warm_started);
  EXPECT_TRUE(refitted->admission.admitted);
  EXPECT_NE(fitter.surface()->iv(spot_, maturity), before);
  ASSERT_TRUE(fitter.published_provenance().has_value());
  EXPECT_EQ(fitter.published_provenance()->board_revision, chain_->quote_revision());
}

// B-I1/B-M1: after an admitted eSSVI expiry refit, the surface's parity_state
// must be RECOMPUTED by refresh_refit_diagnostics, not carried stale from the
// cold build across clone_for_refit. A healthy refit re-scores its target slice
// while every other slice stays scored, so the recomputed state is Valid and
// stays consistent with the reports/aggregates it was derived from; admission
// (which requires Valid for the default Quote consumer) then succeeds. The
// Failed/Disabled branches are defensive: a successful apply_prepared_essvi_refit
// always re-scores its slice (chain_parity errors on an unscored slice), so
// scored == n_slices for any healthy eSSVI refit.
TEST_F(PricerFitterTest, RefitRecomputesParityStateConsistentWithReports) {
  SynthPanelSpec changed_spec = make_spy_synthetic_spec();
  changed_spec.expiries.back().truth.sigma0 *= 1.01;
  auto changed_chain = make_chain_from_spec(changed_spec);
  ASSERT_TRUE(changed_chain.has_value()) << changed_chain.error().to_string();

  PricerFitter fitter{essvi_config()};
  ASSERT_TRUE(fitter.fit(*chain_).has_value());
  ASSERT_EQ(fitter.surface()->diagnostics().parity_state, atx::vol::ParityDiagnosticState::Valid);

  const atx::vol::ExpiryId target =
      static_cast<atx::vol::ExpiryId>(chain_->underlying().chains.size() - 1u);
  replace_expiry_quotes(*chain_, *changed_chain, target);
  const auto refitted = fitter.refit_expiry(*chain_, target);
  ASSERT_TRUE(refitted.has_value()) << refitted.error().to_string();
  ASSERT_TRUE(refitted->admission.admitted);

  // Recomputed to Valid, consistent with the reports it was derived from: every
  // slice scored (n > 0) and worst_frac is the min over exactly those reports.
  EXPECT_EQ(fitter.surface()->diagnostics().parity_state, atx::vol::ParityDiagnosticState::Valid);
  double worst = std::numeric_limits<double>::infinity();
  std::size_t scored = 0;
  for (const atx::vol::ParityReport &p : fitter.surface()->session().parity()) {
    EXPECT_GT(p.n, 0u);
    if (p.n > 0u) {
      worst = std::min(worst, p.frac_fv_within_bidask);
      ++scored;
    }
  }
  EXPECT_EQ(scored, fitter.surface()->diagnostics().n_slices);
  EXPECT_DOUBLE_EQ(fitter.surface()->diagnostics().worst_frac_within_bidask, worst);
}

TEST_F(PricerFitterTest, IncrementalEssviAgreesWithColdFitOnUpdatedBoard) {
  SynthPanelSpec changed_spec = make_spy_synthetic_spec();
  changed_spec.expiries.back().truth.sigma0 *= 1.01;
  auto changed_chain = make_chain_from_spec(changed_spec);
  ASSERT_TRUE(changed_chain.has_value()) << changed_chain.error().to_string();

  PricerFitter incremental{essvi_config()};
  ASSERT_TRUE(incremental.fit(*chain_).has_value());
  // Fast/Robust presets now reuse certified de-Americanization proposals by
  // default; the cold-fit comparison below is the economic correctness guard.
  EXPECT_TRUE(incremental.surface()->session().inputs().use_deam_cache_for_fit);
  EXPECT_NE(incremental.surface()->session().correction_caches().call, nullptr);
  EXPECT_NE(incremental.surface()->session().correction_caches().put, nullptr);
  const atx::vol::ExpiryId target =
      static_cast<atx::vol::ExpiryId>(chain_->underlying().chains.size() - 1u);
  replace_expiry_quotes(*chain_, *changed_chain, target);
  ASSERT_TRUE(incremental.refit_expiry(*chain_, target).has_value());

  PricerFitter cold{essvi_config()};
  ASSERT_TRUE(cold.fit(*changed_chain).has_value());
  const double maturity = changed_spec.expiries.back().T;
  for (const double strike : changed_spec.strikes) {
    EXPECT_NEAR(incremental.surface()->iv(strike, maturity), cold.surface()->iv(strike, maturity),
                2.0e-3);
  }
}

TEST_F(PricerFitterTest, RefitExplicitlyRejectsConfiguredCalendarRepairModes) {
  const auto verify_repair = [&](atx::vol::CalendarRepair repair) {
    SCOPED_TRACE(static_cast<int>(repair));
    PricerConfig config = essvi_config();
    config.admission.consumer = atx::vol::SurfaceConsumer::Mark;
    config.admission.require_calendar_arb_free = false;
    PricerFitter fitter{config};
    ASSERT_TRUE(
        fitter
            .fit(*chain_,
                 [repair](atx::vol::SessionInputs &inputs) { inputs.calendar_repair = repair; })
            .has_value());
    const FittedSurface *const published_surface = fitter.surface();
    ASSERT_TRUE(fitter.published_report().has_value());
    const std::size_t published_attempts = fitter.published_report()->attempts.size();
    ASSERT_TRUE(fitter.published_provenance().has_value());
    const atx::vol::FitSnapshotProvenance published_provenance = *fitter.published_provenance();

    const auto rejected = fitter.refit_expiry(*chain_, 0u);
    ASSERT_FALSE(rejected.has_value());
    EXPECT_EQ(rejected.error().code(), atx::core::ErrorCode::NotImplemented);
    EXPECT_EQ(fitter.surface(), published_surface);
    ASSERT_TRUE(fitter.published_report().has_value());
    EXPECT_TRUE(fitter.published_report()->published);
    EXPECT_EQ(fitter.published_report()->attempts.size(), published_attempts);
    ASSERT_TRUE(fitter.published_provenance().has_value());
    EXPECT_EQ(fitter.published_provenance()->chain_instance_id,
              published_provenance.chain_instance_id);
    EXPECT_EQ(fitter.published_provenance()->board_revision, published_provenance.board_revision);
    EXPECT_EQ(fitter.published_provenance()->expiry_revisions,
              published_provenance.expiry_revisions);
    ASSERT_TRUE(fitter.last_attempt_report().has_value());
    EXPECT_FALSE(fitter.last_attempt_report()->published);
    EXPECT_EQ(fitter.last_attempt_report()->attempts.front().stage,
              atx::vol::SurfaceBuildStage::InputValidation);
  };

  verify_repair(atx::vol::CalendarRepair::MonotoneFit);
  verify_repair(atx::vol::CalendarRepair::Project);
}

TEST_F(PricerFitterTest, CrossingMiddleExpiryRefitIsRejectedAndPreservesPublication) {
  // A calendar-crossing refit is rejected by the risk shape invariants, so this
  // test requests the strict risk contract explicitly (the default now serves
  // marks, which do not gate on calendar structure).
  PricerConfig config = essvi_config();
  config.admission = atx::vol::risk_admission_policy();
  PricerFitter fitter{config};
  ASSERT_TRUE(fitter.fit(*chain_).has_value());
  const FittedSurface *const published = fitter.surface();
  const atx::vol::SessionDiagnostics diagnostics = published->diagnostics();
  ASSERT_TRUE(fitter.published_report().has_value());
  const std::size_t published_attempts = fitter.published_report()->attempts.size();

  SynthPanelSpec stressed_spec = make_spy_synthetic_spec();
  constexpr std::size_t kMiddle = 2u;
  stressed_spec.expiries[kMiddle].truth.sigma0 = 0.28;
  auto stressed_chain = make_chain_from_spec(stressed_spec);
  ASSERT_TRUE(stressed_chain.has_value()) << stressed_chain.error().to_string();
  replace_expiry_quotes(*chain_, *stressed_chain, static_cast<atx::vol::ExpiryId>(kMiddle));

  const auto rejected = fitter.refit_expiry(*chain_, static_cast<atx::vol::ExpiryId>(kMiddle));
  ASSERT_FALSE(rejected.has_value());
  EXPECT_EQ(fitter.surface(), published);
  EXPECT_DOUBLE_EQ(fitter.surface()->diagnostics().worst_frac_within_bidask,
                   diagnostics.worst_frac_within_bidask);
  EXPECT_DOUBLE_EQ(fitter.surface()->diagnostics().mean_rmse_vol, diagnostics.mean_rmse_vol);
  EXPECT_EQ(fitter.surface()->diagnostics().n_quotes, diagnostics.n_quotes);
  ASSERT_TRUE(fitter.published_report().has_value());
  EXPECT_EQ(fitter.published_report()->attempts.size(), published_attempts);
  ASSERT_TRUE(fitter.last_attempt_report().has_value());
  EXPECT_TRUE(fitter.last_attempt_report()->refit_expiry.has_value());
  ASSERT_FALSE(fitter.last_attempt_report()->attempts.empty());
  const auto &admission = fitter.last_attempt_report()->attempts.back().admission;
  EXPECT_TRUE(atx::vol::has_admission_failure(
                  admission, atx::vol::SurfaceAdmissionReason::CalendarTotalVariance) ||
              atx::vol::has_admission_failure(admission,
                                              atx::vol::SurfaceAdmissionReason::ForwardVariance) ||
              atx::vol::has_admission_failure(admission,
                                              atx::vol::SurfaceAdmissionReason::CalendarArbitrage));
}

TEST_F(PricerFitterTest, RefitRejectsDirtyNonTargetExpiryTransactionally) {
  PricerFitter fitter{essvi_config()};
  ASSERT_TRUE(fitter.fit(*chain_).has_value());
  const FittedSurface *const published = fitter.surface();

  SynthPanelSpec changed_spec = make_spy_synthetic_spec();
  changed_spec.expiries[0].truth.sigma0 *= 1.01;
  changed_spec.expiries[1].truth.sigma0 *= 1.01;
  auto changed_chain = make_chain_from_spec(changed_spec);
  ASSERT_TRUE(changed_chain.has_value()) << changed_chain.error().to_string();
  replace_expiry_quotes(*chain_, *changed_chain, 0u);
  replace_expiry_quotes(*chain_, *changed_chain, 1u);

  const auto rejected = fitter.refit_expiry(*chain_, 0u);
  ASSERT_FALSE(rejected.has_value());
  EXPECT_EQ(rejected.error().code(), atx::core::ErrorCode::InvalidArgument);
  EXPECT_EQ(fitter.surface(), published);
  EXPECT_TRUE(fitter.published_report()->published);
  EXPECT_FALSE(fitter.last_attempt_report()->published);
}

TEST_F(PricerFitterTest, RefitRejectsDifferentChainInstanceTransactionally) {
  PricerFitter fitter{essvi_config()};
  ASSERT_TRUE(fitter.fit(*chain_).has_value());
  const FittedSurface *const published = fitter.surface();
  auto other = make_chain_from_spec(make_spy_synthetic_spec());
  ASSERT_TRUE(other.has_value()) << other.error().to_string();
  ASSERT_NE(other->instance_id(), chain_->instance_id());

  const auto rejected = fitter.refit_expiry(*other, 0u);
  ASSERT_FALSE(rejected.has_value());
  EXPECT_EQ(rejected.error().code(), atx::core::ErrorCode::InvalidArgument);
  EXPECT_EQ(fitter.surface(), published);
}

TEST_F(PricerFitterTest, RefitConsumesResolvedOverlayInputs) {
  SynthPanelSpec changed_spec = make_spy_synthetic_spec();
  changed_spec.expiries.back().truth.sigma0 *= 1.005;
  auto changed_chain = make_chain_from_spec(changed_spec);
  ASSERT_TRUE(changed_chain.has_value()) << changed_chain.error().to_string();

  PricerFitter fitter{essvi_config()};
  ASSERT_TRUE(fitter
                  .fit(*chain_,
                       [](atx::vol::SessionInputs &inputs) {
                         inputs.band_k = 1.75;
                         inputs.deam.iv_tol = 2.0e-6;
                         inputs.use_correction_cache = false;
                       })
                  .has_value());
  const atx::vol::ExpiryId target =
      static_cast<atx::vol::ExpiryId>(chain_->underlying().chains.size() - 1u);
  replace_expiry_quotes(*chain_, *changed_chain, target);
  ASSERT_TRUE(fitter.refit_expiry(*chain_, target).has_value());
  EXPECT_DOUBLE_EQ(fitter.surface()->session().inputs().band_k, 1.75);
  EXPECT_DOUBLE_EQ(fitter.surface()->session().inputs().deam.iv_tol, 2.0e-6);
  EXPECT_FALSE(fitter.surface()->session().inputs().use_correction_cache);
}

TEST_F(PricerFitterTest, UnsupportedFamilyRefitIsExplicitAndTransactional) {
  PricerConfig config;
  config.preset = FitPreset::Hft;
  config.admission.consumer = atx::vol::SurfaceConsumer::Mark;
  config.admission.require_calendar_arb_free = false;
  PricerFitter fitter{config};
  ASSERT_TRUE(fitter.fit(*chain_).has_value());
  const FittedSurface *const published = fitter.surface();
  const auto rejected = fitter.refit_expiry(*chain_, 0u);
  ASSERT_FALSE(rejected.has_value());
  EXPECT_EQ(rejected.error().code(), atx::core::ErrorCode::NotImplemented);
  EXPECT_EQ(fitter.surface(), published);
  ASSERT_TRUE(fitter.last_attempt_report().has_value());
  EXPECT_EQ(fitter.last_attempt_report()->attempts.front().stage,
            atx::vol::SurfaceBuildStage::InputValidation);
}

TEST_F(PricerFitterTest, LocalRiskRefitPublishesCopyOnWriteGeneration) {
  PricerConfig config;
  config.quality_mode = atx::vol::FitQualityMode::Latency;
  config.outputs = atx::vol::SurfaceOutputs::Risk;
  config.fallback = atx::vol::SurfaceFallback::LastKnownGood;
  atx::vol::CurveConfig curve;
  curve.kind = atx::vol::VolCurveKind::Essvi;
  config.curve = curve;
  PricerFitter fitter{config};
  ASSERT_TRUE(fitter.fit(*chain_).has_value());
  const atx::vol::SurfaceBundle before = fitter.bundle();
  ASSERT_NE(before.risk, nullptr);
  ASSERT_FALSE(before.risk->session().expiries().empty());
  const double target_T = before.risk->session().expiries().front().T;
  const double probe_K = before.risk->session().expiries().front().forward;
  const double old_iv = before.risk->session().iv(probe_K, target_T);

  std::vector<OptionId> ids;
  std::vector<double> bids;
  std::vector<double> asks;
  for (const OptionId id : chain_->ids()) {
    const auto option = chain_->at(id);
    ASSERT_TRUE(option.has_value());
    if (std::fabs(option->T - target_T) > 1.0e-12)
      continue;
    const std::size_t quote_idx =
        atx::vol::chain_index(atx::vol::cid_strike_idx(id), atx::vol::cid_side(id));
    if (chain_->underlying().chains.front().flags[quote_idx] != 0u)
      continue;
    const double half_spread = 0.45 * (option->ask - option->bid);
    ids.push_back(id);
    bids.push_back(std::max(0.0, option->mid - half_spread));
    asks.push_back(option->mid + half_spread);
  }
  ASSERT_FALSE(ids.empty());
  ASSERT_TRUE(chain_->update_quotes(ids, bids, asks).has_value());
  // Task 2e (carry I1): this update rescales every spread — including the
  // near-ATM carry pairs' — and carry weights are functions of the spread, so
  // the certified observation cache must REFUSE reuse and route the refit
  // through the full recompute path (which re-resolves carry and re-audits).
  const auto cached =
      before.risk->session().cached_refit_observations(chain_->underlying().chains.front(), 0u);
  ASSERT_FALSE(cached.has_value())
      << "carry-pair spread change must invalidate the certified cache";
  ASSERT_TRUE(fitter.refit_risk_slice(*chain_, 0u).has_value());

  const atx::vol::SurfaceBundle after = fitter.bundle();
  ASSERT_NE(after.risk, nullptr);
  EXPECT_NE(after.risk.get(), before.risk.get());
  EXPECT_EQ(after.risk->generation(), before.risk->generation() + 1u);
  EXPECT_EQ(after.risk_health.state, atx::vol::SurfaceState::Healthy);
  EXPECT_TRUE(after.risk_health.validation.admitted());
  EXPECT_DOUBLE_EQ(after.risk->session().iv(probe_K, target_T), old_iv);
  // The retained lease remains valid and unchanged after publication.
  EXPECT_DOUBLE_EQ(before.risk->session().iv(probe_K, target_T), old_iv);
}

TEST_F(PricerFitterTest, FitStoresSurfaceAndGatesValueChain) {
  PricerFitter fitter{PricerConfig{.preset = FitPreset::Fast, .n_threads = 1}};
  // value_chain before a fit is gated.
  EXPECT_FALSE(fitter.value_chain(*chain_, OutputField::ModelIV).has_value());
  EXPECT_FALSE(fitter.fitted());

  ASSERT_TRUE(fitter.fit(*chain_).has_value());
  EXPECT_TRUE(fitter.fitted());
  ASSERT_NE(fitter.surface(), nullptr);
  EXPECT_GT(fitter.surface()->diagnostics().n_slices, 0u);
}

TEST_F(PricerFitterTest, ValueChainRejectsDifferentChainInstanceWithSameLocalUid) {
  PricerFitter fitter{PricerConfig{.preset = FitPreset::Fast, .n_threads = 1}};
  ASSERT_TRUE(fitter.fit(*chain_).has_value());
  ASSERT_NE(fitter.surface(), nullptr);
  auto other = make_chain_from_spec(make_spy_synthetic_spec());
  ASSERT_TRUE(other.has_value()) << other.error().to_string();
  ASSERT_EQ(other->uid(), chain_->uid()) << "fresh single-underlying universes reuse local uid 1";
  ASSERT_NE(other->instance_id(), chain_->instance_id());

  const auto default_purpose = fitter.value_chain(*other, OutputField::ModelIV, 1u);
  ASSERT_FALSE(default_purpose.has_value());
  EXPECT_EQ(default_purpose.error().code(), atx::core::ErrorCode::InvalidArgument);

  const auto explicit_purpose =
      fitter.value_chain(*other, OutputField::ModelIV, fitter.surface()->purpose(), 1u);
  ASSERT_FALSE(explicit_purpose.has_value());
  EXPECT_EQ(explicit_purpose.error().code(), atx::core::ErrorCode::InvalidArgument);
}

TEST_F(PricerFitterTest, ValueChainSelectedRejectsDifferentChainAfterValidatingIds) {
  PricerFitter fitter{PricerConfig{.preset = FitPreset::Fast, .n_threads = 1}};
  ASSERT_TRUE(fitter.fit(*chain_).has_value());
  ASSERT_NE(fitter.surface(), nullptr);
  auto other = make_chain_from_spec(make_spy_synthetic_spec());
  ASSERT_TRUE(other.has_value()) << other.error().to_string();
  ASSERT_EQ(other->uid(), chain_->uid()) << "fresh single-underlying universes reuse local uid 1";
  ASSERT_NE(other->instance_id(), chain_->instance_id());
  const std::vector<OptionId> other_ids = other->ids();
  ASSERT_GE(other_ids.size(), 2u);
  const std::span<const OptionId> selected{other_ids.data(), 2u};

  const auto default_purpose = fitter.value_chain(*other, selected, OutputField::MidIV, 1u);
  ASSERT_FALSE(default_purpose.has_value());
  EXPECT_EQ(default_purpose.error().code(), atx::core::ErrorCode::InvalidArgument);

  const auto explicit_purpose =
      fitter.value_chain(*other, selected, OutputField::MidIV, fitter.surface()->purpose(), 1u);
  ASSERT_FALSE(explicit_purpose.has_value());
  EXPECT_EQ(explicit_purpose.error().code(), atx::core::ErrorCode::InvalidArgument);

  const OptionId invalid{0u};
  const auto invalid_selected =
      fitter.value_chain(*other, std::span<const OptionId>{&invalid, 1u}, OutputField::MidIV, 1u);
  ASSERT_FALSE(invalid_selected.has_value());
  EXPECT_EQ(invalid_selected.error().code(), atx::core::ErrorCode::NotFound)
      << "selected-id validation must remain earlier than fitted-chain provenance validation";
}

TEST_F(PricerFitterTest, SessionBoundaryRejectsUnsupportedPersistedCalibrationPolicies) {
  std::vector<atx::vol::CalibOpts> unsupported;
  atx::vol::CalibOpts interval;
  interval.loss_kind = atx::vol::CalibLossKind::Interval;
  unsupported.push_back(interval);
  atx::vol::CalibOpts shared_rho;
  shared_rho.essvi_rho_mode = atx::vol::EssviRhoMode::Shared;
  unsupported.push_back(shared_rho);
  atx::vol::CalibOpts asymmetric;
  asymmetric.essvi_asymmetric_rho = true;
  unsupported.push_back(asymmetric);
  atx::vol::CalibOpts fallback;
  fallback.essvi_fallback_rmse_threshold = 0.02;
  unsupported.push_back(fallback);
  atx::vol::CalibOpts grid;
  grid.n_butterfly_grid = 128u;
  unsupported.push_back(grid);
  atx::vol::CalibOpts residual;
  residual.residual_basis_kind = atx::vol::ResidualBasisKind::Fengler;
  unsupported.push_back(residual);

  for (const atx::vol::CalibOpts &opts : unsupported) {
    atx::vol::SessionInputs inputs =
        atx::vol::make_session_inputs(FitPreset::Fast, spot_, r_, chain_->now_ns());
    inputs.calib = opts;
    const auto result = VolaSession::build(chain_->underlying(), inputs);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code(), atx::core::ErrorCode::NotImplemented);
  }

  atx::vol::SessionInputs curve_inputs =
      atx::vol::make_session_inputs(FitPreset::Fast, spot_, r_, chain_->now_ns());
  curve_inputs.curve.kind = atx::vol::VolCurveKind::Svi;
  curve_inputs.curve.parametric.loss_kind = atx::vol::CalibLossKind::Interval;
  const auto curve_result = VolaSession::build(chain_->underlying(), curve_inputs);
  ASSERT_FALSE(curve_result.has_value());
  EXPECT_EQ(curve_result.error().code(), atx::core::ErrorCode::NotImplemented);
}

TEST_F(PricerFitterTest, GenericParityOptOutCannotPublishRiskButCanPublishMark) {
  PricerConfig risk_config;
  risk_config.preset = FitPreset::Hft;
  risk_config.admission = atx::vol::risk_admission_policy();
  risk_config.score_parity = false;
  PricerFitter risk_fitter{risk_config};
  const auto risk_status = risk_fitter.fit(*chain_);
  ASSERT_FALSE(risk_status.has_value());
  ASSERT_TRUE(risk_fitter.last_attempt_report().has_value());
  ASSERT_FALSE(risk_fitter.last_attempt_report()->attempts.empty());
  const auto &risk_attempt = risk_fitter.last_attempt_report()->attempts.front();
  EXPECT_EQ(risk_attempt.evidence.parity_state, atx::vol::ParityDiagnosticState::Disabled);
  EXPECT_TRUE(atx::vol::has_admission_failure(
      risk_attempt.admission, atx::vol::SurfaceAdmissionReason::DiagnosticsUnavailable));

  PricerConfig mark_config = risk_config;
  mark_config.admission.consumer = atx::vol::SurfaceConsumer::Mark;
  mark_config.admission.require_calendar_arb_free = false;
  PricerFitter mark_fitter{mark_config};
  ASSERT_TRUE(mark_fitter.fit(*chain_).has_value());
  ASSERT_NE(mark_fitter.surface(), nullptr);
  EXPECT_EQ(mark_fitter.surface()->diagnostics().parity_state,
            atx::vol::ParityDiagnosticState::Disabled);
}

TEST_F(PricerFitterTest, MarkBidAskFloorRetainsParityScoringByDefault) {
  PricerConfig config;
  config.preset = FitPreset::Fast;
  config.curve = atx::vol::CurveConfig{atx::vol::VolCurveKind::ConvexDense};
  config.use_correction_cache = false;
  config.use_deam_cache_for_fit = false;
  config.admission.min_worst_frac_within_bidask = 0.95;

  PricerFitter fitter{config};
  (void)fitter.fit(*chain_);
  ASSERT_TRUE(fitter.last_attempt_report().has_value());
  ASSERT_FALSE(fitter.last_attempt_report()->attempts.empty());
  const atx::vol::SurfaceBuildAttemptReport &attempt =
      fitter.last_attempt_report()->attempts.front();
  ASSERT_TRUE(attempt.build_succeeded);
  EXPECT_EQ(attempt.evidence.parity_state, atx::vol::ParityDiagnosticState::Valid);
  EXPECT_TRUE(attempt.evidence.finite_diagnostics);
  EXPECT_EQ(atx::vol::has_admission_failure(attempt.admission,
                                            atx::vol::SurfaceAdmissionReason::QualityBelowFloor),
            attempt.evidence.worst_frac_within_bidask <
                config.admission.min_worst_frac_within_bidask);
}

TEST_F(PricerFitterTest, V2MarkBidAskFloorRetainsParityScoringByDefault) {
  PricerConfig config;
  config.preset = FitPreset::Hft;
  config.outputs = atx::vol::SurfaceOutputs::MarketMark;
  config.use_correction_cache = false;
  config.use_deam_cache_for_fit = false;
  config.admission.min_worst_frac_within_bidask = 0.95;

  PricerFitter fitter{config};
  (void)fitter.fit(*chain_);
  ASSERT_TRUE(fitter.last_attempt_report().has_value());
  ASSERT_FALSE(fitter.last_attempt_report()->attempts.empty());
  const atx::vol::SurfaceBuildAttemptReport &attempt =
      fitter.last_attempt_report()->attempts.front();
  ASSERT_TRUE(attempt.build_succeeded);
  EXPECT_EQ(attempt.evidence.parity_state, atx::vol::ParityDiagnosticState::Valid);
  EXPECT_TRUE(attempt.evidence.finite_diagnostics);
  EXPECT_EQ(atx::vol::has_admission_failure(attempt.admission,
                                            atx::vol::SurfaceAdmissionReason::QualityBelowFloor),
            attempt.evidence.worst_frac_within_bidask <
                config.admission.min_worst_frac_within_bidask);
}

TEST_F(PricerFitterTest, PinnedEssviStillScoresParityWhenGenericScoreFlagIsFalse) {
  PricerConfig config = essvi_config();
  config.score_parity = false;
  PricerFitter fitter{config};
  ASSERT_TRUE(fitter.fit(*chain_).has_value());
  EXPECT_EQ(fitter.surface()->diagnostics().parity_state, atx::vol::ParityDiagnosticState::Valid);
  const std::span<const atx::vol::ParityReport> parity = fitter.surface()->session().parity();
  ASSERT_FALSE(parity.empty());
  for (const atx::vol::ParityReport &report : parity) {
    EXPECT_GT(report.n, 0u);
  }
}

TEST_F(PricerFitterTest, PinnedConvexMarkDropsUnservedDiagnosticsWithoutMovingArchive) {
  PricerConfig default_config;
  default_config.preset = FitPreset::Fast;
  default_config.curve = atx::vol::CurveConfig{atx::vol::VolCurveKind::ConvexDense};
  default_config.use_deam_cache_for_fit = false;

  PricerConfig scored_config = default_config;
  scored_config.score_parity = true;

  PricerFitter default_fitter{default_config};
  PricerFitter scored_fitter{scored_config};
  ASSERT_TRUE(default_fitter.fit(*chain_).has_value());
  ASSERT_TRUE(scored_fitter.fit(*chain_).has_value());
  ASSERT_NE(default_fitter.surface(), nullptr);
  ASSERT_NE(scored_fitter.surface(), nullptr);

  const VolaSession &default_session = default_fitter.surface()->session();
  const VolaSession &scored_session = scored_fitter.surface()->session();
  EXPECT_TRUE(default_session.inputs().curve_pinned);
  EXPECT_FALSE(default_session.inputs().score_parity);
  EXPECT_EQ(default_session.diagnostics().parity_state, atx::vol::ParityDiagnosticState::Disabled);
  EXPECT_TRUE(scored_session.inputs().score_parity);
  EXPECT_EQ(scored_session.diagnostics().parity_state, atx::vol::ParityDiagnosticState::Valid);

  // LegacyCompatible serves a polymorphic override cold, and ConvexDense also
  // rejects cache proposals during fit. The requested cache therefore has no
  // consumer on either path and must never be constructed.
  EXPECT_TRUE(default_session.inputs().use_correction_cache);
  EXPECT_TRUE(scored_session.inputs().use_correction_cache);
  EXPECT_FALSE(default_session.correction_caches().any());
  EXPECT_FALSE(scored_session.correction_caches().any());

  auto default_priced = default_session.to_priced_surface();
  auto scored_priced = scored_session.to_priced_surface();
  ASSERT_TRUE(default_priced.has_value()) << default_priced.error().to_string();
  ASSERT_TRUE(scored_priced.has_value()) << scored_priced.error().to_string();
  const std::array<atx::vol::SurfaceArchiveItem, 1> default_items{
      atx::vol::SurfaceArchiveItem{"SPY", &*default_priced}};
  const std::array<atx::vol::SurfaceArchiveItem, 1> scored_items{
      atx::vol::SurfaceArchiveItem{"SPY", &*scored_priced}};
  atx::vol::SurfaceArchiveWriteOpts archive_opts;
  archive_opts.created_ts_ns = 42;
  const auto default_archive = atx::vol::write_surface_archive(default_items, archive_opts);
  const auto scored_archive = atx::vol::write_surface_archive(scored_items, archive_opts);
  ASSERT_TRUE(default_archive.has_value()) << default_archive.error().to_string();
  ASSERT_TRUE(scored_archive.has_value()) << scored_archive.error().to_string();
  EXPECT_EQ(*default_archive, *scored_archive);
}

TEST_F(PricerFitterTest, PinnedCurveRiskAdmissionRetainsParityByDefault) {
  PricerConfig config;
  config.preset = FitPreset::Fast;
  config.curve = atx::vol::CurveConfig{atx::vol::VolCurveKind::ConvexDense};
  config.use_correction_cache = false;
  config.use_deam_cache_for_fit = false;
  config.admission = atx::vol::risk_admission_policy();

  PricerFitter fitter{config};
  (void)fitter.fit(*chain_);
  ASSERT_TRUE(fitter.last_attempt_report().has_value());
  ASSERT_FALSE(fitter.last_attempt_report()->attempts.empty());
  const atx::vol::SurfaceBuildAttemptReport &attempt =
      fitter.last_attempt_report()->attempts.front();
  ASSERT_TRUE(attempt.build_succeeded);
  EXPECT_EQ(attempt.evidence.parity_state, atx::vol::ParityDiagnosticState::Valid);
}

TEST_F(PricerFitterTest, ValueChainFieldsMatchSessionScalarQueries) {
  PricerFitter fitter{PricerConfig{.preset = FitPreset::Fast, .n_threads = 1}};
  ASSERT_TRUE(fitter.fit(*chain_).has_value());

  const auto valr = fitter.value_chain(*chain_, OutputField::All, 1);
  ASSERT_TRUE(valr.has_value());
  const ChainValuation &v = *valr;
  const OptionChain &chain = *chain_;
  const VolaSession &sess = fitter.surface()->session();
  ASSERT_EQ(v.size(), chain.ids().size());

  int checked = 0;
  for (std::size_t i = 0; i < v.size(); ++i) {
    const auto ref = chain.at(v.ids[i]);
    ASSERT_TRUE(ref.has_value());
    // model IV / fair value are EXACTLY the session scalar queries (the facade
    // adds ownership + parallelism, never a different number).
    EXPECT_TRUE(same(v.model_iv[i], sess.iv(ref->strike, ref->T)));
    const auto fv = sess.fair_value(ref->strike, ref->T, ref->side);
    EXPECT_TRUE(same(v.model_price[i], fv.has_value() ? *fv : std::nan("")));
    ++checked;
  }
  EXPECT_GT(checked, 0);
}

TEST_F(PricerFitterTest, ValueChainThreadCountDeterminism) {
  PricerFitter fitter{PricerConfig{.preset = FitPreset::Fast}};
  ASSERT_TRUE(fitter.fit(*chain_).has_value());

  const auto v1r = fitter.value_chain(*chain_, OutputField::All, 1);
  const auto v4r = fitter.value_chain(*chain_, OutputField::All, 4);
  ASSERT_TRUE(v1r.has_value());
  ASSERT_TRUE(v4r.has_value());
  const ChainValuation &a = *v1r;
  const ChainValuation &b = *v4r;
  ASSERT_EQ(a.size(), b.size());

  for (std::size_t i = 0; i < a.size(); ++i) {
    EXPECT_EQ(a.ids[i], b.ids[i]);
    EXPECT_TRUE(same(a.model_iv[i], b.model_iv[i]));
    EXPECT_TRUE(same(a.model_price[i], b.model_price[i]));
    EXPECT_TRUE(same(a.bid_iv[i], b.bid_iv[i]));
    EXPECT_TRUE(same(a.ask_iv[i], b.ask_iv[i]));
    EXPECT_TRUE(same(a.mid_iv[i], b.mid_iv[i]));
    EXPECT_TRUE(same(a.greeks[i].price, b.greeks[i].price));
    EXPECT_TRUE(same(a.greeks[i].delta, b.greeks[i].delta));
    EXPECT_TRUE(same(a.greeks[i].vega, b.greeks[i].vega));
  }
  EXPECT_EQ(a.n_bid_unset, b.n_bid_unset);
  EXPECT_EQ(a.n_ask_unset, b.n_ask_unset);
  EXPECT_EQ(a.n_bid_iv_fail, b.n_bid_iv_fail);
  EXPECT_EQ(a.n_ask_iv_fail, b.n_ask_iv_fail);
}

TEST(PricerFitterValueChain, SameExpiryChunksRemainBitIdenticalAcrossWorkers) {
  SynthPanelSpec spec = make_spy_synthetic_spec();
  spec.strikes.clear();
  for (double strike = 540.0; strike <= 660.0 + 1.0e-9; strike += 1.5) {
    spec.strikes.push_back(strike);
  }
  auto chain = make_chain_from_spec(spec);
  ASSERT_TRUE(chain.has_value()) << chain.error().message();
  const std::vector<OptionId> dense_ids = chain->ids();
  const std::size_t first_expiry_rows =
      static_cast<std::size_t>(std::count_if(dense_ids.begin(), dense_ids.end(), [](OptionId id) {
        return atx::vol::cid_expiry(id) == 0u;
      }));
  ASSERT_GT(first_expiry_rows, 128u); // force at least two same-T work chunks

  PricerConfig config{.preset = FitPreset::Hft, .use_correction_cache = true};
  config.query_pricing_tier = QueryPricingTier::RepresentativeFast;
  PricerFitter fitter{config};
  ASSERT_TRUE(fitter.fit(*chain).has_value());

  // Create the process pool before resetting instrumentation. PoolDispatches
  // below proves the valuation uses this executor; WorkerLaunches verifies that
  // its persistent workers were not rebuilt during the measured call.
  atx::vol::PricingExecutor &executor = atx::vol::pricing_executor();
  std::array<unsigned, 128> warm{};
  executor.run_blocks(warm.size(), 8u, [&](std::size_t i) { warm[i] = 1u; });
  atx::vol::counters::reset();

  const auto serial = fitter.value_chain(*chain, OutputField::All, 1u);
  const auto parallel = fitter.value_chain(*chain, OutputField::All, 8u);
  ASSERT_TRUE(serial.has_value()) << serial.error().message();
  ASSERT_TRUE(parallel.has_value()) << parallel.error().message();
  ASSERT_EQ(serial->size(), parallel->size());
  for (std::size_t i = 0u; i < serial->size(); ++i) {
    EXPECT_EQ(serial->ids[i], parallel->ids[i]);
    EXPECT_TRUE(same(serial->model_iv[i], parallel->model_iv[i]));
    EXPECT_TRUE(same(serial->model_price[i], parallel->model_price[i]));
    EXPECT_TRUE(same(serial->bid_iv[i], parallel->bid_iv[i]));
    EXPECT_TRUE(same(serial->ask_iv[i], parallel->ask_iv[i]));
    EXPECT_TRUE(same(serial->mid_iv[i], parallel->mid_iv[i]));
    EXPECT_TRUE(same(serial->greeks[i].price, parallel->greeks[i].price));
    EXPECT_TRUE(same(serial->greeks[i].delta, parallel->greeks[i].delta));
    EXPECT_TRUE(same(serial->greeks[i].gamma, parallel->greeks[i].gamma));
    EXPECT_TRUE(same(serial->greeks[i].vega, parallel->greeks[i].vega));
    EXPECT_TRUE(same(serial->greeks[i].theta, parallel->greeks[i].theta));
    EXPECT_TRUE(same(serial->greeks[i].rho, parallel->greeks[i].rho));
    EXPECT_TRUE(same(serial->greeks[i].vanna, parallel->greeks[i].vanna));
    EXPECT_TRUE(same(serial->greeks[i].volga, parallel->greeks[i].volga));
    EXPECT_TRUE(same(serial->greeks[i].charm, parallel->greeks[i].charm));
  }
  EXPECT_EQ(serial->n_bid_unset, parallel->n_bid_unset);
  EXPECT_EQ(serial->n_ask_unset, parallel->n_ask_unset);
  EXPECT_EQ(serial->n_bid_iv_fail, parallel->n_bid_iv_fail);
  EXPECT_EQ(serial->n_ask_iv_fail, parallel->n_ask_iv_fail);
  if constexpr (atx::vol::counters::counters_enabled()) {
    const atx::vol::counters::Snapshot counts = atx::vol::counters::snapshot();
    EXPECT_EQ(counts.get(atx::vol::counters::Counter::WorkerLaunches), 0u);
    if (executor.size() > 0u) {
      EXPECT_EQ(counts.get(atx::vol::counters::Counter::PoolDispatches), 1u)
          << "only the multi-chunk parallel valuation should wake the persistent pool";
    } else {
      EXPECT_EQ(counts.get(atx::vol::counters::Counter::PoolDispatches), 0u);
    }
  }
}

TEST(PricerFitterValueChain, ColdPriceOnlyChunksAreDeterministicAcrossWorkers) {
  SynthPanelSpec spec = make_spy_synthetic_spec();
  spec.strikes.clear();
  for (double strike = 540.0; strike <= 660.0 + 1.0e-9; strike += 1.5) {
    spec.strikes.push_back(strike);
  }
  auto chain = make_chain_from_spec(spec);
  ASSERT_TRUE(chain.has_value()) << chain.error().message();

  PricerConfig config{.preset = FitPreset::Hft};
  config.query_pricing_tier = QueryPricingTier::ColdReference;
  PricerFitter fitter{config};
  ASSERT_TRUE(fitter.fit(*chain).has_value());

  const auto serial = fitter.value_chain(*chain, OutputField::Prices, 1u);
  const auto parallel = fitter.value_chain(*chain, OutputField::Prices, 8u);
  ASSERT_TRUE(serial.has_value()) << serial.error().message();
  ASSERT_TRUE(parallel.has_value()) << parallel.error().message();
  ASSERT_EQ(serial->ids, parallel->ids);
  ASSERT_EQ(serial->model_iv.size(), parallel->model_iv.size());
  ASSERT_EQ(serial->model_price.size(), parallel->model_price.size());
  for (std::size_t i = 0u; i < serial->size(); ++i) {
    EXPECT_DOUBLE_EQ(serial->model_iv[i], parallel->model_iv[i]);
    EXPECT_DOUBLE_EQ(serial->model_price[i], parallel->model_price[i]);
  }
}

TEST_F(PricerFitterTest, ValueChainSelectedIdsMatchesFullChainRowsInCallerOrder) {
  PricerFitter fitter{PricerConfig{.preset = FitPreset::Fast}};
  ASSERT_TRUE(fitter.fit(*chain_).has_value());
  const std::vector<OptionId> all_ids = chain_->ids();
  ASSERT_GE(all_ids.size(), 6u);
  const std::vector<OptionId> selected{all_ids[5], all_ids[1], all_ids[5]};
  constexpr OutputField fields = OutputField::ModelIV | OutputField::MidIV;

  const auto full = fitter.value_chain(*chain_, fields, 1);
  const auto dirty = fitter.value_chain(*chain_, std::span<const OptionId>(selected), fields, 8);

  ASSERT_TRUE(full.has_value()) << full.error().message();
  ASSERT_TRUE(dirty.has_value()) << dirty.error().message();
  ASSERT_EQ(dirty->ids, selected);
  ASSERT_EQ(dirty->model_iv.size(), selected.size());
  ASSERT_EQ(dirty->mid_iv.size(), selected.size());
  EXPECT_TRUE(dirty->model_price.empty());
  EXPECT_TRUE(dirty->greeks.empty());
  for (std::size_t i = 0u; i < selected.size(); ++i) {
    const auto full_row = full->row_of(selected[i]);
    ASSERT_TRUE(full_row.has_value());
    EXPECT_TRUE(same(dirty->model_iv[i], full->model_iv[*full_row]));
    EXPECT_TRUE(same(dirty->mid_iv[i], full->mid_iv[*full_row]));
  }
}

TEST_F(PricerFitterTest, ValueChainSelectedIdsRejectsUnknownId) {
  PricerFitter fitter{PricerConfig{.preset = FitPreset::Fast}};
  ASSERT_TRUE(fitter.fit(*chain_).has_value());
  const OptionId invalid{0u};

  const auto valued =
      fitter.value_chain(*chain_, std::span<const OptionId>{&invalid, 1u}, OutputField::MidIV, 1);

  ASSERT_FALSE(valued.has_value());
  EXPECT_EQ(valued.error().code(), atx::core::ErrorCode::NotFound);
}

TEST_F(PricerFitterTest, ValueChainSelectedIdsAcceptsEmptySelection) {
  PricerFitter fitter{PricerConfig{.preset = FitPreset::Fast}};
  ASSERT_TRUE(fitter.fit(*chain_).has_value());

  const auto valued =
      fitter.value_chain(*chain_, std::span<const OptionId>{}, OutputField::Bands, 1);

  ASSERT_TRUE(valued.has_value()) << valued.error().message();
  EXPECT_EQ(valued->size(), 0u);
  EXPECT_TRUE(valued->bid_iv.empty());
  EXPECT_TRUE(valued->ask_iv.empty());
  EXPECT_TRUE(valued->mid_iv.empty());
  EXPECT_EQ(valued->n_bid_unset, 0u);
  EXPECT_EQ(valued->n_ask_unset, 0u);
}

TEST_F(PricerFitterTest, ChainSelectedSnapshotPreservesRequestedOrderAndDuplicates) {
  const OptionChain &chain = *chain_;
  const std::vector<OptionId> all_ids = chain.ids();
  ASSERT_GE(all_ids.size(), 4u);
  const std::vector<OptionId> selected{all_ids[3], all_ids[0], all_ids[3]};

  const auto snap = chain.snapshot(std::span<const OptionId>(selected));

  ASSERT_TRUE(snap.has_value()) << snap.error().message();
  ASSERT_EQ(snap->ids, selected);
  ASSERT_EQ(snap->size(), selected.size());
  for (std::size_t i = 0u; i < selected.size(); ++i) {
    const auto ref = chain.at(selected[i]);
    ASSERT_TRUE(ref.has_value());
    EXPECT_DOUBLE_EQ(snap->T[i], ref->T);
    EXPECT_DOUBLE_EQ(snap->strike[i], ref->strike);
    EXPECT_DOUBLE_EQ(snap->bid[i], ref->bid);
    EXPECT_DOUBLE_EQ(snap->ask[i], ref->ask);
    EXPECT_DOUBLE_EQ(snap->mid[i], ref->mid);
    EXPECT_EQ(snap->side[i], ref->side);
  }
}

TEST_F(PricerFitterTest, ChainSelectedSnapshotRejectsForeignOrUnknownId) {
  const OptionChain &chain = *chain_;
  const OptionId invalid{0u};

  const auto snap = chain.snapshot(std::span<const OptionId>{&invalid, 1u});

  ASSERT_FALSE(snap.has_value());
  EXPECT_EQ(snap.error().code(), atx::core::ErrorCode::NotFound);
}

TEST_F(PricerFitterTest, ChainSelectedSnapshotAcceptsEmptySelection) {
  const OptionChain &chain = *chain_;

  const auto snap = chain.snapshot(std::span<const OptionId>{});

  ASSERT_TRUE(snap.has_value()) << snap.error().message();
  EXPECT_EQ(snap->size(), 0u);
}

TEST_F(PricerFitterTest, ValueChainPopulatesOnlyRequestedFields) {
  PricerFitter fitter{PricerConfig{.preset = FitPreset::Fast, .n_threads = 1}};
  ASSERT_TRUE(fitter.fit(*chain_).has_value());

  const auto vr = fitter.value_chain(*chain_, OutputField::ModelIV, 1);
  ASSERT_TRUE(vr.has_value());
  const ChainValuation &v = *vr;
  EXPECT_EQ(v.model_iv.size(), v.size());
  EXPECT_TRUE(v.model_price.empty());
  EXPECT_TRUE(v.bid_iv.empty());
  EXPECT_TRUE(v.ask_iv.empty());
  EXPECT_TRUE(v.greeks.empty());
  EXPECT_TRUE(atx::vol::has(v.filled, OutputField::ModelIV));
  EXPECT_FALSE(atx::vol::has(v.filled, OutputField::ModelPrice));
}

TEST_F(PricerFitterTest, ValueChainSeparatesUnsetSidesFromAttemptedInversionFailures) {
  PricerFitter fitter{PricerConfig{.preset = FitPreset::Fast, .n_threads = 1}};
  ASSERT_TRUE(fitter.fit(*chain_).has_value());

  const auto baseline = fitter.value_chain(*chain_, OutputField::Bands, 1);
  ASSERT_TRUE(baseline.has_value()) << baseline.error().to_string();

  std::vector<OptionId> ids;
  std::vector<double> bids;
  std::vector<double> asks;
  for (const OptionId id : chain_->ids()) {
    const auto option = chain_->at(id);
    ASSERT_TRUE(option.has_value());
    const bool itm = option->side == Side::Call ? option->strike < spot_ : option->strike > spot_;
    if (itm && ids.size() < 4u) {
      ids.push_back(id);
      bids.push_back(0.0);
      asks.push_back(option->ask);
    }
  }
  ASSERT_EQ(ids.size(), 4u);
  ASSERT_TRUE(chain_->update_quotes(ids, bids, asks).has_value());

  const auto unset = fitter.value_chain(*chain_, OutputField::Bands, 1);
  ASSERT_TRUE(unset.has_value()) << unset.error().to_string();
  EXPECT_EQ(unset->n_bid_unset, baseline->n_bid_unset + 4u);
  EXPECT_EQ(unset->n_ask_unset, baseline->n_ask_unset);
  EXPECT_EQ(unset->n_bid_iv_fail, baseline->n_bid_iv_fail);
  const std::size_t bid_nans = static_cast<std::size_t>(std::count_if(
      unset->bid_iv.begin(), unset->bid_iv.end(), [](double value) { return std::isnan(value); }));
  EXPECT_EQ(bid_nans, unset->n_bid_unset + unset->n_bid_iv_fail);

  const OptionId attempted_id = ids.front();
  constexpr double kImpossiblePremium = 1.0e9;
  const std::array<OptionId, 1> attempted_ids{attempted_id};
  const std::array<double, 1> impossible_bids{kImpossiblePremium};
  const std::array<double, 1> impossible_asks{kImpossiblePremium + 1.0};
  ASSERT_TRUE(chain_->update_quotes(attempted_ids, impossible_bids, impossible_asks).has_value());
  const auto attempted = fitter.value_chain(*chain_, OutputField::Bands, 1);
  ASSERT_TRUE(attempted.has_value()) << attempted.error().to_string();
  EXPECT_EQ(attempted->n_bid_unset, baseline->n_bid_unset + 3u);
  EXPECT_EQ(attempted->n_ask_unset, baseline->n_ask_unset);
  EXPECT_EQ(attempted->n_bid_iv_fail, baseline->n_bid_iv_fail + 1u);
  EXPECT_EQ(attempted->n_ask_iv_fail, baseline->n_ask_iv_fail + 1u);
}

TEST_F(PricerFitterTest, ValueChainUnsetCountersAreThreadCountInvariant) {
  PricerFitter fitter{PricerConfig{.preset = FitPreset::Fast}};
  ASSERT_TRUE(fitter.fit(*chain_).has_value());

  const auto baseline = fitter.value_chain(*chain_, OutputField::Bands, 1);
  ASSERT_TRUE(baseline.has_value()) << baseline.error().to_string();

  std::vector<OptionId> ids;
  std::vector<double> bids;
  std::vector<double> asks;
  for (const OptionId id : chain_->ids()) {
    const auto option = chain_->at(id);
    ASSERT_TRUE(option.has_value());
    if (ids.size() < 5u && std::isfinite(option->bid) && option->bid > 0.0) {
      ids.push_back(id);
      bids.push_back(0.0);
      asks.push_back(option->ask);
    }
  }
  ASSERT_EQ(ids.size(), 5u);
  ASSERT_TRUE(chain_->update_quotes(ids, bids, asks).has_value());

  const auto serial = fitter.value_chain(*chain_, OutputField::Bands, 1);
  const auto parallel = fitter.value_chain(*chain_, OutputField::Bands, 4);
  ASSERT_TRUE(serial.has_value());
  ASSERT_TRUE(parallel.has_value());
  EXPECT_EQ(serial->n_bid_unset, baseline->n_bid_unset + 5u);
  EXPECT_EQ(serial->n_bid_unset, parallel->n_bid_unset);
  EXPECT_EQ(serial->n_ask_unset, parallel->n_ask_unset);
  EXPECT_EQ(serial->n_bid_iv_fail, parallel->n_bid_iv_fail);
  EXPECT_EQ(serial->n_ask_iv_fail, parallel->n_ask_iv_fail);
}

TEST(ValueChain, DegenerateLegsCountBothRequestedSidesAsUnset) {
  const SynthPanelSpec spec = make_spy_synthetic_spec();
  auto panel = make_synthetic_american_panel(spec);
  ASSERT_TRUE(panel.has_value()) << panel.error().message();

  atx::vol::QuoteFrame augmented = panel->frame;
  atx::vol::QuoteRow call;
  call.uid = spec.uid;
  call.expiry_iso = spec.snapshot_iso;
  call.strike = spec.spot;
  call.side = Side::Call;
  call.bid = 5.0;
  call.ask = 5.5;
  atx::vol::QuoteRow put = call;
  put.side = Side::Put;
  put.bid = 4.0;
  put.ask = 4.5;
  augmented.rows.push_back(call);
  augmented.rows.push_back(put);

  auto degenerate = OptionChain::from_frame(augmented, spec.r, spec.spot);
  ASSERT_TRUE(degenerate.has_value()) << degenerate.error().message();

  // Fit and value the same chain instance: value_chain deliberately rejects
  // cross-chain surface/quote mixing. Compare the full chain with a selected
  // view that omits only the two T=0 rows added above.
  PricerFitter fitter{PricerConfig{.preset = FitPreset::Fast, .n_threads = 1}};
  ASSERT_TRUE(fitter.fit(*degenerate).has_value());
  const atx::vol::ChainSnapshot snapshot = degenerate->snapshot();
  std::vector<OptionId> regular_ids;
  regular_ids.reserve(snapshot.size());
  for (std::size_t i = 0; i < snapshot.size(); ++i) {
    if (snapshot.T[i] > 0.0) {
      regular_ids.push_back(snapshot.ids[i]);
    }
  }
  ASSERT_EQ(regular_ids.size() + 2u, snapshot.size());
  const auto baseline = fitter.value_chain(*degenerate, regular_ids, OutputField::Bands, 1);
  ASSERT_TRUE(baseline.has_value()) << baseline.error().to_string();
  const auto valued = fitter.value_chain(*degenerate, OutputField::Bands, 1);
  ASSERT_TRUE(valued.has_value()) << valued.error().to_string();
  EXPECT_EQ(valued->n_bid_unset, baseline->n_bid_unset + 2u);
  EXPECT_EQ(valued->n_ask_unset, baseline->n_ask_unset + 2u);
  EXPECT_EQ(valued->n_bid_iv_fail, baseline->n_bid_iv_fail);
  EXPECT_EQ(valued->n_ask_iv_fail, baseline->n_ask_iv_fail);
}

TEST_F(PricerFitterTest, HftUsesDirectLinearVarianceCurve) {
  PricerFitter fitter{PricerConfig{.preset = FitPreset::Hft}};
  ASSERT_TRUE(fitter.fit(*chain_).has_value());
  auto priced = fitter.surface()->session().to_priced_surface();
  ASSERT_TRUE(priced.has_value()) << priced.error().to_string();
  ASSERT_GT(priced->n_slices(), 0u);
  EXPECT_EQ(priced->kind_at(0), atx::vol::VolCurveKind::LinearVariance);
  const VolaSession &session = fitter.surface()->session();
  EXPECT_FALSE(
      session.correction_blend_at(session.expiries().front().T, Side::Call).usable(Side::Call));
}

TEST_F(PricerFitterTest, HftQueryPricingTiersKeepColdAndRepresentativeDistinct) {
  PricerConfig cold_config{.preset = FitPreset::Hft, .use_correction_cache = true};
  cold_config.query_pricing_tier = QueryPricingTier::ColdReference;
  PricerFitter cold{cold_config};
  ASSERT_TRUE(cold.fit(*chain_).has_value());
  const VolaSession &cold_session = cold.surface()->session();
  EXPECT_TRUE(cold_session.inputs().curve_pinned);
  EXPECT_FALSE(cold_session.correction_caches().any());
  EXPECT_FALSE(cold_session.correction_blend_at(cold_session.expiries().front().T, Side::Put)
                   .usable(Side::Put));

  PricerConfig fast_config{.preset = FitPreset::Hft, .use_correction_cache = true};
  fast_config.query_pricing_tier = QueryPricingTier::RepresentativeFast;
  PricerFitter fast{fast_config};
  ASSERT_TRUE(fast.fit(*chain_).has_value());
  const VolaSession &fast_session = fast.surface()->session();
  EXPECT_TRUE(fast_session.inputs().curve_pinned);
  EXPECT_TRUE(fast_session.correction_caches().any());
  EXPECT_EQ(fast_session.query_cache_bank_size(), 0u);
  const atx::vol::CorrectionBlend representative =
      fast_session.correction_blend_at(fast_session.expiries().front().T, Side::Put);
  EXPECT_TRUE(representative.usable(Side::Put));
  EXPECT_EQ(representative.upper_weight, 0.0);
}

TEST_F(PricerFitterTest, HftCarryBankQueriesExplicitlyEnableBankedCacheServing) {
  PricerConfig config{.preset = FitPreset::Hft, .use_correction_cache = true};
  config.query_pricing_tier = QueryPricingTier::CarryBank;
  PricerFitter fitter{config};
  ASSERT_TRUE(fitter.fit(*chain_).has_value());

  const VolaSession &session = fitter.surface()->session();
  const atx::vol::AmericanCorrectionCaches caches = session.correction_caches();
  EXPECT_TRUE(session.inputs().curve_pinned);
  ASSERT_NE(caches.call, nullptr);
  ASSERT_NE(caches.put, nullptr);
  EXPECT_EQ(session.inputs().query_pricing_tier, QueryPricingTier::CarryBank);
  EXPECT_GT(session.query_cache_bank_size(), 0u);

  const std::span<const atx::vol::SliceContext> expiries = session.expiries();
  ASSERT_GE(expiries.size(), 2u);
  for (const double endpoint_T : {expiries.front().T, expiries.back().T}) {
    const atx::vol::CorrectionBlend endpoint = session.correction_blend_at(endpoint_T, Side::Call);
    ASSERT_TRUE(endpoint.usable(Side::Call));
    EXPECT_EQ(endpoint.upper_weight, 0.0);
  }

  const auto option = chain_->at(chain_->ids().front());
  ASSERT_TRUE(option.has_value());
  const auto price = session.fair_value(option->strike, option->T, option->side);
  const auto greeks = session.greeks(option->strike, option->T, option->side);
  ASSERT_TRUE(price.has_value()) << price.error().to_string();
  ASSERT_TRUE(greeks.has_value()) << greeks.error().to_string();
  EXPECT_NEAR(*price, greeks->price, 1.0e-12 * (1.0 + std::fabs(*price)));

  const double between_T = 0.5 * (expiries[0].T + expiries[1].T);
  const atx::vol::CorrectionBlend between = session.correction_blend_at(between_T, Side::Put);
  ASSERT_TRUE(between.usable(Side::Put));
  EXPECT_GE(between.upper_weight, 0.0);
  EXPECT_LE(between.upper_weight, 1.0);
  const auto between_price = session.fair_value(chain_->spot(), between_T, Side::Put);
  const auto between_greeks = session.greeks(chain_->spot(), between_T, Side::Put);
  ASSERT_TRUE(between_price.has_value()) << between_price.error().to_string();
  ASSERT_TRUE(between_greeks.has_value()) << between_greeks.error().to_string();
  EXPECT_NEAR(*between_price, between_greeks->price, 1.0e-12 * (1.0 + std::fabs(*between_price)));
}

// "A profile is a latency prior, not permission to drop an underlier." Every kind
// must therefore have somewhere to go, and a rung equal to its own primary is a
// silent no-op retry -- the shape of the bug where a failed dense LinearVariance
// board "fell back" to LinearVariance and was dropped.
TEST(PricerFitterPolicy, EveryCurveKindDeclaresAProgressingFallbackLadder) {
  using atx::vol::fallback_curve_rungs;
  using atx::vol::VolCurveKind;
  constexpr VolCurveKind kAllKinds[]{VolCurveKind::ConvexDense, VolCurveKind::Essvi,
                                     VolCurveKind::Svi, VolCurveKind::LinearVariance,
                                     VolCurveKind::C8};

  for (const VolCurveKind primary : kAllKinds) {
    const std::span<const VolCurveKind> rungs = fallback_curve_rungs(primary);
    ASSERT_FALSE(rungs.empty()) << to_string(primary) << " has no fallback";
    for (std::size_t i = 0; i < rungs.size(); ++i) {
      EXPECT_NE(rungs[i], primary) << to_string(primary) << " retries itself";
      for (std::size_t j = i + 1; j < rungs.size(); ++j) {
        EXPECT_NE(rungs[i], rungs[j]) << to_string(primary) << " repeats a rung";
      }
    }
    // Each ladder ends at a minimally identified family: two market nodes
    // (LinearVariance) or the five-parameter backbone (eSSVI).
    const VolCurveKind last = rungs.back();
    EXPECT_TRUE(last == VolCurveKind::LinearVariance || last == VolCurveKind::Essvi)
        << to_string(primary) << " bottoms out at " << to_string(last);
  }
}

TEST(PricerFitterPolicy, RiskAutoFitFallsBackWhenPrimaryFailsIndependentOracle) {
  SynthPanelSpec spec = make_spy_synthetic_spec();
  auto panel = make_synthetic_american_panel(spec);
  ASSERT_TRUE(panel.has_value()) << panel.error().message();
  auto chain = OptionChain::from_frame(panel->frame, spec.r, spec.spot);
  ASSERT_TRUE(chain.has_value()) << chain.error().message();

  // The primary LinearVariance route fails the risk strike-convexity invariant on
  // this board; that rejection is what drives the fallback ladder. Request the
  // strict risk contract explicitly (the default serves marks, which admit the
  // non-convex primary directly and would never fall back).
  PricerConfig config;
  config.admission = atx::vol::risk_admission_policy();
  PricerFitter fitter{config};
  ASSERT_TRUE(fitter.fit(*chain).has_value());
  ASSERT_TRUE(fitter.decision().has_value());
  EXPECT_TRUE(fitter.decision()->used_fallback);
  EXPECT_EQ(fitter.decision()->primary_curve.kind, atx::vol::VolCurveKind::LinearVariance);
  EXPECT_EQ(fitter.decision()->curve.kind, atx::vol::VolCurveKind::Essvi);
  ASSERT_TRUE(fitter.published_report().has_value());
  ASSERT_GT(fitter.published_report()->attempts.size(), 1u);
  const auto &primary = fitter.published_report()->attempts.front();
  EXPECT_FALSE(primary.admission.admitted);
  EXPECT_TRUE(atx::vol::has_admission_failure(primary.admission,
                                              atx::vol::SurfaceAdmissionReason::StrikeConvexity));
  EXPECT_EQ(primary.evidence.first_invariant_failure,
            atx::vol::SurfaceAdmissionReason::StrikeConvexity);
  EXPECT_TRUE(primary.evidence.first_failure_maturity.has_value());
  EXPECT_TRUE(primary.evidence.first_failure_log_moneyness.has_value());
  EXPECT_TRUE(fitter.published_report()->attempts.back().admission.admitted);
}

TEST(PricerFitterPolicy, AdmissionFailureRetainsPublishedSurfaceAndDecisionTransactionally) {
  SynthPanelSpec good_spec = make_spy_synthetic_spec();
  auto good_panel = make_synthetic_american_panel(good_spec);
  ASSERT_TRUE(good_panel.has_value()) << good_panel.error().message();
  auto good_chain = OptionChain::from_frame(good_panel->frame, good_spec.r, good_spec.spot);
  ASSERT_TRUE(good_chain.has_value()) << good_chain.error().message();

  // Publishing the risk-clean fallback (eSSVI) rather than the non-convex primary
  // is a risk-contract behavior, so start from the strict risk policy and keep the
  // two overrides this test exercises (min fitted expiries and the calendar-arb
  // relaxation). Under the default mark contract the primary would publish
  // directly and its evidence would not be invariant-clean.
  PricerConfig config;
  config.admission = atx::vol::risk_admission_policy();
  config.admission.min_fitted_expiries = 2u;
  config.admission.require_calendar_arb_free = false;
  PricerFitter fitter{config};
  ASSERT_TRUE(fitter.fit(*good_chain).has_value());
  const auto *const published_surface = fitter.surface();
  ASSERT_NE(published_surface, nullptr);
  ASSERT_TRUE(fitter.decision().has_value());
  const auto published_kind = fitter.decision()->curve.kind;
  ASSERT_TRUE(fitter.published_report().has_value());
  const std::size_t published_attempts = fitter.published_report()->attempts.size();
  const auto &published_evidence = fitter.published_report()->attempts.back().evidence;
  EXPECT_TRUE(published_evidence.finite_iv_domain);
  EXPECT_TRUE(published_evidence.european_price_bounds);
  EXPECT_TRUE(published_evidence.strike_monotone);
  EXPECT_TRUE(published_evidence.strike_convex);
  EXPECT_TRUE(published_evidence.calendar_total_variance);
  EXPECT_TRUE(published_evidence.forward_variance_nonnegative);

  SynthPanelSpec partial_spec = good_spec;
  partial_spec.expiries.resize(1u);
  auto partial_panel = make_synthetic_american_panel(partial_spec);
  ASSERT_TRUE(partial_panel.has_value()) << partial_panel.error().message();
  auto partial_chain =
      OptionChain::from_frame(partial_panel->frame, partial_spec.r, partial_spec.spot);
  ASSERT_TRUE(partial_chain.has_value()) << partial_chain.error().message();

  const auto rejected = fitter.fit(*partial_chain);
  ASSERT_FALSE(rejected.has_value());
  EXPECT_EQ(fitter.surface(), published_surface);
  ASSERT_TRUE(fitter.decision().has_value());
  EXPECT_EQ(fitter.decision()->curve.kind, published_kind);
  ASSERT_TRUE(fitter.published_report().has_value());
  EXPECT_TRUE(fitter.published_report()->published);
  EXPECT_EQ(fitter.published_report()->attempts.size(), published_attempts);
  ASSERT_TRUE(fitter.last_attempt_report().has_value());
  EXPECT_FALSE(fitter.last_attempt_report()->published);
  ASSERT_GT(fitter.last_attempt_report()->attempts.size(), 1u);
  EXPECT_EQ(fitter.last_attempt_report()->attempts.front().admission.primary_reason,
            atx::vol::SurfaceAdmissionReason::InsufficientFittedExpiries);
  for (const auto &attempt : fitter.last_attempt_report()->attempts) {
    EXPECT_FALSE(attempt.admission.admitted);
  }
}

TEST(PricerFitterPolicy, DuplicateMaturityIsRejectedAndReportedWithoutDoubleCounting) {
  SynthPanelSpec spec = make_spy_synthetic_spec();
  auto panel = make_synthetic_american_panel(spec);
  ASSERT_TRUE(panel.has_value()) << panel.error().message();
  auto chain = OptionChain::from_frame(panel->frame, spec.r, spec.spot);
  ASSERT_TRUE(chain.has_value()) << chain.error().message();
  auto &under = const_cast<atx::vol::Underlying &>(chain->underlying());
  ASSERT_GE(under.chains.size(), 2u);
  under.chains[1].T = under.chains[0].T;

  PricerConfig config;
  config.curve = atx::vol::CurveConfig{};
  PricerFitter fitter{config};
  const auto rejected = fitter.fit(*chain);
  ASSERT_FALSE(rejected.has_value());
  ASSERT_TRUE(fitter.last_attempt_report().has_value());
  ASSERT_EQ(fitter.last_attempt_report()->attempts.size(), 1u);
  const auto &attempt = fitter.last_attempt_report()->attempts.front();
  EXPECT_EQ(attempt.stage, atx::vol::SurfaceBuildStage::InputValidation);
  EXPECT_EQ(attempt.admission.primary_reason, atx::vol::SurfaceAdmissionReason::DuplicateMaturity);
  EXPECT_LE(attempt.evidence.fitted_expiries, attempt.evidence.attempted_expiries);
}

TEST(PricerFitterPolicy, SelectorFailureUpdatesAttemptOnlyAndPreservesPublishedState) {
  SynthPanelSpec good_spec = make_spy_synthetic_spec();
  auto good_panel = make_synthetic_american_panel(good_spec);
  ASSERT_TRUE(good_panel.has_value()) << good_panel.error().message();
  auto good_chain = OptionChain::from_frame(good_panel->frame, good_spec.r, good_spec.spot);
  ASSERT_TRUE(good_chain.has_value()) << good_chain.error().message();

  PricerConfig config;
  config.policy.mode = atx::vol::FitSelectionMode::CrossValidated;
  atx::vol::CurveConfig linear;
  linear.kind = atx::vol::VolCurveKind::LinearVariance;
  config.selector.candidates = {linear};
  config.admission.require_calendar_arb_free = false;
  PricerFitter fitter{config};
  ASSERT_TRUE(fitter.fit(*good_chain).has_value());
  const auto *const published = fitter.surface();
  const auto published_kind = fitter.decision()->curve.kind;

  SynthPanelSpec too_short = good_spec;
  too_short.expiries.resize(1u);
  auto short_panel = make_synthetic_american_panel(too_short);
  ASSERT_TRUE(short_panel.has_value()) << short_panel.error().message();
  auto short_chain = OptionChain::from_frame(short_panel->frame, too_short.r, too_short.spot);
  ASSERT_TRUE(short_chain.has_value()) << short_chain.error().message();
  auto &short_under = const_cast<atx::vol::Underlying &>(short_chain->underlying());
  ASSERT_EQ(short_under.chains.size(), 1u);
  short_under.chains.front().T = 0.01;
  ASSERT_FALSE(fitter.fit(*short_chain).has_value());

  EXPECT_EQ(fitter.surface(), published);
  EXPECT_EQ(fitter.decision()->curve.kind, published_kind);
  EXPECT_TRUE(fitter.published_report()->published);
  ASSERT_TRUE(fitter.last_attempt_report().has_value());
  EXPECT_FALSE(fitter.last_attempt_report()->published);
  ASSERT_EQ(fitter.last_attempt_report()->attempts.size(), 1u);
  EXPECT_EQ(fitter.last_attempt_report()->attempts.front().stage,
            atx::vol::SurfaceBuildStage::Selection);
}

TEST(PricerFitterPolicy, SessionOverlayCurveIsThePublishedDecisionAndReportCurve) {
  SynthPanelSpec spec = make_spy_synthetic_spec();
  auto panel = make_synthetic_american_panel(spec);
  ASSERT_TRUE(panel.has_value()) << panel.error().message();
  auto chain = OptionChain::from_frame(panel->frame, spec.r, spec.spot);
  ASSERT_TRUE(chain.has_value()) << chain.error().message();

  PricerConfig config;
  config.admission.consumer = atx::vol::SurfaceConsumer::Mark;
  config.admission.require_calendar_arb_free = false;
  PricerFitter fitter{config};
  const auto fitted = fitter.fit(*chain, [](atx::vol::SessionInputs &inputs) {
    inputs.curve.kind = atx::vol::VolCurveKind::LinearVariance;
  });
  ASSERT_TRUE(fitted.has_value()) << fitted.error().to_string();
  ASSERT_TRUE(fitter.decision().has_value());
  ASSERT_TRUE(fitter.published_report().has_value());
  EXPECT_EQ(fitter.decision()->curve.kind, atx::vol::VolCurveKind::LinearVariance);
  EXPECT_EQ(fitter.published_report()->primary_curve.kind, atx::vol::VolCurveKind::LinearVariance);
  EXPECT_EQ(fitter.published_report()->published_curve.kind,
            atx::vol::VolCurveKind::LinearVariance);
  const auto &evidence = fitter.published_report()->attempts.front().evidence;
  EXPECT_TRUE(evidence.finite_iv_domain);
  EXPECT_TRUE(evidence.european_price_bounds);
}

TEST(PricerFitterPolicy, KnownTruthConvexRiskSurfaceReportsIndependentInvariantEvidence) {
  SynthPanelSpec spec = make_spy_synthetic_spec();
  auto panel = make_synthetic_american_panel(spec);
  ASSERT_TRUE(panel.has_value()) << panel.error().message();
  auto chain = OptionChain::from_frame(panel->frame, spec.r, spec.spot);
  ASSERT_TRUE(chain.has_value()) << chain.error().message();
  PricerConfig config;
  config.preset = FitPreset::Fast;
  config.curve = atx::vol::CurveConfig{};
  config.curve->kind = atx::vol::VolCurveKind::ConvexDense;
  config.curve->convex.node_cap = 40u;
  PricerFitter fitter{config};

  const auto fitted = fitter.fit(*chain);
  if (!fitted.has_value()) {
    ASSERT_TRUE(fitter.last_attempt_report().has_value());
    ASSERT_FALSE(fitter.last_attempt_report()->attempts.empty());
    const auto &evidence = fitter.last_attempt_report()->attempts.front().evidence;
    EXPECT_TRUE(evidence.finite_iv_domain);
    EXPECT_TRUE(evidence.european_price_bounds);
    EXPECT_TRUE(evidence.strike_monotone);
    EXPECT_TRUE(evidence.strike_convex);
    EXPECT_TRUE(evidence.calendar_total_variance);
    EXPECT_TRUE(evidence.forward_variance_nonnegative);
  }
  EXPECT_TRUE(fitted.has_value()) << fitted.error().to_string();
}

// rfx task 5a (perf C1): `VolaSession::build`'s certification layer now
// consumes the parallel de-Am prepass's already-computed per-slice carry +
// inversion-audit diagnostics instead of a second, serial re-derivation. Prove
// that dedup is behavior-preserving: fit the SAME SPY-dense board twice --
// once with PricerConfig::fit_workers forced serial, once auto
// (hardware_concurrency) -- and assert the certification diagnostics AND
// the admission outcome are bit-identical, per the S0-3
// expect_per_expiry_bit_identical precedent. The default auto-routed policy
// on this dense board serves BOTH a LinearVariance market mark and a
// ConvexDense risk surface (see perf finding C1's profile), so this exercises
// both curve-driver call sites the dedup touched.
TEST(PricerFitterPolicy, CertificationAndAdmissionBitIdenticalAcrossFitWorkerCounts) {
  SynthPanelSpec spec = make_spy_synthetic_spec();
  auto panel = make_synthetic_american_panel(spec);
  ASSERT_TRUE(panel.has_value()) << panel.error().message();
  auto chain = OptionChain::from_frame(panel->frame, spec.r, spec.spot);
  ASSERT_TRUE(chain.has_value()) << chain.error().message();

  // MERGE: the merged default (PricerConfig{}) serves a SINGLE mark-grade
  // surface (main's transactional fit; da718f7 "default serves marks"). The
  // dual mark+risk bundle this determinism check exercises is the explicit v2
  // opt-in, so request it explicitly here (quality_mode flips is_v2_request()).
  const auto dual_config = [](unsigned fit_workers) {
    PricerConfig config;
    config.quality_mode = atx::vol::FitQualityMode::Latency;
    config.outputs = atx::vol::SurfaceOutputs::MarketMarkAndRisk;
    config.fit_workers = fit_workers;
    return config;
  };
  PricerFitter serial{dual_config(1u)};
  const auto t0 = std::chrono::steady_clock::now();
  ASSERT_TRUE(serial.fit(*chain).has_value());
  const auto t1 = std::chrono::steady_clock::now();
  const atx::vol::SurfaceBundle serial_bundle = serial.bundle();

  PricerFitter parallel{dual_config(0u)};
  const auto t2 = std::chrono::steady_clock::now();
  ASSERT_TRUE(parallel.fit(*chain).has_value());
  const auto t3 = std::chrono::steady_clock::now();
  const atx::vol::SurfaceBundle parallel_bundle = parallel.bundle();

  // Informational only -- NOT a hard timing assert (flaky under shared CI
  // load; mirrors curve_fit_parallel_test.cpp's SPY prints). rfx task 5c
  // ledger datum: PricerFitter::fit cold wall time (both market-mark +
  // risk builds) with the de-Am prepass forced serial vs auto-parallel.
  const double ms_serial = std::chrono::duration<double, std::milli>(t1 - t0).count();
  const double ms_parallel = std::chrono::duration<double, std::milli>(t3 - t2).count();
  std::printf("[PricerFitterPolicy SPY] fit_workers=1 %.1fms, fit_workers=0(auto) %.1fms, "
              "speedup=%.2fx\n",
              ms_serial, ms_parallel, (ms_parallel > 0.0) ? (ms_serial / ms_parallel) : 0.0);

  // Admission outcome: same health state + failure reasons for both surfaces.
  EXPECT_EQ(serial_bundle.market_mark_health.state, parallel_bundle.market_mark_health.state);
  EXPECT_EQ(serial_bundle.market_mark_health.reasons, parallel_bundle.market_mark_health.reasons);
  EXPECT_EQ(serial_bundle.risk_health.state, parallel_bundle.risk_health.state);
  EXPECT_EQ(serial_bundle.risk_health.reasons, parallel_bundle.risk_health.reasons);
  ASSERT_EQ(serial_bundle.risk_health.state, atx::vol::SurfaceState::Healthy);

  ASSERT_NE(serial_bundle.market_mark, nullptr);
  ASSERT_NE(parallel_bundle.market_mark, nullptr);
  ASSERT_NE(serial_bundle.risk, nullptr);
  ASSERT_NE(parallel_bundle.risk, nullptr);
  EXPECT_EQ(serial_bundle.market_mark->session().inputs().fit_workers, 1u);
  EXPECT_EQ(serial_bundle.risk->session().inputs().fit_workers, 1u);
  EXPECT_EQ(parallel_bundle.market_mark->session().inputs().fit_workers, 0u);
  EXPECT_EQ(parallel_bundle.risk->session().inputs().fit_workers, 0u);

  {
    SCOPED_TRACE("market_mark");
    expect_session_diagnostics_bit_identical(serial_bundle.market_mark->diagnostics(),
                                             parallel_bundle.market_mark->diagnostics());
    expect_slice_diagnostics_bit_identical(
        serial_bundle.market_mark->session().slice_diagnostics(),
        parallel_bundle.market_mark->session().slice_diagnostics());
  }
  {
    SCOPED_TRACE("risk");
    expect_session_diagnostics_bit_identical(serial_bundle.risk->diagnostics(),
                                             parallel_bundle.risk->diagnostics());
    expect_slice_diagnostics_bit_identical(serial_bundle.risk->session().slice_diagnostics(),
                                           parallel_bundle.risk->session().slice_diagnostics());
  }
}

// rfx task 5 review fix (item 3): sibling of the test above for the eSSVI
// (default-surface) branch, where the Critical indexing fix lives —
// VolaSession::build's eSSVI path consumes run_surface_parity's per-slice
// carry through collect_input_diagnostics' precomputed span.
// run_surface_parity itself never reads fit_workers (it is fully serial), so
// the env toggle here pins the seam's determinism CONTRACT; the per-slice
// VALUE correctness against a serially recomputed reference is pinned by
// session_test's CarryCertificationMatchesSerialReferencePerSlice. The
// distinctness assertion at the end is the in-suite regression tripwire for
// the indexing bug (every slice reporting slice 0's carry).
TEST(PricerFitterPolicy, EssviCertificationAndAdmissionBitIdenticalAcrossFitWorkerCounts) {
  FitWorkersEnvGuard env_guard; // restores ATX_VOL_FIT_WORKERS on scope exit

  SynthPanelSpec spec = make_spy_synthetic_spec();
  auto panel = make_synthetic_american_panel(spec);
  ASSERT_TRUE(panel.has_value()) << panel.error().message();
  auto chain = OptionChain::from_frame(panel->frame, spec.r, spec.spot);
  ASSERT_TRUE(chain.has_value()) << chain.error().message();

  // The served eSSVI risk rung (the standard fallback family / explicit pin),
  // as in RiskEssviRungServesOnlyAuditedInversions above.
  PricerConfig config;
  config.quality_mode = atx::vol::FitQualityMode::Balanced;
  config.outputs = atx::vol::SurfaceOutputs::Risk;
  atx::vol::CurveConfig curve;
  curve.kind = atx::vol::VolCurveKind::Essvi;
  config.curve = curve;

  set_fit_workers_env("1");
  ASSERT_EQ(atx::vol::atx_auto_worker_count(), 1u);
  PricerFitter serial{config};
  ASSERT_TRUE(serial.fit(*chain).has_value());
  const atx::vol::SurfaceBundle serial_bundle = serial.bundle();

  unset_fit_workers_env();
  PricerFitter parallel{config};
  ASSERT_TRUE(parallel.fit(*chain).has_value());
  const atx::vol::SurfaceBundle parallel_bundle = parallel.bundle();

  EXPECT_EQ(serial_bundle.risk_health.state, parallel_bundle.risk_health.state);
  EXPECT_EQ(serial_bundle.risk_health.reasons, parallel_bundle.risk_health.reasons);
  ASSERT_EQ(serial_bundle.risk_health.state, atx::vol::SurfaceState::Healthy);
  ASSERT_NE(serial_bundle.risk, nullptr);
  ASSERT_NE(parallel_bundle.risk, nullptr);
  expect_session_diagnostics_bit_identical(serial_bundle.risk->diagnostics(),
                                           parallel_bundle.risk->diagnostics());
  expect_slice_diagnostics_bit_identical(serial_bundle.risk->session().slice_diagnostics(),
                                         parallel_bundle.risk->session().slice_diagnostics());

  // Regression tripwire for the review's Critical finding: with the indexing
  // bug every slice reported slice 0's carry — require at least two slices to
  // differ in some carry field.
  const auto slices = serial_bundle.risk->session().slice_diagnostics();
  ASSERT_GE(slices.size(), std::size_t{2});
  bool any_distinct = false;
  for (std::size_t i = 1; i < slices.size(); ++i) {
    if (slices[i].carry.dispersion != slices[0].carry.dispersion ||
        slices[i].carry.max_pcp_residual != slices[0].carry.max_pcp_residual ||
        slices[i].carry.confidence_half_width != slices[0].carry.confidence_half_width) {
      any_distinct = true;
    }
  }
  EXPECT_TRUE(any_distinct);
}

TEST(PricerFitterPolicy, EventContextBuildsAndServesC8Surface) {
  SynthPanelSpec spec = make_spy_synthetic_spec();
  spec.uid = "AAPL";
  auto panel = make_synthetic_american_panel(spec);
  ASSERT_TRUE(panel.has_value()) << panel.error().message();
  auto chain = OptionChain::from_frame(panel->frame, spec.r, spec.spot);
  ASSERT_TRUE(chain.has_value()) << chain.error().message();

  PricerConfig cfg;
  cfg.context.event_phase = atx::vol::EventPhase::PreAnnouncement;
  cfg.policy.sparse_validation_floor = 0;
  PricerFitter fitter{cfg};
  ASSERT_TRUE(fitter.fit(*chain).has_value());
  ASSERT_TRUE(fitter.decision().has_value());
  EXPECT_EQ(fitter.decision()->curve.kind, atx::vol::VolCurveKind::C8);
  auto priced = fitter.surface()->session().to_priced_surface();
  ASSERT_TRUE(priced.has_value()) << priced.error().to_string();
  ASSERT_GT(priced->n_slices(), 0u);
  EXPECT_EQ(priced->kind_at(0), atx::vol::VolCurveKind::C8);
  EXPECT_TRUE(std::isfinite(priced->iv(spec.spot, spec.expiries.front().T)));
}

// Task 2a (carry C1): the eSSVI rung is a reachable served risk family (the
// standard fallback after a ConvexDense rejection, or an explicit pin). Under
// the risk policy its FIT inversions must run the cold-reference audit, and
// the certificate must describe those audited fit rows — a shortcut result
// must never reach the served path without the §8.1 audit.
TEST_F(PricerFitterTest, RiskEssviRungServesOnlyAuditedInversions) {
  PricerConfig config;
  config.quality_mode = atx::vol::FitQualityMode::Balanced;
  config.outputs = atx::vol::SurfaceOutputs::Risk;
  atx::vol::CurveConfig curve;
  curve.kind = atx::vol::VolCurveKind::Essvi;
  config.curve = curve;
  PricerFitter fitter{config};
  ASSERT_TRUE(fitter.fit(*chain_).has_value());
  const atx::vol::SurfaceBundle bundle = fitter.bundle();
  ASSERT_NE(bundle.risk, nullptr);
  EXPECT_EQ(bundle.risk_health.state, atx::vol::SurfaceState::Healthy);

  const auto &session = bundle.risk->session();
  EXPECT_TRUE(session.inputs().deam.audit_fit_inversions);
  const auto &diag = session.diagnostics();
  EXPECT_GT(diag.n_iv_proposed, std::size_t{0});
  EXPECT_EQ(diag.n_iv_audited, diag.n_iv_proposed);
  EXPECT_TRUE(diag.inversion_certified);
}

// F2 (R-02): the risk rebuild applies a served-breadth floor on ALL routes (a
// superset of the mark-path selector_served_admission_policy), so a rebuild whose
// served quote coverage is below the floor can never admit into the safety-
// critical risk surface. Derive the floor from the board's OWN achieved coverage
// so the assertion does not depend on the exact synthetic quote count.
TEST_F(PricerFitterTest, RiskRebuildRejectedBelowServedBreadthFloor) {
  PricerConfig config;
  config.quality_mode = atx::vol::FitQualityMode::Balanced;
  config.outputs = atx::vol::SurfaceOutputs::Risk;
  atx::vol::CurveConfig curve;
  curve.kind = atx::vol::VolCurveKind::Essvi;
  config.curve = curve; // pin the family: isolate admission from the fallback ladder

  // The clean synthetic board reprices every scored quote in-band, so the served
  // metric (n_within / n) would be exactly 1.0 — no room for a floor to sit below.
  // Tighten every quote's band symmetrically around its UNCHANGED mid: the mids
  // (and thus the arbitrage-free fit) are untouched, but the parsimonious eSSVI's
  // small smoothing residual now exceeds the narrowed band on the strikes where
  // the true smile bends away from it. Served drops below 1.0 with no expiry
  // dropped and no arbitrage introduced — a genuine below-floor case.
  std::vector<OptionId> ids;
  std::vector<double> bids;
  std::vector<double> asks;
  for (const OptionId id : chain_->ids()) {
    const auto option = chain_->at(id);
    ASSERT_TRUE(option.has_value());
    const double mid = option->mid;
    const double half = 0.20 * (option->ask - option->bid); // 40% of the original spread
    ids.push_back(id);
    bids.push_back(mid - half);
    asks.push_back(mid + half);
  }
  ASSERT_FALSE(ids.empty());
  ASSERT_TRUE(chain_->update_quotes(ids, bids, asks).has_value());

  // Control: under the production served floor (0.50) this board still admits.
  PricerFitter admits{config};
  ASSERT_TRUE(admits.fit(*chain_).has_value());
  EXPECT_EQ(admits.bundle().risk_health.state, atx::vol::SurfaceState::Healthy);
  ASSERT_NE(admits.bundle().risk, nullptr);

  // Measure the coverage this board actually served, then set the floor strictly
  // between it and 1.0 so the SAME board is now "below floor".
  ASSERT_TRUE(admits.last_attempt_report().has_value());
  const auto &evidence = admits.last_attempt_report()->attempts.back().evidence;
  ASSERT_GT(evidence.attempted_quotes, 0u);
  const double served_coverage =
      static_cast<double>(evidence.fitted_quotes) / static_cast<double>(evidence.attempted_quotes);
  ASSERT_LT(served_coverage, 1.0) << "the perturbed quotes must be served out of band";
  ASSERT_GE(served_coverage, 0.50) << "the robust fit must still clear the production floor";
  config.selector.min_served_quote_coverage = 0.5 * (served_coverage + 1.0);

  // Below-floor: the risk rebuild must now be rejected and publish no surface.
  PricerFitter rejects{config};
  const auto result = rejects.fit(*chain_);
  EXPECT_FALSE(result.has_value());
  EXPECT_EQ(rejects.bundle().risk, nullptr);
  EXPECT_NE(rejects.bundle().risk_health.state, atx::vol::SurfaceState::Healthy);
}

// F2 metric gate (bt-spyfit-rca): the quote-coverage numerator/denominator must
// measure SERVED quotes — the admitted-universe quotes the built surface reprices
// in-band, counted from the parity serve-check (n_within over n) — NOT the
// node-capped fit-observation count over raw board strikes. On a wide board a
// dense fit capped to `max_obs_per_slice` nodes serves far more of its scored
// quotes than the node/strikes ratio suggests; the old numerator understated
// coverage and wrongly rejected a healthy surface. This gate pins the served
// semantics so the metric can never regress to node-counting.
TEST_F(PricerFitterTest, QuoteCoverageMeasuresServedQuotesNotCappedFitNodes) {
  PricerConfig config;
  config.preset = FitPreset::Fast;
  config.curve = atx::vol::CurveConfig{atx::vol::VolCurveKind::ConvexDense};
  config.max_obs_per_slice = 10u; // bind the node cap hard vs the 25-strike ladder
  config.use_correction_cache = false;
  config.use_deam_cache_for_fit = false;
  config.score_parity = true;                 // the serve-check must run
  config.admission.min_quote_coverage = 0.50; // production served-breadth floor

  PricerFitter fitter{config};
  (void)fitter.fit(*chain_);
  ASSERT_TRUE(fitter.last_attempt_report().has_value());
  const atx::vol::SurfaceBuildAttemptReport &attempt =
      fitter.last_attempt_report()->attempts.front();
  ASSERT_TRUE(attempt.build_succeeded);
  const atx::vol::SurfaceAdmissionEvidence &ev = attempt.evidence;
  ASSERT_GT(ev.attempted_quotes, 0u);

  // The node cap binds, and the OLD (buggy) numerator/denominator — fit NODES over
  // raw board STRIKES — sits below the production served floor.
  std::size_t node_sum = 0u;
  std::size_t strike_sum = 0u;
  bool node_cap_bound = false;
  for (const atx::vol::ExpiryBuildReport &e : attempt.expiries) {
    if (e.outcome == atx::vol::ExpiryBuildOutcome::Fitted) {
      node_sum += e.n_used;
      node_cap_bound = node_cap_bound || e.n_used <= config.max_obs_per_slice.value();
      strike_sum += chain_->underlying().chains[e.expiry_index].n_strikes();
    }
  }
  ASSERT_TRUE(node_cap_bound) << "the node cap must actually bind for this gate";
  ASSERT_GT(strike_sum, 0u);
  const double node_over_strikes =
      static_cast<double>(node_sum) / static_cast<double>(strike_sum);
  EXPECT_LT(node_over_strikes, 0.50) << "node cap did not bind hard enough to prove the point";

  // The metric instead measures SERVED (in-band) over ADMITTED-scored quotes,
  // which clears the floor — so the node-capped surface is NOT rejected.
  const double served =
      static_cast<double>(ev.fitted_quotes) / static_cast<double>(ev.attempted_quotes);
  EXPECT_GT(served, node_over_strikes) << "served=" << served << " node=" << node_over_strikes;
  EXPECT_GE(served, 0.50) << "served coverage must clear the production floor";
  EXPECT_FALSE(atx::vol::has_admission_failure(
      attempt.admission, atx::vol::SurfaceAdmissionReason::InsufficientQuoteCoverage));

  // Pin the numerator/denominator to the serve-check itself: they are exactly the
  // parity n_within / n board-level sums, never the node count.
  ASSERT_NE(fitter.surface(), nullptr);
  std::size_t within_sum = 0u;
  std::size_t scored_sum = 0u;
  for (const atx::vol::ParityReport &p : fitter.surface()->session().parity()) {
    within_sum += p.n_within;
    scored_sum += p.n;
  }
  EXPECT_EQ(ev.fitted_quotes, within_sum);
  EXPECT_EQ(ev.attempted_quotes, scored_sum);
}

TEST_F(PricerFitterTest, StageTimingsAreOptInAndReportedByTheBuiltSession) {
  PricerFitter uninstrumented{essvi_config()};
  ASSERT_TRUE(uninstrumented.fit(*chain_).has_value());
  ASSERT_NE(uninstrumented.surface(), nullptr);
  const atx::vol::SurfaceFitStageTimings &disabled =
      uninstrumented.surface()->diagnostics().fit_timings;
  EXPECT_FALSE(disabled.collected);
  EXPECT_EQ(disabled.total_wall_ms, 0.0);
  EXPECT_EQ(disabled.carry_solve_ms, 0.0);
  EXPECT_EQ(disabled.observation_deam_ms, 0.0);
  EXPECT_EQ(disabled.slice_fit_ms, 0.0);
  EXPECT_EQ(disabled.audit_ms, 0.0);
  EXPECT_EQ(disabled.calendar_validation_ms, 0.0);

  PricerConfig config = essvi_config();
  config.collect_stage_timings = true;
  config.fit_workers = 1u;
  PricerFitter instrumented{config};
  ASSERT_TRUE(instrumented.fit(*chain_).has_value());
  ASSERT_NE(instrumented.surface(), nullptr);
  const atx::vol::SurfaceFitStageTimings &reported =
      instrumented.surface()->diagnostics().fit_timings;
  EXPECT_TRUE(reported.collected);
  EXPECT_GT(reported.total_wall_ms, 0.0);
  EXPECT_GT(reported.carry_solve_ms, 0.0);
  EXPECT_GT(reported.observation_deam_ms, 0.0);
  EXPECT_GT(reported.slice_fit_ms, 0.0);
  EXPECT_GE(reported.audit_ms, 0.0);
  EXPECT_GE(reported.calendar_validation_ms, 0.0);
}

// Task 2d (carry I5): an expiry whose quotes are all crossed fails the carry
// gate and drops out of the fitted risk surface. The surviving surface is
// still served, but the gap is surfaced: Degraded state + CarryGap reason +
// the skip counted in diagnostics — never a clean Healthy with the expiry
// silently missing.
TEST_F(PricerFitterTest, CarryFailedExpiryPublishesDegradedWithCarryGap) {
  PricerConfig config;
  config.quality_mode = atx::vol::FitQualityMode::Balanced;
  config.outputs = atx::vol::SurfaceOutputs::Risk;
  PricerFitter fitter{config};

  // Cross every quote of the second-shortest expiry (bid > ask stamps the
  // Locked|Crossed flags), so no co-terminal pair survives leg validity.
  const double bad_T = chain_->underlying().chains[1].T;
  std::vector<OptionId> ids;
  std::vector<double> bids;
  std::vector<double> asks;
  for (const OptionId id : chain_->ids()) {
    const auto option = chain_->at(id);
    ASSERT_TRUE(option.has_value());
    if (std::fabs(option->T - bad_T) > 1.0e-12)
      continue;
    ids.push_back(id);
    bids.push_back(2.0);
    asks.push_back(1.0);
  }
  ASSERT_FALSE(ids.empty());
  ASSERT_TRUE(chain_->update_quotes(ids, bids, asks).has_value());

  ASSERT_TRUE(fitter.fit(*chain_).has_value());
  const atx::vol::SurfaceBundle bundle = fitter.bundle();
  ASSERT_NE(bundle.risk, nullptr);
  EXPECT_EQ(bundle.risk_health.state, atx::vol::SurfaceState::Degraded);
  EXPECT_TRUE(atx::vol::has_validation_failure(bundle.risk_health.reasons,
                                               atx::vol::ValidationFailure::CarryGap));
  EXPECT_TRUE(bundle.risk_health.serving_candidate());
  EXPECT_FALSE(bundle.risk_health.using_fallback());
  EXPECT_EQ(bundle.risk->session().diagnostics().n_carry_skipped_expiries, std::size_t{1});
}

// Task 2d follow-up (review I-1): a Degraded+CarryGap surface must STAY
// Degraded with the reason retained through a successful local refit — the
// gap (a still-missing expiry) is a property of the served surface, not of
// the one slice that was refit. Without carrying the non-geometric failure
// context into the refit admission, one quote tick would relabel the gapped
// surface clean Healthy (§5.2's hidden-gap state again).
TEST_F(PricerFitterTest, RefitKeepsCarryGapDegradedWhileExpiryStillMissing) {
  PricerConfig config;
  config.quality_mode = atx::vol::FitQualityMode::Balanced;
  config.outputs = atx::vol::SurfaceOutputs::Risk;
  PricerFitter fitter{config};

  const double bad_T = chain_->underlying().chains[1].T;
  std::vector<OptionId> ids;
  std::vector<double> bids;
  std::vector<double> asks;
  for (const OptionId id : chain_->ids()) {
    const auto option = chain_->at(id);
    ASSERT_TRUE(option.has_value());
    if (std::fabs(option->T - bad_T) > 1.0e-12)
      continue;
    ids.push_back(id);
    bids.push_back(2.0);
    asks.push_back(1.0); // crossed: carry fails for this expiry
  }
  ASSERT_FALSE(ids.empty());
  ASSERT_TRUE(chain_->update_quotes(ids, bids, asks).has_value());

  ASSERT_TRUE(fitter.fit(*chain_).has_value());
  const atx::vol::SurfaceBundle gapped = fitter.bundle();
  ASSERT_NE(gapped.risk, nullptr);
  ASSERT_EQ(gapped.risk_health.state, atx::vol::SurfaceState::Degraded);
  ASSERT_TRUE(atx::vol::has_validation_failure(gapped.risk_health.reasons,
                                               atx::vol::ValidationFailure::CarryGap));

  // Local refit of a FITTED slice (index 0, untouched expiry) succeeds and
  // publishes a new generation — but the surface still misses the crossed
  // expiry, so health must remain Degraded with CarryGap retained.
  ASSERT_TRUE(fitter.refit_risk_slice(*chain_, 0u).has_value());
  const atx::vol::SurfaceBundle after = fitter.bundle();
  ASSERT_NE(after.risk, nullptr);
  EXPECT_EQ(after.risk->generation(), gapped.risk->generation() + 1u);
  EXPECT_EQ(after.risk_health.state, atx::vol::SurfaceState::Degraded);
  EXPECT_TRUE(atx::vol::has_validation_failure(after.risk_health.reasons,
                                               atx::vol::ValidationFailure::CarryGap));
  EXPECT_TRUE(after.risk_health.serving_candidate());
  EXPECT_EQ(after.risk->session().diagnostics().n_carry_skipped_expiries, std::size_t{1});
}

// rfx Task 3 review follow-up (oracle I-2): the ConvexDense served-price
// bound self-check must actually reach admission — a session whose
// diagnostics carry n_price_bound_violations > 0 is rejected with
// PriceBounds through the SAME merge seam as CarryGap. Drives the exported
// seam (merge_session_failure_context) with directly-constructed
// diagnostics because the 3c fail-closed QP makes a real board that FITS a
// sub-intrinsic node unconstructible — which is exactly why the seam is the
// production path for this failure class (the clamp count is computed by
// arb_check_price_bounds over the served surface at build/refit time).
TEST(RiskSurfaceAdmission, PriceBoundClampCountRejectsCandidateWithPriceBounds) {
  using atx::vol::AdmissionDecision;
  using atx::vol::decide_risk_surface_admission;
  using atx::vol::finalize_validation_digest;
  using atx::vol::FitQualityMode;
  using atx::vol::has_validation_failure;
  using atx::vol::merge_session_failure_context;
  using atx::vol::SessionDiagnostics;
  using atx::vol::SurfaceFallback;
  using atx::vol::SurfaceState;
  using atx::vol::ValidationDigest;
  using atx::vol::ValidationFailure;

  // A candidate the geometric oracle found clean, from a session whose only
  // defect is two served-price clamp events.
  SessionDiagnostics diagnostics;
  diagnostics.carry_confident = true;     // isolate the price-bound path:
  diagnostics.inversion_certified = true; // no other non-geometric failure
  diagnostics.n_price_bound_violations = 2;

  ValidationDigest digest; // geometrically clean (failures == None)
  ASSERT_TRUE(digest.admitted());
  merge_session_failure_context(diagnostics, digest);
  finalize_validation_digest(digest);

  EXPECT_TRUE(has_validation_failure(digest.failures, ValidationFailure::PriceBounds));
  // Review finding 3: the merge reports the clamp COUNT, not a bare bit.
  EXPECT_EQ(digest.n_price_bound_violations, 2u);

  const AdmissionDecision decision = decide_risk_surface_admission(
      digest, FitQualityMode::Balanced, 42, 41, SurfaceFallback::LastKnownGood);
  EXPECT_FALSE(decision.publish_candidate);
  EXPECT_TRUE(has_validation_failure(decision.health.reasons, ValidationFailure::PriceBounds));
  EXPECT_TRUE(decision.health.using_fallback());
  EXPECT_EQ(decision.health.state, SurfaceState::Degraded);

  // OR-only / additive-only: merging over a digest that already carries a
  // geometric failure and a non-zero count must clear neither.
  ValidationDigest dirty;
  dirty.failures = ValidationFailure::Butterfly;
  dirty.n_price_bound_violations = 3;
  merge_session_failure_context(diagnostics, dirty);
  EXPECT_TRUE(has_validation_failure(dirty.failures, ValidationFailure::Butterfly));
  EXPECT_TRUE(has_validation_failure(dirty.failures, ValidationFailure::PriceBounds));
  EXPECT_EQ(dirty.n_price_bound_violations, 5u);

  // A clean session leaves a clean digest untouched (no false positive).
  SessionDiagnostics clean;
  clean.carry_confident = true;
  clean.inversion_certified = true;
  ValidationDigest untouched;
  merge_session_failure_context(clean, untouched);
  EXPECT_TRUE(untouched.admitted());
  EXPECT_EQ(untouched.n_price_bound_violations, 0u);
}

TEST(LinearVarianceCurve, InterpolatesTotalVarianceAndClampsWings) {
  atx::vol::LinearVarianceCurve curve(0.5, 100.0, 0.98, std::vector<double>{-0.2, 0.0, 0.3},
                                      std::vector<double>{0.03, 0.04, 0.10});
  EXPECT_DOUBLE_EQ(curve.w(-1.0), 0.03);
  EXPECT_DOUBLE_EQ(curve.w(1.0), 0.10);
  EXPECT_DOUBLE_EQ(curve.w(0.15), 0.07);
  EXPECT_DOUBLE_EQ(curve.iv(0.0), std::sqrt(0.04 / 0.5));
}

TEST(LinearVarianceCurve, CalendarFloorUsesPriorBreakpoints) {
  using atx::vol::CurveConfig;
  using atx::vol::FitObs;
  using atx::vol::LinearVarianceCurve;
  using atx::vol::VolCurveKind;

  const LinearVarianceCurve prior(0.25, 100.0, 0.99, std::vector<double>{-0.5, 0.0, 0.5},
                                  std::vector<double>{0.03, 0.08, 0.03});
  std::vector<FitObs> obs(2);
  obs[0].k = -0.5;
  obs[0].w_mkt = 0.04;
  obs[0].active_weight_w = 1.0;
  obs[1].k = 0.5;
  obs[1].w_mkt = 0.04;
  obs[1].active_weight_w = 1.0;

  CurveConfig cfg;
  cfg.kind = VolCurveKind::LinearVariance;
  auto fitted = atx::vol::fit_slice_curve(
      cfg, obs, 100.0, 0.5, 0.98, [&](double k) { return prior.w(k); }, prior.k_nodes());
  ASSERT_TRUE(fitted.has_value()) << fitted.error().to_string();

  for (int i = 0; i <= 100; ++i) {
    const double k = -0.5 + static_cast<double>(i) * 0.01;
    EXPECT_GE((*fitted)->w(k) + 1.0e-15, prior.w(k));
  }
  EXPECT_DOUBLE_EQ((*fitted)->w(0.0), 0.08);
}

} // namespace
