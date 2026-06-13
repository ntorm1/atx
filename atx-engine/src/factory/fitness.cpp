#include "atx/engine/factory/fitness.hpp"

#include <algorithm> // std::clamp
#include <cmath>     // std::abs, std::isnan, std::sqrt
#include <optional>  // std::optional (weak-universe panel; deflation var arg)
#include <span>      // std::span
#include <utility>   // std::move (OosAggregate hand-off)
#include <vector>    // std::vector (fold-sliced streams)

#include "atx/engine/alpha/bytecode.hpp" // alpha::compile, alpha::Program
#include "atx/engine/alpha/streams.hpp"  // alpha::extract_streams, AlphaStreams
#include "atx/engine/alpha/vm.hpp"       // alpha::Engine
#include "atx/engine/combine/correlation.hpp" // combine::pairwise_complete_corr
#include "atx/engine/combine/metrics.hpp"     // combine::compute_metrics, AlphaMetrics
#include "atx/engine/eval/deflated_sharpe.hpp" // eval::deflated_sharpe, DsrResult
#include "atx/engine/eval/stats_ext.hpp"       // eval::skewness, eval::excess_kurtosis

namespace atx::engine::factory {

[[nodiscard]] atx::f64 corr_to_pool(std::span<const atx::f64> candidate_pnl,
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
[[nodiscard]] std::vector<atx::f64> slice_by_idx(std::span<const atx::f64> flat,
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
[[nodiscard]] std::vector<atx::f64> positions_flat0(const alpha::AlphaStreams &strm) {
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
[[nodiscard]] OosAggregate aggregate_oos(const alpha::AlphaStreams &strm,
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
[[nodiscard]] std::vector<eval::LabelSpan> point_label_spans(atx::usize n_periods) {
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
[[nodiscard]] atx::core::Result<alpha::AlphaStreams>
eval_streams(const Genome &cand, const alpha::Panel &panel, const WeightPolicy &policy,
             const exec::ExecutionSimulator &sim) {
  ATX_TRY(const alpha::Program prog, alpha::compile(cand.ast, cand.analysis));
  alpha::Engine engine{panel};
  ATX_TRY(const alpha::SignalSet ss, engine.evaluate(prog));
  return alpha::extract_streams(ss, policy, panel, sim);
}

// Compute every pool-independent fitness term (steps 1, 3, 5 of the §4.6 score:
// the OOS WQ aggregate, the sub-universe robustness re-eval, and the deflation).
// IDENTICAL control flow + values to the original pool_aware_fitness body for
// those steps — the legacy overload below now simply layers the Mean-based
// redundancy on top, so its output is provably unchanged. Err propagates a
// candidate compile/eval/extract failure (full or weak panel).
[[nodiscard]] atx::core::Result<FitnessCore>
fitness_core(const Genome &cand, const alpha::Panel &panel, const WeightPolicy &policy,
             const exec::ExecutionSimulator &sim, const FitnessCfg &cfg,
             const alpha::Panel *weak_panel) {
  // SAFETY (eps): the robustness ratio divides by wq; floor the denominator so a
  //               near-zero full-universe wq cannot blow the ratio to ±inf.
  constexpr atx::f64 kEps = 1e-12;

  // (1) full-universe eval -> OOS fold aggregate.
  ATX_TRY(const alpha::AlphaStreams strm, eval_streams(cand, panel, policy, sim));
  const atx::usize insts = strm.n_instruments();
  const std::vector<eval::LabelSpan> spans = point_label_spans(strm.n_periods());
  const std::vector<eval::CpcvFold> folds =
      eval::cpcv_folds(std::span<const eval::LabelSpan>{spans}, cfg.cpcv);
  OosAggregate agg = aggregate_oos(strm, folds, insts, cfg.book_size);
  const atx::f64 wq = agg.wq;

  // (3) sub-universe robustness (§0.8): re-eval on the weak-universe Panel.
  atx::f64 robust = 1.0; // degenerate default: no weak universe configured.
  if (weak_panel != nullptr) {
    ATX_TRY(const alpha::AlphaStreams weak_strm, eval_streams(cand, *weak_panel, policy, sim));
    const atx::usize weak_insts = weak_strm.n_instruments();
    const std::vector<eval::LabelSpan> weak_spans = point_label_spans(weak_strm.n_periods());
    const std::vector<eval::CpcvFold> weak_folds =
        eval::cpcv_folds(std::span<const eval::LabelSpan>{weak_spans}, cfg.cpcv);
    const OosAggregate weak_agg = aggregate_oos(weak_strm, weak_folds, weak_insts, cfg.book_size);
    const atx::f64 denom = (std::abs(wq) > kEps) ? wq : kEps;
    robust = std::clamp(weak_agg.wq / denom, 0.0, 1.0);
  }

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

  return atx::core::Ok(
      FitnessCore{std::move(agg.oos_pnl), wq, robust, dsr.dsr, dsr.haircut_sharpe});
}

// Fold a pool-dependent redundancy into a FitnessCore -> the final FitnessReport.
// `redundancy` is the (Mean for the legacy AlphaStore path, Max for the PoolView
// path) |corr-to-pool| of core.oos_pnl; diversify = clamp(1−redundancy, 0, 1) and
// raw = wq * diversify * robust — identical to the original assembly.
[[nodiscard]] FitnessReport finish_report(const FitnessCore &core, atx::f64 redundancy) {
  const atx::f64 diversify = std::clamp(1.0 - redundancy, 0.0, 1.0);
  const atx::f64 raw = core.wq * diversify * core.robust;
  return FitnessReport{core.wq, redundancy, diversify, core.robust,
                       raw,     core.dsr,   core.haircut_sharpe};
}

} // namespace detail

[[nodiscard]] atx::core::Result<FitnessReport>
pool_aware_fitness(const Genome &cand, const combine::AlphaStore &pool,
                   const alpha::Panel &panel, const WeightPolicy &policy,
                   const exec::ExecutionSimulator &sim, const FitnessCfg &cfg,
                   const alpha::Panel *weak_panel) {
  // Steps 1, 3, 5 (pool-INDEPENDENT) — written once in fitness_core (byte-identical
  // to the original body for those steps).
  ATX_TRY(const detail::FitnessCore core,
          detail::fitness_core(cand, panel, policy, sim, cfg, weak_panel));

  // (2) diversification discount (F7): MEAN |corr-to-pool| of the OOS PnL — the
  // legacy AlphaStore semantics (UNCHANGED; the green S3 suite gates this).
  const atx::f64 redundancy =
      corr_to_pool(std::span<const atx::f64>{core.oos_pnl}, pool, Reduce::Mean);

  // (4) raw = wq * diversify * robust, assembled into the report.
  return atx::core::Ok(detail::finish_report(core, redundancy));
}

} // namespace atx::engine::factory
