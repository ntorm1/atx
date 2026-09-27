#include "stages.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "atx/core/error.hpp"
#include "atx/core/types.hpp"

#include "atx/engine/alpha/panel.hpp"
#include "atx/engine/data/adapt_factor.hpp"
#include "atx/engine/library/library.hpp"
#include "atx/engine/risk/constraints.hpp"     // ConstraintSet / ParticipationCap / PositionCap (S5-2)
#include "atx/engine/risk/garleanu_pedersen.hpp"
#include "atx/engine/risk/multi_period.hpp"
#include "atx/engine/risk/optimizer.hpp"
#include "atx/engine/risk/reference_data.hpp"   // CapacityRef (%ADV participation reference — S5-2)

#include "artifacts.hpp"
#include "book_shape.hpp"
#include "config.hpp"
#include "dead_alpha_wire.hpp"
#include "diag_risk.hpp"       // DeployPitConfig, diagonal_risk_models_expanding (W0-I0a)
#include "serialize_panel.hpp"
#include "panel_pipeline.hpp"
#include "atx/core/sha256.hpp"
#include "stage_riskmodel.hpp"

namespace atx::impl {

namespace alpha = atx::engine::alpha;
namespace data  = atx::engine::data;
namespace risk  = atx::engine::risk;
namespace combine = atx::engine::combine;
namespace library = atx::engine::library;

// S1-2 / S5-0: the public no-flag entry point (declared in stages.hpp, the
// S5-CLI-hub surface) builds a RiskModelConfig from the S5-0 CLI fields
// (--risk-model / --dead-alpha-factors / --group-neutralize) and forwards to
// the parameterized overload below. At the field defaults
// (risk_model=="diagonal", dead_alpha_factors=false, group_neutralize=false)
// the constructed RiskModelConfig is IDENTICAL to RiskModelConfig{} — same
// code, same input every existing caller already gets — so the no-flag path
// is byte-identical to pre-S1/pre-S5 BY CONSTRUCTION, not by parallel-
// maintained duplicate logic.
atx::core::Result<StageResult> run_optimize(const RunConfig& cfg)
{
    risk::RiskModelConfig risk_cfg{};
    risk_cfg.kind = (cfg.risk_model == "factor") ? risk::RiskModelKind::Factor
                                                  : risk::RiskModelKind::Diagonal;
    risk_cfg.dead_alpha_factors = cfg.dead_alpha_factors;
    risk_cfg.group_neutralize   = cfg.group_neutralize;
    return run_optimize(cfg, risk_cfg);
}

atx::core::Result<StageResult> run_optimize(const RunConfig& cfg, const risk::RiskModelConfig& risk_cfg)
{
    return run_optimize(cfg, risk_cfg, DeployPitConfig{});
}

namespace {

// ---------------------------------------------------------------------------
// W0-I0a: the per-rebalance risk models (I-04 / I-06).
// ---------------------------------------------------------------------------
// `single` is set only on the legacy DiagRiskRule::WholePanelV1 Diagonal path (one
// whole-panel model applied to every step, byte-identical to pre-W0). Every other
// path fills `steps` with one model per sched.periods[s], fitted on rows
// [0, period+1) only. Dead-alpha crowding factors are resolved PER STEP through
// dead_set_at (DeadAlphaRule): a step with an empty dead set gets the plain model.
struct StepModels {
    std::optional<risk::FactorModel> single;
    std::vector<risk::FactorModel> steps;
    atx::usize dead_steps = 0; // steps whose dead set was non-empty
    atx::usize max_dead = 0;   // largest per-step dead set
};

[[nodiscard]] atx::core::Result<StepModels>
build_step_models(const alpha::Panel& research, const risk::RiskModelConfig& risk_cfg,
                  const DeployPitConfig& pit, const risk::RebalanceSchedule& sched,
                  const library::Library* dead_lib, const LibraryPeriodAxis& axis)
{
    StepModels out;
    const atx::usize D = research.dates();
    const auto dead_at = [&](atx::usize date) -> DeadSet {
        return (dead_lib == nullptr) ? DeadSet{} : dead_set_at(*dead_lib, axis, pit.dead, date);
    };
    const bool diag = risk_cfg.kind == risk::RiskModelKind::Diagonal;
    if (diag && pit.diag == DiagRiskRule::WholePanelV1) {
        const DeadSet ds = dead_at(D - 1U); // V1: one set for the whole run
        out.dead_steps = ds.ids.empty() ? 0U : sched.periods.size();
        out.max_dead = ds.ids.size();
        ATX_TRY(auto artifact, build_risk_model(research, risk_cfg, /*group_id=*/{}, dead_lib,
                                                ds.ids, ds.as_of));
        ATX_TRY(auto model, data::artifact_to_factor_model(artifact));
        out.single.emplace(std::move(model));
        return atx::core::Ok(std::move(out));
    }

    const atx::usize S = sched.periods.size();
    std::vector<atx::usize> fit_ends(S);
    for (atx::usize s = 0; s < S; ++s) {
        fit_ends[s] = sched.periods[s] + 1U; // PIT: through `period` inclusive
    }
    std::vector<risk::FactorModel> fast; // Diagonal V2: the expanding-window base models
    if (diag) {
        ATX_TRY(fast, diagonal_risk_models_expanding(research, fit_ends));
    }
    const risk::RiskModelConfig diag_fallback_cfg; // kind==Diagonal -- Factor warm-up fallback
    out.steps.reserve(S);
    for (atx::usize s = 0; s < S; ++s) {
        const DeadSet ds = dead_at(sched.periods[s]);
        if (!ds.ids.empty()) {
            ++out.dead_steps;
            out.max_dead = std::max(out.max_dead, ds.ids.size());
        }
        if (diag && ds.ids.empty()) {
            out.steps.push_back(std::move(fast[s]));
            continue;
        }
        auto artifact = build_risk_model(research, risk_cfg, {}, dead_lib, ds.ids, ds.as_of,
                                         fit_ends[s]);
        if (!artifact.has_value()) {
            if (diag) {
                return atx::core::Err(artifact.error());
            }
            // Factor warm-up fallback: too little history yet for a genuine Factor fit
            // at this step -- a PIT diagonal over [0, fit_end) for THIS STEP ONLY.
            ATX_TRY(auto diag_artifact, build_risk_model(research, diag_fallback_cfg, {}, dead_lib,
                                                         ds.ids, ds.as_of, fit_ends[s]));
            ATX_TRY(auto diag_model, data::artifact_to_factor_model(diag_artifact));
            out.steps.push_back(std::move(diag_model));
            continue;
        }
        ATX_TRY(auto step_model, data::artifact_to_factor_model(*artifact));
        out.steps.push_back(std::move(step_model));
    }
    return atx::core::Ok(std::move(out));
}

// risk::MultiPeriodOptimizer::run (multi_period.hpp) step for step -- same inner
// optimizer construction, same trade-rate blend, same turnover/cost accounting --
// except that `refresh_ref(date)` rewrites the participation reference buffers before
// each solve, so every rebalance is capped by its OWN trailing ADV (R-12). The driver
// itself holds one CapacityRef for the whole run, which is why this loop exists.
[[nodiscard]] atx::core::Result<risk::MultiPeriodResult>
run_multi_period_step_ref(const risk::MultiPeriodConfig& mc, const risk::RebalanceSchedule& sched,
                          const std::function<std::span<const atx::f64>(atx::usize)>& alpha_at,
                          const std::function<const risk::FactorModel&(atx::usize)>& model_at,
                          const atx::engine::book::CostInputs& cost,
                          const std::function<void(atx::usize)>& refresh_ref)
{
    if (mc.trade_rate <= 0.0 || mc.trade_rate > 1.0) {
        return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                              "optimize: trade_rate must be in (0, 1]");
    }
    risk::OptimizerConfig oc = mc.single;
    oc.turnover_penalty = cost.kappa;
    if (mc.capacity_bound_gross) {
        oc.gross_leverage = std::min(oc.gross_leverage, cost.capacity_gross);
    }
    risk::PortfolioOptimizer opt{oc};
    opt.constraints = mc.constraints;
    opt.qp = mc.qp;
    opt.ref = mc.ref; // spans alias the buffers refresh_ref rewrites

    risk::MultiPeriodResult out;
    out.books.reserve(sched.periods.size());
    out.turnover.reserve(sched.periods.size());
    out.cost_bps.reserve(sched.periods.size());
    std::vector<atx::f64> w_prev;
    for (atx::usize s = 0; s < sched.periods.size(); ++s) {
        const atx::usize d = sched.periods[s];
        refresh_ref(d);
        const auto a = alpha_at(d);
        const risk::FactorModel& V = model_at(d);
        ATX_TRY(std::vector<atx::f64> target, opt.solve(a, V, w_prev));
        std::vector<atx::f64> book(target.size(), 0.0);
        atx::f64 tv = 0.0;
        for (atx::usize i = 0; i < target.size(); ++i) {
            const atx::f64 p = i < w_prev.size() ? w_prev[i] : 0.0;
            book[i] = (mc.trade_rate == 1.0) ? target[i] : (p + mc.trade_rate * (target[i] - p));
            tv += std::fabs(book[i] - p);
        }
        out.turnover.push_back(tv);
        out.cost_bps.push_back(tv * cost.round_trip_cost_bps);
        out.books.push_back(book);
        w_prev = std::move(book);
    }
    return atx::core::Ok(std::move(out));
}

} // namespace

atx::core::Result<StageResult> run_optimize(const RunConfig& cfg,
                                            const risk::RiskModelConfig& risk_cfg,
                                            const DeployPitConfig& pit)
{
    // 1. Validate required flags.
    if (cfg.panel.empty() || cfg.combo.empty() || cfg.books_out.empty()) {
        return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                              "optimize: --panel, --combo, and --out required");
    }

    // 2. Load research and combo panels.
    ATX_TRY(auto research_input, read_pipeline_panel(cfg.panel, cfg.allow_unidentified_panels));
    ATX_TRY(auto combo_input, read_pipeline_panel(cfg.combo, cfg.allow_unidentified_panels));
    ATX_TRY_VOID(require_pipeline_parent(combo_input, research_input, "research"));
    auto& research = research_input.panel;
    auto& combo = combo_input.panel;
    ATX_TRY(auto output_guard,
            reserve_pipeline_output(cfg.books_out, research_input.identity.has_value()));

    if (combo.num_fields() < 1) {
        return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                              "optimize: combo panel must have at least one field");
    }
    if (combo.instruments() != research.instruments()) {
        return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                              "optimize: combo and research instrument counts differ");
    }
    if (combo.dates() != research.dates()) {
        return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                              "optimize: combo and research date counts differ");
    }

    const atx::usize M = research.instruments();
    const atx::usize D = research.dates();

    // 4. Validate --rebalance, then derive step.
    if (!cfg.rebalance.empty() &&
        cfg.rebalance != "daily" &&
        cfg.rebalance != "weekly") {
        return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                              "optimize: --rebalance must be 'daily' or 'weekly' (got: " +
                                  cfg.rebalance + ")");
    }
    const atx::usize step = (cfg.rebalance == "daily") ? 1U : 5U;  // default weekly
    risk::RebalanceSchedule sched;
    for (atx::usize d = 0; d < D; d += step) {
        sched.periods.push_back(d);
    }
    const atx::usize S = sched.periods.size();

    // Resolved gross / name_cap scalars shared by both branches.
    const atx::f64 gross_val    = cfg.gross    > 0.0 ? cfg.gross    : 1.0;
    const atx::f64 name_cap_val = cfg.name_cap > 0.0 ? cfg.name_cap : 1.0;

    // Local helper: serialize a flat books array (S*M) + write sidecar + build
    // StageResult. Used by both the position-mode branch and the MVO branch so
    // the output format and kvs keys are byte-identical between the two paths.
    // turnover[s] = Sigma_i |w[s] - w[s-1]|  (w[-1] = 0).
    // cost_bps[s] = 0 for the position-mode branch; the caller supplies the vec.
    auto write_books = [&](const std::vector<atx::f64>& books_flat,
                           const std::vector<double>& turnover,
                           const std::vector<double>& cost_bps)
        -> atx::core::Result<StageResult>
    {
        // Build panel (all cells live).
        std::vector<std::uint8_t> uni(S * M, 1u);
        ATX_TRY(auto cpanel,
                alpha::Panel::create(S, M, {"weight"}, {books_flat}, uni));

        // Write sidecar .meta.txt
        {
            const std::string sidecar = cfg.books_out + ".meta.txt";
            std::ofstream mf{sidecar};
            if (!mf.is_open()) {
                return atx::core::Err(atx::core::ErrorCode::IoError,
                                      "optimize: cannot write sidecar: " + sidecar);
            }
            const std::string rebalance_str =
                cfg.rebalance.empty() ? "weekly" : cfg.rebalance;
            mf << "periods="     << S             << '\n';
            mf << "instruments=" << M             << '\n';
            mf << "gross="       << gross_val     << '\n';
            mf << "name_cap="    << name_cap_val  << '\n';
            mf << "rebalance="   << rebalance_str << '\n';
            for (atx::usize s = 0; s < S; ++s) {
                mf << "s=" << s
                   << " period="   << sched.periods[s]
                   << " turnover=" << turnover[s]
                   << " cost_bps=" << cost_bps[s]
                   << '\n';
            }
            mf.close();
            if (!mf) {
                return atx::core::Err(atx::core::ErrorCode::IoError,
                                      "optimize: schedule sidecar write failed");
            }
        }

        std::vector<PanelParent> parents;
        if (research_input.identity) {
            ATX_TRY(auto schedule_hash, atx::core::sha256_file(cfg.books_out + ".meta.txt"));
            parents = {{"combo", combo_input.artifact_id}, {"book-schedule", schedule_hash}};
        }
        ATX_TRY(auto digest, write_pipeline_panel(cpanel, cfg.books_out, research_input,
            sched.periods, "stage=optimize-v1\nstep=" + std::to_string(step), std::move(parents)));

        // Build StageResult with the same kvs keys for both branches.
        StageResult sr;
        sr.digest = digest;
        sr.kvs = {
            {"periods",     std::to_string(S)},
            {"instruments", std::to_string(M)},
            {"gross",       std::to_string(gross_val)},
            {"name_cap",    std::to_string(name_cap_val)},
            {"rebalance",   step == 1U ? "daily" : "weekly"},
            {"books",       to_hex16(digest)},
        };
        return atx::core::Ok(std::move(sr));
    };

    // 5a. Position-mode branch: signal-as-position deploy — skip MVO entirely.
    // ROOT CAUSE S6-0: default MVO (λ=1) feeds combined target-weights as expected-returns;
    // t=(1/2λ)·P V⁻¹ P α with diagonal V re-weights by 1/dvar and inverts the book
    // (optimizer.hpp:318). The λ=0 branch t=demean(α) is sign-preserving (optimizer.hpp:317).
    // When the input is a combined target-weight panel, use position-mode (shape_book) or
    // force risk_aversion=0. The MVO math in optimizer.hpp is correct for a TRUE expected-return
    // input — do not edit it.
    if (cfg.position_mode) {
        ATX_TRY(const auto alpha_fid, combo.field_id("alpha"));
        std::vector<atx::f64> books_flat(S * M, 0.0);
        std::vector<double>   turnover(S, 0.0);
        // Guard: byte-identical when --cost-bps is absent (mirrors --trade-rate pattern).
        const double cost_bps_val = cfg.set_flags.count("cost-bps") ? cfg.cost_bps : 0.0;
        std::vector<double>   cost_bps(S, cost_bps_val);

        // Read trade-rate once; guard keeps byte-identical output when unset.
        const double trade_rate_val = cfg.set_flags.count("trade-rate")
                                          ? cfg.trade_rate : 1.0;

        std::vector<atx::f64> prev(M, 0.0);  // w[-1] = 0 (flat)

        // S3: Gârleanu-Pedersen aim-portfolio trading (opt-in via cfg.gp_trading). Built
        // ONCE for the whole run: a single whole-panel Diagonal FactorModel (the SAME
        // inert-default kind risk::RiskModelConfig{} builds on the MVO branch below),
        // INDEPENDENT of --risk-model/risk_cfg -- position-mode never threads risk_cfg
        // today, and extending risk-model SELECTION into position mode is S1/S2 turf,
        // not S3's. Kept outside the per-period loop: it is the fixed risk lens
        // gp_aim_and_value inverts every period, not a per-period PIT refit (Diagonal's
        // own variance estimate already reads the whole research panel once, exactly as
        // the MVO Diagonal branch below does -- no look-ahead concern distinct from that
        // existing path).
        //
        // Fail-open (never silent, per the ROADMAP guardrail): if the build Errs (e.g.
        // `research` lacks "close"), gp_trading is disabled FOR THIS RUN -- every period
        // falls back to the pre-S3 linear blend, and the fallback is recorded in kvs
        // (see write_books call below) so it is never silent.
        //
        // W0-I0a (I-04): under DiagRiskRule::PerStepPitV2 (default) the GP risk lens is
        // one diagonal model PER REBALANCE STEP fitted on rows [0, period+1) only; the
        // whole-panel lens above is DiagRiskRule::WholePanelV1 (look-ahead: an early
        // aim portfolio was sized with variance measured on later dates).
        std::optional<risk::FactorModel> gp_v;        // WholePanelV1
        std::vector<risk::FactorModel> gp_steps;      // PerStepPitV2, one per step
        bool gp_fallback = false;
        if (cfg.gp_trading && pit.diag == DiagRiskRule::WholePanelV1) {
            auto gp_artifact = build_risk_model(research, risk::RiskModelConfig{});
            if (gp_artifact.has_value()) {
                auto gp_model = data::artifact_to_factor_model(*gp_artifact);
                if (gp_model.has_value()) {
                    gp_v.emplace(std::move(*gp_model));
                } else {
                    gp_fallback = true;
                }
            } else {
                gp_fallback = true;
            }
        } else if (cfg.gp_trading) {
            std::vector<atx::usize> fit_ends(S);
            for (atx::usize s = 0; s < S; ++s) {
                fit_ends[s] = sched.periods[s] + 1U;
            }
            auto models = diagonal_risk_models_expanding(research, fit_ends);
            if (models.has_value()) {
                gp_steps = std::move(*models);
            } else {
                gp_fallback = true;
            }
        }
        const auto gp_model_at = [&](atx::usize s) -> const risk::FactorModel* {
            if (gp_v.has_value()) return &*gp_v;
            return gp_steps.empty() ? nullptr : &gp_steps[s];
        };

        for (atx::usize s = 0; s < S; ++s) {
            const atx::usize d = sched.periods[s];
            const auto cs = combo.field_cross_section(alpha_fid, d);
            std::vector<atx::f64> w(cs.begin(), cs.end());
            std::vector<std::uint8_t> live(M);
            for (atx::usize i = 0; i < M; ++i) {
                live[i] = research.in_universe(d, i) ? 1u : 0u;
            }
            shape_book(w, std::span<const std::uint8_t>{live}, gross_val, name_cap_val);

            // Partial-step: either the Gârleanu-Pedersen aim-portfolio trade (opt-in,
            // cfg.gp_trading) or the pre-S3 linear blend toward the freshly-shaped target.
            // See garleanu_pedersen.hpp for the closed-form math; this call site is the
            // ONLY thing S3 changes. The legacy `else if` arm below is textually the
            // pre-S3 code, so gp_trading=false is byte-identical by construction.
            //
            // Legacy linear blend (w := prev + rate*(w - prev)): dollar-neutrality is
            // preserved (linear blend of two dollar-neutral books) and name-cap is
            // preserved (|blend| <= max(|prev|,|target|) <= cap); gross may drift
            // slightly BELOW the target (intended -- the partial step IS the deployed
            // position, not re-normalized). Guard keeps byte-identical output at
            // trade_rate == 1.0.
            if (cfg.gp_trading && gp_model_at(s) != nullptr) {
                // alpha_bar: the per-name RETURN-space signal this period -- the SAME raw
                // cross-section shape_book above just turned into the legacy target `w`.
                // NaN names are preserved (gp_aim_and_value maps them to 0 in the V^-1 apply).
                std::vector<atx::f64> alpha_bar(cs.begin(), cs.end());
                auto gp = risk::gp_aim_and_value(std::span<const atx::f64>{alpha_bar},
                                                 *gp_model_at(s), cfg.gp_risk_aversion);
                if (gp.has_value()) {
                    // Shape the GP aim through the SAME gross/name-cap/dollar-neutral
                    // contract as the legacy target `w`, so the GP path never breaks the
                    // book-shape invariants the rest of this function (and shape_book's own
                    // header) document.
                    std::vector<atx::f64> aim = gp->aim_pos;
                    shape_book(aim, std::span<const std::uint8_t>{live}, gross_val, name_cap_val);
                    // kappa: cfg.trade_rate discounted by the trade-cost-scale knob
                    // (gp_trade_cost_scale == 0 => kappa == trade_rate_val, inert).
                    const atx::f64 kappa = trade_rate_val / (1.0 + cfg.gp_trade_cost_scale);
                    w = risk::gp_turnover_native_step(std::span<const atx::f64>{prev},
                                                      std::span<const atx::f64>{aim}, kappa);
                } else {
                    // Degenerate per-period fallback (defensive -- lambda>=0 is CLI-guarded
                    // and the length always matches M by construction, so this should not
                    // fire in practice). Never silently drop the period: trade the legacy way.
                    if (trade_rate_val < 1.0) {
                        for (atx::usize i = 0; i < M; ++i) {
                            w[i] = prev[i] + trade_rate_val * (w[i] - prev[i]);
                        }
                    }
                }
            } else if (trade_rate_val < 1.0) {
                for (atx::usize i = 0; i < M; ++i) {
                    w[i] = prev[i] + trade_rate_val * (w[i] - prev[i]);
                }
            }

            // Compute per-period turnover: Sigma_i |w[s] - w[s-1]|.
            double tv = 0.0;
            for (atx::usize i = 0; i < M; ++i) tv += std::fabs(w[i] - prev[i]);
            turnover[s] = tv;
            // Per-period cost in bps (mirrors the MVO path: turnover * rate).
            // pnl_cost[s] = cost_bps[s] * 1e-4 in the report stage, so this
            // must be the total charge in bps, not the flat rate alone.
            cost_bps[s] = tv * cost_bps_val;

            // Store weights and update previous book.
            for (atx::usize i = 0; i < M; ++i) {
                books_flat[s * M + i] = w[i];
                prev[i] = w[i];
            }
        }

        // Build a position-mode-specific StageResult that includes trade_rate in kvs
        // only when the flag was explicitly provided, preserving the off-path
        // byte-identical digest guarantee when --trade-rate is absent.
        ATX_TRY(auto sr, write_books(books_flat, turnover, cost_bps));
        if (cfg.set_flags.count("trade-rate"))
            sr.kvs.emplace_back("trade_rate", std::to_string(trade_rate_val));
        if (cfg.gp_trading)
            sr.kvs.emplace_back("gp_trading", gp_fallback ? "fallback" : "on");
        // S5-1: measure FIRST (always -- "measure before gate"), gate opt-in second.
        // Same shared helper + guard as the MVO branch, reusing this branch's own
        // per-period `turnover` vector and `sched.periods`.
        const atx::f64 book_turnover = book_turnover_per_day(turnover, sched.periods);
        sr.kvs.emplace_back("book_turnover_per_day", std::to_string(book_turnover));
        if (cfg.book_turnover_gate > 0.0 && book_turnover > cfg.book_turnover_gate) {
            return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                                  "optimize: book turnover " + std::to_string(book_turnover) +
                                      "/day exceeds --book-turnover-gate " +
                                      std::to_string(cfg.book_turnover_gate));
        }
        return atx::core::Ok(std::move(sr));
    }

    // 5b. MVO path (default: position_mode=false).
    // S1 fix-loop (per-fit-window PIT; corrected from the original S1-2
    // single-whole-panel-fit landing, which was a silent look-ahead: an EARLY
    // rebalance decision was informed by covariance estimated from LATER
    // dates). The covariance source now depends on risk_cfg.kind:
    //
    //   Diagonal (the default kind): W0-I0a (I-04) -- ONE model PER REBALANCE
    //   STEP as well, fitted on rows [0, period+1) (DiagRiskRule::PerStepPitV2).
    //   The pre-W0 single whole-panel model applied to every period is
    //   DiagRiskRule::WholePanelV1 (byte-identical to pre-W0 when selected).
    //
    //   Factor: ONE model PER REBALANCE STEP. For step s covering date
    //   `period = sched.periods[s]`, fit at fit_end = period + 1 (data
    //   through `period` inclusive, nothing after -- PIT-clean; see
    //   build_risk_model's fit_end doc). A step too early for a genuine
    //   Factor fit (fit_end < 2, or an under-determined cross-section --
    //   build_risk_model returns Err) falls back to a PIT diagonal over
    //   [0, fit_end) for THAT STEP ONLY (diag_risk.hpp's fit_end overload,
    //   via a default-constructed kind==Diagonal RiskModelConfig) -- never
    //   the whole-panel diagonal, which would reintroduce look-ahead for that
    //   step. This is honest, not a workaround: a factor covariance cannot be
    //   estimated before there is history to estimate it from.
    //
    //
    // W0-I0a (I-04 / I-06): the per-step models come from build_step_models --
    // DiagRiskRule::PerStepPitV2 (default) fits the Diagonal lens per step on rows
    // [0, period+1) (the Factor path already did); DeadAlphaRule::DeadOrDecayingPerStepV2
    // resolves the dead set per step through the library split-range ledger. The
    // library is still opened ONCE (Library::open does sqlite I/O).
    std::optional<library::Library> dead_lib_opt = maybe_open_dead_lib(cfg, risk_cfg);
    const library::Library* dead_lib_ptr = dead_lib_opt.has_value() ? &*dead_lib_opt : nullptr;
    const LibraryPeriodAxis dead_axis =
        (dead_lib_ptr != nullptr)
            ? library_period_axis(resolve_dead_alpha_lib_dir(cfg), *dead_lib_ptr, D,
                                  research_input.identity
                                      ? std::span<const atx::i64>{research_input.identity
                                                                      ->session_keys}
                                      : std::span<const atx::i64>{})
            : LibraryPeriodAxis{};
    ATX_TRY(const StepModels models,
            build_step_models(research, risk_cfg, pit, sched, dead_lib_ptr, dead_axis));

    // date -> step lookup: periods are dense multiples of `step`
    // (sched.periods[s] == s*step by construction above), so the step
    // covering any date d is floor(d/step) -- exact at a schedule date, and
    // FORWARD-FILLED between them (the "in-force step model" the S1-5
    // neutralize block below needs for a non-schedule date).
    auto model_at = [&models, step](atx::usize period) -> const risk::FactorModel& {
        if (models.single.has_value()) {
            return *models.single;
        }
        return models.steps[period / step];
    };

    // S5-2: participation-cap reference panels. Declared at FUNCTION scope (NOT
    // inside the cfg.participation_cap>0 block below) on purpose: risk::CapacityRef
    // holds non-owning (BORROWED) spans, so the buffers mc.ref.adv/price point at
    // MUST outlive the optimizer run further down. Left empty + unread when the cap is off.
    std::vector<atx::f64> part_adv;
    std::vector<atx::f64> part_price;
    std::span<const atx::f64> part_vol_all;
    std::span<const atx::f64> part_cls_all;
    const bool part_per_step =
        cfg.participation_cap > 0.0 &&
        pit.participation == ParticipationAdvRule::TrailingPitPerRebalanceV2;

    risk::MultiPeriodConfig mc;
    mc.single.risk_aversion   = cfg.set_flags.count("risk-aversion")
                                    ? cfg.risk_aversion : 1.0;
    mc.single.gross_leverage  = gross_val;
    mc.single.name_cap        = name_cap_val;
    mc.single.dollar_neutral  = true;
    mc.single.turnover_penalty = cfg.turnover_penalty;

    // S5-2: participation-rate cap INSIDE the QP construction (inert unless
    // --participation-cap > 0). The one correctness trap (RISKS #1):
    // is_minimal_constraint_set returns false the moment .part is populated, so the
    // augmented path activates -- and it materializes ConstraintSet::gross/pos
    // INDEPENDENTLY of mc.single. So cs.gross/cs.pos MUST be populated here from the
    // SAME gross_val/name_cap_val the fast path resolves.
    //
    // R-12: the reference is the trailing 20-day ADV + close AT EACH REBALANCE
    // (ParticipationAdvRule::TrailingPitPerRebalanceV2, refreshed per step by
    // run_multi_period_step_ref below). LastDateV1 keeps the pre-W0 single reference
    // anchored at the panel's LAST date (look-ahead; zero cap for early delistings).
    if (cfg.participation_cap > 0.0) {
        ATX_TRY(const auto vol_fid, research.field_id("volume"));
        ATX_TRY(const auto cls_fid, research.field_id("close"));
        part_vol_all = research.field_all(vol_fid);
        part_cls_all = research.field_all(cls_fid);
        part_adv.assign(M, 0.0);
        part_price.assign(M, 0.0);
        if (!part_per_step) {
            constexpr atx::usize kAdvWindow = 20;
            const atx::usize last = D - 1;
            const atx::usize win_begin = (last + 1 > kAdvWindow) ? (last + 1 - kAdvWindow) : 0;
            for (atx::usize i = 0; i < M; ++i) {
                atx::f64 sum = 0.0;
                atx::usize n = 0;
                for (atx::usize t = win_begin; t <= last; ++t) {
                    const atx::f64 v = part_vol_all[t * M + i];
                    if (!std::isnan(v)) { sum += v; ++n; }
                }
                part_adv[i]   = (n > 0) ? sum / static_cast<atx::f64>(n) : 0.0;
                part_price[i] = part_cls_all[last * M + i];
            }
        }
        risk::ConstraintSet cs;
        cs.gross.gross_leverage = gross_val;   // carry the fast-path gross forward
        cs.gross.dollar_neutral = true;        // (the augmented path does not read cfg.single)
        cs.pos  = risk::PositionCap{name_cap_val};
        cs.part = risk::ParticipationCap{cfg.participation_cap};
        mc.constraints = std::move(cs);
        mc.ref.adv   = part_adv;   // borrowed spans (see the function-scope note above)
        mc.ref.price = part_price;
        // Reuse the SAME AUM field stage_report.cpp assumes for capacity metrics, so
        // construction-time and post-hoc capacity share ONE dollar scale.
        mc.ref.nav = cfg.report_aum > 0.0 ? cfg.report_aum : 1e9;
        mc.ref.horizon_days = 1.0; // conservative 1-day participation horizon
    }

    risk::MultiPeriodOptimizer mpo;
    mpo.cfg = mc;

    atx::engine::book::CostInputs cost;
    cost.kappa = cfg.turnover_penalty;
    cost.round_trip_cost_bps = cfg.set_flags.count("cost-bps") ? cfg.cost_bps : 0.0;

    // 6. Callbacks + run.
    ATX_TRY(const auto alpha_fid, combo.field_id("alpha"));

    // S1-5: factor/industry neutralization (opt-in via risk_cfg.group_neutralize).
    // FactorModel::neutralize residualizes a signal against a model's exposure
    // columns IN PLACE (s <- s - X(XtX)^-1 Xt s); it needs a MUTABLE span, so
    // when the flag is on we precompute one neutralized copy of the WHOLE combo
    // signal (every period) upfront and have alpha_at read from that buffer
    // instead of combo directly. group_neutralize==false (inert default) skips
    // this block entirely -- alpha_at reads combo verbatim, byte-identical to
    // pre-S1. S1 fix-loop (part D): each date `d` is neutralized against
    // model_at(d) -- the IN-FORCE step model for the rebalance step covering
    // `d` (Diagonal: the single model, a no-op since its exposures are all
    // zero; Factor: the per-window PIT model, FORWARD-FILLED between
    // rebalance dates -- model_at already reduces any date to
    // floor(date/step), so calling it here with a non-schedule date is
    // exactly the forward-fill this needs; group_neutralize defaults false,
    // so the default path is unaffected either way). The neutralize itself is
    // a deterministic linear residualization (no RNG); NaN cells propagate
    // per FactorModel::neutralize's documented policy (a WeightPolicy
    // downstream maps a NaN weight to 0 -- unchanged from today's contract).
    std::vector<atx::f64> neutralized_flat;
    if (risk_cfg.group_neutralize) {
        neutralized_flat.resize(D * M);
        for (atx::usize d = 0; d < D; ++d) {
            const auto cs = combo.field_cross_section(alpha_fid, d);
            std::copy(cs.begin(), cs.end(), neutralized_flat.begin() + static_cast<std::ptrdiff_t>(d * M));
            std::span<atx::f64> row{neutralized_flat.data() + d * M, M};
            model_at(d).neutralize(row);
        }
    }

    auto alpha_at = [&combo, alpha_fid, &neutralized_flat, &risk_cfg, M](atx::usize period)
        -> std::span<const atx::f64>
    {
        if (risk_cfg.group_neutralize) {
            return std::span<const atx::f64>{neutralized_flat.data() + period * M, M};
        }
        return combo.field_cross_section(alpha_fid, period);
    };

    risk::MultiPeriodResult result;
    if (part_per_step) {
        const auto refresh_ref = [&](atx::usize d) {
            trailing_participation_reference(part_vol_all, part_cls_all, M, d,
                                            std::span<atx::f64>{part_adv},
                                            std::span<atx::f64>{part_price});
        };
        ATX_TRY(result,
                run_multi_period_step_ref(mc, sched, alpha_at, model_at, cost, refresh_ref));
    } else {
        ATX_TRY(result, mpo.run(sched, alpha_at, model_at, cost));
    }

    // 7. Pack books_flat from MVO result.
    std::vector<atx::f64> flat;
    flat.reserve(S * M);
    for (atx::usize s = 0; s < S; ++s) {
        for (atx::usize i = 0; i < M; ++i) {
            flat.push_back(result.books[s][i]);
        }
    }

    // 8. Serialize + return StageResult.
    // S5-1: measure FIRST (always -- the design-spec's "measure before gate"
    // mitigation), gate opt-in second. The rate is computed AFTER write_books so a
    // rejected run's own books/sidecar stay on disk and inspectable (mirrors
    // --blocking-pbo's documented "already persisted" caveat). The added kv changes
    // sr.kvs's content but never sr.digest (the books panel bytes), so with the gate
    // off the books digest is byte-identical to pre-S5-1.
    ATX_TRY(auto sr, write_books(flat, result.turnover, result.cost_bps));
    const atx::f64 book_turnover = book_turnover_per_day(result.turnover, sched.periods);
    sr.kvs.emplace_back("book_turnover_per_day", std::to_string(book_turnover));
    // W0-I0a telemetry, present only on the opt-in paths it describes (kvs only,
    // never the books digest): which dead set the crowding factors used per step, and
    // which ADV reference the participation cap used.
    if (dead_lib_ptr != nullptr) {
        sr.kvs.emplace_back("dead_alpha_rule", pit.dead == DeadAlphaRule::AdmittedAsOfLastPeriodV1
                                                   ? "admitted_last_period_v1"
                                                   : "dead_or_decaying_per_step_v2");
        sr.kvs.emplace_back("dead_alpha_axis", dead_axis.known ? "recorded" : "unrecorded");
        sr.kvs.emplace_back("dead_alpha_steps", std::to_string(models.dead_steps));
        sr.kvs.emplace_back("dead_alpha_max_ids", std::to_string(models.max_dead));
    }
    if (cfg.participation_cap > 0.0) {
        sr.kvs.emplace_back("participation_adv_ref",
                            part_per_step ? "trailing_pit_per_rebalance_v2" : "last_date_v1");
    }
    if (cfg.book_turnover_gate > 0.0 && book_turnover > cfg.book_turnover_gate) {
        return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                              "optimize: book turnover " + std::to_string(book_turnover) +
                                  "/day exceeds --book-turnover-gate " +
                                  std::to_string(cfg.book_turnover_gate));
    }
    return atx::core::Ok(std::move(sr));
}

} // namespace atx::impl
