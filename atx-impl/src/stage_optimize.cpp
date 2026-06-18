#include "stages.hpp"

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

    // 3. Build diagonal FactorModel from per-instrument TRI-return variance.
    const atx::usize M = research.instruments();
    const atx::usize D = research.dates();

    ATX_TRY(auto model, diagonal_risk_model(research));

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

    // 5. MultiPeriodConfig.
    risk::MultiPeriodConfig mc;
    mc.single.risk_aversion   = cfg.set_flags.count("risk-aversion")
                                    ? cfg.risk_aversion : 1.0;
    mc.single.gross_leverage  = cfg.gross    > 0.0 ? cfg.gross    : 1.0;
    mc.single.name_cap        = cfg.name_cap > 0.0 ? cfg.name_cap : 1.0;
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

    // 7. Serialize books as a weight panel (S periods x M instruments, field "weight").
    std::vector<atx::f64> flat;
    flat.reserve(S * M);
    for (atx::usize s = 0; s < S; ++s) {
        for (atx::usize i = 0; i < M; ++i) {
            flat.push_back(result.books[s][i]);
        }
    }
    std::vector<std::uint8_t> uni(S * M, 1u);

    ATX_TRY(auto cpanel,
            alpha::Panel::create(S, M, {"weight"}, {flat}, uni));
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
        mf << "periods="    << S                         << '\n';
        mf << "instruments=" << M                        << '\n';
        mf << "gross="      << mc.single.gross_leverage  << '\n';
        mf << "name_cap="   << mc.single.name_cap        << '\n';
        mf << "rebalance="  << rebalance_str             << '\n';
        for (atx::usize s = 0; s < S; ++s) {
            mf << "s=" << s
               << " period="   << sched.periods[s]
               << " turnover=" << result.turnover[s]
               << " cost_bps=" << result.cost_bps[s]
               << '\n';
        }
    }

    // 8. Return StageResult.
    StageResult sr;
    sr.digest = digest;
    sr.kvs = {
        {"periods",     std::to_string(S)},
        {"instruments", std::to_string(M)},
        {"gross",       std::to_string(mc.single.gross_leverage)},
        {"name_cap",    std::to_string(mc.single.name_cap)},
        {"rebalance",   step == 1U ? "daily" : "weekly"},
        {"books",       to_hex16(digest)},
    };
    return atx::core::Ok(std::move(sr));
}

} // namespace atx::impl
