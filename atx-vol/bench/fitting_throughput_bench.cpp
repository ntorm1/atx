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
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include <benchmark/benchmark.h>

#include "atx/vol/american.hpp"      // AmericanMethod, american_price
#include "atx/vol/american_iv.hpp"   // american_implied_vol
#include "atx/vol/calib.hpp"         // CalibOpts, FitObs, calib_default_opts, build_observations_european
#include "atx/vol/chain.hpp"         // OptionChain
#include "atx/vol/curve.hpp"         // CurveSet, ForwardPoint
#include "atx/vol/data.hpp"          // iso_to_ns
#include "atx/vol/essvi_calib.hpp"   // essvi_fit_slice, essvi_calib_surface
#include "atx/vol/market_env.hpp"    // MarketEnv
#include "atx/vol/panel.hpp"         // make_synthetic_american_panel, SynthPanelSpec
#include "atx/vol/spy_fixture.hpp"   // make_spy_synthetic_spec
#include "atx/vol/types.hpp"         // Side
#include "atx/vol/universe.hpp"      // Underlying, Chain
#include "atx/vol/vol_surface.hpp"   // VolSurface, EssviParams, Parametrization

#include "bench_util.hpp"

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
  auto surf_res =
      VolSurface::create(1u, Parametrization::Essvi, board->under.chains.size());
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

  auto obs_res =
      build_observations_european(c, board->spot, board->r, fx.F, fx.T, df, fx.opts);
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
    const auto fit =
        essvi_fit_slice(fx->obs, fx->T, fx->F, fx->opts, nullptr, 0.0, &warm_seed);
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
        const auto price =
            american_price(kSpot, K, e.T, e.vol, kRate, kBorrow, side);
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
  state.SetItemsProcessed(state.iterations() *
                          static_cast<std::int64_t>(targets.size()));
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
                                            AmericanMethod::AndersenLake, 1.0e-7, 64,
                                            std::nullopt, nullptr, warm_start);
      sink += iv.has_value() ? *iv : 0.0;
    }
    benchmark::DoNotOptimize(sink);
    benchmark::ClobberMemory();
  }
  state.SetItemsProcessed(state.iterations() *
                          static_cast<std::int64_t>(targets.size()));
}

const int kRegistered = [] {
  apply_common(benchmark::RegisterBenchmark("fit/surface_cold/spy_synth", BM_SurfaceCold))
      ->Unit(benchmark::kMicrosecond);
  apply_common(benchmark::RegisterBenchmark("fit/slice_cold/spy_synth", BM_SliceCold))
      ->Unit(benchmark::kMicrosecond);
  apply_common(
      benchmark::RegisterBenchmark("fit/slice_warm_refit/spy_synth", BM_SliceWarmRefit))
      ->Unit(benchmark::kMicrosecond);
  apply_common(benchmark::RegisterBenchmark("fit/american_iv/cold", BM_AmericanIvCold))
      ->Unit(benchmark::kMicrosecond);
  apply_common(benchmark::RegisterBenchmark("fit/american_iv/warm", BM_AmericanIvWarm))
      ->Unit(benchmark::kMicrosecond);
  return 0;
}();

} // namespace
} // namespace atx::vol::bench
