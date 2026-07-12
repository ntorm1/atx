#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "atx/core/error.hpp"
#include "atx/vol/chain.hpp"
#include "atx/vol/panel.hpp"
#include "atx/vol/pricer_fitter.hpp"
#include "atx/vol/session.hpp"
#include "atx/vol/spy_fixture.hpp"
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
  ASSERT_TRUE(chain.update_quotes(std::span<const OptionId>{&first, 1u},
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
  ASSERT_TRUE(chain.update_quotes(std::span<const OptionId>{&invalid, 1u},
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
  for (const atx::vol::ParityReport& p : fitter.surface()->session().parity()) {
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
  EXPECT_FALSE(incremental.surface()->session().inputs().use_deam_cache_for_fit);
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
    ASSERT_TRUE(fitter
                    .fit(*chain_, [repair](atx::vol::SessionInputs &inputs) {
                      inputs.calendar_repair = repair;
                    })
                    .has_value());
    const FittedSurface *const published_surface = fitter.surface();
    ASSERT_TRUE(fitter.published_report().has_value());
    const std::size_t published_attempts = fitter.published_report()->attempts.size();
    ASSERT_TRUE(fitter.published_provenance().has_value());
    const atx::vol::FitSnapshotProvenance published_provenance =
        *fitter.published_provenance();

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
    EXPECT_EQ(fitter.published_provenance()->board_revision,
              published_provenance.board_revision);
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

  const auto rejected =
      fitter.refit_expiry(*chain_, static_cast<atx::vol::ExpiryId>(kMiddle));
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
              atx::vol::has_admission_failure(
                  admission, atx::vol::SurfaceAdmissionReason::ForwardVariance) ||
              atx::vol::has_admission_failure(
                  admission, atx::vol::SurfaceAdmissionReason::CalendarArbitrage));
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
                  .fit(*chain_, [](atx::vol::SessionInputs &inputs) {
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

  for (const atx::vol::CalibOpts& opts : unsupported) {
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
  PricerFitter risk_fitter{risk_config};
  const auto risk_status = risk_fitter.fit(*chain_);
  ASSERT_FALSE(risk_status.has_value());
  ASSERT_TRUE(risk_fitter.last_attempt_report().has_value());
  ASSERT_FALSE(risk_fitter.last_attempt_report()->attempts.empty());
  const auto& risk_attempt = risk_fitter.last_attempt_report()->attempts.front();
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

TEST_F(PricerFitterTest, PinnedEssviStillScoresParityWhenGenericScoreFlagIsFalse) {
  PricerConfig config = essvi_config();
  config.score_parity = false;
  PricerFitter fitter{config};
  ASSERT_TRUE(fitter.fit(*chain_).has_value());
  EXPECT_EQ(fitter.surface()->diagnostics().parity_state,
            atx::vol::ParityDiagnosticState::Valid);
  const std::span<const atx::vol::ParityReport> parity = fitter.surface()->session().parity();
  ASSERT_FALSE(parity.empty());
  for (const atx::vol::ParityReport &report : parity) {
    EXPECT_GT(report.n, 0u);
  }
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

TEST_F(PricerFitterTest, HftUsesDirectLinearVarianceCurve) {
  PricerFitter fitter{PricerConfig{.preset = FitPreset::Hft}};
  ASSERT_TRUE(fitter.fit(*chain_).has_value());
  auto priced = fitter.surface()->session().to_priced_surface();
  ASSERT_TRUE(priced.has_value()) << priced.error().to_string();
  ASSERT_GT(priced->n_slices(), 0u);
  EXPECT_EQ(priced->kind_at(0), atx::vol::VolCurveKind::LinearVariance);
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
