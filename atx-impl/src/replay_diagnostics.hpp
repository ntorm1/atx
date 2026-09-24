#pragma once

#include <optional>
#include <string_view>
#include <vector>

#include "atx/core/error.hpp"
#include "atx/core/types.hpp"
#include "atx/engine/alpha/panel.hpp"
#include "atx/engine/book/replay.hpp"

namespace atx::impl {

inline constexpr atx::usize kReplayAdvObservations = 21;
inline constexpr std::string_view kReplaySharpeConvention =
    "zero-risk-free;sample-standard-deviation;sqrt(252)-observations";
inline constexpr std::string_view kReplayLiquidityConvention =
    "abs-realized-dollar-trade/mean(raw_close*raw-volume);21-prior-completed-observations";
inline constexpr std::string_view kReplayCapacityStatus =
    "unavailable-without-chronological-impact-replay";

struct ReplayPerformance {
    atx::usize observed_intervals{};
    atx::f64 initial_nav{};
    atx::f64 final_nav{};
    atx::f64 gross_pnl_dollars{};
    atx::f64 net_pnl_dollars{};
    atx::f64 trade_cost_dollars{};
    atx::f64 borrow_cost_dollars{};
    atx::f64 abs_trade_dollars{};
    atx::f64 total_return{};
    atx::f64 max_drawdown{}; // Positive loss fraction, initial NAV plus interval-end samples.
    std::optional<atx::f64> sharpe_252; // Unavailable with fewer than 2 samples/zero variance.
};

enum class ReplayLiquidityStatus {
    Known,
    InsufficientHistory,
    MissingFields,
    InvalidWindow,
    NonpositiveAdv,
    UnrepresentableRatio
};

[[nodiscard]] std::string_view replay_liquidity_status_name(ReplayLiquidityStatus status) noexcept;

struct ReplayTradeParticipation {
    atx::usize trade_index{}; // Same order/index as ReplayResult::trades.
    std::optional<atx::f64> prior_dollar_adv;
    std::optional<atx::f64> participation; // Fraction, never clipped to 1 or winsorized.
    ReplayLiquidityStatus status{ReplayLiquidityStatus::MissingFields};
};

struct ReplayDiagnostics {
    ReplayPerformance full;
    std::optional<ReplayPerformance> post_fit;
    std::optional<atx::usize> first_post_fit_effective_observation;
    std::vector<ReplayTradeParticipation> trade_participation;
    atx::usize known_participation_count{};
    atx::usize unknown_participation_count{};
    // Overall maximum is unknown when any realized trade lacks valid liquidity.
    std::optional<atx::f64> max_participation;
    std::optional<atx::f64> max_known_participation;
};

// Inputs must already share verified research axes. Checks complete chronological
// replay shape and finite/accounting-consistent ledger values before aggregation.
// Sharpe uses observed interval net returns and sample standard deviation with an
// explicit sqrt(252) observation convention, not calendar-time annualization.
// Post-fit starts at the first EFFECTIVE interval with decision_period >= boundary,
// including its execution costs, excluding earlier carried decisions. This merely
// attributes an existing replay; it does not certify a valid holdout or refit.
// Trade/ADV uses raw_close and raw volume from the 21 strictly prior observations.
// A missing/invalid required cell makes that trade's liquidity unknown; no adjusted
// price fallback, final-book backcast, or capacity estimate is performed.
[[nodiscard]] atx::core::Result<ReplayDiagnostics>
diagnose_replay(const atx::engine::book::ReplayResult &replay,
                const atx::engine::alpha::Panel &research,
                std::optional<atx::usize> fit_boundary = std::nullopt);

} // namespace atx::impl
