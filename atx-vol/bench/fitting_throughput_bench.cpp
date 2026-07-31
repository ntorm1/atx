// fitting_throughput_bench.cpp — first-class fitting/calibration throughput
// (C0.3): calibrations/s directly comparable to the published 16.5k/s
// American-calibration anchor, plus whole-surface eSSVI fit cost and the
// warm-vs-cold single-slice refit ratio.
//
// All four cases build their fixtures (board, curves, observation set, IV
// targets) ONCE, OUTSIDE the timed loop; only the fit call itself is timed,
// cold every iteration (no state carried between iterations beyond what the
// public API itself would carry for a from-scratch caller). The synthetic
// SPY board comes from the same public fixture + recipe as
// corpus_build_bench.cpp's `board_from_spec` (spy_fixture.hpp's
// `make_spy_synthetic_spec` -> `make_synthetic_american_panel` ->
// `MarketEnv::flat`), converted to the `Underlying`/`CurveSet` pair the
// eSSVI surface driver (essvi_calib.hpp) consumes directly.
//
//   fit/surface_cold/spy_synth   — whole-surface eSSVI fit via the public
//                                  `essvi_calib_surface` driver.
//   fit/surface_cold/spy_real    — the SAME `essvi_calib_surface` driver, timed
//                                  against ONE real Databento SPY OPRA slice
//                                  (C0.2: the suite was 100% synthetic before
//                                  this case). Pinned to `kSpyFitFixtures[0]`
//                                  ("selloff-open", SPY_2026-02-12T1435Z) for a
//                                  reproducible timing target; registered only
//                                  when the fixture's parquet is found on this
//                                  machine (spy_fit_fixture.hpp's filesystem
//                                  probe over data/spy_fit_slices, mirrored so
//                                  a source-only checkout skips cleanly instead
//                                  of crashing).
//   fit/slice_cold/spy_synth     — single-slice `essvi_fit_slice`, no seed.
//   fit/slice_warm_refit/spy_synth — same slice, seeded with its own
//                                  converged params (essvi_fit_slice DOES
//                                  expose a public `warm` seam:
//                                  essvi_calib.hpp:110 — no TODO needed).
//   fit/american_iv/{cold,warm}  — `american_implied_vol` over a pre-priced
//                                  strike-ladder x few-expiries batch (the
//                                  16.5k-anchor-comparable case); warm passes
//                                  each target's true vol * (1+1e-3) as the
//                                  Newton seed (the realistic live-refresh
//                                  case).

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <utility>
#include <vector>

#include <benchmark/benchmark.h>

#include "atx/vol/american.hpp"    // AmericanMethod, american_price
#include "atx/vol/american_iv.hpp" // american_implied_vol
#include "atx/vol/calib.hpp" // CalibOpts, FitObs, calib_default_opts, build_observations_european
#include "atx/vol/chain.hpp" // OptionChain
#include "atx/vol/correction.hpp"     // CorrectionCache, AmericanCorrectionCaches
#include "atx/vol/curve.hpp"          // CurveSet, ForwardPoint
#include "atx/vol/data.hpp"           // iso_to_ns
#include "atx/vol/deamer.hpp"         // DeAmOptions
#include "atx/vol/dividend.hpp"       // hybrid_forward, HybridDivParams (real-board forward)
#include "atx/vol/essvi_calib.hpp"    // essvi_fit_slice, essvi_calib_surface
#include "atx/vol/market_env.hpp"     // MarketEnv
#include "atx/vol/panel.hpp"          // make_synthetic_american_panel, SynthPanelSpec
#include "atx/vol/pricer_fitter.hpp"  // PricerFitter, PricerConfig
#include "support/spy_fixture.hpp"    // make_spy_synthetic_spec
#include "atx/vol/surface_policy.hpp" // explicit v2 purpose/admission policy
#include "atx/vol/svi_calib.hpp"      // svi_fit_slice (quasi-explicit raw-SVI)
#include "atx/vol/types.hpp"          // Side
#include "atx/vol/universe.hpp"       // Underlying, Chain
#include "atx/vol/vol_surface.hpp"    // VolSurface, EssviParams, Parametrization

#include "bench_util.hpp"
#include "support/spy_fit_fixture.hpp" // kSpyFitFixtures, find_spy_fit_parquet, load_spy_fit_fixture

namespace atx::vol::bench {
namespace {

// ── Shared SPY board fixture (surface_cold + slice cases) ────────────────
//
// The recipe mirrors corpus_build_bench.cpp's `board_from_spec`: build the
// synthetic panel, wrap it in a flat `MarketEnv`, and install it via
// `OptionChain::from_frame`. The `Underlying` is copied out of the (move-only)
// `OptionChain` so the fixture struct stays a plain, reusable value. The
// `CurveSet` forward points are the panel's OWN known-truth forward per
// expiry (`SynthPanel::truth_forward`), keyed onto the installed chains by
// expiry_ns (both sides compute it via the same `iso_to_ns`); the yield curve
// is the spec's flat rate.
struct SpyBoard {
  Underlying under;
  CurveSet curves;
  double spot{0.0};
  double r{0.0};
};

[[nodiscard]] std::optional<SpyBoard> build_spy_board() {
  const SynthPanelSpec spec = make_spy_synthetic_spec();
  const auto panel = make_synthetic_american_panel(spec);
  if (!panel.has_value()) {
    return std::nullopt;
  }

  const MarketEnv env =
      MarketEnv::flat(spec.spot, spec.r, iso_to_ns(spec.snapshot_iso), spec.cash_divs);
  auto chain_res = OptionChain::from_frame(panel->frame, env);
  if (!chain_res.has_value()) {
    return std::nullopt;
  }

  SpyBoard board;
  board.under = chain_res->underlying();
  board.spot = spec.spot;
  board.r = spec.r;

  board.curves.spot = spec.spot;
  const std::array<double, 2> pillar_t{1.0e-3, 50.0};
  const std::array<double, 2> pillar_r{spec.r, spec.r};
  if (!board.curves.set_yield(pillar_t, pillar_r).has_value()) {
    return std::nullopt;
  }

  std::vector<ForwardPoint> fps;
  fps.reserve(board.under.chains.size());
  for (const Chain &c : board.under.chains) {
    ForwardPoint fp{};
    fp.expiry_ns = c.expiry_ns;
    fp.T = c.T;
    fp.F = spec.spot; // fallback; overwritten below on a truth match
    for (std::size_t i = 0; i < spec.expiries.size(); ++i) {
      if (iso_to_ns(spec.expiries[i].expiry_iso) == c.expiry_ns) {
        fp.F = panel->truth_forward[i];
        break;
      }
    }
    fps.push_back(fp);
  }
  board.curves.forward.set(fps);
  return board;
}

// ── fit/surface_cold/spy_synth ────────────────────────────────────────────

void BM_SurfaceCold(benchmark::State &state) {
  const std::optional<SpyBoard> board = build_spy_board();
  if (!board.has_value()) {
    state.SkipWithError("SPY board build failed");
    return;
  }
  auto surf_res = VolSurface::create(1u, Parametrization::Essvi, board->under.chains.size());
  if (!surf_res.has_value()) {
    state.SkipWithError("VolSurface::create failed");
    return;
  }
  VolSurface surface = *surf_res;
  const CalibOpts opts = calib_default_opts();

  for (auto _ : state) {
    const Status st = essvi_calib_surface(surface, board->under, board->curves, opts);
    if (!st.has_value()) {
      state.SkipWithError("essvi_calib_surface failed");
      break;
    }
    benchmark::DoNotOptimize(surface.n_slices());
    benchmark::ClobberMemory();
  }
  state.SetItemsProcessed(state.iterations()); // items = surfaces
}

// ── fit/surface_cold/spy_real ─────────────────────────────────────────────
//
// The real-OPRA counterpart to BM_SurfaceCold above (C0.2): identical timed
// call (`essvi_calib_surface` on an `Underlying`/`CurveSet` pair built ONCE
// outside the loop), but the board comes from one pinned real Databento SPY
// slice instead of the synthetic panel. Pinned fixture: `kSpyFitFixtures[0]`
// ("selloff-open", SPY_2026-02-12T1435Z.parquet) — chosen simply as the
// first entry for a stable, reproducible timing target; the corpus's other
// nine slices are exercised for ACCURACY (not throughput) by
// spy_fit_corpus_test.cpp.
//
// The forward per expiry is the same hybrid dividend forward
// (`hybrid_forward`, dividend.hpp) production code derives from a real
// board's cash-dividend schedule — there is no "truth forward" for real
// data the way the synthetic panel provides one.

struct SpyBoardReal {
  Underlying under;
  CurveSet curves;
  double spot{0.0};
  double r{0.0};
};

[[nodiscard]] std::optional<SpyBoardReal> build_spy_board_real() {
  const auto &fixture = atx::vol::testkit::kSpyFitFixtures[0];
  const std::optional<atx::vol::testkit::OpraBoard> opra =
      atx::vol::testkit::load_spy_fit_fixture(fixture);
  if (!opra.has_value()) {
    return std::nullopt;
  }
  auto chain_res = OptionChain::from_frame(opra->panel.frame, opra->env());
  if (!chain_res.has_value()) {
    return std::nullopt;
  }

  SpyBoardReal board;
  board.under = chain_res->underlying();
  board.spot = opra->spot();
  board.r = opra->r;

  board.curves.spot = board.spot;
  const std::array<double, 2> pillar_t{1.0e-3, 50.0};
  const std::array<double, 2> pillar_r{board.r, board.r};
  if (!board.curves.set_yield(pillar_t, pillar_r).has_value()) {
    return std::nullopt;
  }

  const HybridDivParams hyb{}; // pure escrowed-cash (blend = 0), matches session.cpp's q_rep recipe
  std::vector<ForwardPoint> fps;
  fps.reserve(board.under.chains.size());
  for (const Chain &c : board.under.chains) {
    ForwardPoint fp{};
    fp.expiry_ns = c.expiry_ns;
    fp.T = c.T;
    const double F = hybrid_forward(board.spot, board.r, 0.0, c.T, opra->panel.frame.divs,
                                    c.expiry_ns, opra->now_ns(), hyb);
    fp.F = (std::isfinite(F) && F > 0.0) ? F : board.spot; // fallback mirrors build_spy_board()
    fps.push_back(fp);
  }
  board.curves.forward.set(fps);
  return board;
}

void BM_SurfaceColdReal(benchmark::State &state) {
  const std::optional<SpyBoardReal> board = build_spy_board_real();
  if (!board.has_value()) {
    state.SkipWithError("real SPY board build failed");
    return;
  }
  auto surf_res = VolSurface::create(1u, Parametrization::Essvi, board->under.chains.size());
  if (!surf_res.has_value()) {
    state.SkipWithError("VolSurface::create failed");
    return;
  }
  VolSurface surface = *surf_res;
  const CalibOpts opts = calib_default_opts();

  for (auto _ : state) {
    const Status st = essvi_calib_surface(surface, board->under, board->curves, opts);
    if (!st.has_value()) {
      state.SkipWithError("essvi_calib_surface (real) failed");
      break;
    }
    benchmark::DoNotOptimize(surface.n_slices());
    benchmark::ClobberMemory();
  }
  state.SetItemsProcessed(state.iterations()); // items = surfaces
}

// ── fit/surface_deam_cold + fit/surface_deam_cached ───────────────────────
//
// The C2.3 evidence: the SAME whole-surface `essvi_calib_surface` fit on the
// SAME synthetic SPY board as BM_SurfaceCold, but routed through the opt-in
// de-Americanization path (`build_observations_european`) that strips the
// American early-exercise premium before the fit. `_cold` inverts each strike
// with a fresh Andersen-Lake solve (empty caches); `_cached` routes through the
// per-side Black-76 + Chebyshev correction caches, which are built ONCE outside
// the timed loop (their build cost — 2 sides x kNK*kNT*kNS cold AL grid solves,
// ~1536/side — is a per-underlier surface-fit-cadence cost, not a per-fit one).
// All three surface cases run at the same default worker count for a like-for-
// like raw-vs-cold-vs-cached comparison.

// Canonical facade benchmarks. Unlike BM_SurfaceColdReal's legacy calibration
// primitive, these rows include PricerFitter policy resolution, surface build,
// admission, and transactional publication. Parquet load + OptionChain install
// happen once before the timed loop.
struct FacadeRealFixture {
  OptionChain chain;
  std::size_t quotes{0u};
};

[[nodiscard]] std::optional<FacadeRealFixture> build_facade_real_fixture() {
  const auto &fixture = atx::vol::testkit::kSpyFitFixtures[0];
  std::optional<atx::vol::testkit::OpraBoard> opra =
      atx::vol::testkit::load_spy_fit_fixture(fixture);
  if (!opra.has_value()) {
    return std::nullopt;
  }
  Result<OptionChain> chain = OptionChain::from_frame(opra->panel.frame, opra->env());
  if (!chain.has_value()) {
    return std::nullopt;
  }
  const std::size_t quotes = chain->size();
  return FacadeRealFixture{std::move(*chain), quotes};
}

// B7 (FT-P): the SYNTHETIC canonical-facade fixture — the SAME PricerFitter::fit
// timed target as the real-OPRA rows, but on the synthetic SPY panel so the
// served pipeline (policy resolution -> surface build -> admission -> publication)
// is benched on EVERY machine, not only where the Databento parquet is present.
[[nodiscard]] std::optional<FacadeRealFixture> build_facade_synth_fixture() {
  const SynthPanelSpec spec = make_spy_synthetic_spec();
  const auto panel = make_synthetic_american_panel(spec);
  if (!panel.has_value()) {
    return std::nullopt;
  }
  const MarketEnv env =
      MarketEnv::flat(spec.spot, spec.r, iso_to_ns(spec.snapshot_iso), spec.cash_divs);
  Result<OptionChain> chain = OptionChain::from_frame(panel->frame, env);
  if (!chain.has_value()) {
    return std::nullopt;
  }
  const std::size_t quotes = chain->size();
  return FacadeRealFixture{std::move(*chain), quotes};
}

[[nodiscard]] PricerConfig hft_mark_config() {
  PricerConfig config;
  config.preset = FitPreset::Hft;
  return config;
}

[[nodiscard]] PricerConfig v2_latency_dual_config() {
  PricerConfig config;
  config.quality_mode = FitQualityMode::Latency;
  config.outputs = SurfaceOutputs::MarketMarkAndRisk;
  config.risk_admission = RiskAdmission::Required;
  config.fallback = SurfaceFallback::None;
  CurveConfig risk_curve;
  risk_curve.kind = VolCurveKind::ConvexDense;
  config.curve = risk_curve;
  return config;
}

[[nodiscard]] bool admitted_hft_mark(const PricerFitter &fitter) noexcept {
  const FittedSurface *surface = fitter.market_mark_surface();
  const SurfaceBundle bundle = fitter.bundle();
  const std::optional<SurfaceBuildReport> &report = fitter.published_report();
  return surface != nullptr && surface->purpose() == SurfacePurpose::MarketMark &&
         surface->session().inputs().curve.kind == VolCurveKind::LinearVariance &&
         bundle.market_mark_health.state == SurfaceState::Healthy &&
         bundle.market_mark_health.serving_candidate() && report.has_value() && report->published &&
         report->published_curve.kind == VolCurveKind::LinearVariance &&
         !report->attempts.empty() && report->attempts.back().admission.admitted;
}

[[nodiscard]] bool admitted_v2_latency_dual(const PricerFitter &fitter) noexcept {
  const SurfaceBundle bundle = fitter.bundle();
  const std::optional<SurfaceBuildReport> &report = fitter.published_report();
  const bool admitted_state = bundle.risk_health.state == SurfaceState::Healthy ||
                              bundle.risk_health.state == SurfaceState::Degraded;
  return bundle.market_mark != nullptr && bundle.risk != nullptr &&
         bundle.market_mark->purpose() == SurfacePurpose::MarketMark &&
         bundle.risk->purpose() == SurfacePurpose::Risk &&
         bundle.market_mark->session().inputs().curve.kind == VolCurveKind::LinearVariance &&
         bundle.risk->session().inputs().curve.kind == VolCurveKind::ConvexDense &&
         bundle.market_mark_health.serving_candidate() && bundle.risk_health.serving_candidate() &&
         admitted_state && !bundle.risk_health.using_fallback() && report.has_value() &&
         report->published && report->published_curve.kind == VolCurveKind::ConvexDense;
}

template <class FixtureBuilder, class ConfigFactory, class AdmissionCheck>
void BM_Facade(benchmark::State &state, FixtureBuilder fixture_builder,
               ConfigFactory config_factory, AdmissionCheck admission_check) {
  std::optional<FacadeRealFixture> fixture = fixture_builder();
  if (!fixture.has_value()) {
    state.SkipWithError("SPY facade fixture build failed");
    return;
  }

  std::size_t slices = 0u;
  bool degraded = false;
  // B7 (FT-P): per-phase FitTimings so the next optimization round is gated on the
  // pipeline the canonical facade actually runs (market-mark build / risk build /
  // risk validation / total), not the alternate essvi_calib_surface driver.
  FitPhaseTimings timing_sums{};
  std::uint64_t admitted_iterations = 0u;
  for (auto _ : state) {
    // The benchmark's item is one `fit`, not facade construction, admission/report
    // inspection, or bundle access. Keep those necessary controls outside the timed
    // region so a setup/report change cannot masquerade as a fitter regression.
    state.PauseTiming();
    PricerFitter fitter{config_factory()};
    state.ResumeTiming();
    const Status fitted = fitter.fit(fixture->chain);
    state.PauseTiming();
    const bool admitted = fitted.has_value() && admission_check(fitter);
    if (!admitted) {
      state.ResumeTiming();
      state.SkipWithError("SPY facade fit was not admitted as configured");
      break;
    }
    const FittedSurface *served = fitter.surface();
    slices = served != nullptr ? served->session().expiries().size() : 0u;
    degraded = fitter.bundle().risk_health.state == SurfaceState::Degraded;
    const FitPhaseTimings timings = fitter.bundle().timings;
    timing_sums.market_mark_build_ms += timings.market_mark_build_ms;
    timing_sums.risk_build_ms += timings.risk_build_ms;
    timing_sums.risk_validation_ms += timings.risk_validation_ms;
    timing_sums.total_ms += timings.total_ms;
    ++admitted_iterations;
    benchmark::DoNotOptimize(slices);
    benchmark::ClobberMemory();
    state.ResumeTiming();
  }

  const double iterations = static_cast<double>(state.iterations());
  const double admitted_count = static_cast<double>(admitted_iterations);
  const double timing_denominator = admitted_count > 0.0 ? admitted_count : 1.0;
  const double quotes = static_cast<double>(fixture->quotes);
  state.SetItemsProcessed(state.iterations()); // one admitted facade fit per item
  state.counters["quotes"] = quotes;
  state.counters["slices"] = static_cast<double>(slices);
  state.counters["admitted"] = slices > 0u ? 1.0 : 0.0;
  state.counters["degraded"] = degraded ? 1.0 : 0.0;
  state.counters["quotes_per_s"] =
      benchmark::Counter(iterations * quotes, benchmark::Counter::kIsRate);
  state.counters["slice_fits_per_s"] =
      benchmark::Counter(iterations * static_cast<double>(slices), benchmark::Counter::kIsRate);
  // Mean per-phase wall across every admitted fit, rather than an arbitrary last
  // iteration that may not represent the benchmark's reported aggregate timing.
  state.counters["phase_market_mark_ms"] =
      timing_sums.market_mark_build_ms / timing_denominator;
  state.counters["phase_risk_build_ms"] = timing_sums.risk_build_ms / timing_denominator;
  state.counters["phase_risk_validation_ms"] =
      timing_sums.risk_validation_ms / timing_denominator;
  state.counters["phase_total_ms"] = timing_sums.total_ms / timing_denominator;
  state.counters["phase_samples"] = admitted_count;
}

void BM_FacadeHftMarkSynth(benchmark::State &state) {
  BM_Facade(state, build_facade_synth_fixture, hft_mark_config, admitted_hft_mark);
}

void BM_FacadeHftMarkReal(benchmark::State &state) {
  BM_Facade(state, build_facade_real_fixture, hft_mark_config, admitted_hft_mark);
}

void BM_FacadeV2LatencyDualReal(benchmark::State &state) {
  BM_Facade(state, build_facade_real_fixture, v2_latency_dual_config, admitted_v2_latency_dual);
}

struct BoardCaches {
  CorrectionCache call;
  CorrectionCache put;
};

// Per-side American-minus-European correction caches covering the board, built
// the way session.cpp's `build_session_caches` does (padded (k_log, T) box from
// every strike/expiry with spot as the forward proxy, a representative carry
// from the mid expiry's forward, a generous sigma box).
[[nodiscard]] BoardCaches build_board_caches(const SpyBoard &board) {
  const double S = board.spot;
  double k_min = std::numeric_limits<double>::infinity();
  double k_max = -std::numeric_limits<double>::infinity();
  double T_lo = std::numeric_limits<double>::infinity();
  double T_hi = -std::numeric_limits<double>::infinity();
  for (const Chain &c : board.under.chains) {
    if (!(c.T > 0.0)) {
      continue;
    }
    T_lo = std::min(T_lo, c.T);
    T_hi = std::max(T_hi, c.T);
    for (const double K : c.strikes) {
      const double k = std::log(K / S);
      k_min = std::min(k_min, k);
      k_max = std::max(k_max, k);
    }
  }
  k_min -= 0.05;
  k_max += 0.05;
  const double T_min = 0.9 * T_lo;
  const double T_max = (T_hi > T_lo) ? (1.1 * T_hi) : (1.5 * T_lo);
  constexpr double kSigMin = 0.05;
  constexpr double kSigMax = 1.5;

  const Chain &mid = board.under.chains[board.under.chains.size() / 2];
  const double F_mid = board.curves.forward.forward_at(mid.expiry_id);
  double q_rep = board.r;
  if (mid.T > 0.0 && F_mid > 0.0) {
    q_rep = board.r - std::log(F_mid / S) / mid.T;
  }

  constexpr std::uint16_t kNK = 16;
  constexpr std::uint16_t kNT = 8;
  constexpr std::uint16_t kNS = 12;
  BoardCaches bc;
  if (auto cc = CorrectionCache::build(kNK, kNT, kNS, board.r, q_rep, k_min, k_max, T_min, T_max,
                                       kSigMin, kSigMax, Side::Call)) {
    bc.call = std::move(*cc);
  }
  if (auto pp = CorrectionCache::build(kNK, kNT, kNS, board.r, q_rep, k_min, k_max, T_min, T_max,
                                       kSigMin, kSigMax, Side::Put)) {
    bc.put = std::move(*pp);
  }
  return bc;
}

void BM_SurfaceDeAmCold(benchmark::State &state) {
  const std::optional<SpyBoard> board = build_spy_board();
  if (!board.has_value()) {
    state.SkipWithError("SPY board build failed");
    return;
  }
  auto surf_res = VolSurface::create(1u, Parametrization::Essvi, board->under.chains.size());
  if (!surf_res.has_value()) {
    state.SkipWithError("VolSurface::create failed");
    return;
  }
  VolSurface surface = *surf_res;
  const CalibOpts opts = calib_default_opts();
  const DeAmOptions deam{}; // empty caches => cold Andersen-Lake per strike

  for (auto _ : state) {
    const Status st = essvi_calib_surface(surface, board->under, board->curves, opts,
                                          /*out_diag=*/nullptr, /*prior=*/nullptr,
                                          /*n_workers=*/0u, &deam);
    if (!st.has_value()) {
      state.SkipWithError("essvi_calib_surface (deam cold) failed");
      break;
    }
    benchmark::DoNotOptimize(surface.n_slices());
    benchmark::ClobberMemory();
  }
  state.SetItemsProcessed(state.iterations());
}

void BM_SurfaceDeAmCached(benchmark::State &state) {
  const std::optional<SpyBoard> board = build_spy_board();
  if (!board.has_value()) {
    state.SkipWithError("SPY board build failed");
    return;
  }
  auto surf_res = VolSurface::create(1u, Parametrization::Essvi, board->under.chains.size());
  if (!surf_res.has_value()) {
    state.SkipWithError("VolSurface::create failed");
    return;
  }
  VolSurface surface = *surf_res;
  const CalibOpts opts = calib_default_opts();

  // Caches built ONCE, outside the timed loop (surface-fit-cadence cost).
  const BoardCaches caches = build_board_caches(*board);
  DeAmOptions deam{};
  deam.caches = AmericanCorrectionCaches{&caches.call, &caches.put};

  for (auto _ : state) {
    const Status st = essvi_calib_surface(surface, board->under, board->curves, opts,
                                          /*out_diag=*/nullptr, /*prior=*/nullptr,
                                          /*n_workers=*/0u, &deam);
    if (!st.has_value()) {
      state.SkipWithError("essvi_calib_surface (deam cached) failed");
      break;
    }
    benchmark::DoNotOptimize(surface.n_slices());
    benchmark::ClobberMemory();
  }
  state.SetItemsProcessed(state.iterations());
}

// ── fit/slice_cold + fit/slice_warm_refit ─────────────────────────────────
//
// One slice's observation set, de-Americanized via the public
// `build_observations_european` (the realistic single-slice tick-refresh
// input), built ONCE. `essvi_fit_slice` (essvi_calib.hpp:107-110) exposes a
// public `warm` seed parameter, so both a cold and a self-seeded warm refit
// are registered here (no C1.6 TODO needed — the seam already exists).

struct SliceFixture {
  std::vector<FitObs> obs;
  double T{0.0};
  double F{0.0};
  CalibOpts opts{};
};

[[nodiscard]] std::optional<SliceFixture> build_slice_fixture() {
  const std::optional<SpyBoard> board = build_spy_board();
  if (!board.has_value() || board->under.chains.empty()) {
    return std::nullopt;
  }
  const std::vector<Chain> &chains = board->under.chains;
  const std::size_t idx = std::min<std::size_t>(2u, chains.size() - 1u); // ~2m tenor
  const Chain &c = chains[idx];

  SliceFixture fx;
  fx.T = c.T;
  fx.F = board->curves.forward.forward_at(c.expiry_id);
  const double df = board->curves.yield.disc(fx.T);
  fx.opts = calib_default_opts();

  auto obs_res = build_observations_european(c, board->spot, board->r, fx.F, fx.T, df, fx.opts);
  if (!obs_res.has_value() || obs_res->obs.size() < fx.opts.min_obs_per_slice) {
    return std::nullopt;
  }
  fx.obs = std::move(obs_res->obs);
  return fx;
}

void BM_SliceCold(benchmark::State &state) {
  const std::optional<SliceFixture> fx = build_slice_fixture();
  if (!fx.has_value()) {
    state.SkipWithError("slice fixture build failed");
    return;
  }
  for (auto _ : state) {
    const auto fit = essvi_fit_slice(fx->obs, fx->T, fx->F, fx->opts);
    if (!fit.has_value()) {
      state.SkipWithError("essvi_fit_slice (cold) failed");
      break;
    }
    double theta = fit->theta;
    benchmark::DoNotOptimize(theta);
    benchmark::ClobberMemory();
  }
  state.SetItemsProcessed(state.iterations()); // items = slice fits
}

void BM_SliceWarmRefit(benchmark::State &state) {
  const std::optional<SliceFixture> fx = build_slice_fixture();
  if (!fx.has_value()) {
    state.SkipWithError("slice fixture build failed");
    return;
  }
  const auto prior = essvi_fit_slice(fx->obs, fx->T, fx->F, fx->opts);
  if (!prior.has_value()) {
    state.SkipWithError("prior (seed) slice fit failed");
    return;
  }
  const EssviParams warm_seed = *prior;

  for (auto _ : state) {
    const auto fit = essvi_fit_slice(fx->obs, fx->T, fx->F, fx->opts, nullptr, 0.0, &warm_seed);
    if (!fit.has_value()) {
      state.SkipWithError("essvi_fit_slice (warm) failed");
      break;
    }
    double theta = fit->theta;
    benchmark::DoNotOptimize(theta);
    benchmark::ClobberMemory();
  }
  state.SetItemsProcessed(state.iterations()); // items = slice fits
}

// ── fit/svi_slice_cold/spy_synth ──────────────────────────────────────────
//
// The quasi-explicit raw-SVI single-slice fit (C2.2), on the SAME de-Am'd
// synthetic-SPY slice fixture BM_SliceCold uses, at Trading-level opts
// (calib_default_opts()'s default optimization_level). Times `svi_fit_slice`
// cold every iteration (the raw-SVI fitter had NO bench coverage before C2.2).
// This is the fit-level evidence for the vectorized (u, v) basis kernel + the
// cached inner solve.

void BM_SviSliceCold(benchmark::State &state) {
  const std::optional<SliceFixture> fx = build_slice_fixture();
  if (!fx.has_value()) {
    state.SkipWithError("slice fixture build failed");
    return;
  }
  for (auto _ : state) {
    const auto fit = svi_fit_slice(fx->obs, fx->T, fx->F, fx->opts);
    if (!fit.has_value()) {
      state.SkipWithError("svi_fit_slice (cold) failed");
      break;
    }
    double b = fit->b;
    benchmark::DoNotOptimize(b);
    benchmark::ClobberMemory();
  }
  state.SetItemsProcessed(state.iterations()); // items = slice fits
}

// ── fit/american_iv/{cold,warm} ───────────────────────────────────────────
//
// The 16.5k-anchor-comparable case: a realistic strike-ladder x few-expiries
// batch (25 strikes x 2 sides x 4 expiries = 200 inversions/iteration),
// priced with a KNOWN vol so the inversion is exact-recoverable. Prices are
// generated ONCE outside the loop with `american_price`; only
// `american_implied_vol` is timed.

struct IvTarget {
  double S{0.0};
  double K{0.0};
  double T{0.0};
  double r{0.0};
  double q{0.0};
  Side side{Side::Call};
  double true_vol{0.0};
  double price{0.0};
};

[[nodiscard]] std::vector<IvTarget> build_iv_targets() {
  struct Expiry {
    double T;
    double vol;
  };
  // Tenors + ATM vols representative of the SPY synthetic fixture's front-end
  // term structure (spy_fixture.hpp), independent of its exact calendar.
  const Expiry expiries[] = {
      {1.0 / 12.0, 0.108},
      {2.0 / 12.0, 0.118},
      {3.0 / 12.0, 0.128},
      {6.0 / 12.0, 0.145},
  };
  constexpr double kSpot = 600.0;
  constexpr double kRate = 0.043;
  constexpr double kBorrow = 0.0;

  std::vector<IvTarget> targets;
  targets.reserve(4u * 25u * 2u);
  for (const Expiry &e : expiries) {
    for (double K = 540.0; K <= 660.0 + 1.0e-9; K += 5.0) {
      for (const Side side : {Side::Call, Side::Put}) {
        const auto price = american_price(kSpot, K, e.T, e.vol, kRate, kBorrow, side);
        if (!price.has_value()) {
          continue; // skip an unpriceable corner rather than corrupt the batch
        }
        IvTarget t;
        t.S = kSpot;
        t.K = K;
        t.T = e.T;
        t.r = kRate;
        t.q = kBorrow;
        t.side = side;
        t.true_vol = e.vol;
        t.price = *price;
        targets.push_back(t);
      }
    }
  }
  return targets;
}

void BM_AmericanIvCold(benchmark::State &state) {
  const std::vector<IvTarget> targets = build_iv_targets();
  if (targets.empty()) {
    state.SkipWithError("IV target batch build failed");
    return;
  }
  for (auto _ : state) {
    double sink = 0.0;
    for (const IvTarget &t : targets) {
      const auto iv = american_implied_vol(t.price, t.S, t.K, t.T, t.r, t.q, t.side);
      sink += iv.has_value() ? *iv : 0.0;
    }
    benchmark::DoNotOptimize(sink);
    benchmark::ClobberMemory();
  }
  state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(targets.size()));
}

void BM_AmericanIvWarm(benchmark::State &state) {
  const std::vector<IvTarget> targets = build_iv_targets();
  if (targets.empty()) {
    state.SkipWithError("IV target batch build failed");
    return;
  }
  for (auto _ : state) {
    double sink = 0.0;
    for (const IvTarget &t : targets) {
      const double warm_start = t.true_vol * (1.0 + 1.0e-3); // realistic live-refresh seed
      const auto iv = american_implied_vol(t.price, t.S, t.K, t.T, t.r, t.q, t.side,
                                           AmericanMethod::AndersenLake, 1.0e-7, 64, std::nullopt,
                                           nullptr, warm_start);
      sink += iv.has_value() ? *iv : 0.0;
    }
    benchmark::DoNotOptimize(sink);
    benchmark::ClobberMemory();
  }
  state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(targets.size()));
}

struct SharedDeamFixture {
  Chain chain;
  double S{100.0};
  double T{1.0};
  double r{0.05};
  double q{0.02};
  double F{0.0};
  double df{0.0};
};

// Per-strike quoting vol for the shared-boundary A/B fixture.
//
// `smile == false` is the original board: FLAT 0.24 across every strike. Its
// sigma-box is [0.35 * 0.24, 0.24] = [0.084, 0.24] — only ~2.9x wide and
// trivially interpolable by nine Chebyshev nodes, so that arm measures the route's
// MECHANICS (lane iteration, build amortisation) and not interpolation stress.
//
// `smile == true` sweeps sigma monotonically across [0.15, 0.8] in strike — a
// steep equity put-skew. The box becomes [0.0525, 0.8], ~15x wide (just inside the
// route's own <= 20x admission cap), which is the widest span the shared
// interpolant will ever be asked to serve. This is the arm that actually stresses
// the nine-node interpolation, and it is the same fixture shape the sprint's
// smile-stress accuracy evidence was measured on.
[[nodiscard]] double shared_deam_fixture_sigma(bool smile, std::size_t strike,
                                               std::size_t n_strikes) {
  if (!smile || n_strikes < 2u) {
    return 0.24;
  }
  const double t = static_cast<double>(strike) / static_cast<double>(n_strikes - 1u);
  return 0.80 + (0.15 - 0.80) * t;
}

[[nodiscard]] std::optional<SharedDeamFixture> build_shared_deam_fixture(bool smile) {
  SharedDeamFixture fixture;
  fixture.F = fixture.S * std::exp((fixture.r - fixture.q) * fixture.T);
  fixture.df = std::exp(-fixture.r * fixture.T);
  fixture.chain.uid = 9001u;
  fixture.chain.expiry_id = 1u;
  fixture.chain.T = fixture.T;
  fixture.chain.strikes.reserve(96u);
  for (std::size_t index = 0u; index < 96u; ++index) {
    fixture.chain.strikes.push_back(72.0 + 56.0 * static_cast<double>(index) / 95.0);
  }
  const std::size_t quote_count = 2u * fixture.chain.strikes.size();
  fixture.chain.bids.assign(quote_count, 0.0);
  fixture.chain.asks.assign(quote_count, 0.0);
  fixture.chain.mids.assign(quote_count, 0.0);
  fixture.chain.ivs.assign(quote_count, std::numeric_limits<double>::quiet_NaN());
  fixture.chain.bid_sizes.assign(quote_count, 10u);
  fixture.chain.ask_sizes.assign(quote_count, 10u);
  fixture.chain.ts_ns.assign(quote_count, 0);
  fixture.chain.flags.assign(quote_count, 0u);
  for (std::size_t strike = 0u; strike < fixture.chain.strikes.size(); ++strike) {
    const double sigma =
        shared_deam_fixture_sigma(smile, strike, fixture.chain.strikes.size());
    for (const Side side : {Side::Call, Side::Put}) {
      const Result<double> price =
          american_price(fixture.S, fixture.chain.strikes[strike], fixture.T, sigma, fixture.r,
                         fixture.q, side, AmericanMethod::AndersenLake, std::nullopt);
      if (!price || !(*price > 0.0)) {
        return std::nullopt;
      }
      const std::size_t quote = chain_index(static_cast<std::uint16_t>(strike), side);
      const double half_spread = std::min(0.01, 0.10 * *price);
      fixture.chain.mids[quote] = *price;
      fixture.chain.bids[quote] = *price - half_spread;
      fixture.chain.asks[quote] = *price + half_spread;
    }
  }
  return fixture;
}

// range(0): 0 = scalar reference arm, 1 = shared-boundary arm.
// range(1): 0 = flat 0.24 board, 1 = steep [0.15, 0.8] smile board.
void BM_SharedBoundaryDeam(benchmark::State &state) {
  const std::optional<SharedDeamFixture> fixture =
      build_shared_deam_fixture(/*smile=*/state.range(1) != 0);
  if (!fixture) {
    state.SkipWithError("shared-boundary de-Am fixture build failed");
    return;
  }
  CalibOpts opts = calib_default_opts();
  opts.max_spread_vol = 1.0;
  opts.min_vega_weight = 0.0;
  opts.use_shared_boundary_deam = state.range(0) != 0;
  std::uint32_t shared_lanes = 0u;
  std::uint32_t boundary_nodes = 0u;
  std::uint32_t scalar_fallbacks = 0u;
  for (auto _ : state) {
    const Result<ObsSet> observations = build_observations_european(
        fixture->chain, fixture->S, fixture->r, fixture->F, fixture->T, fixture->df, opts, {},
        std::nullopt, 1.0e-7, 64, AmericanMethod::AndersenLake, false);
    if (!observations) {
      state.SkipWithError("shared-boundary de-Am observation build failed");
      break;
    }
    shared_lanes = observations->deam_audit.n_shared_boundary_lanes;
    boundary_nodes = observations->deam_audit.n_shared_boundary_solves;
    scalar_fallbacks = observations->deam_audit.n_shared_scalar_fallback_lanes;
    benchmark::DoNotOptimize(observations->obs.data());
    benchmark::ClobberMemory();
  }
  state.counters["shared_lanes"] = static_cast<double>(shared_lanes);
  state.counters["boundary_nodes"] = static_cast<double>(boundary_nodes);
  state.counters["scalar_fallbacks"] = static_cast<double>(scalar_fallbacks);
  state.SetItemsProcessed(state.iterations() *
                          static_cast<std::int64_t>(fixture->chain.n_strikes()));
}

const int kRegistered = [] {
  // B7 (FT-P): the CANONICAL fitting-throughput row is the served facade
  // (PricerFitter::fit: policy resolution -> surface build -> admission ->
  // publication), benched on the synthetic SPY board so it runs on EVERY machine
  // and emits per-phase FitTimings counters. The next fit-perf optimization round
  // gates on THIS row, not the alternate essvi_calib_surface driver below.
  apply_common(benchmark::RegisterBenchmark("fit/facade/hft_mark/spy_synth", BM_FacadeHftMarkSynth))
      ->Unit(benchmark::kMillisecond)
      ->UseRealTime();
  // DEMOTED (FT-P / F-10): fit/surface_cold times the ALTERNATE eSSVI driver
  // `essvi_calib_surface`, which NO production path calls (the served path is the
  // facade above -> VolaSession -> run_surface_parity). Kept as a low-level
  // per-driver reference / regression guard only; do NOT gate fit-perf work on it.
  apply_common(benchmark::RegisterBenchmark("fit/surface_cold_altdriver/spy_synth", BM_SurfaceCold))
      ->Unit(benchmark::kMicrosecond);
  // Register the real-OPRA case only when the pinned fixture's parquet is
  // actually present (source-only checkouts / CI without data/ skip cleanly
  // rather than SkipWithError-ing a benchmark that was never meant to run).
  // Each iteration is a whole cold surface fit on a real board (measured
  // ~25-60ms in Release on this fixture — faster than a first estimate of
  // ~0.3-0.5s, since kSpyFitFixtures[0]'s board is smaller than assumed) —
  // MinTime(2s) so Repetitions(5) still lands dozens of iterations per
  // repetition instead of just a handful.
  if (!atx::vol::testkit::find_spy_fit_parquet(atx::vol::testkit::kSpyFitFixtures[0]).empty()) {
    // DEMOTED (FT-P): alternate-driver reference on a real board — see the
    // fit/surface_cold_altdriver/spy_synth note. The canonical real-board row is
    // fit/facade/hft_mark/spy_real just below.
    apply_common(
        benchmark::RegisterBenchmark("fit/surface_cold_altdriver/spy_real", BM_SurfaceColdReal))
        ->Unit(benchmark::kMicrosecond)
        ->MinTime(2.0);
    apply_common(benchmark::RegisterBenchmark("fit/facade/hft_mark/spy_real", BM_FacadeHftMarkReal))
        ->Unit(benchmark::kMillisecond)
        ->UseRealTime();
    apply_common(benchmark::RegisterBenchmark("fit/facade/v2_latency_dual_convex/spy_real",
                                              BM_FacadeV2LatencyDualReal))
        ->Unit(benchmark::kMillisecond)
        ->UseRealTime();
  }
  // C2.3 de-Am surface cases: same board + driver as fit/surface_cold, routed
  // through the opt-in de-Americanization path (cold Andersen-Lake vs cached
  // Black-76 + Chebyshev correction). Directly comparable to fit/surface_cold.
  apply_common(benchmark::RegisterBenchmark("fit/surface_deam_cold/spy_synth", BM_SurfaceDeAmCold))
      ->Unit(benchmark::kMicrosecond);
  apply_common(
      benchmark::RegisterBenchmark("fit/surface_deam_cached/spy_synth", BM_SurfaceDeAmCached))
      ->Unit(benchmark::kMicrosecond);
  apply_common(benchmark::RegisterBenchmark("fit/slice_cold/spy_synth", BM_SliceCold))
      ->Unit(benchmark::kMicrosecond);
  apply_common(benchmark::RegisterBenchmark("fit/slice_warm_refit/spy_synth", BM_SliceWarmRefit))
      ->Unit(benchmark::kMicrosecond);
  apply_common(benchmark::RegisterBenchmark("fit/svi_slice_cold/spy_synth", BM_SviSliceCold))
      ->Unit(benchmark::kMicrosecond);
  apply_common(benchmark::RegisterBenchmark("fit/american_iv/cold", BM_AmericanIvCold))
      ->Unit(benchmark::kMicrosecond);
  apply_common(benchmark::RegisterBenchmark("fit/american_iv/warm", BM_AmericanIvWarm))
      ->Unit(benchmark::kMicrosecond);
  // Flat 0.24 board: measures route mechanics on a trivially-interpolable box.
  apply_common(benchmark::RegisterBenchmark("fit/deam_shared_boundary/scalar_reference",
                                            BM_SharedBoundaryDeam))
      ->Args({0, 0})
      ->Unit(benchmark::kMicrosecond);
  apply_common(
      benchmark::RegisterBenchmark("fit/deam_shared_boundary/retained", BM_SharedBoundaryDeam))
      ->Args({1, 0})
      ->Unit(benchmark::kMicrosecond);
  // Steep [0.15, 0.8] smile board: the ~15x-wide sigma-box that actually stresses
  // the nine-node interpolation. Same A/B, so the two boards are comparable.
  apply_common(benchmark::RegisterBenchmark("fit/deam_shared_boundary_smile/scalar_reference",
                                            BM_SharedBoundaryDeam))
      ->Args({0, 1})
      ->Unit(benchmark::kMicrosecond);
  apply_common(benchmark::RegisterBenchmark("fit/deam_shared_boundary_smile/retained",
                                            BM_SharedBoundaryDeam))
      ->Args({1, 1})
      ->Unit(benchmark::kMicrosecond);
  return 0;
}();

} // namespace
} // namespace atx::vol::bench
