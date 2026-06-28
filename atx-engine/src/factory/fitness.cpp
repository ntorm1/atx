#include "atx/engine/factory/fitness.hpp"

#include <algorithm> // std::clamp
#include <cmath>     // std::abs, std::isnan, std::sqrt
#include <optional>  // std::optional (weak-universe panel; deflation var arg)
#include <span>      // std::span
#include <utility>   // std::move (OosAggregate hand-off)
#include <vector>    // std::vector (fold-sliced streams)

#include "atx/engine/alpha/bytecode.hpp"       // alpha::compile, alpha::Program
#include "atx/engine/alpha/streams.hpp"        // alpha::extract_streams, AlphaStreams
#include "atx/engine/alpha/vm.hpp"             // alpha::Engine
#include "atx/engine/combine/correlation.hpp"  // combine::pairwise_complete_corr
#include "atx/engine/combine/metrics.hpp"      // combine::compute_metrics, AlphaMetrics
#include "atx/engine/cost/cost_aware.hpp"      // cost::round_trip_cost_bps (S4.3 ONE cost model)
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

// W4a split-sample stability (single source of truth; unit-tested via the header
// declaration). `oos_moments` is the OOS PnL stream with the structural index-0
// zero ALREADY dropped (the deflation-moment span). Slice at the FLOOR midpoint:
// H1 = the first floor(T/2) periods, H2 = the remaining ceil(T/2). Each half's
// per-period Sharpe is ms.mean/ms.std (std==0 ⇒ 0 — the PBO/subset_sharpe
// convention). `stable` iff BOTH half-Sharpes share `full_sign` (the full-sample
// per-period Sharpe sign: +1 / -1 / 0). PURE (no RNG, no eval).
[[nodiscard]] SplitHalf split_half_sharpe(std::span<const atx::f64> oos_moments,
                                          atx::f64 full_sign) noexcept {
  auto half_sharpe = [](std::span<const atx::f64> r) noexcept -> atx::f64 {
    const eval::MeanStd ms = eval::mean_std_pop(r);
    return (ms.std == 0.0) ? 0.0 : ms.mean / ms.std;
  };
  const atx::usize T = oos_moments.size();
  const atx::usize mid = T / 2U; // floor midpoint
  SplitHalf out;
  out.sharpe_h1 = (mid > 0U) ? half_sharpe(oos_moments.subspan(0U, mid)) : 0.0;
  out.sharpe_h2 = (T > mid) ? half_sharpe(oos_moments.subspan(mid)) : 0.0;
  auto sign_match = [](atx::f64 s, atx::f64 sign) noexcept -> bool {
    return (sign > 0.0) ? (s > 0.0) : (sign < 0.0 ? (s < 0.0) : (s == 0.0));
  };
  out.stable = sign_match(out.sharpe_h1, full_sign) && sign_match(out.sharpe_h2, full_sign);
  return out;
}

// ===========================================================================
//  S4.3 cost-window helpers — the per-name participation / ADV / σ sizing over
//  the DATE-MAJOR alpha::Panel (date 0 = earliest; date dates-1 = the newest /
//  current mark). These mirror risk::capacity.hpp's PanelView arithmetic EXACTLY
//  (same windows, same NaN/degenerate guards) but read the alpha::Panel layout
//  the fitness eval already holds. The √-impact COEFFICIENTS are not here — the
//  one cost model lives in cost::round_trip_cost_bps; only the sizing is local.
// ===========================================================================

// Dollar-ADV lookback (the P4-6 adv20 convention) and return-volatility lookback
// (the P4-6 vol convention) — the SAME named windows risk::capacity.hpp pins.
inline constexpr atx::usize kCostAdvWindow = 20U;
inline constexpr atx::usize kCostVolWindow = 60U;

// Per-step return ret_i(t) = close(t,i)/close(t-1,i) − 1 over the date-major
// `close` column (length dates*insts). A NaN/non-positive prior close yields 0.
[[nodiscard]] atx::f64 dm_step_return(std::span<const atx::f64> close, atx::usize insts,
                                      atx::usize t, atx::usize i) noexcept {
  const atx::f64 prev = close[(t - 1U) * insts + i];
  const atx::f64 cur = close[t * insts + i];
  if (std::isnan(prev) || std::isnan(cur) || prev <= 0.0) {
    return 0.0;
  }
  return cur / prev - 1.0;
}

// Dollar ADV of instrument i: mean of close*volume over the newest `w` rows
// [dates-w, dates). Skips NaN close/volume; 0 if no valid row (-> name skipped).
[[nodiscard]] atx::f64 dm_dollar_adv(std::span<const atx::f64> close,
                                     std::span<const atx::f64> volume, atx::usize dates,
                                     atx::usize insts, atx::usize i, atx::usize w) noexcept {
  const atx::usize start = (dates > w) ? (dates - w) : 0U;
  atx::f64 sum = 0.0;
  atx::usize n = 0U;
  for (atx::usize t = start; t < dates; ++t) { // ascending row -> order-fixed
    const atx::f64 c = close[t * insts + i];
    const atx::f64 v = volume[t * insts + i];
    if (!std::isnan(c) && !std::isnan(v)) {
      sum += c * v;
      ++n;
    }
  }
  return (n == 0U) ? 0.0 : sum / static_cast<atx::f64>(n);
}

// Population stddev of the newest `w` per-step returns of instrument i, skipping
// NaN terms. 0 when fewer than two valid returns remain (no measurable spread).
// Window covers returns at rows [dates-w, dates); a return at row t needs row t-1,
// so the oldest usable return row is 1 — the start is clamped to >= 1.
[[nodiscard]] atx::f64 dm_return_volatility(std::span<const atx::f64> close, atx::usize dates,
                                            atx::usize insts, atx::usize i, atx::usize w) noexcept {
  if (dates < 2U) {
    return 0.0;
  }
  const atx::usize start = (dates > w) ? (dates - w) : 1U;
  const atx::usize lo = (start < 1U) ? 1U : start;
  atx::f64 sum = 0.0;
  atx::usize n = 0U;
  for (atx::usize t = lo; t < dates; ++t) {
    const atx::f64 r = dm_step_return(close, insts, t, i);
    if (!std::isnan(r)) {
      sum += r;
      ++n;
    }
  }
  if (n < 2U) {
    return 0.0;
  }
  const atx::f64 mean = sum / static_cast<atx::f64>(n);
  atx::f64 ss = 0.0;
  for (atx::usize t = lo; t < dates; ++t) {
    const atx::f64 r = dm_step_return(close, insts, t, i);
    if (!std::isnan(r)) {
      const atx::f64 d = r - mean;
      ss += d * d;
    }
  }
  return std::sqrt(ss / static_cast<atx::f64>(n)); // population std
}

// The aggregate OOS metrics produced by averaging compute_metrics().fitness/
// .sharpe over the CPCV TEST folds, plus the candidate's full realized OOS PnL
// stream (the diversification + deflation input).
struct OosAggregate {
  atx::f64 wq;       // mean fold WQ fitness
  atx::f64 sharpe;   // mean fold ANNUALIZED Sharpe (de-annualized before deflation)
  atx::f64 turnover; // S3-0: mean fold turnover (same averaging as wq/sharpe)
  // The candidate's FULL realized PnL stream (length == n_periods). Used at full
  // length for corr-to-pool (must match the pool members' stream length; the
  // shared structural index-0 ~0 is mean-centered away in Pearson — correlation.hpp
  // deliberately INCLUDES index 0). For the DEFLATION moments the structural zero
  // is dropped (see pool_aware_fitness: skew/kurtosis/T are taken over r[1..)).
  std::vector<atx::f64> oos_pnl;
};

// Slice a flat per-period stream by an ascending index set (one fold's TEST
// indices). `width` == 1 for the PnL stream; == n_instruments for positions.
[[nodiscard]] std::vector<atx::f64>
slice_by_idx(std::span<const atx::f64> flat, std::span<const atx::usize> idx, atx::usize width) {
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
  atx::f64 sum_turnover = 0.0; // S3-0: accumulated alongside wq/sharpe, same valid-fold gate
  atx::usize n_valid = 0U;
  for (const eval::CpcvFold &fold : folds) {
    if (fold.test_idx.empty()) {
      continue;
    }
    const std::vector<atx::f64> test_pnl =
        slice_by_idx(pnl0, std::span<const atx::usize>{fold.test_idx}, 1U);
    const std::vector<atx::f64> test_pos = slice_by_idx(
        std::span<const atx::f64>{pos0}, std::span<const atx::usize>{fold.test_idx}, n_instruments);
    const combine::AlphaMetrics m =
        combine::compute_metrics(test_pnl, test_pos, n_instruments, book_size);
    // A degenerate fold (zero-variance / single-obs) yields NaN moments; skip it
    // from the mean rather than poison the aggregate with NaN.
    // S3-0: turnover is NOT NaN for a degenerate fold (mean_turnover returns 0 for
    // an empty/zero-instrument stream, never NaN); we gate it on the same n_valid
    // counter as wq/sharpe for a consistent average denominator.
    if (!std::isnan(m.fitness)) {
      sum_wq += m.fitness;
      sum_sharpe += std::isnan(m.sharpe) ? 0.0 : m.sharpe;
      sum_turnover += m.turnover; // m.turnover is always finite (mean_turnover never NaN)
      ++n_valid;
    }
  }
  const atx::f64 inv = (n_valid == 0U) ? 0.0 : 1.0 / static_cast<atx::f64>(n_valid);
  // Full realized stream (length == n_periods) — see OosAggregate::oos_pnl.
  std::vector<atx::f64> oos_pnl(pnl0.begin(), pnl0.end());
  return OosAggregate{sum_wq * inv, sum_sharpe * inv, sum_turnover * inv, std::move(oos_pnl)};
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
// PRECONDITION (when engine != nullptr): the passed engine MUST be bound to `panel`.
// Reusing it is byte-identical to a fresh engine because Engine::evaluate is
// idempotent — output depends only on (program, panel), never on prior engine state.
// PRECONDITION (when signals != nullptr): `signals` MUST be the SignalSet obtained by
// evaluating `cand` over `panel`. extract_streams is a pure function of
// (SignalSet, policy, panel, sim), so extracting from the caller's precomputed
// SignalSet is bit-identical to recomputing it here.
[[nodiscard]] atx::core::Result<alpha::AlphaStreams>
eval_streams(const Genome &cand, const alpha::Panel &panel, const WeightPolicy &policy,
             const exec::ExecutionSimulator &sim, alpha::Engine *engine = nullptr,
             const alpha::SignalSet *signals = nullptr) {
  if (signals != nullptr) {
    return alpha::extract_streams(*signals, policy, panel, sim);
  }
  ATX_TRY(const alpha::Program prog, alpha::compile(cand.ast, cand.analysis));
  alpha::Engine local{panel};
  alpha::Engine &eng = (engine != nullptr) ? *engine : local;
  ATX_TRY(const alpha::SignalSet ss, eng.evaluate(prog));
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
             const alpha::Panel *weak_panel, alpha::Engine *engine,
             const alpha::SignalSet *signals, CpcvCache *cpcv_cache) {
  // SAFETY (eps): the robustness ratio divides by wq; floor the denominator so a
  //               near-zero full-universe wq cannot blow the ratio to ±inf.
  constexpr atx::f64 kEps = 1e-12;

  // (1) full-universe eval -> OOS fold aggregate. Pass through the optional reusable
  // engine (nullptr -> fresh engine, non-null -> reuse the caller-supplied instance).
  // When `signals` is non-null, eval_streams skips compile+evaluate and extracts
  // directly from the caller's precomputed SignalSet (bit-identical, see eval_streams).
  ATX_TRY(const alpha::AlphaStreams strm, eval_streams(cand, panel, policy, sim, engine, signals));
  const atx::usize insts = strm.n_instruments();

  // S3-1 PERF: use cpcv_cache when supplied; fall back to recomputing when nullptr.
  // Both paths produce bit-identical spans and folds (pure deterministic functions).
  OosAggregate agg{};
  if (cpcv_cache != nullptr) {
    const CpcvCache::Entry &entry = cpcv_cache->get_or_build(strm.n_periods(), cfg.cpcv);
    agg = aggregate_oos(strm, entry.folds, insts, cfg.book_size);
  } else {
    const std::vector<eval::LabelSpan> spans = point_label_spans(strm.n_periods());
    const std::vector<eval::CpcvFold> folds =
        eval::cpcv_folds(std::span<const eval::LabelSpan>{spans}, cfg.cpcv);
    agg = aggregate_oos(strm, folds, insts, cfg.book_size);
  }
  const atx::f64 wq = agg.wq;

  // (3) sub-universe robustness (§0.8): re-eval on the weak-universe Panel.
  atx::f64 robust = 1.0; // degenerate default: no weak universe configured.
  if (weak_panel != nullptr) {
    ATX_TRY(const alpha::AlphaStreams weak_strm, eval_streams(cand, *weak_panel, policy, sim));
    const atx::usize weak_insts = weak_strm.n_instruments();

    // S3-1 PERF: same cache for weak panel path (the fold geometry is the same
    // function of n_periods; only the streams differ).
    OosAggregate weak_agg{};
    if (cpcv_cache != nullptr) {
      const CpcvCache::Entry &weak_entry =
          cpcv_cache->get_or_build(weak_strm.n_periods(), cfg.cpcv);
      weak_agg = aggregate_oos(weak_strm, weak_entry.folds, weak_insts, cfg.book_size);
    } else {
      const std::vector<eval::LabelSpan> weak_spans = point_label_spans(weak_strm.n_periods());
      const std::vector<eval::CpcvFold> weak_folds =
          eval::cpcv_folds(std::span<const eval::LabelSpan>{weak_spans}, cfg.cpcv);
      weak_agg = aggregate_oos(weak_strm, weak_folds, weak_insts, cfg.book_size);
    }
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

  // (5b) W4a split-sample stability over the SAME index-0-dropped OOS PnL stream
  // (`moments`). Full-sample per-period Sharpe sign reference (de-annualized
  // agg.sharpe; the /sqrt(252) factor is sign-preserving). split_half_sharpe slices
  // at the floor midpoint and forms each half's per-period Sharpe (single source of
  // truth, unit-tested). PURE over `moments` — no value/RNG/digest perturbation.
  const atx::f64 full_sign =
      (per_period_sharpe > 0.0) ? 1.0 : (per_period_sharpe < 0.0 ? -1.0 : 0.0);
  const SplitHalf split = split_half_sharpe(moments, full_sign);

  // (6) S4.3 book round-trip cost (bps) at the recorded target_aum — the cost
  // objective. GUARDED on target_aum > 0: when off (the default) NO cost compute
  // runs at all, cost_bps stays 0, and the eval path is byte-identical to pre-S4.3
  // (the boundary pin holds). When on, it is the |w|-weighted round-trip cost over
  // the candidate's LAST-period weights (book_cost_bps reuses the ONE cost model).
  atx::f64 cost_bps = 0.0;
  if (cfg.target_aum > 0.0) {
    cost_bps = book_cost_bps(strm, panel, cfg.cost, cfg.target_aum);
  }

  // S3-0: thread the OOS mean turnover (already computed in aggregate_oos with
  // no additional eval) into FitnessCore so finish_report can apply the opt-in
  // penalty.  The FitnessCore field order (matched by this aggregate init) is:
  //   oos_pnl, wq, robust, dsr, haircut_sharpe, cost_bps, turnover,
  //   sharpe_h1, sharpe_h2, split_stable
  return atx::core::Ok(FitnessCore{std::move(agg.oos_pnl), wq, robust, dsr.dsr,
                                   dsr.haircut_sharpe, cost_bps, agg.turnover,
                                   split.sharpe_h1, split.sharpe_h2, split.stable});
}

// Fold a pool-dependent redundancy into a FitnessCore -> the final FitnessReport.
// `redundancy` is the (Mean for the legacy AlphaStore path, Max for the PoolView
// path) |corr-to-pool| of core.oos_pnl; diversify = clamp(1−redundancy, 0, 1) and
// raw = wq * diversify * robust — identical to the original assembly. `cost_active`
// (FitnessCfg.target_aum > 0) gates the S4.3 cost objective (objectives[4]).
//
// S3-0 TURNOVER PENALTY: when cfg.turnover_penalty_slope > 0.0 a multiplicative
// discount `mult` in [kFloor, 1.0] is applied to `raw` after the product:
//
//   excess = max(0, turnover - max_turnover_target)
//   slack  = max(max_turnover_target * slope, kPenaltyEps)   // div-by-zero guard
//   mult   = clamp(1 - excess/slack, kFloor, 1.0)
//
// WHY THIS IS SAFE WITH max_turnover_target == +inf (the default):
//   excess = max(0, turnover - inf) = max(0, -inf) = 0.0          (well-defined)
//   slack  = max(inf * slope, kPenaltyEps) = +inf                 (well-defined)
//   mult   = clamp(1.0 - 0.0/inf, 0.0, 1.0) = clamp(1.0, ...) = 1.0
// 0.0/inf == 0.0 in IEEE 754; NOT NaN.  So the penalty is a clean no-op when
// the target is +inf, regardless of slope.  The REAL bite happens only when
// both slope > 0 AND max_turnover_target is finite.
[[nodiscard]] FitnessReport finish_report(const FitnessCore &core, atx::f64 redundancy,
                                          bool cost_active, const FitnessCfg &cfg) {
  // kFloor: prevents raw going negative (a negative raw would invert the selection
  // ordering in ScalarRaw mode — floor at 0.0 means a heavily-penalised alpha
  // scores the same as a degenerate zero-signal alpha, which is the right ceiling
  // on damage). kPenaltyEps: the div-by-zero guard on slack; matched to the
  // 1e-12 kEps convention already in use in fitness_core.
  constexpr atx::f64 kFloor      = 0.0;
  constexpr atx::f64 kPenaltyEps = 1e-12;

  const atx::f64 diversify = std::clamp(1.0 - redundancy, 0.0, 1.0);
  atx::f64 raw = core.wq * diversify * core.robust;

  // S3-0 opt-in turnover penalty — entered ONLY when slope > 0 (default 0.0 ->
  // branch never reached -> byte-identical to pre-S3-0, no NaN risk, no RNG).
  if (cfg.turnover_penalty_slope > 0.0) {
    const atx::f64 slope    = cfg.turnover_penalty_slope;
    const atx::f64 target   = cfg.max_turnover_target;  // may be +inf (the default)
    const atx::f64 turnover = core.turnover;
    // excess = max(0, turnover - target). When target==+inf this is max(0,-inf)=0.
    const atx::f64 excess = (turnover > target) ? (turnover - target) : 0.0;
    // slack = max(target * slope, kPenaltyEps). When target==+inf: inf*slope=+inf.
    // When target==0: 0*slope=0 -> guarded by kPenaltyEps.
    const atx::f64 slack = std::max(target * slope, kPenaltyEps);
    // mult in [kFloor, 1.0]. When excess==0: mult=1.0 (no penalty). When
    // excess/slack >= 1: mult=kFloor (maximally penalised). IEEE 754: 0.0/+inf==0.0
    // (not NaN), so the +inf-target path cleanly gives mult=1.0.
    const atx::f64 mult = std::clamp(1.0 - excess / slack, kFloor, 1.0);
    raw *= mult;
  }
  FitnessReport rep{core.wq, redundancy, diversify,          core.robust,
                    raw,     core.dsr,   core.haircut_sharpe};
  // S4.1: project the existing fields into the multi-objective vector (NO new
  // fitness math — these are the SAME wq/diversify/robust already assembled into
  // `raw`). MultiObjective mode ranks over these via NSGA-II; ScalarRaw ignores
  // them and uses `raw`. The product raw == objectives[0]*objectives[1]*objectives[2]
  // is the boundary-pin collapse target.
  rep.objectives[0] = core.wq;
  rep.objectives[1] = diversify;
  rep.objectives[2] = core.robust;
  rep.n_objectives = 3;
  // S4.3: when the cost objective is active (target_aum > 0) push the NEGATED book
  // round-trip cost into the FIXED cost slot (index 4) so pareto.hpp's pure-max
  // dominance treats a CHEAPER alpha as better, and bump n_objectives to cover it.
  // Slot 3 (novelty) is left at its default 0 here — uniform across genomes scored
  // by finish_report, hence INERT in NSGA dominance; the search_driver novelty pass
  // fills it later when active. When inactive, cost_bps stays 0, objectives[4] is
  // untouched, and n_objectives stays 3 — the boundary-pin no-op (NO digest drift).
  if (cost_active) {
    rep.cost_bps = core.cost_bps;
    rep.objectives[4] = -core.cost_bps;
    rep.n_objectives = 5;
  }
  // S4.2: carry the candidate's realized OOS PnL profile (the behavioral
  // descriptor / phenotype) out of the core so the SearchDriver can canon-cache it
  // and compute population-relative behavioral novelty without a re-eval. Copy (not
  // move) — `core` is borrowed const and may be read again by the caller.
  rep.descriptor = core.oos_pnl;
  // W4a: carry the split-sample stability metrics straight through (reporting + the
  // optional, default-disabled split-Sharpe admission floor). They do NOT enter
  // `raw`, the objective vector, or the digest — pure projection, byte-identical.
  rep.sharpe_h1 = core.sharpe_h1;
  rep.sharpe_h2 = core.sharpe_h2;
  rep.split_stable = core.split_stable;
  // S3-0: surface the OOS mean turnover the penalty reads (pure projection — does
  // NOT enter `raw`, the objective vector, or the digest; byte-identical reporting).
  rep.turnover = core.turnover;
  return rep;
}

} // namespace detail

[[nodiscard]] atx::f64 book_cost_bps(const alpha::AlphaStreams &strm, const alpha::Panel &panel,
                                     const cost::CalibratedCost &cost,
                                     atx::f64 target_aum) noexcept {
  if (target_aum <= 0.0 || strm.n_alphas() == 0U || strm.n_periods() == 0U) {
    return 0.0; // cost off / no streams -> no cost (the boundary-pin no-op guard)
  }
  const atx::usize dates = panel.dates();
  const atx::usize insts = panel.instruments();
  // The cost reads the candidate's LAST-period target weights (capacity_for_alpha
  // convention: the most recent rebalance is what is sized to target_aum).
  const std::span<const atx::f64> w = strm.positions(0U, strm.n_periods() - 1U);

  // "close" is mandatory (extract_streams already required it). "volume" gives the
  // dollar-ADV; a panel WITHOUT volume -> 0 ADV everywhere -> 0 cost (documented
  // degenerate, no NaN/Inf leak). Resolve once (cold path).
  const auto close_id = panel.field_id("close");
  const auto volume_id = panel.field_id("volume");
  if (!close_id.has_value() || !volume_id.has_value()) {
    return 0.0;
  }
  const std::span<const atx::f64> close = panel.field_all(*close_id);
  const std::span<const atx::f64> volume = panel.field_all(*volume_id);
  if (dates == 0U || insts == 0U) {
    return 0.0;
  }

  // Book aggregate: Σ_i |w_i| · round_trip_cost_bps(cost, part_i, σ_i). A dead/NaN
  // weight, non-positive price, zero ADV, zero participation, or zero σ makes a
  // name contribute nothing (mirrors risk::capacity_curve's guards exactly).
  atx::f64 acc = 0.0;
  const atx::usize n = (insts < w.size()) ? insts : w.size();
  for (atx::usize i = 0U; i < n; ++i) { // ascending inst -> order-fixed reduction
    const atx::f64 wi = w[i];
    if (std::isnan(wi) || wi == 0.0) {
      continue;
    }
    const atx::f64 abs_w = (wi < 0.0) ? -wi : wi;
    const atx::f64 price = close[(dates - 1U) * insts + i]; // newest mark (date dates-1)
    if (std::isnan(price) || price <= 0.0) {
      continue;
    }
    const atx::f64 adv =
        detail::dm_dollar_adv(close, volume, dates, insts, i, detail::kCostAdvWindow);
    if (adv <= 0.0) {
      continue;
    }
    const atx::f64 part = (target_aum * abs_w / price) / adv; // (AUM·|w|/price)/ADV
    const atx::f64 sigma =
        detail::dm_return_volatility(close, dates, insts, i, detail::kCostVolWindow);
    if (part <= 0.0 || sigma <= 0.0) {
      continue;
    }
    acc += abs_w * cost::round_trip_cost_bps(cost, part, sigma); // the ONE cost model
  }
  return acc;
}

[[nodiscard]] atx::core::Result<FitnessReport>
pool_aware_fitness(const Genome &cand, const combine::AlphaStore &pool, const alpha::Panel &panel,
                   const WeightPolicy &policy, const exec::ExecutionSimulator &sim,
                   const FitnessCfg &cfg, const alpha::Panel *weak_panel,
                   alpha::Engine *engine, const alpha::SignalSet *signals,
                   CpcvCache *cpcv_cache) {
  // Steps 1, 3, 5 (pool-INDEPENDENT) — written once in fitness_core (byte-identical
  // to the original body for those steps).  S3-1: cpcv_cache forwarded to eliminate
  // redundant span+fold rebuilds across genomes sharing the same (n_periods, cpcv).
  ATX_TRY(const detail::FitnessCore core,
          detail::fitness_core(cand, panel, policy, sim, cfg, weak_panel, engine, signals,
                               cpcv_cache));

  // (2) diversification discount (F7): MEAN |corr-to-pool| of the OOS PnL — the
  // legacy AlphaStore semantics (UNCHANGED; the green S3 suite gates this).
  const atx::f64 redundancy =
      corr_to_pool(std::span<const atx::f64>{core.oos_pnl}, pool, Reduce::Mean);

  // (4) raw = wq * diversify * robust (+ S3-0 opt-in turnover penalty), assembled
  // into the report. S4.3: the cost objective (objectives[4]) is active iff
  // target_aum > 0 (cost_bps is already in `core`, computed by fitness_core under
  // the same guard). S3-0: cfg carries slope/max_turnover_target; finish_report
  // applies the penalty when slope > 0.
  return atx::core::Ok(detail::finish_report(core, redundancy, cfg.target_aum > 0.0, cfg));
}

} // namespace atx::engine::factory
