# atx-vol — changelog

Breaking behavioural changes are recorded here with their migration. Anything
that silently changes a NUMBER a caller already depends on belongs in this file.

## Unreleased

### NEW — vol-derivatives production surface: capped/mid-life swaps, greeks, dated fixings, DerivBook (derivatives-production sprint, Tasks 1-10)

**What shipped.** `derivatives.hpp` gains two capped product kinds
(`DerivKind::CappedVarSwap`, `CappedVolSwap`) plus a mid-life dispatch for
`VolSwap` contracts with `0 < n_done < n_total` (previously priced only at the
two exact-aged endpoints, inception and full accrual). All three price their
model leg against a lognormal distribution for the future realized-variance
leg (Gauss-Hermite / split-domain Gauss-Legendre quadrature,
`detail/rv_lognormal.hpp`): closed-form for the capped variance swap, kinked
split-domain quadrature for the capped vol swap, plain Gauss-Hermite for the
smooth mid-life sqrt payoff. A new `DerivConfig::vol_of_vol` knob (0 =
auto-calibrate so the lognormal's `E[sqrt(W)]` reproduces the surface's own
Carr-Lee `K_vol`) drives all three. `deriv_greeks` differentiates every
product kind via sticky-strike finite differences, with the center's resolved
strip grid and any auto-calibrated vol-of-vol pinned into every bump so a
bumped evaluation cannot land on a different quadrature scheme than the
center. `RealizedTracker::observe_dated` adds a strictly-ascending-timestamp
fixing entry point for daily-fixing drivers. New `deriv_book.hpp` prices a
book of swap positions against a `SurfaceSet` (additive companion to
`portfolio_pricer.hpp`'s option book, combined via `combine_totals`), and
`backtest.hpp`'s strategy-aware engine gains an additive variance/vol-swap
lane (`PortfolioState::swap_lots`, held to expiry, no early close in v1).

**Effect on existing callers.** Additive only. Every new type/field defaults
to the prior behavior: `DerivKind::VarSwap`/`VolSwap` dispatch is unchanged,
`DerivConfig::vol_of_vol = 0.0` auto-calibrates but that resolver is reachable
only from the new capped/mid-life dispatch paths, and
`PortfolioState::swap_lots` defaults empty (an empty-swap-lots book prices,
accrues and settles exactly as it did before this lane existed). One field's
OBSERVABLE SENTINEL does change: `DerivQuote::integration_error_est` was
unconditionally `NaN` (documented as "this port does not yet run the
Richardson half-step refinement"); `var_swap_fair_strike` now populates it
with a real Richardson half-grid estimate (`|I_h - I_2h|/15`) whenever the
resolved strip node count is `4m+1` — every quality-tier default and the E2
adaptive-wing rescale land there. A caller gating on `(x == x)` to mean "not
estimated" now sees a real number on those grids; a caller-pinned
`strip_nodes` that isn't `4m+1` still leaves it `NaN`, unchanged.

**Not shipped.** The RV distribution-affine / Monte-Carlo QE pricing engines
and the discrete-monitoring full-Monte-Carlo correction remain reserved and
actively return `NotImplemented`. CBOE variance-future marking
(`DerivMarkingConvention::CboeVarianceFuture`) is declared but unenforced — no
pricing path reads `DerivContract::marking` yet. `BacktestDb` refuses (rather
than silently drops) a run, checkpoint, or append that actually carries swap
data — its checkpoint and series schema predate the swap lane; schema support
is a deferred follow-on. Swap-lot entry is frictionless (zero cost, no
spread/impact) in v1, and `DerivBook` prices its positions single-threaded.

### BREAKING — `DispersionConfig::target_vega` is now dollars per VOL POINT (E1 / AN-P1-1)

**What changed.** `build_dispersion_book` (the projected / surface dispersion
route) sized the index leg as

```
straddle_qty = target_vega / (straddle_vega * multiplier)
```

where `DispersionLeg::straddle_vega` is a per-share `dP/dsigma`, i.e. vega per
**unit** vol. The listed route (`build_listed_dispersion_roll`) has always sized
off `vega_per_contract_per_vol_point = vega_per_unit_vol * multiplier * 0.01`,
i.e. vega per **vol point**. The same conceptual knob therefore carried two
conventions 100x apart: `target_vega = 10000` built a projected-route book
carrying \$10,000 of vega per 1.00 of sigma (\$100 per vol point) while the
listed schedule built one carrying \$10,000 per vol point.

Cross-route PnL and parity comparisons were meaningless unless the caller knew
to rescale by hand.

**The canonical unit is now dollars of gross index vega per ONE VOL POINT** — a
0.01 move in sigma. This is the industry convention and the unit the listed
route already used. `build_dispersion_book` divides by
`straddle_vega * multiplier * 0.01`, textually matching the listed route.

**Effect.** For an unchanged `target_vega`, projected-route books GROW BY EXACTLY
100x — contract counts, gross notional, premium, PnL and NAV all scale by 100.
`K`, `T`, `sigma`, `straddle_vega`, `call_mark` and `put_mark` are unchanged
bit-for-bit; only `straddle_qty` (and everything downstream of it) moves.

Note: the sprint plan's parenthetical said projected books would "shrink 100x".
That is the wrong direction — dividing by an extra factor of 0.01 makes the
quantity larger. The plan's normative formula (divide by
`straddle_vega * multiplier * 0.01`) and its cross-route test gate are both what
is implemented here.

**Migration.** Any caller tuned against the old projected-route behaviour must
DIVIDE its `target_vega` / `gross_index_vega` by 100 to keep the same book size.
This includes `DispersionBacktestConfig::gross_index_vega` and the
`gross_index_vega` key in `run_spec.tsv` for surface-route backtest runs.
Callers that were already feeding the listed route's number now get a book that
matches it, which is the point.

**Migration also applies to RISK LIMITS, and silently if you skip it.**
`DispersionRiskLimits::max_gross_vega` / `max_gross_notional` and
`DispersionRunConfig::capital` (`dispersion_run.cpp`, `strategy.hpp`) are
compared against a book that is now 100x larger. Following the migration above
(divide `gross_index_vega` by 100) DOES fix them, because
`measure_book`/`binding_limit` scale with the book — but a spec that sets a
limit and is NOT migrated starts CLAMPING or HALTING with no error: the same
limit value now binds at 1/100th of the intended book size. The failure mode is
a book that quietly stops trading, not a diagnostic. Migrate the limits with the
target, or set them to 0 (unlimited) while you do.

**Affected surfaces.** `DispersionConfig::target_vega`,
`DispersionBacktestConfig::gross_index_vega`, the `gross_index_vega` run-spec
key, `dispersion_run_surface_backtest`, `dispersion_run_projected_var`, and
every artifact they emit. The listed schedule route
(`gross_index_vega_target_per_vol_point`) is UNCHANGED — it was already correct.

**Golden replay pins.** The 82-session and 135-session `surface_backtest.tsv`
reproducibility pins move as a direct consequence (position scale -> NAV scale).
Re-pinning is a single coordinated event owned by the sprint controller; see the
disp-hotpath STATUS doc.

**Test gate.** `ListedDispersionSchedule.ProjectedAndListedRoutesAgreeOnVegaUnit`
builds both routes over the SAME three `PricedSurface`s at the same tenor,
multiplier and target, and asserts the two index-leg dollar-vega-per-vol-point
figures agree to 5%. Before this change it failed by exactly 100x (projected
100 vs listed 10000).

### FOLLOW-UP — the X3 gross-vega limit now honours the multiplier (C-2)

E1 above migrated the SIZING to dollars per vol point but left the X3 risk probe
(`measure_book`, `dispersion_strategy.cpp`) summing a bare
`|straddle_vega * straddle_qty|`. That expression discards the `multiplier` the
function is handed AND the per-vol-point scale, so
`DispersionRiskLimits::max_gross_vega` was compared in the advertised unit only
at the historical `multiplier == 100`; elsewhere the measured exposure was off by
exactly `100 / multiplier` (10x under-reported at 1,000, 10x over-clamped at 10).
`multiplier` is a real typed run-spec key on this branch, so non-100 books are
reachable from production.

**Effect.** `max_gross_vega`, and the `risk_clamp_scale` / `risk_breach_reason`
telemetry it drives, are UNCHANGED at `multiplier == 100` (the default, and the
82-session golden's value — which also configures no limits at all, so the golden
path never measures). At any other multiplier the measured gross vega changes by
`multiplier / 100`; a spec that pins both a non-100 multiplier and a
`max_gross_vega` must restate the cap in dollars per vol point.

The conversion now lives once, in `contract_vega_per_vol_point` (dispersion.hpp).
Projected sizing, the listed schedule's `vega_per_contract_per_vol_point` column
and its round-trip validator all adopt it with the same operand association, so
those three are bit-identical. Guarded by
`Strategy.DispersionGrossVegaLimitIsDollarsPerVolPointAtNonHistoricalMultiplier`,
whose oracle (`2 * target_vega`, for any multiplier) is hand-derived from the
sizing contract rather than re-evaluated from the code.
