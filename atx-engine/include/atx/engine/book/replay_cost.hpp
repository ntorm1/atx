#pragma once

// atx::engine::book -- per-name trade cost models for the scheduled replay.
//
// A ReplayCostModel prices ONE instrument's requested dollar trade at one
// execution period against that period's liquidity row, and may fill less than
// was requested (a participation cap). The replay carries the unfilled residual
// forward as a working order toward the same TRI-unit goal until it fills, a new
// decision replaces it, or the name loses its mark.
//
// FlatBpsCost is the per-dollar rate the replay has always charged through
// ReplayConfig::trade_bps. It advertises that rate through proportional_rate(),
// and the replay then keeps the legacy aggregate operation order
// (total traded dollars * rate), so its output is bit-identical to trade_bps.
//
// SqrtImpactCost is the square-root temporary-impact law used by
// exec::ExecutionSimulator (temp = Y * sigma * participation^delta) plus a
// half-spread crossing charge and an optional commission, per dollar traded:
//   cost = |x| * (half_spread_bps + commission_bps) * 1e-4
//        + |x| * Y * sigma_daily * (|x| / adv_dollars)^delta
// (Almgren et al. 2005; Toth et al. 2011; Frazzini-Israel-Moskowitz 2018).
// With a finite max_participation, |fill| <= max_participation * adv_dollars.
// A row with no usable ADV (nonfinite or nonpositive) fills nothing: an honest
// replay does not trade a name it has no liquidity estimate for.
//
// LIQUIDITY TIMING: the replay reads the liquidity row of the EXECUTION period.
// The caller must build that row from information available before the fill
// (for example a trailing ADV that ends at the previous session).

#include <optional>

#include "atx/core/error.hpp"
#include "atx/core/types.hpp"

namespace atx::engine::book {

struct LiquidityRow {
  atx::f64 adv_dollars{};     // Average daily dollar volume.
  atx::f64 daily_vol{};       // Daily return volatility (fraction, e.g. 0.02).
  atx::f64 half_spread_bps{}; // Half quoted spread in basis points.
};

// Result of pricing one requested trade. filled_dollars carries the request's
// sign with |filled_dollars| <= |requested|; cost_dollars is finite and >= 0.
struct TradeCost {
  atx::f64 filled_dollars{};
  atx::f64 cost_dollars{};
};

class ReplayCostModel {
public:
  ReplayCostModel() = default;
  ReplayCostModel(const ReplayCostModel &) = default;
  ReplayCostModel &operator=(const ReplayCostModel &) = default;
  ReplayCostModel(ReplayCostModel &&) = default;
  ReplayCostModel &operator=(ReplayCostModel &&) = default;
  virtual ~ReplayCostModel() = default;

  // Price a nonzero requested trade (positive buys). Must be deterministic and
  // free of side effects: the replay may run concurrently on other threads.
  [[nodiscard]] virtual TradeCost cost(atx::usize instrument, atx::usize period,
                                       atx::f64 trade_dollars,
                                       const LiquidityRow &liquidity) const noexcept = 0;

  // A pure proportional model with no cap returns its per-dollar rate; the
  // replay then charges rate * total traded dollars exactly as trade_bps does.
  [[nodiscard]] virtual std::optional<atx::f64> proportional_rate() const noexcept {
    return std::nullopt;
  }

  // False when cost() never reads its liquidity row, so none is required.
  [[nodiscard]] virtual bool needs_liquidity() const noexcept { return true; }
};

class FlatBpsCost final : public ReplayCostModel {
public:
  // Rejects a negative or nonfinite rate.
  [[nodiscard]] static atx::core::Result<FlatBpsCost> create(atx::f64 bps);

  [[nodiscard]] TradeCost cost(atx::usize instrument, atx::usize period,
                               atx::f64 trade_dollars,
                               const LiquidityRow &liquidity) const noexcept override;
  [[nodiscard]] std::optional<atx::f64> proportional_rate() const noexcept override {
    return rate_;
  }
  [[nodiscard]] bool needs_liquidity() const noexcept override { return false; }
  [[nodiscard]] atx::f64 bps() const noexcept { return bps_; }

private:
  explicit FlatBpsCost(atx::f64 bps) noexcept;
  atx::f64 bps_{};
  atx::f64 rate_{};
};

// Mirrors the temporary term of exec::ImpactCfg so fitness, optimizer and replay
// can share one calibration.
struct ReplayImpactCfg {
  atx::f64 y{1.0};     // Temporary-impact scale.
  atx::f64 delta{0.5}; // Participation exponent (square root by default).
};

class SqrtImpactCost final : public ReplayCostModel {
public:
  // y >= 0, delta in (0, 2], commission_bps >= 0, all finite; max_participation
  // is in (0, inf] (inf == uncapped).
  [[nodiscard]] static atx::core::Result<SqrtImpactCost>
  create(ReplayImpactCfg cfg, atx::f64 max_participation, atx::f64 commission_bps = 0.0);

  [[nodiscard]] TradeCost cost(atx::usize instrument, atx::usize period,
                               atx::f64 trade_dollars,
                               const LiquidityRow &liquidity) const noexcept override;

  // Per-dollar cost fraction of a fill of |dollars| on `liquidity` (no cap);
  // exposed so optimizers can reuse the same kappa_i. NaN on an unusable row.
  [[nodiscard]] atx::f64 cost_fraction(atx::f64 abs_dollars,
                                       const LiquidityRow &liquidity) const noexcept;

  [[nodiscard]] const ReplayImpactCfg &impact() const noexcept { return cfg_; }
  [[nodiscard]] atx::f64 max_participation() const noexcept { return max_participation_; }
  [[nodiscard]] atx::f64 commission_bps() const noexcept { return commission_bps_; }

private:
  SqrtImpactCost(ReplayImpactCfg cfg, atx::f64 max_participation,
                 atx::f64 commission_bps) noexcept;
  ReplayImpactCfg cfg_{};
  atx::f64 max_participation_{};
  atx::f64 commission_bps_{};
};

} // namespace atx::engine::book
