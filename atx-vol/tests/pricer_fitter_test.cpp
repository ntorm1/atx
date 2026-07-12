#include <gtest/gtest.h>

#include <cmath>
#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <vector>

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

  PricerFitter fitter{PricerConfig{}};
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

  PricerConfig config;
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
