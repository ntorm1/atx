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
// B1 SCOPE: no frictions, no cash ledger, no hedge overlay (B2). Entries fill at
// mid (the surface mark); PnL still flows through `pnl_explain`. `HedgeSpec` and
// `TenorSpec::snap_to_listed` are declared but not executed here (reserved hooks).

#include <cstddef>
#include <cstdint>
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

// WHICH tenor: a target year-fraction, optionally snapped to a listed slice T.
// B1 always prices on the interpolated surface at `target_T` (snap_to_listed is a
// reserved hook, ignored here).
struct TenorSpec {
  double target_T{30.0 / 365.25}; // e.g. 0.25 (3m), 0.75 (9m)
  bool snap_to_listed{false};     // reserved (B1: false / ignored)
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
  enum class Holding : std::uint8_t { HoldToExpiry = 0, RollAtHorizon = 1 };
  Entry entry{Entry::EveryNDays};
  Holding holding{Holding::RollAtHorizon};
  unsigned entry_every_n{21}; // EveryNDays cadence (trading steps)
  // HoldToExpiry => overlapping clips (a new cohort each entry, each aged to its
  // own expiry, auto-closed at T<=0). RollAtHorizon => single book, closed+reopened
  // when the front cohort's residual T falls below `roll_at_T`.
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

struct StrategySpec {
  std::string name;
  std::vector<LegSpec> legs;
  CrossLegConstraint constraint{};
  LifecycleSpec lifecycle{};
  HedgeSpec hedge{};
};

// ── Resolution primitives ───────────────────────────────────────────────────

// A concrete option leg produced by expanding a `LegSpec` against a snapshot,
// before sizing. `vega` is the per-share American greek vega (> 0 for both call
// and put); `sigma` is the surface IV at (K, T).
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

// Resolve a `StrikeSelector` to an absolute strike K (AtmForward = F(target_T);
// Delta = the solver; Moneyness = F * exp(value); AbsStrike = value).
[[nodiscard]] Result<double> resolve_strike(const PricedSurface &s, const TenorSpec &tenor,
                                            Side side, const StrikeSelector &sel);

// Expand a `LegSpec` against a snapshot into concrete (uid,K,T,side) legs with
// per-share vega + sigma, before sizing. Single -> 1, Straddle/Strangle -> 2,
// RiskReversal -> InvalidArgument (not in B1).
[[nodiscard]] Result<std::vector<ResolvedLeg>> expand_leg(const MarketSnapshot &snap,
                                                          const LegSpec &leg);

// Resolve a whole `StrategySpec` into a sized book: expand every leg, apply
// per-leg base sizing (FixedContracts/TargetVega/Weight, multiplier 100), then the
// `CrossLegConstraint` (FlatVega / VegaNeutralBasket scale one group's gross vega
// onto another's). Deterministic; emits legs in spec order (call before put).
[[nodiscard]] Result<std::vector<SizedLeg>> resolve_spec(const MarketSnapshot &snap,
                                                         const StrategySpec &spec);

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
};

// Interprets a `StrategySpec` against each snapshot. Holds the lifecycle state:
// a monotonic cohort counter and the front cohort's expiry (for RollAtHorizon).
class DeclarativeStrategy : public IStrategy {
public:
  explicit DeclarativeStrategy(StrategySpec spec) noexcept : spec_{std::move(spec)} {}

  Status on_step(const MarketSnapshot &base, std::size_t step_index, PortfolioState &book,
                 std::uint64_t &next_lot_id) override;

  [[nodiscard]] HedgeSpec hedge_spec() const override { return spec_.hedge; }

  [[nodiscard]] const StrategySpec &spec() const noexcept { return spec_; }

private:
  // Resolve the spec against `base` and append a fresh cohort of lots.
  Status open_cohort(const MarketSnapshot &base, PortfolioState &book, std::uint64_t &next_lot_id);

  StrategySpec spec_;
  std::uint32_t cohort_counter_{0};
  std::int64_t front_expiry_{0};
  bool have_front_{false};
};

// ── DispersionStrategy (adapter over build_dispersion_book) ──────────────────

// An IStrategy over a dispersion universe. On entry it calls the existing
// `build_dispersion_book` (authoritative P4-1 sizing — NOT reimplemented) and
// converts the emitted `Position`s into `Lot`s; `signals` surfaces the
// implied-correlation diagnostic plus an `n_names_dropped` count. Default
// lifecycle: RollAtHorizon.
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
  DispersionStrategy(DispersionUniverse universe, DispersionConfig cfg,
                     LifecycleSpec lifecycle = {}, HedgeSpec hedge = {})
      : universe_{std::move(universe)}, cfg_{cfg}, lifecycle_{lifecycle}, hedge_{hedge} {}

  Status on_step(const MarketSnapshot &base, std::size_t step_index, PortfolioState &book,
                 std::uint64_t &next_lot_id) override;

  [[nodiscard]] std::vector<std::pair<std::string, double>>
  signals(const MarketSnapshot &base) const override;

  // Parity accessor: the exact P4-1 book this strategy would open on `base`.
  [[nodiscard]] Result<DispersionBook> build_book(const MarketSnapshot &base) const;

  // Diagnostic accessor: the names dropped on `base` under `cfg.missing` — both
  // resolve-stage drops (symbol absent from the snapshot) and signal-stage drops
  // (surface missing / ATM straddle unusable), in that order. Empty under Error.
  [[nodiscard]] std::vector<DroppedName> dropped_on(const MarketSnapshot &base) const;
  [[nodiscard]] HedgeSpec hedge_spec() const override { return hedge_; }

private:
  DispersionUniverse universe_;
  DispersionConfig cfg_;
  LifecycleSpec lifecycle_;
  HedgeSpec hedge_{};
  std::uint32_t cohort_counter_{0};
  std::int64_t front_expiry_{0};
  bool have_front_{false};
};

} // namespace atx::vol
