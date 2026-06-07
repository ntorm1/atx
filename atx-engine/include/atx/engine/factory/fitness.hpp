#pragma once

// atx::engine::factory — pool-aware fitness: the WorldQuant marginal-contribution
// score (S3-4, plan §4.6 + §0.6 / §0.7 / §0.8).
//
// ===========================================================================
//  What this unit is — the WQ thesis, made into a number
// ===========================================================================
//  The factory does NOT reward a candidate's standalone Sharpe. It rewards the
//  candidate's MARGINAL contribution to an already-diversified live pool, judged
//  OUT-OF-SAMPLE and DEFLATED by the running search trial count. A strong alpha
//  that merely duplicates a pool member adds nothing; a weaker alpha that is
//  uncorrelated with the pool genuinely expands the frontier. `raw` encodes that:
//
//      raw = wq * diversify * robust
//
//  where wq is the OOS WorldQuant fitness (combine::compute_metrics().fitness,
//  reused VERBATIM — no second convention), diversify = 1 − mean|corr-to-pool|
//  (F7), and robust is the sub-universe-stability ratio (§0.8). Admission (S3-6)
//  then gates on the DEFLATED Sharpe `dsr` (F4), which a higher trial count N
//  drives toward 0 — the anti-snooping lever.
//
// ===========================================================================
//  §0.6 — the missing corr-to-pool helper, and the DANGLING-SPAN hazard
// ===========================================================================
//  combine/ exposes only `pairwise_complete_corr(a, b) -> f64`; there is no
//  reusable corr-to-pool. This unit writes the public `corr_to_pool(candidate,
//  pool, Reduce)` over it: Reduce::Max (the gate-consistent max|corr| screen) and
//  Reduce::Mean (the diversification discount). CRITICAL: AlphaStore::pnl()
//  returns a span that ALIASES the backing vector and DANGLES after the next
//  insert() (store.hpp BORROW LIFETIME). corr_to_pool therefore reads the pool
//  WITHOUT mutating it; the caller (S3-6) must compute corr-to-pool BEFORE
//  inserting the candidate. See the SAFETY note on corr_to_pool.
//
// ===========================================================================
//  §0.7 — the as-built S1 signatures consumed here
// ===========================================================================
//  * combine::compute_metrics(pnl, positions_flat, n_instruments, book) -> the
//    AlphaMetrics POD whose `.fitness` IS the WQ term (turnover floor 0.125).
//  * eval::deflated_sharpe(sr, T, skew, exkurt, N, std::optional<var>) -> DsrResult
//    where N is the TRIAL COUNT. Moments: eval::skewness / eval::excess_kurtosis.
//  * eval::cpcv_folds(spans, cfg) -> per-fold {train_idx, test_idx}; fitness is
//    computed on the TEST partitions only (F3 OOS-only).
//
// ===========================================================================
//  §0.8 — sub-universe robustness is a RE-EVAL, not a re-score
// ===========================================================================
//  No scoring layer takes a universe argument. Robustness re-runs the candidate
//  through extract_streams against an ALTERNATE Panel built with a weaker
//  universe (a different UniversePolicy / mask). pool_aware_fitness takes the weak
//  panel as an optional borrow; when none is configured, robust = 1.0 (a clean,
//  documented degenerate — robustness neither rewards nor penalizes).
//
//  Header-only, every function inline; the fitness path is COLD (one call per
//  distinct candidate, never on the VM hot loop), so std::vector is fine.

#include <algorithm> // std::clamp
#include <cmath>     // std::abs, std::isnan, std::sqrt
#include <optional>  // std::optional (weak-universe panel; deflation var arg)
#include <span>      // std::span
#include <utility>   // std::move (OosAggregate hand-off)
#include <vector>    // std::vector (fold-sliced streams)

#include "atx/core/error.hpp" // Result, Ok, Err, ErrorCode
#include "atx/core/types.hpp" // atx::f64, atx::usize

#include "atx/engine/alpha/bytecode.hpp" // alpha::compile, alpha::Program
#include "atx/engine/alpha/panel.hpp"    // alpha::Panel, alpha::SignalSet
#include "atx/engine/alpha/streams.hpp"  // alpha::extract_streams, AlphaStreams
#include "atx/engine/alpha/vm.hpp"       // alpha::Engine
#include "atx/engine/combine/correlation.hpp" // combine::pairwise_complete_corr
#include "atx/engine/combine/metrics.hpp"     // combine::compute_metrics, AlphaMetrics
#include "atx/engine/combine/store.hpp"       // combine::AlphaStore, AlphaId
#include "atx/engine/eval/cpcv.hpp"           // eval::cpcv_folds, CpcvFold, LabelSpan
#include "atx/engine/eval/deflated_sharpe.hpp" // eval::deflated_sharpe, DsrResult
#include "atx/engine/eval/stats_ext.hpp"       // eval::skewness, eval::excess_kurtosis
#include "atx/engine/exec/execution_sim.hpp"   // exec::ExecutionSimulator
#include "atx/engine/factory/genome.hpp"       // factory::Genome
#include "atx/engine/loop/weight_policy.hpp"   // engine::WeightPolicy

namespace atx::engine::factory {

// =========================================================================
//  Reduce — how corr_to_pool folds the per-member |corr| over the pool.
//
//  Max  : max_j |corr(candidate, member_j)| — the gate-consistent screen
//         (AlphaGate rejects on this exact MAX, gate.hpp).
//  Mean : mean_j |corr(candidate, member_j)| — the diversification discount
//         (a candidate redundant with the WHOLE pool, not just its nearest
//         neighbour, is penalized in proportion).
// =========================================================================
enum class Reduce : atx::u8 { Max, Mean };

// =========================================================================
//  corr_to_pool — the marginal-correlation helper (§0.6), over the shared
//  pairwise-complete Pearson convention.
//
//  Returns max|corr| (Reduce::Max) or mean|corr| (Reduce::Mean) of the
//  candidate's PnL against every pool member's PnL. An EMPTY pool -> 0 (a
//  candidate is maximally diversifying against nothing — diversify = 1).
//
//  SAFETY: this function only READS the pool — it never inserts. AlphaStore::pnl()
//  returns a span aliasing the backing vector that DANGLES after the next
//  insert()/ingest_streams() (store.hpp §0.6). The caller (S3-6 admission) MUST
//  compute corr-to-pool on the candidate BEFORE inserting it into the pool; doing
//  it after would read freed memory. Each member span is consumed immediately
//  inside the loop (no span outlives one iteration), so the read is sound here.
// =========================================================================
[[nodiscard]] inline atx::f64 corr_to_pool(std::span<const atx::f64> candidate_pnl,
                                           const combine::AlphaStore &pool, Reduce reduce) noexcept {
  const atx::usize n = pool.n_alphas();
  if (n == 0U) {
    return 0.0;
  }
  atx::f64 acc = 0.0; // running max (init 0) or running sum (Mean), then /n
  for (atx::usize i = 0U; i < n; ++i) {
    // SAFETY: member aliases pool's backing vector; consumed in-iteration only,
    //         and the pool is never mutated here, so it cannot dangle (§0.6).
    const std::span<const atx::f64> member = pool.pnl(combine::AlphaId{static_cast<atx::u32>(i)});
    const atx::f64 c = combine::pairwise_complete_corr(candidate_pnl, member);
    const atx::f64 ac = std::abs(c);
    if (reduce == Reduce::Max) {
      acc = (ac > acc) ? ac : acc;
    } else {
      acc += ac;
    }
  }
  return (reduce == Reduce::Max) ? acc : acc / static_cast<atx::f64>(n);
}

// =========================================================================
//  FitnessReport — one candidate's scored result (plan §4.6 step 5).
//
//  wq            : OOS WorldQuant fitness, mean over CPCV TEST folds.
//  redundancy    : mean|corr-to-pool| of the OOS PnL (the diversification input).
//  diversify     : clamp(1 − redundancy, 0, 1) — the F7 marginal-contribution weight.
//  robust        : clamp(wq_on(weak_universe) / max(wq, eps), 0, 1); 1.0 if no
//                  weak universe is configured (documented degenerate, §0.8).
//  raw           : wq * diversify * robust — the signal the search maximizes.
//  dsr           : deflated Sharpe (F4) at the running trial count N — the
//                  admission statistic; higher N -> lower dsr.
//  haircut_sharpe: max(0, sharpe − SR*_N) — the selection-adjusted point estimate.
//
//  Trivial aggregate (Rule of Zero). Matches the fwd.hpp forward declaration.
// =========================================================================
struct FitnessReport {
  atx::f64 wq;
  atx::f64 redundancy;
  atx::f64 diversify;
  atx::f64 robust;
  atx::f64 raw;
  atx::f64 dsr;
  atx::f64 haircut_sharpe;
};

// =========================================================================
//  FitnessCfg — the knobs the search feeds the fitness call.
//
//  trial_count : N for eval::deflated_sharpe (F4). Every distinct candidate the
//                search scores increments it; a higher N lowers dsr.
//  cpcv        : the CPCV fold geometry (TEST folds are the OOS partitions).
//  book_size   : the notional divisor for turnover (1.0 when weights are already
//                gross-normalized fractions, the extract_streams convention).
// =========================================================================
struct FitnessCfg {
  atx::usize trial_count = 1;
  eval::CpcvConfig cpcv{};
  atx::f64 book_size = 1.0;
};

namespace detail {

// The aggregate OOS metrics produced by averaging compute_metrics().fitness/
// .sharpe over the CPCV TEST folds, plus the candidate's full realized OOS PnL
// stream (the diversification + deflation input).
struct OosAggregate {
  atx::f64 wq;     // mean fold WQ fitness
  atx::f64 sharpe; // mean fold ANNUALIZED Sharpe (de-annualized before deflation)
  // The candidate's FULL realized PnL stream (length == n_periods). Used at full
  // length for corr-to-pool (must match the pool members' stream length; the
  // shared structural index-0 ~0 is mean-centered away in Pearson — correlation.hpp
  // deliberately INCLUDES index 0). For the DEFLATION moments the structural zero
  // is dropped (see pool_aware_fitness: skew/kurtosis/T are taken over r[1..)).
  std::vector<atx::f64> oos_pnl;
};

// Slice a flat per-period stream by an ascending index set (one fold's TEST
// indices). `width` == 1 for the PnL stream; == n_instruments for positions.
[[nodiscard]] inline std::vector<atx::f64> slice_by_idx(std::span<const atx::f64> flat,
                                                        std::span<const atx::usize> idx,
                                                        atx::usize width) {
  std::vector<atx::f64> out;
  out.reserve(idx.size() * width);
  for (const atx::usize t : idx) {
    for (atx::usize j = 0U; j < width; ++j) {
      out.push_back(flat[t * width + j]);
    }
  }
  return out;
}

// Per-period positions for alpha 0, flat-packed [n_periods * n_instruments]
// (positions(0, t) is one contiguous cross-section; concatenate over periods).
[[nodiscard]] inline std::vector<atx::f64> positions_flat0(const alpha::AlphaStreams &strm) {
  const atx::usize periods = strm.n_periods();
  const atx::usize insts = strm.n_instruments();
  std::vector<atx::f64> out;
  out.reserve(periods * insts);
  for (atx::usize t = 0U; t < periods; ++t) {
    const std::span<const atx::f64> cs = strm.positions(0, t);
    out.insert(out.end(), cs.begin(), cs.end());
  }
  return out;
}

// Aggregate the OOS WQ fitness / Sharpe over the CPCV TEST folds of alpha 0's
// streams. F3: only TEST indices are scored — never in-sample. The causal VM is
// evaluated CONTIGUOUSLY (warm-up intact) then the realized PnL/positions are
// SLICED by each fold's test indices — equivalent precisely because nothing is
// fitted (§4.6). `wq`/`sharpe` are the MEAN over folds (the OOS-only estimate).
//
// The exposed `oos_pnl` is the candidate's FULL realized PnL stream MINUS the
// structural index-0 zero (combine's §0-F convention): every period is a TEST
// observation in at least one CPCV fold (the union of all test groups covers
// [0, N)), so the deduplicated "full OOS PnL" IS the whole stream. Using it (not
// the fold-concatenation, which repeats each period across overlapping folds)
// keeps T = the true OOS count for deflation and the corr/moment inputs honest.
[[nodiscard]] inline OosAggregate aggregate_oos(const alpha::AlphaStreams &strm,
                                                const std::vector<eval::CpcvFold> &folds,
                                                atx::usize n_instruments, atx::f64 book_size) {
  const std::span<const atx::f64> pnl0 = strm.pnl(0);
  const std::vector<atx::f64> pos0 = positions_flat0(strm);

  atx::f64 sum_wq = 0.0;
  atx::f64 sum_sharpe = 0.0;
  atx::usize n_valid = 0U;
  for (const eval::CpcvFold &fold : folds) {
    if (fold.test_idx.empty()) {
      continue;
    }
    const std::vector<atx::f64> test_pnl =
        slice_by_idx(pnl0, std::span<const atx::usize>{fold.test_idx}, 1U);
    const std::vector<atx::f64> test_pos =
        slice_by_idx(std::span<const atx::f64>{pos0}, std::span<const atx::usize>{fold.test_idx},
                     n_instruments);
    const combine::AlphaMetrics m =
        combine::compute_metrics(test_pnl, test_pos, n_instruments, book_size);
    // A degenerate fold (zero-variance / single-obs) yields NaN moments; skip it
    // from the mean rather than poison the aggregate with NaN.
    if (!std::isnan(m.fitness)) {
      sum_wq += m.fitness;
      sum_sharpe += std::isnan(m.sharpe) ? 0.0 : m.sharpe;
      ++n_valid;
    }
  }
  const atx::f64 inv = (n_valid == 0U) ? 0.0 : 1.0 / static_cast<atx::f64>(n_valid);
  // Full realized stream (length == n_periods) — see OosAggregate::oos_pnl.
  std::vector<atx::f64> oos_pnl(pnl0.begin(), pnl0.end());
  return OosAggregate{sum_wq * inv, sum_sharpe * inv, std::move(oos_pnl)};
}

// One label span per period: a point alpha's label is [t, t+1) (it informs only
// its own bar). This is the CPCV input that partitions the periods into folds.
[[nodiscard]] inline std::vector<eval::LabelSpan> point_label_spans(atx::usize n_periods) {
  std::vector<eval::LabelSpan> spans;
  spans.reserve(n_periods);
  for (atx::usize t = 0U; t < n_periods; ++t) {
    spans.push_back(eval::LabelSpan{t, t + 1U});
  }
  return spans;
}

// Compile + evaluate a genome over `panel` and extract its per-alpha streams.
// Single-thread Engine path (the correct, slower fallback; S3-5 swaps in the S2
// parallel path at the driver). Err propagates compile/eval/extract failures.
[[nodiscard]] inline atx::core::Result<alpha::AlphaStreams>
eval_streams(const Genome &cand, const alpha::Panel &panel, const WeightPolicy &policy,
             const exec::ExecutionSimulator &sim) {
  ATX_TRY(const alpha::Program prog, alpha::compile(cand.ast, cand.analysis));
  alpha::Engine engine{panel};
  ATX_TRY(const alpha::SignalSet ss, engine.evaluate(prog));
  return alpha::extract_streams(ss, policy, panel, sim);
}

} // namespace detail

// =========================================================================
//  pool_aware_fitness — the §4.6 marginal-contribution score (OOS + deflated).
//
//  (1) Eval the candidate ONCE over the full `panel` (causal VM, no look-ahead),
//      extract its PnL/position streams, and average combine::compute_metrics()
//      .fitness/.sharpe over the CPCV TEST folds -> OOS `wq`/`sharpe` (F3).
//  (2) redundancy = corr_to_pool(OOS PnL, pool, Mean); diversify = clamp(1−red,0,1) (F7).
//  (3) robust = clamp(wq_on(weak_panel)/max(wq,eps),0,1) when a weak universe Panel
//      is supplied; else 1.0 (§0.8 degenerate).
//  (4) raw = wq * diversify * robust.
//  (5) dsr = eval::deflated_sharpe(sharpe, T, skew, kurt, N=trial_count, nullopt) (F4).
//
//  `weak_panel` (optional borrow): the §0.8 alternate-universe Panel for the
//  robustness re-eval. nullptr/nullopt -> robust = 1.0. Borrows `panel`,
//  `policy`, `sim`, `pool` for the duration of the call (no ownership taken).
//  Returns Err only if the candidate fails to compile/evaluate/extract.
// =========================================================================
[[nodiscard]] inline atx::core::Result<FitnessReport>
pool_aware_fitness(const Genome &cand, const combine::AlphaStore &pool,
                   const alpha::Panel &panel, const WeightPolicy &policy,
                   const exec::ExecutionSimulator &sim, const FitnessCfg &cfg,
                   const alpha::Panel *weak_panel = nullptr) {
  // SAFETY (eps): the robustness ratio divides by wq; floor the denominator so a
  //               near-zero full-universe wq cannot blow the ratio to ±inf.
  constexpr atx::f64 kEps = 1e-12;

  // (1) full-universe eval -> OOS fold aggregate.
  ATX_TRY(const alpha::AlphaStreams strm, detail::eval_streams(cand, panel, policy, sim));
  const atx::usize insts = strm.n_instruments();
  const std::vector<eval::LabelSpan> spans = detail::point_label_spans(strm.n_periods());
  const std::vector<eval::CpcvFold> folds =
      eval::cpcv_folds(std::span<const eval::LabelSpan>{spans}, cfg.cpcv);
  const detail::OosAggregate agg = detail::aggregate_oos(strm, folds, insts, cfg.book_size);
  const atx::f64 wq = agg.wq;

  // (2) diversification discount (F7): mean |corr-to-pool| of the OOS PnL.
  const atx::f64 redundancy =
      corr_to_pool(std::span<const atx::f64>{agg.oos_pnl}, pool, Reduce::Mean);
  const atx::f64 diversify = std::clamp(1.0 - redundancy, 0.0, 1.0);

  // (3) sub-universe robustness (§0.8): re-eval on the weak-universe Panel.
  atx::f64 robust = 1.0; // degenerate default: no weak universe configured.
  if (weak_panel != nullptr) {
    ATX_TRY(const alpha::AlphaStreams weak_strm,
            detail::eval_streams(cand, *weak_panel, policy, sim));
    const atx::usize weak_insts = weak_strm.n_instruments();
    const std::vector<eval::LabelSpan> weak_spans =
        detail::point_label_spans(weak_strm.n_periods());
    const std::vector<eval::CpcvFold> weak_folds =
        eval::cpcv_folds(std::span<const eval::LabelSpan>{weak_spans}, cfg.cpcv);
    const detail::OosAggregate weak_agg =
        detail::aggregate_oos(weak_strm, weak_folds, weak_insts, cfg.book_size);
    const atx::f64 denom = (std::abs(wq) > kEps) ? wq : kEps;
    robust = std::clamp(weak_agg.wq / denom, 0.0, 1.0);
  }

  // (4) the raw search signal.
  const atx::f64 raw = wq * diversify * robust;

  // (5) deflation by the running trial count N (F4): higher N -> lower dsr.
  //
  // RECONCILIATION (§0.7): combine::compute_metrics().sharpe is ANNUALIZED
  // (sqrt(252)*mean/std), but eval::deflated_sharpe expects a PER-PERIOD Sharpe
  // (its variance-of-Sharpe estimator and finite-sample (T-1) correction are
  // per-observation). We therefore DE-ANNUALIZE the aggregate Sharpe by
  // sqrt(252) before deflation — feeding the annualized figure saturates PSR to
  // 1.0 and the trial-count lever never bites. skew/kurtosis are scale-free, so
  // no adjustment is needed there.
  // Moment/T input = r[1..) — drop the structural index-0 zero (combine §0-F: it
  // would bias mean/variance). The full-length oos_pnl is for corr-to-pool only.
  const std::span<const atx::f64> oos_full{agg.oos_pnl};
  const std::span<const atx::f64> moments = (oos_full.size() > 1U) ? oos_full.subspan(1) : oos_full;
  const atx::usize T = moments.size();
  const atx::f64 per_period_sharpe = agg.sharpe / std::sqrt(combine::kAnnualizationDays);
  const eval::DsrResult dsr =
      eval::deflated_sharpe(per_period_sharpe, T, eval::skewness(moments),
                            eval::excess_kurtosis(moments), cfg.trial_count, std::nullopt);

  return atx::core::Ok(FitnessReport{wq, redundancy, diversify, robust, raw, dsr.dsr,
                                     dsr.haircut_sharpe});
}

} // namespace atx::engine::factory
