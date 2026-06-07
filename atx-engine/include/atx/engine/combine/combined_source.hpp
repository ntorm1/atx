#pragma once

// atx::engine::combine — CombinedSignalSource: the mega-alpha as an ISignalSource
// (P4-5, Sprint 4a CAPSTONE).
//
// ===========================================================================
//  What this unit is — the payoff
// ===========================================================================
//  CombinedSignalSource wraps the pool's constituent signal sources + a FROZEN
//  Combination (the per-alpha blend weights fit OOS-safely by P4-4) and presents
//  the blended "mega-alpha" as a single ISignalSource. Because it satisfies the
//  Phase-2 seam contract verbatim, it drops into the EXISTING BacktestLoop with
//  ZERO loop changes — the seam Phase-2 explicitly anticipated (signal_source.hpp:
//  "Phase-4's mega-alpha combiner will plug in the same way"). This source only
//  APPLIES the frozen Combination; the OOS-safe fit happened in P4-4.
//
// ===========================================================================
//  Two forms — and which one this is (§0-C reconciliation)
// ===========================================================================
//  §0-C records TWO forms of the combined source:
//    1. THIS unit — the frozen §8 `std::vector<ISignalSource*>` multi-source
//       blend: each constituent is evaluated independently over the panel and the
//       per-instrument cross-sections are blended. This is the form BUILT here and
//       is exactly the §8 P4-5 data structure / ctor.
//    2. PRODUCTION RESIDUAL (NOT built here) — a single `compile_batch` Program
//       adapter (CSE-shared) that evaluates ALL constituents in ONE VM pass and
//       whose max_lookback() forwards Program::required_lookback. That single-
//       Program path overlaps VmSignalSource and is a documented §0-C optimization;
//       it will be lifted to the ROADMAP at the 4a close. We ship the
//       vector<ISignalSource*> form exactly as the frozen ctor specifies, so
//       max_lookback() here is the MAX over the constituents' own max_lookback().
//
// ===========================================================================
//  Blend semantics (implement EXACTLY — the ambiguities are resolved + documented)
// ===========================================================================
//  evaluate(panel) evaluates each constituent over `panel` (propagating any Err)
//  and blends their current-date cross-sections into ONE signal of length
//  n_instruments. Let V_k = { i : s_i[k] is not NaN } be the surviving constituents
//  at instrument k. The EXHAUSTIVE CombineMethod switch (NO default) selects:
//
//   * LINEAR methods (EqualWeight, IcWeighted, ShrinkageMv, BoundedRegression):
//        out[k] = (Σ_{i∈V_k} w_i·s_i[k]) / (Σ_{i∈V_k} |w_i|)
//     The denominator is the SURVIVING GROSS Σ_{i∈V_k}|w_i| — the GROSS-PRESERVING
//     RENORM. WHY divide by the gross (not Σw_i)?
//       (a) it keeps the blend SCALE stable when some constituents are NaN at a
//           cell (the surviving weights are re-normalized to their own gross), and
//       (b) it is well-defined for a DOLLAR-NEUTRAL combo where Σw_i = 0 but the
//           gross Σ|w_i| > 0 — no div-by-zero (pinned by the dollar-neutral test).
//     RELATIONSHIP TO P4-4's NORMALIZATION: P4-4 gross-normalizes the Combination
//     so Σ|w| == 1 (combiner.hpp renorm_abs_sum). When NO constituent is NaN this
//     denominator is therefore 1, and the blend reduces to the plain weighted sum
//     Σ_i w_i·s_i[k]. The renorm only ever rescales when some constituent drops out.
//     DEGENERATE: V_k empty (all NaN) OR Σ_{i∈V_k}|w_i| == 0 -> out[k] = NaN
//     ("no opinion" — there is no well-defined blend).
//     SINGLE CONSTITUENT: out[k] = (w_0·s_0[k]) / |w_0| = sign(w_0)·s_0[k] — the
//     alpha sign-normalized by its weight (a negative weight flips the sign; the
//     magnitude is unchanged). A zero-weight single constituent -> NaN (zero gross).
//
//   * RankAverage: combine in RANK space. Each constituent's cross-section s_i is
//     mapped to its cross-sectional ORDINAL-PERCENTILE rank (r/(n_valid-1),
//     ascending value, NaN-last, stable index tie-break — the Phase-3 CsRank
//     convention, cs_ops.hpp cs_rank_row: a singleton valid set -> 0.5). Then
//        out[k] = mean_{i∈V_k} rank_i[k]
//     (the rank of a NaN cell is NaN, so it is excluded from the per-cell mean).
//     V_k empty -> out[k] = NaN. The Combination weights are IGNORED by RankAverage
//     (an equal-weight mean in rank space, per the §8 spec).
//
//  RANK SOURCE: a small LOCAL deterministic ordinal-percentile rank is implemented
//  below rather than calling alpha::detail::cs_rank_row. That kernel lives in the
//  alpha::detail namespace shared by the oracle/VM differential test (an ODR-fenced
//  TU); reaching into it from combine would couple this header to the VM internals,
//  and §8 explicitly permits "implement a small deterministic ordinal rank" when a
//  clean reuse is not available. The local kernel reproduces the cs_rank_row
//  convention EXACTLY (same r/(n-1) percentile, same NaN-last + stable tie-break),
//  which is what the differential rank test pins.
//
// ===========================================================================
//  Seam / determinism / lifetime contracts
// ===========================================================================
//  * PURE in `panel` (the ISignalSource contract): each constituent is pure in the
//    panel, so the blend is too. No hidden state mutated by evaluate() except the
//    owned out_ buffer the returned SignalView borrows.
//  * NON-OWNING constituents: sources_ holds raw ISignalSource* — the CALLER owns
//    each constituent's lifetime (and must keep them alive for this source's
//    lifetime). Mirrors the loop's non-owning collaborator discipline.
//  * SignalView borrow: evaluate() returns a SignalView over out_ (the owned blend
//    buffer) — constructed EXACTLY as VmSignalSource does
//    (Ok(SignalView{std::span<const f64>{out_}})). out_ is sized ONCE per shape
//    change (resize only when it grows), so the apply path does NO per-cell heap
//    allocation (the seam forbids hot-path alloc).
//  * DETERMINISM (§3.2): blend reductions run in FIXED constituent order (ascending
//    i) and FIXED instrument order (ascending k); RankAverage uses the stable
//    ordinal tie-break. No RNG. Same panel + same Combination -> byte-identical out_.

#include <algorithm> // std::max, std::stable_sort
#include <cmath>     // std::isnan, std::fabs
#include <limits>    // std::numeric_limits (the "no opinion" quiet-NaN sentinel)
#include <span>      // std::span (SignalView borrow)
#include <utility>   // std::move
#include <vector>    // std::vector (owned blend + scratch buffers)

#include "atx/core/error.hpp" // Result, Ok, ATX_TRY
#include "atx/core/macro.hpp" // ATX_ASSERT (constituent length agreement)
#include "atx/core/types.hpp" // usize, f64

#include "atx/engine/combine/combiner.hpp"   // Combination, CombineMethod
#include "atx/engine/loop/signal_source.hpp" // ISignalSource, SignalView, PanelView

namespace atx::engine::combine {

namespace detail {

// Quiet NaN "no opinion" sentinel (matches the SignalView NaN contract / the
// VmSignalSource kNoOpinion convention).
inline constexpr atx::f64 kCombineNaN = std::numeric_limits<atx::f64>::quiet_NaN();

// Cross-sectional ordinal-percentile rank of one constituent's cross-section `s`,
// written into `out` (same length). Reproduces the Phase-3 CsRank convention
// (cs_ops.hpp cs_rank_row) EXACTLY: ranks are computed over the VALID set (non-NaN
// cells), ascending by value, ties broken by ascending instrument index (a stable
// sort by value preserves index order). Rank r (0-based) of n_valid maps to
// r/(n_valid-1); a SINGLETON valid set -> 0.5 (centred, avoids 0/0). NaN cells are
// NaN-last == out-of-set: they receive a NaN rank (so the per-cell mean excludes
// them). Empty valid set -> every cell NaN. Order-fixed -> deterministic.
inline void cs_percentile_rank(std::span<const atx::f64> s, std::vector<atx::usize> &order_scratch,
                               std::span<atx::f64> out) {
  const atx::usize n = s.size();
  order_scratch.clear();
  for (atx::usize i = 0U; i < n; ++i) {
    out[i] = kCombineNaN; // default: out-of-set (NaN) unless ranked below
    if (!std::isnan(s[i])) {
      order_scratch.push_back(i); // ascending instrument index (stable tie-break)
    }
  }
  const atx::usize nv = order_scratch.size();
  if (nv == 0U) {
    return; // no valid cell -> all NaN
  }
  // Stable sort by value; equal values keep their ascending-index order -> the
  // deterministic ordinal tie-break (identical to cs_ops.hpp cs_rank_row).
  std::stable_sort(order_scratch.begin(), order_scratch.end(),
                   [&](atx::usize a, atx::usize b) { return s[a] < s[b]; });
  for (atx::usize r = 0U; r < nv; ++r) {
    const atx::f64 pct =
        (nv == 1U) ? 0.5 : static_cast<atx::f64>(r) / static_cast<atx::f64>(nv - 1U);
    out[order_scratch[r]] = pct;
  }
}

} // namespace detail

// ===========================================================================
//  CombinedSignalSource — the frozen §8 vector<ISignalSource*> mega-alpha.
//
//  final ISignalSource: virtual dtor/copy/move come from the base (the seam's
//  concern — we do NOT add our own dtor). Constituents are NON-OWNING (caller
//  owns lifetime). out_ is the owned blend buffer the returned SignalView borrows.
//
//  NOT REENTRANT / NOT THREAD-SAFE: evaluate() mutates per-instance scratch (out_,
//  cross_, rank_*, order_scratch_), so at most ONE evaluate() may be in flight per
//  instance — concurrent calls on the same instance race. The returned SignalView
//  borrows out_ and is valid ONLY until the next evaluate() on this instance (the
//  next call overwrites out_) — the same single-in-flight-borrow property as
//  VmSignalSource. The loop calls one source serially per rebalance, so this holds.
// ===========================================================================
class CombinedSignalSource final : public ISignalSource {
public:
  /// Wrap the constituent sources + a frozen Combination + the blend method.
  /// NON-OWNING constituents (the caller keeps each alive for this source's
  /// lifetime). noexcept: moves the vectors/combo in (no allocation, no throw).
  /// PRECONDITION: combo.weights.size() == sources.size() (the blend reads
  /// weights[i] for every constituent i; a length disagreement is a wiring bug that
  /// would read OOB on the apply path — ABORTS in debug, fail-closed).
  CombinedSignalSource(std::vector<ISignalSource *> sources, Combination combo,
                       CombineMethod method) noexcept
      : sources_{std::move(sources)}, combo_{std::move(combo)}, method_{method} {
    // ATX_ASSERT aborts (noexcept-compatible): the per-alpha weight vector must be
    // index-aligned to the constituents (combiner.hpp documents Σ|w|=1 over exactly
    // pool.size() == sources_.size() weights). RankAverage ignores the weights, but
    // the invariant is cheap and uniform, so it is asserted unconditionally.
    ATX_ASSERT(combo_.weights.size() == sources_.size());
  }

  /// Evaluate each constituent over `panel`, blend into ONE cross-sectional signal,
  /// and return a SignalView borrowing the owned out_ buffer (valid until the next
  /// evaluate()). PURE in `panel`. Propagates any constituent's Err verbatim.
  /// NOT REENTRANT: one in-flight evaluate() per instance; the returned SignalView
  /// is invalidated by the next evaluate() on this instance (see the class note).
  [[nodiscard]] atx::core::Result<SignalView> evaluate(PanelView panel) override {
    const atx::usize m = sources_.size();
    // Evaluate every constituent; cache the borrowed cross-sections. Each borrow is
    // valid until the NEXT evaluate() on that SAME source — distinct sources here,
    // so all m borrows are simultaneously live for this blend (then consumed).
    cross_.resize(m);
    atx::usize n_instruments = 0U;
    for (atx::usize i = 0U; i < m; ++i) {
      ATX_TRY(const SignalView sv, sources_[i]->evaluate(panel));
      cross_[i] = sv.values;
      if (i == 0U) {
        n_instruments = sv.values.size();
      }
      // The loop guarantees every constituent shares the universe length; a
      // disagreement is a wiring bug (fail loud in debug).
      ATX_ASSERT(sv.values.size() == n_instruments);
    }

    if (out_.size() < n_instruments) {
      out_.resize(n_instruments); // grow-only: steady-state no realloc (seam contract)
    }

    // EXHAUSTIVE switch over CombineMethod — NO default (a new enumerator is a
    // compile error, not a silent linear fall-through).
    switch (method_) {
    case CombineMethod::EqualWeight:
    case CombineMethod::IcWeighted:
    case CombineMethod::ShrinkageMv:
    case CombineMethod::BoundedRegression:
      blend_linear(m, n_instruments);
      break;
    case CombineMethod::RankAverage:
      blend_rank(m, n_instruments);
      break;
    }
    return atx::core::Ok(SignalView{std::span<const atx::f64>{out_.data(), n_instruments}});
  }

  /// MAX over the constituents' own max_lookback() (so the loop sizes its
  /// RollingPanel for the deepest constituent). 0 for an empty source. noexcept.
  [[nodiscard]] atx::usize max_lookback() const noexcept override {
    atx::usize lb = 0U;
    for (const ISignalSource *s : sources_) {
      lb = std::max(lb, s->max_lookback());
    }
    return lb;
  }

private:
  /// Linear blend with the GROSS-PRESERVING renorm (see the header):
  ///   out[k] = (Σ_{i∈V} w_i·s_i[k]) / (Σ_{i∈V} |w_i|),  V = non-NaN constituents.
  /// V empty OR zero surviving gross -> out[k] = NaN. Fixed constituent + instrument
  /// order (determinism). Reads combo_.weights[i] (P4-4 gross-normalized to Σ|w|=1).
  void blend_linear(atx::usize m, atx::usize n_instruments) noexcept {
    for (atx::usize k = 0U; k < n_instruments; ++k) {
      atx::f64 num = 0.0;
      atx::f64 gross = 0.0;
      for (atx::usize i = 0U; i < m; ++i) {
        const atx::f64 v = cross_[i][k];
        if (std::isnan(v)) {
          continue; // NaN constituent skipped per-cell (re-normalized below)
        }
        const atx::f64 w = combo_.weights[i];
        num += w * v;
        gross += std::fabs(w);
      }
      out_[k] = (gross > 0.0) ? (num / gross) : detail::kCombineNaN;
    }
  }

  /// RankAverage blend: out[k] = mean over non-NaN constituents of rank_i[k], where
  /// rank_i is the cross-sectional ordinal-percentile rank of constituent i (NaN
  /// cells -> NaN rank, excluded from the mean). V empty -> NaN. Weights IGNORED.
  void blend_rank(atx::usize m, atx::usize n_instruments) {
    rank_scratch_.resize(n_instruments);
    // Accumulate the per-cell rank sum + valid count across constituents (fixed
    // order). rank_sum_/rank_cnt_ are reused scratch (sized once per shape).
    rank_sum_.assign(n_instruments, 0.0);
    rank_cnt_.assign(n_instruments, 0U);
    for (atx::usize i = 0U; i < m; ++i) {
      detail::cs_percentile_rank(cross_[i], order_scratch_,
                                 std::span<atx::f64>{rank_scratch_.data(), n_instruments});
      for (atx::usize k = 0U; k < n_instruments; ++k) {
        const atx::f64 rk = rank_scratch_[k];
        if (!std::isnan(rk)) {
          rank_sum_[k] += rk;
          ++rank_cnt_[k];
        }
      }
    }
    for (atx::usize k = 0U; k < n_instruments; ++k) {
      out_[k] = (rank_cnt_[k] > 0U) ? (rank_sum_[k] / static_cast<atx::f64>(rank_cnt_[k]))
                                    : detail::kCombineNaN;
    }
  }

  std::vector<ISignalSource *> sources_; // NON-OWNING constituents (caller owns lifetime)
  Combination combo_;                    // frozen blend (weights; [fit_begin,fit_end) inert here)
  CombineMethod method_;                 // linear-vs-rank selector (exhaustive switch)
  std::vector<atx::f64> out_;            // owned blend buffer; the SignalView borrows it

  // ---- reused scratch (sized once per shape; no per-cell heap alloc on apply) ---
  std::vector<std::span<const atx::f64>> cross_; // per-constituent borrowed cross-sections
  std::vector<atx::f64> rank_scratch_;           // one constituent's rank vector (RankAverage)
  std::vector<atx::usize> order_scratch_;        // sort-index scratch for the rank kernel
  std::vector<atx::f64> rank_sum_;               // per-cell Σ rank (RankAverage)
  std::vector<atx::usize> rank_cnt_;             // per-cell valid-constituent count (RankAverage)
};

} // namespace atx::engine::combine
