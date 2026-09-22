#include "replay_diagnostics.hpp"

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace atx::impl {
namespace {
using atx::core::Err;
using atx::core::ErrorCode;
using atx::core::Ok;
using atx::core::Result;
using atx::core::Status;
using atx::engine::book::ReplayInterval;
using atx::engine::book::ReplayResult;

[[nodiscard]] bool finite(std::initializer_list<atx::f64> values) {
    return std::all_of(values.begin(), values.end(), [](atx::f64 value) {
        return std::isfinite(value);
    });
}

[[nodiscard]] bool close_money(atx::f64 left, atx::f64 right) {
    const atx::f64 scale = std::max({1.0, std::abs(left), std::abs(right)});
    return std::abs(left / scale - right / scale) <= 1e-10;
}

[[nodiscard]] bool balanced(const ReplayInterval &row) {
    const atx::f64 scale = std::max({1.0, row.pretrade_nav, row.nav,
        std::abs(row.gross_pnl), row.trade_cost, row.borrow_cost});
    const atx::f64 expected = row.pretrade_nav / scale + row.gross_pnl / scale -
                             row.trade_cost / scale - row.borrow_cost / scale;
    return std::abs(row.nav / scale - expected) <= 1e-10;
}

[[nodiscard]] Status add(atx::f64 &total, atx::f64 value) {
    const atx::f64 next = total + value;
    if (!std::isfinite(next)) {
        return Err(ErrorCode::OutOfRange, "replay diagnostics: dollar total overflow");
    }
    total = next;
    return Ok();
}

[[nodiscard]] Status validate(const ReplayResult &replay,
                              const atx::engine::alpha::Panel &research) {
    const auto dates = research.dates();
    const auto instruments = research.instruments();
    if (dates == 0 || instruments == 0 ||
        dates > std::numeric_limits<atx::usize>::max() / instruments ||
        replay.intervals.size() != dates - 1 || replay.final_tri_units.size() != instruments ||
        !finite({replay.initial_nav, replay.final_nav, replay.final_cash, replay.final_assets}) ||
        replay.initial_nav <= 0.0 || replay.final_nav <= 0.0) {
        return Err(ErrorCode::InvalidArgument, "replay diagnostics: invalid replay shape/NAV");
    }
    for (const auto units : replay.final_tri_units) {
        if (!std::isfinite(units)) {
            return Err(ErrorCode::InvalidArgument, "replay diagnostics: nonfinite final holdings");
        }
    }
    atx::f64 previous_nav = replay.initial_nav;
    std::optional<atx::usize> previous_decision;
    for (atx::usize i = 0; i < replay.intervals.size(); ++i) {
        const auto &row = replay.intervals[i];
        if (row.start_period != i || row.end_period != i + 1 ||
            !finite({row.pretrade_nav, row.cash, row.assets, row.nav, row.gross_pnl,
                     row.trade_cost, row.borrow_cost, row.net_return, row.traded_dollars,
                     row.start_gross, row.end_gross}) || row.pretrade_nav <= 0.0 || row.nav <= 0.0 ||
            row.trade_cost < 0.0 || row.borrow_cost < 0.0 || row.traded_dollars < 0.0 ||
            row.start_gross < 0.0 || row.end_gross < 0.0 ||
            !close_money(row.pretrade_nav, previous_nav) || !balanced(row)) {
            return Err(ErrorCode::InvalidArgument, "replay diagnostics: invalid interval ledger");
        }
        const atx::f64 actual_return = row.nav / row.pretrade_nav - 1.0;
        const atx::f64 balance_scale = std::max({1.0, row.nav, std::abs(row.cash),
                                                std::abs(row.assets)});
        if (!std::isfinite(actual_return) || !close_money(actual_return, row.net_return) ||
            std::abs(row.nav / balance_scale - row.cash / balance_scale -
                     row.assets / balance_scale) > 1e-10 ||
            (row.decision_period && *row.decision_period > row.start_period) ||
            (previous_decision && (!row.decision_period ||
                                    *row.decision_period < *previous_decision))) {
            return Err(ErrorCode::InvalidArgument, "replay diagnostics: invalid return/decision");
        }
        previous_nav = row.nav;
        previous_decision = row.decision_period;
    }
    const atx::f64 final_scale = std::max({1.0, replay.final_nav, std::abs(replay.final_cash),
                                           std::abs(replay.final_assets)});
    if (!close_money(previous_nav, replay.final_nav) ||
        std::abs(replay.final_nav / final_scale - replay.final_cash / final_scale -
                 replay.final_assets / final_scale) > 1e-10) {
        return Err(ErrorCode::InvalidArgument, "replay diagnostics: final NAV does not reconcile");
    }
    std::vector<atx::f64> traded(replay.intervals.size(), 0.0);
    for (atx::usize i = 0; i < replay.trades.size(); ++i) {
        const auto &trade = replay.trades[i];
        if (trade.period >= replay.intervals.size() || trade.instrument >= instruments ||
            !std::isfinite(trade.dollar_delta) || trade.dollar_delta == 0.0) {
            return Err(ErrorCode::InvalidArgument, "replay diagnostics: invalid sparse trade");
        }
        const auto decision = replay.intervals[trade.period].decision_period;
        if (!decision || trade.decision_period != *decision || (i > 0 &&
            (trade.period < replay.trades[i - 1].period ||
             (trade.period == replay.trades[i - 1].period &&
              trade.instrument <= replay.trades[i - 1].instrument)))) {
            return Err(ErrorCode::InvalidArgument, "replay diagnostics: unordered trade/decision");
        }
        ATX_TRY_VOID(add(traded[trade.period], std::abs(trade.dollar_delta)));
    }
    for (atx::usize i = 0; i < traded.size(); ++i) {
        if (!close_money(traded[i], replay.intervals[i].traded_dollars)) {
            return Err(ErrorCode::InvalidArgument, "replay diagnostics: trade totals do not match");
        }
    }
    return Ok();
}

[[nodiscard]] Result<ReplayPerformance> performance(std::span<const ReplayInterval> rows,
                                                   atx::f64 initial_nav) {
    ReplayPerformance result;
    result.observed_intervals = rows.size();
    result.initial_nav = initial_nav;
    result.final_nav = rows.empty() ? initial_nav : rows.back().nav;
    atx::f64 peak = initial_nav;
    atx::f64 return_scale = 0.0;
    for (const auto &row : rows) {
        ATX_TRY_VOID(add(result.gross_pnl_dollars, row.gross_pnl));
        ATX_TRY_VOID(add(result.trade_cost_dollars, row.trade_cost));
        ATX_TRY_VOID(add(result.borrow_cost_dollars, row.borrow_cost));
        ATX_TRY_VOID(add(result.abs_trade_dollars, row.traded_dollars));
        peak = std::max(peak, row.nav);
        result.max_drawdown = std::max(result.max_drawdown, 1.0 - row.nav / peak);
        return_scale = std::max(return_scale, std::abs(row.net_return));
    }
    result.net_pnl_dollars = result.final_nav - initial_nav;
    result.total_return = result.final_nav / initial_nav - 1.0;
    if (!std::isfinite(result.total_return)) {
        return Err(ErrorCode::OutOfRange, "replay diagnostics: total return overflow");
    }
    // Scaling cancels in mean/standard-deviation and prevents squaring extreme
    // but finite returns. Welford then computes SAMPLE variance over all intervals.
    if (rows.size() >= 2 && return_scale > 0.0) {
        atx::f64 mean = 0.0;
        atx::f64 m2 = 0.0;
        atx::usize count = 0;
        for (const auto &row : rows) {
            ++count;
            const atx::f64 value = row.net_return / return_scale;
            const atx::f64 delta = value - mean;
            mean += delta / static_cast<atx::f64>(count);
            m2 += delta * (value - mean);
        }
        const atx::f64 variance = m2 / static_cast<atx::f64>(rows.size() - 1);
        if (variance > 0.0) {
            const atx::f64 sharpe = mean / std::sqrt(variance) * std::sqrt(252.0);
            if (!std::isfinite(sharpe)) {
                return Err(ErrorCode::OutOfRange, "replay diagnostics: Sharpe overflow");
            }
            result.sharpe_252 = sharpe;
        }
    }
    return Ok(std::move(result));
}

[[nodiscard]] ReplayTradeParticipation participation(
    atx::usize trade_index, const atx::engine::book::ReplayTrade &trade,
    atx::usize instruments, std::span<const atx::f64> raw_close,
    std::span<const atx::f64> volume) {
    ReplayTradeParticipation result;
    result.trade_index = trade_index;
    if (raw_close.empty() || volume.empty()) return result;
    if (trade.period < kReplayAdvObservations) {
        result.status = ReplayLiquidityStatus::InsufficientHistory;
        return result;
    }
    atx::f64 mean = 0.0;
    atx::usize count = 0;
    for (atx::usize period = trade.period - kReplayAdvObservations; period < trade.period; ++period) {
        const auto cell = period * instruments + trade.instrument;
        const atx::f64 price = raw_close[cell];
        const atx::f64 shares = volume[cell];
        const atx::f64 dollars = price * shares;
        if (!finite({price, shares, dollars}) || price <= 0.0 || shares < 0.0 ||
            (shares > 0.0 && dollars <= 0.0)) {
            result.status = ReplayLiquidityStatus::InvalidWindow;
            return result;
        }
        ++count;
        mean += (dollars - mean) / static_cast<atx::f64>(count);
    }
    if (!std::isfinite(mean) || mean <= 0.0) {
        result.status = ReplayLiquidityStatus::NonpositiveAdv;
        return result;
    }
    result.prior_dollar_adv = mean;
    const atx::f64 ratio = std::abs(trade.dollar_delta) / mean;
    if (!std::isfinite(ratio) || ratio <= 0.0) {
        result.status = ReplayLiquidityStatus::UnrepresentableRatio;
        return result;
    }
    result.participation = ratio;
    result.status = ReplayLiquidityStatus::Known;
    return result;
}

} // namespace

std::string_view replay_liquidity_status_name(ReplayLiquidityStatus status) noexcept {
    switch (status) {
    case ReplayLiquidityStatus::Known: return "known";
    case ReplayLiquidityStatus::InsufficientHistory: return "insufficient-prior-observations";
    case ReplayLiquidityStatus::MissingFields: return "missing-raw-close-or-volume";
    case ReplayLiquidityStatus::InvalidWindow: return "invalid-raw-liquidity-window";
    case ReplayLiquidityStatus::NonpositiveAdv: return "nonpositive-dollar-adv";
    case ReplayLiquidityStatus::UnrepresentableRatio: return "unrepresentable-participation";
    }
    return "invalid-status";
}

Result<ReplayDiagnostics> diagnose_replay(const ReplayResult &replay,
                                         const atx::engine::alpha::Panel &research,
                                         std::optional<atx::usize> fit_boundary) {
    ATX_TRY_VOID(validate(replay, research));
    if (fit_boundary && *fit_boundary > research.dates()) {
        return Err(ErrorCode::InvalidArgument, "replay diagnostics: fit boundary outside axis");
    }
    ReplayDiagnostics result;
    const std::span<const ReplayInterval> intervals{replay.intervals};
    ATX_TRY(result.full, performance(intervals, replay.initial_nav));
    if (fit_boundary) {
        for (atx::usize i = 0; i < intervals.size(); ++i) {
            const auto &row = intervals[i];
            if (row.decision_period && *row.decision_period >= *fit_boundary) {
                result.first_post_fit_effective_observation = row.start_period;
                ATX_TRY(auto post_fit, performance(intervals.subspan(i), row.pretrade_nav));
                result.post_fit = std::move(post_fit);
                break;
            }
        }
    }
    std::span<const atx::f64> raw_close;
    std::span<const atx::f64> volume;
    if (const auto field = research.field_id("raw_close"); field) {
        raw_close = research.field_all(*field);
    }
    if (const auto field = research.field_id("volume"); field) {
        volume = research.field_all(*field);
    }
    result.trade_participation.reserve(replay.trades.size());
    for (atx::usize i = 0; i < replay.trades.size(); ++i) {
        auto trade = participation(i, replay.trades[i], research.instruments(), raw_close, volume);
        if (trade.participation) {
            ++result.known_participation_count;
            result.max_known_participation = result.max_known_participation
                ? std::max(*result.max_known_participation, *trade.participation)
                : *trade.participation;
        } else {
            ++result.unknown_participation_count;
        }
        result.trade_participation.push_back(std::move(trade));
    }
    if (result.unknown_participation_count == 0) {
        result.max_participation = result.max_known_participation;
    }
    return Ok(std::move(result));
}

} // namespace atx::impl
