#include "stages.hpp"

#include <cmath>
#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

#include "atx/core/error.hpp"
#include "atx/core/types.hpp"

#include "atx/engine/alpha/panel.hpp"
#include "atx/engine/risk/multi_period.hpp"
#include "atx/engine/risk/optimizer.hpp"

#include "artifacts.hpp"
#include "book_shape.hpp"
#include "config.hpp"
#include "diag_risk.hpp"
#include "serialize_panel.hpp"

namespace atx::impl {

namespace alpha = atx::engine::alpha;
namespace risk  = atx::engine::risk;

atx::core::Result<StageResult> run_optimize(const RunConfig& cfg)
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
    if (cfg.position_mode) {
        ATX_TRY(const auto alpha_fid, combo.field_id("alpha"));
        std::vector<atx::f64> books_flat(S * M, 0.0);
        std::vector<double>   turnover(S, 0.0);
        std::vector<double>   cost_bps(S, 0.0);

        std::vector<atx::f64> prev(M, 0.0);  // w[-1] = 0 (flat)

        for (atx::usize s = 0; s < S; ++s) {
            const atx::usize d = sched.periods[s];
            const auto cs = combo.field_cross_section(alpha_fid, d);
            std::vector<atx::f64> w(cs.begin(), cs.end());
            std::vector<std::uint8_t> live(M);
            for (atx::usize i = 0; i < M; ++i) {
                live[i] = research.in_universe(d, i) ? 1u : 0u;
            }
            shape_book(w, std::span<const std::uint8_t>{live}, gross_val, name_cap_val);

            // Compute per-period turnover: Sigma_i |w[s] - w[s-1]|.
            double tv = 0.0;
            for (atx::usize i = 0; i < M; ++i) tv += std::fabs(w[i] - prev[i]);
            turnover[s] = tv;

            // Store weights and update previous book.
            for (atx::usize i = 0; i < M; ++i) {
                books_flat[s * M + i] = w[i];
                prev[i] = w[i];
            }
        }

        return write_books(books_flat, turnover, cost_bps);
    }

    // 5b. MVO path (default: position_mode=false).
    // Build the diagonal FactorModel from per-instrument TRI-return variance — needed
    // ONLY by the mean-variance path; position-mode returned above without it.
    ATX_TRY(auto model, diagonal_risk_model(research));
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
    cost.round_trip_cost_bps = 0.0;

    // 6. Callbacks + run.
    ATX_TRY(const auto alpha_fid, combo.field_id("alpha"));

    auto alpha_at = [&combo, alpha_fid](atx::usize period)
        -> std::span<const atx::f64>
    {
        return combo.field_cross_section(alpha_fid, period);
    };
    auto model_at = [&model](atx::usize) -> const risk::FactorModel& {
        return model;
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
    return write_books(flat, result.turnover, result.cost_bps);
}

} // namespace atx::impl
