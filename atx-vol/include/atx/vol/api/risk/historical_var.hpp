#pragma once

// historical_var — historical-simulation VaR / ES of a reference book of RELATIVE
// option templates (fixed TTE with fixed delta or fixed moneyness) plus share legs,
// replayed across historical (base, shifted) market-state pairs on the risk engine.
//
// FROZEN API CONTRACT (sprint 2026-10-24; see risk_types.hpp banner). Replaces the
// src/analytics/var.* + historical_projection.* pipeline (deleted; no compatibility).
//
// Per scenario i: every template is resolved on the BASE state's surface (fixed
// TTE; fixed delta re-solved, or fixed k), the resolved legs plus the share legs
// form a RiskBook, both states are evaluated with the chosen kernel, and
// `explain(base -> shifted)` gives the scenario's P&L with its Greek split. The
// signed quantities are the reference book's, never re-sized. VaR / ES are the
// nearest-rank loss quantiles over scenarios whose explain had no exclusions (or
// whose excluded premium is below `max_excluded_premium_frac` of |pv_base|).

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "atx/vol/api/core/types.hpp"
#include "atx/vol/api/risk/market_state.hpp"
#include "atx/vol/api/risk/pnl_explain.hpp"
#include "atx/vol/api/risk/risk_book.hpp"
#include "atx/vol/api/risk/risk_kernel.hpp"
#include "atx/vol/api/risk/theo_contract.hpp"

namespace atx::vol {

struct TheoLeg {
  TheoContract contract{};
  double qty{0.0}; // signed contracts
  [[nodiscard]] bool operator==(const TheoLeg &) const = default;
};

struct HistoricalVarBook {
  std::vector<TheoLeg> options{};
  std::vector<ShareLeg> shares{};
};

struct HistoricalVarScenario {
  const MarketState *base{nullptr};
  const MarketState *shifted{nullptr};
};

struct HistoricalVarConfig {
  ProjectionPolicy projection{};
  RiskKernelKind kernel{RiskKernelKind::Family};
  FamilyKernelPolicy family{};
  ColdKernelPolicy cold{};
  RiskNeeds needs{};
  ExplainOptions explain{};
  std::vector<double> confidences{0.95, 0.99};
  double max_excluded_premium_frac{0.0}; // a scenario with more excluded premium is dropped
  unsigned n_threads{0};
};

struct HistoricalVarFrame {
  std::int64_t base_ts_ns{0};
  std::int64_t shifted_ts_ns{0};
  ExplainTotals totals{};        // the scenario's explain totals (pnl_total is THE P&L)
  std::uint32_t n_legs_resolved{0};
  std::uint32_t n_legs_unresolved{0}; // templates that failed to resolve on the base
  bool admitted{false};               // enters the quantiles
};

struct HistoricalVarPoint {
  double confidence{0.0};
  double value_at_risk{0.0};    // loss = -pnl_total at the nearest-rank quantile
  double expected_shortfall{0.0};
  std::size_t tail_scenarios{0};
  ExplainTotals tail_mean{};    // mean Greek split over the tail (attribution of the tail)
};

struct HistoricalVarResult {
  std::vector<HistoricalVarFrame> frames; // scenario order
  std::vector<HistoricalVarPoint> points; // one per confidence
  std::size_t n_admitted{0};
  double reference_pv{0.0};               // mean pv_base over admitted scenarios
};

// InvalidArgument on an empty book, a null state pointer, a confidence outside
// (0, 1), or an unprepared kernel policy. Scenario-level failures never fail the
// call: they are reported in `frames[i]` and excluded from the quantiles.
[[nodiscard]] Result<HistoricalVarResult>
historical_var(const HistoricalVarBook &book, std::span<const HistoricalVarScenario> scenarios,
               const HistoricalVarConfig &config = {});

} // namespace atx::vol
