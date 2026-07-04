#include "stages.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

#include "atx/core/error.hpp"
#include "atx/core/types.hpp"

#include "atx/engine/alpha/panel.hpp"
#include "atx/engine/data/adapt_factor.hpp"
#include "atx/engine/library/library.hpp"
#include "atx/engine/risk/garleanu_pedersen.hpp"
#include "atx/engine/risk/multi_period.hpp"
#include "atx/engine/risk/optimizer.hpp"

#include "artifacts.hpp"
#include "book_shape.hpp"
#include "config.hpp"
#include "dead_alpha_wire.hpp"
#include "serialize_panel.hpp"
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
    // 1. Validate required flags.
    if (cfg.panel.empty() || cfg.combo.empty() || cfg.books_out.empty()) {
        return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                              "optimize: --panel, --combo, and --out required");
    }

    // 2. Load research and combo panels.
    ATX_TRY(auto research, read_panel(cfg.panel));
    ATX_TRY(auto combo, read_panel(cfg.combo));

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
        ATX_TRY(auto digest, write_panel(cpanel, cfg.books_out));

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
        }

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
        std::optional<risk::FactorModel> gp_v;
        bool gp_fallback = false;
        if (cfg.gp_trading) {
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
        }

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
            if (cfg.gp_trading && gp_v.has_value()) {
                // alpha_bar: the per-name RETURN-space signal this period -- the SAME raw
                // cross-section shape_book above just turned into the legacy target `w`.
                // NaN names are preserved (gp_aim_and_value maps them to 0 in the V^-1 apply).
                std::vector<atx::f64> alpha_bar(cs.begin(), cs.end());
                auto gp = risk::gp_aim_and_value(std::span<const atx::f64>{alpha_bar}, *gp_v,
                                                 cfg.gp_risk_aversion);
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
    //   Diagonal (inert default, reached by every existing caller via the
    //   zero-arg run_optimize forward above): UNCHANGED -- byte-identical to
    //   pre-S1. ONE whole-panel model, applied to every period
    //   (build_risk_model's Diagonal branch lowers diagonal_risk_model's own
    //   (X, F, D) straight back into a FactorModel).
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
    // model_at's TYPE (const risk::FactorModel&) and the mpo.run call below
    // are UNCHANGED; only the backing model's SOURCE + cadence differ.
    std::optional<risk::FactorModel> single_model; // Diagonal path only
    std::vector<risk::FactorModel> step_models;    // Factor path only; one per sched.periods[s]

    // S1 (p9): resolve the dead-alpha wire ONCE, shared by both the Diagonal
    // single-model branch and the Factor per-step loop below (Library::open does
    // real sqlite I/O; reopening it per rebalance step would be wasteful and
    // unnecessary since the resolved triple is identical for every step -- the
    // library's own holdings/period axis has no established alignment with the
    // per-step fit_end, so a single "latest known crowding snapshot" is used for
    // the whole run rather than attempting a per-step correspondence).
    std::optional<library::Library> dead_lib_opt = maybe_open_dead_lib(cfg, risk_cfg);
    const library::Library* dead_lib_ptr = dead_lib_opt.has_value() ? &*dead_lib_opt : nullptr;
    std::vector<combine::AlphaId> dead_ids;
    atx::usize dead_as_of = 0;
    if (dead_lib_ptr != nullptr) {
        dead_as_of = dead_lib_ptr->n_periods() > 0 ? dead_lib_ptr->n_periods() - 1 : 0;
        dead_ids = collect_dead_alpha_ids(*dead_lib_ptr, dead_as_of);
    }

    if (risk_cfg.kind == risk::RiskModelKind::Diagonal) {
        ATX_TRY(auto artifact,
                build_risk_model(research, risk_cfg, /*group_id=*/{}, dead_lib_ptr, dead_ids, dead_as_of));
        ATX_TRY(auto model, data::artifact_to_factor_model(artifact));
        single_model.emplace(std::move(model));
    } else {
        const risk::RiskModelConfig diag_fallback_cfg; // kind==Diagonal (default) -- warm-up fallback
        step_models.reserve(S);
        for (atx::usize s = 0; s < S; ++s) {
            const atx::usize fit_end = sched.periods[s] + 1U; // PIT: through `period` inclusive
            auto factor_artifact =
                build_risk_model(research, risk_cfg, {}, dead_lib_ptr, dead_ids, dead_as_of, fit_end);
            if (factor_artifact.has_value()) {
                ATX_TRY(auto step_model, data::artifact_to_factor_model(*factor_artifact));
                step_models.push_back(std::move(step_model));
            } else {
                // Warm-up fallback: too little history yet for a genuine
                // Factor fit at this step -- a PIT diagonal over [0, fit_end).
                ATX_TRY(auto diag_artifact, build_risk_model(research, diag_fallback_cfg, {}, dead_lib_ptr,
                                                             dead_ids, dead_as_of, fit_end));
                ATX_TRY(auto diag_model, data::artifact_to_factor_model(diag_artifact));
                step_models.push_back(std::move(diag_model));
            }
        }
    }

    // date -> step lookup: periods are dense multiples of `step`
    // (sched.periods[s] == s*step by construction above), so the step
    // covering any date d is floor(d/step) -- exact at a schedule date, and
    // FORWARD-FILLED between them (the "in-force step model" the S1-5
    // neutralize block below needs for a non-schedule date).
    auto model_at = [&](atx::usize period) -> const risk::FactorModel& {
        if (risk_cfg.kind == risk::RiskModelKind::Diagonal) {
            return *single_model;
        }
        return step_models[period / step];
    };

    risk::MultiPeriodConfig mc;
    mc.single.risk_aversion   = cfg.set_flags.count("risk-aversion")
                                    ? cfg.risk_aversion : 1.0;
    mc.single.gross_leverage  = gross_val;
    mc.single.name_cap        = name_cap_val;
    mc.single.dollar_neutral  = true;
    mc.single.turnover_penalty = cfg.turnover_penalty;
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

    ATX_TRY(auto result, mpo.run(sched, alpha_at, model_at, cost));

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
    if (cfg.book_turnover_gate > 0.0 && book_turnover > cfg.book_turnover_gate) {
        return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                              "optimize: book turnover " + std::to_string(book_turnover) +
                                  "/day exceeds --book-turnover-gate " +
                                  std::to_string(cfg.book_turnover_gate));
    }
    return atx::core::Ok(std::move(sr));
}

} // namespace atx::impl
