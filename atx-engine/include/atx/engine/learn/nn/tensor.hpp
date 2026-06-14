#pragma once

// atx::engine::learn::nn — Seq3, a non-owning (batch x time x feature) view.
//
// =====================================================================
//  What this header is
// =====================================================================
//  The minimal value type a sequence model trains on: a flat (B x T x F) tensor
//  addressed in row-major-by-(batch, time) order. It OWNS nothing — it aliases a
//  caller-provided backing store (a std::vector<f64> living in SequenceTensor).
//  This keeps the "windowing" output and its "view" separable: the owner holds
//  the bytes, the view names the shape.
//
//  Layout (R1, ascending-index addressing). The element at (batch b, time t,
//  feature f) lives at flat index (b * T + t) * F + f. The whole tensor is
//  contiguous and dense: B*T*F elements, no padding, no stride table. Iterating
//  b, then t, then f in ascending order walks the buffer front-to-back — the
//  same deterministic order the builder packs it in.
//
//  Non-owning by design (agent profile: weakest sufficient type). Seq3 carries a
//  std::span<const f64>; it must NOT outlive the buffer it views. The owner
//  (SequenceTensor) hands out a fresh Seq3 from its .view() each call, so the
//  view's lifetime is naturally scoped to a single use.

#include <span> // std::span (non-owning aliased storage)

#include "atx/core/types.hpp" // f64, usize

namespace atx::engine::learn::nn {

// ===========================================================================
//  Seq3 — a non-owning (batch x time x feature) view over flat, dense,
//  row-major-by-(batch, time) f64 storage.
//
//  INVARIANT: data.size() == B * T * F. The caller (SequenceTensor::view)
//  establishes this; Seq3 itself does not allocate or resize.
// ===========================================================================
struct Seq3 {
  std::span<const atx::f64> data; // length = B*T*F; idx(b,t,f) = (b*T + t)*F + f
  atx::usize B{0};                // batch / sample count
  atx::usize T{0};                // time / lookback depth
  atx::usize F{0};                // feature count

  // Element at (batch b, time t, feature f). Ascending-index addressing (R1):
  // contiguous walk of b, then t, then f matches the builder's pack order.
  // PRECONDITION: b < B, t < T, f < F (the span is exactly B*T*F long). noexcept
  // — a violated precondition is a programming error, surfaced by the span's own
  // bounds in a checked build, not a recoverable Result.
  [[nodiscard]] atx::f64 at(atx::usize b, atx::usize t, atx::usize f) const noexcept {
    return data[(b * T + t) * F + f];
  }
};

} // namespace atx::engine::learn::nn
