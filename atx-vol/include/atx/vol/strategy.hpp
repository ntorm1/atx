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
//
// ## Thread-safety (plan 4.7)
//
// A `StrategySpec` is inert DATA and is concurrent-const-safe. An `IStrategy`
// IMPLEMENTATION is not: it carries mutable lifecycle state (cohort counters, the
// front cohort's expiry, halt/limit state) and per-step diagnostic buffers that
// `on_step` rewrites, so ONE strategy instance is driven by ONE engine loop on ONE
// thread. Every accessor here — `entry_risk_seeds`, `dropped_on_last_entry`,
// `risk_events`, `signals`, and the mutators `set_risk_limits` / `halt_from_step`
// — is meant for that thread, between steps. Running two independent strategy
// instances concurrently is fine; sharing one is not.
//
// The engine's own fan-out inside a step (leg pricing, dispersion book builds)
// goes through the PROCESS-GLOBAL pricing pool (detail/pricing_executor.hpp), so
// concurrent backtests share one core budget rather than each spawning a pool.
// That header's caller-facing rule holds here too: set the pool's topology with
// `configure_pricing_executor` before the first pricing call builds it.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
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
// `snap_to_sessions` snaps the synthetic expiry onto `StrategySpec::session_ts`
// instead: the raw `valuation + round(target_T * kNsPerYear)` anchor is pulled
// back onto the GREATEST session at or before it, and `T` is recomputed from the
// snapped anchor. A hold-to-expiry cohort then always lands on a timestamp the
// run actually observes (a raw anchor can fall on a weekend/holiday, which makes
// settlement unobservable). A raw anchor beyond the last session is left
// UNSNAPPED — that cohort out-lives the corpus and is liquidation-marked at run
// end. Requires a non-empty `StrategySpec::session_ts`; never a silent no-op.
struct TenorSpec {
  double target_T{30.0 / 365.25}; // e.g. 0.25 (3m), 0.75 (9m)
  bool snap_to_listed{false};     // true is rejected; never silently ignored
  bool snap_to_sessions{false};   // snap the synthetic expiry onto session_ts
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
    // A CYCLE fixes ONE expiry and holds it: the first session in
    // `StrategySpec::session_ts` at or after base_ts + tenor (ceil-snap — NOT
    // `TenorSpec::snap_to_sessions`' floor-snap; see `select_fixed_cycle_expiry`
    // — or the LAST session when the grid ends before the anchor: a short
    // final cycle the run still observes settling). Every entry tick inside
    // the cycle CLOSES the option lots and REOPENS them at freshly resolved
    // strikes at the SAME expiry (the engine books the diff as a roll-close
    // plus an entry, both at today's marks); the engine settles the cycle at
    // its expiry session and the next cycle is fixed on that same step.
    // `Lot::cohort` counts CYCLES, not clips.
    //
    // The cycle TENOR comes from the option legs: every leg must carry the
    // IDENTICAL finite positive `tenor.target_T` with both snap flags false —
    // the lifecycle owns expiry snapping in this mode, and a leg requesting
    // its own snap is InvalidArgument, never silently ignored. Requires a
    // non-empty sorted `session_ts`; `Entry` gives the RESTRIKE cadence
    // (EveryStep is the daily restrike; an off-tick step holds the book).
    //
    // KEEP-STRIKES POLICY: a step whose strike resolution fails soft (missing
    // board, unreachable delta, unpriceable wing) KEEPS the live lots and
    // counts `skipped_restrikes`; the same failure with an EMPTY book counts
    // `unopened_entry_steps` (nothing was held, and saying otherwise would put
    // a fictitious hold on the record). Never a fabricated strike, never a
    // 0.0 mark. Configuration errors stay fatal.
    //
    // This is the only holding mode that may carry `StrategySpec::swap_legs`:
    // a swap lot can never be erased (the engine's lane is held to expiry), so
    // only a lifecycle whose expiry the engine settles can open one.
    FixedExpiryRestrike = 3,
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

// ── The swap-leg lane (FixedExpiryRestrike only) ────────────────────────────

// HOW MUCH swap, resolved on the cycle-open step after the option legs are
// sized (MatchGroupVega needs their entry greeks).
struct SwapSizeSpec {
  enum class Kind : std::uint8_t {
    FixedQty = 0,   // qty = value (signed); still struck fair
    TargetVega = 1, // qty = sign * value / (swap entry vega)
    // qty = (option group's entry DOLLAR vega) / (swap entry vega): per-share
    // American vega x qty x multiplier summed over the group's freshly opened
    // lots — the very scaling the portfolio pricer applies — so the leg opens
    // EQUAL-VEGA against those options and the sign carries (long-vega options
    // size a long, variance-receiving swap).
    MatchGroupVega = 2,
  };
  Kind kind{Kind::MatchGroupVega};
  double value{0.0}; // FixedQty: the qty; TargetVega: the target |dollar vega|
  double sign{+1.0}; // TargetVega only
  std::string group; // MatchGroupVega: option-leg group to match; empty = ALL option legs
};

// ONE swap leg per CYCLE on one underlier: opened on the step that fixes the
// cycle (never on a restrike — the engine's swap lane is append-only and held
// to expiry, so a per-step reopen would pile up one stale leg per session),
// expiring at the cycle's own expiry, struck at its own fair strike through
// the same pricing bridge the engine's mark lane uses (the only construction
// that opens at genuine zero PV — see `solve_cycle_swap`, swap_leg.hpp).
//
// FAIL-SOFT: every entry-solve refusal (dark board, short cycle, failed fair
// strike, non-finite/zero vega or qty) runs that cycle without this leg and
// counts one `skipped_swap_cycles` — a one-legged cycle is reported, never a
// fabricated quantity, and a later session getting its board back does not
// retro-open a per-cycle instrument.
struct SwapLegSpec {
  std::string symbol;   // resolved to uid against the snapshot's SurfaceSet
  std::uint32_t uid{0}; // preferred if nonzero, else `symbol` lookup
  DerivKind kind{DerivKind::VarSwap};
  double cap_dec{0.0};         // > 0 required on a capped kind; must be 0 otherwise
  double notional{1.0};        // sizing rides on qty; notional stays a readable constant
  double annualization{252.0}; // trading-day variance convention
  SwapSizeSpec size{};
  // ENTRY SOLVE ONLY (fair strike + vega). The engine marks and settles a live
  // swap under its own hard-coded default DerivConfig (`swap_price_cfg`,
  // backtest.cpp), which no strategy can reach — so a non-default config here
  // changes what the swap is STRUCK at, never how it is subsequently valued.
  // Left at the default the two agree exactly, which is the only setting that
  // opens the swap at a genuine zero PV. The `swap_*` signal columns follow
  // the ENGINE's config, so they explain the marks the run is actually paid
  // on; a non-default value here therefore also breaks the equal-vega identity
  // between `options_vega` and `swap_vega` on a cycle-open row — the same one
  // divergence, seen in the signal columns.
  DerivConfig deriv_cfg{};
  std::string group; // diagnostic tag
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
  // The swap-leg lane. Empty (the default) is bit-identical to the pre-lane
  // grammar — no work, no allocation, no signal columns (the additive-lane
  // rule the engine's own swap lane follows). Non-empty requires
  // `lifecycle.holding == FixedExpiryRestrike` (NotImplemented otherwise: a
  // swap lot cannot be erased, so no other lifecycle can carry one).
  std::vector<SwapLegSpec> swap_legs;
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
  // The run clock's snapshot timestamps, SORTED ASCENDING. Consumed ONLY by legs
  // with `tenor.snap_to_sessions` (empty is fine for every other spec). The
  // builders are corpus-agnostic, so the CALLER fills this from its `Clock`
  // refs after assembling the spec.
  std::vector<std::int64_t> session_ts;
  // Treat "the scaled hedge group's symbol is not in this snapshot" as a NO-ENTRY
  // day instead of a run-ending error. Applies to the leg whose `group` equals
  // `constraint.group_b` — the index leg of a dispersion spec, the one leg the
  // structure cannot be built without — and ONLY to `NotFound`, i.e. that symbol
  // is absent from the snapshot. Every other failure on that leg (a degenerate
  // fit, a configuration error) still propagates, and basket-name handling under
  // `missing.policy` is untouched.
  //
  // READ `constraint.group_b` LITERALLY before setting this. It is "the index" for
  // `Kind::FlatVega`, which is what `make_dispersion_strangle_spec` builds. For
  // `Kind::VegaNeutralBasket` the group_b DEFAULT is "basket", so on such a spec
  // this flag would turn ONE missing basket name into a whole-entry skip — the
  // opposite of what `MissingNamePolicy::DropRenormalize` is for.
  //
  // Motivation: a real corpus loses its index board on a handful of sessions (an
  // arb-violating snapshot minute the fitter must reject), and a year-to-date run
  // has to drop those entry days rather than abort. `resolve_spec_with_policy`
  // then returns an EMPTY sized book with the skip recorded in `dropped`, which
  // `DeclarativeStrategy` already reads as "open nothing this step"; existing
  // cohorts are untouched. Default false preserves the pre-existing hard error,
  // and the flag is independent of `missing.policy`.
  bool skip_entry_on_missing_index{false};
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
// [-5,5] to catch extreme deltas; validates the terminal canonical delta evaluation.
// @return InvalidArgument if the target is outside (0,1) or unreachable.
[[nodiscard]] Result<double> resolve_strike_by_delta(const SurfaceRef &s, double T, Side side,
                                                     double target_abs_delta);

// Adaptive overload. When enabled, every successful return has been validated
// by `PricedSurface::delta(..., QueryExecution::ColdReference)` within
// `cold_delta_tolerance`; local refinement is bounded and exhaustion falls back
// to the robust solver running entirely ColdReference.
[[nodiscard]] Result<double> resolve_strike_by_delta(const SurfaceRef &s, double T, Side side,
                                                     double target_abs_delta,
                                                     const ResolutionOptions &options);

// Resolve a `StrikeSelector` to a synthetic-model absolute strike K (AtmForward =
// F(target_T); Delta = the solver; Moneyness = F * exp(value); AbsStrike = value).
// NotImplemented when `tenor.snap_to_listed` is true; this API has no listed
// contract or quote provenance and never pretends a model strike is tradeable.
[[nodiscard]] Result<double> resolve_strike(const SurfaceRef &s, const TenorSpec &tenor,
                                            Side side, const StrikeSelector &sel);
[[nodiscard]] Result<double> resolve_strike(const SurfaceRef &s, const TenorSpec &tenor,
                                            Side side, const StrikeSelector &sel,
                                            const ResolutionOptions &options);
[[nodiscard]] Result<double> resolve_strike(const SurfaceRef &s, const TenorSpec &tenor,
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
// `spec.missing` is ignored) — with ONE exception, and only when the caller has
// opted into it: `spec.skip_entry_on_missing_index` is honored here too (the
// resolution body is shared), so a NotFound on the `constraint.group_b` leg yields
// an EMPTY book rather than an error. This overload has no `dropped` sink, so on
// this path the skip is SILENT; use `resolve_spec_with_policy` if you need the
// reason. Default false leaves the "any leg failure is fatal" contract exact.
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
//
// Under EITHER policy, `spec.skip_entry_on_missing_index` (default false) turns a
// NotFound on the `constraint.group_b` leg into an EMPTY result — no legs, no
// error — with the skip recorded in `*dropped`. Callers that treat a non-error
// result as "there is a book" must check for empty; `DeclarativeStrategy` already
// does. Nothing else about either policy changes.
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
// FixedExpiryRestrike never reaches this helper — the interpreter's restrike
// path owns that mode's whole per-step decision.
[[nodiscard]] LifecycleDecision lifecycle_decide(const LifecycleSpec &lifecycle,
                                                 std::size_t step_index, bool book_empty,
                                                 std::int64_t base_ts, std::int64_t front_expiry,
                                                 bool have_front);

// The FixedExpiryRestrike cycle expiry for a step at `base_ts`: the FIRST
// session in `sessions` (sorted ascending) at or after `base_ts + tenor_ns`
// (overflow-guarded), or the LAST session when the grid ends before the anchor
// (a final, short cycle that still settles inside the run). 0 when no session
// strictly after `base_ts` remains — the grid is exhausted and no cycle can
// open. Ceil-snap by design, where `TenorSpec::snap_to_sessions` floor-snaps:
// a cycle wants AT LEAST its tenor, a synthetic leg wants an expiry the run
// has already observed.
[[nodiscard]] std::int64_t select_fixed_cycle_expiry(std::span<const std::int64_t> sessions,
                                                     std::int64_t base_ts,
                                                     std::int64_t tenor_ns) noexcept;

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
  //
  // BORROW, AND A SINGLE-STEP ONE. The span names a buffer the STRATEGY owns, not
  // the caller — and every shipped override rebuilds that buffer inside `on_step`
  // (clear, then move a fresh vector in), so the NEXT `on_step` invalidates it.
  // "The engine consumes this view immediately" is therefore a requirement, not a
  // description: read or copy the seeds before stepping again, and never store
  // the span across a step or past the strategy's own lifetime. Destroying the
  // strategy invalidates it as well. A strategy is stepped by ONE thread — it
  // carries mutable lifecycle state and `on_step` is its writer — so this
  // accessor is safe to read only from that thread, between steps.
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
  // WS-F F5 (BT-T2): the COMPLETE set of underlier uids this strategy will ever
  // touch, if it can be enumerated before the run. The engine uses it to build
  // its PRIVATE snapshot cache with a subset-deserialize, so a replay against a
  // wide archive reconstructs only the names the strategy references instead of
  // the whole board on every date.
  //
  // The default is EMPTY, which means "not known up front" and keeps the
  // whole-board load — the correct answer for any strategy that discovers names
  // inside `on_step` (a point-in-time universe, a signal-driven basket). A
  // schedule-driven strategy knows the answer exactly.
  //
  // CONTRACT — an INCOMPLETE set is a silent wrong number, not a slow run: a uid
  // omitted here is simply absent from every snapshot, so its lots go unpriced
  // and its hedge shares unmarked (both now fail closed under F1, but only under
  // the Error policy). Return everything or return nothing. The engine ignores
  // this entirely when the caller supplies its own snapshot cache, since a
  // shared cache may serve other books with different referenced sets.
  [[nodiscard]] virtual std::span<const std::uint32_t> referenced_uids() const noexcept {
    return {};
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
  // first entry. Same borrow rule as `entry_risk_seeds` above: the span names
  // this strategy's own buffer, the next `on_step` rewrites it (which can
  // reallocate), and destroying the strategy invalidates it. Copy the drops out
  // if a diagnostic sink keeps them beyond the current step.
  [[nodiscard]] std::span<const ResolveDrop> dropped_on_last_entry() const noexcept {
    return last_dropped_;
  }

  // ── FixedExpiryRestrike attribution counters ─────────────────────────────
  //
  // All three are CUMULATIVE across the run and 0 outside restrike mode. See
  // the keep-strikes policy on `LifecycleSpec::Holding::FixedExpiryRestrike`
  // for what distinguishes the first two, and `SwapLegSpec` for the third —
  // one counter, every "no swap on this cycle" cause, none of them silent.
  [[nodiscard]] std::uint64_t skipped_restrikes() const noexcept { return skipped_restrikes_; }
  [[nodiscard]] std::uint64_t unopened_entry_steps() const noexcept {
    return unopened_entry_steps_;
  }
  [[nodiscard]] std::uint64_t skipped_swap_cycles() const noexcept { return skipped_swap_cycles_; }

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

  // The FixedExpiryRestrike per-step body (cycle fix, restrike/keep-strikes,
  // the swap lane). Dispatched from `on_step` after one-time validation;
  // existing lifecycles never enter it.
  [[nodiscard]] Status step_restrike(const MarketSnapshot &base, std::size_t step_index,
                                     PortfolioState &book, std::uint64_t &next_lot_id,
                                     const PriceOptions &price_options);

  StrategySpec spec_;
  std::uint32_t cohort_counter_{0};
  std::int64_t front_expiry_{0};
  bool have_front_{false};
  std::vector<ResolveDrop> last_dropped_;
  std::vector<FullGreekSeed> last_entry_seeds_;
  // ── FixedExpiryRestrike state (inert in every other mode) ────────────────
  bool restrike_validated_{false};
  std::int64_t cycle_tenor_ns_{0};     // the legs' common tenor, set by validation
  std::int64_t cycle_expiry_ts_ns_{0}; // 0 = no live cycle
  std::int64_t last_step_ts_ns_{0};    // the snapshot the last completed step ran on
  // The option book's entry DOLLAR vega as of the last restrike — the number a
  // MatchGroupVega swap leg is sized against, cached for the signal row. NaN on
  // a step that kept strikes or held.
  double last_options_vega_{std::numeric_limits<double>::quiet_NaN()};
  std::uint64_t skipped_restrikes_{0};
  std::uint64_t unopened_entry_steps_{0};
  std::uint64_t skipped_swap_cycles_{0};
};

// ── X3: risk limits / capital / drawdown stop ────────────────────────────────
//
// Before this there was NO capital, gross exposure or drawdown control anywhere
// in the dispersion loop: the book sized to `gross_index_vega` unconditionally,
// every step, forever. These limits are checked on the ENTRY path before the
// sized book is committed, and every clamp or halt is recorded so it can never be
// silent.
//
// ALL DEFAULTS ARE UNLIMITED (0 == no limit), so a default config reproduces the
// pinned 82-session golden bit-for-bit. That golden is
// `final_nav = 24740.624124981368` since the E1 unit migration (2026-07-25):
// `target_vega` and these limits are denominated per VOL POINT, so the default
// book -- and every $-denominated figure below -- is 100x its pre-E1 value.

enum class RiskBreachAction : std::uint8_t {
  Clamp = 0, // scale the entry down to the binding limit and keep trading
  Halt = 1,  // open no further risk for the remainder of the run
};

enum class RiskBreachReason : std::uint8_t {
  None = 0,
  GrossVega = 1,
  GrossNotional = 2,
  Capital = 3,
  DrawdownStop = 4,
};

[[nodiscard]] std::string_view to_string(RiskBreachReason reason) noexcept;

struct DispersionRiskLimits {
  // Book GROSS vega cap, in DOLLARS PER VOL POINT — the same unit as
  // `DispersionConfig::target_vega`, so a vega-neutral book sized to
  // `target_vega` measures 2 × target_vega here (index leg + matched basket).
  // The gate converts per-share, per-unit-vol leg vegas with
  // `contract_vega_per_vol_point` (dispersion.hpp); it is NOT multiplier-
  // dependent. 0 => unlimited.
  double max_gross_vega{0.0};
  double max_gross_notional{0.0}; // gross premium notional cap; 0 => unlimited
  double capital{0.0};            // net premium outlay cap; 0 => unlimited
  // Fraction of CAPITAL (0.2 == halt after losing 20% of capital), so it requires
  // `capital` to be set. Deliberately not a fraction of peak NAV: the track's NAV
  // is cumulative P&L from an inception of zero, not an equity curve, which makes
  // a peak-relative ratio degenerate. 0 => no stop. This limit is NOT enforceable
  // inside on_step — the engine never shows a strategy its NAV — so it is enforced
  // at the run seam; see `halt_from_step`.
  double drawdown_stop{0.0};
  RiskBreachAction action{RiskBreachAction::Clamp};

  [[nodiscard]] bool any_sizing_limit() const noexcept {
    return max_gross_vega > 0.0 || max_gross_notional > 0.0 || capital > 0.0;
  }
  [[nodiscard]] bool any() const noexcept {
    return any_sizing_limit() || drawdown_stop > 0.0;
  }
};

// One recorded breach. `scale` is the factor actually applied to the entry
// (1.0 == untouched, 0.0 == the entry was suppressed entirely).
struct RiskEvent {
  std::size_t step_index{0};
  RiskBreachReason reason{RiskBreachReason::None};
  RiskBreachAction action{RiskBreachAction::Clamp};
  double limit{0.0};
  double requested{0.0};
  double scale{1.0};
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

  // X3. Install the pre-sizing risk gate. Default-constructed limits (all zero =
  // unlimited) leave on_step bit-identical to the ungated path.
  void set_risk_limits(DispersionRiskLimits limits) noexcept { limits_ = limits; }
  [[nodiscard]] const DispersionRiskLimits &risk_limits() const noexcept { return limits_; }

  // Suppress every entry from `step` onward. This is how the seam enforces a
  // drawdown stop: the engine never shows a strategy its NAV, so the seam runs
  // the track, finds the first breaching step, and replays with the halt armed.
  // NAV before the breach is unaffected by the halt, so one replay is exact.
  void halt_from_step(std::size_t step) noexcept { halt_from_step_ = step; }

  // Every clamp/halt applied, in step order. Empty when no limit was configured
  // or none bound — so a halt is never silent, and neither is a clamp.
  //
  // BORROW of this strategy's own append-only log. Unlike the per-step seeds
  // above it ACCUMULATES across the run, but it is appended to (`push_back`)
  // inside `on_step`, so any step that records an event can reallocate and
  // invalidate an outstanding span. Re-call the accessor after stepping rather
  // than holding one across steps; destroying the strategy invalidates it too.
  // Read it from the stepping thread, and copy the events out to keep them.
  [[nodiscard]] std::span<const RiskEvent> risk_events() const noexcept { return risk_events_; }

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
  // X3 risk gate. All-zero limits => the gate is inert and never allocates.
  DispersionRiskLimits limits_{};
  std::size_t halt_from_step_{std::numeric_limits<std::size_t>::max()};
  bool halted_{false};
  std::vector<RiskEvent> risk_events_;
  // Per-step telemetry mirrored into `signals()` (which does not see step_index).
  double last_risk_scale_{1.0};
  RiskBreachReason last_risk_reason_{RiskBreachReason::None};
};

} // namespace atx::vol
