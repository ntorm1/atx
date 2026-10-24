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
  // ADDITIVE (lane D): keep every template's resolution on every base state in
  // `HistoricalVarResult::resolved_legs` (which strike / expiry the relative template
  // became on that date). Off by default: the rows carry cold Greeks and are only
  // worth their memory when a caller reports them.
  bool keep_resolved_legs{false};
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
  // ADDITIVE (lane D): scenario-major (frames.size() * book.options.size()), filled iff
  // HistoricalVarConfig::keep_resolved_legs. Row (i, j) is template j resolved on
  // scenario i's base state; `leg.qty` is the template's signed qty; a template that
  // did not resolve carries its TheoStatus (SurfaceUnavailable when the base state has
  // no surface for its uid), NaN strike-level fields and fingerprint 0.
  std::vector<ResolvedTheo> resolved_legs;
};

// InvalidArgument on an empty book, a null state pointer, a confidence outside
// (0, 1), or an unprepared kernel policy. Scenario-level failures never fail the
// call: they are reported in `frames[i]` and excluded from the quantiles.
[[nodiscard]] Result<HistoricalVarResult>
historical_var(const HistoricalVarBook &book, std::span<const HistoricalVarScenario> scenarios,
               const HistoricalVarConfig &config = {});

// ADDITIVE (lane D): the quantile half of `historical_var` over already-evaluated
// frames — nearest-rank VaR / ES / tail attribution per confidence, n_admitted and
// reference_pv — with `frames` moved into the result. `historical_var` ends with
// exactly this call, so a caller that evaluates a long history in bounded windows
// (one `historical_var` per window, `confidences` empty) summarizes the concatenated
// frames once, and a verifier recomputes published points from stored frames, both
// bit-identical to one `historical_var` over the whole history. With no admitted
// scenario every point has NaN value_at_risk / expected_shortfall, tail_scenarios 0
// and a zero tail_mean, and reference_pv is NaN. InvalidArgument on a confidence
// outside (0, 1) or on an ADMITTED frame whose pnl_total or pv_base is not finite
// (historical_var never admits one); frames that are not admitted are never read.
[[nodiscard]] Result<HistoricalVarResult>
summarize_historical_var(std::vector<HistoricalVarFrame> frames,
                         std::span<const double> confidences);

} // namespace atx::vol
