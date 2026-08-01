#pragma once

// ┌──────────────────────────────────────────────────────────────────────────┐
// │ DEPRECATED — legacy VolSurface/Universe-bound portfolio engine.            │
// │ The CANONICAL portfolio path is `portfolio_pricer.hpp` (PricedSurface-     │
// │ native: contract dedup, American mark + American cold-FD Greeks, Taylor    │
// │ PnL-explain). Do NOT build new features here. This module is retained as   │
// │ reference only for capabilities not yet on the canonical path — stock/cash │
// │ legs, by-uid/by-expiry/by-group aggregation, and chain moneyness/strike    │
// │ bulk selection; migrate those onto PricedSurface when a consumer needs     │
// │ them rather than extending this VolSurface-bound code.                     │
// └──────────────────────────────────────────────────────────────────────────┘
//
// Portfolio pricing engine + bulk chain pricer.
//
// Ported from the C `ats-vol` library (ats_vol_portfolio.h /
// ats_vol_portfolio_price.c, ats_vol_bulk.h / ats_vol_bulk_pricer.c /
// ats_vol_bulk_select.c, ats_greeks_portfolio.c). This is the CORE pricing +
// first-order-risk path; the separate portfolio-RISK track (plan/resolve/
// explain/projection/theoretical-leg superset) is deliberately NOT ported —
// but the shared leg/book/aggregation vocabulary it will reuse is defined here.
//
// ## What this module does
//
//   1. `PortfolioLeg` / `AggMode` — the shared position model. An option leg
//      references an existing chain via a `ContractId`; stock legs price at
//      the underlier spot; cash legs are constant-value rows so a book total
//      stays self-contained.
//   2. `MarketBinding` — binds a book (or bulk request) to live market state:
//      the `Universe` supplies the chain/strike layout and spot; a per-uid
//      `UnderlyingMarket` supplies the fitted `VolSurface`, the `CurveSet`
//      (forward / discount per expiry), and optionally the American
//      correction cache per side.
//   3. `bulk_price` — batch-price a selected contract set (explicit list,
//      moneyness band, or strike range) against surfaces/curves, then reduce
//      per-lane Greeks into aggregate buckets. Scalar port of the C SoA
//      streaming pass (the AVX2 batch kernels are deferred).
//   4. `price_portfolio` — price a book leg-by-leg, aggregate dollar value
//      (and first-order Greeks) by `AggMode` into groups.
//   5. `aggregate_european_b76_greeks_raw_qty` — the
//      `ats_greeks_portfolio` flavour: raw-qty-weighted sum of the eight
//      analytic European Black-76 Greeks per bucket. `aggregate_greeks` is the
//      source-compatible legacy name.
//
// ## Numeric conventions (matched to the C)
//
//   - Option legs price European Black-76 from the surface IV, discounted by
//     df = exp(-rT); a bound per-side correction cache adds the American
//     early-exercise premium F * C(k_log, T, sigma).
//   - First-order option Greeks use the fused chain rule from the C batch
//     path: spot delta = exp((r-q)T) * dP/dF, with the American rho/theta
//     adjustments applied even on the no-correction route (the portfolio
//     treats listed equity options as American for Greek scaling; only the
//     early-exercise premium is gated on a bound correction cache).
//   - `price_portfolio` value / first-order aggregation is qty * multiplier
//     weighted (dollar convention, matches `AtsVolPortfolioAggregate`);
//     `aggregate_greeks` / bulk aggregation is raw-qty weighted (position
//     convention, matches `AtsVolAggregate`).
//
// Thread-safety: every entry is a pure function of its inputs — the bound
// `Universe` / `VolSurface` / `CurveSet` / `CorrectionCache` are read-only
// ("many readers OR one writer"), so concurrent calls against one binding are
// safe.

#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <utility>
#include <vector>

#include "atx/vol/adjusted_greeks.hpp"
#include "atx/vol/correction.hpp"
#include "atx/vol/greeks.hpp"
#include "atx/vol/rates_curve.hpp"
#include "atx/vol/types.hpp"
#include "atx/vol/universe.hpp"
#include "atx/vol/vol_surface.hpp"

namespace atx::vol {

// Quiet-NaN "no value yet" sentinel (matches the C NaN-as-unset convention).
inline constexpr double kPortNaN = std::numeric_limits<double>::quiet_NaN();

// "No exact surface slice matched" sentinel (mirrors the C 0xFFFF). Typed so
// comparisons stay uint16-vs-uint16 (no signed/unsigned mix under /W4).
inline constexpr std::uint16_t kNoSliceMatch = 0xFFFFu;

// ── Leg / book model (ports AtsVolPortfolioLeg) ──────────────────────────

// Three leg kinds. Option legs reference a chain contract; stock legs price at
// the underlier spot; cash legs carry a constant dollar value per unit qty.
enum class LegKind : std::uint8_t {
  Option = 0,
  Stock = 1,
  Cash = 2,
};

// One book position. All members value-initialized (aggregate type).
struct PortfolioLeg {
  LegKind kind{LegKind::Option};
  Uid uid{kInvalidUid};      // OPTION + STOCK; ignored for CASH
  ContractId contract_id{0}; // OPTION only
  double qty{0.0};           // signed: + long / - short
  double multiplier{100.0};  // OPTION deliverable (default 100); STOCK/CASH 1
  double cash_value{0.0};    // CASH only — dollar value per unit qty
  std::uint32_t group_id{0}; // opaque caller key; surfaced by ByGroupId agg
};

// ── Aggregation model (ports AtsVolPortfolioAggMode) ─────────────────────

// Numeric values line up with the C enum (TOTAL=0, BY_UID=1, BY_UID_EXPIRY=2,
// BY_GROUP_ID=3).
enum class AggMode : std::uint8_t {
  Total = 0,
  ByUid = 1,
  ByUidExpiry = 2,
  ByGroupId = 3,
};

// Group key for `leg` under `mode` (ports agg_key_for_leg):
//   Total       -> 0
//   ByUid       -> uid
//   ByUidExpiry -> (uid << 16) | (option ? cid_expiry : 0xFFFF)
//   ByGroupId   -> group_id
[[nodiscard]] std::uint64_t group_key_for_leg(const PortfolioLeg &leg, AggMode mode) noexcept;

// ── Pricing route + lane status (port of the C diagnostics) ──────────────

// `PricingRoute` (the actually-used route, reported per option lane) is defined
// in types.hpp — the shared vocabulary header — because profile.hpp consumes it
// as config while this header emits it as a diagnostic; a single definition
// keeps the two in agreement (and lets both be included together).

// Ports AtsVolBulkStatus. `route == 0xFF` marks stock/cash lanes.
enum class LaneStatus : std::uint8_t {
  Ok = 0,
  InvalidContract = 1,
  Filtered = 2,
  ModelUnavailable = 3,
  Unsupported = 4,
  NumericError = 5,
};

// ── Market binding ───────────────────────────────────────────────────────

// Per-uid market state the pricer reads. All pointers are non-owning observers
// whose lifetime must enclose the pricing call. `surface`/`curves` are required
// for option pricing; the correction caches are optional (present => American
// cache route for that side).
struct UnderlyingMarket {
  const VolSurface *surface{nullptr}; // IV source
  const CurveSet *curves{nullptr};    // F / df per expiry
  const CorrectionCache *correction_call{nullptr};
  const CorrectionCache *correction_put{nullptr};
};

// Binds a book / bulk request to live market state. The `Universe` supplies the
// chain/strike layout and per-uid spot; `set_market` registers the surface /
// curves / correction views per uid. Non-owning throughout (Rule of Zero).
class MarketBinding {
public:
  const Universe *universe{nullptr};

  // Register (or replace) the market view for `uid`.
  void set_market(Uid uid, const UnderlyingMarket &market);

  // Look up the market view for `uid`, or nullptr if none was registered.
  [[nodiscard]] const UnderlyingMarket *market_for(Uid uid) const noexcept;

private:
  std::vector<std::pair<Uid, UnderlyingMarket>> markets_;
};

// ── Portfolio pricing ────────────────────────────────────────────────────

enum class PortfolioRiskMode : std::uint8_t {
  PriceOnly = 0,
  FirstOrder = 1, // delta / vega / theta / rho on option legs
};

// Per-leg output (ports the AtsVolPortfolioOutput SoA, as an AoS row). For an
// option lane `price` is the per-share premium; for a stock lane it is
// spot * multiplier; for a cash lane it is cash_value * multiplier.
struct LegValue {
  double price{kPortNaN};
  double iv{kPortNaN};    // option legs only
  double delta{kPortNaN}; // option: spot delta; stock: multiplier; cash: 0
  double gamma{kPortNaN};
  double vega{kPortNaN};
  double theta{kPortNaN};
  double rho{kPortNaN};
  std::uint8_t route{0xFFu};
  LaneStatus status{LaneStatus::InvalidContract};
};

// One aggregation bucket (ports AtsVolPortfolioAggregate). `value` sums
// qty * multiplier * price across the bucket's legs; the Greeks sum
// qty * multiplier * per-leg Greek.
struct PortfolioAggregate {
  std::uint64_t group_key{0};
  double value{0.0};
  double delta{0.0};
  double vega{0.0};
  double theta{0.0};
  double rho{0.0};
  std::uint32_t n_legs{0};
};

struct PortfolioValuation {
  std::vector<LegValue> legs;                 // one per input leg, input order
  std::vector<PortfolioAggregate> aggregates; // one per distinct group key
};

// Price `book` under `risk_mode` and aggregate value (+ first-order Greeks in
// FirstOrder mode) by `agg_mode`. Per-leg failures are reported through the
// lane `status`; the call returns an error only on a structurally invalid
// request.
//
// @return InvalidArgument if `binding.universe` is null.
[[nodiscard]] Result<PortfolioValuation> price_portfolio(std::span<const PortfolioLeg> book,
                                                         const MarketBinding &binding,
                                                         PortfolioRiskMode risk_mode,
                                                         AggMode agg_mode);

// ── Portfolio Greeks (ports ats_greeks_portfolio) ────────────────────────

// One Greeks bucket. Keyed by `group_key`; `uid` / `expiry_id` mirror the C
// `AtsVolAggregate` diagnostic fields (uid == 0 for Total; expiry_id valid only
// for ByUidExpiry). `net_qty` sums raw signed qty; `greeks` sums qty-weighted
// analytic Black-76 Greeks (all eight).
struct GreeksAggregate {
  std::uint64_t group_key{0};
  Uid uid{kInvalidUid};
  ExpiryId expiry_id{kInvalidExpiry};
  Greeks greeks{};
  double net_qty{0.0};
};

// Raw-qty-weighted eight-Greek European Black-76 aggregation over the OPTION
// legs of `book`, bucketed by `agg_mode`. Stock/cash legs are skipped and
// `PortfolioLeg::multiplier` is deliberately ignored. Buckets are returned in
// first-seen input order; legs within each bucket accumulate in input order.
// Mirrors the C `ats_greeks_portfolio` convention.
// Expected complexity is O(book.size()) with O(bucket-count) output/index
// storage. The function does not mutate `book` or the bound market state, so
// allocation failure preserves all caller-owned state.
//
// `skew_adjusted_delta` (off by default => bit-identical to every pre-I6
// caller) applies SpiderRock's skew-adjusted delta (adjusted_greeks.hpp) to
// each option leg BEFORE the qty-weighted accumulate: delta + VegaSlope *
// vega, VegaSlope sourced from `surface_skew_slope` on the leg's resolved
// surface at (k_log, T) and blended by `sticky.ref_uprc_weight`. Black-76
// legs are FORWARD-quoted (`black76_greeks`'s delta = dP/dF, per
// greeks.hpp), and `detail::ExpiryContext` carries no separate spot -- so the
// VegaSlope's "S" is the leg's forward `ctx.F` (the natural sticky-delta
// slide variable for an F-quoted leg: k = ln(K/F) slides bodily with F, not
// with any spot this context does not have). A degenerate slope (e.g. off
// the surface's no-extrapolation domain) propagates a non-finite delta for
// that leg's contribution, exactly as `skew_adjusted` documents.
//
// @return InvalidArgument if `binding.universe` is null.
// @throws std::bad_alloc if result or index allocation fails.
[[nodiscard]] Result<std::vector<GreeksAggregate>>
aggregate_european_b76_greeks_raw_qty(std::span<const PortfolioLeg> book,
                                      const MarketBinding &binding, AggMode agg_mode,
                                      bool skew_adjusted_delta = false,
                                      const StickyParams &sticky = {});

// Source-compatible legacy name for
// `aggregate_european_b76_greeks_raw_qty`. New callers should use the explicit
// name so European-model and raw-quantity semantics are visible at the call
// site. The trailing `skew_adjusted_delta`/`sticky` parameters (off by default
// => bit-identical to every pre-I6 caller) forward straight through to the
// canonical entry above.
[[nodiscard]] Result<std::vector<GreeksAggregate>>
aggregate_greeks(std::span<const PortfolioLeg> book, const MarketBinding &binding,
                 AggMode agg_mode, bool skew_adjusted_delta = false,
                 const StickyParams &sticky = {});

// ── Bulk chain pricer + select (ports ats_vol_bulk_*) ────────────────────

enum class BulkSelectKind : std::uint8_t {
  ContractList = 0,       // price req.contract_ids as-is (may span uids)
  ChainMoneynessBand = 1, // strikes within F * (1 +/- moneyness_pct), all expiries
  ChainStrikeRange = 2,   // strikes in [strike_lo, strike_hi], all expiries
};

enum class BulkRiskMode : std::uint8_t {
  PriceOnly = 0,          // price through the route dispatcher only
  B76Greeks = 1,          // European B76 analytic Greeks; cache route adjusts price only
  AmericanFirstOrder = 2, // American first-order Greeks (spot-delta scaling)
};

// Bulk request. For chain selectors `uid` is required; for a contract list it
// may be `kInvalidUid` to allow a multi-underlying portfolio.
struct BulkRequest {
  BulkSelectKind select_kind{BulkSelectKind::ContractList};
  Uid uid{kInvalidUid};
  std::span<const ContractId> contract_ids; // ContractList input
  std::span<const std::int64_t> qty;        // optional agg weights (default +1)
  double moneyness_pct{0.0};
  double strike_lo{0.0};
  double strike_hi{0.0};
  BulkRiskMode risk_mode{BulkRiskMode::PriceOnly};
};

// Structure-of-arrays lane output. Every column is sized to the selected lane
// count; columns a risk mode does not compute stay NaN.
struct BulkOutput {
  std::vector<ContractId> contract_id;
  std::vector<double> price;
  std::vector<double> iv;
  std::vector<double> delta;
  std::vector<double> gamma;
  std::vector<double> vega;
  std::vector<double> theta;
  std::vector<double> rho;
  std::vector<double> vanna;
  std::vector<double> volga;
  std::vector<double> charm;
  std::vector<std::uint8_t> route;
  std::vector<LaneStatus> status;

  [[nodiscard]] std::size_t size() const noexcept { return contract_id.size(); }
};

struct BulkResult {
  BulkOutput out;
  std::vector<GreeksAggregate> aggregates;
};

// Select, price, and (for a valid `agg_mode`) reduce a bulk contract set.
//
// @return InvalidArgument on a structurally invalid request (null universe,
//         bad selector parameters, or a required uid missing);
//         NotImplemented for AggMode::ByGroupId (the bulk path has no per-leg
//         group_id — use price_portfolio for group aggregation).
[[nodiscard]] Result<BulkResult> bulk_price(const BulkRequest &req, const MarketBinding &binding,
                                            AggMode agg_mode);

// ── Shared expiry-context resolution (internal, used across the .cpp files) ─
namespace detail {

// Per-(uid, expiry) pricing context, derived once and reused across the lanes
// that share it. Ports PortExpiryCtx / AtsVolBulkExpiryCtx.
struct ExpiryContext {
  const Underlying *under{nullptr};
  const Chain *chain{nullptr};
  const VolSurface *surface{nullptr};
  const CorrectionCache *correction_call{nullptr};
  const CorrectionCache *correction_put{nullptr};
  double T{0.0};
  double F{0.0};
  double r{0.0};
  double q{0.0};
  double df{1.0};
  double sqrt_t{0.0};
  double log_f{0.0};
  std::uint16_t slice_idx{kNoSliceMatch}; // exact-slice index, 0xFFFF if none
};

// Resolve the pricing context for (uid, expiry_id) against the binding.
// @return NotFound if the uid/expiry is unknown; InvalidArgument if T or the
//         forward is degenerate.
[[nodiscard]] Result<ExpiryContext> resolve_expiry_context(const MarketBinding &binding, Uid uid,
                                                           ExpiryId expiry_id);

} // namespace detail

} // namespace atx::vol
