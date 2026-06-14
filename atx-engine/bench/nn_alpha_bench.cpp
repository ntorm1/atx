// nn_alpha_bench.cpp — p2 S5 NN-alpha fit / predict micro-benchmarks (S5-5b).
//
// =====================================================================
//  What is measured
// =====================================================================
//  The S5 learned-SEQUENCE-alpha surface, under the Google Benchmark harness:
//
//    * FIT throughput per architecture — fit_tcn / fit_gru / fit_attn (the
//      predictive track) and fit_autoencoder_factors (the GKX factor track). Each
//      runs the full deflation-gated CPCV walk + the deployed seed-ensemble refit
//      over a synthetic SequenceTensor fixture (built ONCE per case, outside the
//      timed region). This is the heavy path (it trains real nets), so each fit
//      case is pinned to ->Iterations(1) and reported in milliseconds.
//
//    * SEED-ENSEMBLE cost — one fit case at ensemble_size = 1 vs ensemble_size = 5
//      (same arch / size), so the linear ×ensemble_size training cost is visible.
//
//    * PREDICT throughput — predict_nn (per call, on a PRE-FIT model + a fixed L*F
//      window) and SeqLearnedSignalSource::evaluate (the LIVE cross-section path on
//      a pre-built RollingPanel/PanelView). Both pre-fit the model OUTSIDE the timed
//      loop; only the predict is timed. This is the train-vs-PREDICT split the plan
//      calls out (predict must be cheap relative to fit).
//
// =====================================================================
//  The size subset + bounded-runtime rationale
// =====================================================================
//  The plan's full fit grid is (arch ∈ {tcn,gru,attn,ae}, L ∈ {20,40,60},
//  N universe ∈ {500,1000,3000}, channels ∈ {16,24,32}) — 4×3×3×3 = 108 cells,
//  each TRAINING a seed-ensemble across every CPCV fold. A single fit at the plan's
//  full scale (N=500, L=20, 6 epochs) measured >150 s in a Debug / clang-cl build —
//  a forward+backward through a real net, per minibatch, per epoch, per CPCV fold,
//  per ensemble member is ~1000× a linear/GBT fit, so the full grid is hours.
//  This bench takes a BOUNDED, DOWN-SCALED diagonal subset so the whole thing runs
//  in well under a minute:
//    - L (lookback) ∈ {16, 32}  (the plan's {20,40} rounded to <= the live ring cap
//                                64; drop 60 — the longest window dominates cost);
//    - N samples are SHAPED by the synthetic generator: N ∈ {24, 48} VALID samples
//      (NOT the plan's 500/1000/3000 — a Debug NN train is ~1000× a linear fit, and
//      measured ~7 s/fit even at N=48/ch=16, so sample count is slashed ~20× to keep
//      each fit sub-second);
//    - channels ∈ {16, 32}      (the plan endpoints; drop 24 — they bracket cost.
//      channels is the COSTLIEST axis (TCN conv ~channels²; ch=32 was ~3× ch=16), so
//      only ONE ch=32 demonstrator runs per arch, off the smallest (L,N) corner);
//    - epochs = 3, batch_size 32, CPCV 2 folds (C(2,1)), ensemble_size = 1 for the
//      per-arch grid (tiny budget — the point is the SURFACE cost SHAPE, not
//      convergence); the ONE ensemble case sweeps ensemble_size ∈ {1, 5} (×5).
//  The grid is a SUBSET (not the full cross-product): each arch runs 3-4 (L,N,ch)
//  rows that vary ONE axis at a time off a common {16,24,16} corner, so the per-axis
//  cost slope is visible without paying for the 8-cell cross-product per arch.
//  Fits are ->Iterations(1) (one train per reported row); predict cases run the
//  default iteration count (they are microsecond-scale). All RNG is fixed-seeded,
//  so the fixtures + fits are run-to-run deterministic.
//
//  Build is Debug / clang-cl (the project default), so every number here is an
//  UPPER-BOUND latency; host/build context is recorded by Google Benchmark's own
//  header. This bench exists to COMPILE+LINK+RUN the S5 NN fit/predict surface
//  under the bench harness; it is NOT pinned to a perf target (mirrors cost_bench).

#include <span>   // std::span (predict window / signal view)
#include <string> // std::string (raw field names)
#include <vector> // std::vector (fixture storage)

#include <benchmark/benchmark.h>

#include "atx/core/domain/domain.hpp" // Bar, Price, Quantity, Symbol
#include "atx/core/datetime.hpp"      // Timestamp
#include "atx/core/decimal.hpp"       // Decimal (Price/Quantity round-trip)
#include "atx/core/random.hpp"        // Xoshiro256pp (synthetic fixture draws)
#include "atx/core/types.hpp"         // f64, i64, u8, u16, u32, u64, usize

#include "atx/engine/learn/autoencoder_alpha.hpp" // fit_autoencoder_factors, AeFactorCfg
#include "atx/engine/learn/learned_source.hpp"    // LearnedModel, predict_nn
#include "atx/engine/learn/nn_source.hpp"         // SeqLearnedSignalSource
#include "atx/engine/learn/sequence_features.hpp" // SequenceTensor
#include "atx/engine/learn/tcn_alpha.hpp"         // fit_tcn/gru/attn + cfgs
#include "atx/engine/loop/panel_types.hpp"        // SliceRow, MarketSlice
#include "atx/engine/loop/rolling_panel.hpp"      // RollingPanel
#include "atx/engine/loop/signal_source.hpp"      // SignalView
#include "atx/engine/loop/types.hpp"              // InstrumentId

namespace {

using atx::f64;
using atx::i64;
using atx::u16;
using atx::u32;
using atx::u64;
using atx::u8;
using atx::usize;
using atx::core::Decimal;
using atx::core::domain::Bar;
using atx::core::domain::Price;
using atx::core::domain::Quantity;
using atx::core::domain::Symbol;
using atx::core::time::Timestamp;
using atx::engine::InstrumentId;
using atx::engine::MarketSlice;
using atx::engine::SignalView;
using atx::engine::SliceRow;
using atx::engine::learn::AeFactorCfg;
using atx::engine::learn::AttnAlphaCfg;
using atx::engine::learn::fit_attn;
using atx::engine::learn::fit_autoencoder_factors;
using atx::engine::learn::fit_gru;
using atx::engine::learn::fit_tcn;
using atx::engine::learn::GruAlphaCfg;
using atx::engine::learn::LearnedModel;
using atx::engine::learn::predict_nn;
using atx::engine::learn::SeqLearnedSignalSource;
using atx::engine::learn::SequenceTensor;
using atx::engine::learn::TcnAlphaCfg;

// RollingPanel ring capacity for the live-predict fixture: a power of two >= the
// longest benched lookback (L = 40 here -> 64 is the next power of two).
constexpr usize kLiveCap = 64;

// ---------------------------------------------------------------------------
//  make_seq — a small synthetic SequenceTensor built DIRECTLY (full control over
//  the window bytes + planted labels), mirroring the S5-2b TCN/GRU unit tests. Two
//  raw fields per step (F is fixed at 2 — the channels knob sizes the NET, not the
//  feature count) and a clean planted label so the fit has signal to compress.
//
//  `n_samples_target` shapes how many valid samples the tensor carries: with one
//  instrument per anchor and a full L-deep trailing window over [L-1, n_dates), the
//  sample count is (n_dates - L + 1) * n_inst. We fix n_inst and derive n_dates.
// ---------------------------------------------------------------------------
constexpr usize kSeqFeatures = 2; // F (raw channels per step); fixed across the grid

[[nodiscard]] SequenceTensor make_seq(usize lookback, usize n_samples_target, usize n_horizons,
                                      u64 seed) {
  constexpr usize kNInst = 4; // anchors per date; n_dates derives the sample count
  const usize windows_per_inst = (n_samples_target + kNInst - 1U) / kNInst; // ceil
  const usize n_dates = (lookback - 1U) + windows_per_inst;

  SequenceTensor st;
  st.lookback = lookback;
  st.n_features = kSeqFeatures;
  st.y.assign(n_horizons, {});
  atx::core::Xoshiro256pp rng{seed};

  for (usize d = lookback - 1U; d < n_dates; ++d) {
    for (usize i = 0; i < kNInst; ++i) {
      std::vector<f64> window(lookback * kSeqFeatures, 0.0);
      for (usize l = 0; l < lookback; ++l) {
        const usize wd = d - (lookback - 1U) + l;
        for (usize f = 0; f < kSeqFeatures; ++f) {
          const f64 smooth = 0.1 * static_cast<f64>(wd) + 0.3 * static_cast<f64>(i) +
                             0.05 * static_cast<f64>(f);
          window[l * kSeqFeatures + f] = smooth + 0.2 * rng.normal();
        }
      }
      const f64 trailing0 = window[(lookback - 1U) * kSeqFeatures + 0U];
      for (usize h = 0; h < n_horizons; ++h) {
        st.y[h].push_back(trailing0 + 0.01 * static_cast<f64>(h)); // planted
      }
      for (const f64 v : window) {
        st.x.push_back(v);
      }
      st.date_of.push_back(d);
      st.inst_of.push_back(i);
      st.sample_valid.push_back(static_cast<u8>(1));
      ++st.n_samples;
    }
  }
  return st;
}

// ---------------------------------------------------------------------------
//  Tiny, fast, deterministic configs. `channels` sizes the architecture; epochs +
//  ensemble are kept tiny so the SURFACE cost (not convergence) is what's measured.
// ---------------------------------------------------------------------------
[[nodiscard]] TcnAlphaCfg bench_tcn_cfg(usize channels, usize ensemble, usize epochs = 3U) {
  TcnAlphaCfg cfg;
  cfg.blocks = 2;
  cfg.kernel = 2;
  cfg.channels = channels;
  cfg.dropout = 0.0;
  cfg.cpcv.n_groups = 2;
  cfg.cpcv.n_test_groups = 1;
  cfg.cpcv.embargo = 0.0;
  cfg.horizons = {1, 2};
  cfg.train.epochs = epochs;
  cfg.train.batch_size = 32;
  cfg.train.ckpt_every = 5;
  cfg.train.ensemble_size = ensemble;
  cfg.train.master_seed = 4242;
  return cfg;
}

[[nodiscard]] GruAlphaCfg bench_gru_cfg(usize hidden, usize ensemble, usize epochs = 3U) {
  GruAlphaCfg cfg;
  cfg.hidden = hidden;
  cfg.dropout = 0.0;
  cfg.cpcv.n_groups = 2;
  cfg.cpcv.n_test_groups = 1;
  cfg.cpcv.embargo = 0.0;
  cfg.horizons = {1, 2};
  cfg.train.epochs = epochs;
  cfg.train.batch_size = 32;
  cfg.train.ckpt_every = 5;
  cfg.train.ensemble_size = ensemble;
  cfg.train.master_seed = 4242;
  return cfg;
}

[[nodiscard]] AttnAlphaCfg bench_attn_cfg(usize d_model, usize ensemble, usize epochs = 3U) {
  AttnAlphaCfg cfg;
  cfg.d_model = d_model;
  cfg.dropout = 0.0;
  cfg.cpcv.n_groups = 2;
  cfg.cpcv.n_test_groups = 1;
  cfg.cpcv.embargo = 0.0;
  cfg.horizons = {1, 2};
  cfg.train.epochs = epochs;
  cfg.train.batch_size = 32;
  cfg.train.ckpt_every = 5;
  cfg.train.ensemble_size = ensemble;
  cfg.train.master_seed = 4242;
  return cfg;
}

[[nodiscard]] AeFactorCfg bench_ae_cfg(usize ensemble, usize epochs = 3U) {
  AeFactorCfg cfg;
  cfg.k_factors = 2; // K <= F (== kSeqFeatures); leading latent is the alpha
  cfg.beta_hidden = {};
  cfg.l1 = 0.0;
  cfg.l2 = 0.0;
  cfg.train.epochs = epochs;
  cfg.train.batch_size = 32;
  cfg.train.ckpt_every = 5;
  cfg.train.ensemble_size = ensemble;
  cfg.train.master_seed = 4242;
  return cfg;
}

// Decode the (L, N, channels) triple from a benchmark Args row. state.range(i) is
// std::int64_t; convert to usize explicitly (-Wconversion clean).
[[nodiscard]] usize arg_usize(const benchmark::State &state, int i) {
  return static_cast<usize>(state.range(i));
}

// ---------------------------------------------------------------------------
//  FIT benches — the fixture is built ONCE per case (outside the timed region);
//  the fit (CPCV walk + deployed refit) is what's timed. ->Iterations(1) because a
//  single fit is heavy; ->Unit(kMillisecond) for readable rows.
// ---------------------------------------------------------------------------
void BM_NnAlphaFit_Tcn(benchmark::State &state) {
  const usize L = arg_usize(state, 0);
  const usize N = arg_usize(state, 1);
  const usize channels = arg_usize(state, 2);
  const SequenceTensor seq = make_seq(L, N, /*n_horizons=*/2U, 0xA11CE5ULL);
  const TcnAlphaCfg cfg = bench_tcn_cfg(channels, /*ensemble=*/1U);
  for (auto _ : state) {
    auto model = fit_tcn(seq, cfg);
    benchmark::DoNotOptimize(model);
    benchmark::ClobberMemory();
  }
}
BENCHMARK(BM_NnAlphaFit_Tcn)
    ->Args({16, 24, 16})  // common corner
    ->Args({32, 24, 16})  // +L
    ->Args({16, 48, 16})  // +N
    ->Args({16, 24, 32})  // +channels (the costliest axis; one demonstrator)
    ->Iterations(1)
    ->Unit(benchmark::kMillisecond);

void BM_NnAlphaFit_Gru(benchmark::State &state) {
  const usize L = arg_usize(state, 0);
  const usize N = arg_usize(state, 1);
  const usize hidden = arg_usize(state, 2);
  const SequenceTensor seq = make_seq(L, N, /*n_horizons=*/2U, 0xB22D06ULL);
  const GruAlphaCfg cfg = bench_gru_cfg(hidden, /*ensemble=*/1U);
  for (auto _ : state) {
    auto model = fit_gru(seq, cfg);
    benchmark::DoNotOptimize(model);
    benchmark::ClobberMemory();
  }
}
BENCHMARK(BM_NnAlphaFit_Gru)
    ->Args({16, 24, 16})  // common corner
    ->Args({32, 24, 16})  // +L
    ->Args({16, 48, 16})  // +N
    ->Args({16, 24, 32})  // +hidden
    ->Iterations(1)
    ->Unit(benchmark::kMillisecond);

void BM_NnAlphaFit_Attn(benchmark::State &state) {
  const usize L = arg_usize(state, 0);
  const usize N = arg_usize(state, 1);
  const usize d_model = arg_usize(state, 2);
  const SequenceTensor seq = make_seq(L, N, /*n_horizons=*/2U, 0xC33E07ULL);
  const AttnAlphaCfg cfg = bench_attn_cfg(d_model, /*ensemble=*/1U);
  for (auto _ : state) {
    auto model = fit_attn(seq, cfg);
    benchmark::DoNotOptimize(model);
    benchmark::ClobberMemory();
  }
}
BENCHMARK(BM_NnAlphaFit_Attn)
    ->Args({16, 24, 16})  // common corner
    ->Args({32, 24, 16})  // +L
    ->Args({16, 48, 16})  // +N
    ->Args({16, 24, 32})  // +d_model
    ->Iterations(1)
    ->Unit(benchmark::kMillisecond);

void BM_NnAlphaFit_Ae(benchmark::State &state) {
  const usize L = arg_usize(state, 0);
  const usize N = arg_usize(state, 1);
  // channels arg is unused for the AE (its width is F->K, not a channel count); it
  // is carried so the AE rows line up with the predictive-track grid in the JSON.
  const SequenceTensor seq = make_seq(L, N, /*n_horizons=*/1U, 0xD44F08ULL);
  const AeFactorCfg cfg = bench_ae_cfg(/*ensemble=*/1U);
  for (auto _ : state) {
    auto model = fit_autoencoder_factors(seq, cfg);
    benchmark::DoNotOptimize(model);
    benchmark::ClobberMemory();
  }
}
BENCHMARK(BM_NnAlphaFit_Ae)
    ->Args({16, 24, 16})  // common corner
    ->Args({32, 24, 16})  // +L
    ->Args({16, 48, 16})  // +N
    ->Iterations(1)
    ->Unit(benchmark::kMillisecond);

// ---------------------------------------------------------------------------
//  SEED-ENSEMBLE cost — the SAME arch / size at ensemble_size ∈ {1, 5}, so the
//  ×ensemble_size training multiplier is directly readable from two adjacent rows.
// ---------------------------------------------------------------------------
void BM_NnAlphaFit_Tcn_Ensemble(benchmark::State &state) {
  const usize ensemble = arg_usize(state, 0);
  const SequenceTensor seq = make_seq(/*L=*/16U, /*N=*/24U, /*n_horizons=*/2U, 0xE55009ULL);
  const TcnAlphaCfg cfg = bench_tcn_cfg(/*channels=*/16U, ensemble);
  for (auto _ : state) {
    auto model = fit_tcn(seq, cfg);
    benchmark::DoNotOptimize(model);
    benchmark::ClobberMemory();
  }
}
BENCHMARK(BM_NnAlphaFit_Tcn_Ensemble)->Arg(1)->Arg(5)->Iterations(1)->Unit(benchmark::kMillisecond);

// ---------------------------------------------------------------------------
//  PREDICT benches — the model is fit ONCE outside the timed loop; only the
//  inference forward is timed. predict_nn on a fixed L*F window row.
// ---------------------------------------------------------------------------
[[nodiscard]] std::vector<f64> window_of(const SequenceTensor &seq, usize sample) {
  const usize wlen = seq.lookback * seq.n_features;
  std::vector<f64> row(wlen);
  for (usize j = 0; j < wlen; ++j) {
    row[j] = seq.x[sample * wlen + j];
  }
  return row;
}

void BM_NnAlphaPredict_Nn(benchmark::State &state) {
  const usize L = arg_usize(state, 0);
  const usize channels = arg_usize(state, 1);
  const SequenceTensor seq = make_seq(L, /*N=*/24U, /*n_horizons=*/2U, 0xF6610AULL);
  const auto fitted = fit_tcn(seq, bench_tcn_cfg(channels, /*ensemble=*/2U));
  if (!fitted) {
    state.SkipWithError("fit_tcn failed in predict fixture");
    return;
  }
  const LearnedModel model = *fitted;
  const std::vector<f64> row = window_of(seq, 0U);
  for (auto _ : state) {
    f64 score = predict_nn(model, std::span<const f64>{row});
    benchmark::DoNotOptimize(score);
  }
  state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_NnAlphaPredict_Nn)->Args({16, 16})->Args({32, 32});

// ---------------------------------------------------------------------------
//  LIVE predict — SeqLearnedSignalSource::evaluate over a pre-built RollingPanel.
//  The model + panel are built once; only the cross-section evaluate is timed (it
//  runs predict_nn per in-universe instrument — the live path the loop drives).
// ---------------------------------------------------------------------------
struct LivePanelFixture {
  std::vector<InstrumentId> universe;
  atx::engine::RollingPanel<kLiveCap> panel;
  explicit LivePanelFixture(std::vector<InstrumentId> uni, usize lookback)
      : universe(std::move(uni)),
        panel(std::span<const InstrumentId>{universe}, lookback) {}
};

[[nodiscard]] Price price_of(f64 v) {
  return Price::from_decimal(Decimal::from_double(v).value());
}
[[nodiscard]] Quantity qty_of(f64 v) {
  return Quantity::from_decimal(Decimal::from_double(v).value());
}

void BM_NnAlphaPredict_SeqSource(benchmark::State &state) {
  const usize L = arg_usize(state, 0);
  const usize channels = arg_usize(state, 1);
  constexpr usize kNInst = 4;
  const std::vector<std::string> raw_fields{"close", "volume"}; // F = 2 (matches kSeqFeatures)
  const usize n_dates = L + 4U; // a few rows beyond the window depth

  const SequenceTensor seq = make_seq(L, /*N=*/24U, /*n_horizons=*/2U, 0x17710BULL);
  const auto fitted = fit_tcn(seq, bench_tcn_cfg(channels, /*ensemble=*/2U));
  if (!fitted) {
    state.SkipWithError("fit_tcn failed in live-predict fixture");
    return;
  }

  std::vector<InstrumentId> uni;
  uni.reserve(kNInst);
  for (usize j = 0; j < kNInst; ++j) {
    uni.push_back(Symbol{static_cast<u32>(j + 1U)});
  }
  LivePanelFixture fx{uni, /*lookback=*/L};
  for (usize d = 0; d < n_dates; ++d) {
    std::vector<SliceRow> rows;
    rows.reserve(kNInst);
    for (usize j = 0; j < kNInst; ++j) {
      const f64 px = 100.0 + static_cast<f64>(j) * 5.0 + static_cast<f64>(d) * 0.5;
      const f64 vol = 1000.0 + static_cast<f64>(j) * 30.0 + static_cast<f64>(d) * 12.0;
      Bar b{};
      b.ts = Timestamp::from_unix_nanos(static_cast<i64>(d) + 1);
      const Price p = price_of(px);
      b.open = p;
      b.high = p;
      b.low = p;
      b.close = p;
      b.volume = qty_of(vol);
      rows.push_back(SliceRow{fx.universe[j], b, false});
    }
    fx.panel.append_sealed_row(
        MarketSlice{Timestamp::from_unix_nanos(static_cast<i64>(d) + 1),
                    std::span<const SliceRow>{rows}});
  }

  SeqLearnedSignalSource src{*fitted, raw_fields, L, kNInst};
  for (auto _ : state) {
    auto sig = src.evaluate(fx.panel.view());
    benchmark::DoNotOptimize(sig);
    benchmark::ClobberMemory();
  }
  state.SetItemsProcessed(state.iterations() * static_cast<i64>(kNInst));
}
BENCHMARK(BM_NnAlphaPredict_SeqSource)->Args({16, 16})->Args({32, 32});

} // namespace
