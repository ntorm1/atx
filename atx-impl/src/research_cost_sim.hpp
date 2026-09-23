#pragma once

// research_cost_sim.hpp -- cost-aware research simulation for alpha admission.
//
// The frictionless research_sim.hpp simulator scores an alpha as if trading
// were free, so a high-turnover alpha with a good gross Sharpe is admitted even
// when its edge cannot survive costs. This header scores the same weight path
// net of the replay's own per-name cost model (book::ReplayCostModel), so the
// fitness, the optimizer and the replay can share one cost calibration.
//
// Accounting, per period t (weights row t is held over (t, t+1]):
//   drifted_{t,i} = w_{t-1,i} (1 + r_{t-1,i}) / (1 + gross_{t-1})
//   trade_{t,i}   = (w_{t,i} - drifted_{t,i}) * aum        (t = 0: from cash)
//   cost_t        = sum_i model.cost(i, t, trade_{t,i}, liquidity_{t,i}) / aum
//   gross_t       = sum_i w_{t,i} r_{t,i};   net_t = gross_t - cost_t
// A research sim never rations fills: every requested trade is charged in full
// through ReplayCostModel::unrationed_cost, i.e. at the model's UNCAPPED rate
// for the full size (for sqrt impact that is (q/f)^delta above a capped fill's
// average rate, so large trades are not undercharged). A trade the model cannot
// price (no usable liquidity estimate: NaN/0/negative ADV) is NEVER free: it is
// charged unusable_liquidity_penalty_bps, or rejected (Err) when that penalty is
// NaN. Weights of NaN mean "no position"; a nonzero weight on a NaN return is
// rejected.
//
// Admission: net annualized Sharpe >= min_net_sharpe AND mean net return > 0.

#include <algorithm>
#include <cmath>
#include <span>
#include <utility>
#include <vector>

#include "atx/core/error.hpp"
#include "atx/core/types.hpp"
#include "atx/engine/book/replay_cost.hpp"

namespace atx::impl {

struct ResearchCostSimConfig {
    atx::f64 aum{1.0e8};
    atx::f64 min_net_sharpe{0.5};
    atx::f64 periods_per_year{252.0};
    // Per-dollar charge (bps) on trades in names with no usable liquidity row.
    // Deliberately punitive (10%): an alpha concentrated in names without a
    // liquidity estimate must not look cheaper than one trading liquid names.
    // NaN == reject the run instead. Must otherwise be finite and >= 0.
    atx::f64 unusable_liquidity_penalty_bps{1000.0};
};

struct ResearchCostSimResult {
    std::vector<atx::f64> gross_returns;
    std::vector<atx::f64> net_returns;
    std::vector<atx::f64> turnover; // One-way L1 traded weight per period.
    atx::f64 gross_sharpe{};
    atx::f64 net_sharpe{};
    atx::f64 mean_turnover{};
    atx::f64 cost_drag_bps{}; // Mean per-period cost in bps of AUM.
    bool admitted{};
};

namespace research_cost_detail {

[[nodiscard]] inline atx::f64 annualized_sharpe(std::span<const atx::f64> r, atx::f64 periods) {
    if (r.size() < 2) return 0.0;
    atx::f64 mean = 0.0;
    for (const auto x : r) mean += x;
    mean /= static_cast<atx::f64>(r.size());
    atx::f64 var = 0.0;
    for (const auto x : r) var += (x - mean) * (x - mean);
    var /= static_cast<atx::f64>(r.size() - 1);
    return var > 0.0 ? mean / std::sqrt(var) * std::sqrt(periods) : 0.0;
}

// Cost of one requested trade in dollars at the model's unrationed full-size
// rate; an unpriceable trade takes the penalty rate, or NaN when rejecting.
[[nodiscard]] inline atx::f64 unrationed_cost(const atx::engine::book::ReplayCostModel &model,
                                              atx::usize i, atx::usize t, atx::f64 trade,
                                              const atx::engine::book::LiquidityRow &row,
                                              atx::f64 penalty_bps) {
    const auto charged = model.unrationed_cost(i, t, trade, row);
    if (std::isfinite(charged) && charged >= 0.0) return charged;
    return std::abs(trade) * penalty_bps * 1.0e-4; // NaN penalty -> NaN -> Err.
}

} // namespace research_cost_detail

// weights/returns are periods x names, row-major; liquidity is the same shape
// or empty when the model does not need it.
[[nodiscard]] inline atx::core::Result<ResearchCostSimResult>
research_cost_sim(std::span<const atx::f64> weights, std::span<const atx::f64> returns,
                  atx::usize periods, atx::usize names,
                  std::span<const atx::engine::book::LiquidityRow> liquidity,
                  const atx::engine::book::ReplayCostModel &model,
                  const ResearchCostSimConfig &cfg = {}) {
    using atx::core::Err;
    using atx::core::ErrorCode;
    const auto cells = periods * names;
    if (periods == 0 || names == 0 || weights.size() != cells || returns.size() != cells ||
        (model.needs_liquidity() && liquidity.size() != cells) || !std::isfinite(cfg.aum) ||
        cfg.aum <= 0.0 || !std::isfinite(cfg.periods_per_year) || cfg.periods_per_year <= 0.0 ||
        !std::isfinite(cfg.min_net_sharpe) ||
        (!std::isnan(cfg.unusable_liquidity_penalty_bps) &&
         (!std::isfinite(cfg.unusable_liquidity_penalty_bps) ||
          cfg.unusable_liquidity_penalty_bps < 0.0))) {
        return Err(ErrorCode::InvalidArgument, "research cost sim: invalid shape or config");
    }
    ResearchCostSimResult out;
    out.gross_returns.resize(periods);
    out.net_returns.resize(periods);
    out.turnover.resize(periods);
    std::vector<atx::f64> drifted(names, 0.0);
    const atx::engine::book::LiquidityRow empty_row{};
    atx::f64 cost_sum = 0.0;
    for (atx::usize t = 0; t < periods; ++t) {
        atx::f64 gross = 0.0;
        atx::f64 cost = 0.0;
        atx::f64 traded = 0.0;
        for (atx::usize i = 0; i < names; ++i) {
            const auto raw = weights[t * names + i];
            const auto w = std::isnan(raw) ? 0.0 : raw;
            const auto r = returns[t * names + i];
            if (!std::isfinite(w) || (w != 0.0 && !std::isfinite(r))) {
                return Err(ErrorCode::InvalidArgument,
                           "research cost sim: nonfinite weight or held return");
            }
            const auto delta = w - drifted[i];
            if (delta != 0.0) {
                const auto &row = liquidity.empty() ? empty_row : liquidity[t * names + i];
                const auto charged = research_cost_detail::unrationed_cost(
                    model, i, t, delta * cfg.aum, row, cfg.unusable_liquidity_penalty_bps);
                if (!std::isfinite(charged)) {
                    return Err(ErrorCode::OutOfRange,
                               "research cost sim: trade in a name with no usable liquidity");
                }
                cost += charged / cfg.aum;
                traded += std::abs(delta);
            }
            if (w != 0.0) gross += w * r;
        }
        if (!std::isfinite(gross) || !std::isfinite(cost) || gross <= -1.0) {
            return Err(ErrorCode::OutOfRange, "research cost sim: nonfinite or ruinous period");
        }
        for (atx::usize i = 0; i < names; ++i) {
            const auto raw = weights[t * names + i];
            const auto w = std::isnan(raw) ? 0.0 : raw;
            drifted[i] = w == 0.0 ? 0.0 : w * (1.0 + returns[t * names + i]) / (1.0 + gross);
        }
        out.gross_returns[t] = gross;
        out.net_returns[t] = gross - cost;
        out.turnover[t] = traded;
        cost_sum += cost;
        out.mean_turnover += traded;
    }
    const auto count = static_cast<atx::f64>(periods);
    out.mean_turnover /= count;
    out.cost_drag_bps = cost_sum / count * 1.0e4;
    out.gross_sharpe = research_cost_detail::annualized_sharpe(out.gross_returns, cfg.periods_per_year);
    out.net_sharpe = research_cost_detail::annualized_sharpe(out.net_returns, cfg.periods_per_year);
    atx::f64 mean_net = 0.0;
    for (const auto x : out.net_returns) mean_net += x;
    out.admitted = out.net_sharpe >= cfg.min_net_sharpe && mean_net > 0.0;
    return atx::core::Ok(std::move(out));
}

} // namespace atx::impl
