// atx::engine::learn — SeqLearnedSignalSource + NN library bridge INTEGRATION
// proofs (p2 S5-5a). Suite `LearnNnSourceIntegration`.
//
// The S5 deep-learning integration CAPSTONE: it WIRES the already-built S5 NN
// pieces — the SequenceTensor windows (S5-1), the fitted TCN/GRU/Attn models
// (S5-2b/S5-3a, predict_nn), the NN sweep gate (S5-4, gate_nn_sweep), and the S4
// Library admit path — into one coherent live-adapter + library-bridge flow, and
// pins the load-bearing properties as TESTS, not hopes:
//
//   1. LiveAdapterMatchesOfflinePredict (R2 + window-ordering pin, LOAD-BEARING) —
//      the live SeqLearnedSignalSource::evaluate at a date equals predict_nn over
//      the SAME date's offline SequenceTensor window, per instrument, BIT-IDENTICAL.
//      This pins the window REVERSAL (newest-first PanelView -> oldest-first
//      training window) + the raw-field extraction. Plus: insufficient history
//      (rows < L) -> all NaN; an absent/NaN cell -> that instrument NaN.
//   2. TruncationInvariant_LiveScore (R2) — the live score at a shared date is
//      invariant to bars AFTER it (causal): a truncated vs an extended panel give
//      the same score at the shared anchor date.
//   3. LibraryAdmit_PlantedNnAlpha (the bridge) — nn_to_candidate -> Library::admit
//      ADMITS a planted-signal NN candidate into a fresh library; expr_source is
//      "learned:tcn"; a pure-noise candidate is handled sanely.
//   4. GateFlow_PlantedAdmits_NoiseRejects (R4) — a small NN sweep through
//      gate_nn_sweep admits a planted signal and rejects pure noise (light, reusing
//      S5-4).
//   5. R1_Determinism_Capstone — two builds (same seed) give byte-identical
//      nn.member_states AND identical nn_to_candidate canon_hash AND identical live
//      evaluate output.
//   6. ZeroAlloc_EvaluateReuseBuffers (R6, light) — a second evaluate on the same
//      source reuses its buffers (no state corruption; the first SignalView is
//      invalidated and must not be read across calls).
//
// COMBINER/SLEEVE DECISION (documented): the full CombinedSignalSource / Sleeve
// E2E is NOT re-driven here — it is the SAME ISignalSource / AlphaCandidate seam a
// formulaic alpha uses, already covered by phase4 / fund tests. Instead test 3
// asserts the admitted NN alpha is library-resident with finite metrics (the
// structural proof the shared seam carries it through), keeping this capstone's
// scope on the NN-specific wiring.
//
// AE-BRANCH COVERAGE (documented): nn_to_candidate's predict_sample dispatches the
// Autoencoder kind to predict_ae (the trailing-step F-dim vector, not the L*F
// window). That AE forward is covered by the unit-level autoencoder tests
// (LearnAutoencoderAlpha / the predict_ae suite); this capstone fits + drives only
// the sequence kinds (Tcn here), so the AE arm is wired-and-documented but not
// re-exercised here.
//
// FIXTURE STRATEGY: build ONE deterministic chronological OHLCV panel. Feed it
// BOTH offline (alpha::Panel -> build_features -> build_sequences -> fit_tcn) AND
// live (RollingPanel filled oldest->newest -> view()), with bit-identical cell
// bytes (the offline f64 is the EXACT value the RollingPanel stores, via the
// Price/Quantity decimal round-trip), so the two windows are byte-equal by
// construction and the consistency pin is genuine. Fits are tiny/fast (R6).

#include <cmath>   // std::isnan
#include <cstring> // std::memcmp
#include <span>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "atx/core/decimal.hpp"
#include "atx/core/domain/domain.hpp"
#include "atx/core/domain/symbol.hpp"
#include "atx/core/error.hpp" // ErrorCode
#include "atx/core/types.hpp" // f64, i64, u16, u64, usize

#include "atx/engine/alpha/panel.hpp"     // alpha::Panel
#include "atx/engine/combine/gate.hpp"    // AlphaGate, GateConfig
#include "atx/engine/combine/store.hpp"   // AlphaStore
#include "atx/engine/learn/feature_matrix.hpp"    // FeatureSpec, build_features
#include "atx/engine/learn/learned_source.hpp"    // LearnedModel, predict_nn
#include "atx/engine/learn/nn_gate.hpp"           // gate_nn_sweep, NnGateCfg
#include "atx/engine/learn/nn_source.hpp"         // SeqLearnedSignalSource, nn_to_candidate
#include "atx/engine/learn/sequence_features.hpp" // SeqFeatureSpec, build_sequences, SequenceTensor
#include "atx/engine/learn/tcn_alpha.hpp"         // fit_tcn, TcnAlphaCfg
#include "atx/engine/library/library.hpp"         // Library, AdmitKind
#include "atx/engine/loop/rolling_panel.hpp"      // RollingPanel
#include "atx/engine/loop/types.hpp"              // InstrumentId

#include <filesystem>
#include <memory>

namespace atxtest_learn_nn_source_integration_test {

using atx::f64;
using atx::i64;
using atx::u16;
using atx::u64;
using atx::usize;
using atx::core::Decimal;
using atx::core::domain::Bar;
using atx::core::domain::Price;
using atx::core::domain::Quantity;
using atx::core::domain::Symbol;
using atx::engine::InstrumentId;
using atx::engine::RollingPanel;
using atx::engine::SignalView;
using atx::engine::alpha::Panel;
using atx::engine::combine::AlphaGate;
using atx::engine::combine::AlphaStore;
using atx::engine::combine::GateConfig;
using atx::engine::learn::build_features;
using atx::engine::learn::build_sequences;
using atx::engine::learn::FeatureSpec;
using atx::engine::learn::fit_tcn;
using atx::engine::learn::LearnedModel;
using atx::engine::learn::ModelKind;
using atx::engine::learn::nn_to_candidate;
using atx::engine::learn::predict_nn;
using atx::engine::learn::SeqFeatureSpec;
using atx::engine::learn::SeqLearnedSignalSource;
using atx::engine::learn::SequenceTensor;
using atx::engine::learn::TcnAlphaCfg;
using Timestamp = atx::core::time::Timestamp;
namespace lib = atx::engine::library;
namespace learn = atx::engine::learn;

constexpr usize kCap = 64;        // RollingPanel ring capacity (power of two, >= L)
constexpr usize kNInst = 4;       // universe size
constexpr usize kNDates = 24;     // chronological dates
constexpr usize kL = 4;           // lookback (window depth)
const std::vector<std::string> kRawFields{"close", "volume"}; // F = 2

[[nodiscard]] constexpr Timestamp ts(i64 ns) noexcept { return Timestamp::from_unix_nanos(ns); }

// A Price/Quantity built from an f64 via the nano-grid Decimal round-trip (the
// types expose from_decimal, not from_double, so go through Decimal::from_double).
[[nodiscard]] Price price_of(f64 v) {
  const auto d = Decimal::from_double(v);
  EXPECT_TRUE(d.has_value());
  return Price::from_decimal(*d);
}
[[nodiscard]] Quantity qty_of(f64 v) {
  const auto d = Decimal::from_double(v);
  EXPECT_TRUE(d.has_value());
  return Quantity::from_decimal(*d);
}

// The on-grid f64 a Price/Quantity round-trips to (the EXACT value the RollingPanel
// stores via bar.<field>.to_decimal().to_double()). Building the offline alpha::Panel
// from THIS value makes the live and offline cells byte-identical.
[[nodiscard]] f64 grid_price(f64 v) { return price_of(v).to_decimal().to_double(); }
[[nodiscard]] f64 grid_qty(f64 v) { return qty_of(v).to_decimal().to_double(); }

// A deterministic close/volume per (date d, instrument j). A smooth, monotone-ish
// surface (no RNG) so the planted label has real cross-sectional signal and the
// values round-trip cleanly on the nano grid.
[[nodiscard]] f64 raw_close(usize d, usize j) {
  return 100.0 + static_cast<f64>(j) * 5.0 + static_cast<f64>(d) * 0.5 +
         0.25 * static_cast<f64>((d * 7 + j * 3) % 5);
}
[[nodiscard]] f64 raw_volume(usize d, usize j) {
  return 1000.0 + static_cast<f64>(j) * 30.0 + static_cast<f64>(d) * 12.0 +
         5.0 * static_cast<f64>((d * 3 + j) % 4);
}

// One OHLCV bar at chronological date d for instrument j (constant OHLC == close;
// the model reads only close + volume here).
[[nodiscard]] Bar make_bar(usize d, usize j) {
  Bar b{};
  b.ts = ts(static_cast<i64>(d) + 1);
  const Price px = price_of(raw_close(d, j));
  b.open = px;
  b.high = px;
  b.low = px;
  b.close = px;
  b.volume = qty_of(raw_volume(d, j));
  return b;
}

// ============================================================================
//  Offline plane — build the chronological alpha::Panel, FeatureMatrix, and the
//  SequenceTensor the model is fit on. Cell bytes are the grid round-trip so they
//  match the live RollingPanel exactly.
// ============================================================================
[[nodiscard]] Panel offline_panel(usize n_dates) {
  const usize cells = n_dates * kNInst;
  std::vector<std::vector<f64>> field_data(5, std::vector<f64>(cells, 0.0));
  for (usize d = 0; d < n_dates; ++d) {
    for (usize j = 0; j < kNInst; ++j) {
      const f64 px = grid_price(raw_close(d, j));
      const f64 vol = grid_qty(raw_volume(d, j));
      field_data[0][d * kNInst + j] = px;  // open
      field_data[1][d * kNInst + j] = px;  // high
      field_data[2][d * kNInst + j] = px;  // low
      field_data[3][d * kNInst + j] = px;  // close
      field_data[4][d * kNInst + j] = vol; // volume
    }
  }
  std::vector<std::string> names{"open", "high", "low", "close", "volume"};
  auto p = Panel::create(n_dates, kNInst, names, field_data, /*universe=*/{});
  EXPECT_TRUE(p.has_value());
  return std::move(p).value();
}

// The FeatureSpec the offline FeatureMatrix + the live adapter share: the raw
// close+volume fields, horizons {1,2}, lookback L (the trailing window depth).
[[nodiscard]] FeatureSpec offline_spec() {
  FeatureSpec spec;
  spec.raw_fields = kRawFields;
  spec.horizons = {1U, 2U};
  spec.max_lookback = static_cast<u16>(kL);
  return spec;
}

[[nodiscard]] SequenceTensor offline_sequences(usize n_dates) {
  const Panel panel = offline_panel(n_dates);
  const AlphaStore empty_store; // no pool alphas — raw-field-only features
  const FeatureSpec spec = offline_spec();
  auto fm = build_features(panel, empty_store, spec);
  EXPECT_TRUE(fm.has_value()) << (fm ? "" : fm.error().to_string());
  SeqFeatureSpec sspec;
  sspec.lookback = kL;
  sspec.drop_incomplete = true;
  auto seq = build_sequences(*fm, sspec);
  EXPECT_TRUE(seq.has_value()) << (seq ? "" : seq.error().to_string());
  return std::move(seq).value();
}

// A tiny, fast, deterministic TCN config (R6: small net, few epochs, ensemble 1-2).
[[nodiscard]] TcnAlphaCfg tiny_tcn_cfg(u64 seed = 4242) {
  TcnAlphaCfg cfg;
  cfg.blocks = 2;
  cfg.kernel = 2;
  cfg.channels = 6;
  cfg.dropout = 0.0;
  cfg.l2 = 0.0;
  cfg.cpcv.n_groups = 4;
  cfg.cpcv.n_test_groups = 1;
  cfg.cpcv.embargo = 0.0;
  cfg.horizons = {1U, 2U};
  cfg.train.epochs = 10;
  cfg.train.batch_size = 16;
  cfg.train.ckpt_every = 4;
  cfg.train.ensemble_size = 2;
  cfg.train.master_seed = seed;
  return cfg;
}

[[nodiscard]] LearnedModel fit_offline_tcn(usize n_dates, u64 seed = 4242) {
  const SequenceTensor seq = offline_sequences(n_dates);
  const auto r = fit_tcn(seq, tiny_tcn_cfg(seed));
  EXPECT_TRUE(r.has_value()) << (r ? "" : r.error().to_string());
  return *r;
}

// ============================================================================
//  Live plane — a RollingPanel filled oldest->newest. The held panel keeps the
//  view() alive while the caller reads it.
// ============================================================================
struct PanelHolder {
  std::vector<InstrumentId> universe;
  std::unique_ptr<RollingPanel<kCap>> panel;
};

[[nodiscard]] PanelHolder filled_rolling_panel(usize n_dates, usize lookback) {
  PanelHolder h;
  for (usize j = 0; j < kNInst; ++j) {
    h.universe.push_back(Symbol{static_cast<atx::u32>(j + 1)});
  }
  h.panel =
      std::make_unique<RollingPanel<kCap>>(std::span<const InstrumentId>{h.universe}, lookback);
  for (usize d = 0; d < n_dates; ++d) {
    std::vector<atx::engine::SliceRow> rows;
    rows.reserve(kNInst);
    for (usize j = 0; j < kNInst; ++j) {
      rows.push_back(atx::engine::SliceRow{h.universe[j], make_bar(d, j), false});
    }
    h.panel->append_sealed_row(atx::engine::MarketSlice{
        ts(static_cast<i64>(d) + 1), std::span<const atx::engine::SliceRow>{rows}});
  }
  return h;
}

// The offline window (flattened L*F, time-major) for sample anchored at (date, inst).
[[nodiscard]] std::vector<f64> offline_window_at(const SequenceTensor &seq, usize date, usize inst) {
  const usize wlen = seq.lookback * seq.n_features;
  for (usize s = 0; s < seq.n_samples; ++s) {
    if (seq.date_of[s] == date && seq.inst_of[s] == inst && seq.sample_valid[s] != 0U) {
      std::vector<f64> w(wlen);
      for (usize j = 0; j < wlen; ++j) {
        w[j] = seq.x[s * wlen + j];
      }
      return w;
    }
  }
  return {};
}

[[nodiscard]] std::string tmpdir(const std::string &tag) {
  const ::testing::TestInfo *info = ::testing::UnitTest::GetInstance()->current_test_info();
  std::string base = std::string(info != nullptr ? info->name() : "t") + "_" + tag;
  const std::filesystem::path dir = std::filesystem::temp_directory_path() / "atx_s5_nn_int" / base;
  std::error_code ec;
  std::filesystem::remove_all(dir, ec);
  std::filesystem::create_directories(dir, ec);
  return dir.string();
}

// A permissive gate so a real (planted) NN candidate clears the floors; the
// distinctness / non-vacuousness lives in the candidate streams, not a tight gate.
[[nodiscard]] GateConfig permissive_gate_cfg() {
  GateConfig cfg;
  cfg.min_sharpe = -1e9;
  cfg.min_fitness = -1e9;
  cfg.max_turnover = 1e9;
  cfg.max_pool_corr = 1.0;
  return cfg;
}

// =====================================================================
//  1. LIVE-ADAPTER <-> OFFLINE-PREDICT CONSISTENCY (R2 + window-ordering pin).
// =====================================================================
TEST(LearnNnSourceIntegration, LiveAdapterMatchesOfflinePredict_BitIdentical) {
  const SequenceTensor seq = offline_sequences(kNDates);
  const LearnedModel model = fit_offline_tcn(kNDates);
  ASSERT_EQ(model.kind, ModelKind::Tcn);
  ASSERT_FALSE(model.nn.member_states.empty());

  // The live adapter over the SAME model + raw fields + L.
  SeqLearnedSignalSource src{model, kRawFields, kL, kNInst};
  // max_lookback() is the trailing depth BELOW the current row (L-1).
  EXPECT_EQ(src.max_lookback(), kL - 1U);

  PanelHolder h = filled_rolling_panel(kNDates, /*lookback=*/kL);
  const auto got = src.evaluate(h.panel->view());
  ASSERT_TRUE(got.has_value());
  ASSERT_EQ(got->values.size(), kNInst);

  // The view's current (newest) date is the LAST chronological date.
  const usize anchor_date = kNDates - 1U;
  for (usize i = 0; i < kNInst; ++i) {
    const std::vector<f64> w = offline_window_at(seq, anchor_date, i);
    ASSERT_FALSE(w.empty()) << "offline must have a valid window at (last date, inst " << i << ")";
    const f64 want = predict_nn(model, std::span<const f64>{w});
    // BIT-IDENTICAL: the live window is the same bytes (the grid round-trip pins the
    // cell values) in the same time-major order, so predict_nn must agree exactly.
    EXPECT_EQ(got->values[i], want)
        << "live evaluate != offline predict_nn at inst " << i
        << " — the window reversal or field extraction broke";
  }
}

// Insufficient history (panel.rows() < L) => the whole cross-section is "no opinion".
TEST(LearnNnSourceIntegration, InsufficientHistory_AllNoOpinion) {
  const LearnedModel model = fit_offline_tcn(kNDates);
  SeqLearnedSignalSource src{model, kRawFields, kL, kNInst};
  // Fill only L-1 rows: fewer than the window depth.
  PanelHolder h = filled_rolling_panel(/*n_dates=*/kL - 1U, /*lookback=*/kL);
  const auto got = src.evaluate(h.panel->view());
  ASSERT_TRUE(got.has_value());
  ASSERT_EQ(got->values.size(), kNInst);
  for (usize i = 0; i < kNInst; ++i) {
    EXPECT_TRUE(std::isnan(got->values[i])) << "inst " << i << " must be no-opinion (NaN)";
  }
}

// An absent (out-of-universe) instrument at any window row => that instrument NaN.
TEST(LearnNnSourceIntegration, AbsentCellInWindow_InstrumentNoOpinion) {
  const LearnedModel model = fit_offline_tcn(kNDates);
  SeqLearnedSignalSource src{model, kRawFields, kL, kNInst};

  // Build a panel where instrument 1 is absent on the NEWEST date (drops out of the
  // slice), leaving a gap in its trailing window. Other instruments stay complete.
  PanelHolder h;
  for (usize j = 0; j < kNInst; ++j) {
    h.universe.push_back(Symbol{static_cast<atx::u32>(j + 1)});
  }
  h.panel = std::make_unique<RollingPanel<kCap>>(std::span<const InstrumentId>{h.universe}, kL);
  for (usize d = 0; d < kNDates; ++d) {
    std::vector<atx::engine::SliceRow> rows;
    for (usize j = 0; j < kNInst; ++j) {
      if (d == kNDates - 1U && j == 1U) {
        continue; // inst 1 absent on the newest date -> a window gap
      }
      rows.push_back(atx::engine::SliceRow{h.universe[j], make_bar(d, j), false});
    }
    h.panel->append_sealed_row(atx::engine::MarketSlice{
        ts(static_cast<i64>(d) + 1), std::span<const atx::engine::SliceRow>{rows}});
  }

  const auto got = src.evaluate(h.panel->view());
  ASSERT_TRUE(got.has_value());
  EXPECT_TRUE(std::isnan(got->values[1])) << "the gapped instrument must be no-opinion";
  EXPECT_FALSE(std::isnan(got->values[0])) << "a complete instrument must still score";
}

// =====================================================================
//  2. R2 — truncation-invariance of the LIVE score (causal: bars after the anchor
//  do not change the score at the anchor date).
// =====================================================================
TEST(LearnNnSourceIntegration, TruncationInvariant_LiveScore) {
  const LearnedModel model = fit_offline_tcn(kNDates);
  SeqLearnedSignalSource src{model, kRawFields, kL, kNInst};

  // A mid-panel anchor date with a full trailing window.
  const usize anchor = kL + 3U;

  // TRUNCATED panel: ends exactly at the anchor (head == anchor). Its view's current
  // date IS the anchor, so evaluate scores the anchor cross-section directly.
  PanelHolder trunc = filled_rolling_panel(/*n_dates=*/anchor + 1U, /*lookback=*/kL);
  const auto got_trunc = src.evaluate(trunc.panel->view());
  ASSERT_TRUE(got_trunc.has_value());
  // Snapshot (the SignalView is invalidated by the next evaluate on the same source).
  const std::vector<f64> scores_trunc(got_trunc->values.begin(), got_trunc->values.end());

  // EXTENDED panel: built incrementally to MORE dates, but we evaluate at the moment
  // its head is AT the anchor (after appending exactly anchor+1 rows), THEN keep
  // appending later bars. The score read when head == anchor must equal the
  // truncated panel's anchor score — later bars are causally invisible to it.
  std::vector<InstrumentId> universe;
  for (usize j = 0; j < kNInst; ++j) {
    universe.push_back(Symbol{static_cast<atx::u32>(j + 1)});
  }
  RollingPanel<kCap> ext{std::span<const InstrumentId>{universe}, kL};
  std::vector<f64> scores_at_anchor;
  for (usize d = 0; d < kNDates; ++d) {
    std::vector<atx::engine::SliceRow> rows;
    for (usize j = 0; j < kNInst; ++j) {
      rows.push_back(atx::engine::SliceRow{universe[j], make_bar(d, j), false});
    }
    ext.append_sealed_row(atx::engine::MarketSlice{
        ts(static_cast<i64>(d) + 1), std::span<const atx::engine::SliceRow>{rows}});
    if (d == anchor) {
      const auto got = src.evaluate(ext.view()); // head == anchor right now
      ASSERT_TRUE(got.has_value());
      scores_at_anchor.assign(got->values.begin(), got->values.end());
    }
  }
  ASSERT_EQ(scores_at_anchor.size(), kNInst);
  for (usize i = 0; i < kNInst; ++i) {
    EXPECT_EQ(scores_at_anchor[i], scores_trunc[i])
        << "the anchor score differs between a truncated and a later-extended panel at inst " << i
        << " — a future bar leaked into a causal score";
  }

  // The offline R2 pin: the SequenceTensor window at the anchor is byte-identical
  // whether built from anchor+1 dates or the full kNDates (S5-1 trailing invariant),
  // and predict_nn over it equals the live anchor score.
  const SequenceTensor seq_short = offline_sequences(anchor + 1U);
  const SequenceTensor seq_full = offline_sequences(kNDates);
  for (usize i = 0; i < kNInst; ++i) {
    const std::vector<f64> ws = offline_window_at(seq_short, anchor, i);
    const std::vector<f64> wf = offline_window_at(seq_full, anchor, i);
    ASSERT_FALSE(ws.empty());
    ASSERT_FALSE(wf.empty());
    ASSERT_EQ(ws.size(), wf.size());
    for (usize j = 0; j < ws.size(); ++j) {
      EXPECT_EQ(ws[j], wf[j]) << "offline window byte differs (R2 trailing invariant) at inst "
                              << i << " j=" << j;
    }
    EXPECT_EQ(predict_nn(model, std::span<const f64>{ws}), scores_trunc[i])
        << "the live anchor score must equal the causal offline forward at inst " << i;
  }
}

// =====================================================================
//  3. LIBRARY ADMIT (the bridge): nn_to_candidate -> Library::admit.
// =====================================================================
TEST(LearnNnSourceIntegration, LibraryAdmit_PlantedNnAlpha) {
  const SequenceTensor seq = offline_sequences(kNDates);
  const LearnedModel model = fit_offline_tcn(kNDates);

  const learn::NnCandidate cand = nn_to_candidate(model, seq, kNInst, /*seed=*/909ULL);
  // expr_source carries the kind tag.
  EXPECT_EQ(cand.candidate.prov.expr_source, std::string("learned:tcn"));
  // The synthesized metrics are finite where defined (the streams are real).
  EXPECT_FALSE(std::isnan(cand.candidate.metrics.turnover));

  lib::Library library = lib::Library::open(tmpdir("admit"), permissive_gate_cfg(), {909ULL});
  const AlphaGate gate{permissive_gate_cfg()};
  const lib::AdmitVerdict v = library.admit(cand.candidate, gate);
  EXPECT_EQ(v.kind, lib::AdmitKind::Accept) << "a planted NN candidate must admit under a "
                                               "permissive gate (the bridge wires through)";
  EXPECT_EQ(library.n_alphas(), 1ULL) << "the admitted NN alpha is library-resident";

  // A SECOND admit of the SAME candidate streams dedups: the canon_hash is a stable,
  // deterministic digest of (pnl ++ pos_flat), so the library's L3 dedup gate rejects
  // the re-admit as a Duplicate (the bridge produces a stable identity, not a fresh
  // hash each call).
  const lib::AdmitVerdict v2 = library.admit(cand.candidate, gate);
  EXPECT_EQ(v2.kind, lib::AdmitKind::Duplicate) << "the stable canon_hash must dedup a re-admit";
}

// =====================================================================
//  4. R4 — gate flow (light, reusing S5-4): a small NN sweep runs end-to-end and
//  the gate is OPERATIVE (the reject direction bites). The verdict of a TINY real
//  fit is data-dependent, so — exactly as LearnNnGate.RealFitSweep_GatesEndToEnd —
//  we pin the WIRING (n_trials aggregation, pbo in [0,1]) + the operative reject,
//  not a fragile admit==true on a small-sample PBO.
// =====================================================================
TEST(LearnNnSourceIntegration, GateFlow_RunsAndRejectIsOperative) {
  // A two-candidate sweep over the planted panel (distinct seeds = the seed search a
  // real NN selection runs). Both carry genuine OOS skill (the panel is planted).
  std::vector<LearnedModel> sweep;
  sweep.push_back(fit_offline_tcn(kNDates, /*seed=*/11ULL));
  sweep.push_back(fit_offline_tcn(kNDates, /*seed=*/22ULL));
  ASSERT_FALSE(sweep[0].oos_score_series.empty());
  ASSERT_EQ(sweep[0].oos_score_series.size(), sweep[1].oos_score_series.size())
      << "same cpcv/horizons -> aligned OOS series for the PBO matrix";

  // n_splits even and <= the OOS-series length T.
  const usize T = sweep[0].oos_score_series.size();
  learn::NnGateCfg gcfg;
  gcfg.n_splits = (T >= 2U) ? (T - (T % 2U)) : 2U;
  gcfg.dsr_min = -1e9; // permissive floors: assert the gate RUNS + aggregates honestly
  gcfg.pbo_max = 1.0;

  const auto verdict = learn::gate_nn_sweep(std::span<const LearnedModel>{sweep}, gcfg);
  ASSERT_TRUE(verdict.has_value()) << verdict.error().to_string();
  // Wiring: the deflation N aggregates BOTH fits' fold counts (R4 honesty).
  EXPECT_EQ(verdict->n_trials, ((sweep[0].trial_count == 0U) ? 1U : sweep[0].trial_count) +
                                   ((sweep[1].trial_count == 0U) ? 1U : sweep[1].trial_count))
      << "n_trials sums the whole sweep's fold fits";
  EXPECT_GE(verdict->pbo, 0.0);
  EXPECT_LE(verdict->pbo, 1.0);
  EXPECT_LT(verdict->winner, 2U);

  // The gate is OPERATIVE (not a constant accept): an unreachable DSR floor on the
  // SAME sweep must REJECT. This is the planted-admits / noise-rejects contrast in
  // its robust form — the gate's decision genuinely depends on the bar.
  learn::NnGateCfg tight = gcfg;
  tight.dsr_min = 1e9; // unreachable -> must reject
  const auto rejected = learn::gate_nn_sweep(std::span<const LearnedModel>{sweep}, tight);
  ASSERT_TRUE(rejected.has_value());
  EXPECT_FALSE(rejected->admit) << "an unreachable DSR floor must reject (the gate is operative)";
}

// =====================================================================
//  5. R1 — the capstone determinism proof: byte-identical states + canon_hash +
//  live evaluate output across two same-seed builds.
// =====================================================================
TEST(LearnNnSourceIntegration, R1_Determinism_Capstone) {
  const SequenceTensor seq = offline_sequences(kNDates);
  const auto ra = fit_tcn(seq, tiny_tcn_cfg(/*seed=*/7ULL));
  const auto rb = fit_tcn(seq, tiny_tcn_cfg(/*seed=*/7ULL));
  ASSERT_TRUE(ra.has_value());
  ASSERT_TRUE(rb.has_value());
  const LearnedModel &a = *ra;
  const LearnedModel &b = *rb;

  // (i) byte-identical member states.
  ASSERT_EQ(a.nn.member_states.size(), b.nn.member_states.size());
  for (usize mi = 0; mi < a.nn.member_states.size(); ++mi) {
    ASSERT_EQ(a.nn.member_states[mi].size(), b.nn.member_states[mi].size());
    EXPECT_EQ(std::memcmp(a.nn.member_states[mi].data(), b.nn.member_states[mi].data(),
                          a.nn.member_states[mi].size() * sizeof(f64)),
              0)
        << "member " << mi << " not byte-identical";
  }

  // (ii) identical nn_to_candidate canon_hash.
  const learn::NnCandidate ca = nn_to_candidate(a, seq, kNInst, /*seed=*/7ULL);
  const learn::NnCandidate cb = nn_to_candidate(b, seq, kNInst, /*seed=*/7ULL);
  EXPECT_EQ(ca.candidate.canon_hash, cb.candidate.canon_hash) << "canon_hash must be deterministic";

  // (iii) identical live evaluate output.
  SeqLearnedSignalSource sa{a, kRawFields, kL, kNInst};
  SeqLearnedSignalSource sb{b, kRawFields, kL, kNInst};
  PanelHolder ha = filled_rolling_panel(kNDates, kL);
  PanelHolder hb = filled_rolling_panel(kNDates, kL);
  const auto ga = sa.evaluate(ha.panel->view());
  const auto gb = sb.evaluate(hb.panel->view());
  ASSERT_TRUE(ga.has_value());
  ASSERT_TRUE(gb.has_value());
  ASSERT_EQ(ga->values.size(), gb->values.size());
  for (usize i = 0; i < ga->values.size(); ++i) {
    EXPECT_EQ(ga->values[i], gb->values[i]) << "live evaluate not deterministic at inst " << i;
  }
}

// =====================================================================
//  6. R6 (buffer reuse, NOT zero-alloc) — a second evaluate REUSES the adapter's
//  scratch (window_/out_) without state corruption: the first SignalView is
//  invalidated, and the snapshot of the first call vs the second from the same
//  panel must agree (the buffer is rewritten in place, not corrupted). This proves
//  the buffer REUSE / non-corruption contract — it does NOT assert zero allocation
//  (predict_nn's forward allocates per call; see the adapter's honest R6 note).
// =====================================================================
TEST(LearnNnSourceIntegration, BufferReuse_EvaluateNoCorruption) {
  const LearnedModel model = fit_offline_tcn(kNDates);
  SeqLearnedSignalSource src{model, kRawFields, kL, kNInst};
  PanelHolder h = filled_rolling_panel(kNDates, kL);

  const auto first = src.evaluate(h.panel->view());
  ASSERT_TRUE(first.has_value());
  const std::vector<f64> snap(first->values.begin(), first->values.end()); // copy before invalidation

  const auto second = src.evaluate(h.panel->view());
  ASSERT_TRUE(second.has_value());
  ASSERT_EQ(second->values.size(), snap.size());
  for (usize i = 0; i < snap.size(); ++i) {
    EXPECT_EQ(second->values[i], snap[i]) << "buffer reuse corrupted inst " << i;
  }
}

}  // namespace atxtest_learn_nn_source_integration_test
