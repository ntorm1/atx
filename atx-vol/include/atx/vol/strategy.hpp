#pragma once

// atx-vol strategy DSL (Phase B1) — declarative leg-template strategies resolved
// by the backtest engine into concrete `Lot`s, plus the `IStrategy` interface the
// engine loop drives and the `DeclarativeStrategy` interpreter over a `StrategySpec`.
//
// A strategy is DATA (`StrategySpec`): a set of leg templates on one or more
// underliers, an optional cross-leg sizing constraint, and a lifecycle (when to
// enter, how long to hold). The interpreter resolves a spec against a market
// snapshot — strike-from-delta / tenor / structure / sizing / cross-leg
// constraints — so trades like "3m 25Δ put, new clip each day" or "9m 40Δ XOM
// strangle vs 3m 40Δ SPY strangle, flat vega" are expressible with no bespoke code.
//
// B1 SCOPE: no frictions, no cash ledger, no hedge overlay (B2). Entries are
// model-on-model continuous contracts filled at the surface mark; PnL still flows
// through `pnl_explain`. `HedgeSpec` is executed by B2. Listed strikes, expiries,
// quotes, and fills belong to the existing `listed_opra.hpp` workflow (see the
// `spy_strangle_tradeable` example), not this declarative interpreter.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "atx/vol/backtest.hpp"         // MarketSnapshot, Lot, PortfolioState
#include "atx/vol/dispersion.hpp"       // DispersionUniverse, DispersionConfig, DispersionBook
#include "atx/vol/portfolio_pricer.hpp" // OptionContract, kNsPerYear
#include "atx/vol/priced_surface.hpp"   // PricedSurface
#include "atx/vol/types.hpp"            // Result, Status, Side

namespace atx::vol {

// ── The DSL grammar ─────────────────────────────────────────────────────────

// WHICH model tenor: a continuous target year-fraction on the interpolated
// surface. This creates a synthetic model contract, not a listed OPRA contract.
// `snap_to_listed=true` fails with NotImplemented; use the listed OPRA workflow
// in `listed_opra.hpp` when contract identity and tradeable quotes are required.
struct TenorSpec {
  double target_T{30.0 / 365.25}; // e.g. 0.25 (3m), 0.75 (9m)
  bool snap_to_listed{false};     // true is rejected; never silently ignored
};

// WHICH strike, per leg-side.
struct StrikeSelector {
  enum class Kind : std::uint8_t { AtmForward = 0, Delta = 1, Moneyness = 2, AbsStrike = 3 };
  Kind kind{Kind::AtmForward};
  double value{0.0}; // Delta: target |delta|; Moneyness: k=ln(K/F); AbsStrike: K
};

// WHAT structure. Each expands to 1..N option legs at resolved strikes.
struct StructureSpec {
  enum class Kind : std::uint8_t { Single = 0, Straddle = 1, Strangle = 2, RiskReversal = 3 };
  Kind kind{Kind::Straddle};
  Side single_side{Side::Call}; // Single only
  StrikeSelector call_leg{};    // Strangle/RR: OTM call selector (e.g. Delta 0.40)
  StrikeSelector put_leg{};     // Strangle/RR: OTM put selector (e.g. Delta 0.40)
};

// HOW MUCH. Resolved after strikes are known (needs the per-leg greeks).
//
// The Target* kinds size to a book GREEK: `qty = sign * target / (|Σ leg greek| *
// multiplier)`, so `value` is the target MAGNITUDE of that book greek and `sign`
// picks long/short. Using the absolute structure greek makes the target
// axis-agnostic (theta is < 0 for a long option, vega > 0), so a SHORT (sign = -1)
// strangle sized `TargetTheta 50` holds a book theta of +$50/day regardless of the
// surface. Re-resolved every entry, so under a daily restrike the unit count floats
// each day to hold the greek constant (constant-risk sizing).
//
// THETA UNITS: `value` for TargetTheta is a per-CALENDAR-DAY $ theta (the trader
// convention). The American greek theta is annualized (dP/dt, t in years), so the
// interpreter scales the target by 365.25 internally; the recorded book `gross_theta`
// remains annualized (== value * 365.25).
struct SizeSpec {
  enum class Kind : std::uint8_t {
    FixedContracts = 0,
    TargetVega = 1,
    Weight = 2,
    TargetTheta = 3, // size so |book theta| == value $ PER DAY (annualized greek / 365.25)
    TargetGamma = 4, // size so |book gamma| == value
  };
  Kind kind{Kind::TargetVega};
  double value{10'000.0}; // contracts, target |greek| ($ vega, $/day theta, or gamma), or weight
  double sign{+1.0};      // +1 long / -1 short the structure
};

// ONE leg-template on ONE underlier.
struct LegSpec {
  std::string symbol;   // resolved to uid against the snapshot's SurfaceSet
  std::uint32_t uid{0}; // preferred if nonzero, else `symbol` lookup
  TenorSpec tenor{};
  StructureSpec structure{};
  StrikeSelector strike{}; // Single/Straddle strike; Strangle uses structure.{call,put}_leg
  SizeSpec size{};
  std::string group; // cross-leg constraint group tag (e.g. "index"/"basket", "a"/"b")
};

// CROSS-LEG sizing constraint applied AFTER per-leg base sizing.
struct CrossLegConstraint {
  enum class Kind : std::uint8_t { None = 0, FlatVega = 1, VegaNeutralBasket = 2 };
  Kind kind{Kind::None};
  std::string group_a; // FlatVega: scale group_b so gross vega(b) == gross vega(a)
  std::string group_b; // VegaNeutralBasket: a defaults "index", b defaults "basket"
};

// LIFECYCLE: when to enter, how long to hold.
struct LifecycleSpec {
  enum class Entry : std::uint8_t { EveryStep = 0, EveryNDays = 1 };
  enum class Holding : std::uint8_t {
    HoldToExpiry = 0,
    RollAtHorizon = 1,
    // Overlapping cohorts (one per entry tick, like HoldToExpiry), but each lot
    // is closed by the strategy when its residual maturity falls below
    // roll_at_T: close when (lot.expiry_ts_ns - base_ts) < roll_at_T * kNsPerYear.
    // The engine books the close at current marks (roll-close diff), never
    // settlement. lifecycle_decide never returns clear=true in this mode.
    CloseAtHorizon = 2,
  };
  Entry entry{Entry::EveryNDays};
  Holding holding{Holding::RollAtHorizon};
  unsigned entry_every_n{21}; // EveryNDays cadence (trading steps)
  // HoldToExpiry => overlapping clips (a new cohort each entry, each aged to its
  // own expiry, auto-closed at T<=0). RollAtHorizon => single book, closed+reopened
  // when the front cohort's residual T falls below `roll_at_T`. CloseAtHorizon =>
  // overlapping clips like HoldToExpiry, but each cohort is independently closed
  // (by the strategy, at marks) once ITS OWN residual T falls below `roll_at_T`.
  double roll_at_T{7.0 / 365.25};
};

// HEDGE overlay (engine-owned, configurable). Declared for the full grammar; B1
// IGNORES it (B2 executes it).
struct HedgeSpec {
  enum class Kind : std::uint8_t { None = 0, DeltaToZero = 1 };
  enum class Cadence : std::uint8_t { AtEntry = 0, Daily = 1 };
  Kind kind{Kind::None};
  Cadence cadence{Cadence::Daily};
  double band{0.0}; // rebalance only when |net delta| > band (0 = every cadence tick)
};

// Optional strike-resolution accuracy policy. The default preserves the
// configured surface route exactly. Adaptive mode is intended for a surface
// prepared as RepresentativeFast or CarryBank: it uses that route only to
// propose a delta strike, then cold-validates/refines the proposal and falls
// back to the robust all-cold bracketed solver when necessary. Sizing Greeks
// and entry marks are also forced cold in this mode.
struct ResolutionOptions {
  bool fast_screen_cold_confirm{false};
  double cold_delta_tolerance{1.0e-5};
  double max_log_strike_step{0.05};
  unsigned max_refine_iterations{8};
};

struct StrategySpec {
  std::string name;
  std::vector<LegSpec> legs;
  CrossLegConstraint constraint{};
  LifecycleSpec lifecycle{};
  HedgeSpec hedge{};
  // Missing-name policy for leg resolution (S1-3, extended to the declarative DSL).
  // Default {Error, min_names=2} preserves resolve_spec's pre-existing hard-fail
  // behavior exactly: `resolve_spec` ignores this field entirely (always Error).
  MissingNameSpec missing{};
  // Appended after the original aggregate fields so existing positional
  // StrategySpec initializers keep their pre-adaptive meaning.
  ResolutionOptions resolution{};
};

// ── Resolution primitives ───────────────────────────────────────────────────

// A synthetic model option leg produced by expanding a `LegSpec` against a
// snapshot, before sizing. It has continuous (K,T) surface coordinates, not a
// listed contract identity or executable quote. `vega` is the per-share American
// greek vega (> 0 for both call and put); `sigma` is the surface IV at (K, T),
// and `model_price` is carried from that same final Greeks query for entry reuse.
struct ResolvedLeg {
  std::uint32_t uid{0};
  double K{0.0};
  double T{0.0};
  double sigma{0.0};
  double vega{0.0};  // per-share American vega (> 0 for both call and put)
  double theta{0.0}; // per-share American theta (dP/dt, calendar; < 0 for a long option)
  double gamma{0.0}; // per-share American gamma (> 0 for a long option)
  Side side{Side::Call};
  std::string group;
  // Appended after the original aggregate fields for source compatibility.
  double model_price{-1.0}; // nonnegative mark returned with the final sizing Greeks
  // Exact integer anchor chosen before any strike or risk query. `T` is the
  // year fraction re-derived from this anchor and the snapshot timestamp, so
  // the immediate engine portfolio has the identical raw contract key.
  std::int64_t expiry_ts_ns{0};
  // Genuine PricedSurface-produced full-risk result for the exact resolved
  // contract and route. Empty only for legacy/default-constructed aggregate use.
  std::optional<FullGreekSeed> full_greek_seed{};
};

// A sized option leg: a `ResolvedLeg` with a signed contract `qty` and contract
// `multiplier`, ready to become a `Lot` / `Position`.
struct SizedLeg {
  ResolvedLeg leg;
  double qty{0.0};
  double multiplier{100.0};
};

// Root-find the strike whose American |delta| equals `target_abs_delta`, on a
// log-moneyness bracket around F(T). |delta|(K) is monotone per side (call |delta|
// falls in K, put |delta| rises in K), so bisection converges deterministically
// (fixed bracket, fixed iteration cap). Widens the bracket [-1.5,1.5] -> [-3,3] ->
// [-5,5] to catch extreme deltas; validates the repriced delta at the root.
// @return InvalidArgument if the target is outside (0,1) or unreachable.
[[nodiscard]] Result<double> resolve_strike_by_delta(const PricedSurface &s, double T, Side side,
                                                     double target_abs_delta);

// Adaptive overload. When enabled, every successful return has been validated
// by `PricedSurface::delta(..., QueryExecution::ColdReference)` within
// `cold_delta_tolerance`; local refinement is bounded and exhaustion falls back
// to the robust solver running entirely ColdReference.
[[nodiscard]] Result<double> resolve_strike_by_delta(const PricedSurface &s, double T, Side side,
                                                     double target_abs_delta,
                                                     const ResolutionOptions &options);

// ── WS-P P4: batched N-name strike-from-delta resolve ──────────────────────
// One 40Δ (or any target) strike to resolve per lane, each on its own surface —
// the strategy-entry hot path for an N-name basket (a dispersion strangle resolves
// call+put per name = 2N lanes). Instead of the per-name serial iterative solve
// (bottleneck #4), the whole basket is resolved in ONE batched call: the lanes fan
// out over the pricing executor (disjoint per-lane writes → bit-identical to the
// serial `resolve_strike_by_delta` regardless of worker count) so an N-name entry
// costs ~one lane's latency at ≥ (min(N, cores))× the serial wall.
//
// Each lane runs the identical Illinois/false-position solver `resolve_strike_by_
// delta` uses (same `PricedSurface::delta` canonical evaluations, same
// QueryExecution::Configured, same tolerance), so out[i] is bit-identical to
// `resolve_strike_by_delta(*lanes[i].surface, lanes[i].T, lanes[i].side,
// lanes[i].target_abs_delta)`. A null surface or a lane that fails to bracket
// yields InvalidArgument in that slot without failing its neighbours.
//
// A single-thread SoA delta-wave (american_greeks_batch) is a further lever but
// cannot be bit-identical to the correction-cache `PricedSurface::delta` path, so it
// is deferred to its own economic-parity gate (see report; SIMD carry-forward).
struct DeltaResolveLane {
  const PricedSurface *surface{nullptr};
  double T{0.0};
  Side side{Side::Call};
  double target_abs_delta{0.0};
};

// @param n_threads worker fan-out (0 = full pool; clamped to lane count). Output is
//        bit-identical for any value.
[[nodiscard]] std::vector<Result<double>>
resolve_strikes_by_delta_batched(std::span<const DeltaResolveLane> lanes, unsigned n_threads = 0);

// Resolve a `StrikeSelector` to a synthetic-model absolute strike K (AtmForward =
// F(target_T); Delta = the solver; Moneyness = F * exp(value); AbsStrike = value).
// NotImplemented when `tenor.snap_to_listed` is true; this API has no listed
// contract or quote provenance and never pretends a model strike is tradeable.
[[nodiscard]] Result<double> resolve_strike(const PricedSurface &s, const TenorSpec &tenor,
                                            Side side, const StrikeSelector &sel);
[[nodiscard]] Result<double> resolve_strike(const PricedSurface &s, const TenorSpec &tenor,
                                            Side side, const StrikeSelector &sel,
                                            const ResolutionOptions &options);
[[nodiscard]] Result<double> resolve_strike(const PricedSurface &s, const TenorSpec &tenor,
                                            Side side, const StrikeSelector &sel,
                                            const ResolutionOptions &options,
                                            const PriceOptions &price_options);

// Expand a `LegSpec` against a snapshot into concrete (uid,K,T,side) legs with
// per-share vega + sigma, before sizing. Single -> 1, Straddle/Strangle -> 2,
// RiskReversal -> InvalidArgument (not in B1).
[[nodiscard]] Result<std::vector<ResolvedLeg>> expand_leg(const MarketSnapshot &snap,
                                                          const LegSpec &leg);
[[nodiscard]] Result<std::vector<ResolvedLeg>>
expand_leg(const MarketSnapshot &snap, const LegSpec &leg, const ResolutionOptions &options);
[[nodiscard]] Result<std::vector<ResolvedLeg>> expand_leg(const MarketSnapshot &snap,
                                                          const LegSpec &leg,
                                                          const ResolutionOptions &options,
                                                          const PriceOptions &price_options);

// Resolve a whole `StrategySpec` into a sized book: expand every leg, apply
// per-leg base sizing (FixedContracts/TargetVega/Weight, multiplier 100), then the
// `CrossLegConstraint` (FlatVega / VegaNeutralBasket scale one group's gross vega
// onto another's). Deterministic; emits legs in spec order (call before put).
// EXACTLY resolve_spec_with_policy under policy Error (any leg failure is fatal,
// `spec.missing` is ignored).
[[nodiscard]] Result<std::vector<SizedLeg>> resolve_spec(const MarketSnapshot &snap,
                                                         const StrategySpec &spec);
[[nodiscard]] Result<std::vector<SizedLeg>> resolve_spec(const MarketSnapshot &snap,
                                                         const StrategySpec &spec,
                                                         const PriceOptions &price_options);

// One leg dropped by `resolve_spec_with_policy` under `DropRenormalize`: the
// leg's symbol and the underlying resolve/sizing error, verbatim (a drop is
// never silent).
struct ResolveDrop {
  std::string symbol;
  std::string detail;
};

// Policy-aware `resolve_spec`. Under `spec.missing.policy == Error` this is
// EXACTLY `resolve_spec` (identical errors; `dropped` untouched by policy — see
// below). Under `DropRenormalize`:
//  - a leg whose expansion or sizing fails with NotFound/Unavailable is DROPPED
//    and recorded in `*dropped` (symbol + error detail), UNLESS the leg's group
//    equals `spec.constraint.group_b`
//    (the scaled hedge group) — a missing hedge leg makes the whole entry
//    unbuildable: returns Err(Unavailable, ...). Configuration/capability errors
//    such as InvalidArgument always propagate.
//  - if the count of surviving legs whose group == `spec.constraint.group_a`
//    (all legs when `constraint.kind == None`) is < `spec.missing.min_names`,
//    returns Err(Unavailable, ...).
//  - sizing + the cross-leg constraint then run on the survivors only, so
//    FlatVega's scale = gross_a/gross_b is computed from surviving legs' actual
//    vegas and the hedge renormalizes automatically.
// `dropped`, if non-null, is cleared then populated on every call (even one that
// ultimately errors out, e.g. via the hedge-leg or min_names guard above).
[[nodiscard]] Result<std::vector<SizedLeg>>
resolve_spec_with_policy(const MarketSnapshot &snap, const StrategySpec &spec,
                         std::vector<ResolveDrop> *dropped = nullptr);
[[nodiscard]] Result<std::vector<SizedLeg>>
resolve_spec_with_policy(const MarketSnapshot &snap, const StrategySpec &spec,
                         const PriceOptions &price_options,
                         std::vector<ResolveDrop> *dropped = nullptr);

// ── Lifecycle helper (shared by strategies) ─────────────────────────────────

// The per-step lifecycle decision. `open` => open a fresh cohort this step;
// `clear` => the existing (single) cohort must be erased first (a roll).
struct LifecycleDecision {
  bool open{false};
  bool clear{false};
};

// Decide, for `lifecycle` at `step_index`, whether to (re)open a cohort. For
// HoldToExpiry: open on every entry tick (EveryStep / EveryNDays), never clear
// (overlapping cohorts). For RollAtHorizon: a single cohort — open when the book
// is empty OR the front cohort's residual T = (front_expiry - base_ts)/year has
// fallen below `roll_at_T`; clear the prior cohort when rolling a non-empty book.
[[nodiscard]] LifecycleDecision lifecycle_decide(const LifecycleSpec &lifecycle,
                                                 std::size_t step_index, bool book_empty,
                                                 std::int64_t base_ts, std::int64_t front_expiry,
                                                 bool have_front);

// ── Strategy interface + declarative interpreter ────────────────────────────

// The engine-driven strategy interface. `on_step` is called at inception
// (step_index == 0) and after each move-swap, on the NEW base snapshot. It may
// append lots (entries / roll-reopen) and erase lots (roll-close) on `book`, using
// `next_lot_id` (monotonic) for new ids. It must NOT settle expiries — the engine
// settles at intrinsic and drops expired lots itself.
class IStrategy {
public:
  virtual ~IStrategy() = default;
  virtual Status on_step(const MarketSnapshot &base, std::size_t step_index, PortfolioState &book,
                         std::uint64_t &next_lot_id) = 0;
  // Pricing-aware entry point used by the backtest engine. The default forwards
  // to the original virtual so ordinary strategy overrides remain source
  // compatible; overloaded member-function pointers require explicit
  // disambiguation. Adding this public virtual changes the vtable ABI, so binary
  // consumers must rebuild. Implementations that price or resolve new positions
  // may override it to share the engine's economics and Greek route.
  virtual Status on_step(const MarketSnapshot &base, std::size_t step_index, PortfolioState &book,
                         std::uint64_t &next_lot_id, const PriceOptions & /*price_options*/) {
    return on_step(base, step_index, book, next_lot_id);
  }
  // Full-risk seeds produced for lots opened by the most recent on_step call.
  // The engine consumes this view immediately; the default keeps existing
  // strategies source-compatible and safely falls back to ordinary pricing.
  [[nodiscard]] virtual std::span<const FullGreekSeed> entry_risk_seeds() const noexcept {
    return {};
  }
  // Strategy diagnostics evaluated on the base snapshot (name -> value), recorded
  // per persisted row. Default: none.
  [[nodiscard]] virtual std::vector<std::pair<std::string, double>>
  signals(const MarketSnapshot & /*base*/) const {
    return {};
  }
  // The engine-owned delta-hedge overlay this strategy requests (B2). The engine
  // reads it each step and trades the shares ledger to satisfy it. Default: None
  // (no hedge), so a strategy that ignores it runs exactly as in B1.
  [[nodiscard]] virtual HedgeSpec hedge_spec() const { return {}; }
  // Economic route required by the strategy's decision policy. The engine
  // rejects a fast prepared tier when it cannot satisfy a ColdReference
  // requirement, preventing confirmed decisions from leaking into fast marks.
  [[nodiscard]] virtual QueryExecution required_economic_execution() const noexcept {
    return QueryExecution::Configured;
  }
};

// Interprets a `StrategySpec` against each snapshot. Holds the lifecycle state:
// a monotonic cohort counter and the front cohort's expiry (for RollAtHorizon).
class DeclarativeStrategy : public IStrategy {
public:
  explicit DeclarativeStrategy(StrategySpec spec) noexcept : spec_{std::move(spec)} {}

  Status on_step(const MarketSnapshot &base, std::size_t step_index, PortfolioState &book,
                 std::uint64_t &next_lot_id) override;
  Status on_step(const MarketSnapshot &base, std::size_t step_index, PortfolioState &book,
                 std::uint64_t &next_lot_id, const PriceOptions &price_options) override;
  [[nodiscard]] std::span<const FullGreekSeed> entry_risk_seeds() const noexcept override {
    return last_entry_seeds_;
  }

  [[nodiscard]] HedgeSpec hedge_spec() const override { return spec_.hedge; }
  [[nodiscard]] QueryExecution required_economic_execution() const noexcept override {
    return spec_.resolution.fast_screen_cold_confirm ? QueryExecution::ColdReference
                                                     : QueryExecution::Configured;
  }

  [[nodiscard]] const StrategySpec &spec() const noexcept { return spec_; }

  // The per-name drops (`ResolveDrop`) from the most recent entry attempt (an
  // `on_step` that ran `open_cohort`) under `spec.missing.policy ==
  // DropRenormalize` — the "document per-name failures" hook. Cleared then
  // repopulated at each entry attempt; empty under policy Error or before the
  // first entry.
  [[nodiscard]] std::span<const ResolveDrop> dropped_on_last_entry() const noexcept {
    return last_dropped_;
  }

private:
  struct PendingCohort {
    std::vector<Lot> lots;
    std::vector<FullGreekSeed> seeds;
    std::int64_t front_expiry{0};
  };

  // Resolve and fully validate a fresh cohort without changing externally
  // visible book/id/lifecycle state. `nullopt` is a policy-driven no-trade.
  [[nodiscard]] Result<std::optional<PendingCohort>>
  prepare_cohort(const MarketSnapshot &base, std::uint64_t first_lot_id,
                 const PriceOptions &price_options);

  StrategySpec spec_;
  std::uint32_t cohort_counter_{0};
  std::int64_t front_expiry_{0};
  bool have_front_{false};
  std::vector<ResolveDrop> last_dropped_;
  std::vector<FullGreekSeed> last_entry_seeds_;
};

// ── DispersionStrategy (adapter over build_dispersion_book) ──────────────────

// An IStrategy over a dispersion universe. On entry it calls the existing
// `build_dispersion_book` (authoritative P4-1 sizing — NOT reimplemented) and
// converts the emitted `Position`s into `Lot`s. Implied correlation and dropped
// name series are recorded only when `cfg.record_diagnostics` is explicitly
// enabled. Default lifecycle: RollAtHorizon.
//
// Missing-name handling rides in `cfg.missing` (S1-3). Under `DropRenormalize` a
// member absent/unusable on a date is dropped and the basket renormalized rather
// than aborting the run. NO-TRADE CONTRACT: if `build_dispersion_book` fails with
// `ErrorCode::Unavailable` (too few names survived that date) AND the policy is
// `DropRenormalize`, the step is treated as a flat / no-trade step — no lots are
// opened, the existing book is left untouched, and the run continues. Any other
// error code (InvalidArgument, NotFound — including an unresolved INDEX) stays
// fatal. Under the default `Error` policy nothing is dropped and behaviour is
// bit-identical to pre-S1-3.
class DispersionStrategy : public IStrategy {
public:
  // C1 POINT-IN-TIME UNIVERSE. `pit_resolver`, when set, is invoked at the top of
  // every `on_step` with the step snapshot's `ts_ns()` and must return the
  // constituent basket effective on THAT date; the strategy adopts it before the
  // build so a mid-backtest reconstitution (membership add/drop/reweight) is
  // honored at the next roll instead of freezing day-1 membership for the whole
  // run. `universe` is the seed/fallback used before the first successful resolve
  // and whenever a resolve fails (e.g. a date before the first effective block).
  // The DEFAULT (empty resolver) leaves the universe frozen — behaviour, and the
  // dispersion golden, are byte-identical to pre-C1. Typically built from a
  // schedule via `make_pit_universe_resolver` (dispersion_workflow.hpp).
  DispersionStrategy(DispersionUniverse universe, DispersionConfig cfg,
                     LifecycleSpec lifecycle = {}, HedgeSpec hedge = {},
                     std::function<Result<DispersionUniverse>(std::int64_t)> pit_resolver = {})
      : universe_{std::move(universe)}, cfg_{cfg}, lifecycle_{lifecycle}, hedge_{hedge},
        pit_resolver_{std::move(pit_resolver)} {}

  Status on_step(const MarketSnapshot &base, std::size_t step_index, PortfolioState &book,
                 std::uint64_t &next_lot_id) override;
  Status on_step(const MarketSnapshot &base, std::size_t step_index, PortfolioState &book,
                 std::uint64_t &next_lot_id, const PriceOptions &price_options) override;
  [[nodiscard]] std::span<const FullGreekSeed> entry_risk_seeds() const noexcept override {
    return last_entry_seeds_;
  }

  [[nodiscard]] std::vector<std::pair<std::string, double>>
  signals(const MarketSnapshot &base) const override;

  // Parity accessor: the exact P4-1 book this strategy would open on `base`.
  [[nodiscard]] Result<DispersionBook> build_book(const MarketSnapshot &base) const;

  // Diagnostic accessor: the names dropped on `base` under `cfg.missing` — both
  // resolve-stage drops (symbol absent from the snapshot) and IV-stage drops
  // (surface missing / ATM straddle unusable), in that order. Empty under Error.
  [[nodiscard]] std::vector<DroppedName> dropped_on(const MarketSnapshot &base) const;
  [[nodiscard]] HedgeSpec hedge_spec() const override { return hedge_; }

private:
  // `price_options == nullptr` preserves the documented legacy 4-arg/build_book
  // construction exactly. A non-null route is the engine seed-producing path.
  Status on_step_impl(const MarketSnapshot &base, std::size_t step_index, PortfolioState &book,
                      std::uint64_t &next_lot_id, const PriceOptions *price_options);

  DispersionUniverse universe_;
  DispersionConfig cfg_;
  LifecycleSpec lifecycle_;
  HedgeSpec hedge_{};
  // C1: point-in-time basket resolver keyed on the step snapshot's ts_ns. Empty =>
  // frozen universe (pre-C1 behaviour, bit-identical golden).
  std::function<Result<DispersionUniverse>(std::int64_t)> pit_resolver_{};
  std::uint32_t cohort_counter_{0};
  std::int64_t front_expiry_{0};
  bool have_front_{false};
  std::vector<FullGreekSeed> last_entry_seeds_;
};

} // namespace atx::vol
