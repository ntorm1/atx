#pragma once

// Theoretical portfolio-RISK engine: scenario/shock revaluation plus the
// plan / resolve / price / PnL-explain lifecycle for theoretical option legs.
//
// Ported from the C `ats-vol` library:
//   - scenario/shock engine: ats_greeks_scenario (ats_greeks_portfolio.c)
//   - plan / bind / price:    ats_vol_portfolio_risk.c (Sprint 20 Stage II)
//   - resolver pipeline:      ats_vol_portfolio_resolver.c
//   - PnL explain:            ats_vol_portfolio_explain.c
//
// This module builds ON TOP of the already-ported pieces and does NOT re-port
// them:
//   - the leg/book/binding vocabulary + first-order pricing/aggregation lives
//     in portfolio.hpp (`PortfolioLeg`, `MarketBinding`, `UnderlyingMarket`,
//     `AggMode`, `PortfolioRiskMode`, `PortfolioAggregate`, `LaneStatus`,
//     `price_portfolio`, `aggregate_greeks`, `detail::resolve_expiry_context`);
//   - the eight-Greek qty-weighted risk aggregation is `aggregate_greeks`;
//   - the projection spine (inserted-slice IV, forward_T, delta solve, eval)
//     lives in projection.hpp.
//
// ## What this module adds
//
//   1. `scenario_pnl` — revalue a book under a chain of shocks (spot / vol /
//      rate / time), reporting the theoretical PnL vs the base. Faithful port
//      of `ats_greeks_scenario`.
//   2. Theoretical-leg engine — legs specified in (T_clock, coordinate) space
//      rather than by listed contract id. `PricingPlan` groups legs by
//      (uid, side, route, interp, extrap, time, quantized-T), binds each group
//      to market state (inserted constant-maturity slice + resolved route),
//      prices per lane (B76 + optional American-cache overlay + first-order
//      Greeks), and aggregates dollar value / Greeks by `AggMode`.
//   3. `PricingPlan::project_compare` — reprice the same plan against two
//      market snapshots and decompose each leg's price change into
//      forward / vol / route / interp contributions (with an optional
//      vol-attribution sub-split into level / skew / curvature / higher-order).
//
// ## Port adaptations (see PORT NOTEs in the .cpp)
//
//   - The C Stage II binding (`AtsVolMarketBinding` with universe + per-uid
//     overrides) is replaced by the portfolio-core `MarketBinding` model:
//     each theoretical group resolves its (surface, curves, correction) from
//     `binding.market_for(uid)` (the same per-uid `UnderlyingMarket` the
//     pricing engine already uses). The per-side correction comes from
//     `UnderlyingMarket::correction_call` / `correction_put`.
//   - Native (listed-contract) legs are NOT handled here — they go through the
//     already-ported `price_portfolio`. This engine is theoretical-leg-only,
//     matching the Sprint 20 Stage II test surface.
//   - The parallel fan-out entries and the AVX2 inserted-slice batch kernel
//     are deferred (scalar only).
//
// Thread-safety: `scenario_pnl` and `PricingPlan::price` are pure reads of
// their (read-only) market inputs once bound; `bind_market` /
// `project_compare` mutate the plan and require exclusive access.

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "atx/vol/portfolio.hpp"   // PortfolioLeg, MarketBinding, AggMode, ...
#include "atx/vol/projection.hpp"  // TimeModel, CoordKind, InsertedSliceHandle, ...

namespace atx::vol {

// ── Scenario / shock engine (ports ats_greeks_scenario) ──────────────────

// Shock kinds. Repeated shocks of the same kind compose additively.
enum class ShockKind : std::uint8_t {
  SpotPct = 0,       // S' = S * (1 + amount)
  VolAbs = 1,        // sigma' = sigma + amount
  VolRel = 2,        // sigma' = sigma * (1 + amount)
  RateAbs = 3,       // r' = r + amount
  TimeAbs = 4,       // T' = T - amount   (theta-roll)
  SurfaceTwist = 5,  // reserved — skew twist (NotImplemented)
};

struct Shock {
  ShockKind kind{ShockKind::SpotPct};
  double amount{0.0};
};

// Revalue `book`'s OPTION legs under a chain of shocks and return the net
// theoretical PnL (sum of qty * (shocked_price - base_price)). Stock/cash legs
// are ignored (matching the C `ats_greeks_scenario`, which prices positions).
// Market state is never mutated. Per-position resolution failures are skipped.
//
// @return InvalidArgument if `binding.universe` is null; NotImplemented if any
//         shock is a reserved SurfaceTwist.
[[nodiscard]] Result<double> scenario_pnl(std::span<const PortfolioLeg> book,
                                          const MarketBinding& binding,
                                          std::span<const Shock> shocks);

// ── Theoretical-leg model ────────────────────────────────────────────────

// One theoretical option leg: an option specified in (T_clock, coordinate)
// space rather than by a listed contract id. `input_ix` preserves the caller's
// order in the output vectors. `multiplier <= 0` / non-finite defaults to 100.
struct TheoreticalLeg {
  std::uint32_t input_ix{0u};
  Uid uid{kInvalidUid};
  Side side{Side::Call};
  double T_clock{0.0};
  double x{0.0};  // coordinate value; see coord_kind
  CoordKind coord_kind{CoordKind::Strike};
  InterpMode interp_mode{InterpMode::PiecewiseTotalVariance};
  ProjExtrapPolicy extrap_policy{ProjExtrapPolicy::Forbid};
  DeltaConvention delta_convention{DeltaConvention::Forward};
  TimeMode time_mode{TimeMode::Clock};
  RoutePolicy route_policy{RoutePolicy::B76Only};
  double qty{0.0};
  double multiplier{100.0};
  std::uint32_t group_id{0u};
};

// ── Per-leg + aggregate output ───────────────────────────────────────────

// One theoretical leg's priced output (AoS row; the C SoA columns). `route` is
// the actually-used route; `resolver_flags` carries the provenance bits.
struct TheoLegValue {
  double price{kQuietNaN};
  double iv{kQuietNaN};
  double delta{kQuietNaN};  // spot delta (FirstOrder only)
  double gamma{kQuietNaN};  // spot gamma (FirstOrder only)
  double vega{kQuietNaN};
  double theta{kQuietNaN};
  double rho{kQuietNaN};
  double T_clock{kQuietNaN};
  double tau_vol{kQuietNaN};
  double quote_delta{kQuietNaN};
  RoutePolicy route{RoutePolicy::B76Only};
  std::uint32_t resolver_flags{0u};
  LaneStatus status{LaneStatus::InvalidContract};
};

// Full pricing output: one row per leg (indexed by input_ix), plus one
// aggregate bucket per distinct group key. Aggregates reuse the portfolio-core
// `PortfolioAggregate` (qty*multiplier-weighted dollar value + Greeks).
struct PricingResult {
  std::vector<TheoLegValue> legs;
  std::vector<PortfolioAggregate> aggregates;
};

// One leg's PnL-explain decomposition. The four base components sum to
// `d_price` (target - source); the four vol sub-components sum to `d_from_vol`.
struct ExplainRow {
  double d_price{0.0};             // pt - ps
  double d_from_forward{0.0};
  double d_from_vol{0.0};
  double d_from_route{0.0};
  double d_from_interp{0.0};
  double d_from_vol_level{0.0};
  double d_from_vol_skew{0.0};
  double d_from_vol_curvature{0.0};
  double d_from_vol_higher{0.0};
  std::uint32_t resolver_flags{0u};
};

// project_compare output: the source and target pricing passes plus the
// per-leg explain rows (indexed by input_ix).
struct ProjectCompareResult {
  PricingResult source;
  PricingResult target;
  std::vector<ExplainRow> explain;
};

// ── Pricing plan (ports AtsVolPricingPlan, theoretical path) ─────────────

// Groups theoretical legs, binds each group to market state (building one
// inserted constant-maturity slice per unique (uid, quantized-T, surface,
// interp)), and prices/reprices them. Rule of Zero (owns its state by value;
// non-owning market pointers are captured at bind).
class PricingPlan {
 public:
  // Build a plan from theoretical legs (grouping only; no market binding yet).
  [[nodiscard]] static Result<PricingPlan> create(
      std::span<const TheoreticalLeg> legs);

  // Bind every group to market state resolved from `binding` (per-uid
  // surface/curves/correction via `MarketBinding::market_for`) and `tm`. Safe
  // to call repeatedly across snapshots.
  //
  // @return InvalidArgument on a structurally invalid resolve; the call itself
  //         succeeds even when individual groups fail to resolve (those legs
  //         are marked ModelUnavailable at price time).
  [[nodiscard]] Status bind_market(const MarketBinding& binding,
                                   const TimeModel& tm);

  // Price the bound plan under `risk_mode`, aggregating by `agg_mode`.
  // @return InvalidArgument if the plan is not bound.
  [[nodiscard]] Result<PricingResult> price(PortfolioRiskMode risk_mode,
                                            AggMode agg_mode) const;

  // Reprice the plan against two snapshots and decompose each leg's price
  // change. Binds `source` then `target` internally (mutates the plan).
  [[nodiscard]] Result<ProjectCompareResult> project_compare(
      const MarketBinding& source, const MarketBinding& target,
      const TimeModel& tm);

  [[nodiscard]] std::size_t n_theoretical_legs() const noexcept {
    return legs_.size();
  }
  [[nodiscard]] std::size_t n_theoretical_groups() const noexcept {
    return groups_.size();
  }
  [[nodiscard]] std::size_t n_inserted_slice_ctxs() const noexcept {
    return ctxs_.size();
  }

 private:
  PricingPlan() = default;

  // One inserted constant-maturity slice context, keyed by
  // (uid, quantized-T, surface, interp) and reused across all lanes sharing it.
  struct Ctx {
    Uid uid{kInvalidUid};
    double T_clock_q{0.0};
    const VolSurface* surface{nullptr};
    InterpMode interp_mode{InterpMode::PiecewiseTotalVariance};
    InsertedSliceHandle handle{};
  };

  // One group of lanes sharing an inserted-slice context + resolved route.
  struct Group {
    Uid uid{kInvalidUid};
    Side side{Side::Call};
    RoutePolicy route_policy{RoutePolicy::B76Only};
    InterpMode interp_mode{InterpMode::PiecewiseTotalVariance};
    ProjExtrapPolicy extrap_policy{ProjExtrapPolicy::Forbid};
    TimeMode time_mode{TimeMode::Clock};
    double T_clock_q{0.0};
    std::uint32_t lane_start{0u};
    std::uint32_t lane_count{0u};
    // Bind-time resolved state.
    std::uint32_t ctx_ix{0xFFFFFFFFu};
    bool invalid{false};
    std::uint32_t flags{0u};
    const VolSurface* surface{nullptr};
    const CurveSet* curves{nullptr};
    const CorrectionCache* correction{nullptr};
    RoutePolicy resolved_route{RoutePolicy::B76Only};
  };

  // One lane = one theoretical leg, resolved to a (K, k_log) at bind time.
  struct Lane {
    std::uint32_t input_ix{0u};
    std::uint32_t group_ix{0u};
    Side side{Side::Call};
    bool invalid{false};
    double qty_x_mult{0.0};
    double K{kQuietNaN};
    double k_log{kQuietNaN};
    double quote_delta{kQuietNaN};
    double x_orig{0.0};
    CoordKind coord_kind{CoordKind::Strike};
    DeltaConvention delta_convention{DeltaConvention::Forward};
    std::uint32_t lane_flags{0u};
    std::uint32_t group_id{0u};
  };

  // Find or build the inserted-slice context for (uid, T_q, surface, interp).
  // Sets `out_flags` to the context provenance and `out_ok` to whether the
  // build/reuse succeeded; returns the context index (undefined if !out_ok).
  [[nodiscard]] std::uint32_t find_or_build_ctx(Uid uid, double t_q,
                                                const VolSurface* surface,
                                                const CurveSet* curves,
                                                InterpMode interp,
                                                ProjExtrapPolicy extrap,
                                                const TimeModel& tm,
                                                std::uint32_t& out_flags,
                                                bool& out_ok);

  // Resolve one group against `binding`: market lookup, inserted-slice context,
  // route resolution, and per-lane strike resolution.
  void resolve_group(Group& g, const MarketBinding& binding,
                     const TimeModel& tm);

  // Price one group's lanes into `legs_out`, folding aggregates into `aggs`.
  void price_group(const Group& g, PortfolioRiskMode risk_mode,
                   AggMode agg_mode, std::vector<TheoLegValue>& legs_out,
                   std::vector<PortfolioAggregate>& aggs) const;

  std::vector<TheoreticalLeg> legs_;
  std::vector<Lane> lanes_;
  std::vector<Group> groups_;
  std::vector<Ctx> ctxs_;
  TimeModel time_model_{};
  bool bound_{false};
};

}  // namespace atx::vol
