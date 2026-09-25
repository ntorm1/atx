#include "stages.hpp"
#include "stage_combine.hpp"
#include "stage_regime.hpp" // atx::impl::fit_regime_hmm (S3-3)

#include <algorithm>   // std::sort
#include <array>       // std::array (single-horizon span for meta_features_from_pool)
#include <cmath>       // std::floor (A2a holdout-fit window)
#include <filesystem>
#include <fstream>
#include <iomanip>     // std::setprecision (W0-I0a exact weights sidecar)
#include <limits>      // std::numeric_limits
#include <optional>
#include <span>        // std::span (per-alpha position rows)
#include <string>
#include <string_view>
#include <vector>

#include "atx/core/error.hpp"
#include "atx/core/types.hpp"

#include "atx/engine/alpha/bytecode.hpp"       // alpha::compile_batch, alpha::Program
#include "atx/engine/alpha/panel.hpp"          // alpha::Panel, alpha::SignalSet
#include "atx/engine/alpha/parser.hpp"         // alpha::Library
#include "atx/engine/alpha/streams.hpp"        // alpha::extract_streams, alpha::AlphaStreams
#include "atx/engine/alpha/vm.hpp"             // alpha::Engine
#include "atx/engine/combine/combiner.hpp"     // combine::AlphaCombiner, CombinerConfig, CombineMethod, Combination
#include "atx/engine/combine/conviction.hpp"   // combine::conviction, ConvictionScore, ConvictionConfig, ExplainFlag
#include "atx/engine/combine/crowding.hpp"     // combine::decorrelate_weights, CrowdingConfig (9.2)
#include "atx/engine/combine/gate.hpp"         // combine::GateConfig (library open, 8.B)
#include "atx/engine/combine/metrics.hpp"      // combine::compute_metrics
#include "atx/engine/combine/regime_combiner.hpp" // combine::fit_regime_combiner, RegimeCombiner (S3-3)
#include "atx/engine/combine/store.hpp"        // combine::AlphaStore
#include "atx/engine/data/factor_model_artifact.hpp" // data::cleaned_alpha_cov (S3-4, S1 seam)
#include "atx/engine/eval/cpcv.hpp"            // eval::CpcvConfig (S3-1 stack CPCV threading)
#include "atx/engine/eval/deflated_sharpe.hpp" // eval::deflated_sharpe, DsrResult
#include "atx/engine/eval/pbo.hpp"             // eval::PboResult (full def needed to construct zero value)
#include "atx/engine/eval/perf_metrics.hpp"    // eval::compute_return_metrics, ReturnMetricsCfg, ReturnMetrics
#include "atx/engine/eval/breadth.hpp"          // eval::effective_breadth
#include "atx/engine/eval/stats_ext.hpp"       // eval::mean_std_pop (MeanStd), skewness, excess_kurtosis
#include "atx/engine/learn/ensemble.hpp"       // learn::fit_stack, meta_features_from_pool, StackingCfg (S3-1)
#include "atx/engine/learn/feature_matrix.hpp" // learn::FeatureMatrix (S3-1)
#include "atx/engine/learn/hmm.hpp"            // learn::Hmm, baum_welch, regime_posterior_at (S3-3)
#include "atx/engine/library/library.hpp"      // library::Library, AlphaId, AlphaRecordView (8.B)
#include "atx/engine/loop/weight_policy.hpp"   // engine::WeightPolicy
#include "atx/engine/risk/factor_model.hpp"    // risk::FactorModel (S5-2 diagonal cov)
#include "atx/engine/risk/kelly_sizing.hpp"    // risk::kelly_size, KellyConfig, KellyWeights (S5-2)
#include "atx/core/linalg/linalg.hpp"          // core::linalg::VecX, MatX (S5-2 Kelly inputs)
#include "atx/core/linalg/solve.hpp"           // core::linalg::solve_spd (S3-1 stack->weight projection)

#include "artifacts.hpp"
#include "config.hpp"
#include "dead_alpha_wire.hpp" // split-range ledger (W0-I0a, I-01)
#include "research_sim.hpp"
#include "sector_groups.hpp"
#include "serialize_panel.hpp"
#include "panel_pipeline.hpp"
#include "atx/core/sha256.hpp"

namespace atx::impl {

namespace alpha   = atx::engine::alpha;
namespace combine = atx::engine::combine;
namespace risk    = atx::engine::risk;  // S3-4: RiskModelConfig/RiskModelKind
namespace learn   = atx::engine::learn; // S3-1/S3-3: fit_stack, meta_features_from_pool, Hmm
using atx::engine::WeightPolicy;

// ---------------------------------------------------------------------------
// method_from_string — map --method string to CombineMethod.
// ---------------------------------------------------------------------------
static atx::core::Result<combine::CombineMethod>
method_from_string(const std::string& s) {
    if (s.empty() || s == "shrinkage-mv") {
        return atx::core::Ok(combine::CombineMethod::ShrinkageMv);
    }
    if (s == "equal") {
        return atx::core::Ok(combine::CombineMethod::EqualWeight);
    }
    if (s == "rank") {
        return atx::core::Ok(combine::CombineMethod::RankAverage);
    }
    if (s == "ic") {
        return atx::core::Ok(combine::CombineMethod::IcWeighted);
    }
    if (s == "bounded") {
        return atx::core::Ok(combine::CombineMethod::BoundedRegression);
    }
    // S3-1: opt-in nonlinear / regime-conditional stacking. CLI flag threading
    // (making these reachable from the actual --method command line argument)
    // is Sprint 5's job (the four hub files); this string arm is exercised
    // today only by direct-call tests that populate cfg.method by hand.
    if (s == "stack") {
        return atx::core::Ok(combine::CombineMethod::Stack);
    }
    if (s == "regime-stack") {
        return atx::core::Ok(combine::CombineMethod::RegimeStack);
    }
    return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                          "combine: unknown --method '" + s + "'");
}

// method_to_string — the inverse of method_from_string, used for the kvs
// "method" field and the weights-sidecar "method=" line. S3 threads the
// ACTUAL CombineMethod being run (via the CombinerConfig overloads below) —
// which need not equal cfg.method's raw string (a direct-call test may set
// combiner_cfg.method without ever populating cfg.method) — so those labels
// are derived from the resolved enum, not the raw config string. For the five
// legacy methods this reproduces cfg.method's pre-S3 label BYTE-IDENTICALLY
// (method_from_string/method_to_string round-trip on the same exact strings),
// so the zero-arg run_combine(cfg) path is unaffected.
static std::string method_to_string(combine::CombineMethod m) {
    switch (m) {
    case combine::CombineMethod::EqualWeight:       return "equal";
    case combine::CombineMethod::RankAverage:       return "rank";
    case combine::CombineMethod::IcWeighted:        return "ic";
    case combine::CombineMethod::ShrinkageMv:       return "shrinkage-mv";
    case combine::CombineMethod::BoundedRegression: return "bounded";
    case combine::CombineMethod::Stack:             return "stack";
    case combine::CombineMethod::RegimeStack:       return "regime-stack";
    }
    return "shrinkage-mv"; // unreachable: every CombineMethod handled above
}

// ---------------------------------------------------------------------------
// blend_window_sharpe — annualized Sharpe of the weighted-blend PnL Σ_a w[a]*pool.pnl(a)[i]
// over [begin, end). Prepends a structural zero so compute_return_metrics' pnl[1..T) convention
// scores every real return. Shared by D3a's realized_ir and D3b's per-fold OOS Sharpe.
// ---------------------------------------------------------------------------
static atx::f64 blend_window_sharpe(const combine::AlphaStore& pool,
                                    const std::vector<atx::f64>& w,
                                    atx::usize begin, atx::usize end) {
    if (end <= begin) return 0.0;
    const atx::usize len = end - begin;
    std::vector<atx::f64> blend(len + 1U, 0.0);   // index 0 = structural zero
    const atx::usize na = w.size();
    for (atx::usize a = 0; a < na; ++a) {
        const auto pnl = pool.pnl(combine::AlphaId{static_cast<atx::u32>(a)});
        for (atx::usize i = 0; i < len; ++i) blend[i + 1U] += w[a] * pnl[begin + i];
    }
    namespace ev = atx::engine::eval;
    ev::ReturnMetricsCfg rmc{};
    const atx::f64 s = ev::compute_return_metrics(
        std::span<const atx::f64>{blend}, rmc).sharpe;
    return std::isfinite(s) ? s : 0.0;
}

// ---------------------------------------------------------------------------
// apply_conviction (D1.2 / T7 NEW-1) — the per-alpha conviction post-fit
// transform, WINDOWED. For each alpha it computes a combine-time conviction
// score (deflated-Sharpe probability + first/second-half Sharpe stability) from
// that alpha's OWN PnL RESTRICTED to [conv_begin, conv_end), scales weights[a]
// by the score, then renormalizes Σ|w| = 1.
//
// The window is the ONLY generalization over the original inline D1.2 block:
//   * MAIN path (W0-I0a, I-02): the FIT window [fit_begin, fit_end) under
//     ConvictionWindowRule::FitWindowV2 (default). FullStreamV1 passes the FULL
//     stream [0, np) -- byte-identical to the pre-W0 inline code, and a holdout
//     leak: the score read the out-of-sample PnL the report later calls "OOS".
//   * WF folds pass the fold's TRAIN window [fit_begin, train_end) (causal —
//     never the test window), so walk_forward_oos_sharpe reflects the shipped
//     conviction-weighted book per fold.
//
// Determinism: order-fixed alpha loop, no RNG/alloc beyond the window sub-span;
// identical inputs -> identical weights.
//
// S5-1 telemetry: when `out_scores != nullptr`, the per-alpha ConvictionScore
// (final score + clamped DSR / stability terms) is appended in AlphaId order so
// the caller can surface WHY each alpha was sized the way it was. The collector is
// PURE telemetry — it does not alter the weights, the renorm, or the order, so the
// main path (collector or not) computes byte-identical weights. The walk-forward
// folds pass nullptr (their scratch weights are not the shipped book's telemetry).
// ---------------------------------------------------------------------------
static void apply_conviction(const combine::AlphaStore& pool,
                             std::vector<atx::f64>& weights,
                             atx::usize conv_begin, atx::usize conv_end,
                             atx::usize na,
                             std::vector<combine::ConvictionScore>* out_scores = nullptr) {
    namespace ce = atx::engine::combine;
    namespace ev = atx::engine::eval;

    // Drop the PBO term (PBO is a per-RUN set statistic, NOT a per-alpha input we
    // have here) and renormalize the remaining weights to sum to 1:
    // conviction = w_dsr*DSR + w_stability*ratio.
    ce::ConvictionConfig ccfg{};
    const atx::f64 wsum = ccfg.w_dsr + ccfg.w_stability;
    ccfg.w_dsr       = ccfg.w_dsr / wsum;
    ccfg.w_stability = ccfg.w_stability / wsum;
    ccfg.w_pbo       = 0.0;

    ev::ReturnMetricsCfg rmc{};  // default convention (periods_per_year = 252)
    for (atx::usize a = 0; a < na; ++a) {
        // The alpha's PnL RESTRICTED to the conviction window [conv_begin, conv_end).
        // Full window (conv_begin=0, conv_end=np) == the whole pnl stream the
        // original inline D1.2 code used -> byte-identical main-path weights.
        const std::span<const atx::f64> full = pool.pnl(ce::AlphaId{static_cast<atx::u32>(a)});
        const atx::usize wlen = (conv_end > conv_begin) ? (conv_end - conv_begin) : 0U;
        const std::span<const atx::f64> pnl = full.subspan(conv_begin, wlen);
        const atx::usize T = pnl.size();

        // (1) DSR from the alpha's own PnL — mirror score_arm (cluster_eval.hpp:562-577):
        //     per-period sr_pp = mean/std_pop over r = pnl[1..T), REAL skew/excess-kurtosis,
        //     N = na (selection inflation across the na-alpha book we are combining).
        ev::DsrResult dsr{};
        if (T > 1U) {
            const std::span<const atx::f64> r{pnl.data() + 1, T - 1U};
            const atx::f64 skew = ev::skewness(r);
            const atx::f64 exk  = ev::excess_kurtosis(r);
            const ev::MeanStd ms = ev::mean_std_pop(r);
            const atx::f64 sr_pp = (ms.std > 0.0) ? ms.mean / ms.std : 0.0;
            dsr = ev::deflated_sharpe(sr_pp, r.size(), skew, exk,
                                      /*N=*/std::max<atx::usize>(na, 1), std::nullopt);
        }

        // (2) First/second-half Sharpe stability ratio (annualized sharpe via compute_return_metrics).
        atx::f64 ratio = 0.0;
        if (T >= 4U) {
            const atx::usize mid = T / 2;
            const atx::f64 sh1 = ev::compute_return_metrics(pnl.subspan(0, mid), rmc).sharpe;
            const atx::f64 sh2 = ev::compute_return_metrics(pnl.subspan(mid), rmc).sharpe;
            if (std::isfinite(sh1) && std::isfinite(sh2) && std::fabs(sh1) > 1e-9) {
                ratio = sh2 / sh1;
            }
        }

        // w_pbo == 0.0 so the pbo field is unused; construct a valid zero PboResult.
        const ev::PboResult pbo{/*pbo=*/0.0, /*split_logits=*/{}, /*mean_logit=*/0.0};
        const ce::ConvictionScore cs =
            ce::conviction(dsr, pbo, ratio, ce::ExplainFlag::PartlyExplained, ccfg);
        weights[a] *= cs.score;
        if (out_scores != nullptr) {
            out_scores->push_back(cs); // S5-1: per-alpha breakdown for KV telemetry
        }
    }
    // Renormalize so Σ|w| = 1 (gross-exposure target maintained).
    ce::detail::renorm_abs_sum(weights);
}

// ---------------------------------------------------------------------------
// W0-I0a (I-03): this function and alpha_max_participation below are
// CapacityRule::FullPeriodHoldingsV1 -- kept verbatim so frozen artifacts re-derive.
// The default is alpha_capacity_trades (fit window, trades, trailing PIT liquidity).
//
// alpha_capacity_aum — the per-alpha CAPACITY AUM in dollars (T6): the AUM at
// which alpha `a`'s LAST-period target book's temporary √-impact erodes its gross
// frictionless edge to zero. This is the SAME capacity notion risk::capacity_curve
// reports (the net-edge zero-crossing AUM), but computed directly over the
// date-major alpha::Panel the combine stage already holds — there is no
// alpha::Panel -> loop::PanelView adapter, so we mirror risk/capacity.hpp's
// participation arithmetic locally (the SAME kAdvWindow=20 / kVolWindow=60 windows
// and the SAME √-impact form), reading the impact-bearing sim's OWN ImpactCfg
// coefficients (one cost surface — exactly book_cost_bps's strategy in
// factory/fitness.cpp, the documented "SAME participation/ADV/σ sizing arithmetic
// risk::capacity_curve uses").
//
// CRITICAL: the `impact` here MUST be an impact-BEARING ImpactCfg (engine default
// ImpactCfg{}, Y=1.0). The combine stage's `frictionless_sim()` zeroes Y, which
// makes every √-impact term 0 -> cost 0 -> the net edge never crosses zero ->
// capacity = +inf for EVERY alpha -> a uniform cap_scale that washes out (the D3c
// no-op). We pass the DEFAULT impact model so capacity is finite and per-name.
//
// Closed form (no aum_grid needed): cost_bps(aum) = C·aum^delta exactly, because
// every name's participation part_i ∝ aum, so
//   cost_bps(aum) = aum^delta · [ 1e4·Σ_i |w_i|·Y·σ_i·(|w_i|/(price_i·ADV_i))^delta ]
//                 = C · aum^delta   (C is the AUM-independent bracket).
// The net edge gross − C·aum^delta crosses zero at aum_cap = (gross / C)^(1/delta).
// Returns:
//   +inf  — C <= 0 (no priced/liquid name contributes impact: the book is
//           effectively frictionless, unbounded capacity) — matches
//           cost::capacity_point's "net edge never reaches zero" sentinel.
//   0.0   — gross <= 0 (the alpha has no positive frictionless edge to erode; a
//           name with zero remaining capacity is dropped by decorrelate_weights'
//           cap_scale==0 rail). This is the conservative "no edge -> no capacity".
//   (gross/C)^(1/delta) otherwise — the finite capacity AUM.
// PURE, NO RNG, order-fixed (ascending date, ascending instrument). Mirrors the
// guards of book_cost_bps EXACTLY: dead/NaN weight, non-positive price, zero ADV,
// zero participation, or zero σ makes a name contribute nothing.
// ---------------------------------------------------------------------------
static atx::f64 alpha_capacity_aum(const alpha::AlphaStreams& streams, atx::usize a,
                                   std::span<const atx::f64> close,
                                   std::span<const atx::f64> volume, atx::usize dates,
                                   atx::usize insts,
                                   const atx::engine::exec::ImpactCfg& impact) {
    constexpr atx::usize kAdvWindow = 20U; // dollar-ADV lookback (P4-6 adv20)
    constexpr atx::usize kVolWindow = 60U; // return-volatility lookback (P4-6 vol)
    if (dates < 2U || insts == 0U) {
        return std::numeric_limits<atx::f64>::infinity();
    }
    // Per-step return ret_i(t) = close(t,i)/close(t-1,i) − 1 over the date-major
    // close column. A NaN/non-positive prior close yields 0 (no return term).
    const auto step_return = [&](atx::usize t, atx::usize i) -> atx::f64 {
        const atx::f64 prev = close[(t - 1U) * insts + i];
        const atx::f64 cur  = close[t * insts + i];
        if (std::isnan(prev) || std::isnan(cur) || prev <= 0.0) return 0.0;
        return cur / prev - 1.0;
    };
    // Dollar ADV of i: mean close*volume over the newest kAdvWindow rows. Skips NaN.
    const auto dollar_adv = [&](atx::usize i) -> atx::f64 {
        const atx::usize start = (dates > kAdvWindow) ? (dates - kAdvWindow) : 0U;
        atx::f64 sum = 0.0; atx::usize n = 0U;
        for (atx::usize t = start; t < dates; ++t) {
            const atx::f64 c = close[t * insts + i];
            const atx::f64 v = volume[t * insts + i];
            if (!std::isnan(c) && !std::isnan(v)) { sum += c * v; ++n; }
        }
        return (n == 0U) ? 0.0 : sum / static_cast<atx::f64>(n);
    };
    // Population stddev of the newest kVolWindow per-step returns of i. Skips NaN;
    // 0 when < 2 valid returns. Window covers return rows [dates-w, dates), >= 1.
    const auto return_vol = [&](atx::usize i) -> atx::f64 {
        const atx::usize start = (dates > kVolWindow) ? (dates - kVolWindow) : 1U;
        const atx::usize lo = (start < 1U) ? 1U : start;
        atx::f64 sum = 0.0; atx::usize n = 0U;
        for (atx::usize t = lo; t < dates; ++t) {
            const atx::f64 r = step_return(t, i);
            if (!std::isnan(r)) { sum += r; ++n; }
        }
        if (n < 2U) return 0.0;
        const atx::f64 mean = sum / static_cast<atx::f64>(n);
        atx::f64 ss = 0.0;
        for (atx::usize t = lo; t < dates; ++t) {
            const atx::f64 r = step_return(t, i);
            if (!std::isnan(r)) { const atx::f64 d = r - mean; ss += d * d; }
        }
        return std::sqrt(ss / static_cast<atx::f64>(n));
    };

    // BUG S6-1: original used last-period frozen weights over all history to estimate edge.
    // A mean-reversion alpha's terminal book is random-sign over history → gross_edge_bps≤0
    // → hard-zeros capacity (stage_combine.cpp:280-281) even when realized OOS edge is +1.93.
    // Fix: use pool.pnl(a) stream mean (actual realized per-period edge) as the edge estimate.

    // Realized per-period PnL mean (bps): mean of streams.pnl(a) over all periods.
    // streams.pnl(a) is the actual per-period blend PnL from extract_streams, NOT the
    // frozen-book approximation. No .mean() on std::span — iterate manually.
    const std::span<const atx::f64> pnl = streams.pnl(a);
    const atx::usize np = pnl.size();
    atx::f64 pnl_sum = 0.0; atx::usize pnl_n = 0U;
    for (atx::usize t = 0U; t < np; ++t) {
        const atx::f64 p = pnl[t];
        if (std::isfinite(p)) { pnl_sum += p; ++pnl_n; }
    }
    // The realized mean is already in PnL units (not bps); scale to bps to match the
    // downstream capacity formula (gross_edge_bps feeds the C·aum^delta zero-crossing).
    const atx::f64 gross_edge_bps =
        (pnl_n == 0U) ? 0.0 : 1.0e4 * (pnl_sum / static_cast<atx::f64>(pnl_n));
    if (gross_edge_bps <= 0.0) {
        return 0.0; // realized OOS edge is genuinely ≤ 0 → conservative zero capacity
    }

    // The alpha's LAST-period target weights (the capacity_for_alpha convention:
    // the most recent rebalance is what is sized to target_aum). Used below only for
    // the cost bracket C — the edge estimate above no longer depends on these.
    const std::span<const atx::f64> w = streams.positions(a, streams.n_periods() - 1U);
    const atx::usize n = (insts < w.size()) ? insts : w.size();

    // The AUM-independent cost bracket C: cost_bps(aum) = C·aum^delta, with
    //   C = 1e4 · Σ_i |w_i| · Y · σ_i · (|w_i|/ADV_i)^delta.
    // S3-SEAM (p8, fix handed off by S4-1): participation is NOTIONAL/DOLLAR-ADV
    // (unitless) — `abs_w` is already a FRACTION of book (not a share count), so
    // dividing by `price` on top of `adv` (itself a DOLLAR-ADV, Σ close·volume)
    // was off by a factor of `price` (identical shape to the S4-1 bug fixed in
    // risk/capacity.hpp and factory/fitness.cpp). `price` is still READ below to
    // gate out unpriced names (no book value) but no longer enters the ratio.
    atx::f64 C = 0.0;
    for (atx::usize i = 0U; i < n; ++i) {
        const atx::f64 wi = w[i];
        if (std::isnan(wi) || wi == 0.0) continue;
        const atx::f64 abs_w = (wi < 0.0) ? -wi : wi;
        const atx::f64 price = close[(dates - 1U) * insts + i]; // newest mark
        if (std::isnan(price) || price <= 0.0) continue;
        const atx::f64 adv = dollar_adv(i);
        if (adv <= 0.0) continue;
        const atx::f64 sigma = return_vol(i);
        if (sigma <= 0.0) continue;
        const atx::f64 part_per_aum = abs_w / adv; // part_i = part_per_aum·aum (S3-SEAM fix)
        if (part_per_aum <= 0.0) continue;
        C += abs_w * impact.Y * sigma * std::pow(part_per_aum, impact.delta);
    }
    C *= 1.0e4;
    if (C <= 0.0) {
        return std::numeric_limits<atx::f64>::infinity(); // frictionless book
    }
    // Zero-crossing: gross = C·aum_cap^delta  =>  aum_cap = (gross/C)^(1/delta).
    return std::pow(gross_edge_bps / C, 1.0 / impact.delta);
}

// ---------------------------------------------------------------------------
// alpha_max_participation — the max per-name participation part_i =
// (target_aum·|w_i|)/ADV_i over alpha `a`'s LAST-period book at `target_aum`. A
// pure liquidity-footprint telemetry figure (the single most liquidity-stressed
// name in the book): 0 when no name has finite price+ADV. Mirrors
// book_cost_bps's guards. S3-SEAM (p8, fix handed off by S4-1): participation
// is notional/dollar-ADV (unitless) — `price` no longer enters the ratio (see
// alpha_capacity_aum's identical fix above for the full rationale).
// ---------------------------------------------------------------------------
static atx::f64 alpha_max_participation(const alpha::AlphaStreams& streams, atx::usize a,
                                        std::span<const atx::f64> close,
                                        std::span<const atx::f64> volume, atx::usize dates,
                                        atx::usize insts, atx::f64 target_aum) {
    constexpr atx::usize kAdvWindow = 20U;
    if (dates < 1U || insts == 0U || target_aum <= 0.0) return 0.0;
    const auto dollar_adv = [&](atx::usize i) -> atx::f64 {
        const atx::usize start = (dates > kAdvWindow) ? (dates - kAdvWindow) : 0U;
        atx::f64 sum = 0.0; atx::usize n = 0U;
        for (atx::usize t = start; t < dates; ++t) {
            const atx::f64 c = close[t * insts + i];
            const atx::f64 v = volume[t * insts + i];
            if (!std::isnan(c) && !std::isnan(v)) { sum += c * v; ++n; }
        }
        return (n == 0U) ? 0.0 : sum / static_cast<atx::f64>(n);
    };
    const std::span<const atx::f64> w = streams.positions(a, streams.n_periods() - 1U);
    const atx::usize n = (insts < w.size()) ? insts : w.size();
    atx::f64 max_part = 0.0;
    for (atx::usize i = 0U; i < n; ++i) {
        const atx::f64 wi = w[i];
        if (std::isnan(wi) || wi == 0.0) continue;
        const atx::f64 abs_w = (wi < 0.0) ? -wi : wi;
        const atx::f64 price = close[(dates - 1U) * insts + i];
        if (std::isnan(price) || price <= 0.0) continue;
        const atx::f64 adv = dollar_adv(i);
        if (adv <= 0.0) continue;
        const atx::f64 part = (target_aum * abs_w) / adv; // S3-SEAM fix (drop /price)
        if (part > max_part) max_part = part;
    }
    return max_part;
}

// ---------------------------------------------------------------------------
// W0-I0a (I-03) — CapacityRule::TrailingPitTradesV2.
//
// LiquidityPanels holds, for every date t in [begin, end), the trailing dollar ADV
// (mean close*volume over rows [t-19, t], NaN cells skipped) and the trailing return
// volatility (population std of the step returns over rows [max(1, t-59), t], a
// NaN/non-positive prior close contributing a 0 return exactly as alpha_capacity_aum
// does). Both read rows <= t only, so a cell is point-in-time at its own date; the
// panels are computed ONCE per combine run over the fit window and shared by every
// alpha and every walk-forward fold (which all live inside the fit window).
// ---------------------------------------------------------------------------
struct LiquidityPanels {
    atx::usize begin = 0;
    atx::usize end = 0;
    atx::usize insts = 0;
    std::span<const atx::f64> close;
    std::vector<atx::f64> dollar_adv; // (t - begin) * insts + i
    std::vector<atx::f64> vol;        // (t - begin) * insts + i

    [[nodiscard]] atx::f64 adv_at(atx::usize t, atx::usize i) const noexcept {
        return dollar_adv[(t - begin) * insts + i];
    }
    [[nodiscard]] atx::f64 vol_at(atx::usize t, atx::usize i) const noexcept {
        return vol[(t - begin) * insts + i];
    }
};

static LiquidityPanels build_liquidity_panels(std::span<const atx::f64> close,
                                              std::span<const atx::f64> volume, atx::usize insts,
                                              atx::usize begin, atx::usize end) {
    constexpr atx::usize kAdvWindow = 20U; // same windows as alpha_capacity_aum (V1)
    constexpr atx::usize kVolWindow = 60U;
    LiquidityPanels liq;
    liq.begin = begin;
    liq.end = end;
    liq.insts = insts;
    liq.close = close;
    const atx::usize rows = (end > begin) ? (end - begin) : 0U;
    liq.dollar_adv.assign(rows * insts, 0.0);
    liq.vol.assign(rows * insts, 0.0);
    const auto step_return = [&](atx::usize t, atx::usize i) -> atx::f64 {
        const atx::f64 prev = close[(t - 1U) * insts + i];
        const atx::f64 cur = close[t * insts + i];
        if (std::isnan(prev) || std::isnan(cur) || prev <= 0.0) return 0.0;
        return cur / prev - 1.0;
    };
    for (atx::usize t = begin; t < end; ++t) {
        const atx::usize adv_lo = (t + 1U > kAdvWindow) ? (t + 1U - kAdvWindow) : 0U;
        const atx::usize vol_lo =
            std::max<atx::usize>(1U, (t + 1U > kVolWindow) ? (t + 1U - kVolWindow) : 0U);
        for (atx::usize i = 0; i < insts; ++i) {
            atx::f64 sum = 0.0;
            atx::usize n = 0U;
            for (atx::usize u = adv_lo; u <= t; ++u) {
                const atx::f64 c = close[u * insts + i];
                const atx::f64 v = volume[u * insts + i];
                if (!std::isnan(c) && !std::isnan(v)) { sum += c * v; ++n; }
            }
            liq.dollar_adv[(t - begin) * insts + i] =
                (n == 0U) ? 0.0 : sum / static_cast<atx::f64>(n);
            if (t < 1U) {
                continue; // no return exists at date 0
            }
            atx::f64 rs = 0.0;
            atx::usize rn = 0U;
            for (atx::usize u = vol_lo; u <= t; ++u) { rs += step_return(u, i); ++rn; }
            if (rn < 2U) {
                continue;
            }
            const atx::f64 mean = rs / static_cast<atx::f64>(rn);
            atx::f64 ss = 0.0;
            for (atx::usize u = vol_lo; u <= t; ++u) {
                const atx::f64 d = step_return(u, i) - mean;
                ss += d * d;
            }
            liq.vol[(t - begin) * insts + i] = std::sqrt(ss / static_cast<atx::f64>(rn));
        }
    }
    return liq;
}

struct AlphaCapacity {
    atx::f64 aum = 0.0;      // capacity AUM (dollars); +inf when frictionless
    atx::f64 max_part = 0.0; // largest single trade / dollar ADV at target_aum
};

// The capacity AUM of alpha `a` over the window [fb, fe) (fb >= liq.begin, fe <= liq.end):
//   edge   = 1e4 * mean realized PnL over [fb, fe)                         (bps / period)
//   cost_t = 1e4 * sum_i |dw_i| * Y * sigma_i(t) * (|dw_i| / ADV_i(t))^delta, dw = w_t - w_{t-1}
//   C      = mean_t cost_t,  so the mean per-period cost at AUM A is C * A^delta
//   aum    = (edge / C)^(1/delta)   (0 when edge <= 0, +inf when C == 0)
// Only TRADES pay impact: an alpha that holds its book pays nothing, however large the
// book. w_{t-1} at t == 0 is the flat book. Guards mirror alpha_capacity_aum: a NaN weight
// counts as 0; an unpriced name, zero ADV or zero volatility contributes no cost.
static AlphaCapacity alpha_capacity_trades(const alpha::AlphaStreams& streams, atx::usize a,
                                           const LiquidityPanels& liq,
                                           const atx::engine::exec::ImpactCfg& impact,
                                           atx::usize fb, atx::usize fe, atx::f64 target_aum) {
    AlphaCapacity out;
    const std::span<const atx::f64> pnl = streams.pnl(a);
    atx::f64 pnl_sum = 0.0;
    atx::usize pnl_n = 0U;
    for (atx::usize t = fb; t < fe; ++t) {
        if (std::isfinite(pnl[t])) { pnl_sum += pnl[t]; ++pnl_n; }
    }
    const atx::f64 gross_edge_bps =
        (pnl_n == 0U) ? 0.0 : 1.0e4 * (pnl_sum / static_cast<atx::f64>(pnl_n));
    const atx::usize n = std::min(liq.insts, streams.n_instruments());
    const auto weight = [](atx::f64 w) { return std::isnan(w) ? 0.0 : w; };
    atx::f64 c_sum = 0.0;
    for (atx::usize t = fb; t < fe; ++t) {
        const std::span<const atx::f64> w = streams.positions(a, t);
        for (atx::usize i = 0; i < n; ++i) {
            const atx::f64 prev = (t == 0U) ? 0.0 : weight(streams.positions(a, t - 1U)[i]);
            const atx::f64 dw = std::fabs(weight(w[i]) - prev);
            if (dw == 0.0) continue;
            const atx::f64 price = liq.close[t * liq.insts + i];
            if (std::isnan(price) || price <= 0.0) continue;
            const atx::f64 adv = liq.adv_at(t, i);
            if (adv <= 0.0) continue;
            const atx::f64 sigma = liq.vol_at(t, i);
            if (sigma <= 0.0) continue;
            const atx::f64 part_per_aum = dw / adv;
            c_sum += dw * impact.Y * sigma * std::pow(part_per_aum, impact.delta);
            out.max_part = std::max(out.max_part, target_aum * part_per_aum);
        }
    }
    if (gross_edge_bps <= 0.0) {
        out.aum = 0.0; // no positive edge over the window -> no capacity
        return out;
    }
    const atx::usize periods = (fe > fb) ? (fe - fb) : 1U;
    const atx::f64 C = 1.0e4 * c_sum / static_cast<atx::f64>(periods);
    out.aum = (C <= 0.0) ? std::numeric_limits<atx::f64>::infinity()
                         : std::pow(gross_edge_bps / C, 1.0 / impact.delta);
    return out;
}

// ---------------------------------------------------------------------------
// windowed_pool (S3-1) — a sub-pool covering ONLY [fit_begin, fit_end)
// periods, re-indexed so window-local period 0 == fit_begin.
//
// meta_features_from_pool (learn/ensemble.hpp) has no fit-window parameter of
// its own — it always emits one row per (date, instrument) cell over the
// WHOLE pool it is given. To give the stack the SAME [fit_begin, fit_end)
// firewall the linear AlphaCombiner::fit enforces (window_span reads only
// pnl(id).subspan(fit_begin, T)), the stack's meta must be built from a pool
// that ONLY CONTAINS that window — so this function re-slices the
// already-PIT-correct full pool's pnl/positions (a pure COPY of a sub-span,
// not a recomputation) into a fresh AlphaStore. PURE, order-fixed (ascending
// alpha, ascending period).
// ---------------------------------------------------------------------------
static atx::core::Result<combine::AlphaStore>
windowed_pool(const combine::AlphaStore& pool, atx::usize fit_begin, atx::usize fit_end) {
    combine::AlphaStore out;
    const atx::usize ni = pool.n_instruments();
    const atx::usize wlen = fit_end - fit_begin;
    for (atx::usize a = 0; a < pool.n_alphas(); ++a) {
        const auto id = combine::AlphaId{static_cast<atx::u32>(a)};
        const auto full_pnl = pool.pnl(id);
        const std::vector<atx::f64> pnl_w(full_pnl.begin() + static_cast<std::ptrdiff_t>(fit_begin),
                                          full_pnl.begin() + static_cast<std::ptrdiff_t>(fit_end));
        std::vector<atx::f64> pos_w;
        pos_w.reserve(wlen * ni);
        for (atx::usize t = 0; t < wlen; ++t) {
            const auto cs = pool.positions(id, fit_begin + t);
            pos_w.insert(pos_w.end(), cs.begin(), cs.end());
        }
        ATX_TRY(auto new_id, out.insert(/*source*/nullptr, pnl_w, pos_w, pool.get(id).metrics));
        (void)new_id;
    }
    return atx::core::Ok(std::move(out));
}

// ---------------------------------------------------------------------------
// build_forward_returns_window (S3-1) — the forward-return label the stack's
// meta consumes, period-major then instrument-minor, over the SAME
// [fit_begin, fit_end) window as windowed_pool (window-local index t in
// [0, fit_end-fit_begin)).
//
// PIT firewall: row t's label reads close at (fit_begin+t) and
// (fit_begin+t+horizon) ONLY WHEN fit_begin+t+horizon < fit_end — i.e. it
// NEVER reads a panel row >= fit_end even though the full research panel may
// carry real data there (the report stage's OOS window). The window's own
// tail (the last `horizon` rows) therefore carries NaN, exactly mirroring
// FeatureMatrix's own forward_return contract ("the tail is unknowable, NOT
// zero") — meta_features_from_pool only reads this array when
// fit_begin+t+horizon < windowed.n_periods() (== fit_end-fit_begin), so the
// NaN fill here is a belt-and-suspenders match to that guard, not load-bearing
// on its own.
// ---------------------------------------------------------------------------
static std::vector<atx::f64>
build_forward_returns_window(std::span<const atx::f64> close_all, atx::usize ni,
                             atx::usize fit_begin, atx::usize fit_end, atx::u16 horizon) {
    const atx::usize wlen = fit_end - fit_begin;
    std::vector<atx::f64> out(wlen * ni, std::numeric_limits<atx::f64>::quiet_NaN());
    const atx::usize h = static_cast<atx::usize>(horizon);
    for (atx::usize t = 0; t < wlen; ++t) {
        const atx::usize d = fit_begin + t;
        if (d + h >= fit_end) {
            continue; // tail: unknowable inside the window (PIT — never read fit_end+)
        }
        for (atx::usize i = 0; i < ni; ++i) {
            const atx::f64 now = close_all[d * ni + i];
            const atx::f64 fut = close_all[(d + h) * ni + i];
            out[t * ni + i] = (std::isnan(now) || std::isnan(fut) || now == 0.0)
                                  ? std::numeric_limits<atx::f64>::quiet_NaN()
                                  : (fut / now - 1.0);
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// weights_from_stack_projection (S3-1) — the stack -> per-alpha-weight bridge.
//
// fit_stack/stack_to_candidate produce a per-(date,instrument) predicted
// POSITION stream (stack_pos_flat, over `windowed`'s window), not a per-alpha
// weight vector — but every downstream consumer of `combo.weights` (the
// combined mega-alpha blend at step 9, --conviction, --kelly-fraction,
// --corr-penalty/--capacity-floor, the breadth telemetry) is built around
// "one weight per pool alpha". DECISION (documented, pinned by the twice-run
// test): ship the CONSERVATIVE order-fixed least-squares projection of the
// stack's position stream onto the pool's own per-alpha position streams over
// the fit window, keeping the artifact shape identical to every other method
// — NOT sc.pos_flat directly, which would silently bypass every one of those
// downstream transforms for Stack/RegimeStack (a much larger behavior change
// than a wiring sprint should introduce; see the sprint-3 ledger's "stack->
// weight bridge" entry for the full risk comparison).
//
// Solve: w = argmin_w Sum_{t,i} (Sum_a w_a*pos_a(t,i) - stack_pos(t,i))^2, the
// normal-equations form  G w = rhs  with
//   G[a,b]  = Sum_{t,i} pos_a(t,i)*pos_b(t,i)      (the pool's own Gram matrix)
//   rhs[a]  = Sum_{t,i} pos_a(t,i)*stack_pos(t,i)
// solved via solve_spd on G + ridge_lambda*I (a tiny ridge floor — the SAME
// role cfg.ridge_lambda already plays for BoundedRegression — guarantees a
// well-defined solve even when two pool alphas are near-collinear over the
// window). Deterministic: ascending alpha/date/instrument sums, no RNG; ends
// with the SAME renorm_abs_sum every other method's Combination uses
// (Sum|w|=1). No new estimator math — reuses combine::detail::renorm_abs_sum
// and atx::core::linalg::solve_spd, both already-frozen public helpers.
// ---------------------------------------------------------------------------
static atx::core::Result<std::vector<atx::f64>>
weights_from_stack_projection(const combine::AlphaStore& windowed,
                              std::span<const atx::f64> stack_pos_flat,
                              atx::f64 ridge_lambda) {
    using atx::core::linalg::MatX;
    using atx::core::linalg::VecX;
    const atx::usize na = windowed.n_alphas();
    const atx::usize wp = windowed.n_periods();
    const atx::usize ni = windowed.n_instruments();

    MatX g = MatX::Zero(static_cast<Eigen::Index>(na), static_cast<Eigen::Index>(na));
    VecX rhs = VecX::Zero(static_cast<Eigen::Index>(na));
    for (atx::usize a = 0; a < na; ++a) {
        const auto id_a = combine::AlphaId{static_cast<atx::u32>(a)};
        for (atx::usize b = a; b < na; ++b) {
            const auto id_b = combine::AlphaId{static_cast<atx::u32>(b)};
            atx::f64 sum = 0.0;
            for (atx::usize t = 0; t < wp; ++t) {
                const auto pa = windowed.positions(id_a, t);
                const auto pb = windowed.positions(id_b, t);
                for (atx::usize i = 0; i < ni; ++i) {
                    sum += pa[i] * pb[i];
                }
            }
            g(static_cast<Eigen::Index>(a), static_cast<Eigen::Index>(b)) = sum;
            g(static_cast<Eigen::Index>(b), static_cast<Eigen::Index>(a)) = sum;
        }
        atx::f64 r = 0.0;
        for (atx::usize t = 0; t < wp; ++t) {
            const auto pa = windowed.positions(id_a, t);
            for (atx::usize i = 0; i < ni; ++i) {
                r += pa[i] * stack_pos_flat[t * ni + i];
            }
        }
        rhs(static_cast<Eigen::Index>(a)) = r;
    }
    for (Eigen::Index a = 0; a < g.rows(); ++a) {
        g(a, a) += ridge_lambda; // SPD floor (mirrors the BoundedRegression ridge role)
    }
    ATX_TRY(VecX w_raw, atx::core::linalg::solve_spd(g, rhs));
    std::vector<atx::f64> w(na);
    for (atx::usize a = 0; a < na; ++a) {
        w[a] = w_raw(static_cast<Eigen::Index>(a));
    }
    combine::detail::renorm_abs_sum(w); // Sum|w| = 1, the shared gross convention
    return atx::core::Ok(std::move(w));
}

// ---------------------------------------------------------------------------
// fit_shrinkage_mv_cleaned_cov (S3-4) — the opt-in ShrinkageMv weight fit that
// consumes atx::engine::data::cleaned_alpha_cov (the S1-shipped LW constant-
// correlation shrink + Marchenko-Pastur eigen-clip + strict-PD floor pipeline,
// factor_model_artifact.hpp's "combine-visible shrunk/denoised covariance")
// IN PLACE OF the raw complete-case MLE sample covariance, gated on
// `risk_cfg.kind == risk::RiskModelKind::Factor` at this function's ONE call
// site in run_combine (step 8a). Default `risk_cfg.kind == Diagonal` never
// calls this function at all -> combine::AlphaCombiner::fit's ShrinkageMv path
// (Ledoit-Wolf shrunk-to-scaled-identity, `fit_shrinkage_mv` in combiner.hpp)
// is untouched and the digest stays byte-identical to pre-S3-4.
//
// Deliberately NOT a modification of AlphaCombiner::fit's internal ShrinkageMv/
// Ledoit-Wolf pipeline (combiner.hpp is append-only under this sprint's
// ownership boundaries, and double-shrinking — LW-to-identity THEN MP-eigen-
// clipping the same matrix — would be an uncontrolled, undocumented estimator
// change). Instead this is a SEPARATE, PARALLEL weight fit reusing the exact
// same public inputs combiner.hpp's own fit_shrinkage_mv reads
// (`combine::detail::window_means` + `combine::detail::complete_case_centered`
// over the SAME [fit_begin, fit_end) window), swapping only the covariance
// source for the risk-model-cleaned one, then solving Σ̂w = μ via the SAME
// `solve_spd` + `renorm_abs_sum(Σ|w|=1)` convention every other method uses.
//
// PIT/firewall: identical to fit_shrinkage_mv — window_means/complete_case_
// centered read ONLY pool.pnl(id).subspan(fit_begin, t); cleaned_alpha_cov is a
// PURE function of that same `centered` matrix (data/factor_model_artifact.hpp
// — no RNG, no clock). Determinism: same inputs -> byte-identical weights,
// every run.
// ---------------------------------------------------------------------------
atx::core::Result<combine::Combination>
fit_shrinkage_mv_cleaned_cov(const combine::AlphaStore& pool, atx::usize fit_begin, atx::usize fit_end) {
    using atx::core::linalg::MatX;
    using atx::core::linalg::VecX;
    namespace data = atx::engine::data;

    const atx::usize n = pool.n_alphas();
    const atx::usize t = fit_end - fit_begin;
    const VecX mu = combine::detail::window_means(pool, n, fit_begin, t);
    const MatX centered = combine::detail::complete_case_centered(pool, n, fit_begin, t, mu);
    const MatX cov = data::cleaned_alpha_cov(centered); // S1's shrunk/denoised covariance (non-fallible)

    ATX_TRY(VecX raw, atx::core::linalg::solve_spd(cov, mu));
    std::vector<atx::f64> w(n);
    for (atx::usize i = 0; i < n; ++i) {
        w[i] = raw[static_cast<Eigen::Index>(i)];
    }
    combine::detail::renorm_abs_sum(w); // Sum|w| = 1, the shared gross convention

    combine::Combination combo;
    combo.weights = std::move(w);
    combo.fit_begin = fit_begin;
    combo.fit_end = fit_end;
    return atx::core::Ok(std::move(combo));
}

// ---------------------------------------------------------------------------
// fit_stack_combo (S3-1 producer / S3-2 the honest-gate / S3-3 the regime
// overlay) — declared in stage_combine.hpp so the decisive admit/reject/
// single-state fixtures can drive it directly against a HAND-BUILT pool
// (bypassing the DSL/VM entirely, the same technique ensemble_test.cpp uses
// for fit_stack itself), independent of whether a real alpha DSL expression
// happens to produce interaction/linear structure in its positions.
//
// Builds the meta over [fit_begin, fit_end) (windowed_pool + the PIT-causal
// forward-return label), then when `with_regime` is true (RegimeStack) fits a
// PIT HMM (via stage_regime.hpp's fit_regime_hmm) on
// learn::ensemble_detail::regime_observable(meta) — NOT a panel-close or
// macro-series proxy (see stage_regime.hpp's header note): fit_stack's
// regime-conditional arm RE-DERIVES that exact observable internally from
// `meta`, so the Hmm passed to it must have been fit on the SAME series or
// the composition is statistically meaningless. Then decides admit vs
// fallback — the SAME purged-CPCV-scored linear-vs-nonlinear gate fit_stack
// always computes (eval/cpcv.hpp — López de Prado AFML Ch. 7, purge+embargo
// cited at fit_stack's own call site in ensemble.cpp):
//
//   S3-2 the honest-gate (MANDATORY, not advisory, applies identically to
//   BOTH Stack and RegimeStack). Phase-D measured oos_pbo=0.79 (textbook
//   overfit) and a GBT is the highest-variance learner in the codebase — an
//   UNGATED stacking step would make the overfit WORSE, not better. ADMIT the
//   stack as the shipped combiner ONLY if verdict.admitted (the nonlinear
//   base beat linear OOS-after-deflation on the SAME folds/metric); ELSE fall
//   back to a LINEAR combiner with a FRESH DEFAULT CombinerConfig
//   (method==ShrinkageMv) — never combiner_cfg (whose .method is
//   Stack/RegimeStack and would hit AlphaCombiner::fit's S3-0 Err arm):
//     * Stack (with_regime==false): the flat AlphaCombiner{}.fit(pool,
//       fit_begin, fit_end) — byte-identical to today's `--method
//       shrinkage-mv` book (stack_rejects_on_linear_fixture proves this).
//     * RegimeStack (with_regime==true): the REGIME-CONDITIONAL linear blend
//       — combine::fit_regime_combiner(pool, labels, hmm.n_states, fit_begin,
//       fit_end, {}) partitioned by the SAME Hmm's PIT-argmax regime per
//       date (learn::ensemble_detail::date_regimes), blended at the
//       fit-boundary date by learn::regime_posterior_at(hmm, obs, wlen-1)
//       via RegimeCombiner::blend — so a pool whose optimal combination
//       DIFFERS by regime (mirrors BAB flipping sign) tracks the
//       regime-appropriate linear combo even when the nonlinear arm itself
//       does not admit.
//
//   n_regimes==1 (HmmCfg.n_states==1, combiner_cfg.regime_n_states) makes
//   BOTH arms above reduce to their Stack (with_regime==false) counterpart
//   EXACTLY: fit_regime_nonlinear's single-partition union equals
//   fit_flat_nonlinear (identical verdict); deploy_nonlinear never reads
//   regime at all (identical admit-path weights); and
//   fit_regime_combiner/blend with one all-zero-labeled regime is regime_
//   combiner.hpp's OWN documented byte-identical reduction to
//   AlphaCombiner{}.fit (identical fallback-path weights) — this composition
//   is the RegimeStack single-state fallback guard
//   (regime_stack_single_state_byte_identical), not a special case coded here.
// ---------------------------------------------------------------------------
atx::core::Result<StackFitResult>
fit_stack_combo(const combine::AlphaStore& pool, std::span<const atx::f64> close_all, atx::usize ni,
                atx::usize fit_begin, atx::usize fit_end, const combine::CombinerConfig& combiner_cfg,
                bool with_regime) {
    ATX_TRY(auto windowed, windowed_pool(pool, fit_begin, fit_end));
    const std::vector<atx::f64> fwd = build_forward_returns_window(
        close_all, ni, fit_begin, fit_end, combiner_cfg.stack_horizon);
    const std::array<atx::u16, 1> horizons{combiner_cfg.stack_horizon};
    const learn::FeatureMatrix meta = learn::meta_features_from_pool(
        windowed, std::span<const atx::f64>{fwd}, std::span<const atx::u16>{horizons});

    learn::StackingCfg scfg;
    scfg.master_seed = combiner_cfg.stack_master_seed;
    scfg.horizons = {combiner_cfg.stack_horizon};
    scfg.cpcv.n_groups = combiner_cfg.stack_cpcv_groups;
    scfg.cpcv.n_test_groups = combiner_cfg.stack_cpcv_test_groups;
    scfg.cpcv.embargo = combiner_cfg.stack_cpcv_embargo;

    // S3-3: fit the PIT regime HMM on the meta's OWN regime marker (the
    // frozen fit_stack's binding observable choice — see the doc block
    // above). `obs`/`hmm` stay default/unused when with_regime is false.
    atx::core::linalg::MatX obs;
    learn::Hmm hmm;
    const learn::Hmm* regime_ptr = nullptr;
    if (with_regime) {
        obs = learn::ensemble_detail::regime_observable(meta);
        learn::HmmCfg hcfg;
        hcfg.n_states = combiner_cfg.regime_n_states;
        hcfg.master_seed = combiner_cfg.stack_master_seed;
        hmm = fit_regime_hmm(obs, hcfg);
        regime_ptr = &hmm;
    }

    StackFitResult out;
    out.verdict = learn::fit_stack(meta, regime_ptr, scfg);
    if (out.verdict.admitted) {
        // deploy_nonlinear (inside stack_to_candidate) is ALWAYS the flat
        // refit regardless of regime_ptr (ensemble.hpp's own documented
        // contract) -- the admit-path weights are identical for Stack and a
        // (still-admitted) RegimeStack.
        const learn::StackCandidate sc = learn::stack_to_candidate(out.verdict, meta, scfg);
        ATX_TRY(auto w, weights_from_stack_projection(
                            windowed, std::span<const atx::f64>{sc.pos_flat}, combiner_cfg.ridge_lambda));
        out.combo.weights = std::move(w);
    } else if (!with_regime) {
        combine::AlphaCombiner linear_fallback; // default-constructed cfg: method == ShrinkageMv
        ATX_TRY(out.combo, linear_fallback.fit(pool, fit_begin, fit_end));
    } else {
        // RegimeStack fallback: the regime-conditional LINEAR blend. Labels
        // are WINDOW-LOCAL dates (length == windowed.n_periods()); place them
        // into a GLOBAL-length vector at offset fit_begin (fit_regime_combiner
        // requires length == pool.n_periods() but only reads [fit_begin,
        // fit_end) — positions outside the window are never consulted).
        const std::vector<atx::u32> window_labels = learn::ensemble_detail::date_regimes(hmm, obs);
        std::vector<atx::u32> global_labels(pool.n_periods(), 0U);
        for (atx::usize t = 0; t < window_labels.size(); ++t) {
            global_labels[fit_begin + t] = window_labels[t];
        }
        const combine::CombinerConfig lin_cfg{}; // default-constructed: method == ShrinkageMv
        ATX_TRY(auto rc, combine::fit_regime_combiner(pool, std::span<const atx::u32>{global_labels},
                                                       hmm.n_states, fit_begin, fit_end, lin_cfg));
        // The ship-time (PIT) posterior: "as of" the last in-window date,
        // mirroring every other method's [fit_begin, fit_end) convention.
        const atx::usize wlen_minus_1 =
            (window_labels.empty()) ? 0U : (window_labels.size() - 1U);
        const atx::core::linalg::VecX posterior =
            learn::regime_posterior_at(hmm, obs, wlen_minus_1);
        std::vector<atx::f64> posterior_vec(static_cast<atx::usize>(posterior.size()));
        for (Eigen::Index s = 0; s < posterior.size(); ++s) {
            posterior_vec[static_cast<atx::usize>(s)] = posterior(s);
        }
        out.combo.weights = rc.blend(std::span<const atx::f64>{posterior_vec});
    }
    out.combo.fit_begin = fit_begin;
    out.combo.fit_end = fit_end;
    return atx::core::Ok(std::move(out));
}

// ---------------------------------------------------------------------------
// run_combine
// ---------------------------------------------------------------------------
// S2 (p9): builds RiskModelConfig from cfg.risk_model, mirroring stage_optimize.cpp's own
// run_optimize(const RunConfig&) seam (S1/S5-0, stage_optimize.cpp:40-48) field-for-field.
// Duplicated locally rather than shared: stage_optimize.cpp is not an S2-owned file, and this
// is a 3-line block, not an abstraction worth a new shared header. At the RunConfig{} default
// (risk_model=="diagonal") this constructs RiskModelConfig{} (kind==Diagonal) -- identical to
// the pre-S2 hardcode below -- so the no-flag path is byte-identical BY CONSTRUCTION.
static risk::RiskModelConfig risk_cfg_from_run_config(const RunConfig& cfg) {
    risk::RiskModelConfig risk_cfg{};
    risk_cfg.kind = (cfg.risk_model == "factor") ? risk::RiskModelKind::Factor
                                                  : risk::RiskModelKind::Diagonal;
    // dead_alpha_factors / group_neutralize are deliberately NOT copied from cfg here: the
    // S3-4 Factor branch this feeds (fit_shrinkage_mv_cleaned_cov) reads ONLY risk_cfg.kind --
    // it never calls build_risk_model/extract_dead_factors, so those two fields have no
    // observable effect on this path (they exist only for stage_optimize's build_risk_model
    // call). Leaving them at their RiskModelConfig{} default (false) keeps this function
    // honest about what it actually threads.
    return risk_cfg;
}

// ---------------------------------------------------------------------------
// W0-I0a (I-08): the ONE weight dispatch every fitted book goes through -- the shipped
// combo AND every walk-forward fold -- so a fold scores exactly the book that would
// have shipped had the fit window ended at its train_end.
//
//   fit (8a)    Stack/RegimeStack -> fit_stack_combo; ShrinkageMv + Factor risk ->
//               fit_shrinkage_mv_cleaned_cov; else AlphaCombiner{combiner_cfg}.fit
//   conviction  (--conviction) apply_conviction over [conv_begin, conv_end)
//   Kelly       (--kelly-fraction) diagonal-covariance fractional Kelly over [fb, fe)
//   crowding    (--corr-penalty / --capacity-floor) decorrelate_weights over [fb, fe),
//               with the per-alpha capacity AUM of CombinePitConfig::capacity
//
// Every step reads pool rows in [fb, fe) only, except conviction under FullStreamV1
// (the caller passes [0, n)) and capacity under FullPeriodHoldingsV1 (whole panel).
// ---------------------------------------------------------------------------
struct ShipInputs {
    const RunConfig& cfg;
    const combine::CombinerConfig& combiner_cfg;
    const risk::RiskModelConfig& risk_cfg;
    const CombinePitConfig& pit;
    const combine::AlphaStore& pool;
    const alpha::AlphaStreams& streams;
    std::span<const atx::f64> close;  // research "close" (date-major); empty if absent
    std::span<const atx::f64> volume; // research "volume"; empty if absent
    atx::usize dates = 0;
    atx::usize insts = 0;
    const LiquidityPanels* liq = nullptr; // TrailingPitTradesV2 inputs (capacity on only)
};

struct ShippedWeights {
    combine::Combination combo;
    bool stack_on = false;
    learn::StackingVerdict verdict{};
    std::vector<combine::ConvictionScore> conviction_scores;
    std::string kelly_fraction_used;
    std::string kelly_gross;
    std::string kelly_scale_applied;
    bool capacity_on = false;
    std::vector<atx::f64> capacity_aum;
    atx::f64 capacity_max_participation = 0.0;
};

static atx::core::Status apply_kelly(const ShipInputs& in, ShippedWeights& out, atx::usize fb,
                                     atx::usize fe) {
    using atx::core::linalg::MatX;
    using atx::core::linalg::VecX;
    const atx::usize na = in.pool.n_alphas();
    ATX_ASSERT(out.combo.weights.size() == na); // one weight per pool alpha (step-9 invariant)
    // Per-alpha realized mean PnL (mu) and population variance (the diagonal D) over the fit
    // window. A degenerate (zero-variance) alpha is floored by FactorModel::create.
    const atx::usize wlen = (fe > fb) ? (fe - fb) : 0U;
    VecX mu(static_cast<Eigen::Index>(na));
    VecX d(static_cast<Eigen::Index>(na));
    for (atx::usize a = 0; a < na; ++a) {
        const std::span<const atx::f64> pnl =
            in.pool.pnl(combine::AlphaId{static_cast<atx::u32>(a)});
        atx::f64 sum = 0.0;
        atx::usize n = 0U;
        for (atx::usize i = 0; i < wlen; ++i) {
            const atx::f64 p = pnl[fb + i];
            if (std::isfinite(p)) { sum += p; ++n; }
        }
        const atx::f64 mean = (n == 0U) ? 0.0 : sum / static_cast<atx::f64>(n);
        atx::f64 ss = 0.0;
        for (atx::usize i = 0; i < wlen; ++i) {
            const atx::f64 p = pnl[fb + i];
            if (std::isfinite(p)) { const atx::f64 dv = p - mean; ss += dv * dv; }
        }
        mu[static_cast<Eigen::Index>(a)] = mean;
        d[static_cast<Eigen::Index>(a)] = (n == 0U) ? 0.0 : ss / static_cast<atx::f64>(n);
    }
    // Per-alpha conviction in [0,1]: the conviction scores when --conviction is on, else 1.
    VecX conv(static_cast<Eigen::Index>(na));
    for (atx::usize a = 0; a < na; ++a) {
        atx::f64 c = 1.0;
        if (in.cfg.conviction && a < out.conviction_scores.size()) {
            c = out.conviction_scores[a].score;
        }
        conv[static_cast<Eigen::Index>(a)] = std::clamp(c, 0.0, 1.0);
    }
    // Diagonal FactorModel V = diag(D): zero exposures, K=1 with F=[1] (minimal SPD form).
    MatX x = MatX::Zero(static_cast<Eigen::Index>(na), 1);
    MatX f(1, 1);
    f(0, 0) = 1.0;
    auto fm = risk::FactorModel::create(std::move(x), std::move(f), d, fb, fe);
    if (!fm.has_value()) {
        return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                              "combine: --kelly-fraction could not build the diagonal "
                              "covariance: " + fm.error().message());
    }
    risk::KellyConfig kcfg{};
    kcfg.kelly_fraction = in.cfg.kelly_fraction;
    kcfg.max_gross = in.cfg.kelly_max_gross;
    const risk::KellyWeights kw = risk::kelly_size(mu, *fm, conv, kcfg);
    ATX_ASSERT(static_cast<atx::usize>(kw.weights.size()) == na);
    for (atx::usize a = 0; a < na; ++a) {
        out.combo.weights[a] = kw.weights[static_cast<Eigen::Index>(a)];
    }
    out.kelly_fraction_used = std::to_string(in.cfg.kelly_fraction);
    out.kelly_gross = std::to_string(kw.gross);
    out.kelly_scale_applied = std::to_string(kw.scale_applied);
    return atx::core::Ok();
}

// The per-alpha capacity vector decorrelate_weights consumes (see step 8c's history in
// the pre-W0 comments: constant 1.0 when capacity scaling is off, real dollar capacity
// AUM with the impact-BEARING engine default ImpactCfg{} when it is on).
static std::vector<atx::f64> capacity_vector(const ShipInputs& in, ShippedWeights& out,
                                             atx::usize fb, atx::usize fe) {
    std::vector<atx::f64> capacity(in.pool.size(), 1.0);
    if (!(in.cfg.capacity_floor > 0.0 && in.cfg.target_aum > 0.0)) {
        return capacity;
    }
    const atx::engine::exec::ImpactCfg impact{}; // engine DEFAULT (Appendix-A), impact-bearing
    const bool liquid_data = !in.close.empty() && !in.volume.empty();
    for (atx::usize a = 0; a < in.streams.n_alphas(); ++a) {
        if (!liquid_data) {
            capacity[a] = std::numeric_limits<atx::f64>::infinity(); // no volume -> no bound
            continue;
        }
        if (in.pit.capacity == CapacityRule::FullPeriodHoldingsV1) {
            capacity[a] = alpha_capacity_aum(in.streams, a, in.close, in.volume, in.dates,
                                             in.insts, impact);
            const atx::f64 part = alpha_max_participation(in.streams, a, in.close, in.volume,
                                                          in.dates, in.insts, in.cfg.target_aum);
            out.capacity_max_participation = std::max(out.capacity_max_participation, part);
        } else {
            ATX_ASSERT(in.liq != nullptr && in.liq->begin <= fb && fe <= in.liq->end);
            const AlphaCapacity c =
                alpha_capacity_trades(in.streams, a, *in.liq, impact, fb, fe, in.cfg.target_aum);
            capacity[a] = c.aum;
            out.capacity_max_participation = std::max(out.capacity_max_participation, c.max_part);
        }
    }
    out.capacity_aum = capacity;
    out.capacity_on = true;
    return capacity;
}

static atx::core::Result<ShippedWeights>
fit_shipped_weights(const ShipInputs& in, atx::usize fb, atx::usize fe, atx::usize conv_begin,
                    atx::usize conv_end) {
    ShippedWeights out;
    const combine::CombineMethod cm = in.combiner_cfg.method;
    if (cm == combine::CombineMethod::Stack || cm == combine::CombineMethod::RegimeStack) {
        if (in.close.empty()) {
            return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                                  "combine: stacking needs a \"close\" field");
        }
        ATX_TRY(auto sfr, fit_stack_combo(in.pool, in.close, in.insts, fb, fe, in.combiner_cfg,
                                          cm == combine::CombineMethod::RegimeStack));
        out.combo = std::move(sfr.combo);
        out.verdict = sfr.verdict;
        out.stack_on = true;
    } else if (cm == combine::CombineMethod::ShrinkageMv &&
               in.risk_cfg.kind == risk::RiskModelKind::Factor) {
        ATX_TRY(out.combo, fit_shrinkage_mv_cleaned_cov(in.pool, fb, fe));
    } else {
        combine::AlphaCombiner combiner;
        combiner.cfg = in.combiner_cfg;
        ATX_TRY(out.combo, combiner.fit(in.pool, fb, fe));
    }
    if (in.cfg.conviction) {
        apply_conviction(in.pool, out.combo.weights, conv_begin, conv_end, in.pool.n_alphas(),
                         &out.conviction_scores);
    }
    if (in.cfg.kelly_fraction > 0.0) {
        ATX_TRY_VOID(apply_kelly(in, out, fb, fe));
    }
    if (in.cfg.corr_penalty > 0.0 || in.cfg.capacity_floor > 0.0) {
        const std::vector<atx::f64> capacity = capacity_vector(in, out, fb, fe);
        combine::CrowdingConfig ccfg{};
        ccfg.corr_penalty = in.cfg.corr_penalty;
        ccfg.capacity_floor = in.cfg.capacity_floor;
        out.combo.weights = combine::decorrelate_weights(out.combo.weights, in.pool, fb, fe,
                                                         std::span<const atx::f64>{capacity}, ccfg);
    }
    return atx::core::Ok(std::move(out));
}

// ---------------------------------------------------------------------------
// W0-I0a (I-01): the final-test guard against the library's split-range ledger.
// Refuses a final test [test_begin, n) that shares a date with any recorded discover
// train/holdout range (those alphas were selected on those dates, so a "test" there
// is in-sample), then records the test range so a later discover run refuses it.
// Returns "checked" (the ledger held discover ranges) or "unrecorded" (legacy library).
// ---------------------------------------------------------------------------
static atx::core::Result<std::string> guard_final_test(const std::string& lib_dir,
                                                       const PipelinePanel& input,
                                                       atx::usize test_begin, atx::usize n) {
    const std::span<const atx::i64> keys =
        input.identity ? std::span<const atx::i64>{input.identity->session_keys}
                       : std::span<const atx::i64>{};
    ATX_TRY(const SplitRange test, make_split_range(SplitRole::FinalTest, test_begin, n, n, keys));
    ATX_TRY(const auto ranges, read_split_ranges(lib_dir));
    bool any_discover = false;
    for (const SplitRange& r : ranges) {
        if (r.role == SplitRole::FinalTest) {
            continue;
        }
        any_discover = true;
        ATX_TRY(const bool overlap, split_ranges_overlap(test, r));
        if (overlap) {
            return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                "combine: final test [" + std::to_string(test_begin) + "," + std::to_string(n) +
                ") overlaps the " + std::string{split_role_name(r.role)} + " range [" +
                std::to_string(r.begin) + "," + std::to_string(r.end) +
                ") recorded in the library -- nested splits require discover < fit < test "
                "(the alphas were selected on those dates)");
        }
    }
    ATX_TRY_VOID(append_split_range(lib_dir, test));
    return atx::core::Ok(std::string{any_discover ? "checked" : "unrecorded"});
}

// ---------------------------------------------------------------------------
// resolve_nested_split (I-01) — see stage_combine.hpp.
// ---------------------------------------------------------------------------
atx::core::Result<NestedSplit> resolve_nested_split(atx::usize n_dates,
                                                    const NestedSplitConfig& cfg) {
    const auto bad = [&](const std::string& why) {
        return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                              "nested split: " + why + " (n_dates=" + std::to_string(n_dates) +
                                  ")");
    };
    if (!(cfg.test_frac > 0.0 && cfg.test_frac < 1.0) ||
        !(cfg.combine_frac > 0.0 && cfg.combine_frac < 1.0) ||
        !(cfg.test_frac + cfg.combine_frac < 1.0)) {
        return bad("test_frac and combine_frac must lie in (0, 1) and sum below 1");
    }
    const auto n_f = static_cast<atx::f64>(n_dates);
    const auto test_n = static_cast<atx::usize>(std::floor(cfg.test_frac * n_f));
    const auto fit_n = static_cast<atx::usize>(std::floor(cfg.combine_frac * n_f));
    if (test_n < 2U || fit_n < 2U || test_n + fit_n + 2U * cfg.embargo + 2U > n_dates) {
        return bad("too few dates for a discover, a fit and a test window of >= 2 dates each");
    }
    NestedSplit s;
    s.n_dates = n_dates;
    s.test_begin = n_dates - test_n;
    s.fit_end = s.test_begin - cfg.embargo;
    s.fit_begin = s.fit_end - fit_n;
    s.discover_end = s.fit_begin - cfg.embargo;
    return atx::core::Ok(s);
}

atx::core::Result<StageResult> run_combine(const RunConfig& cfg)
{
    return run_combine(cfg, CombinePitConfig{});
}

atx::core::Result<StageResult> run_combine(const RunConfig& cfg, const CombinePitConfig& pit)
{
    ATX_TRY(auto cm0, method_from_string(cfg.method));
    combine::CombinerConfig combiner_cfg0{};
    combiner_cfg0.method = cm0;
    return run_combine(cfg, combiner_cfg0, risk_cfg_from_run_config(cfg), pit);
}

atx::core::Result<StageResult> run_combine(const RunConfig& cfg,
                                           const combine::CombinerConfig& combiner_cfg)
{
    return run_combine(cfg, combiner_cfg, risk_cfg_from_run_config(cfg), CombinePitConfig{});
}

atx::core::Result<StageResult> run_combine(const RunConfig& cfg,
                                           const combine::CombinerConfig& combiner_cfg,
                                           const risk::RiskModelConfig& risk_cfg)
{
    return run_combine(cfg, combiner_cfg, risk_cfg, CombinePitConfig{});
}

atx::core::Result<StageResult> run_combine(const RunConfig& cfg,
                                           const combine::CombinerConfig& combiner_cfg,
                                           const risk::RiskModelConfig& risk_cfg,
                                           const CombinePitConfig& pit)
{
    // S3-4: risk_cfg.kind==Factor (opt-in, default Diagonal) is consumed at two
    // sites below — the ShrinkageMv cleaned-covariance weight fit (step 8a) and
    // the breadth-instrumentation covariance (step 12). See fit_shrinkage_mv_
    // cleaned_cov's doc block for the full rationale.

    // 1. Validate required flags. The alpha SOURCE is either the loose .dsl directory
    //    (--alphas) or, when --library-dir is set (8.B), the accumulated persistent
    //    library::Library. Exactly one source feeds the combine inputs; everything
    //    downstream (compile_batch + evaluate + the combine math) is identical.
    const bool from_library = !cfg.library_dir.empty();
    if (cfg.panel.empty() || cfg.combo_out.empty() ||
        (!from_library && cfg.alphas.empty())) {
        return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                              "combine: --panel, --combo-out, and one of "
                              "--alphas / --library-dir required");
    }

    // 2. Load the research panel.
    ATX_TRY(auto input, read_pipeline_panel(cfg.panel, cfg.allow_unidentified_panels));
    auto& panel = input.panel;
    ATX_TRY(auto output_guard, reserve_pipeline_output(cfg.combo_out, input.identity.has_value()));

    // 3. Collect the alpha DSL sources + a per-alpha label (sidecar provenance).
    //    Two interchangeable sources; both yield `dsl` (expression strings, in a
    //    deterministic order) and `labels` (the weights-sidecar provenance string).
    //    Bind the consumed expression and source-local identity, so relocating an
    //    identical library or DSL directory does not change downstream artifact IDs.
    //    The SHA covers the loaded text passed to compile_batch (including the
    //    existing loose-file whitespace normalization), not raw source file bytes.
    std::vector<std::string> dsl;
    std::vector<std::string> labels;
    if (from_library) {
        // 3a (8.B). Library-backed input: enumerate ALL admitted records in AlphaId
        //     order (the same deterministic order discover writes alpha_NNN.dsl in)
        //     and use each record's stored expression source — the unparse'd DSL the
        //     library persisted on admit. Re-opening is read-only here (no admit), so
        //     the gate floors are irrelevant; a default GateConfig suffices. No seeds
        //     are needed for a pure enumeration (the corr index is not consulted).
        namespace library = atx::engine::library;
        std::filesystem::path lib_path{cfg.library_dir};
        if (!std::filesystem::exists(lib_path) ||
            !std::filesystem::is_directory(lib_path)) {
            return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                                  "combine: --library-dir not found: " + cfg.library_dir);
        }
        library::Library liblib =
            library::Library::open(cfg.library_dir, combine::GateConfig{}, {});
        const atx::u64 n = liblib.n_alphas();
        if (n == 0) {
            return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                                  "combine: --library-dir has no admitted alphas: " +
                                  cfg.library_dir);
        }
        dsl.reserve(static_cast<atx::usize>(n));
        labels.reserve(static_cast<atx::usize>(n));
        for (atx::u64 a = 0; a < n; ++a) {
            const auto rec = liblib.get(library::AlphaId{static_cast<atx::u32>(a)});
            dsl.push_back(rec.provenance.expr_source);
            ATX_TRY(auto source_hash, atx::core::sha256_hex(dsl.back()));
            labels.push_back("lib:alpha_" + std::to_string(a) +
                             " dsl_sha256=" + source_hash);
        }
    } else {
        // 3b. Loose-.dsl input (backward compat): enumerate + sort .dsl files (sort for
        //     determinism), then read each, trimming trailing whitespace/newlines.
        std::vector<std::filesystem::path> dsl_paths;
        {
            std::filesystem::path alphas_dir{cfg.alphas};
            if (!std::filesystem::exists(alphas_dir) ||
                !std::filesystem::is_directory(alphas_dir)) {
                return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                                      "combine: --alphas directory not found: " + cfg.alphas);
            }
            for (const auto& entry : std::filesystem::directory_iterator(alphas_dir)) {
                if (entry.path().extension() == ".dsl") {
                    dsl_paths.push_back(entry.path());
                }
            }
        }
        std::sort(dsl_paths.begin(), dsl_paths.end());

        if (dsl_paths.empty()) {
            return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                                  "combine: no .dsl alphas found in --alphas dir");
        }

        dsl.reserve(dsl_paths.size());
        labels.reserve(dsl_paths.size());
        for (const auto& p : dsl_paths) {
            std::ifstream f{p};
            if (!f.is_open()) {
                return atx::core::Err(atx::core::ErrorCode::IoError,
                                      "combine: cannot open DSL file: " + p.string());
            }
            std::string contents{std::istreambuf_iterator<char>(f),
                                 std::istreambuf_iterator<char>()};
            // Trim trailing whitespace/newlines.
            while (!contents.empty() &&
                   (contents.back() == '\n' || contents.back() == '\r' ||
                    contents.back() == ' '  || contents.back() == '\t')) {
                contents.pop_back();
            }
            dsl.push_back(std::move(contents));
            ATX_TRY(auto source_hash, atx::core::sha256_hex(dsl.back()));
            labels.push_back("dsl:" + p.filename().generic_string() +
                             " dsl_sha256=" + source_hash);
        }
    }

    // 4. Compile all DSL sources as a multi-root batch Program.
    alpha::Library lib{};
    std::vector<std::string_view> views(dsl.begin(), dsl.end());
    ATX_TRY(auto program,
            alpha::compile_batch(std::span<const std::string_view>{views}, lib));

    // 5. Evaluate the batch program on the research panel.
    alpha::Engine engine{panel};
    ATX_TRY(auto signals, engine.evaluate(program));

    // 5b. Sector (industry) neutralization: per-alpha books are sector-demeaned so
    //     the mega-alpha expresses idiosyncratic views, not sector bets (WQ
    //     indneutralize). group_map empty (no "sector" field) -> neutralization off.
    std::vector<atx::u32> group_map;
    if (cfg.sector_neutral) {
        group_map = sector_group_map(panel);
    }

    // 6. Extract per-alpha PnL + position streams (sector-neutral when group_map set).
    WeightPolicy policy{};
    policy.industry_neutral = !group_map.empty();
    auto sim = frictionless_sim();
    ATX_TRY(auto streams,
            atx::engine::alpha::extract_streams(
                signals, policy, panel, sim,
                std::span<const atx::u32>{group_map}));

    // 7. Build the AlphaStore pool from real constituent streams.
    combine::AlphaStore pool;
    const atx::usize np = streams.n_periods();
    const atx::usize ni = streams.n_instruments();
    for (atx::usize a = 0; a < streams.n_alphas(); ++a) {
        std::vector<atx::f64> pos_flat;
        pos_flat.reserve(np * ni);
        for (atx::usize t = 0; t < np; ++t) {
            auto cs = streams.positions(a, t);
            pos_flat.insert(pos_flat.end(), cs.begin(), cs.end());
        }
        const auto m = combine::compute_metrics(
            streams.pnl(a), pos_flat, ni, /*book_size*/1.0);
        ATX_TRY(auto id, pool.insert(/*source*/nullptr, streams.pnl(a), pos_flat, m));
        (void)id;
    }

    // 8. Resolve method and fit window.
    const combine::CombineMethod cm = combiner_cfg.method;

    const atx::usize fit_begin =
        cfg.fit_begin > 0 ? static_cast<atx::usize>(cfg.fit_begin) : 0;
    // A2a — fit_end resolution. Three cases, in priority order:
    //   1. The user EXPLICITLY set --fit-end (in set_flags): that value wins,
    //      clamped to np (today's behavior, unchanged).
    //   2. Else --holdout-frac > 0: hold the last `oos_n = floor(frac*np)` periods
    //      OUT of the fit so report can score [fit_end, np) out-of-sample. The
    //      weights fit on [fit_begin, fit_end) and never see the OOS window.
    //   3. Else: fit_end = np (today's full-history default). With the flag at its
    //      0.0 default and no --fit-end, this is byte-identical to before A2a.
    atx::usize fit_end = np;
    if (cfg.set_flags.count("fit-end") != 0 && cfg.fit_end > 0 &&
        static_cast<atx::usize>(cfg.fit_end) <= np) {
        fit_end = static_cast<atx::usize>(cfg.fit_end);
    } else if (cfg.set_flags.count("fit-end") == 0 && cfg.combine_holdout_frac > 0.0) {
        const atx::usize oos_n =
            static_cast<atx::usize>(std::floor(cfg.combine_holdout_frac * static_cast<double>(np)));
        if (oos_n < 1 || oos_n >= np || (np - oos_n) < 2) {
            return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                "combine: --holdout-frac leaves too few in-sample/out-of-sample periods (np=" +
                std::to_string(np) + ")");
        }
        fit_end = np - oos_n;     // weights fit on [fit_begin, fit_end); OOS = [fit_end, np)
    }
    // Existing guard: fit_end never exceeds np (all branches above already respect it).
    if (fit_end > np) fit_end = np;

    // W0-I0a (I-01): the final test starts at test_begin (== fit_end unless a nested
    // split leaves an h+delay embargo after the fit window). With a library source, the
    // test must not overlap any discover range recorded in the library.
    const atx::usize test_begin = (pit.test_begin == 0U) ? fit_end : pit.test_begin;
    if (test_begin < fit_end || test_begin > np) {
        return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
            "combine: test_begin " + std::to_string(test_begin) + " must lie in [fit_end=" +
            std::to_string(fit_end) + ", np=" + std::to_string(np) + "]");
    }
    std::string final_test_guard;
    if (from_library && test_begin < np && pit.holdout_guard == HoldoutGuardRule::RefuseOverlapV2) {
        ATX_TRY(final_test_guard, guard_final_test(cfg.library_dir, input, test_begin, np));
    }

    // 8a-8c. The shipped weights: ONE dispatch (fit_shipped_weights) shared with every
    //     walk-forward fold (I-08). The research "close"/"volume" columns feed stacking
    //     labels and the capacity model; the trailing liquidity panels (I-03) are built
    //     once over the fit window, which also contains every walk-forward fold.
    const auto close_id  = panel.field_id("close");
    const auto volume_id = panel.field_id("volume");
    const std::span<const atx::f64> close_all =
        close_id.has_value() ? panel.field_all(*close_id) : std::span<const atx::f64>{};
    const std::span<const atx::f64> volume_all =
        volume_id.has_value() ? panel.field_all(*volume_id) : std::span<const atx::f64>{};
    std::optional<LiquidityPanels> liq;
    if (cfg.capacity_floor > 0.0 && cfg.target_aum > 0.0 &&
        pit.capacity == CapacityRule::TrailingPitTradesV2 && !close_all.empty() &&
        !volume_all.empty()) {
        liq.emplace(build_liquidity_panels(close_all, volume_all, panel.instruments(), fit_begin,
                                           fit_end));
    }
    const ShipInputs ship{cfg, combiner_cfg, risk_cfg, pit, pool, streams, close_all,
                          volume_all, panel.dates(), panel.instruments(),
                          liq.has_value() ? &*liq : nullptr};
    // Conviction window (I-02): the fit window, or the whole stream under FullStreamV1.
    const bool conv_full = pit.conviction == ConvictionWindowRule::FullStreamV1;
    ATX_TRY(ShippedWeights shipped,
            fit_shipped_weights(ship, fit_begin, fit_end, conv_full ? 0U : fit_begin,
                                conv_full ? np : fit_end));
    combine::Combination combo = std::move(shipped.combo);
    const bool stack_telemetry_on = shipped.stack_on;
    const learn::StackingVerdict stack_verdict = shipped.verdict;
    const std::vector<combine::ConvictionScore> conviction_scores =
        std::move(shipped.conviction_scores);
    std::string kelly_fraction_used_str = std::move(shipped.kelly_fraction_used);
    std::string kelly_gross_str = std::move(shipped.kelly_gross);
    std::string kelly_scale_applied_str = std::move(shipped.kelly_scale_applied);
    const bool capacity_telemetry_on = shipped.capacity_on;
    const std::vector<atx::f64> capacity_alpha_aum = std::move(shipped.capacity_aum);
    const atx::f64 capacity_max_participation = shipped.capacity_max_participation;

    // 9. Build the combined mega-alpha matrix [dates * insts] from the per-alpha
    //    TARGET-WEIGHT (position) streams — the representation each alpha's
    //    metrics were validated on, and the engine's documented combiner input
    //    (streams.hpp: "the Phase-4 combiner ... consumes, per alpha, the
    //    position (target-weight) stream"). Each position row is winsorized,
    //    rank/zscore-transformed, dollar-neutralized and gross-normalized, so
    //    the alphas live on a COMPARABLE scale and enter in their validated
    //    profitable orientation. Averaging the RAW signals here (the prior bug)
    //    mixed incomparable scales (e.g. market_cap/close ~1e7 vs rank ~0..1) and
    //    applied the combiner weights — fit against the position/PnL streams — to
    //    a different, un-normalized book, inverting the realized portfolio sign
    //    relative to the per-alpha Sharpes. Combining positions makes the combo's
    //    PnL exactly Σ_a w_a·pnl_a, the stream the combiner optimized.
    //
    //    Dead cells: WeightPolicy emits 0.0 (not NaN) for out-of-universe / warmup
    //    names, so an alpha simply does not participate there. A cell is left NaN
    //    (no name) only when it is out of the panel universe for that date.
    const atx::usize D = panel.dates();
    const atx::usize N = panel.instruments();
    // 9.1 INVARIANT (positional AlphaId keying): the fitted weight vector is keyed
    // by AlphaId — combo.weights[a] is the weight of the alpha whose AlphaId is `a`.
    // By construction AlphaId `a` == streams row `a` == the step-3 dsl/labels index
    // `a`: step-7 inserts the pool in ascending `a` over streams.n_alphas(), and the
    // combiner returns one weight per pool alpha in that same AlphaId order. So the
    // blend below MUST apply combo.weights[a] to the stream whose AlphaId is `a`
    // (NOT to a directory-sort position or any other ordering). Assert the one fact
    // that makes the positional index sound: one weight per stream alpha. This is a
    // debug-only programmer-error guard; it changes NO numeric behavior on the
    // static path (the loop bounds and arithmetic are unchanged).
    ATX_ASSERT(combo.weights.size() == streams.n_alphas());
    std::vector<atx::f64> combined(D * N, std::numeric_limits<atx::f64>::quiet_NaN());
    std::vector<std::span<const atx::f64>> rows(combo.weights.size());
    for (atx::usize t = 0; t < D; ++t) {
        for (atx::usize a = 0; a < combo.weights.size(); ++a) {
            rows[a] = streams.positions(a, t);
        }
        for (atx::usize i = 0; i < N; ++i) {
            if (!panel.in_universe(t, i)) {
                continue; // leave NaN — not a tradable name on this date
            }
            atx::f64 acc = 0.0;
            for (atx::usize a = 0; a < combo.weights.size(); ++a) {
                acc += combo.weights[a] * rows[a][i];
            }
            combined[t * N + i] = acc;
        }
    }

    // 10. Serialize as a 1-field panel with the research panel's universe mask.
    std::vector<std::uint8_t> uni(D * N);
    for (atx::usize d = 0; d < D; ++d) {
        for (atx::usize i = 0; i < N; ++i) {
            uni[d * N + i] = panel.in_universe(d, i) ? 1 : 0;
        }
    }
    ATX_TRY(auto cpanel,
            alpha::Panel::create(D, N, {"alpha"}, {combined}, uni));

    // 11. Write weights sidecar.
    {
        const std::string sidecar_path = cfg.combo_out + ".weights.txt";
        std::ofstream wf{sidecar_path};
        if (!wf.is_open()) {
            return atx::core::Err(atx::core::ErrorCode::IoError,
                                  "combine: cannot write weights sidecar: " + sidecar_path);
        }
        const std::string method_str = method_to_string(cm);
        wf << "method="     << method_str        << '\n';
        wf << "fit_begin="  << fit_begin         << '\n';
        wf << "fit_end="    << fit_end            << '\n';
        for (atx::usize a = 0; a < combo.weights.size(); ++a) {
            // W0-I0a: max_digits10 so the sidecar round-trips the exact fitted weight
            // (stage_metabook's sleeve signals are rebuilt from it, I-07).
            wf << "w[" << a << "]=" << std::setprecision(17) << combo.weights[a]
               << std::setprecision(6)
               << ' ' << labels[a] << '\n';
        }
        wf.close();
        if (!wf) {
            return atx::core::Err(atx::core::ErrorCode::IoError,
                                  "combine: weights sidecar write failed");
        }
    }

    // 11b. (A2a) Write the combo.meta boundary sidecar. This is a SEPARATE file
    //      from combo.bin and is NOT part of the panel digest, so it never changes
    //      the deterministic combine output. Task A2b's report reads it to split the
    //      equity curve into in-sample [fit_begin, fit_end) and out-of-sample
    //      [holdout_begin, n_periods). holdout_begin == fit_end by definition.
    {
        const std::string meta_path = cfg.combo_out + ".meta";
        std::ofstream mf{meta_path};
        if (!mf.is_open()) {
            return atx::core::Err(atx::core::ErrorCode::IoError,
                                  "combine: cannot write combo.meta sidecar: " + meta_path);
        }
        mf << "n_periods="     << np                          << '\n';
        mf << "fit_begin="     << fit_begin                   << '\n';
        mf << "fit_end="       << fit_end                     << '\n';
        mf << "holdout_begin=" << test_begin                  << '\n';
        mf << "holdout_frac="  << cfg.combine_holdout_frac    << '\n';
        mf.close();
        if (!mf) {
            return atx::core::Err(atx::core::ErrorCode::IoError,
                                  "combine: fit-boundary sidecar write failed");
        }
    }

    ATX_TRY(auto weights_hash, atx::core::sha256_file(cfg.combo_out + ".weights.txt"));
    ATX_TRY(auto boundary_hash, atx::core::sha256_file(cfg.combo_out + ".meta"));
    ATX_TRY(auto digest, write_pipeline_panel(cpanel, cfg.combo_out, input, {},
        "stage=combine-v1\nmethod=" + method_to_string(cm) +
            "\nfit_begin=" + std::to_string(fit_begin) + "\nfit_end=" + std::to_string(fit_end),
        {{"weights", weights_hash}, {"fit-boundary", boundary_hash}}));

    // 12. Breadth instrumentation (D3a — recorded-only / W5/C2.2 telemetry).
    //     Compute the Fundamental-Law-of-Active-Management decomposition of the
    //     fitted mega-alpha AFTER weights are final (post-conviction, post-crowding),
    //     so the breadth reflects the actual shipped book. Always-computed; no flag.
    //     DETERMINISM: reuses the combiner's order-fixed detail helpers
    //     (complete_case_centered + mle_covariance) so the covariance convention
    //     is identical to the LW path — no new RNG, fixed reduction order.
    //     BYTE-IDENTICAL proof: these three scalars go into sr.kvs ONLY — they are
    //     NEVER folded into combo.bin, the panel digest, or any hashed artifact.
    atx::f64 effective_n  = 0.0;
    atx::f64 realized_ir  = 0.0;
    atx::f64 implied_ic   = 0.0;
    {
        const atx::usize na = pool.n_alphas();
        const atx::usize t  = fit_end - fit_begin;  // fit-window length (>= 2, guarded above)
        if (na > 0 && t >= 2) {
            namespace ev = atx::engine::eval;
            using atx::core::linalg::VecX;
            using atx::core::linalg::MatX;

            // Step 1 — N_eff via the alpha-return covariance over [fit_begin, fit_end).
            // Reuse the combiner's deterministic helpers (same convention as the LW path):
            //   window_means  -> per-alpha window means mu (VecX, length na)
            //   complete_case_centered -> T_cc x na demeaned matrix (listwise NaN drop)
            //   mle_covariance -> N x N MLE covariance S (divisor T_cc)
            // (S3-4) risk_cfg.kind==Factor: breadth is measured against the SAME
            // cleaned_alpha_cov the ShrinkageMv weight fit above uses, so the
            // telemetry stays coherent with whichever covariance actually shipped.
            // Default risk_cfg.kind==Diagonal keeps mle_covariance -> byte-identical.
            const VecX mu = combine::detail::window_means(pool, na, fit_begin, t);
            const MatX centered = combine::detail::complete_case_centered(pool, na, fit_begin, t, mu);
            const MatX cov = (risk_cfg.kind == risk::RiskModelKind::Factor)
                                 ? atx::engine::data::cleaned_alpha_cov(centered)
                                 : combine::detail::mle_covariance(centered, na);
            effective_n = ev::effective_breadth(cov);

            // Step 2 — realized IR: annualized Sharpe of the weighted-blend PnL stream
            // over the fit window [fit_begin, fit_end) (fixed order a = 0..na).
            // This is the IN-SAMPLE realized IR, paired with the in-sample covariance.
            // Delegates to the shared blend_window_sharpe helper (D3b DRY extraction);
            // result is byte-identical to the prior inline implementation.
            realized_ir = blend_window_sharpe(pool, combo.weights, fit_begin, fit_end);

            // Step 3 — implied IC = IR / sqrt(N_eff)  (Fundamental Law: IR = IC * sqrt(breadth)).
            if (std::isfinite(realized_ir) && effective_n > 0.0) {
                implied_ic = realized_ir / std::sqrt(effective_n);
            }
        }
    }

    // 13. D3b — opt-in walk-forward re-fit OOS harness (telemetry only: the shipped
    //     combo.bin never depends on it).
    //
    //     W0-I0a (I-08) WalkForwardRule::ShippedFitEmbargoedV2 (default): K expanding
    //     folds INSIDE the fit window [fit_begin, fit_end) -- the final test is never
    //     scored here -- each refit through fit_shipped_weights (the SAME dispatch as the
    //     shipped book: method, stacking/regime, cleaned covariance, conviction, Kelly,
    //     crowding/capacity), and each fold's test window starts h + delay dates after
    //     its train window ends (h = stack_horizon for Stack/RegimeStack, else 1).
    //     LinearNoEmbargoV1: the pre-W0 loop over [fit_begin, np) with a plain
    //     AlphaCombiner (method only; conviction when on), no embargo.
    std::string wf_folds_str;
    std::string wf_mean_str;
    std::string wf_sharpes_str;
    std::string wf_embargo_str;
    if (cfg.walk_forward >= 1) {
        const atx::usize K = static_cast<atx::usize>(cfg.walk_forward);
        const bool wf_v1 = pit.walk_forward == WalkForwardRule::LinearNoEmbargoV1;
        const atx::usize wf_end = wf_v1 ? np : fit_end;
        const bool stacked =
            cm == combine::CombineMethod::Stack || cm == combine::CombineMethod::RegimeStack;
        const atx::usize horizon = stacked ? static_cast<atx::usize>(combiner_cfg.stack_horizon)
                                           : 1U;
        const atx::usize embargo = wf_v1 ? 0U : horizon + pit.execution_delay;
        const atx::usize span = (wf_end > fit_begin) ? (wf_end - fit_begin) : 0U;
        const atx::usize seg = span / (K + 1U); // K+1 equal segments; folds test segments 1..K
        if (seg < embargo + 2U) {
            return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                "combine: --walk-forward " + std::to_string(K) + " leaves <2 test periods per "
                "fold after the " + std::to_string(embargo) + "-date h+delay embargo (window [" +
                std::to_string(fit_begin) + "," + std::to_string(wf_end) + "))");
        }
        std::vector<atx::f64> fold_sharpe;
        fold_sharpe.reserve(K);
        ATX_TRY(auto cm_wf, method_from_string(cfg.method));
        for (atx::usize k = 1; k <= K; ++k) {
            const atx::usize train_end = fit_begin + k * seg; // expanding [fit_begin, train_end)
            const atx::usize test_end = (k == K) ? wf_end : (train_end + seg);
            std::vector<atx::f64> wf_weights;
            if (wf_v1) {
                combine::AlphaCombiner wf;
                wf.cfg.method = cm_wf;
                ATX_TRY(auto wf_combo, wf.fit(pool, fit_begin, train_end));
                if (cfg.conviction) {
                    apply_conviction(pool, wf_combo.weights, fit_begin, train_end,
                                     pool.n_alphas());
                }
                wf_weights = std::move(wf_combo.weights);
            } else {
                ATX_TRY(auto fold, fit_shipped_weights(ship, fit_begin, train_end, fit_begin,
                                                       train_end));
                wf_weights = std::move(fold.combo.weights);
            }
            fold_sharpe.push_back(
                blend_window_sharpe(pool, wf_weights, train_end + embargo, test_end));
        }
        atx::f64 mean = 0.0;
        for (const atx::f64 s : fold_sharpe) mean += s;
        mean = fold_sharpe.empty() ? 0.0 : mean / static_cast<atx::f64>(fold_sharpe.size());
        // Build the comma-joined per-fold Sharpe string "s1,s2,...,sK".
        std::string joined;
        for (atx::usize k = 0; k < fold_sharpe.size(); ++k) {
            if (k > 0) joined += ',';
            joined += std::to_string(fold_sharpe[k]);
        }
        wf_folds_str   = std::to_string(K);
        wf_mean_str    = std::to_string(mean);
        wf_sharpes_str = std::move(joined);
        wf_embargo_str = std::to_string(embargo);
    }

    // 14. Return StageResult.
    const std::string method_label = method_to_string(cm);
    StageResult sr;
    sr.digest = digest;
    sr.kvs = {
        {"alphas",              std::to_string(dsl.size())},
        {"method",              method_label},
        {"fit_begin",           std::to_string(fit_begin)},
        {"fit_end",             std::to_string(fit_end)},
        {"holdout_begin",       std::to_string(test_begin)},
        {"combo",               to_hex16(digest)},
        {"breadth_effective_n", std::to_string(effective_n)},
        {"breadth_realized_ir", std::to_string(realized_ir)},
        {"breadth_implied_ic",  std::to_string(implied_ic)},
    };
    // D3b: additive WF telemetry — only present when --walk-forward >= 1.
    // Absent from the default (k==0) path so default kvs is byte-identical.
    if (cfg.walk_forward >= 1) {
        sr.kvs.emplace_back("walk_forward_folds",           wf_folds_str);
        sr.kvs.emplace_back("walk_forward_oos_sharpe_mean", wf_mean_str);
        sr.kvs.emplace_back("walk_forward_oos_sharpe",      wf_sharpes_str);
        sr.kvs.emplace_back("walk_forward_embargo",         wf_embargo_str);
    }
    // T6: additive capacity telemetry — present ONLY on the opt-in capacity path
    // (--capacity-floor>0 && --target-aum>0). Absent otherwise, so the default and
    // corr-only kvs stay byte-identical. These scalars feed kvs ONLY — they never
    // enter combo.bin or the panel digest (sr.digest is set above from write_panel).
    //   capacity_alpha_aum        — comma-joined per-alpha capacity AUM (AlphaId
    //                               order); "inf" denotes an unbounded (frictionless)
    //                               alpha so the consumer can distinguish it.
    //   capacity_min_alpha_aum    — the binding (smallest) per-alpha capacity AUM:
    //                               the AUM ceiling the most capacity-limited alpha
    //                               imposes on the book.
    //   capacity_max_participation — the largest per-name ADV participation across
    //                               the books at --target-aum (the liquidity stress).
    if (capacity_telemetry_on) {
        std::string caps_csv;
        atx::f64 cap_min = std::numeric_limits<atx::f64>::infinity();
        for (atx::usize a = 0; a < capacity_alpha_aum.size(); ++a) {
            if (a > 0) caps_csv += ',';
            const atx::f64 c = capacity_alpha_aum[a];
            caps_csv += std::isinf(c) ? std::string("inf") : std::to_string(c);
            if (c < cap_min) cap_min = c;
        }
        sr.kvs.emplace_back("capacity_alpha_aum", caps_csv);
        sr.kvs.emplace_back("capacity_min_alpha_aum",
                            std::isinf(cap_min) ? std::string("inf") : std::to_string(cap_min));
        sr.kvs.emplace_back("capacity_max_participation",
                            std::to_string(capacity_max_participation));
    }
    // S5-1: additive conviction telemetry — present ONLY when --conviction (the
    // collector is empty otherwise, so the default kvs set is byte-identical). The
    // three keys are comma-joined per-alpha breakdowns in AlphaId order, so the
    // operator can see, per admitted alpha, the final score and the (clamped, in
    // [0,1]) DSR and stability terms that produced it. These feed kvs ONLY — never
    // combo.bin or the panel digest (sr.digest is set from write_panel above).
    if (!conviction_scores.empty()) {
        std::string scores_csv;
        std::string dsr_csv;
        std::string stab_csv;
        for (atx::usize a = 0; a < conviction_scores.size(); ++a) {
            if (a > 0) {
                scores_csv += ',';
                dsr_csv += ',';
                stab_csv += ',';
            }
            const combine::ConvictionScore& cs = conviction_scores[a];
            scores_csv += std::to_string(cs.score);
            dsr_csv += std::to_string(cs.dsr_term);
            stab_csv += std::to_string(cs.stability_term);
        }
        sr.kvs.emplace_back("conviction_scores", std::move(scores_csv));
        sr.kvs.emplace_back("conviction_dsr_terms", std::move(dsr_csv));
        sr.kvs.emplace_back("conviction_stability_terms", std::move(stab_csv));
    }
    // S5-2: additive fractional-Kelly telemetry — present ONLY when kelly_fraction > 0
    // (the strings are empty otherwise, so the default kvs set is byte-identical). These
    // record the realized Kelly sizing of the shipped book; they feed kvs ONLY (the
    // weights themselves are already folded into combo.bin above when the block ran).
    //   kelly_fraction_used  — the cfg.kelly_fraction applied.
    //   kelly_gross          — realized Sum|w| after the gross clamp.
    //   kelly_scale_applied  — gross-clamp factor (1.0 when the clamp was not binding).
    if (!kelly_fraction_used_str.empty()) {
        sr.kvs.emplace_back("kelly_fraction_used", std::move(kelly_fraction_used_str));
        sr.kvs.emplace_back("kelly_gross",         std::move(kelly_gross_str));
        sr.kvs.emplace_back("kelly_scale_applied", std::move(kelly_scale_applied_str));
    }
    // S3-1/S3-2: additive stacking-gate telemetry — present ONLY on the
    // Stack/RegimeStack path (absent otherwise, so every legacy method's kvs
    // set stays byte-identical). `stack_verdict_hash` is fit_stack's M1
    // byte-identical digest of the decided verdict fields (the "same seed ->
    // same verdict" pin V1's scorecard reads); `stack_admitted`/the four
    // oos_* scalars are the honest linear-vs-nonlinear comparison S3-2's
    // admit-vs-fallback gate decided on. Never folded into combo.bin/digest.
    if (stack_telemetry_on) {
        sr.kvs.emplace_back("stack_verdict_hash",
                            std::to_string(stack_verdict.verdict_hash));
        sr.kvs.emplace_back("stack_admitted", stack_verdict.admitted ? "1" : "0");
        sr.kvs.emplace_back("stack_oos_dsr_nonlinear",
                            std::to_string(stack_verdict.oos_dsr_nonlinear));
        sr.kvs.emplace_back("stack_oos_dsr_linear",
                            std::to_string(stack_verdict.oos_dsr_linear));
        sr.kvs.emplace_back("stack_oos_ic_nonlinear",
                            std::to_string(stack_verdict.oos_ic_nonlinear));
        sr.kvs.emplace_back("stack_oos_ic_linear",
                            std::to_string(stack_verdict.oos_ic_linear));
    }
    return atx::core::Ok(std::move(sr));
}

} // namespace atx::impl
