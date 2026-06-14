#pragma once

// atx::engine::learn — SeqFeatureSpec + SequenceTensor + build_sequences (S5-1).
//
// =====================================================================
//  What this header is
// =====================================================================
//  The bridge from the as-built tabular `learn::FeatureMatrix` (a PIT
//  (date x instrument) x feature tensor + forward labels) to the
//  (sample x time x feature) layout a deep-learning sequence model wants. The
//  ONLY new thing this unit adds is the WINDOWING: for each anchor row (date t,
//  instrument i) it packs that instrument's TRAILING feature rows [t-L+1 .. t]
//  into one sample. Every feature value, every label, and every (date,inst)
//  provenance is REUSED verbatim from the FeatureMatrix — nothing is recomputed.
//
// =====================================================================
//  The two load-bearing invariants (inherited from FeatureMatrix, M1/M2)
// =====================================================================
//  Trailing / no look-ahead (R2). A sample's window is [t-L+1, t] of one
//  instrument. It is TRAILING BY CONSTRUCTION: the builder NEVER reads a row
//  whose date > t. Adding later dates to the source FeatureMatrix can therefore
//  never change an earlier anchor's window — the truncation-invariance test pins
//  this. This is the property a sequence model must not violate.
//
//  Deterministic emit order (R1). Samples are emitted in ascending (anchor date
//  t, instrument i) order — a single nested loop, no map iteration on the emit
//  path. The flat backing store `x` is packed in ascending (sample, time l,
//  feature f) order. The result is run-to-run byte-identical.
//
// =====================================================================
//  Window completeness & the NaN / no-survivorship policy (M8)
// =====================================================================
//  A window is COMPLETE iff t+1 >= L (no date underflow) AND every one of the L
//  cells (t-L+1, i) .. (t, i) is an emitted FeatureMatrix row (has_row). An
//  instrument that drops out of the universe mid-window leaves a GAP — that is
//  an incomplete window. We NEVER zero-pad and NEVER borrow another instrument's
//  rows (no survivorship peek): the missing history is simply absent.
//
//   - drop_incomplete == true  : an incomplete anchor emits NO sample.
//   - drop_incomplete == false : it emits a sample whose window is filled with
//                                quiet NaN ("no opinion"), sample_valid = 0, and
//                                the labels still carried from the anchor row
//                                (the anchor is an emitted row, so its label /
//                                provenance are valid).
//
//  NaN is carried VERBATIM. A feature cell that is NaN in fm.X stays NaN in the
//  window and forces sample_valid = 0; it is NEVER replaced by 0. sample_valid
//  mirrors FeatureMatrix.row_valid semantics: 1 iff the anchor row is valid AND
//  the whole packed window is finite.
//
// Construction is a COLD path (once per training window), so std::vector
// allocation is fine (M7). The build logic lives in src/learn/sequence_features.cpp.

#include <vector> // std::vector (owned dense storage)

#include "atx/core/error.hpp" // Result, Ok, Err, ErrorCode
#include "atx/core/types.hpp" // f64, u8, usize

#include "atx/engine/learn/feature_matrix.hpp" // FeatureMatrix (the consumed seam)
#include "atx/engine/learn/nn/tensor.hpp"      // nn::Seq3 (the view type)

namespace atx::engine::learn {

// ===========================================================================
//  SeqFeatureSpec — the declarative recipe for one SequenceTensor build.
//
//  lookback (L)    : trailing window depth in dates. Must be >= 1. L == 1
//                    degenerates to the tabular FeatureMatrix (one row per
//                    anchor). The default 32 is a typical sequence length.
//  drop_incomplete : an anchor with < L available trailing rows (insufficient
//                    history, or a gap in the instrument's row presence) emits
//                    NO sample when true; a NaN-filled "no opinion" window with
//                    sample_valid = 0 when false.
// ===========================================================================
struct SeqFeatureSpec {
  atx::usize lookback{32};
  bool drop_incomplete{true};
};

// ===========================================================================
//  SequenceTensor — the immutable, deterministic, PIT (sample x time x feature)
//  tensor + per-horizon labels + (date,inst) provenance.
//
//  x is the flat Seq3 backing store: idx(s, l, f) = (s * L + l) * F + f, length
//  n_samples * lookback * n_features. y[h] is one label per sample (the
//  horizon-h forward return carried from the anchor row). date_of / inst_of map
//  a sample back to its anchor (date t, instrument i), in ascending order.
//  sample_valid[s] == 1 iff the anchor row was valid AND the whole window is
//  finite (mirrors FeatureMatrix.row_valid; NaN is NOT coerced).
// ===========================================================================
struct SequenceTensor {
  std::vector<atx::f64> x;              // [n_samples * lookback * n_features], Seq3 store
  std::vector<std::vector<atx::f64>> y; // y[h] = [n_samples] anchor-row label at horizon h
  std::vector<atx::usize> date_of;      // sample -> anchor date t   (ascending)
  std::vector<atx::usize> inst_of;      // sample -> anchor instrument i
  std::vector<atx::u8> sample_valid;    // 1 iff anchor valid AND window all-finite

  atx::usize n_samples{0};
  atx::usize lookback{0};   // L
  atx::usize n_features{0}; // F (== fm.n_features)

  // A non-owning (n_samples x lookback x n_features) view over x. The returned
  // Seq3 must not outlive this SequenceTensor (it aliases x). INVARIANT:
  // x.size() == n_samples * lookback * n_features.
  [[nodiscard]] nn::Seq3 view() const noexcept {
    return nn::Seq3{x, n_samples, lookback, n_features};
  }
};

// ===========================================================================
//  build_sequences — assemble the PIT trailing-window SequenceTensor from a
//  FeatureMatrix.
//
//  Sample at anchor (date t, instrument i) packs feature rows [t-L+1 .. t] of
//  instrument i. TRAILING by construction (R2): NEVER reads a row with date > t.
//  Emitted in ascending (anchor date t, instrument i) order (R1). Reuses the
//  FeatureMatrix labels + (date,inst) provenance; adds ONLY the windowing.
//
//  Errors (expected failures, travel in the Result):
//    - spec.lookback == 0  => Err(InvalidArgument)  (L must be >= 1)
//    - fm.n_features == 0   => Err(InvalidArgument)  (no features to window)
// ===========================================================================
[[nodiscard]] atx::core::Result<SequenceTensor>
build_sequences(const FeatureMatrix &fm, const SeqFeatureSpec &spec);

} // namespace atx::engine::learn
