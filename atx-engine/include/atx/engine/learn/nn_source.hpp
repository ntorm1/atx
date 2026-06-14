#pragma once

// atx::engine::learn — SeqLearnedSignalSource + the NN library bridge (p2 S5-5a).
//
// =====================================================================
//  What this header is (the S5 integration capstone — it WIRES the seams)
// =====================================================================
//  Two things, both thin adapters over the already-built S5 NN pieces:
//
//    * SeqLearnedSignalSource — the LIVE ISignalSource adapter for a fitted NN
//      sequence model (kind Tcn|Gru|Attn). It is the sequence analog of
//      LearnedSignalSource (learned_source.hpp): per evaluate(PanelView) it feeds
//      a TRAILING WINDOW of the panel (L dates x F raw fields) to predict_nn, one
//      score per instrument. The ONE delicate thing is the window ORDERING: the
//      training convention (SequenceTensor, S5-1) is window position l=0 = OLDEST,
//      l=L-1 = NEWEST, while PanelView row 0 = NEWEST — so the live build REVERSES
//      the row axis (see evaluate() in the .cpp). The adapter's OWN scratch
//      (window_/out_) is pre-sized in the ctor and reused — the adapter adds no
//      per-evaluate allocation. NOTE: predict_nn itself rebuilds the seed-ensemble
//      (factory_from_payload + reload member states) and allocates its forward
//      activations PER CALL (the substrate's Module::forward returns MatX by value),
//      so each in-universe instrument's forward heap-allocates — out of this
//      adapter's control. A fully allocation-free live NN forward (a pre-built
//      ensemble + pre-sized activation scratch) is a recorded substrate-level lift.
//      This mirrors the honest per-call-cost note VmSignalSource carries. It also
//      mirrors LearnedSignalSource's raw-field-only contract: a model whose features
//      were built from pool alphas is REJECTED at construction (pool-alpha features
//      are not reconstructable from a bare PanelView).
//
//    * nn_to_candidate — the NN -> library bridge: a stack_to_candidate-style
//      synthesizer that runs a fitted NN model over a SequenceTensor's samples,
//      scatters per-(date,instrument) scores into a position stream, derives the
//      per-date pnl, computes combine::AlphaMetrics, hashes the streams, and fills
//      a library::AlphaCandidate (expr_source "learned:tcn"/"learned:gru"/
//      "learned:attn"/"learned:ae"). The caller hands the candidate to
//      Library::admit. It REUSES predict_nn / predict_ae / compute_metrics /
//      hash_bytes verbatim — no new model or metric logic.
//
// =====================================================================
//  Determinism (R1) / causality (R2) / allocation (R6)
// =====================================================================
//  R1: predict_nn / predict_ae are byte-deterministic; nn_to_candidate folds the
//  samples ascending; no RNG, no SIMD. R2: the live window is trailing + causal
//  (it reads only the panel's sealed rows), and the offline window is trailing by
//  construction (S5-1) — the integration test pins the two agree bit-for-bit and
//  that the live score is invariant to bars after the anchor date. R6 (scoped
//  honestly): window_/out_ are pre-sized in the ctor and reused, so the ADAPTER
//  adds no per-evaluate allocation — but predict_nn's per-instrument forward DOES
//  allocate (MatX by value; see the SeqLearnedSignalSource note above), so
//  evaluate() is not globally allocation-free; the recorded lift is a pre-built,
//  pre-sized-activation NN forward at the substrate level.
//
// Header = API; the logic (predict dispatch, window build, candidate synth) lives
// in src/learn/nn_source.cpp.

#include <string> // std::string (raw field names, expr_source)
#include <vector> // std::vector (owned model params + source scratch)

#include "atx/core/error.hpp" // Result
#include "atx/core/types.hpp" // f64, u64, usize

#include "atx/engine/learn/learned_source.hpp"    // LearnedModel, ModelKind, predict_nn
#include "atx/engine/learn/sequence_features.hpp" // SequenceTensor
#include "atx/engine/library/library.hpp"         // library::AlphaCandidate
#include "atx/engine/loop/panel_types.hpp"        // PanelView, PanelField
#include "atx/engine/loop/signal_source.hpp"      // ISignalSource, SignalView

namespace atx::engine::learn {

// ===========================================================================
//  SeqLearnedSignalSource — the live ISignalSource adapter for an NN sequence
//  model (kind Tcn|Gru|Attn).
//
//  evaluate(PanelView) builds, per in-universe instrument, the L*F trailing
//  window in the S5-1 time-major layout (idx(l,f) = l*F + f; l=0 OLDEST, l=L-1
//  NEWEST) and predicts via predict_nn. The window position l reads PanelView row
//  (L-1)-l (the reversal that aligns NEWEST-first storage with OLDEST-first
//  training). An instrument with < L history, or any non-present / non-finite cell
//  in its window, emits the NaN "no opinion" sentinel.
// ===========================================================================
class SeqLearnedSignalSource final : public atx::engine::ISignalSource {
public:
  /// Wrap a fitted NN sequence model. `raw_fields` are the F raw Panel field names
  /// (open/high/low/close/volume) the model's features were built from, in column
  /// order; `lookback` is L (the trailing window depth in dates); `universe_size`
  /// is the fixed signal length each evaluate() returns. PRECONDITIONS (ABORT at
  /// construction — a malformed deployment is a programmer error, not a runtime
  /// path): the model is an NN sequence kind (Tcn|Gru|Attn); raw_fields.size()
  /// equals the model's F (== nn.n_seq_features); lookback * F == the model's base
  /// dimension; every raw field name resolves to a PanelField. A pool-alpha-bearing
  /// model is unrepresentable here (raw_fields only) — see LearnedSignalSource.
  SeqLearnedSignalSource(LearnedModel model, std::vector<std::string> raw_fields,
                         atx::usize lookback, atx::usize universe_size);

  /// One blended score per live-universe instrument from the panel's trailing L*F
  /// window. Reads ONLY the sealed trailing rows (causal). Out-of-universe / non-
  /// finite-window / insufficient-history instruments emit NaN. Returns a SignalView
  /// borrowing out_ (valid only until the next evaluate() on this source). The
  /// adapter's own scratch (window_/out_) is pre-sized in the ctor and reused, so
  /// evaluate() adds no per-call allocation OF ITS OWN — but each in-universe
  /// instrument's predict_nn forward allocates its activations (MatX by value; the
  /// substrate-level residual noted on the class), so evaluate() is not globally
  /// allocation-free.
  [[nodiscard]] atx::core::Result<SignalView> evaluate(PanelView panel) override;

  /// The trailing DEPTH the loop must provision below the current row: lookback_-1
  /// (the window spans L rows total — the current row plus lookback_-1 history).
  [[nodiscard]] atx::usize max_lookback() const noexcept override;

private:
  /// Read one raw field cell at row_from_newest for instrument i (dispatch over the
  /// resolved PanelField slot).
  [[nodiscard]] static atx::f64 read_field(PanelView panel, PanelField f, atx::usize row,
                                           atx::usize inst);

  /// Build the L*F window for instrument `inst` into window_ (time-major, l=0
  /// OLDEST). Returns true iff every cell is present AND finite (else the caller
  /// emits "no opinion"). PRECONDITION: panel.rows() >= lookback_.
  [[nodiscard]] bool fill_window(PanelView panel, atx::usize inst);

  LearnedModel model_;                  // immutable fitted NN parameters (kind Tcn|Gru|Attn)
  std::vector<PanelField> fields_;      // resolved raw-field slots (column order; cold-path)
  atx::usize lookback_;                 // L (trailing window depth in dates)
  atx::usize universe_;                 // fixed signal length per evaluate
  atx::usize n_features_;               // F (== fields_.size() == model_.nn.n_seq_features)
  std::vector<atx::f64> window_;        // [L*F] window scratch (reused; no per-evaluate alloc)
  std::vector<atx::f64> out_;           // [universe_] emitted cross-section (borrowed by SignalView)
};

// ===========================================================================
//  NnCandidate — a synthesized library candidate + the streams it borrows.
//
//  pnl / pos_flat are the OWNING backing buffers the candidate's spans point into;
//  they MUST outlive any Library::admit(candidate) call (the §0.3 dangling-span
//  discipline — admit reads the spans before staging a copy). Mirrors S5's
//  StackCandidate.
// ===========================================================================
struct NnCandidate {
  std::vector<atx::f64> pnl;      // [n_dates] per-date realized pnl
  std::vector<atx::f64> pos_flat; // [n_dates * n_instruments] period-major, inst-minor
  library::AlphaCandidate candidate;
};

// ===========================================================================
//  nn_to_candidate — synthesize a library::AlphaCandidate for a fitted NN model
//  over a SequenceTensor (the stack_to_candidate analog).
//
//  Per sample s (ascending) with sample_valid: score = predict for the window;
//  pos_flat[date_of[s]*n_inst + inst_of[s]] = score; pnl[date_of[s]] += score *
//  seq.y[0][s] when that label is finite. Then compute_metrics over the streams,
//  hash_bytes over (pnl ++ pos_flat) for canon_hash, and a Provenance with
//  expr_source = "learned:" + kind_tag(model.kind). The prediction path is
//  kind-dispatched: Tcn|Gru|Attn -> predict_nn over the L*F window; Autoencoder ->
//  predict_ae over the trailing-step F-dim vector (the AE input is NOT the window —
//  it is the newest cross-sectional feature row, x[(s*L + (L-1))*F .. +F)).
//  n_dates = max(date_of)+1; n_instruments is the caller's universe size. `seed`
//  is recorded in the provenance only (no RNG is consulted — the synthesis is
//  deterministic). PURE in (model, seq); allocates its own streams (a cold path).
// ===========================================================================
[[nodiscard]] NnCandidate nn_to_candidate(const LearnedModel &model, const SequenceTensor &seq,
                                          atx::usize n_instruments, atx::u64 seed);

} // namespace atx::engine::learn
