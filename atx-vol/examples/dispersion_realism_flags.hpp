#pragma once

// dispersion_realism_flags.hpp — Task E1 (backtest-lakehouse sprint): the
// shared CLI flag surface for the friction / financing / policy knobs the
// dispersion-strangle example drivers (mag7_dispersion_backtest,
// spy_dispersion_pnl) build a `RunConfig` from.
//
// Before this task NONE of `FinancingConfig`, `hedge_slippage_bps`,
// `FrictionModel::SpreadKind::QuoteSide` (+ its crossing fractions),
// `UnpricedLotPolicy`, `ExercisePolicy` or `MarginBreachPolicy` were reachable
// from either CLI's argv — A3/B1/D1 made the engine honour them, but the
// example drivers never exposed a flag, so every run silently took
// `RunConfig{}`'s default no matter what an operator asked for
// (docs/reviews/2026-07-21-pipeline-sota-review/review-backtest.md:77: "wired
// by zero examples").
//
// Pulled into its own header rather than duplicated per-CLI so the two
// drivers cannot drift on a flag's spelling or its RunConfig mapping, and so
// the parse -> RunConfig path is unit-testable without spawning either binary
// -- the same seam `tools/surface_db_build_cli.hpp` already established for
// `--snapshot-suffix` (see `tests/dispersion_realism_flags_test.cpp`, which
// includes this header the same way `surface_db_build_test.cpp` includes
// that one).
//
// Division of labor: this header validates ENUM SPELLING only (an
// unparseable `--unpriced foo` is a loud usage error, matching this file's
// own `apply_realism_args` contract below). Numeric flags are accepted
// syntactically as given; an out-of-range value (e.g. a negative
// `--crossing-fraction-single`) is caught by `validate_run_config` at
// `run_backtest` time, exactly like every other RunConfig knob these CLIs
// already set without their own range checks (`--delta`, `--tenor-days`, …).

#include <cstdint>
#include <cstdlib>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

#include "atx/vol/backtest.hpp" // RunConfig, FrictionModel, FinancingConfig, UnpricedLotPolicy, ExercisePolicy, MarginBreachPolicy, FrictionRegime

namespace atx::vol {

// Raw parsed values for every flag this header owns. Every field mirrors a
// RunConfig knob 1:1; an unset optional (or `false` for the two bare bool
// flags) means "operator did not pass this flag", so `apply_realism_args`
// leaves the corresponding RunConfig field exactly as the caller already set
// it (RunConfig{}'s own default, or an earlier `--frictions`-style preset).
struct RealismArgs {
  std::optional<std::string> unpriced;        // --unpriced {exclude,error}
  std::optional<std::string> exercise_policy; // --exercise-policy {advisory,simulate}
  std::optional<std::string> margin_breach;   // --margin-breach {ignore,halt}

  std::optional<std::string> spread_kind;          // --friction-spread-kind {none,price_bps,vol_ticks,quote_side}
  std::optional<double> half_spread_bps;           // --half-spread-bps
  std::optional<double> vol_tick;                  // --vol-tick
  std::optional<double> impact_fraction;           // --impact-fraction
  std::optional<double> per_contract_cost;         // --per-contract-cost
  std::optional<double> hedge_slippage_bps;        // --hedge-slippage-bps
  std::optional<double> crossing_fraction_single;  // --crossing-fraction-single
  std::optional<double> crossing_fraction_complex; // --crossing-fraction-complex

  std::optional<double> borrow_rate;                     // --borrow-rate
  bool finance_premium{false};                           // --finance-premium
  bool shares_carry{false};                              // --shares-carry
  std::optional<double> initial_cash;                    // --initial-cash
  std::optional<std::uint32_t> financing_reference_uid;  // --financing-reference-uid
  std::optional<double> financing_flat_r;                // --financing-flat-r
};

// Appended verbatim to each CLI's own usage banner so the two drivers' --help
// text cannot drift from the flags this header actually recognizes.
inline constexpr std::string_view kRealismUsage =
    " [--unpriced exclude|error] [--exercise-policy advisory|simulate] "
    "[--margin-breach ignore|halt] "
    "[--friction-spread-kind none|price_bps|vol_ticks|quote_side] "
    "[--half-spread-bps N] [--vol-tick N] [--impact-fraction N] [--per-contract-cost N] "
    "[--hedge-slippage-bps N] [--crossing-fraction-single N] [--crossing-fraction-complex N] "
    "[--borrow-rate N] [--finance-premium] [--shares-carry] [--initial-cash N] "
    "[--financing-reference-uid N] [--financing-flat-r N]";

// Recognizes one of this header's flags at `arg`, consuming its value (via
// `nv`, called AT MOST ONCE) when it takes one. Returns false for anything it
// does not own -- a caller's own arg loop tries its OWN flags first (an
// exact-name collision belongs to the CLI, not this shared seam) and falls
// through to this dispatcher, then to "unknown arg", exactly the chain each
// CLI's own `parse_args` already implements for its existing flags. `nv`
// mirrors each CLI's own `(i + 1 < argc) ? argv[++i] : ""` next-value lambda.
[[nodiscard]] inline bool parse_realism_flag(std::string_view arg,
                                             const std::function<const char *()> &nv,
                                             RealismArgs &a) {
  if (arg == "--unpriced") {
    a.unpriced = nv();
  } else if (arg == "--exercise-policy") {
    a.exercise_policy = nv();
  } else if (arg == "--margin-breach") {
    a.margin_breach = nv();
  } else if (arg == "--friction-spread-kind") {
    a.spread_kind = nv();
  } else if (arg == "--half-spread-bps") {
    a.half_spread_bps = std::strtod(nv(), nullptr);
  } else if (arg == "--vol-tick") {
    a.vol_tick = std::strtod(nv(), nullptr);
  } else if (arg == "--impact-fraction") {
    a.impact_fraction = std::strtod(nv(), nullptr);
  } else if (arg == "--per-contract-cost") {
    a.per_contract_cost = std::strtod(nv(), nullptr);
  } else if (arg == "--hedge-slippage-bps") {
    a.hedge_slippage_bps = std::strtod(nv(), nullptr);
  } else if (arg == "--crossing-fraction-single") {
    a.crossing_fraction_single = std::strtod(nv(), nullptr);
  } else if (arg == "--crossing-fraction-complex") {
    a.crossing_fraction_complex = std::strtod(nv(), nullptr);
  } else if (arg == "--borrow-rate") {
    a.borrow_rate = std::strtod(nv(), nullptr);
  } else if (arg == "--finance-premium") {
    a.finance_premium = true;
  } else if (arg == "--shares-carry") {
    a.shares_carry = true;
  } else if (arg == "--initial-cash") {
    a.initial_cash = std::strtod(nv(), nullptr);
  } else if (arg == "--financing-reference-uid") {
    a.financing_reference_uid = static_cast<std::uint32_t>(std::strtoul(nv(), nullptr, 10));
  } else if (arg == "--financing-flat-r") {
    a.financing_flat_r = std::strtod(nv(), nullptr);
  } else {
    return false;
  }
  return true;
}

[[nodiscard]] inline bool parse_unpriced_policy(std::string_view s, UnpricedLotPolicy &out) {
  if (s == "exclude") {
    out = UnpricedLotPolicy::ExcludeAndReport;
    return true;
  }
  if (s == "error") {
    out = UnpricedLotPolicy::Error;
    return true;
  }
  return false;
}

[[nodiscard]] inline bool parse_exercise_policy(std::string_view s, ExercisePolicy &out) {
  if (s == "advisory") {
    out = ExercisePolicy::Advisory;
    return true;
  }
  if (s == "simulate") {
    out = ExercisePolicy::Simulate;
    return true;
  }
  return false;
}

[[nodiscard]] inline bool parse_margin_breach_policy(std::string_view s, MarginBreachPolicy &out) {
  if (s == "ignore") {
    out = MarginBreachPolicy::Ignore;
    return true;
  }
  if (s == "halt") {
    out = MarginBreachPolicy::Halt;
    return true;
  }
  return false;
}

[[nodiscard]] inline bool parse_spread_kind(std::string_view s, FrictionModel::SpreadKind &out) {
  if (s == "none") {
    out = FrictionModel::SpreadKind::None;
    return true;
  }
  if (s == "price_bps") {
    out = FrictionModel::SpreadKind::PriceBps;
    return true;
  }
  if (s == "vol_ticks") {
    out = FrictionModel::SpreadKind::VolTicks;
    return true;
  }
  if (s == "quote_side") {
    out = FrictionModel::SpreadKind::QuoteSide;
    return true;
  }
  return false;
}

// Applies every flag SET in `a` onto `rc`, in one place, so a flag this
// header parses is either visible here or provably dead (mirrors
// dispersion_run.cpp's own "single place the typed spec becomes engine
// behaviour" convention for its RunConfig builder). A field `a` leaves unset
// is untouched on `rc` -- calling this with a default-constructed
// `RealismArgs{}` is therefore a strict no-op, which is what keeps a CLI run
// with none of these flags byte-identical to before this header existed.
//
// Returns false (leaving `err` populated, `rc` possibly partially updated by
// the fields already applied before the bad one) on an unrecognized enum
// spelling -- the caller's convention throughout these two CLIs is to print
// the message and exit 2, the same "bad args" exit code `parse_args` itself
// uses.
[[nodiscard]] inline bool apply_realism_args(const RealismArgs &a, RunConfig &rc, std::string &err) {
  if (a.unpriced.has_value() && !parse_unpriced_policy(*a.unpriced, rc.unpriced)) {
    err = "invalid --unpriced value: " + *a.unpriced;
    return false;
  }
  if (a.exercise_policy.has_value() && !parse_exercise_policy(*a.exercise_policy, rc.exercise_policy)) {
    err = "invalid --exercise-policy value: " + *a.exercise_policy;
    return false;
  }
  if (a.margin_breach.has_value() &&
      !parse_margin_breach_policy(*a.margin_breach, rc.margin_breach)) {
    err = "invalid --margin-breach value: " + *a.margin_breach;
    return false;
  }
  if (a.spread_kind.has_value() && !parse_spread_kind(*a.spread_kind, rc.frictions.spread_kind)) {
    err = "invalid --friction-spread-kind value: " + *a.spread_kind;
    return false;
  }

  if (a.half_spread_bps.has_value()) {
    rc.frictions.half_spread_bps = *a.half_spread_bps;
  }
  if (a.vol_tick.has_value()) {
    rc.frictions.vol_tick = *a.vol_tick;
  }
  if (a.impact_fraction.has_value()) {
    rc.frictions.impact_fraction = *a.impact_fraction;
  }
  if (a.per_contract_cost.has_value()) {
    rc.frictions.per_contract_cost = *a.per_contract_cost;
  }
  if (a.hedge_slippage_bps.has_value()) {
    rc.frictions.hedge_slippage_bps = *a.hedge_slippage_bps;
  }
  if (a.crossing_fraction_single.has_value()) {
    rc.frictions.crossing_fraction_single = *a.crossing_fraction_single;
  }
  if (a.crossing_fraction_complex.has_value()) {
    rc.frictions.crossing_fraction_complex = *a.crossing_fraction_complex;
  }

  if (a.borrow_rate.has_value()) {
    rc.financing.borrow_rate = *a.borrow_rate;
  }
  if (a.finance_premium) {
    rc.financing.finance_premium = true;
  }
  if (a.shares_carry) {
    rc.financing.shares_carry = true;
  }
  if (a.initial_cash.has_value()) {
    rc.financing.initial_cash = *a.initial_cash;
  }
  if (a.financing_reference_uid.has_value()) {
    rc.financing.reference_uid = *a.financing_reference_uid;
  }
  if (a.financing_flat_r.has_value()) {
    rc.financing.flat_r = *a.financing_flat_r;
  }
  return true;
}

// B1's `FrictionRegime` has no stringifier anywhere in the library (it is a
// result-scalar enum, not a spec key any binder round-trips) -- this is the
// one CLI-local name table both drivers print into their meta block and
// console summary so a reader never has to cross-reference the numeric enum.
[[nodiscard]] inline std::string_view to_string(FrictionRegime regime) noexcept {
  switch (regime) {
  case FrictionRegime::Frictionless:
    return "frictionless";
  case FrictionRegime::Modeled:
    return "modeled";
  case FrictionRegime::QuoteSide:
    return "quote_side";
  }
  return "unknown";
}

} // namespace atx::vol
