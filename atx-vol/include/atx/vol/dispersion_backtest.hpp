#pragma once

// Reusable surface-only dispersion backtest orchestration. The example CLI is
// intentionally limited to parsing files and writing artifacts; strategy,
// lifecycle, hedge, and engine defaults live here for library callers.

#include <cstddef>
#include <string_view>
#include <vector>

#include "atx/vol/backtest.hpp"
#include "atx/vol/dispersion.hpp"
#include "atx/vol/dispersion_workflow.hpp" // UniverseRow (point-in-time schedule)
#include "atx/vol/strategy.hpp"

namespace atx::vol {

// ── X6: transaction-cost model ──────────────────────────────────────────────
//
// Fills were unconditionally at the fitted MID. Real option execution pays the
// half-spread plus a size-dependent market impact; the standard form is the
// square-root law (Almgren, beta ~ 0.6; Obizhaeva-Wang):
//
//     impact = k * participation^beta        (as a FRACTION of price)
//
// `k` is dimensionless so impact composes with the engine's existing per-share
// price-bps spread lane, which is what lets the dispersion path express this
// model without a new engine fill hook — see `dispersion_effective_frictions`.
//
// ALL-ZERO IS THE DEFAULT and collapses exactly to the historical mid fill, so
// the pinned golden is untouched until a run spec opts in.
struct DispersionCostModel {
  double k{0.0};            // impact coefficient (fraction of price)
  double beta{0.6};         // square-root-law exponent
  double adv_fraction{0.0}; // participation: traded quantity / ADV

  [[nodiscard]] bool active() const noexcept { return k > 0.0 && adv_fraction > 0.0; }
};

// The model in its textbook form: signed half-spread plus square-root impact,
// both per share. `half_spread` and the returned price are absolute; `m.k` is a
// fraction of `mid`. Trade direction comes from the SIGN of `signed_qty` — note
// that `Side` in this codebase is the option's Call/Put, not a buy/sell, so it is
// not the right discriminator. Exposed (and unit-tested) as the single definition
// of the cost arithmetic even though the run path folds it into `FrictionModel`.
[[nodiscard]] double fill_price(double signed_qty, double mid, double half_spread, double adv_frac,
                                const DispersionCostModel &m) noexcept;

// Fold the square-root impact into `base` as an additional price-bps half-spread.
// EXACTNESS NOTE: participation is a per-RUN constant here, so the impact is a
// constant fraction of price and is therefore exactly representable in the
// existing PriceBps lane. A per-trade, ADV-varying participation would need an
// engine-side fill hook (the engine owns execution); that is a deliberate
// limitation of this seam, not an approximation hidden inside it.
[[nodiscard]] FrictionModel dispersion_effective_frictions(const FrictionModel &base,
                                                           const DispersionCostModel &costs);

struct DispersionBacktestConfig {
  double target_dte_days{30.0};
  double roll_dte_days{7.0};
  // ── UNIT (E1 / AN-P1-1): DOLLARS OF VEGA PER VOL POINT ────────────────────
  //
  // Assigned STRAIGHT THROUGH to `DispersionConfig::target_vega`
  // (dispersion_backtest.cpp:26) with no scaling, so it carries that field's
  // unit exactly: dollars of index-leg gross vega per ONE VOL POINT (a 0.01
  // move in sigma). Same unit as `ListedScheduleSpec::gross_index_vega` and
  // `ListedScheduleBuildConfig::gross_index_vega_target_per_vol_point`.
  //
  // BREAKING CHANGE (E1), recorded here because E1 changed this field's MEANING
  // without touching this header (REV-TAIL M-6). It used to reach a
  // `target_vega` read as dollars per UNIT vol, so the same number now builds a
  // book 100x LARGER. A caller tuned before E1 must DIVIDE its old value by 100.
  double gross_index_vega{10'000.0};
  double delta_band{0.0};
  std::size_t min_names{2};
  unsigned entry_every_n{21};
  bool project_to_calendar_expiry{true};
  bool record_diagnostics{false};
  RunConfig run{};
  // Appended so existing aggregate initialization keeps compiling. Defaults are
  // exactly the previously-hardcoded values.
  DispersionSide side{DispersionSide::ShortIndexLongNames};
  double multiplier{100.0}; // was a hardcoded 100.0 at every construction site
  HedgeSpec::Kind hedge_kind{HedgeSpec::Kind::DeltaToZero};
  HedgeSpec::Cadence hedge_cadence{HedgeSpec::Cadence::Daily};
  DispersionRiskLimits limits{}; // X3; default unlimited
  // X4 sizing/strike policies; both default to exactly the shipped construction.
  WeightingScheme weighting{WeightingScheme::VegaNeutral};
  StrikePolicy strike{};
};

// Construct the canonical surface-only dispersion strategy used by research
// and production backtests. The authored universe is rebound by symbol on each
// snapshot by DispersionStrategy. This overload FREEZES the passed membership for
// the whole run (no point-in-time reconstitution); prefer the schedule overload
// below for a multi-block universe.
[[nodiscard]] DispersionStrategy
make_dispersion_backtest_strategy(DispersionUniverse universe,
                                  const DispersionBacktestConfig &config = {});

// C1 point-in-time overload. The strategy re-resolves its basket from `schedule`
// on every step (via `make_pit_universe_resolver`), so a mid-window
// reconstitution — including a name being REMOVED (C3) — is honored at the next
// roll instead of freezing day-1 membership. `index_symbol` (default "SPY") is
// the never-a-constituent index leg. With a single-block schedule this is
// behaviourally identical to the frozen overload.
[[nodiscard]] DispersionStrategy
make_dispersion_backtest_strategy(std::vector<UniverseRow> schedule,
                                  const DispersionBacktestConfig &config = {},
                                  std::string_view index_symbol = "SPY");

// Run the canonical strategy over an already-qualified Clock. Callers retain
// control of corpus construction and artifact persistence.
[[nodiscard]] Result<BacktestResult>
run_dispersion_backtest(const Clock &clock, DispersionUniverse universe,
                        const DispersionBacktestConfig &config = {});

// C1 point-in-time overload: same as above but re-resolves the basket per step
// from `schedule`. This is the flagship surface-path entry that honors mid-window
// reconstitution/removals.
[[nodiscard]] Result<BacktestResult>
run_dispersion_backtest(const Clock &clock, std::vector<UniverseRow> schedule,
                        const DispersionBacktestConfig &config = {},
                        std::string_view index_symbol = "SPY");

} // namespace atx::vol
