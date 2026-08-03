# atx-vol — changelog

Breaking behavioural changes are recorded here with their migration. Anything
that silently changes a NUMBER a caller already depends on belongs in this file.

## 1.0.0

The first release with a stability promise. Everything below happened during the
production-v1 release sprint, on the way from an internal library to one that
installs into a prefix and can be depended on.

**What 1.0.0 actually promises** is a *tier*, not the tree: the 56 headers
`atx/vol/vol.hpp` includes are frozen for 1.x, and the manifest that says which
those are is machine-checked (`kTierA` in `atx-vol/tests/vol_umbrella_test.cpp`).
Everything else — Tier-B, `detail/`, `tools/`, `research/` — is public-but-
unfrozen or internal. The full policy, with the counts and the tests that
enforce it, is the *API stability policy* section of `README.md`. Read it before
depending on a header: this release moved a lot of them, deliberately, precisely
so the frozen set could be small and honest.

Because that reshaping is the release, **this section is mostly breaking
changes**. They are grouped by what a caller has to do about them.

### BREAKING — the public surface was tiered, and headers moved

Nothing was deleted in the tiering itself; every relocation is a `git mv`.

* **12 headers demoted to `detail/`** (`#include "atx/vol/X.hpp"` →
  `"atx/vol/detail/X.hpp"`): `parallel_for`, `pricing_executor`, `counters`,
  `phase_profile`, `prepared_fitting`, `prepared_policy`, `prepared_portfolio`,
  `strip_grid`, `run_archive_schema`, `backtest_series_columns`,
  `risk_surface_validation`. `listed_quote_key` was demoted and then **returned
  to Tier-B**, because `listed_opra.hpp` names `ListedQuoteKey` in a public
  signature — a caller could not use that parameter without naming a type the
  tier says carries no promise.
* **6 headers → `atx-vol-tools`** (`"atx/vol/X.hpp"` → `"atx/vol/tools/X.hpp"`):
  `run_report`, `surface_db_admin`, `surface_db_build`, `surface_db_exit_codes`,
  `surface_db_populate`, `tearsheet`.
* **9 headers → `atx-vol-research`** (`"atx/vol/X.hpp"` →
  `"atx/vol/research/X.hpp"`): `backtest_driver`, `dispersion_backtest`,
  `dispersion_run`, `dispersion_workflow`, `listed_definitions_cache`,
  `listed_dispersion_pipeline`, `listed_dispersion_reconciliation`,
  `run_archive`, `run_diagnostics`. The split line is **driver vs vocabulary**:
  headers that *compose* a research run moved; the dispersion domain vocabulary
  they are written in stayed public.
* **`atx/vol/curve.hpp` → `atx/vol/rates_curve.hpp`** (Tier-A). No symbol
  renamed; the rates vocabulary just collided visually with the vol-smile family
  (`vol_curve.hpp` / `spline_curve.hpp`).
* **`spy_fixture.hpp`** moved the other way — out of `tests/support/` and onto
  the public surface as Tier-B `atx/vol/spy_fixture.hpp` — because the shipped
  Python module and a bench reached into `tests/` for it.
* **The umbrella is now exactly Tier-A.** 7 headers joined it (`adjusted_greeks`,
  `corpus`, `priced_surface_view`, `query_pricing`, `spline_curve`, `surface_db`,
  `surface_policy`) and 14 left it for Tier-B (`batch`, `c8_calib`, `cstar`,
  `cstar_calib`, `essvi_calib`, `svi_calib`, `historical_projection`,
  `listed_dispersion`, `listed_dispersion_schedule`,
  `listed_dispersion_strategy`, `listed_opra`, `occ_ess`, `panel`, `s3`).
  Reaching those 14 now needs a direct include. `curve_selector` and
  `dense_slice` are deliberately NOT in the joined set — both were in the
  umbrella throughout 0.1.0 and describe no change for an upgrading caller — and
  `portfolio` / `portfolio_risk` are not in the Tier-B set, because they were
  **removed outright rather than demoted** (see REMOVED below).
* **`Surface<Slice>`, `SviSurface`, `EssviSurface`, `C8Surface`, `CStarSurface`**
  moved to `detail/legacy_surface.hpp`, `detail/legacy_c8_surface.hpp`,
  `detail/legacy_cstar_surface.hpp`. The **namespace did not change** — they are
  still `atx::vol::` — so the migration is one added include. The canonical
  pipeline is `CurveSurface` (fit) → `PricedSurface`/`PricedSurfaceView` (serve)
  → `SurfaceSet` (portfolio), and public headers may no longer name the demoted
  containers even in prose.
* **The templated `derivatives.hpp` entries are now instantiated for
  `VolSurface`.** `var_swap_fair_strike`, `vol_swap_fair_strike`, `deriv_price`
  and `deriv_greeks` are templates on the surface type whose bodies live in
  `derivatives.cpp`; every instantiation used to be on a demoted container, so
  these Tier-A declarations could only be linked against by including a
  `detail/` header. `VolSurface` — which answers `iv(k_log, T)`, the template's
  whole requirement — joins the instantiation set, and the demoted pair stays
  for source compatibility. Purely additive: no existing call changes.

### BREAKING — error model: batch entries report how many lanes they wrote

Ten `Status`-returning batch entries now return `Result<std::size_t>`, carrying
the number of lanes written and defined only on success. `Result<T>` itself is
unchanged (`tl::expected<T, atx::core::Error>`); error codes and messages are
byte-identical.

`black76_price_batch`, `black76_price_from_lnfk_batch`,
`black76_value_and_vega_batch`, `implied_vol_batch`, `black76_greeks_batch`,
`essvi_w_batch` (`batch.hpp`); `american_price_batch`,
`american_price_batch_resolved`, `american_greeks_batch`
(`american_batch.hpp`); `american_implied_vol_batch` (`american_iv.hpp`,
Tier-A).

*Migration.* Inline uses (`if (f(...))`, `ASSERT_TRUE(f(...))`) bind unchanged —
both types are `expected`. Only declaration-form sites move:
`const Status st = f(...)` → `const Result<std::size_t> st = f(...)`, and
forwarding sites change `return st;` → `return Err(st.error());`. Output spans
and the per-lane `std::span<Status>` channel are untouched.

Two smaller shape changes: `configure_pricing_executor` returns
`[[nodiscard]] Status` instead of a discardable `bool`, and
`ticker_seed_profile` returns `std::optional<ProfileKind>` instead of taking an
out-param (`if (ticker_seed_profile(t, kind))` → `if (const auto k =
ticker_seed_profile(t); k.has_value())`).

### BREAKING — positional aggregate initialisation is no longer supported

`AlOpts`, `RunConfig`, `SessionInputs` and `SurfaceParityReport` were reordered
and pinned with a field-count `static_assert`. **Use designated initializers.**
`AlOpts{3, 3, 1, 1.0e-1}` is now wrong — `n_quad_price` moved from last to
third. Each of the four headers states that this is the last layout change
allowed; post-1.0 knobs append at the end with no positional promise. Python is
unaffected: keyword names, arity and signature are unchanged.

`RunConfig`'s pin moved **15 → 16** later in the same sprint when `cancel` was
added (see *Embedding* below). It was INSERTED beside `step_observer`, its
semantic group — not appended — which is precisely the freedom the new convention
buys and the old one forbade. Named initialisation is unaffected by construction;
a positional one would have rebound, which is why none is allowed to exist.

It then moved **16 → 17** when the release branch merged `main` (2026-08-02),
which brought the backtest-replay work's `RunConfig::prefetch_depth`
(`std::size_t`, default `2`). This one is **appended at the end**, the form the
convention prescribes for a new knob. Two notes a caller may care about:

* **It changes no output at any value.** `prefetch_depth` is purely an I/O
  schedule — how many future snapshots may be in flight — never which bytes are
  deserialised nor the order the economics consume them.
  `Backtest.PrefetchDepthIsBitIdenticalToSingleStepLookAhead` pins that
  bit-identity, and the SPY-dispersion NAV determinism legs reproduce their
  anchors bit-exactly across the merge that introduced it.
* **The default is `2`, not the historical single-step `1`.** It arrived from
  `main` at `1` and was moved to `2` in this release (S6-T32, plan 6.7) on a
  paired measurement: one binary with the depth alternated inside a single
  session, 12 interleaved rounds on the 135-session SPY-dispersion replay,
  medians and win-counts only — `1 → 2` **+15.2 % (11/12 rounds)**, then
  `1 → 4` +19.8 % (10/12) and `1 → 8` +19.6 % (10/12), while `2 → 4` (+1.9 %,
  7/12) and `4 → 8` (+1.6 %, 7/12) are washes. The curve is a step, not a ramp:
  overlapping the first load is the whole win, so `2` is the cheapest default
  that takes it. **A run that wants the old shape sets `1` explicitly and gets
  it bit-for-bit** — by the note above, no value of this field moves a number.
  The cost of the new default is one extra in-flight snapshot and a private
  cache of `4` slots instead of `3`.
  `0` is still normalised to `1` — "no look-ahead" is expressed by
  `prefetch_snapshots = false`, not by a zero depth. A caller-supplied
  `snapshot_cache` must retain at least `depth + 2` entries or the LRU drops a
  completed prefetch before its step reaches it (costing throughput, never
  correctness); `run_backtest`'s private cache is sized from the field
  automatically.

Python's ARITY and keyword names are unaffected by both moves — the binding is a
hand-kept `def_readwrite` list, and `prefetch_depth` is exposed through it
(`python/src/bindings/backtest.cpp`) as an attribute, not as a constructor
keyword. A Python caller who never touches the attribute therefore picks up the
new default exactly as a C++ caller does;
`python/tests/test_backtest.py::test_run_config_prefetch_depth_round_trips`
asserts it.

### REMOVED

* **The deprecated `VolSurface`-bound portfolio engine**: `portfolio.hpp`,
  `portfolio_risk.hpp` and 34 symbols (`PortfolioLeg`, `LegKind`, `AggMode`,
  `bulk_price`, `scenario_pnl`, `project_compare`, …). Replacements, honestly:
  multi-shock scenarios → `scenario_grid.hpp`; theoretical legs →
  `contract_projection.hpp`; factor attribution → `pnl_attribution.hpp`; `ByUid`
  aggregation → `reduce_risk_buckets`. **Stock/cash legs, bulk selection, and
  the ByUidExpiry / ByGroupId aggregation views have no canonical counterpart.**
  Deleting the header also resolved a latent ODR conflict: `atx::vol::LaneStatus`
  had two different definitions, and the `american_batch.hpp` one survives.
  `scenario_grid.hpp` and `pnl_attribution.hpp` were promoted to Tier-A.
* **The `SurfaceArchive` v1 writer/reader** (`write_surface_archive[_file]`,
  `class SurfaceArchive`, `SurfaceArchiveWriteOpts`,
  `archive_identity_from_header`) and the `atx-vol-archive-v1` library.
  ATXVSA2 is the only shipped surface-archive format. The retired format's
  on-disk record declarations are kept as reference — `RunDir::run_identity_hash`
  still recognises such a file by its magic. Note this header used to declare
  symbols a plain `atx::vol` link could not resolve.
* `calib_pool.hpp` (`calibrate_pool`, `CadenceQueue`); `vola_parity.hpp`;
  `arb_project_calendar_essvi_total`; the four `derivatives.hpp` unit
  constexprs (`var_dec_to_points`, `var_points_to_dec`, `vol_dec_to_points`,
  `vol_points_to_dec`); `dispersion_build_schedule`, `dispersion_run_backtest`,
  `dispersion_verify`, `DispersionVerifyReport`.

### Numbers that moved

These change results without changing a signature — the category this file
exists for.

* **Deep-OTM put premia.** Black-76 puts are priced from `Φ(-d)` instead of
  `1 - Φ(d)` in `black76_aux`, `black76_value_and_vega`, `black76_greeks`, the
  implied-vol Halley loop and the AVX2 kernels. Far-wing values move; they were
  catastrophically cancelling to zero or negative.
* **AVX2 P&L.** The vector kernel adopts the scalar association tree, so
  `total == sum(terms)` holds and a position's P&L no longer depends on its
  batch index — a contract `simd/pnl_batch.hpp` already claimed.
* **Archive bytes and content identity are now reproducible.** Slice-params
  padding is no longer memcpy'd into archive records, so the same fitted slice
  produces the same `payload_crc32c` every time. Archives written by earlier
  builds are not byte-reproducible by this one.
* **`VolSurface::iv` returns NaN** for non-finite or non-positive `T` (was
  `+inf`).
* **A moved-from `PricedSurfaceView` is structurally empty** and answers no
  queries; it previously answered and could index out of bounds.
* **Vol-time is fail-closed.** `trading_hours_between`, `vol_time_years`,
  `time_to_expiry_years` and `tenor_years` return `Result<double>` instead of
  `double`, and `VolTimeCalendar` requires an explicit coverage window
  (`us_default()` covers 2024-01-01..2028-12-31). Out-of-window queries are
  `ErrorCode::OutOfRange` rather than a silently credited 7.5h session.
* **`all_symbols` / `universe_at`** lost their `index_symbol = "SPY"` default;
  the argument is required.
* **Corrupt archives that used to be accepted now fail with `ParseError`** —
  the LinearVariance node axis is validated and slice payload extents must be
  monotone and disjoint. Backtests fail closed on backwards snapshot timestamps.
* **`PortfolioPricer`'s returning `price()` / `pnl_explain()` are genuinely
  concurrent-const-safe** (per-call workspace), at the cost of the cross-call
  cached workspace.
* **`BacktestResult::validate()`** is new and enforced at `run_backtest` and the
  three TSV/CSV writers: every column must be empty or exactly `size()` long. A
  producer handed a skewed result now returns `InvalidArgument`.
* **Loose dispersion result TSVs are off by default**, behind
  `DispersionRunConfig::emit_tsv_diagnostics` (spec key of the same name,
  default `false`). Retained-input and evidence TSVs are unaffected.
* **`surface_insert_vol_slice(..., with_no_arb_check = true)` now actually
  checks.** The parameter used to be accepted and discarded, leaving
  `InsertedSliceHandle::no_arb_status == 0` unconditionally. It now runs a dense
  butterfly/calendar sweep over the resolved slice and reports through
  `no_arb_status` (`kNoArbStatusButterfly` / `kNoArbStatusCalendar` /
  `kNoArbStatusNotEvaluated`) plus the `kFlagNoArbWarning` provenance bit. It is
  a report, never a rejection — the handle is still returned, with the same
  numeric contents. The default (`false`) path is unchanged and still costs
  nothing, so no shipped caller's numbers move.

### NEW — embedding: a diagnostic sink and cooperative cancellation

The two things a library has to offer before a host can embed it: give up the
process's streams, and be stoppable.

* **`atx/vol/log.hpp` (Tier-B) — diagnostic sink.** `install_log_sink(sink, user)`
  routes every diagnostic atx-vol emits to a callback carrying a `LogLevel`, a
  `LogStream` and one newline-free line. **All 13 library stream writes across 5
  source files now go through it**; no `fprintf`/`printf`/`cerr`/`cout` to a
  process stream remains in library code.
  **With no sink installed, output is byte-identical to 1.0.0-pre**: the same
  text on the same stream, so this is not a behavioural change for any existing
  consumer. The stream is carried on the record rather than derived from the
  level, precisely so the two Info-level sites that historically wrote to
  *different* streams both stay unchanged.
  The callback must not throw (the emit path is `noexcept`), must not re-enter
  atx-vol, and **must tolerate concurrent invocation** — pricing-pool workers
  emit, so records arrive on threads the host never created, and record order
  across threads is not defined. Install once, before the first emitting call.
* **`ErrorCode::Cancelled` (atx-core)** — appended last, so no existing
  enumerator's `u16` value moved.
* **Cooperative cancellation on the four long-running entries.** A `CancelToken`
  (`atx/vol/types.hpp`) is a non-owning view of a caller-owned
  `std::atomic<bool>`; a default-constructed one never cancels and costs one
  branch per poll. Plumbed as `RunConfig::cancel` (**this is the 15 → 16 field
  above**), `CorpusConfig::cancel`, `SurfaceDbPopulateConfig::cancel`, and — for
  the run-dir-only entry that has no caller-supplied config — a defaulted
  trailing parameter on `dispersion_run_projected_var`.
  Cancellation is a **clean early return with `ErrorCode::Cancelled`, never a
  partial write**. Each entry polls at the top of a loop iteration, before that
  iteration writes anything: `run_backtest` returns no result at all (and writes
  no files in any case); `build_corpus` leaves no manifest, so the corpus never
  claims to be complete; `populate_surface_db` leaves a **valid database holding
  a prefix of the dates**, because each date is committed atomically with a
  generation-bumped manifest — stop a long backfill and re-run to resume;
  `dispersion_run_projected_var` writes its artifacts only after the work it
  cancels, so the run dir is untouched. The two fan-out entries (`build_corpus`,
  `populate_surface_db`) additionally poll at the **top of each fit task**, so a
  stop drains the queued fits instead of running them to completion — the cancel
  shortens the run rather than only declining to publish its index. A fit already
  **in flight** is never abandoned: the call returns once the boards already
  running finish.

### Packaging, versioning and ABI

* **`find_package(atx-vol)` works from an install prefix.**
  `cmake --install <build> --prefix P` ships headers, static archives and
  `atx-volConfig.cmake`; the exported targets are `atx::vol`, `atx::core`,
  `atx::tsdb`, `atx::vol-tools`, `atx::vol-research`, with `atx::vol::tools` /
  `atx::vol::research` recreated so in-tree source compiles against the install
  unchanged. `Result<T>` is still `tl::expected<T, atx::core::Error>` and
  `tl-expected` installs into the same prefix.
* **The version is single-sourced** from `project(atx VERSION ...)` through a
  generated `atx/vol/detail/version_generated.hpp`. `atx::vol::version()` no
  longer carries its own literal. New: `ATX_VOL_VERSION_{MAJOR,MINOR,PATCH}`,
  `ATX_VOL_VERSION_STRING`, `ATX_VOL_VERSION_NUM(a,b,c)` and `ATX_VOL_VERSION`
  for preprocessor feature-gating, plus `atx::vol::kVersionString`.
* **Package compatibility is now `SameMajorVersion`** (was `SameMinorVersion`,
  correct only while the version was 0.y.z). A `find_package(atx-vol 1.0)`
  consumer accepts any 1.z.
* **atx-vol 1.x is distributed static-only**, with no `ATX_VOL_API` export
  macro — see the *Linkage and distribution policy* section of `README.md` for
  why (header-inline instrumentation globals get one instance per image on
  Windows). `BUILD_SHARED_LIBS` now fails configure with the reason instead of
  being silently ignored, and `cmake --install` refuses a shared build.
* **The `ATX_VOL_COUNTERS` / `ATX_VOL_PROFILE` ODR trap is closed.** Those
  options change the definition of inline entities in
  `atx/vol/detail/counters.hpp` and `atx/vol/detail/phase_profile.hpp`; a
  consumer that disagreed with the library used to silently read a plane nobody
  wrote. The configuration is now part of an inline namespace name, so a
  mismatch fails to **link**, naming both sides. No struct layout changed and no
  computed value moves. `ATX_VOL_PROFILE_CONCAT[_INNER]` are renamed
  `ATX_VOL_PROFILE_DETAIL_CONCAT[_INNER]` and defined in both configurations.
* **Archive format naming is unified on the on-disk magic**: the live format is
  **ATXVSA2** (magic `ATXVSA20`) and the retired one is **ATXVSA03** (magic
  `ATXVSA03`). The old "v1" / "v3" ordinals are gone from the headers — they
  named the same format both ways. Comment-only; no identifier changed.

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
