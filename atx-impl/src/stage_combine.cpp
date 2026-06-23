#include "stages.hpp"

#include <algorithm>   // std::sort
#include <cmath>       // std::floor (A2a holdout-fit window)
#include <filesystem>
#include <fstream>
#include <limits>      // std::numeric_limits
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
#include "atx/engine/combine/store.hpp"        // combine::AlphaStore
#include "atx/engine/eval/deflated_sharpe.hpp" // eval::deflated_sharpe, DsrResult
#include "atx/engine/eval/pbo.hpp"             // eval::PboResult (full def needed to construct zero value)
#include "atx/engine/eval/perf_metrics.hpp"    // eval::compute_return_metrics, ReturnMetricsCfg, ReturnMetrics
#include "atx/engine/eval/breadth.hpp"          // eval::effective_breadth
#include "atx/engine/eval/stats_ext.hpp"       // eval::mean_std_pop (MeanStd), skewness, excess_kurtosis
#include "atx/engine/library/library.hpp"      // library::Library, AlphaId, AlphaRecordView (8.B)
#include "atx/engine/loop/weight_policy.hpp"   // engine::WeightPolicy

#include "artifacts.hpp"
#include "config.hpp"
#include "research_sim.hpp"
#include "sector_groups.hpp"
#include "serialize_panel.hpp"

namespace atx::impl {

namespace alpha   = atx::engine::alpha;
namespace combine = atx::engine::combine;
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
    return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                          "combine: unknown --method '" + s + "'");
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
//   * MAIN path passes the FULL stream [0, np) (na = pool.n_alphas()), which is
//     byte-identical to the old inline code — pnl.subspan(0, np) IS the whole
//     pnl stream, so DSR (over r=pnl[1..T)), stability (split at T/2), N=na, and
//     the PBO-dropped renormalized ConvictionConfig all match exactly.
//   * WF folds pass the fold's TRAIN window [fit_begin, train_end) (causal —
//     never the test window), so walk_forward_oos_sharpe reflects the shipped
//     conviction-weighted book per fold.
//
// Determinism: order-fixed alpha loop, no RNG/alloc beyond the window sub-span;
// identical inputs -> identical weights.
// ---------------------------------------------------------------------------
static void apply_conviction(const combine::AlphaStore& pool,
                             std::vector<atx::f64>& weights,
                             atx::usize conv_begin, atx::usize conv_end,
                             atx::usize na) {
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
    }
    // Renormalize so Σ|w| = 1 (gross-exposure target maintained).
    ce::detail::renorm_abs_sum(weights);
}

// ---------------------------------------------------------------------------
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

    // The alpha's LAST-period target weights (the capacity_for_alpha convention:
    // the most recent rebalance is what is sized to target_aum).
    const std::span<const atx::f64> w = streams.positions(a, streams.n_periods() - 1U);
    const atx::usize n = (insts < w.size()) ? insts : w.size();

    // Gross frictionless edge (bps): 1e4 · mean over usable return rows of the
    // cross-sectional book return Σ_i w_i·ret_i(t). A NaN weight/return drops the
    // term; rows with no contributing term are skipped (do not bias the mean).
    atx::f64 sum_rows = 0.0; atx::usize n_rows = 0U;
    for (atx::usize t = 1U; t < dates; ++t) {
        atx::f64 row_ret = 0.0; bool any = false;
        for (atx::usize i = 0U; i < n; ++i) {
            const atx::f64 wi = w[i];
            if (std::isnan(wi) || wi == 0.0) continue;
            const atx::f64 r = step_return(t, i);
            if (!std::isnan(r)) { row_ret += wi * r; any = true; }
        }
        if (any) { sum_rows += row_ret; ++n_rows; }
    }
    const atx::f64 gross_edge_bps =
        (n_rows == 0U) ? 0.0 : 1.0e4 * (sum_rows / static_cast<atx::f64>(n_rows));
    if (gross_edge_bps <= 0.0) {
        return 0.0; // no positive edge to erode -> conservative zero capacity
    }

    // The AUM-independent cost bracket C: cost_bps(aum) = C·aum^delta, with
    //   C = 1e4 · Σ_i |w_i| · Y · σ_i · (|w_i|/(price_i·ADV_i))^delta.
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
        const atx::f64 part_per_aum = abs_w / (price * adv); // part_i = part_per_aum·aum
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
// alpha_max_participation — the max per-name participation part_i = (target_aum·
// |w_i|/price_i)/ADV_i over alpha `a`'s LAST-period book at `target_aum`. A pure
// liquidity-footprint telemetry figure (the single most liquidity-stressed name in
// the book): 0 when no name has finite price+ADV. Mirrors book_cost_bps's guards.
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
        const atx::f64 part = (target_aum * abs_w / price) / adv;
        if (part > max_part) max_part = part;
    }
    return max_part;
}

// ---------------------------------------------------------------------------
// run_combine
// ---------------------------------------------------------------------------
atx::core::Result<StageResult> run_combine(const RunConfig& cfg)
{
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
    ATX_TRY(auto panel, read_panel(cfg.panel));

    // 3. Collect the alpha DSL sources + a per-alpha label (sidecar provenance).
    //    Two interchangeable sources; both yield `dsl` (expression strings, in a
    //    deterministic order) and `labels` (the weights-sidecar provenance string).
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
            labels.push_back("lib:" + cfg.library_dir + "#alpha_" + std::to_string(a));
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
            labels.push_back(p.string());
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
    ATX_TRY(auto cm, method_from_string(cfg.method));
    combine::AlphaCombiner combiner;
    combiner.cfg.method = cm;

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

    ATX_TRY(auto combo, combiner.fit(pool, fit_begin, fit_end));

    // 8b. (D1.2) Opt-in conviction weighting: a post-fit transform that scales each
    //     alpha's fitted weight by a per-alpha conviction score computed AT COMBINE TIME
    //     from that alpha's own PnL stream (deflated-Sharpe probability + first/second-half
    //     Sharpe stability), then renormalizes Σ|w|=1. Default (cfg.conviction == false)
    //     skips this block entirely — combo.weights are untouched and the output is
    //     byte-identical to today. The conviction math runs ONLY inside this if-block.
    if (cfg.conviction) {
        // Apply the conviction transform over the FULL stream [0, np). This is
        // byte-identical to the original inline D1.2 block (the helper over the
        // full window reproduces today's weights EXACTLY — the conviction-digest
        // test is pinned). The same helper re-applies per-fold conviction in the
        // walk-forward loop below (T7 NEW-1).
        apply_conviction(pool, combo.weights, /*conv_begin=*/0U, /*conv_end=*/np,
                         pool.n_alphas());
    }

    // T6 capacity telemetry (additive; populated only on the opt-in capacity path).
    // capacity_alpha_aum holds the per-alpha capacity AUM (dollars) in AlphaId order;
    // these feed step-14 kvs ONLY — never combo.bin, the digest, or any hashed artifact.
    std::vector<atx::f64> capacity_alpha_aum;
    atx::f64 capacity_max_participation = 0.0;
    bool capacity_telemetry_on = false;

    // 8c. (9.2) Opt-in crowding de-correlation: a post-fit transform that shrinks
    //     each fitted weight by how mutually correlated its alpha is with the rest
    //     of the pool (corr_penalty) and, optionally, by how capacity-limited the
    //     name is (capacity_floor). Both knobs default 0.0 -> the engine's EXACT
    //     passthrough rail (corr_penalty==0 AND capacity_floor<=0 => weights returned
    //     bit-for-bit), so the no-flag combine output is byte-identical to today. We
    //     only call into decorrelate_weights when at least one knob is active; the
    //     default path never touches combo.weights. The transform reuses the SAME
    //     [fit_begin, fit_end) window the weights were fit on (the engine takes
    //     correlations over that PnL sub-span).
    if (cfg.corr_penalty > 0.0 || cfg.capacity_floor > 0.0) {
        // The per-name capacity vector decorrelate_weights consumes. Two regimes:
        //
        //  * CAPACITY OFF (cfg.capacity_floor <= 0, OR no --target-aum): the engine
        //    DISABLES capacity scaling (cap_scale_i == 1 for every name regardless of
        //    these values), so the vector is UNUSED. Fill the constant-1.0 stub so it
        //    is well-formed and deterministic. This keeps the corr-only path and the
        //    default path BYTE-IDENTICAL to before T6 (the gate below is never entered).
        //
        //  * CAPACITY ON — T6 (cfg.capacity_floor > 0 AND cfg.target_aum > 0): replace
        //    the stub with a REAL per-alpha capacity AUM in dollars. capacity[a] is the
        //    AUM at which alpha a's last-period book's temporary √-impact erodes its
        //    gross frictionless edge to zero (the risk::capacity_curve net-edge
        //    zero-crossing, computed locally over the date-major alpha::Panel — see
        //    alpha_capacity_aum). UNITS: capacity[a] and the user's --capacity-floor are
        //    both DOLLAR AUM, so decorrelate_weights' cap_scale_a = clamp(capacity[a] /
        //    capacity_floor, 0, 1) is meaningful: an alpha whose capacity AUM is BELOW
        //    the floor (it cannot absorb `floor` dollars without eroding its edge) is
        //    faded linearly, and an alpha at/above the floor holds full size. This is
        //    the real per-name differentiation the D3c placeholder lacked.
        //
        //  IMPACT MODEL: capacity uses the engine DEFAULT (Appendix-A) ImpactCfg{}
        //  (Y = 1.0, delta = 0.5, gamma = 0.314) — an impact-BEARING model — NOT the
        //  combine stage's frictionless_sim (which zeroes Y and would yield +inf
        //  capacity for every alpha, re-creating the D3c no-op). The frictionless sim
        //  remains the stream-extraction sim above (that output stays byte-identical);
        //  the impact model here is used ONLY to size capacity.
        std::vector<atx::f64> capacity(pool.size(), 1.0);
        if (cfg.capacity_floor > 0.0 && cfg.target_aum > 0.0) {
            namespace exec = atx::engine::exec;
            const exec::ImpactCfg impact{}; // engine DEFAULT (Appendix-A), impact-bearing
            const auto close_id  = panel.field_id("close");
            const auto volume_id = panel.field_id("volume");
            // "close" is mandatory (extract_streams already required it). "volume"
            // gives the dollar ADV; a panel WITHOUT volume -> 0 ADV everywhere ->
            // C == 0 -> capacity +inf for every alpha (a documented degenerate: no
            // liquidity data means no capacity bound — the floor cannot bite, which
            // mirrors book_cost_bps's no-volume -> 0-cost degenerate).
            const std::span<const atx::f64> close =
                close_id.has_value() ? panel.field_all(*close_id) : std::span<const atx::f64>{};
            const std::span<const atx::f64> volume =
                volume_id.has_value() ? panel.field_all(*volume_id) : std::span<const atx::f64>{};
            const atx::usize dts = panel.dates();
            const atx::usize its = panel.instruments();
            if (close_id.has_value() && volume_id.has_value()) {
                for (atx::usize a = 0; a < streams.n_alphas(); ++a) {
                    capacity[a] = alpha_capacity_aum(streams, a, close, volume, dts, its, impact);
                    const atx::f64 part = alpha_max_participation(
                        streams, a, close, volume, dts, its, cfg.target_aum);
                    if (part > capacity_max_participation) capacity_max_participation = part;
                }
            } else {
                // No volume field -> unbounded capacity for every alpha (no fade).
                for (atx::usize a = 0; a < streams.n_alphas(); ++a) {
                    capacity[a] = std::numeric_limits<atx::f64>::infinity();
                }
            }
            // Telemetry (additive — see step 14). Record the per-alpha capacity AUM
            // and the book aggregates BEFORE decorrelate_weights consumes the vector.
            capacity_alpha_aum = capacity;          // copy out for the kvs line
            capacity_telemetry_on = true;
        }
        combine::CrowdingConfig ccfg{};
        ccfg.corr_penalty   = cfg.corr_penalty;
        ccfg.capacity_floor = cfg.capacity_floor;
        combo.weights = combine::decorrelate_weights(
            combo.weights, pool, fit_begin, fit_end,
            std::span<const atx::f64>{capacity}, ccfg);
    }

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
    ATX_TRY(auto digest, write_panel(cpanel, cfg.combo_out));

    // 11. Write weights sidecar.
    {
        const std::string sidecar_path = cfg.combo_out + ".weights.txt";
        std::ofstream wf{sidecar_path};
        if (!wf.is_open()) {
            return atx::core::Err(atx::core::ErrorCode::IoError,
                                  "combine: cannot write weights sidecar: " + sidecar_path);
        }
        const std::string method_str = cfg.method.empty() ? "shrinkage-mv" : cfg.method;
        wf << "method="     << method_str        << '\n';
        wf << "fit_begin="  << fit_begin         << '\n';
        wf << "fit_end="    << fit_end            << '\n';
        for (atx::usize a = 0; a < combo.weights.size(); ++a) {
            wf << "w[" << a << "]=" << combo.weights[a]
               << ' ' << labels[a] << '\n';
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
        mf << "holdout_begin=" << fit_end                     << '\n';
        mf << "holdout_frac="  << cfg.combine_holdout_frac    << '\n';
    }

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
            const VecX mu = combine::detail::window_means(pool, na, fit_begin, t);
            const MatX centered = combine::detail::complete_case_centered(pool, na, fit_begin, t, mu);
            const MatX cov = combine::detail::mle_covariance(centered, na);
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

    // 13. D3b — opt-in walk-forward re-fit OOS harness.
    //     When cfg.walk_forward >= 1, runs K expanding-window folds over [fit_begin, np)
    //     and records each fold's OOS Sharpe + their mean as additive telemetry.
    //     The shipped combo.bin is UNCHANGED: wf_combo vectors are scratch, used only
    //     for scoring, then discarded. All code lives inside this if-block so the
    //     default (k==0) path is byte-identical and incurs zero extra compute.
    //
    //     SCOPE CAVEAT (read before interpreting walk_forward_oos_sharpe*): T7 NEW-1
    //     made each fold CONVICTION-aware — when --conviction is on, the fold re-applies
    //     the per-fold conviction transform (apply_conviction over the fold's TRAIN
    //     window [fit_begin, train_end), causal — never the test window) BEFORE scoring,
    //     so walk_forward_oos_sharpe now reflects the post-conviction book that actually
    //     ships. It still does NOT reflect post-CROWDING (--corr-penalty / decorrelate)
    //     weights — making WF crowding-aware is out of T7 scope. So with --corr-penalty
    //     also set, walk_forward_oos_sharpe still measures a DIFFERENT book than the final
    //     post-crowding weights; do not directly compare it to breadth_realized_ir then.
    //     When --conviction is OFF, the fold scores the BASE combiner fit exactly as
    //     before (byte-identical telemetry).
    std::string wf_folds_str;
    std::string wf_mean_str;
    std::string wf_sharpes_str;
    if (cfg.walk_forward >= 1) {
        const atx::usize K = static_cast<atx::usize>(cfg.walk_forward);
        const atx::usize span = (np > fit_begin) ? (np - fit_begin) : 0U;
        const atx::usize seg = span / (K + 1U);          // K+1 equal segments; folds test segments 1..K
        if (seg < 2U) {
            return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                "combine: --walk-forward " + std::to_string(K) + " leaves <2 periods per fold (np=" +
                std::to_string(np) + ")");
        }
        std::vector<atx::f64> fold_sharpe;
        fold_sharpe.reserve(K);
        ATX_TRY(auto cm_wf, method_from_string(cfg.method));
        for (atx::usize k = 1; k <= K; ++k) {
            const atx::usize train_end = fit_begin + k * seg;       // expanding train [fit_begin, train_end)
            const atx::usize test_end  = (k == K) ? np : (train_end + seg); // last fold absorbs the remainder
            combine::AlphaCombiner wf;
            wf.cfg.method = cm_wf;                                  // SAME method as the shipped fit
            ATX_TRY(auto wf_combo, wf.fit(pool, fit_begin, train_end));
            // (T7 NEW-1) When --conviction is on, re-apply the per-fold conviction
            // transform over the fold's TRAIN window [fit_begin, train_end) (causal —
            // never the test window) so this fold scores the SHIPPED conviction-weighted
            // book, mirroring how the live book fits + applies conviction in-sample then
            // deploys forward. wf_combo is scratch; combo.weights (the shipped book) is
            // never touched, so combo.bin stays byte-identical regardless of WF. When
            // --conviction is off, this is skipped and the fold scores the base fit.
            if (cfg.conviction) {
                apply_conviction(pool, wf_combo.weights, fit_begin, train_end, pool.n_alphas());
            }
            fold_sharpe.push_back(blend_window_sharpe(pool, wf_combo.weights, train_end, test_end));
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
    }

    // 14. Return StageResult.
    const std::string method_label = cfg.method.empty() ? "shrinkage-mv" : cfg.method;
    StageResult sr;
    sr.digest = digest;
    sr.kvs = {
        {"alphas",              std::to_string(dsl.size())},
        {"method",              method_label},
        {"fit_begin",           std::to_string(fit_begin)},
        {"fit_end",             std::to_string(fit_end)},
        {"holdout_begin",       std::to_string(fit_end)},
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
    return atx::core::Ok(std::move(sr));
}

} // namespace atx::impl
