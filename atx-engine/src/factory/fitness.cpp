#include "atx/engine/factory/fitness.hpp"

#include <algorithm> // std::clamp
#include <cmath>     // std::abs, std::isnan, std::sqrt
#include <limits>
#include <optional>  // std::optional (weak-universe panel; deflation var arg)
#include <span>      // std::span
#include <string>
#include <utility>   // std::move (OosAggregate hand-off)
#include <vector>    // std::vector (fold-sliced streams)

#include "atx/engine/alpha/bytecode.hpp"       // alpha::compile, alpha::Program
#include "atx/engine/alpha/streams.hpp"        // alpha::extract_streams, AlphaStreams
#include "atx/engine/alpha/ts_ops.hpp"         // alpha::detail::ou_ar1_fit (S4-2 AR(1) reuse)
#include "atx/engine/alpha/vm.hpp"             // alpha::Engine
#include "atx/engine/combine/correlation.hpp"  // combine::pairwise_complete_corr
#include "atx/engine/combine/metrics.hpp"      // combine::compute_metrics, AlphaMetrics
#include "atx/engine/cost/capacity.hpp"       // cost::capacity_point, risk::CapacityPoint (S4-1)
#include "atx/engine/cost/cost_aware.hpp"      // cost::round_trip_cost_bps (S4.3 ONE cost model)
#include "atx/engine/eval/deflated_sharpe.hpp" // eval::deflated_sharpe, DsrResult
#include "atx/engine/eval/stats_ext.hpp"       // eval::skewness, eval::excess_kurtosis
#include "atx/engine/factory/fitness_cost_selection.hpp" // factory::apply_selection_cost (B7)

namespace atx::engine::factory {

bool ResidualFitnessBinding::matches(const alpha::Panel &panel) const noexcept {
  return panel_ == &panel && context_.dates() == panel.dates() &&
      context_.instruments() == panel.instruments() && !context_.identity_sha256().empty();
}
const ObjectiveIcContext &ResidualFitnessBinding::context() const noexcept { return context_; }
atx::core::Result<ResidualFitnessBinding> prepare_residual_fitness_binding(
    const ObjectiveIcContext &context, const alpha::Panel &panel) {
  ATX_TRY(const bool matches, objective_ic_panel_matches(context, panel));
  if (!matches)
    return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
        "residual fitness: captured price/presence payload differs");
  ResidualFitnessBinding binding;
  binding.panel_ = &panel; binding.context_ = context;
  return binding;
}

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

// Fold-averaged WQ fitness and turnover, plus the unique realized PnL stream
// used for diversification and deflation. These are search-window scores;
// independent post-selection validation requires a separate holdout.
struct OosAggregate {
  atx::f64 wq;       // mean fold WQ fitness
  atx::f64 turnover; // S3-0: mean fold turnover (same averaging as wq)
  // The candidate's FULL realized PnL stream (length == n_periods). Used at full
  // length for corr-to-pool (must match the pool members' stream length; the
  // shared structural index-0 ~0 is mean-centered away in Pearson — correlation.hpp
  // deliberately INCLUDES index 0). For the DEFLATION moments the structural zero
  // is dropped (see pool_aware_fitness: skew/kurtosis/T are taken over r[1..)).
  std::vector<atx::f64> oos_pnl;
};

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

// Aggregate the selected dates of each CPCV fold. Evaluate the causal VM and
// trades continuously before selecting dates, preserving warmup and adjacency.
// The full stream includes its structural zero for pool correlation; only the
// moment calculations drop that zero. Never concatenate overlapping folds for
// DSR: doing so repeats observations and artificially increases sample size.
[[nodiscard]] OosAggregate aggregate_oos(const alpha::AlphaStreams &strm,
                                         const std::vector<eval::CpcvFold> &folds,
                                         atx::usize n_instruments, atx::f64 book_size) {
  const std::span<const atx::f64> pnl0 = strm.pnl(0);
  const std::vector<atx::f64> pos0 = positions_flat0(strm);
  std::vector<atx::f64> turnover;
  (void)combine::detail::turnover_fill(pos0, n_instruments, book_size, &turnover);
  // Reuse scalar scratch across folds. Position differences must be computed
  // on the original calendar: joining test blocks invents trades across gaps.
  std::vector<atx::f64> test_pnl;
  test_pnl.reserve(pnl0.size() + 1U);

  atx::f64 sum_wq = 0.0;
  atx::f64 sum_turnover = 0.0; // S3-0: accumulated alongside wq, same valid-fold gate
  atx::usize n_valid = 0U;
  for (const eval::CpcvFold &fold : folds) {
    if (fold.test_idx.empty()) {
      continue;
    }
    // compute_metrics excludes its first input as a structural zero. A fold
    // starting later than date zero starts with a REAL return, so prepend the
    // structural zero explicitly and skip only the global structural date.
    test_pnl.assign(1U, 0.0);
    atx::f64 traded = 0.0;
    for (const auto t : fold.test_idx) {
      if (t != 0U) test_pnl.push_back(pnl0[t]);
      if (!turnover.empty()) traded += turnover[t];
    }
    combine::AlphaMetrics m = combine::compute_metrics(test_pnl, {}, 0U, book_size);
    m.turnover = traded / static_cast<atx::f64>(fold.test_idx.size());
    m.fitness = std::sqrt(std::abs(m.returns) /
        std::max(m.turnover, combine::kTurnoverFloor)) * m.sharpe;
    // A degenerate fold (zero-variance / single-obs) yields NaN moments; skip it
    // from the mean rather than poison the aggregate with NaN.
    // S3-0: turnover is NOT NaN for a degenerate fold (mean_turnover returns 0 for
    // an empty/zero-instrument stream, never NaN); we gate it on the same n_valid
    // counter as wq for a consistent average denominator.
    if (!std::isnan(m.fitness)) {
      sum_wq += m.fitness;
      sum_turnover += m.turnover; // m.turnover is always finite (mean_turnover never NaN)
      ++n_valid;
    }
  }
  const atx::f64 inv = (n_valid == 0U) ? 0.0 : 1.0 / static_cast<atx::f64>(n_valid);
  // Full realized stream (length == n_periods) — see OosAggregate::oos_pnl.
  std::vector<atx::f64> oos_pnl(pnl0.begin(), pnl0.end());
  return OosAggregate{sum_wq * inv, sum_turnover * inv, std::move(oos_pnl)};
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

// IC-only training objective. No backtest, invented P&L, sign search or DSR.
[[nodiscard]] atx::core::Result<FitnessCore>
residual_fitness_core(const Genome &cand, const alpha::Panel &panel, const FitnessCfg &cfg,
                     const alpha::Panel *weak_panel, alpha::Engine *engine,
                     const alpha::SignalSet *signals) {
  if (!cfg.residual_binding || !cfg.residual_binding->matches(panel))
    return atx::core::Err(atx::core::ErrorCode::InvalidArgument, "residual fitness: unbound panel/context");
  if (cfg.execution.rule != ExecutionObjectiveRule::LegacyStreamsV1 || cfg.execution_context ||
      weak_panel || cfg.target_aum != 0 || cfg.cost_selection.impact_in_selection ||
      cfg.capacity_objective || cfg.turnover_objective || cfg.turnover_penalty_slope != 0)
    return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
        "residual fitness: execution/cost/weak/turnover overlays are unsupported");
  const auto &context = cfg.residual_binding->context();
  std::optional<ObjectiveIcScratch> owned_scratch;
  auto *scratch = cfg.residual_scratch;
  if (scratch == nullptr) {
    ATX_TRY(auto prepared, prepare_objective_ic_scratch(context));
    owned_scratch.emplace(std::move(prepared)); scratch = &*owned_scratch;
  }
  std::optional<alpha::SignalSet> evaluated;
  if (signals == nullptr) {
    ATX_TRY(auto program, alpha::compile(cand.ast, cand.analysis));
    if (engine) {
      ATX_TRY(auto ss, engine->evaluate(program)); evaluated.emplace(std::move(ss));
    } else {
      alpha::Engine owned_engine{panel};
      ATX_TRY(auto ss, owned_engine.evaluate(program)); evaluated.emplace(std::move(ss));
    }
    signals = &*evaluated;
  }
  if (signals->dates != panel.dates() || signals->instruments != panel.instruments() ||
      signals->alphas.size() != 1)
    return atx::core::Err(atx::core::ErrorCode::InvalidArgument, "residual fitness: one matching signal required");
  ATX_TRY(auto result, evaluate_objective_ic(signals->alphas.front().values, context, *scratch));
  FitnessCore out{};
  out.objective_rule = FitnessObjectiveRule::ResidualHacIcV2;
  out.residual_available = true;
  for (const auto &h : result.horizons) {
    out.residual_available = out.residual_available && h.inference_defined && std::isfinite(h.hac_ir);
    out.residual_score += h.hac_ir / 3; // signed equal-weight mean, not absolute IC
  }
  out.residual_available = out.residual_available && std::isfinite(out.residual_score);
  out.residual_ic = std::move(result);
  return out;
}

// Explicit V2 objective: mature, chronological NET return SR. No fold slicing,
// target-difference turnover, structural zero observation or second cost overlay.
// This bounded execution slice does not claim residual/HAC/half-life calibration.
[[nodiscard]] atx::core::Result<FitnessCore>
execution_fitness_core(const Genome& cand, const alpha::Panel& panel,
                       const WeightPolicy& policy, const FitnessCfg& cfg,
                       const alpha::Panel* weak_panel, alpha::Engine* engine,
                       const alpha::SignalSet* signals) {
  const auto* context = cfg.execution_context;
  if (context == nullptr || !execution_objective_matches(*context, panel, policy, cfg.execution))
    return atx::core::Err(atx::core::ErrorCode::InvalidArgument, "execution fitness: context mismatch");
  if (weak_panel != nullptr || cfg.target_aum != 0.0 || cfg.cost_selection.impact_in_selection ||
      cfg.capacity_objective || cfg.turnover_objective || cfg.turnover_penalty_slope != 0.0)
    return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
        "execution fitness: legacy cost/turnover/capacity/weak-panel overlay unsupported");
  auto evaluate = [&]() -> atx::core::Result<alpha::AlphaStreams> {
    if (signals != nullptr) {
      if (signals->alphas.size() != 1)
        return atx::core::Err(atx::core::ErrorCode::InvalidArgument, "execution fitness: one root required");
      return extract_execution_streams(*signals, *context);
    }
    ATX_TRY(const alpha::Program program, alpha::compile(cand.ast, cand.analysis));
    alpha::Engine local{panel};
    auto& evaluator = engine != nullptr ? *engine : local;
    ATX_TRY(const alpha::SignalSet evaluated, evaluator.evaluate(program));
    if (evaluated.alphas.size() != 1)
      return atx::core::Err(atx::core::ErrorCode::InvalidArgument, "execution fitness: one root required");
    return extract_execution_streams(evaluated, *context);
  };
  ATX_TRY(auto streams, evaluate());
  if (streams.n_alphas() != 1)
    return atx::core::Err(atx::core::ErrorCode::InvalidArgument, "execution fitness: one root required");
  const auto begin = streams.first_realization_, end = streams.realization_end_;
  const auto count = end - begin;
  if (count < 3)
    return atx::core::Err(atx::core::ErrorCode::Unavailable, "execution fitness: fewer than three mature returns");
  const auto returns = streams.pnl(0).subspan(begin, count);
  atx::f64 turnover = 0;
  for (atx::usize t = begin; t < end; ++t) {
    if (streams.valid_flat[t] == 0 || !std::isfinite(streams.pnl_flat[t]) ||
        !std::isfinite(streams.turnover_flat[t]))
      return atx::core::Err(atx::core::ErrorCode::Unavailable, "execution fitness: invalid interior realization");
    turnover += streams.turnover_flat[t];
  }
  const auto moments = eval::mean_std_pop(returns);
  const auto per_period = moments.std > 0 ? moments.mean / moments.std : 0.0;
  const auto skew = eval::skewness(returns), kurtosis = eval::excess_kurtosis(returns);
  const auto deflated = eval::deflated_sharpe(per_period, count, skew, kurtosis, cfg.trial_count, std::nullopt);
  const auto annual = per_period * std::sqrt(combine::kAnnualizationDays);
  if (!std::isfinite(moments.mean) || !std::isfinite(moments.std) || !std::isfinite(skew) ||
      !std::isfinite(kurtosis) || !std::isfinite(annual) || !std::isfinite(turnover) ||
      !std::isfinite(deflated.dsr) || !std::isfinite(deflated.haircut_sharpe))
    return atx::core::Err(atx::core::ErrorCode::OutOfRange, "execution fitness: nonfinite moments");
  const auto split = split_half_sharpe(returns, per_period > 0 ? 1.0 : per_period < 0 ? -1.0 : 0.0);
  FitnessCore out{};
  out.oos_pnl = std::move(streams.pnl_flat); // uncompressed calendar, NaN outside maturity
  // DSR remains a reported diagnostic. A multiplicative DSR haircut is invalid
  // for signed scores; SearchDriver refuses that legacy selection overlay.
  out.wq = annual; out.robust = 1.0; out.dsr = deflated.dsr;
  out.haircut_sharpe = deflated.haircut_sharpe;
  out.turnover = turnover / static_cast<atx::f64>(count);
  out.sharpe_h1 = split.sharpe_h1; out.sharpe_h2 = split.sharpe_h2; out.split_stable = split.stable;
  out.execution_rule = ExecutionObjectiveRule::DelayedSurfaceV2;
  out.execution_context_sha256 = streams.execution_context_sha256;
  out.realized_begin = begin; out.realized_end = end;
  out.dsr_sample = {count, per_period, skew, kurtosis, true};
  return atx::core::Ok(std::move(out));
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
  if (cfg.objective_rule == FitnessObjectiveRule::ResidualHacIcV2)
    return residual_fitness_core(cand, panel, cfg, weak_panel, engine, signals);
  if (cfg.objective_rule != FitnessObjectiveRule::LegacyV1)
    return atx::core::Err(atx::core::ErrorCode::InvalidArgument, "fitness: unknown objective rule");
  if (cfg.execution.rule == ExecutionObjectiveRule::DelayedSurfaceV2)
    return execution_fitness_core(cand, panel, policy, cfg, weak_panel, engine, signals);
  if (cfg.execution.rule != ExecutionObjectiveRule::LegacyStreamsV1)
    return atx::core::Err(atx::core::ErrorCode::InvalidArgument, "fitness: unknown execution rule");
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
  CpcvCache local_cache;
  if (cpcv_cache == nullptr && cfg.cpcv.rule != eval::CpcvRule::ObservationV1)
    cpcv_cache = &local_cache;
  OosAggregate agg{};
  if (cpcv_cache != nullptr) {
    ATX_TRY(const auto* entry, cpcv_cache->get_or_build_checked(strm.n_periods(), cfg.cpcv, cfg.cpcv_session_stride));
    agg = aggregate_oos(strm, entry->folds, insts, cfg.book_size);
  } else {
    const std::vector<eval::LabelSpan> spans = point_label_spans(strm.n_periods());
    ATX_TRY(auto plan, eval::cpcv_plan(std::span<const eval::LabelSpan>{spans}, cfg.cpcv));
    agg = aggregate_oos(strm, plan.folds, insts, cfg.book_size);
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
      ATX_TRY(const auto* weak_entry,
              cpcv_cache->get_or_build_checked(weak_strm.n_periods(), cfg.cpcv, cfg.cpcv_session_stride));
      weak_agg = aggregate_oos(weak_strm, weak_entry->folds, weak_insts, cfg.book_size);
    } else {
      const std::vector<eval::LabelSpan> weak_spans = point_label_spans(weak_strm.n_periods());
      ATX_TRY(auto weak_plan,
              eval::cpcv_plan(std::span<const eval::LabelSpan>{weak_spans}, cfg.cpcv));
      weak_agg = aggregate_oos(weak_strm, weak_plan.folds, weak_insts, cfg.book_size);
    }
    const atx::f64 denom = (std::abs(wq) > kEps) ? wq : kEps;
    robust = std::clamp(weak_agg.wq / denom, 0.0, 1.0);
  }

  // (5) deflation by the running trial count N (F4): higher N -> lower dsr.
  //
  // DSR's Sharpe, T, skewness and kurtosis must describe the SAME unique
  // realized sample. Averaging fold Sharpes is nonlinear and cannot be paired
  // with full-stream moments (nor treated as independent observations).
  // Drop only the original structural zero; retain full length for pool corr.
  const std::span<const atx::f64> oos_full{agg.oos_pnl};
  const std::span<const atx::f64> moments = (oos_full.size() > 1U) ? oos_full.subspan(1) : oos_full;
  const atx::usize T = moments.size();
  const auto sample = eval::mean_std_pop(moments);
  const atx::f64 per_period_sharpe = sample.std == 0.0 ? 0.0 : sample.mean / sample.std;
  const DsrSampleStats dsr_sample{T, per_period_sharpe, eval::skewness(moments),
                                  eval::excess_kurtosis(moments), true};
  const eval::DsrResult dsr =
      eval::deflated_sharpe(dsr_sample.per_period_sharpe, dsr_sample.observations,
                            dsr_sample.skewness, dsr_sample.excess_kurtosis,
                            cfg.trial_count, std::nullopt);

  // (5b) W4a split-sample stability over the SAME index-0-dropped OOS PnL stream
  // (`moments`). Full-sample per-period Sharpe sign reference. split_half_sharpe slices
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

  // (6b) B7 (S4-4/S5 wire): the SELECTION-scalar cost (see
  // FitnessCore::selection_cost_bps and fitness_cost_selection.hpp's SEAM).
  // Pre-computed HERE (strm/panel are in scope) so finish_report -- which has
  // neither -- can net it into `raw` via apply_selection_cost. GUARDED
  // identically to CostSelectionConfig's own inert-default contract: off
  // (impact_in_selection==false) or selection_aum<=0 -> selection_cost_bps stays
  // 0.0 (apply_selection_cost's OWN guard would no-op regardless -- computing it
  // only when active also avoids a redundant book_cost_bps call on the common
  // off path).
  atx::f64 selection_cost_bps = 0.0;
  if (cfg.cost_selection.impact_in_selection && cfg.cost_selection.selection_aum > 0.0) {
    selection_cost_bps = book_cost_bps(strm, panel, cfg.cost, cfg.cost_selection.selection_aum);
  }

  // (6c) S4-1: sqrt-law capacity objective, GATED on cfg.capacity_objective (mirrors
  // the S4.3 cost gate -- zero compute at all when off, preserving both the
  // off-path byte-identity AND the off-path perf cost).
  atx::f64 capacity_score = 0.0;
  if (cfg.capacity_objective) {
    capacity_score = capacity_sqrt_law_score(strm, panel, cfg.cost, cfg.target_aum);
  }
  // (6d) S4-2: turnover/alpha-decay objective, GATED on cfg.turnover_objective.
  atx::f64 turnover_autocorr = 0.0;
  if (cfg.turnover_objective) {
    turnover_autocorr = turnover_autocorr_score(strm);
  }

  // S3-0: thread the OOS mean turnover (already computed in aggregate_oos with
  // no additional eval) into FitnessCore so finish_report can apply the opt-in
  // penalty.  The FitnessCore field order (matched by this aggregate init) is:
  //   oos_pnl, wq, robust, dsr, haircut_sharpe, cost_bps, turnover,
  //   sharpe_h1, sharpe_h2, split_stable, selection_cost_bps,
  //   capacity_score, turnover_autocorr (S4 — the two trailing gate columns; left
  //   at their inert 0.0 defaults here, populated by S4-3's gated compute block).
  FitnessCore core{std::move(agg.oos_pnl), wq, robust, dsr.dsr,
                                   dsr.haircut_sharpe, cost_bps, agg.turnover,
                                   split.sharpe_h1, split.sharpe_h2, split.stable,
                                   selection_cost_bps, capacity_score, turnover_autocorr};
  core.dsr_sample = dsr_sample;
  return atx::core::Ok(std::move(core));
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
  if (core.objective_rule == FitnessObjectiveRule::ResidualHacIcV2) {
    FitnessReport report{};
    const auto missing = std::numeric_limits<atx::f64>::quiet_NaN();
    report.wq = missing; report.redundancy = missing; report.diversify = missing;
    report.robust = missing; report.dsr = missing; report.haircut_sharpe = missing;
    report.cost_bps = missing; report.turnover = missing;
    report.sharpe_h1 = missing; report.sharpe_h2 = missing;
    report.objective_rule = core.objective_rule;
    report.residual_available = core.residual_available;
    report.residual_ic = core.residual_ic;
    report.raw = core.residual_available ? core.residual_score :
        -std::numeric_limits<atx::f64>::infinity();
    if (core.residual_available) { report.objectives[0] = report.raw; report.n_objectives = 1; }
    return report;
  }
  // kFloor: prevents raw going negative (a negative raw would invert the selection
  // ordering in ScalarRaw mode — floor at 0.0 means a heavily-penalised alpha
  // scores the same as a degenerate zero-signal alpha, which is the right ceiling
  // on damage). kPenaltyEps: the div-by-zero guard on slack; matched to the
  // 1e-12 kEps convention already in use in fitness_core.
  constexpr atx::f64 kFloor      = 0.0;
  constexpr atx::f64 kPenaltyEps = 1e-12;

  const atx::f64 diversify = std::clamp(1.0 - redundancy, 0.0, 1.0);
  atx::f64 raw = core.wq * diversify * core.robust;

  // B7 (S4-4/S5 wire): net `raw` of the SELECTION-scalar impact cost.
  // apply_selection_cost's OWN inert-default guard (impact_in_selection==false
  // or selection_aum<=0 -> return raw unchanged) makes this byte-identical to
  // pre-B7 on every existing path — core.selection_cost_bps is also 0.0 by
  // default (fitness_core only computes it under the same guard), so the call
  // is a documented no-op even before its internal guard is checked.
  raw = apply_selection_cost(raw, core.selection_cost_bps, cfg.cost_selection);

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
  rep.dsr_sample = core.dsr_sample;
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
  // S4-1: kObjCapacity -- active iff cfg.capacity_objective. std::max (not a hard
  // assignment) so this never REGRESSES n_objectives if cost_active already bumped
  // it to 5 -- mirrors search_driver.cpp's kObjParsimony/kObjDeflation bump pattern.
  // When off: rep.capacity_score stays core's inert 0.0, objectives[7] is untouched
  // (uniform default across genomes -> inert in NSGA), n_objectives is unchanged --
  // the boundary-pin no-op, byte-identical.
  rep.capacity_score = core.capacity_score;
  if (cfg.capacity_objective) {
    rep.objectives[kObjCapacity] = core.capacity_score;
    rep.n_objectives = static_cast<atx::u8>(
        std::max<atx::usize>(rep.n_objectives, kObjCapacity + 1U));
  }
  // S4-2: kObjTurnover -- active iff cfg.turnover_objective (same discipline).
  rep.turnover_autocorr = core.turnover_autocorr;
  if (cfg.turnover_objective) {
    rep.objectives[kObjTurnover] = core.turnover_autocorr;
    rep.n_objectives = static_cast<atx::u8>(
        std::max<atx::usize>(rep.n_objectives, kObjTurnover + 1U));
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
  rep.execution_rule = core.execution_rule;
  rep.execution_context_sha256 = core.execution_context_sha256;
  rep.realized_begin = core.realized_begin; rep.realized_end = core.realized_end;
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
    // S4-1 [B1 fix]: participation is notional/dollar-ADV, NOT
    // (notional/price)/dollar-ADV -- the prior formula was off by a factor of
    // price_i (identical bug to risk/capacity.hpp's impact_cost_bps; see
    // capacity_participation_test.cpp for the by-construction proof). `price`
    // is still read above to gate out unpriced names (an unpriced name has no
    // book value), it just no longer enters the participation ratio.
    const atx::f64 part = (target_aum * abs_w) / adv; // notional / dollar-ADV
    const atx::f64 sigma =
        detail::dm_return_volatility(close, dates, insts, i, detail::kCostVolWindow);
    if (part <= 0.0 || sigma <= 0.0) {
      continue;
    }
    acc += abs_w * cost::round_trip_cost_bps(cost, part, sigma); // the ONE cost model
  }
  return acc;
}

namespace detail {
// S4-1: log-spaced AUM grid, 1 decade either side of `center`, ascending.
// Mirrors cost::compute_capacity_vector's grid-building CONVENTION
// (cost/capacity.hpp:145-161) at a coarser resolution -- this objective only
// needs to bracket the zero-crossing for GA selection pressure, not emit a
// diagnostic-grade curve, so 8 points is ample and far cheaper per genome.
inline constexpr atx::usize kCapacityObjGridPoints = 8U;

[[nodiscard]] std::vector<atx::f64> capacity_obj_aum_grid(atx::f64 center) {
  std::vector<atx::f64> grid;
  grid.reserve(kCapacityObjGridPoints);
  const atx::f64 log_lo = std::log(0.1 * center);
  const atx::f64 log_hi = std::log(10.0 * center);
  const atx::f64 denom = static_cast<atx::f64>(kCapacityObjGridPoints - 1U);
  for (atx::usize k = 0U; k < kCapacityObjGridPoints; ++k) {
    const atx::f64 frac = static_cast<atx::f64>(k) / denom;
    grid.push_back(std::exp(log_lo + frac * (log_hi - log_lo)));
  }
  return grid;
}
} // namespace detail

[[nodiscard]] atx::f64 capacity_sqrt_law_score(const alpha::AlphaStreams &strm,
                                               const alpha::Panel &panel,
                                               const cost::CalibratedCost &cost,
                                               atx::f64 target_aum) noexcept {
  if (target_aum <= 0.0 || strm.n_alphas() == 0U || strm.n_periods() == 0U) {
    return 0.0; // no AUM anchor -> no capacity signal (mirrors book_cost_bps's guard)
  }
  const std::span<const atx::f64> pnl0 = strm.pnl(0U);
  atx::f64 sum = 0.0;
  for (const atx::f64 p : pnl0) {
    sum += p;
  }
  const atx::f64 gross_edge_bps =
      pnl0.empty() ? 0.0 : 1.0e4 * (sum / static_cast<atx::f64>(pnl0.size()));

  const std::vector<atx::f64> grid = detail::capacity_obj_aum_grid(target_aum);
  std::vector<risk::CapacityPoint> curve;
  curve.reserve(grid.size());
  for (const atx::f64 aum : grid) { // ascending -> capacity_point's monotonicity precondition
    const atx::f64 cost_bps_at_aum = book_cost_bps(strm, panel, cost, aum); // the ONE cost model
    curve.push_back(risk::CapacityPoint{aum, gross_edge_bps - cost_bps_at_aum});
  }
  const atx::f64 capacity_aum =
      cost::capacity_point(std::span<const risk::CapacityPoint>{curve});
  if (std::isinf(capacity_aum)) {
    return 1.0; // ample capacity (never crosses zero on the grid) -> saturate
  }
  return capacity_aum / (capacity_aum + target_aum);
}

[[nodiscard]] atx::f64 turnover_autocorr_score(const alpha::AlphaStreams &strm) noexcept {
  const atx::usize insts = strm.n_instruments();
  const atx::usize periods = strm.n_periods();
  if (insts == 0U || periods < 3U || strm.n_alphas() == 0U) {
    return 0.0; // need >= 2 lag pairs (ou_ar1_fit's own floor)
  }
  const std::span<const atx::f64> last_w = strm.positions(0U, periods - 1U);
  std::vector<atx::f64> series;
  series.reserve(periods);
  atx::f64 wsum = 0.0;
  atx::f64 acc = 0.0;
  for (atx::usize i = 0U; i < insts; ++i) { // ascending inst -> order-fixed reduction
    const atx::f64 wi = last_w[i];
    if (std::isnan(wi) || wi == 0.0) {
      continue; // dead name -> no turnover signal to weight in
    }
    series.clear();
    for (atx::usize t = 0U; t < periods; ++t) { // ascending period -> order-fixed
      series.push_back(strm.positions(0U, t)[i]);
    }
    const alpha::detail::OuAr1Fit fit =
        alpha::detail::ou_ar1_fit(std::span<const atx::f64>{series});
    if (std::isnan(fit.b)) {
      continue; // degenerate fit -- SKIP, do not zero-in a real neighbour's signal
    }
    const atx::f64 abs_w = std::abs(wi);
    acc += abs_w * fit.b;
    wsum += abs_w;
  }
  return (wsum == 0.0) ? 0.0 : acc / wsum;
}

[[nodiscard]] atx::core::Result<FitnessReport>
pool_aware_fitness(const Genome &cand, const combine::AlphaStore &pool, const alpha::Panel &panel,
                   const WeightPolicy &policy, const exec::ExecutionSimulator &sim,
                   const FitnessCfg &cfg, const alpha::Panel *weak_panel,
                   alpha::Engine *engine, const alpha::SignalSet *signals,
                   CpcvCache *cpcv_cache) {
  if (cfg.objective_rule == FitnessObjectiveRule::ResidualHacIcV2 && pool.n_alphas() != 0)
    return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
        "residual fitness: a P&L pool has no residual IC recipe");
  if (cfg.execution.rule == ExecutionObjectiveRule::DelayedSurfaceV2 && pool.n_alphas() != 0)
    return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
        "execution fitness: nonempty pool has no bound execution/calendar recipe");
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
