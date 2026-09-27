#pragma once

#include <limits>
#include <memory>
#include <span>
#include <string>
#include <string_view>

#include "atx/core/error.hpp"
#include "atx/core/types.hpp"

namespace atx::engine::cost {

enum class CostSurfaceRule : atx::u8 { SqrtOneWayV1 = 1, ModeledInputsV2 = 2 };
enum class CostInputState : atx::u8 { Available = 1, Unavailable = 2 };
enum class CostFillRule : atx::u8 { FullRequest = 1, ParticipationCapped = 2 };
enum class CostQuoteStatus : atx::u8 {
  Priced, Unavailable, InvalidRequest, WrongDecision, OutOfRange, NumericalOverflow
};

struct CostSurfaceRecipe {
  CostSurfaceRule rule{CostSurfaceRule::SqrtOneWayV1};
  atx::f64 impact_y{0.6};
  atx::f64 commission_bps{1.0};
  atx::f64 spread_scale{1.0};
  atx::f64 max_participation{std::numeric_limits<atx::f64>::infinity()};
};

struct CostSurfaceIdentity {
  atx::i64 decision_time_ns{};
  std::string source_sha256;
  // Pin raw price*raw volume ADV, return-volatility basis, trailing windows,
  // spread estimator/version, and modeled-versus-observed calibration origin.
  std::string liquidity_recipe;
  std::string calibration_identity;
};

struct CostSurfaceRow {
  atx::u64 instrument_id{}; // order is part of the snapshot identity; IDs are unique
  CostInputState state{CostInputState::Unavailable};
  atx::i64 available_at_ns{}; // strict < decision_time_ns for an available row
  atx::f64 adv_dollars{};
  atx::f64 daily_vol{}; // daily return fraction, not percent
  atx::f64 full_spread{}; // full spread fraction, not bps or half spread
  // Used only by explicit ModeledInputsV2; V1 ignores/canonicalizes these fields.
  atx::f64 impact_multiplier{1.0};
  CostInputState borrow_state{CostInputState::Unavailable};
  atx::i64 borrow_available_at_ns{};
  atx::f64 borrow_annual_fraction{}; // separate holding fee; NOT a locate assertion
};

struct CostBorrowRateQuote {
  CostQuoteStatus status{CostQuoteStatus::Unavailable};
  atx::f64 annual_fraction{std::numeric_limits<atx::f64>::quiet_NaN()};
};

struct CostQuote {
  CostQuoteStatus status{CostQuoteStatus::InvalidRequest};
  atx::f64 requested_dollars{};
  atx::f64 filled_dollars{}; // signed; a cap changes the quantity, not its cost rate
  atx::f64 spread_dollars{};
  atx::f64 commission_dollars{};
  atx::f64 impact_dollars{};
  atx::f64 total_dollars{};
  [[nodiscard]] bool priced() const noexcept { return status == CostQuoteStatus::Priced; }
};

struct CostWeightQuote {
  CostQuote dollars;
  atx::f64 cost_return{std::numeric_limits<atx::f64>::quiet_NaN()};
};

struct CostSurfaceCoefficients {
  CostQuoteStatus status{CostQuoteStatus::InvalidRequest};
  atx::f64 spread_linear{};
  atx::f64 commission_linear{};
  atx::f64 impact_three_halves{};
  atx::f64 max_trade_weight{}; // zero on any unavailable row
  [[nodiscard]] bool priced() const noexcept { return status == CostQuoteStatus::Priced; }
};

namespace cost_surface_detail { struct Data; }

// Immutable O(names) snapshot. Copies share owned const rows; no caller buffers
// need outlive creation. A quote must name this exact decision time. Refreshing
// a snapshot at a later decision is an explicit new, hash-bound construction.
// This core models costs, not observed fills, locate availability or tradeability.
class CostSurface {
public:
  [[nodiscard]] static atx::core::Result<CostSurface> create(
      const CostSurfaceRecipe& recipe, const CostSurfaceIdentity& identity,
      std::span<const CostSurfaceRow> rows,
      atx::u64 max_working_bytes = atx::u64{64} * 1024U * 1024U);

  // Retained payload capacities plus explicit 1 KiB allocator/control-block
  // slack; saturates on overflow. Shared copies each report the full payload.
  // This is an admission charge, not measured RSS.
  [[nodiscard]] atx::u64 bytes() const noexcept;
  [[nodiscard]] atx::usize instruments() const noexcept;
  [[nodiscard]] atx::i64 decision_time_ns() const noexcept;
  [[nodiscard]] std::span<const CostSurfaceRow> rows() const noexcept;
  [[nodiscard]] std::string_view recipe_sha256() const noexcept;
  [[nodiscard]] std::string_view snapshot_sha256() const noexcept;
  [[nodiscard]] std::string_view source_sha256() const noexcept;

  // One-way dollars: |q|*(spread_scale*full_spread/2 + commission_bps*1e-4)
  //                   + |q|*Y*daily_vol*sqrt(|q|/ADV_dollars).
  // Zero trades need no available liquidity, but geometry/time must still match.
  // Nonzero unpriceable requests never produce a successful zero-cost quote.
  [[nodiscard]] CostQuote quote_dollars(atx::usize instrument, atx::i64 decision_time_ns,
      atx::f64 signed_dollars, CostFillRule fill = CostFillRule::FullRequest) const noexcept;
  [[nodiscard]] CostWeightQuote quote_weight_change(atx::usize instrument,
      atx::i64 decision_time_ns, atx::f64 signed_weight_delta, atx::f64 pretrade_nav,
      CostFillRule fill = CostFillRule::FullRequest) const noexcept;
  [[nodiscard]] CostSurfaceCoefficients coefficients(atx::usize instrument,
      atx::i64 decision_time_ns, atx::f64 pretrade_nav) const noexcept;
  // V2 modeled annual holding rate; V1 has no borrow input and returns Unavailable.
  // The caller must apply its explicit elapsed/day-count convention separately.
  [[nodiscard]] CostBorrowRateQuote borrow_annual_rate(atx::usize instrument,
      atx::i64 decision_time_ns) const noexcept;

private:
  explicit CostSurface(std::shared_ptr<const cost_surface_detail::Data> data) noexcept;
  std::shared_ptr<const cost_surface_detail::Data> data_;
};

struct CostPathTrade {
  atx::usize snapshot{};
  atx::usize instrument{};
  atx::i64 decision_time_ns{};
  atx::f64 pretrade_nav{};
  atx::f64 signed_dollars{}; // actual marked holding delta supplied by the caller
};

struct CostPathSummary {
  atx::f64 total_dollars{};
  atx::f64 spread_return{};
  atx::f64 commission_return{};
  atx::f64 impact_return{};
  atx::f64 cost_return{}; // sum of per-trade NAV fractions, NOT compounded P&L
  atx::f64 one_way_turnover{}; // sum |executed dollars| / pretrade NAV
};

// Pure fitness adapter for a supplied chronological trade path. No backtest,
// target-weight turnover approximation, hidden fill, borrow or permanent term.
// No allocation on success; every supplied nonzero trade must be priceable.
[[nodiscard]] atx::core::Result<CostPathSummary> price_trade_path(
    std::span<const CostSurface> snapshots, std::span<const CostPathTrade> trades,
    CostFillRule fill = CostFillRule::FullRequest);

} // namespace atx::engine::cost
