#include <gtest/gtest.h>

#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "atx/vol/chain.hpp"
#include "atx/vol/panel.hpp"
#include "atx/vol/parallel_for.hpp"  // atx_auto_worker_count (ATX_VOL_FIT_WORKERS gate)
#include "atx/vol/pricer_fitter.hpp"
#include "atx/vol/risk_surface_validation.hpp"
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

// rfx task 5a (perf C1 dedup): portable ATX_VOL_FIT_WORKERS env set/unset, the
// same mechanism curve_fit_parallel_test.cpp's S0-4' gate uses to force the
// de-Am prepass worker count without a session-level knob (VolaSession::build
// always drives fit_curve_surface with fit_workers=0 / auto).
#if defined(_MSC_VER)
void set_fit_workers_env(const char* value) { ::_putenv_s("ATX_VOL_FIT_WORKERS", value); }
void unset_fit_workers_env() { ::_putenv_s("ATX_VOL_FIT_WORKERS", ""); }
#else
void set_fit_workers_env(const char* value) { ::setenv("ATX_VOL_FIT_WORKERS", value, 1); }
void unset_fit_workers_env() { ::unsetenv("ATX_VOL_FIT_WORKERS"); }
#endif

// RAII guard: restores the prior ATX_VOL_FIT_WORKERS value on scope exit, even
// if an ASSERT_* below exits the test early.
class FitWorkersEnvGuard {
 public:
  FitWorkersEnvGuard() {
#if defined(_MSC_VER)
    char* prev = nullptr;
    std::size_t prev_n = 0;
    had_prev_ = (::_dupenv_s(&prev, &prev_n, "ATX_VOL_FIT_WORKERS") == 0) && (prev != nullptr);
    if (prev != nullptr) {
      prev_val_ = prev;
      std::free(prev);
    }
#else
    const char* prev = std::getenv("ATX_VOL_FIT_WORKERS");
    had_prev_ = prev != nullptr;
    if (prev != nullptr) {
      prev_val_ = prev;
    }
#endif
  }
  FitWorkersEnvGuard(const FitWorkersEnvGuard&) = delete;
  FitWorkersEnvGuard& operator=(const FitWorkersEnvGuard&) = delete;
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
void expect_route_bit_identical(const atx::vol::InversionRouteDiagnostics& a,
                                const atx::vol::InversionRouteDiagnostics& b,
                                const char* label) {
  EXPECT_EQ(a.n_proposed, b.n_proposed) << label;
  EXPECT_EQ(a.n_audited, b.n_audited) << label;
  EXPECT_EQ(a.n_accepted, b.n_accepted) << label;
  EXPECT_EQ(a.n_fallback, b.n_fallback) << label;
  EXPECT_EQ(a.p50_residual_half_spreads, b.p50_residual_half_spreads) << label;
  EXPECT_EQ(a.p95_residual_half_spreads, b.p95_residual_half_spreads) << label;
  EXPECT_EQ(a.max_residual_half_spreads, b.max_residual_half_spreads) << label;
}

void expect_inversion_bit_identical(const atx::vol::DeAmAuditDiagnostics& a,
                                    const atx::vol::DeAmAuditDiagnostics& b) {
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
}

void expect_carry_bit_identical(const atx::vol::SessionCarryDiagnostics& a,
                                const atx::vol::SessionCarryDiagnostics& b) {
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

void expect_slice_diagnostics_bit_identical(
    std::span<const atx::vol::SessionSliceDiagnostics> a,
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

void expect_session_diagnostics_bit_identical(const atx::vol::SessionDiagnostics& a,
                                              const atx::vol::SessionDiagnostics& b) {
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
    if (std::fabs(option->T - target_T) > 1.0e-12) continue;
    const std::size_t quote_idx =
        atx::vol::chain_index(atx::vol::cid_strike_idx(id), atx::vol::cid_side(id));
    if (chain_->underlying().chains.front().flags[quote_idx] != 0u) continue;
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
      before.risk->session().cached_refit_observations(
          chain_->underlying().chains.front(), 0u);
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

// A healthy auto-routed board reports no fallback, so `used_fallback` is a real
// signal rather than a field that is always false.
TEST(PricerFitterPolicy, HealthyAutoFitReportsNoFallback) {
  SynthPanelSpec spec = make_spy_synthetic_spec();
  auto panel = make_synthetic_american_panel(spec);
  ASSERT_TRUE(panel.has_value()) << panel.error().message();
  auto chain = OptionChain::from_frame(panel->frame, spec.r, spec.spot);
  ASSERT_TRUE(chain.has_value()) << chain.error().message();

  PricerFitter fitter{PricerConfig{}};
  ASSERT_TRUE(fitter.fit(*chain).has_value());
  ASSERT_TRUE(fitter.decision().has_value());
  EXPECT_FALSE(fitter.decision()->used_fallback);
  EXPECT_EQ(fitter.decision()->primary_curve.kind, fitter.decision()->curve.kind);
}

// rfx task 5a (perf C1): `VolaSession::build`'s certification layer now
// consumes the parallel de-Am prepass's already-computed per-slice carry +
// inversion-audit diagnostics instead of a second, serial re-derivation. Prove
// that dedup is behavior-preserving: fit the SAME SPY-dense board twice --
// once with the de-Am prepass forced serial (ATX_VOL_FIT_WORKERS=1), once
// auto (hardware_concurrency) -- and assert the certification diagnostics AND
// the admission outcome are bit-identical, per the S0-3
// expect_per_expiry_bit_identical precedent. The default auto-routed policy
// on this dense board serves BOTH a LinearVariance market mark and a
// ConvexDense risk surface (see perf finding C1's profile), so this exercises
// both curve-driver call sites the dedup touched.
TEST(PricerFitterPolicy, CertificationAndAdmissionBitIdenticalAcrossFitWorkerCounts) {
  FitWorkersEnvGuard env_guard;  // restores ATX_VOL_FIT_WORKERS on scope exit

  SynthPanelSpec spec = make_spy_synthetic_spec();
  auto panel = make_synthetic_american_panel(spec);
  ASSERT_TRUE(panel.has_value()) << panel.error().message();
  auto chain = OptionChain::from_frame(panel->frame, spec.r, spec.spot);
  ASSERT_TRUE(chain.has_value()) << chain.error().message();

  set_fit_workers_env("1");
  ASSERT_EQ(atx::vol::atx_auto_worker_count(), 1u);
  PricerFitter serial{PricerConfig{}};
  const auto t0 = std::chrono::steady_clock::now();
  ASSERT_TRUE(serial.fit(*chain).has_value());
  const auto t1 = std::chrono::steady_clock::now();
  const atx::vol::SurfaceBundle serial_bundle = serial.bundle();

  unset_fit_workers_env();
  PricerFitter parallel{PricerConfig{}};
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
  std::printf(
      "[PricerFitterPolicy SPY] fit_workers=1 %.1fms, fit_workers=0(auto) %.1fms, "
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

  const auto& session = bundle.risk->session();
  EXPECT_TRUE(session.inputs().deam.audit_fit_inversions);
  const auto& diag = session.diagnostics();
  EXPECT_GT(diag.n_iv_proposed, std::size_t{0});
  EXPECT_EQ(diag.n_iv_audited, diag.n_iv_proposed);
  EXPECT_TRUE(diag.inversion_certified);
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
    if (std::fabs(option->T - bad_T) > 1.0e-12) continue;
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
  EXPECT_TRUE(atx::vol::has_validation_failure(
      bundle.risk_health.reasons, atx::vol::ValidationFailure::CarryGap));
  EXPECT_TRUE(bundle.risk_health.serving_candidate());
  EXPECT_FALSE(bundle.risk_health.using_fallback());
  EXPECT_EQ(bundle.risk->session().diagnostics().n_carry_skipped_expiries,
            std::size_t{1});
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
    if (std::fabs(option->T - bad_T) > 1.0e-12) continue;
    ids.push_back(id);
    bids.push_back(2.0);
    asks.push_back(1.0);  // crossed: carry fails for this expiry
  }
  ASSERT_FALSE(ids.empty());
  ASSERT_TRUE(chain_->update_quotes(ids, bids, asks).has_value());

  ASSERT_TRUE(fitter.fit(*chain_).has_value());
  const atx::vol::SurfaceBundle gapped = fitter.bundle();
  ASSERT_NE(gapped.risk, nullptr);
  ASSERT_EQ(gapped.risk_health.state, atx::vol::SurfaceState::Degraded);
  ASSERT_TRUE(atx::vol::has_validation_failure(
      gapped.risk_health.reasons, atx::vol::ValidationFailure::CarryGap));

  // Local refit of a FITTED slice (index 0, untouched expiry) succeeds and
  // publishes a new generation — but the surface still misses the crossed
  // expiry, so health must remain Degraded with CarryGap retained.
  ASSERT_TRUE(fitter.refit_risk_slice(*chain_, 0u).has_value());
  const atx::vol::SurfaceBundle after = fitter.bundle();
  ASSERT_NE(after.risk, nullptr);
  EXPECT_EQ(after.risk->generation(), gapped.risk->generation() + 1u);
  EXPECT_EQ(after.risk_health.state, atx::vol::SurfaceState::Degraded);
  EXPECT_TRUE(atx::vol::has_validation_failure(
      after.risk_health.reasons, atx::vol::ValidationFailure::CarryGap));
  EXPECT_TRUE(after.risk_health.serving_candidate());
  EXPECT_EQ(after.risk->session().diagnostics().n_carry_skipped_expiries,
            std::size_t{1});
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
  diagnostics.carry_confident = true;      // isolate the price-bound path:
  diagnostics.inversion_certified = true;  // no other non-geometric failure
  diagnostics.n_price_bound_violations = 2;

  ValidationDigest digest;  // geometrically clean (failures == None)
  ASSERT_TRUE(digest.admitted());
  merge_session_failure_context(diagnostics, digest);
  finalize_validation_digest(digest);

  EXPECT_TRUE(has_validation_failure(digest.failures, ValidationFailure::PriceBounds));
  // Review finding 3: the merge reports the clamp COUNT, not a bare bit.
  EXPECT_EQ(digest.n_price_bound_violations, 2u);

  const AdmissionDecision decision = decide_risk_surface_admission(
      digest, FitQualityMode::Balanced, 42, 41, SurfaceFallback::LastKnownGood);
  EXPECT_FALSE(decision.publish_candidate);
  EXPECT_TRUE(has_validation_failure(decision.health.reasons,
                                     ValidationFailure::PriceBounds));
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
