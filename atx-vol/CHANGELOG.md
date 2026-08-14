# atx-vol — changelog

Breaking behavioural changes are recorded here with their migration. Anything
that silently changes a NUMBER a caller already depends on belongs in this file.

## 1.1.0

Vol-derivatives production sprint, Phase 1 (correctness), Phase 2
(performance), and Phase 3 (features).

SCOPE, CORRECTED IN THIS RELEASE. This section used to say it "grows only with
changes that move a number a caller could already be marking with". That has
never been the rule this file follows. Every Phase-3 entry below is a purely
ADDITIVE feature that states in its own text that it moves no existing number
("MOVES NO EXISTING NUMBER", "NO EXISTING NUMBER MOVES", "No existing mark
moves"), and 1.0.0's `### NEW` entries are the same shape. Keeping that
sentence would mean deleting most of what is here, so the sentence goes
instead. What actually governs is the preamble's rule -- a change that moves a
number a caller depends on MUST be recorded, with its migration -- widened by
long practice to also record PUBLIC API a 1.x caller can newly reach. A change
that moves no number and adds no public name is still out. `b1558f7` (one
shared butterfly FD stencil in place of four hand-written copies, established
bit-for-bit against the pre-unification arithmetic) and `8cf6951` (one
ns-per-year constant in place of four, bit-exact and now `static_assert`ed) are
worth naming because each looks at a glance like it should have an entry; both
deliberately have none. THAT IS TWO EXAMPLES, NOT A COUNT. Read as "exactly two
such commits landed", the sentence was already stale: `331f86f` unified a third
predicate the same way, and it earns an entry below only because it adds two
public NAMES, never because it moved a number. Judge a commit against the rule,
not against this list.

### Added — surface dynamics: `SurfaceOverlay` / `StickyMode`, `DerivPnlExplain`, and a scenario deriv leg (FIT-F4 / GK-G4 / GK-G5 / LIT-8, Task F-8)

NO EXISTING NUMBER MOVES. Three new public surfaces, one of them a refactor of
shipped internals whose bit-identity is the entry's main content.

**`atx/vol/surface_overlay.hpp` (Tier-B).** `SurfaceOverlay<SurfaceT>` is a
non-owning view over any `iv(k, T)` source with
`{vol_shift, skew_shift, convexity_shift, k_shift, term_scale}`, plus
`StickyMode` and `sticky_k_shift(mode, spot_rel)`. Before this there was no way
to say "the same surface, seen from 1% higher spot" without refitting, and
`derivatives.cpp` carried three private views doing exactly that, kept in step
by hand. Those three are now one composition over this type. Sticky-strike is
`k_shift = +ln(1+h)`: with `k = ln(K/F)` a spot move scales `F` by `(1+h)`, so a
fixed strike sits at `k - ln(1+h)` and reading its unchanged vol means
evaluating the base at `k + ln(1+h)`.

MIGRATION: none. `deriv_greeks` / `deriv_price` / `price_deriv_book` return
bit-identical values across 6084 dumped bit patterns (all 14 `DerivGreeks`
fields plus 14 centre-quote diagnostics, over 2 surfaces x 5 kinds x 4 bump
configs x aged/fresh x cached/uncached, plus the deriv-book memo lane), byte-
compared before and after. `kMinSmileShiftedIv` and `floor_smile_iv` moved from
`derivatives.cpp`'s anonymous namespace into this header unchanged.

**`atx/vol/deriv_pnl.hpp` (Tier-B).** `deriv_pnl_explain` decomposes a swap
position's two-date mark move into carry, realized, vol level, skew, convexity
and discount, leaving everything else in a visible residual. It holds no
surface and calls no pricer; `var_swap_fixing_weight` supplies the linear
variance leg's per-fixing sensitivity and returns NaN for every kind that has
no constant one. An unavailable sensitivity yields a NaN component, a
`DerivPnlFlags` bit naming it, and a poisoned residual -- never a zero.

**`scenario_grid` deriv overload.** Takes a `DerivPosition` book alongside the
option book and fills `ScenarioGridResult::deriv_pnl` / `deriv_route` /
`n_deriv_*`. `ScenarioDerivSpec` adds `d_skew` / `d_convexity` scalars applied
to every cell. `ScenarioGridResult` gained `deriv_pnl`, `deriv_route`,
`n_deriv_ok`, `n_deriv_failed` and `n_deriv_exact_fallback_lanes` here, plus
`n_deriv_missing_sensitivity` in the round-1 entry below -- all EMPTY or zero on
the option-only overload, whose per-cell output is byte-identical to before. The
struct's TOTAL is deliberately not restated here: it is a `static_assert`
(`detail::aggregate_arity_is_v<ScenarioGridResult, ...>`, scenario_grid.hpp,
added by `f505225` -- the struct had no pin at all, which is how this leg landed
without one to update), so the count lives where a build breaks if it drifts
rather than in prose nothing checks. `detail::deriv_price_shocked_on_ref`
(+ `detail::DerivShock`) is the Exact cell's repricing: a sticky-strike respot
with no smile roll, with the centre scheme pinned as of the round-1 entry below.

### Added — the backtest swap lane emits its P&L explain: `RunConfig::swap_pnl_explain` (GK-G5, Task F-8)

NO EXISTING NUMBER MOVES, and that is measured with the feature ON rather than
argued from the flag being off. `swap_pnl` reported the number and nothing about
where it came from, so a bad day and a bad model looked identical.

`BacktestResult` gains the attribution columns `swap_explain_carry`,
`_realized`, `_vol_level`, `_skew`, `_convexity`, `_discount` and
`_residual`, plus a `swap_explain_unattributed` counter, decomposing exactly the
`swap_pnl` beside them through `deriv_pnl_explain` (above), evaluated per live
lot against the START of each step and summed over the lane. The roster is
single-sourced as `swap_explain_columns()`, with a per-index `static_assert`
pinning each row to its own member, so a reorder cannot compile; `backtest.hpp`
names the sites a new column still has to touch, and what stops the build at
each, rather than restating a tally here.

* OFF BY DEFAULT (`RunConfig::swap_pnl_explain`). Not
  timidity about the numbers: ON, each live lot costs one extra
  `deriv_greeks_on_ref` per step -- up to 20 repricings where the mark alone is
  one -- plus three surface reads for the smile observables. A run that does not
  read the explain should not pay for it.
* OFF, THE COLUMNS ARE EMPTY RATHER THAN ZERO-FILLED -- the distinction
  `nav_liquidation` already makes, and the one that separates "not measured"
  from "measured flat". Also always empty on the fixed-book overload
  (`BacktestSwapExplain.ColumnsAreEmptyRatherThanZeroFilledWhenOff`).
* EVERY COMPONENT IS A FLOW, block-summed exactly like `swap_pnl`, so
  `carry + realized + vol_level + skew + convexity + discount + residual ==
  swap_pnl` holds row by row AND under `record_every_n > 1`. A component
  accumulated as STATE would have broken that silently at any stride above 1,
  which is why it is pinned rather than trusted
  (`BacktestSwapExplain.ComponentsSumToSwapPnlOnEveryRow`,
  `IdentitySurvivesRecordEveryN`).
* WHAT IS DELIBERATELY NOT ATTRIBUTED, and counted instead of hidden: a step
  where no fixing landed -- which covers a lot's own first mark, and a closed
  series, because carry prices a fixing arriving at a zero return and on such a
  step that term describes an event that did not happen -- a lot whose
  sensitivities came back "not computed", and every
  settlement (a payoff is not a market move). Each books its whole move to
  `swap_explain_residual`, so the identity stays exact, and increments
  `swap_explain_unattributed` -- so a large residual on an expiry date reads as
  a settlement rather than as a modelling gap.
* THE CONVEXITY COLUMN'S SCALE. The smile observables are read in the same
  `k = ln(K/F)` convention the sensitivities differentiate, and the curvature
  read is the coefficient `c` in `a + b*k + c*k^2` -- HALF the second difference
  -- because that is what `SurfaceOverlay::convexity_shift` adds. The other
  choice halves or doubles every convexity attribution while leaving the
  identity intact, so the identity alone would not have caught it.
* NO PRIOR-STEP STATE IS CARRIED AT ALL, and `SwapAccrual` was not widened for
  the explain either -- that struct round-trips through checkpoints and carries a
  hand-written comparator with its own drift pin, and widening it for a
  diagnostic that moves no mark would be the wrong trade. The start-of-step
  sensitivities resolve against the snapshot the step is measured FROM, which
  the engine already holds. A RESUMED RUN THEREFORE ATTRIBUTES ITS FIRST STEP;
  see the round-2 entry below, which is where that stopped being a limitation.

**THE SHIPPED EXAMPLE'S TSV GAINS THE EXPLAIN COLUMNS** (`5cbbc55`, `2cc386f`),
so this is visible in a report and not only through the C++ API.
`examples/varswap_compare_example.cpp` turns the explain ON -- one swap lot is
live per cycle there, so the bill is one lot's worth of repricing, and
attributing `swap_pnl` is the whole point of the report it feeds -- and attaches
the whole roster beside the `swap_pv` / `swap_pnl` it already emitted, by
ITERATING `swap_explain_columns()` rather than hand-listing anything, so a
column added to the roster reaches the TSV with no edit to the example. THE
COLUMN NAME IS THE FIELD NAME on every one, so a reader parsing that TSV BY
POSITION rather than by header name must re-read it.
`tools/render_strangle_vs_varswap.py` grows the matching attribution panel,
DISCOVERING the components off the track's own header row by the
`swap_explain_` prefix rather than from a roster written down a second time.

* `swap_explain_unattributed` IS NOT A TERM IN THE IDENTITY. It is block-summed
  the same way the seven flows are, but it counts LOT-STEPS rather than dollars,
  and the moves it counts already sit inside `swap_explain_residual` -- so the
  renderer sums every `swap_explain_` column EXCEPT this one. Aiming that single
  exclusion at a stale name folds the counter into the attribution and moves the
  measured identity gap from $0.00 to $2.00 on the example's own fixture, which
  is what the Python lane's gate test measures rather than asserts.
* A TRACK WITHOUT THESE COLUMNS READS AS NOT MEASURED, NEVER AS ZERO, and that
  holds at each layer for its own reason. The engine leaves the vectors EMPTY
  with the flag off, never zero-filled. The example refuses to emit a
  half-populated track at all: a column that is not row-parallel is
  `InvalidArgument` NAMING the column and pointing at
  `RunConfig::swap_pnl_explain`, not a zero-filled stand-in. And the renderer's
  derived summaries are NaN rather than 0.0 on a track that never carried the
  columns -- `Legs.total_unattributed` deliberately does NOT forward to pandas'
  `.sum()`, which answers 0.0 for an all-NaN series and would report "every
  lot-step was attributed" for a run that never looked. Inside a column that IS
  present, a NaN cell stays NaN and gaps its line. This is the same "not
  computed" vs "computed as zero" distinction the round-1 entry below turns on.

MIGRATION: none. With the flag ON, `nav`, `cash`, `pnl_total`, `swap_pv`,
`swap_pnl` and `pnl_settlement` are bit-identical across every row of the same
book (`BacktestSwapExplain.NavIsUnmovedByTheExplain`, which runs the book twice
and compares on the bits) -- the explain only ever reads. The new columns are
NOT part of the frozen `kBacktestSeriesColumns` / RunArchive registry, so
`ra_schema_hash()` and every golden are untouched; they ride the TSV's dynamic
signal tail, which is why the example could grow them without a schema bump.
Landed in `f505225`, carried to the Python lane in `5cbbc55`.

### Fixed — the swap explain's carry and realized columns moved, and a partial explain set is now refused (Task F-8, review rounds 2 and 6)

Both defects are in the explain this same release adds, so no number that
existed before 1.1.0 moves. Both are worth reading anyway: the first changes
numbers anyone who took the explain off this branch early has already looked at,
and the second turns a call that used to succeed into an error.

**THE CARRY COLUMN WAS SCALED BY THE WRONG INTERVAL** (`96a3c70`).
`theta_zero_fixing` is a RATE obtained by rolling `bumps.time_years` while
injecting exactly ONE fixing -- independent of the roll's length -- so
`theta_zero_fixing * h` is the deterministic move over `h` INCLUDING that
fixing. The first cut resolved the sensitivities one step early and multiplied
them by the NEXT step's `dt`, scaling the fixing leg by `dt_this / dt_prev`. On
a real calendar a Friday-to-Monday step follows a one-day step and OVER-STATES
carry 3x, with the following step under-stating it by the same ratio. The
residual absorbed all of it, so the identity closed and NAV never moved: it read
exactly like a thing that passed, which is how it cleared five tests, a NAV gate
and a review.

Fixed by DELETING the carried state rather than correcting the arithmetic
applied to it -- the start-of-step sensitivities now resolve against the
snapshot the step is measured FROM, so the interval a greek is bumped over and
the interval it is multiplied by are the same number by construction. Pinned by
SHAPE rather than by value, on a 1/3/1/7/1-day calendar: carry is one fixing
(independent of step length) plus a dt-proportional roll, so it must not track
the step-length ratio. Against the old arithmetic that test reports a 6.97x
spread and fails
(`BacktestSwapExplain.CarryDoesNotScaleWithStepLengthOnAnIrregularCalendar`;
`IdentityHoldsOnAnIrregularCalendar` stays green throughout, which is the point
-- the identity could not see this). Deleting the state also retired a per-lot
entry that leaked for every lot leaving the book by a path other than
settlement, and is what lets a resumed run attribute its first step.

**THE REALIZED COLUMN WAS UNDISCOUNTED** against discounted marks (`96a3c70`);
it now uses the to-date's own discount factor, which the smile sample already
returned.

MIGRATION for both: `swap_pnl`, NAV and every other column are unaffected --
the residual absorbed the error, which is precisely why this needs stating
rather than showing up as a NAV break. A stored `swap_explain_carry` /
`_realized` series from an earlier build of this branch should be recomputed;
on a regular calendar the carry error vanishes, so a run whose steps were all
one day is unchanged.

**A PARTIALLY-POPULATED EXPLAIN SET IS NOW REFUSED, BY NAME** (`c1771a6`). Both
shape validators checked every explain column INDEPENDENTLY (`empty ||
row-parallel`), and every column is independently allowed to be empty -- so a
result with one column populated and the rest empty was a collection of
individually-legal columns forming an illegal SET that nothing asked about. The
append path then decided the set's shape by reading the FIRST column, justified
by a comment asserting the validators had already rejected a partial set. They
had not. Measured: such a result passed `validate()`, passed
`append_backtest_results`, and came out ragged -- carry at 3 rows, skew at 1 --
with the combined result refusing only afterwards, by which point the malformed
result had already escaped.

New public `enum class SwapExplainShape { Absent, Present, Mixed }` and
`swap_explain_shape(const BacktestResult &)` answer that question over EVERY
column, and there is no accessor that answers it from fewer. `Mixed` is the
state that was previously unrepresentable and is the reason the enum has three
values: it is always malformed, both validators reject it by name, and a caller
comparing against `Present` cannot silently treat it as such.

MIGRATION: CALLER-OBSERVABLE. A `BacktestResult::validate()` or
`append_backtest_results` call that used to succeed on a partial explain set now
returns an error naming the shape. A caller assembling a `BacktestResult` by
hand must populate the whole roster or none of it -- `swap_explain_shape` is how
to check before calling. No caller that was populating the columns together, or
leaving them all empty, is affected
(`BacktestSwapExplain.TheShapeAccessorReadsEveryColumnNotTheFirst`,
`AppendRefusesAPartiallyPopulatedExplainRatherThanRaggedIt`).

### Fixed — a zero-surface subset load verifies its date instead of sampling one entry (Task F-8 round 7)

`MarketSnapshot::load` had sample-then-verify and sample-and-trust in two
branches of one function, ten lines apart. The full-load branch reads
`pricing_at(0).now_ts_ns`, checks every loaded surface against it and errors on
disagreement; the other branch mapped the directory's first entry and trusted
it.

THE ASYMMETRY IS NARROWER THAN IT LOOKS, and the shape of the fix follows from
that. The trusting branch is reached only when a subset was requested and
matched NOTHING, so it owns zero surfaces -- a subset that matched anything
falls through to the verifying branch. No loaded surface was ever unverified;
what went unchecked was the DATE the resulting zero-surface snapshot carries.
"Verify across what the subset loaded" is therefore vacuous here, so the branch
now maps every directory entry and requires agreement: the same rule the sibling
applies, over the only set this branch has.

Measured against a two-surface archive stamped a day apart and loaded with a uid
matching neither: pre-fix the zero-surface subset load was ACCEPTED while the
whole-board load of the same archive was refused; post-fix both refuse. Both
agreeing cases still load, and an empty-subset load still yields a zero-surface
snapshot carrying the date's timestamp, which is the legitimate behaviour that
path exists for
(`BacktestSwapExplain.AZeroSurfaceSnapshotRefusesAnArchiveWhoseDatesDisagree`,
`AZeroSurfaceSnapshotStillLoadsWhenTheArchiveAgrees` as its positive control).

MIGRATION: CALLER-OBSERVABLE, and narrowly. An archive whose entries disagree on
their date, loaded through a subset request that matched no uid, now errors
where it previously returned a snapshot timestamped from whichever entry the
directory happened to list first. Any caller that was relying on that
acceptance was relying on an arbitrary choice. The added walk is header reads
over the already-mapped archive rather than I/O, and runs only on that one
branch.

### Fixed — the scenario grid's deriv leg returned NaN cells and differenced a model, not a price (Task F-8 round 1)

Both defects are in the deriv leg this same release adds (the `scenario_grid`
overload above, `1f04a01`), so nothing a pre-1.1.0 caller reads moves -- but
every number that leg produced before `8d2f5de` was affected, on the kinds named
below.

**NaN poisoning, and what a caller now gets instead.** `DerivGreeks` carries
"not computed" as NaN in the slots a caller did not ask for, and `NaN * 0.0` is
NaN, so an unguarded product destroyed EVERY term of a Taylor cell over an axis
the caller never asked about. Only the two smile terms were gated; theta, charm
and vanna were bare multiplies. MEASURED: a contract shorter than the default
roll (`1/365.25`) on a grid whose `dt` is ZERO returned 9 of 9 NaN cells while
reporting `n_deriv_ok = 1`. Every NaN-capable term is now gated on its own
shock, once and in the same form
(`ScenarioGridDeriv.EveryNaNCapableTermIsGatedByItsOwnShock`). The artifact that
stands behind the GENERAL claim is
`ScenarioGridDeriv.ASufficientVerdictAlwaysYieldsAFiniteKernel`, whose slot list
names every `double` member of `DerivGreeks` rather than the ones the kernel
reads today -- a list of what is read today is definitionally unable to catch a
kernel that starts reading something new. Its sibling equality over the shock
combinations is a gate-set agreement check and is NOT an exhaustiveness proof;
the round that added it called it one, and its own comment now names a mutant it
misses.

A NON-ZERO shock against a NaN sensitivity has no answer to give, so the grid
now detects that case BEFORE pricing and EXCLUDES the position, counting it in
a new `ScenarioGridResult::n_deriv_missing_sensitivity`. It is kept separate
from `n_deriv_failed` because the two have different fixes -- a failed solve is
a broken position, this is a grid asking for an axis nothing measured -- and it
is counted rather than contributed as `0.0`, which would read as "measured, and
this position has no exposure". `n_deriv_ok + n_deriv_failed +
n_deriv_missing_sensitivity == deriv_book.size()`, always. The header's promise
that no NaN enters a cell total was prose; a non-finite cell total is now
`ErrorCode::Internal` rather than a NaN matrix stamped `Ok`
(`ScenarioGridDeriv.AContractTooShortToRollIsExcludedAndCountedNotZeroed`,
`AShortContractDoesNotPoisonAGridThatAsksForNoTimeRoll`).

**The Exact cell differenced a change of MODEL.**
`detail::deriv_price_shocked_on_ref` did not pin the centre scheme, so a shocked
reprice re-resolved its grid, re-read its wing band and RE-CALIBRATED the
vol-of-vol; the difference against the base then carried a model change rather
than a price change. This was the third site of a rule `deriv_greeks` and
`deriv_pv_skew_shifted_for_test` already applied. Measured across the kind
space, the Taylor-vs-Exact gap converged at O(h) instead of O(h^3) for VolSwap,
both capped kinds and VariancePut: VolSwap `89.03 -> 44.34 -> 22.13` as the
shock halved twice, and after the pin `0.115 -> 0.0143 -> 0.00178` (ratio 8.02,
a 776x reduction). VarSwap, Corridor and VarianceCall were unaffected and hid it
completely (`ScenarioGridDeriv.TaylorAndExactAgreeAcrossTheKindSpace`).

MIGRATION: none for the option-only overload -- its per-cell output is
byte-identical and none of these fields exist on it. A caller of the deriv
overload that reads `n_deriv_ok` alone must now ALSO read
`n_deriv_missing_sensitivity` to account for the whole book; a grid that
previously returned NaN cells alongside a healthy `n_deriv_ok` now returns
finite cells and a non-zero exclusion count. Every pre-existing greek is
unmoved: the F-8 bit-identity probe byte-matches its pre-task baseline on 6052
of 6084 values, and the 32 that differ are exactly `39c934c`'s own smile-vega
fix (`0.0 -> NaN` on 16 deriv-book memo rows), which this probe independently
confirms landed only there. Landed in `8d2f5de`.

### Added — smile greeks (`skew_vega` / `convexity_vega`) and a per-tenor vega ladder (GK-G1 / GK-G2 / LIT-8, Task F-7)

NO EXISTING NUMBER MOVES. `DerivGreeks` gains `skew_vega` and `convexity_vega`
(arity pin 12 -> 14), `DerivGreekBumps` gains `skew_abs` / `convexity_abs`
(both 1e-3) and `smile_greeks` (arity 7 -> 10), and `DerivPriceFrame` gains
`vega_by_tenor` (arity 5 -> 6).

**Why.** `vega` is a PARALLEL shift, but a variance swap is long every strike:
the strip integrates the whole smile, so a rotation or a steepening moves
`K_var` while the ATM vol sits still. That exposure had no number.

**Convention.** `k = ln(K/F)`, the same coordinate the strip integrates in and
the same one `SurfaceAnalytics::skew_slope` reports in, so bumping `s` shifts
the surface's own `skew_slope` by exactly `+s`. `skew_vega` is `dPV/ds` under
`iv(k,T) -> max(iv + s*k, 1e-4)` and `convexity_vega` is `dPV/dc` under
`iv(k,T) -> max(iv + c*k*k, 1e-4)`, both per 1.00 of coefficient — a large
perturbation, so a desk figure is `skew_vega * 0.01`. `s < 0` is the equity
direction (richer puts), which raises `K_var`, so **`skew_vega < 0` and
`convexity_vega > 0` on a long var swap**. Both signs, and their SCALE, are
pinned against a closed form: on a flat surface the strip's sensitivity density
in `k` is Gaussian with mean `-sigma^2*T/2` and s.d. `sigma*sqrt(T)`, giving
`skew_vega = vega*E[k]` and `convexity_vega = vega*E[k^2]` — matched to 1.9e-6
and 8.8e-7 relative. Note this also means a FLAT surface does not give zero
skew vega: the density is not centred at zero.

**Wing-clamp saturation, a modelling limit to read before using these.** The
strip clamps `k` into the resolved trust band BEFORE calling `iv`, so the shift
a node past the band receives is `s*band`, not `s*k` — the linear term
saturates. With the default certified half-band of 0.5 and a 1Y 20-vol strip
spanning about +-1.2, the outer wings all receive the same shift. These are
therefore sensitivities of the CLAMPED surface the strip actually prices, which
is the honest target (it is the surface `pv` came from) but is NOT an unclamped
analytic smile. Widen `DerivConfig::wing_clamp_k` for the greek call as well as
the mark to change that.

**Cost.** OFF BY DEFAULT. Per contract, 4 extra repricings, 16 -> 20 — and the
maximal default count really is 16, not the "7 / 13 / 17" three doc comments
claimed after Task P-2 removed the FD rate bump without re-counting; all three
were corrected and are now pinned by a measured test rather than restated in
prose. ON A BOOK the flag ALSO makes a VarSwap row ineligible for the P-6
per-(uid,T) strip memo, since the shared block carries no smile slots: measured
13 -> 200 strip evaluations on a 10-row single-tenor book, a 15.4x step, not
the ~25% "+4 repricings" suggests. That trade is deliberate (the alternative was
a second, independently maintained smile implementation whose bit-identity
nothing would have checked) and is documented on `price_deriv_book` itself.

**`vega_by_tenor`** is `totals.vega` split by `contract.maturity_t`, keyed by
the raw maturity so `std::map`'s ordering is the ladder. A net vega hides the
commonest real position — long front, short back, flat overall. It costs no
extra repricing, is EMPTY (never a map of zeros) on a marks-only call, and
NaN-poisons only its own bucket.

MIGRATION: none. Every pre-existing greek is bit-identical with the flag on and
off, which also covers the bump-cache recording-mode change the smile reads
required.

### Added — the single-name return convention: `RealizedTracker::set_dividend_adjustment` and a three-argument `observe_dated` (PV feature list / LIT-9, Task F-6)

`RealizedVarianceSpec::include_dividend_adjustment` shipped from the C port
commented "reserved; unused in this port", and it had NO writer anywhere in the
tree: `RealizedTracker::create`/`create_corridor` build `rv_` from
`annualization` and `n_obs_total` alone, so nothing public could set it and the
field's own "when the flag is set" precondition was unreachable. It now names
the ISDA/MCA SINGLE-NAME return convention `r_i = ln(S_i / (S_{i-1} - D_i))` --
the prior close reduced by the cash dividend going ex on day i -- so a stock's
mechanical ex-div drop is not booked as realized variance. `false`, still the
default, remains the INDEX convention this accumulator has always computed.

* INDEX LEGS ARE BIT-FOR-BIT UNCHANGED, by construction rather than by
  tolerance. One private `observe_impl(spot, ex_div_cash)` now forms every
  return, and the only line a dividend moves is its denominator,
  `prev_spot_ - ex_div_cash`; every unadjusted path passes `0.0`, so that
  expression IS the `prev_spot_` the line always used. The two-argument
  `observe_dated` forwards with `0.0` rather than duplicating the guard, and
  keeps validating the timestamp FIRST, so a stale replay still mutates
  nothing. Beyond that, no pre-existing tracker can even BE
  dividend-adjusted: the flag had no writer before this task.
* NO ARITY PIN MOVES. No field was appended -- the field already existed, it
  merely became reachable -- and `SwapAccrual::operator==` already compared it.
* TWO REAL OVERLOADS, never a defaulted third argument. A default would make
  every existing two-argument `observe_dated` call ambiguous against the new
  entry, and folding the two declarations into one defaulted signature would
  change an already-published signature under the v1.x additive-only freeze.
* NEW REFUSALS, ALL UNREACHABLE WITHOUT FIRST CALLING THE NEW SETTER, so no
  call that was correct before this release returns an error now. On a
  dividend-adjusted tracker the UNDATED `observe(double)` is `InvalidArgument`
  outright, and `observe_batch` inherits that through its own loop: that entry
  has no channel to carry `D`, so it would accrue INDEX-convention returns
  under a snapshot advertising the single-name one. MIGRATION for anyone
  opting in: drive a single-name leg through the three-argument
  `observe_dated` only, passing `0.0` on the days with no dividend.
  `set_dividend_adjustment` itself returns `InvalidArgument` once ANYTHING has
  been observed (`have_prev()`, which the seeding call sets) -- configure,
  then accumulate. A mid-stream flip would leave `Sigma r^2` an undecomposable
  mix of two conventions under a snapshot carrying ONE flag for all of it.
* A DIVIDEND THAT WOULD DO NOTHING IS REFUSED, NOT DROPPED -- the rule
  `cap_dec` and the corridor bounds already follow. `ex_div_cash` is
  `InvalidArgument` when it is negative or non-finite (tested as
  `!(x >= 0.0)`, so a NaN is caught at the boundary instead of poisoning every
  accumulator downstream), when it is positive on a tracker that is NOT
  dividend-adjusted, when it is positive on the SEEDING observation (which
  forms no return for it to adjust), when it is not strictly below the
  previous close (which would make the adjusted denominator zero or negative
  and the return undefined), and when it exceeds the OBSERVED CLOSE `spot`
  (the transposition rule below). Silently dropping `D` instead would hand back
  `ln(94/100)` to a caller who asked for `ln(94/95)` -- the exact failure this
  task exists to remove.
* THE TRANSPOSED CALL IS REFUSED, and the argument is forced rather than
  empirical. `spot` and `ex_div_cash` are both `double`, so
  `observe_dated(ts, div, spot)` compiles silently and, unguarded, booked a
  ~1900x wrong variance and left `prev_spot()` poisoned for every later fixing.
  Two rules close it. On an INDEX tracker -- the default, and every caller who
  never calls `set_dividend_adjustment` -- the swapped call puts a positive
  value in `ex_div_cash`, which the rule above already rejects outright. On a
  dividend-adjusted tracker, `ex_div_cash > spot` is the fifth refusal listed
  above. Together they cover every fixing this API accepts: acceptance requires
  `D <= S`, so both orderings of a pair being acceptable would need `D <= S` and
  `S <= D`, i.e. `S == D`, in which case the two orderings are the SAME call.
  `RealizedTracker.TransposedFixingIsRefusedWheneverTheOriginalIsAccepted`
  sweeps that property rather than sampling a pair. THE RESIDUE, which is not
  nothing: a fixing this API ALREADY REJECTS can transpose into an accepted one
  (intending `S = 40, D = 50` is refused; typing it as `(50, 40)` is accepted).
  No correct-and-accepted fixing is at risk. ACCEPTED COST of the fifth
  refusal, so a caller meeting it is not surprised: a LIQUIDATING DISTRIBUTION
  larger than the residual price is refused even though it is a real corporate
  action -- such a fixing has no agreed return in this convention anyway, and a
  leg carrying one needs handling above this class.
* THREE RULINGS THE SPEC LEFT OPEN, decided at the site and pinned. The
  gamma-swap weight keeps reading the RAW just-observed close: Lee's `y` is the
  price LEVEL at which the variance is earned, and on an ex-div day that level
  IS the post-drop close. The corridor indicator keeps testing the RAW previous
  close, because a corridor is a barrier in TRADED PRICE space while the
  adjustment is a return-construction device with no business restating where
  the stock traded (`Corridor.TrackerCountsTheRawPreviousCloseUnderADividend`).
  And `prev_spot_` stores the RAW close, so `prev_spot()` stays a truthful
  report of the last close and the same dividend is not charged a second time
  to TOMORROW's return. Corridor x single-name is a real product, reachable
  here by composition rather than by a second factory, and is pinned.
* A MODE SETTER, NOT A `create_single_name` FACTORY. `create_corridor` is a
  factory because it VALIDATES; a boolean convention has no invariant to
  validate, so that rationale is absent, and a factory per mode would multiply
  against the corridor entry. The one thing a factory would have bought --
  immutability for the accumulating lifetime -- is bought by the refusal above.
* NOT WIRED TO THE BACKTEST. The swap lane's fixing driver
  (`observe_swap_fixing`, backtest.cpp) is a separate transcription of this
  arithmetic that passes no dividend at all, and it stays on the index
  convention until a corporate-actions feed exists to source `D` from.
  `FinancingConfig::share_dividends` (backtest.hpp) is the OPTION lane's
  hedge-share cash ledger and is not that feed. README's standing claim that
  the backtest calls `RealizedTracker::observe_dated` was wrong and was
  corrected in the same commit -- it does not.

### Added — options on realized variance: `DerivKind::VarianceCall` / `VariancePut` (PV-F5 / LIT-5, Task F-5)

Two new enumerators (7 and 8) price a European call/put on the SAME blended
realized variance `V = a + b*W` the capped kinds already price.
`DerivContract::strike_dec` is read as the OPTION strike in annualized decimal
variance; `cap_dec` names nothing and is rejected as on every other uncapped
kind. Engines: `Auto` or `RvDistributionProxy`. Greeks come through
`deriv_price` dispatch (finite differences), so a mark and its greeks cannot
come from two different engines.

* MOVES NO EXISTING NUMBER. Purely additive: new enumerators, one new pricer,
  one new `detail::lognormal_put` beside the existing `lognormal_call`. No
  struct gained a field, so no arity pin moved.
* `DerivQuote::fair_strike_dec` MEANS SOMETHING DIFFERENT ON THESE TWO KINDS,
  and it is the one thing a caller must read before using them: it is the
  option's undiscounted PREMIUM, and `pv == df * notional * fair_strike_dec`
  with NO strike subtraction, because K is already inside the payoff. No strike
  prices an option to PV = 0, so a break-even level does not exist here. E[V]
  stays readable as `accrued_component_dec + future_component_dec` on every path
  that resolved a future leg — but NOT on the put-pin path below.
* NEW `DerivFlags::OptionPinned = 1u << 16` (appended; bit 16 was free, the
  previous high bit being `CalendarInconsistent = 1u << 15`). Set on a variance
  PUT whose accrued leg alone already reached its strike (`a >= K`), where
  `V >= a >= K` makes the payoff identically 0 — deterministic, no strip, valid
  at `T == 0`. It is the option analogue of `CapPinned`, which is deliberately
  NOT reused: that flag is documented as always accompanied by `CapApplied` and
  names a cap these kinds do not carry. Never set on a CALL — `a >= K` makes
  exercise certain but leaves the value `a + b*m - K`, which still needs the
  strip. The flag exists because a pinned put and a genuinely near-worthless one
  both quote ~0, and "dead by accrual" vs "cheap by model" are different facts
  for anything marking or risking the position. It also marks the ONE path where
  `E[V] == accrued_component_dec + future_component_dec` fails: no strip ran, so
  the future leg is 0 in this library's standing "0.0 means NOT COMPUTED" sense
  while `b > 0`.
* NOT CARRYABLE BY THE BACKTEST ENGINE. `engine_supports_swap_kind` refuses
  both kinds. Marking would have worked; SETTLEMENT would not — the swap-lot
  settle path pays `qty * notional * (terminal - strike_dec)`, linear in the
  terminal rate, and an option's payoff is kinked. Admitting them without
  teaching that path the kink would have paid a short option position a profit
  it never had on every path where it expired worthless.
* MODEL RISK, PUBLISHED (LIT-5). Realized variance has a right tail materially
  fatter than lognormal, so this engine UNDERPRICES out-of-the-money variance
  CALLS, with the error growing in moneyness. It is a model choice, not a
  calibration artifact: xi is calibrated to the surface's Carr-Lee convexity, a
  sqrt-moment, which pins the middle of the distribution and says nothing about
  its tail. `DerivEngine::RvDistributionAffine` / `McQe` remain reserved as the
  escape hatch. Treat a far-OTM variance-call mark from this library as a LOWER
  BOUND. See `DerivKind::VarianceCall`'s header doc for the full statement.

### Changed — `DerivMarkingConvention::CboeVarianceFuture` now fails loud instead of pricing as OTC (Task F-5)

`DerivContract::marking` shipped as a field **no executable code read**. A
contract carrying `DerivMarkingConvention::CboeVarianceFuture` fell straight
through `validate_deriv_dispatch` into the parametric OTC strip and returned a
successful `DerivQuote` — a confident OTC number for a listed-variance
convention. The header documented the gap in prose ("DECLARED, UNENFORCED")
and nothing enforced it.

* NOW: `validate_deriv_dispatch` returns `ErrorCode::NotImplemented`
  ("reserved marking convention") for any `marking != Otc`, on **every**
  `DerivKind` (the field is not kind-scoped — no kind reads it) and through
  **both** lanes, because `deriv_price` and the P-6 book-memo lane call that
  one validator. On the book lane the row's `PriceStatus` is `NumericError`,
  the same bucket every other reserved value already lands in
  (`status_for`, deriv_book.cpp, sends everything but `InvalidArgument` there).
* MIGRATION: none for anyone whose quotes were right. `marking` defaults to
  `Otc`, every construction site in this repo and every test assigns only
  `Otc`, so no quote that was correct before changes value. A caller who WAS
  setting `CboeVarianceFuture` was receiving an OTC mark under a listed label;
  they now get an error instead of a wrong number. There is no
  behaviour-preserving migration for that case by design — the old behaviour
  was the defect.
* `NotImplemented` rather than `InvalidArgument`: the contract is well formed
  and the enumerator is a legal value the library reserves, matching
  `DerivEngine::RvDistributionAffine`/`McQe` and
  `DerivDiscreteCorrection::FullMc`.

### Added — `cboe_var_strike` / `cboe_parametric_basis`: the Cboe discrete-strike variance strip, new header `atx/vol/cboe_strip.hpp` (PV-F2 / LIT-1, Task F-9)

Everywhere else this library prices variance off a FITTED surface, quadratured
on a synthetic log-strike grid. A LISTED variance future does not settle that
way: it settles against the exchange's own finite sum over the strikes actually
quoted at the settlement snapshot, with the exchange's own strike spacing, its
own out-of-the-money selection and its own quote-exclusion rules. Before this
there was no way to compute that second number, so there was no way to measure
the BASIS a desk hedging OTC variance with listed variance actually carries
(PV-F2). `cboe_var_strike` computes the listed number; `cboe_parametric_basis`
reports the gap against a parametric strike in both variance and vol units.

* NEW PUBLIC HEADER `atx/vol/cboe_strip.hpp`, Tier-B: `vol.hpp` does not
  include it and no Tier-A header does, and it depends only on `types.hpp`
  plus `detail/aggregate_arity.hpp`. It took the filesystem-counted Tier-B
  total 31 -> 32; the README table and
  `VolUmbrella.TierCountsMatchTheReadmeTable` followed one commit later in
  `1e0b708`, which is the update procedure that pin asks for.
* IT SHARES NO CODE WITH THE PARAMETRIC STRIP, deliberately, so a basis
  measured between the two is a MEASUREMENT rather than a tautology. Same
  reason `parametric_var_dec` is passed IN rather than computed here: the
  parametric side is a template over surface type carrying a whole
  wing/quality/corridor policy with it, and binding one instantiation into
  this module would make the basis a statement about that policy rather than
  about the board. `CboeVarStrip::var_strike_dec` is in the SAME units as
  `DerivQuote::var_strike_dec` (0.04 <-> 20 vol), which is what makes the two
  directly differenceable at all.
* NEW AGGREGATES, all arity-pinned AT BIRTH rather than retrofitted:
  `CboeStrikeQuote` (5), `CboeStripTerm` (5), `CboeVarStrip` (12) and
  `CboeParametricBasis` (5), plus `enum class CboeStripLeg`.
  `CboeVarStrip::terms` retains every published intermediate -- each `dK_i`,
  each `Q(K_i)`, and which leg supplied it -- so a reader AUDITS the sum
  instead of re-deriving it.
* A NON-CALCULABLE BOARD RETURNS `ErrorCode::Unavailable`, NOT A NUMBER. The
  methodology enumerates two such conditions and both are implemented: a K0
  leg that is null (`0.00/0.00`) or that carries a bid above its ask with no
  offer (`0.30/0.00`), and an empty out-of-the-money wing on either side (a
  one-sided strip is not a cheaper answer, it is half the log contract).
  `Unavailable` rather than `InvalidArgument` on purpose -- the board is well
  formed and the caller did nothing wrong; this is the SNAPSHOT saying it
  admits no index, which the exchange itself handles by republishing the last
  valid value. WHAT A CALLER MUST DO WITH IT: treat `Unavailable` as "this
  snapshot yields no settlement value" and carry the previous one forward;
  treat `InvalidArgument` as a defect in the board it passed in. A caller that
  collapses the two cannot implement the republish behaviour.
* THE SPLIT AGAINST MALFORMED INPUT is deliberate and tested in both
  directions. A K0 leg crossed with BOTH sides quoted (`0.30/0.20`) is
  `InvalidArgument`, rejected board-wide before K0 is even resolved. A K0 leg
  quoted `0.00/0.30` -- no bid but a REAL offer -- is neither null nor
  bid-above-ask, so it is SERVED, contributing its genuine 0.15 midpoint. That
  last case is the ONE question the source leaves open; it is recorded in the
  header as open rather than answered, with `check_k0_quotable`
  (cboe_strip.cpp) named as the single function to change if Cboe resolves it
  the other way.
* TWO PLACES THE PUBLISHED TEXT NEEDED A DECISION, both cited against the
  current methodology and pinned by test rather than left re-derivable:
  `K0 = max{K : K <= F}`, the inclusive reading (the 2009 edition said strictly
  "below F" and the 2019 edition is internally inconsistent, which is how two
  careful readers reach opposite conclusions -- the two readings differ by 5
  vol points on this module's own fixture, so the tie is not negligible); and
  `dK` resolved over the SELECTED strip's neighbours, not the board's, since a
  midpoint rule exists so the widths tile the strike axis and only the
  surviving strikes still tile it after exclusion.
* `zero_quote_truncated_low` / `_high` are TRUE only when the
  two-consecutive-zero-quote stop actually REFUSED a listed strike that was
  still there -- not merely when the walk ran off the end of the board.
  Defined the other way the flags would read TRUE constantly on an illiquid
  wing and mean nothing; as defined, TRUE is a statement about QUOTE QUALITY,
  while a short strip with the flag FALSE is one about LISTING COVERAGE.
* `diagnostic_out` is ASSIGNED ON EVERY RETURN PATH, success and every
  failure, so the caller who most needs the audit trail -- the one whose board
  was refused -- can still read how far resolution got. Same channel
  convention as `forward_var_fair_strike`'s own out-parameter below; paths
  that failed before resolving anything leave it default-constructed.
* THIS IS NOT THE PUBLISHED INDEX. It is a SINGLE-EXPIRY variance strike. The
  two-expiry interpolation onto a fixed 30-day horizon and the scaling by 100
  are an index construction rather than a settlement primitive and are not
  here; the test performs that composition itself against Cboe's published
  worked example.
* NO EXISTING NUMBER MOVES. Nothing in the library calls this module and it
  calls nothing -- a new header, four new aggregates plus an enum, and two new
  entry points, with no ledger counter and no Python binding. It is NOT the
  destination of the `### Changed` entry above: a contract marked
  `DerivMarkingConvention::CboeVarianceFuture` is REFUSED by
  `validate_deriv_dispatch`, not re-routed here. Wiring the marking convention
  to this module is separate, future work.

### Added — `forward_var_fair_strike`: forward-start variance + calendar diagnostic (PV-F4 / FIT-F2 / LIT-7, Task F-4)

New entry `forward_var_fair_strike` (a `PricedSurface`-native overload plus a
templated sibling over the usual `SurfaceT` set) prices the forward-start
variance strike between two tenors from total-variance additivity:
`K_fwd = (K_var(T2)*T2 - K_var(T1)*T1)/(T2 - T1)`, with each leg the FULL-SMILE
model-free strip this file already prices.

* ONE CONVENTION IS NOW CANONICAL. `atx::vol::forward_vol` (analytics.hpp)
  derives forward variance from the ATMF total variance ALONE, which is a
  different number on any surface with skew, and it returned a bare NaN on an
  inverted term structure -- indistinguishable from its NaN for a bad argument.
  It is NOT removed (1.x is additive-only) and stays correct for what it is,
  the ATM term-structure DIAGNOSTIC behind `SurfaceAnalytics::
  forward_vol_segments`; its declaration now says so and points at the new
  entry as the pricing convention.
* BOTH LEGS ARE PRICED UNDER ONE POLICY RESOLUTION -- one `DerivConfig` object
  and one certified-wing-band argument reach both strips, so wing mode, wing
  trust band and `width_sigmas` cannot differ between them. Only the GRID is
  per-tenor (each leg's adaptive span and node budget follow its own
  `sigma_atm*sqrt(T)`), which is what makes the difference meaningful.
* NEW `DerivFlags::CalendarInconsistent = 1u << 15` (appended; bit 15 was free,
  the previous high bit being `WingExtrapolated = 1u << 14`). A T2 strip
  pricing LESS total variance than the T1 strip, by more than the two legs'
  combined accuracy, returns `ErrorCode::Internal` with this flag. Because an
  `Err` carries no `DerivQuote`, the flag is delivered through the entry's
  `diagnostic_out` out-parameter, which is assigned on EVERY return path; a
  caller passing `nullptr` gets the status and no flag. The error MESSAGE is
  not a channel.
* THE DETECTOR'S DEAD BAND IS ANCHORED TO THE LIBRARY'S OWN CALENDAR ACCURACY
  FLOOR, not to a new magic number: `2 * kCalendarTotalVarianceTol` plus both
  legs' MEASURED `integration_error_est` (times their tenors). A surface merely
  at the limit of fit precision is served as `K_fwd == 0.0` with no flag; a
  genuinely arbitrageable one fails loud. Measured at High tier on the
  `CalendarDetectorRespectsTheFitAccuracyFloor` fixture: dead band 2.00016e-07
  in total variance, of which the two quadrature terms are 1.63522e-11
  (8.17611e-12 from each leg).
* NEW EXPORTED CONSTANT `atx::vol::kCalendarTotalVarianceTol` (types.hpp) =
  1.0e-7 in total-variance units, THE calendar tolerance. It previously existed
  as SIX hand-kept copies of that literal (arb.cpp x2, projection.cpp,
  vol_curve.cpp, spline_curve.cpp -- two of them carrying comments asking the
  reader to keep them in sync -- plus `ConvexRepairSpec::tolerance` in
  vol_curve.hpp, which feeds the SAME variable as vol_curve.cpp's copy through
  the other branch of one ternary). All six now name the constant. It lives in
  `types.hpp` rather than `arb.hpp` because `arb.hpp` includes `vol_curve.hpp`,
  so the sixth site could not have named it there. The value is unchanged, so
  this is bit-identical by construction; what changes is that it can no longer
  drift between the fit-side checks, the calendar projectors, the ConvexDense
  repair loop and this new detector.
* `ConvexRepairSpec`'s OTHER THREE defaults (`k_min`, `k_max`, `grid_points`)
  were the same shape: hand-kept duplicates of `kRiskCalendarMin` /
  `kRiskCalendarMax` / `kRiskCalendarIntervals`, which lived as literals in
  vol_curve.cpp's anonymous namespace. All three are now
  `atx::vol::kConvexCalendarLatticeKMin` / `...KMax` / `...Intervals`
  (vol_curve.hpp), named by BOTH the spec defaults and the `nullopt` scan.
  `VolCurve.ConvexRepairSpecDefaultsAreTheNullOptLattice` pins the four
  compile-time identities AND that the two ternary branches produce the same
  served curve on a fit carrying a sub-tolerance calendar crossing. Values
  unchanged throughout.
* NEW EXPORTED CONSTANT `atx::vol::kFwdVarNoiseCeilingVar` = 1.0e-3 decimal
  variance. `K_fwd` differences two nearly-equal totals and divides by a small
  number, so the dead band above is amplified by `1/(T2 - T1)`; the entry
  returns `OutOfRange` when that amplified floor passes this ceiling rather
  than serving a number whose leading digits are error. Measured boundary at
  Audit tier: `T2 - T1` below 2.0e-4 years (~1.75 hours) refuses.
* `DerivQuote` gains `leg_T1_var_dec` / `leg_T2_var_dec` (arity pin 17 -> 19),
  each carrying its leg's `K_var` bit-identically. `accrued_component_dec` /
  `future_component_dec` are deliberately NOT reused: they mean "realized leg"
  and "implied leg of an aged blend" everywhere else, and a forward-start
  strike has no accrued leg. NaN, not 0, on every quote no forward-start entry
  produced. Grid-provenance fields report the T2 leg's grid (the two legs
  resolve different grids by design).
* NO EXISTING NUMBER MOVES. This is an additive entry plus two appended quote
  fields with a NaN default and one appended flag bit; the five tolerance sites
  above keep the same value.

### Added — `DerivKind::CorridorVarSwap`: corridor variance swaps (PV-F3 / LIT-7, Task F-3)

New `DerivKind::CorridorVarSwap = 6` (appended, additive-only) prices the
model-free variance strip with the replicating weight `1{K in C}/K^2`. Same
per-node integrand and same `2/T` outer scale as `VarSwap`; the ONE difference
is the integration window, restricted to
`[ln(corridor_lo/F), ln(corridor_hi/F)]` intersected with the resolved span, so
the corridor edges are composite-Simpson PANEL BOUNDARIES (C-3's split
machinery, invoked on the restricted interval -- not reimplemented).

* `DerivContract` gains `corridor_lo` / `corridor_hi` (APPENDED; arity pin
  added at 9 -- this struct had none before). ABSOLUTE STRIKES, with `0`
  meaning UNBOUNDED on that side, so `0/0` reproduces `VarSwap` on the same
  nodes with the same weights (`Corridor.FullCorridorIdentity`, pinned
  BIT-EXACT plus a ledger-counter dispatch witness). Non-zero on any other
  kind is `InvalidArgument`, exactly as `cap_dec` already behaves -- a knob
  that names nothing on a kind is a caller error, not a silent no-op. Enforced
  in `validate_deriv_dispatch`, the ONE dispatch validator that BOTH the
  `deriv_price` lane and the P-6 book-memo lane call. Review fix round 1 (C-1)
  found this rule had gone into what was then a hand-synchronised SECOND copy
  of that validation, so the memo lane silently priced such a contract at 2.56x
  the corridored value while the other lane rejected it; the two copies are now
  one function, which is what makes the "exactly as `cap_dec` behaves" claim
  true on every lane rather than only on the one it was written against
  (`DerivBook.CorridorBoundsOnAVarSwapAreRejectedOnBothLanes`).
* THE CORRIDOR IS RE-RESOLVED PER PRICING against that pricing's own forward,
  not frozen at inception: a corridor is a fixed barrier in price space.
  Consequence: for this kind `DerivQuote::strip_k_lo_used`/`strip_k_hi_used`
  report the PRE-corridor span, because their contract is reproduction and a
  replaying caller re-derives the corridor itself. Reporting the window would
  make `deriv_greeks`' pinned grid clip the corridor on one side only under a
  spot bump and lose roughly half the edge's contribution to delta.
* `RealizedVarianceSpec` gains THREE appended fields (arity pin 9 -> 12):
  `n_obs_in_corridor`, `sum_sq_log_returns_in_corridor`,
  `rv_corridor_done_dec`. REALIZED-LEG CONVENTION: a fixing counts iff its
  PREVIOUS CLOSE `S_{i-1}` lies in the corridor (closed both ends) -- a
  predictable indicator, deliberately NOT the gamma weight's own
  spot-at-the-return convention. `RealizedTracker::create_corridor` applies it.
* `DerivQuote` gains `conditional_corridor_var_dec` (arity pin 16 -> 17): the
  realized corridor variance normalized by the IN-CORRIDOR count instead of the
  observed count. A quote field, not a second `DerivKind` -- both numbers come
  from one accrual. It is a REALIZED quantity, NOT a forward-looking
  conditional strike (that needs an expected occupation time, which no
  single-expiry strip replicates). NaN, not 0, when nothing has been inside.
* `DerivDiscreteCorrection::Diffusion1OverN` is REJECTED (`NotImplemented`) on
  this kind: Broadie-Jain's addend is derived for the plain, always-counting
  estimator and has no re-derivation for an indicator-gated one.
* `DerivGreekMethod::AnalyticStrip` is NOT extended -- the closed form
  differentiates the full-span strip and has no term for a moving integration
  boundary, so it falls back to finite differences (bit-identical greeks,
  `Corridor.AnalyticStripMethodFallsBackToFiniteDifference`).
* New ledger counter `counters::ledger::Solve::CorridorVarSwapStripEvals`,
  separate from `VarSwapStripEvals` for the same reason `GammaSwapStripEvals`
  is: P-6's book-memo gate reads `VarSwapStripEvals` specifically. The Python
  `SOLVE_LEDGER_KEYS` tuple grows 10 -> 11.
* NOT admitted to the live backtest engine nor to `solve_cycle_swap`:
  `SwapLot` carries no corridor bounds, so a solved lot would silently be an
  UNBOUNDED corridor struck at the all-strike `K_var`. The engine's admission
  list now lives in `backtest.hpp` as `engine_supports_swap_kind` (exhaustive
  over `DerivKind`, so `-Wswitch -WX` forces a future kind to be explicitly
  admitted or refused), and `backtest.cpp`'s `valid_deriv_kind` plus the
  STRATEGY layer's swap-leg spec validation both call it rather than each
  listing kinds. Review fix round 1 (I-3): `validate_restrike_spec`
  (strategy.cpp) never checked `leg.kind` at all, so an engine-unsupported kind
  reached `solve_cycle_swap` and -- for CorridorVarSwap specifically, because
  F-3 added a refusal there -- was folded into `++skipped_swap_cycles_`,
  completing the run at exit 0 with the swap lane silently absent, where a
  GammaSwap leg failed the whole run. Both now fail identically and loudly at
  spec validation, where the caller wrote the mistake
  (`StrategyRestrikeValidation.RejectsEngineUnsupportedSwapKinds`).
* `SwapAccrual::operator==` (backtest.hpp) now compares ALL TWELVE
  `RealizedVarianceSpec` fields, up from six of nine. Provably inert today
  (the checkpoint format refuses the swap lane, so every appended field is 0 on
  both sides), but it retires a correctness argument that rested on another
  file's kind whitelist.

No existing mark moves: every appended field defaults to its no-op value, and
the corridor intersection is `fmax(x, -inf)` / `fmin(x, +inf)` -- exactly the
identity -- on every non-corridor path.

### Added — `DerivKind::GammaSwap`: gamma (weighted-variance) swaps (PV-F1 / LIT-7, Task F-2)

New `DerivKind::GammaSwap = 5` (appended, additive-only) prices Lee's
weighted-variance swap (weight `w(y) = y/Y0`) through the SAME model-free
OTM-strip machinery `VarSwap` already uses -- identical grid/span/wing-clamp/
kink resolution (`strip_fair_value_core`, derivatives.cpp, factored out of
what used to be `var_swap_fair_strike`'s own body; the VarSwap path through it
is bit-for-bit unchanged). The two kinds differ in exactly two places: the
per-node integrand (VarSwap divides by `K`; GammaSwap's `1/K` weight cancels
against the log-strike Jacobian, leaving the raw undiscounted OTM price), and
the outer scale (`2/T` for VarSwap; `2/(T*S0)` for GammaSwap).

* Fair strike, aged blend, PV, and finite-difference greeks dispatch through
  the SAME functions VarSwap uses -- `deriv_price`/`deriv_greeks`/
  `price_deriv_book`/`solve_cycle_swap` -- no new public entry point. This is
  a dispatch-STRUCTURE claim only (m-8, review fix round 2): the aged blend
  and carry-theta injection are NOT numerically "exactly like VarSwap" once
  `gamma_seed_spot` differs from `curves.spot` -- see the anchor-rescale
  bullets below, which VarSwap's own dispatch never needs (its future leg
  carries no S/S0 weight to anchor).
* `RealizedVarianceSpec` gains two appended fields (arity pin raised, newly
  established at 8 -- this is the first time the struct has grown since v1.0):
  `sum_weighted_sq_log_returns_done` (raw running `Sigma (S_i/S0)*r_i^2`) and
  `rv_gamma_done_dec` (its annualized decimal form). `RealizedTracker::observe`
  populates both alongside the pre-existing plain accumulator, anchoring `S0`
  at the tracker's own seed spot.
* **Review fix round 1, C-1 (Critical):** the aged blend mixed two anchors --
  `rv_gamma_done_dec` at the tracker's SEED spot, the future leg at TODAY's
  `curves.spot` -- silently, with no error and no flag (measured: 15.43% of
  the strike, $4,800 PV on a mid-life 1e6-notional contract). Arity raised
  again to 9: a third appended field, `gamma_seed_spot`, carries the anchor so
  `price_gamma_swap` can rescale the future leg by `curves.spot /
  gamma_seed_spot` before blending; a mid-life contract missing the anchor now
  fails loud (`InvalidArgument`) instead of pricing silently
  (`GammaSwap.AgedBlendRescalesFutureLegOntoAccrualAnchor`,
  `GammaSwap.AgedBlendFailsLoudWithoutSeedSpotAnchor`).
* **Review fix round 1, C-2 (Critical):** `inject_carry_fixing` (the
  carry-theta / theta-zero-fixing finite-difference stencil) updated only the
  plain accumulator, never `rv_gamma_done_dec` -- `price_gamma_swap` reads the
  latter, so `theta_carry` and `theta_zero_fixing` were bitwise IDENTICAL on
  every scheduled gamma swap (this file's own "must differ" contract),
  wrong by 305x/364x with a sign flip. Now updates both accumulators
  unconditionally (a VarSwap contract never reads the gamma one back, so this
  is a no-op for it), and auto-anchors `gamma_seed_spot` at the injection's own
  `curves.spot` exactly when the injected fixing is a contract's first-ever
  observation (`n_obs_done == 0` before injection -- the shape
  `solve_cycle_swap` produces) (`GammaSwap.CarryThetaDiffersFromZeroFixing
  ThetaMidLife`, `GammaSwap.CarryThetaFiniteAndDiffersAtZeroObservationsDone`).
* **Review fix round 2, C-3/C-4 (Critical):** round 1 closed C-1/C-2 on the
  `genuinely_mixing` REGIME predicate (`0 < n_obs_done < n_obs_total`)
  instead of the anchor INVARIANT itself, and the same defect resurfaced in
  the regimes that predicate excludes, at LARGER magnitude. C-3: a
  tracker-seeded contract with `n_obs_done == 0` (the state between the seed
  `observe()` call and the first return fixing) has a live `gamma_seed_spot`
  that the old predicate, being false there, never inspected -- silent error
  up to 33.3% of the strike / $20,000 on a 1e6-notional contract at spot 150
  (`w_future == 1.0` here, larger than C-1's 0.6), `theta_carry` wrong by
  4.13e9x. C-4: `inject_carry_fixing` added the injected fixing (anchored at
  today's spot) directly into `rv_gamma_done_dec` (seed-anchored) with no
  rescale -- `theta_carry` sign-flipped and ~8.4e7x wrong whenever
  `gamma_seed_spot != curves.spot`. Fixed by stating the anchor invariant
  once (`gamma_anchor_rescale`, derivatives.cpp) and routing both the blend
  and the injection through it UNCONDITIONALLY -- rescale whenever an anchor
  exists, never gated on `n_obs_done`
  (`GammaSwap.AgedBlendRescalesUnaccruedAnchorAtZeroObservationsDone`,
  `GammaSwap.CarryThetaRescalesInjectedFixingOntoSeedAnchor`).
* **Review cleanup round, m-9/m-12/m-13 (Minor):** `inject_carry_fixing`'s
  anchor check disagreed with `price_gamma_swap`'s two (`> 0.0` alone,
  admitting `+Inf`, whose rescale factor divides to 0.0 and silently zeroes
  the injected fixing) -- named the shared predicate once
  (`gamma_anchor_valid`, alongside `gamma_anchor_rescale`) and routed all
  three sites through it; moves only the `+Inf`/non-finite-anchor path, no
  other number changes. **Undisclosed behaviour change from fix round 2,
  disclosed here (m-12):** `n_obs_total == 0` with a positive
  `gamma_seed_spot` now ALSO rescales the future leg (previously returned it
  raw, matching `n_obs_total == 0`'s own "fully unaged, no accrual concept"
  reading) -- e.g. `done=0 tot=0 anchor=100` at spot 120 now prices
  `0.048000000469519313` where `24d0342` priced `0.040000000391266097`. This
  follows from stating the rescale condition on anchor existence rather than
  on a regime predicate (the same fix that closed C-3), is unreachable
  through `RealizedTracker` (`create` rejects `n_obs_total == 0`, so only a
  hand-built spec can reach it), and is arguably more correct -- but it is a
  numeric change on a path fix round 2's own change list did not name.
  `DerivQuote::uncapped_var_dec`'s doc (`derivatives.hpp`) now states its
  GammaSwap meaning explicitly (m-13): unlike `fair_strike_dec`/
  `undiscounted_expectation_dec`/`future_component_dec`, it is the RAW,
  today-anchored future leg, never rescaled -- combining it with any of the
  other three on a GammaSwap quote mixes anchors.
* `DerivGreekMethod::AnalyticStrip` is NOT extended to `GammaSwap` -- P-4's
  scope predicate (`kind == DerivKind::VarSwap`, a whitelist) already excludes
  it, so a caller requesting the closed form on a gamma swap silently falls
  back to `FiniteDifference`, bit-identical to requesting it explicitly
  (`GammaSwap.AnalyticStripMethodFallsBackToFiniteDifference`).
* `DerivDiscreteCorrection::Diffusion1OverN` is REJECTED (`NotImplemented`) on
  `GammaSwap`: Broadie-Jain's addend is derived for the plain realized-
  variance estimator, and there is no re-derivation for the `S_i/S0`-weighted
  one in this task's scope. Rejected loudly rather than silently ignored or
  misapplied.
* EXACTNESS CAVEAT: the single-expiry OTM-strip replication this ships is
  EXACT only under zero carry (`r == q`) -- under `r - q != 0` the true
  weighted-variance hedge needs a continuum of expiries (Lee, "Weighted
  Variance Swaps," EQF), out of this task's scope. The shipped strike is a
  FIRST-ORDER-IN-`(r-q)*T` approximation to the true expectation: for a flat
  surface, `K_gamma_shipped = sigma^2 * e^{(r-q)*T}` against a true continuous-
  monitoring expectation of `sigma^2 * (e^{(r-q)*T}-1)/((r-q)*T)`, a measured
  gap of `sigma^2*(r-q)*T/2` to leading order (`GammaSwap.CarryApproximation
  ClosedForm`: ~5.09e-4 decimal variance units at sigma=20%, r-q=5%, T=0.5Y --
  1.68% from the leading-order estimate, ratio 1.016824). The real
  discriminators against `VarSwap` are `GammaSwap.SkewOrdering` (above) and
  `CarryApproximationClosedForm` itself; `GammaSwap.MCOracle` (review fix
  round 1, I-2) is a calibration check only, kept for catching a wrong outer
  scale or broken quadrature -- it cannot discriminate a gamma swap from a
  variance swap at GBM/flat-vol MC precision, since the discrimination signal
  and this task's own single-expiry approximation bias are the same order,
  `O((r-q)*T)*sigma^2`.
* New ledger counter `counters::ledger::Solve::GammaSwapStripEvals`, separate
  from `VarSwapStripEvals` (folding the two would corrupt what P-6's book-memo
  O(K)-not-O(L) gate measures against `VarSwapStripEvals` specifically).
  `var_swap_memo_eligible` (deriv_book.cpp) is NOT extended to `GammaSwap` --
  it whitelists `kind == DerivKind::VarSwap` only, so a gamma-swap row always
  takes the generic unmemoized path (`DerivBook.GammaSwapNeverUsesTheVarSwap
  Memo`).
* `backtest.cpp`'s `valid_deriv_kind` (the live backtest engine's swap-lot
  gate) deliberately does NOT admit `GammaSwap` yet -- the engine's own
  per-lot accrual state (`SwapAccrual`) still mirrors only the plain
  estimator, so wiring the `S_i/S0`-weighted accumulator through the live
  daily-fixing loop is separate, future work. A strategy that tries to open a
  live `GammaSwap` `SwapLot` fails the WHOLE run loud
  (`validate_swap_lot_economics`), never silently mis-accrues one.
  `swap_leg.cpp`'s `solve_cycle_swap`/`swap_contract_for_lot` (a standalone
  fair-strike/vega solve against a `SurfaceRef`, no engine involved) has no
  such gap and works today (`SwapLeg.GammaSwapKindPassesThrough`).

### Added — `DerivConfig::wing_mode`: Lee-consistent wing extrapolation, `LeeSlopeExtrapolation` (FIT-F1 / PV-6 / LIT-6, Task F-1)

The variance strip's flat-vol wing clamp (`DerivConfig::wing_clamp_k`) is
Jiang-Tian-standard, desk-stable, and the v1.1 default — but freezing every
node beyond the certified band at the band-edge vol systematically
UNDERSTATES `K_var` whenever the band sits inside the smile's own real
curvature (roughly `sigma_atm*sqrt(T) gtrsim 0.083`). The fitting side has
never had this problem: eSSVI's phi ceiling caps total-variance slope at
`<= 2` by construction (Lee 2004's moment-formula bound), and ConvexDense's
tails are power-law, not flat — the strip simply never trusted either.

New `enum class StripWingMode { FlatClamp = 0, LeeSlopeExtrapolation = 1, Raw
= 2 }` and appended `DerivConfig::wing_mode` (arity pin raised 13 -> 14)
select what a node beyond the band serves:

* `FlatClamp` (v1.1 DEFAULT, unchanged) — freeze at the band-edge vol.
* `LeeSlopeExtrapolation` — continue TOTAL VARIANCE at the fitted slice's OWN
  slope at the band edge (`w(k) = w(k_band) + beta*(|k| - k_band)`), `beta`
  a central difference of the surface's own `iv()` clamped to Lee's
  `[0, 2-eps]` bound (`eps = 1e-3`) before use — an ill-behaved or
  misconfigured fit can never smuggle a moment-violating wing through this
  path, even though a Lee-compliant fit never needs the clamp to bind.
  Continuous AND slope-matched (C1) at the band edge whenever the clamp does
  not bind, which also means the C-3 quadrature split no longer needs to cut
  a panel boundary there (see `var_swap_fair_strike`'s `split_wing_band`,
  derivatives.cpp) — FlatClamp still does, unchanged.
* `Raw` — no clamp anywhere, identical to `wing_clamp_k < 0` regardless of
  `wing_clamp_k`'s own value (an explicit alternative spelling of that sign
  convention, not a second knob that composes with it).

New appended `DerivFlags::WingExtrapolated = 1u << 14`: `WingClamped`'s
sibling under `LeeSlopeExtrapolation`, same structural condition (the
resolved span extended beyond the trust band) fired on that mode instead.

**Measured** (skew fixture — eSSVI phi=4.0, rho=-0.7, sigma_atm=0.20, 6M
tenor, Standard tier, Balanced 0.5 mode-blind band):
`K_var(FlatClamp) < K_var(LeeSlope) <= K_var(Raw)`, with the
`LeeSlope`-to-`Raw` gap at 0.8-2.4% of the `FlatClamp`-to-`Raw` gap across
the fixture matrix this task's tests cover — LeeSlope recovers nearly the
entire understatement on a fit whose own wing is already close to its
asymptotic slope by the certified band edge (`WingMode.OrderingUnderSkew`,
derivatives_test.cpp). Lee's clamp is exercised (not merely present in code)
by a HINGE_QUAD wing-residual fixture (Task C-8's residual basis) whose
band-edge slope is engineered to 2.53, correctly served at `2 - eps`
(`WingMode.SlopeClampBinds`).

**P-4 interaction**: `deriv_greeks`'s `DerivGreekMethod::AnalyticStrip`
closed form (Task P-4) hard-codes the FlatClamp/Raw "clamp the grid position
once, then shift" wing identity and has no chain-rule term for a band-edge
slope that is itself a finite difference of the surface — `wing_mode ==
LeeSlopeExtrapolation` is excluded from `analytic_in_scope` at both call
sites (`deriv_greeks`, and P-6's shared-block builder
`ensure_var_swap_greeks_block`) and falls back to `FiniteDifference`
SILENTLY, the same fallback shape as every other out-of-scope case that knob
already had.

**P-6 interaction**: audited against the book-level `VarSwapMemo` key
(`deriv_book.cpp`) and found PROVABLY IRRELEVANT to it, on the SAME grounds
the key's own comment already established for the rest of `DerivConfig`:
`cfg` (and so `wing_mode`) is one value for the entire `price_deriv_book`
call, the memo is a local variable scoped to that one call, and every row a
given memo instance ever serves reads the identical `wing_mode` by
construction — there is no cross-row or cross-call path for a block built
under one mode to reach a caller expecting another.

**Migration**: `wing_mode` defaults to `FlatClamp` (enumerator 0), the same
value a value-initialized `DerivConfig{}` already carried before this field
existed — every construction site in this codebase uses designated-field
`DerivConfig{}` initialization (never positional brace-init), so the append
changes zero existing behavior and zero existing marks. Verified by re-running
every pre-existing `WingClamp.*`/`VarStrip.*`/`StripQuadrature.*`/`CarrLee.*`
test unmodified (31/31 green) plus the full `AnalyticGreeks.*`/`DerivGreeks.*`
(38/38) and `VarSwapMemo.*`/`PreparedPortfolio.*` (42/42, including the
bit-identity and fingerprint-pinned suites) regression surfaces — the diff is
the evidence, not a tolerance.

### Added — public entry points from the performance and dispatch work, every one bit-identical (Tasks P-1, P-3, F-5)

NO EXISTING NUMBER MOVES on any of these. They are here because this file
records PUBLIC API a 1.x caller can newly reach, not because a mark moved —
each was added to make an existing computation cheaper or a rule single-sourced,
and each is pinned bit-identical to what it replaced.

**Strip-carry hoisting: `SurfaceStripCarry`, `PricedSurface::strip_carry_at` /
`iv_with_carry` (`priced_surface.hpp`, Task P-1, `3bfce2f`).** `iv(K, T)`
re-derives `interp_forward(T)` and `CurveSurface::bracket(T)` on every call, and
a quadrature strip pays that 97–2049 times for one CONSTANT `T`. The new pair
resolves the forward/carry and the T-bracket once and reuses it;
`PricedSurfaceView` mirrors the pair and `SurfaceRef` (`portfolio_pricer.hpp`)
forwards it. `iv_with_carry(K, carry)` is bit-identical to `iv(K, carry.T)` —
both resolutions are pure functions of `T` over the surface's immutable state —
and returns NaN on the same domain `iv` does. `SurfaceStripCarry` is an OPAQUE
token: construct it with `strip_carry_at`, consume it with `iv_with_carry`, and
do not compare, mutate, or reuse one across a different `T` or a different
surface instance. Measured on the consumer that motivated it: paired A/B
(dev/Debug preset, 10 alternating pairs,
`BM_DerivGreeks_Standard_PricedSurface`) median **3.32x**, range 2.71x–3.97x,
10/10 pairs favouring the hoist.

**`PricedSurface::iv_batch(K, T, out)` (Task P-3, `0a63305`).** The batched
sibling of `iv(K, T)`: the same one-time bracket/carry resolution applied to a
whole strike vector, bit-identical to calling `iv(K[i], T)` for each `i`, NaN
wherever `iv` would be, and requiring `K.size() == out.size()` (asserted in
debug; degrades to the shorter length in release rather than running off the
end). `var_swap_fair_strike` uses it for `PricedSurface`-backed strips only —
every other surface type keeps the per-node scalar loop — behind the
`ATX_VOL_DISABLE_STRIP_BATCH` bench-only seam, and `black76_price_batch` was
deliberately NOT wired into the pricing pass because its AVX2 result is a
documented ~1e-9..1e-12 approximation that would break the bit-identity gate.

**IT IS NOT A MEASURED SPEEDUP, and the name should not be read as promising
one.** On this repo's Debug/SSE2 preset the paired A/B ran 0.90x before the
review fix round and 1.09x after it (`deriv/greeks/standard_priced_surface`,
1988µs → 1826µs), with `deriv/price/audit_priced_surface` at 977µs → 977µs,
exact parity; both series sit above the repo's 5% CV noise gate, so those are
directional readings and not bounds (`ba5916f`). The greek bump table's read
dedup that shipped alongside it has the same shape: its first cut, an
`std::unordered_map`, measured SLOWER than no caching at all under
`_ITERATOR_DEBUG_LEVEL=2`, and the sorted-vector replacement is what brought it
back to parity — it was never faster than no cache on this preset. What the
batch entry buys is a single resolution per call that a caller can invoke
directly; a Release re-measure is a separate exercise and has not been done.

**`deriv_kind_is_capped` / `deriv_kind_settles_in_vol` (`derivatives.hpp`, Task
F-5 pre-feature refactor, `331f86f`).** The two per-kind payoff-shape questions,
each now stated ONCE beside `DerivKind`. "Which kind carries a `cap_dec`" had
three independent hand-written disjunctions — the `cap_dec` scope rule in
`validate_deriv_dispatch`, `is_capped_kind` in `backtest.cpp` (feeding both the
swap-lot boundary check and the settlement haircut), and an inline expression in
`validate_restrike_spec` — which is exactly the shape that produced F-3's own
Critical C-1, two lanes reaching different verdicts about one contract. They
live beside the enum rather than beside `engine_supports_swap_kind` because they
answer a question about the PRODUCT's payoff, not about what the engine can
carry. Both are exhaustive `switch`es with NO `default:`, so `-Wswitch -WX`
turns a future enumerator into a compile error instead of a silent assignment to
the negated branch. Behaviour-preserving: each returns what the disjunction it
replaces returned for every in-enum input, and `false` for an out-of-enum one,
exactly as the `==` chains already did.

**Migration**: none for any of the three. Every pre-existing entry point keeps
its signature and its number; nothing here is on a path a caller must adopt.

### Changed — ConvexDense served `iv()` now within 1e-11 (not bit-identical) of the pre-P-5 value (FIT-P1, Task P-5)

`ConvexSliceFit::iv()`'s Black-76 inversion was a fixed 64-iteration
bisection with no early exit — up to ~260k Black-76 calls per Audit-tier
strip on a ConvexDense surface. It now breaks as soon as the bracket is
already near-machine width (`hi - lo < 1e-12 * max(1.0, hi)`, the same bound
the function's own docstring already promises), instead of always finishing
all 64 halvings once that width is reached. This is a genuine (if small)
behavioral change: the returned midpoint can differ from the old
fixed-iteration result by up to the final bracket width, so served vols are
no longer bit-identical to pre-P-5 builds, only guaranteed within 1e-11
(measured max drift on the pinned characterization fixture: ~2.8e-13, five
orders of magnitude inside the bound — `ConvexSliceFit.
IvBisectionEarlyExitMatchesPreP5BaselineWithin1e11`, dense_slice_test.cpp).
Measured iterations dropped from 64 to 44 uniformly across the fixture's
whole k-grid (bisection bracket width is a deterministic function of
iteration count alone, independent of vega/moneyness) — a 31% cut, not the
brief's optimistic "~1/3 of iterations" reading; paired A/B on a
~4097-node Audit-strip `iv()` sweep (Debug/dev preset, 10 alternating
pairs) measured a **1.40x** median speedup, matching the 65-vs-45
Black-76-call-count ratio almost exactly and falling short of the sprint
plan's 2x target. The shortfall is not mysterious and needs no external
document: the early exit removes bisection iterations only, so the speedup is
bounded by the Black-76 call-count ratio quoted above, and 65/45 is 1.44 —
the 2x target assumed iterations the fit never performed. Landed in `d283efe`.

The ConvexDense calendar-admission scan (`fit_slice_curve`'s shared-k
refit loop, `src/vol_curve.cpp`) also no longer inverts the fitted node
price to a vol via `iv()` just to square it back into a total variance for
comparison — it compares directly in price space against
`black76_price` of the floor's implied vol, which is what the floor is
actually enforced in anyway (`fit_convex_slice`'s own `cfloor` rows). Two
Black-76 calls per scanned k instead of ~65. This is proven to select the
IDENTICAL set of floor violations as the pre-P-5 vol-space scan on a
fixture matrix spanning every scan_k call site (legacy slack, legacy
crossing, strict on-lattice, strict off-lattice) —
`VolCurve.CalendarScanPriceSpaceSelectsIdenticalFloorsAsPreP5Baseline`,
vol_curve_test.cpp.

**Migration**: none required for either change. No public API or knob moved
— `ATX_VOL_DISABLE_IV_EARLY_EXIT` is a process-load-time bench-only seam
(mirrors `ATX_VOL_DISABLE_STRIP_BATCH`, Task P-3), never read by production
call sites, and does not change the default. A caller comparing served
`iv()` against a stored pre-1.1.0 value should widen any bit-identity
assumption to a 1e-11 tolerance.

### Added — `DerivGreekBumps::method`: opt-in closed-form VarSwap strip greeks, `AnalyticStrip` (GK-P, Task P-4)

`deriv_greeks`' delta/gamma/vega/vanna/volga for an uncapped `DerivKind::VarSwap`
(any age) priced with `DerivConfig::discrete_correction_mode ==
DerivDiscreteCorrection::None` can now come from the strip's own closed form
instead of a bumped repricing. `K_var` is a LINEAR functional of the strip's
Black-76 node prices with fixed composite-Simpson weights, so differentiating
it costs one pass of node-level Black-76 vega/volga plus four extra batched
smile-derivative read vectors (`sigma'(k)`, `sigma''(k)` via a 5-point
stencil) instead of repricing the whole N-node strip integral once per bump —
up to 8 of the up-to-16 strip repricings a full second-order `deriv_greeks`
call pays are skipped when in scope. Every other `DerivKind` (`VolSwap`, both
capped kinds), AND an in-kind VarSwap priced with the `Diffusion1OverN`
discrete-monitoring correction ON (its addend is quadratic in `K_var`, which
the raw-strip closed form does not account for — Review fix round 1, C-1),
prices through — or via — a term this closed form cannot shortcut, and falls
back to `FiniteDifference` SILENTLY: requesting `AnalyticStrip` out of scope
reprices bit-identically to an explicit `FiniteDifference` request, never an
error and never a wrong analytic result. `theta`/`theta_carry`/
`theta_zero_fixing` are unaffected by this knob (each still rolls
`maturity_t`, genuinely new information no closed form shortcuts); `rho` is
already the P-2 closed form on every path regardless. `charm` IS affected —
it differences the FD-rolled delta at `T - dt` against whichever delta the
knob selected at `T`, so under `AnalyticStrip` one side of that difference is
analytic; measured move is small (skew-unaged fixture: `+1.362483629` (FD) →
`+1.361984058` (AnalyticStrip), 3.67e-4 relative) but real and gated
(`AnalyticGreeks.*`, `deriv_greeks_test.cpp`, Review fix round 1, I-2).

`DerivGreekBumps` grows one field (`method`, arity 6 -> 7): a new
`enum class DerivGreekMethod : uint8_t { FiniteDifference = 0, AnalyticStrip = 1 }`.
**Default `FiniteDifference` — no mark move, no evaluation-count change, for
any existing caller.** The flip to `AnalyticStrip`-by-default is a 2.0
decision, not this task's.

**Migration**: none required. A caller pricing VarSwap greeks in a hot loop
can opt in with `bumps.method = DerivGreekMethod::AnalyticStrip`; parity
against FD is gated at `|Δ| <= max(1e-8*scale, 5*FD-noise-floor)` per greek,
flat and skewed surfaces, unaged and mid-life contracts (`AnalyticGreeks.*`,
`deriv_greeks_test.cpp`) — that test IS the parity record, and re-running it
reproduces the deltas. The paired A/B (Debug/SSE2 preset, 10 same-invocation-
order pairs) measured a **1.65x** median-of-medians speedup, pairwise median
1.76x over a 1.26x–2.45x range, with every one of the 10 pairs favouring the
analytic path; a Release re-measure is scheduled separately. Landed in
`337158b`.

### Changed — `deriv_greeks`'s rho is now the closed form `-T*PV`, not a finite difference (GK-C3, Task P-2)

Every quote this library prices is `pv = df(r) * X`, X (the fair strike /
undiscounted expectation / cap-option blend) provably independent of the
rate curve — the variance strip's own `OTM(K)/df` integrand cancels its
discount factor algebraically, the Carr-Lee ATMF-straddle formula never
reads `curves.yield`, and the `Diffusion1OverN` discrete-monitoring
correction's carry differential is read off the forward/spot, not the yield
curve. So `dPV/dr = -T*PV` identically — the same identity the fully-aged
branch already used — for every `DerivKind` and aging state, not just the
fully-aged one. `rho` is now computed directly from that identity instead of
a one-sided finite difference (`(price(r+dr) - price(r)) / dr`, `dr =
DerivGreekBumps::rate_abs`), which only ever recovered the same number to
the stencil's own truncation error.

**The shipped formula is `-max(T, 0) * PV`, not `-T * PV`.** `deriv_df_at_T`
returns `df = 1.0` unconditionally for `T <= 0` — no rate dependence at all —
so the true `dPV/dr` past maturity is 0, and an unclamped multiply sign-flips
a negative `T` into a fabricated POSITIVE rho. Two entry paths reach a
successful quote at `T <= 0`: a `FullyAged` lot past its own maturity (the
PV-9 bullet below), and a cap-pinned PARTIALLY-aged contract, whose cap-pin
exit returns successfully for ANY `T` without setting `DerivFlags::FullyAged`
(`c3bab3f`). Both are clamped by the same `std::fmax(contract.maturity_t,
0.0)`.

**This moves `rho` by the FD truncation this deletes** — on the order of
`(rate_abs * T / 2)` relative (e.g. ~1.25e-5 relative, ~0.001%, at the
default `rate_abs = 1.0e-4` and `T = 0.25`; grows linearly with `T` and with
`rate_abs`) — a real, if small, mark move on any book already reading `rho`.
`DerivGreekBumps::rate_abs` is left in the struct (source-compatible,
harmless) but is no longer consumed by anything; see its own field doc.
**−1 repricing per `deriv_greeks` call** (the deleted `r+` bump; Standard
unaged VarSwap greeks: 14 → 13 evaluations).

**Migration**: any test or downstream number pinning `rho` to FD precision
against the OLD one-sided stencil should re-pin against `-max(T, 0)*PV` (now
exact, not approximate) instead; every other greek is bit-identical
(`Rho.AnalyticMatchesFD`, `deriv_greeks_test.cpp`, pins the identity against
the FD bump this replaced, across every `DerivKind` and aging state, before
and after the deletion).

### Added — `DerivGreekBumps::carry_theta`: the fixing-roll theta a calendar-only roll drops, on by default (GK-C2, Task C-10)

`DerivGreeks::theta` rolls ONLY the calendar (`T -> T - dt`) with `rv_spec`
held fixed, so it silently omits the implied-to-realized fixing rollover —
the largest deterministic daily P&L term on an unaged/mid-life swap (a
fair-struck swap reports `theta ~= 0` even though the fixing roll itself is
a real, large daily mark move). `theta_carry`/`theta_zero_fixing` price that
roll too, by injecting one extra fixing into a COPY of `rv_spec` (struck at
the fresh `K_var_future`, or at a literal zero return respectively) before
the same `T - dt` roll `theta` already takes. Both go NaN when the contract
cannot roll, when the injected fixing would cross a dispatch-ENGINE boundary
the center was never in (e.g. an unaged VolSwap under an explicit
`DerivEngine::VolCarrLee`, which cannot price mid-life), or — after the C-R
gate fix below — when the strip resolving `K_var_future` itself fails on a
holey surface. Fully-aged: both equal `theta` exactly, unconditionally.

`DerivGreeks` grows two public fields (`theta_carry`, `theta_zero_fixing`;
arity 10 -> 12) and `DerivGreekBumps` grows one (`carry_theta`; arity 5 -> 6,
**default `true`**). Default-on costs one extra `var_swap_fair_strike`
evaluation (resolving `K_var_future`) plus two extra `deriv_price` repricings
on top of the block's existing up-to-14 — **up to 17 repricings per
`deriv_greeks` call, ~+21% over the pre-C-10 worst case** — skipped for free
(no extra evaluation at all) when `contract.rv_spec.n_obs_total == 0` (no
fixing schedule to inject into).

**Migration**: every existing `deriv_greeks`/`price_deriv_book(greeks=true)`
caller pays the extra cost starting now; set `DerivGreekBumps::carry_theta =
false` to opt back out to the pre-C-10 evaluation count (both fields then
stay at their `NaN` default, same as a contract that cannot roll). A reader
of `DerivGreeks` gains two fields it must default-initialize/assign
explicitly (no positional brace-init of this type exists in-repo today).

### Fixed — an unaged VolSwap's `deriv_greeks` no longer loses the whole block over a holey surface (C-R aggregate review, Critical)

`carry_theta`'s strip call (above) sat behind a plain `ATX_TRY`, and the
SAME `var_swap_fair_strike` routine gained a new hard `Internal` failure
this sprint (PV-4, past `max(2, n/100)` interior-bad nodes) — so a
holey-but-otherwise-servable surface, under the DEFAULT config (`carry_theta`
true, engine `Auto`), lost the ENTIRE `Result<DerivGreeks>` where pre-sprint
it returned a complete block. `price_vol_swap`'s own unaged branch treats
this identical strip call as best-effort and never fails the price over it.
Both carry fields now degrade to `NaN` on any failure in that strip
resolution or its two dependent repricings, mirroring the existing
engine-boundary degrade — every other greek is unaffected, and the CENTER
quote's fail-loud contract (PV-4) is untouched.

**Migration**: none for a caller who was already getting a complete block;
a caller relying on the (regressed, sprint-internal) hard failure to detect
a holey surface should instead check `has_flag(quote.flags,
DerivFlags::InteriorBadNodes)` on the embedded center quote, or call
`var_swap_fair_strike` directly for its own fail-loud contract.

### Fixed — five `DerivGreeks`/`FitDiag` correctness gaps from Task C-9

**PV-9** — An expired-but-not-yet-rolled `FullyAged` lot (`contract.
maturity_t < 0`, obs-count-driven aging already complete) computed `rho =
-T*PV` with the NEGATIVE `T`, sign-flipping into a fabricated POSITIVE rho
instead of "nothing left to discount". `T` is now clamped to `>= 0` before
that multiply (`std::fmax(contract.maturity_t, 0.0)`), so a `FullyAged`
contract past its own maturity now reports `rho == 0` like every other
fully-realized lot. This number flows into `PriceTotals::rho` through
`deriv_book::accumulate` for any book carrying such a lot.

**GK-C8** — Front-pillar theta/charm (a roll landing below the surface's
first fitted pillar) used to clamp the forward flat and the discount flat —
dropping theta's own discount-roll term — and the header told callers to
treat the result as merely indicative. `carry_from` (the PricedSurface-
native carry snapshot) now inserts a second forward + rate pillar at the
rolled tenor, read off the surface's OWN economic extrapolation
(`PricedSurface::forward_at`/`rate_at`) instead of the flat clamps
`resolve_forward`/`YieldCurve` apply outside the pillar range — mirroring
the `SurfaceRef` bridge (`carry_from_ref`), which already carried that
second pillar. Theta/charm on a front-pillar contract MOVE, and the header
caveat calling the result indicative is deleted — it is now the surface's
real extrapolated carry, not an approximation.

**GK-C7** — `deriv_greeks` gains a new `InvalidArgument`: a `vol_abs` bump
sitting at or above the surface's own ATM implied vol (`b.vol_abs >=
sigma_atm`) used to push the vol-down bump's shifted nodes to a non-positive
IV, which the strip funnel resolved to zero silently — hollowing out
vega/volga/vanna with no visible signal. `deriv_greeks` now rejects that
bump size up front (`"greek bump sizes must be > 0 (spot_rel < 1, vol_abs <
ATM vol)"`), reading `sigma_atm` once at the contract's own `(k=0, T)` — an
error-contract tightening exactly parallel to PV-5's dispatch-matrix fix.

**GK-C9b** — `DerivPriceFrame::totals.{theta,vanna,charm}` are NaN-poisoned
by design the moment any `Ok` row's own column is NaN (a contract too short
to roll; `second_order == false` for vanna/charm) — but a NaN total used to
be a dead end with no way to tell "1 excluded lane" from "every lane
excluded". Three new fields, `n_theta_excluded`/`n_vanna_excluded`/
`n_charm_excluded`, name how many `Ok` rows were excluded from each column
(0 when `greeks` was false — nothing attempted, not "nothing excluded" — or
every `Ok` lane's column was finite). `DerivPriceFrame` arity 2 -> 5.

**FIT-C11** — The dense-slice right-wing power tail's exponent used to floor
at exactly `0.0` on a degenerate fitted edge (last two node prices equal,
zero slope), which made the tail flat-clamp a non-zero option price forever
— exactly the flat-clamp this tail form exists to avoid. The exponent now
floors at `1.0e-6` instead: still decays, if slowly, rather than never
decaying at all. Far-strike prices on a degenerate board move from
flat-clamped to (slowly) decaying.

**Migration**: PV-9 corrects a fabricated positive rho to 0 for any expired,
not-yet-rolled `FullyAged` lot — a caller marking such lots should expect
this number to move. GK-C8 moves front-pillar theta/charm to the surface's
real extrapolated carry; a caller that special-cased "indicative" front-
pillar theta can remove that special case. GK-C7 is a new `InvalidArgument`
a caller with a `vol_abs` bump close to a low-vol surface's ATM level may
now see for the first time. GK-C9b is a pure append (three new zero-default
fields) with no effect on existing totals. FIT-C11 moves far-wing prices on
boards with a degenerate fitted edge (last two node prices exactly equal) by
an amount that decays with distance from the edge.

### Fixed — a converged, wing-clamp-certified QP solution where the fit used to fail closed (FIT-C3/FIT-C4, Task C-7)

The dense-slice active-set QP's ratio test could produce a spurious NEGATIVE
step size `alpha` at a numerically dual-degenerate vertex: a row whose
directional derivative sits inside the `-1.0e-14` anti-cycling dead zone can
still drift a few ulp past zero over many iterations, and selection takes
the MINIMUM `alpha` — so an unclamped negative value beat every legitimate
positive one, with its magnitude unbounded as the row's own gradient
projection shrank toward the dead-zone edge. `x += alpha * p` then stepped
BACKWARD out of the feasible region by an arbitrary multiple; the solver's
own certificate correctly refused the resulting point, and the caller
dropped the whole slice (or, under `fail_board_on_hard_slice_error`, the
whole board). `alpha` is now clamped at `0.0`
(`std::max(0.0, -gix/gip)`), turning that failure into the degenerate
zero-length step an active-set method takes at a tied vertex: the row joins
the working set at the current iterate and the walk continues to a genuine
KKT-optimal certificate. A companion fix (FIT-C4) makes the DROP-side tie-
break deterministic on an exactly-tied most-negative multiplier (lowest
constraint-row index wins, matching the existing BLOCKING-side tie-break) —
previously untested by any board that reaches the drop path.

**Migration**: a board whose dense-slice fit used to fail (slice dropped, or
the whole board refused under `fail_board_on_hard_slice_error`) at a
numerically dual-degenerate vertex can now converge to a certified solution
instead — an AVAILABILITY change, not a value change on any board that was
already fitting successfully (the clamp only ever turns a would-be-refused
certificate into a continued walk to the SAME KKT-optimal point a
non-degenerate path would have found). No magnitude table: there is no
"before" value to compare against on a board this fix newly admits.

### Fixed — the eSSVI alternate driver's `validate_no_arb` is no longer a dead knob (FIT-C1/FIT-C5, Task C-8)

`essvi_calib_surface`[`_sequential`] unconditionally stamped `out_diag->
n_butterfly_viol = 0` and never ran a calendar check, regardless of
`CalibOpts::validate_no_arb` (default `true`) — the knob's documented
contract ("run the static-arb validators at the end and bail on a
violation", `calib.hpp`) was dead on this path. A genuinely calendar- or
butterfly-arbitrageable surface (e.g. from an un-projected HINGE_QUAD
wing-residual layer — the per-slice Roper projector stays out of port
scope, see the PORT NOTE on `fit_wing_residual`) was served silently.

`validate_no_arb` now runs a real post-fit audit (`arb_check_total_
surface_all` over the surface's assembled slices, `[-0.5, 0.5]` / 64-grid
— the same window the surface-recovery tests already independently
certify clean), populates `FitDiag` with the real counts, and returns
`Unavailable` on a nonzero count. This is INDEPENDENT of
`essvi_alt_driver_theta_project` (FT-C9a's opt-in theta-bump repair, still
default off, still the only thing that MOVES a fitted level) — the audit
runs after that repair, so enabling both knobs is repair-then-audit.

`FitDiag::n_calendar_viol` (new field — `FitDiag` carries no arity pin, so
this is a plain append) reports the real calendar-violation count;
`n_butterfly_viol` is no longer unconditionally zero on the eSSVI path.
Both read 0 when `validate_no_arb` is false (audit not run — not itself a
"verified clean" claim).

The `fit_slice_curve` (canonical PricerFitter) Essvi branch is UNCHANGED
on the default (`residual_disable == true`) path — bit-identical. When a
caller has opted into the HINGE_QUAD/C2Bspline wing residual, the served-
slice butterfly check now scans the full quoted range +/- 0.5 (mirroring
FT-C2's raw-SVI fix) instead of the fixed `[-0.6, 0.6]` band, catching wing
arb the un-projected residual can carry past it.

**Migration**: a caller of `essvi_calib_surface`/`_sequential` relying on
the default `validate_no_arb == true` that was previously served a
(silently) arbitrageable surface now gets `Err(Unavailable)` instead —
inspect `FitDiag::n_calendar_viol`/`n_butterfly_viol` for the real counts,
or set `validate_no_arb = false` to keep the historical permissive
behavior (e.g. a deliberately-biased fixture used to measure a KNOWN
disease, as `essvi_deam_test.cpp`'s raw-route tests now do explicitly).
The per-slice `essvi_fit_slice`/`fit_slice_curve` (PricerFitter) paths are
unaffected either way.

### Fixed — the wing clamp now trusts a Latency-mode surface's OWN certified band, not a wider one nobody certified (FIT-C7, Task C-6)

`DerivConfig::wing_clamp_k == 0` (the default) resolved unconditionally to
`strip::kCertifiedWingHalfBand` (0.5) — the band the fit pipeline's
independent risk validator certifies for a **Balanced**-quality surface
(`risk_validation_config`, `pricer_fitter.cpp`). A surface fit at
**Latency** quality is certified only to `±0.35`; a default-config quote
against it was reading `[0.35, 0.5]` as trusted when nothing in the fit
pipeline ever validated that range — precisely the uncertified extrapolation
the clamp exists to keep out.

`atx::vol::certified_wing_half_band(FitQualityMode)` (new,
`surface_policy.hpp`) is the canonical mode-keyed band (Latency 0.35,
Balanced 0.50 — unchanged, Accuracy 0.60). The `PricedSurface`/`SurfaceRef`-
native entry points (`var_swap_fair_strike`, `vol_swap_fair_strike`,
`deriv_price`, `deriv_greeks`, `deriv_price_on_ref`, `deriv_greeks_on_ref`)
each take a new trailing `surface_certified_wing_band` parameter (default
`std::nullopt`): a caller that knows the surface's own build quality mode
resolves it through `certified_wing_half_band` and passes it in, and
`wing_clamp_k == 0` now resolves to THAT band instead of the mode-blind
default. The templated legacy-surface entry points (`VolSurface`/
`EssviSurface`/`SviSurface`) carry no such provenance and are unaffected —
they keep reading `strip::kCertifiedWingHalfBand` exactly as before.
`DerivQuote::resolved_wing_clamp` (new field, arity pin 15 -> 16) records
the band a quote actually resolved, so a caller can inspect it directly
instead of inferring it from `DerivFlags::WingClamped` alone. A bumped
`deriv_greeks` evaluation prices through an adapter with no provenance of
its own, so `pin_center_scheme` now also pins the CENTER's resolved band
into every bump's config — without this, a Latency-certified center's
vega/gamma/etc. would difference against bumps silently read at the wider
mode-blind band.

**Migration**: every existing call site is unaffected — `surface_certified_
wing_band` defaults to `std::nullopt`, which resolves to the SAME `0.5` this
code always used, bit for bit (also regression-tested). A caller pricing a
`PricedSurface`/`SurfaceRef` it knows was fit at Latency or Accuracy quality
should start passing `certified_wing_half_band(quality_mode)` explicitly;
doing so for a Latency-mode surface **moves the mark** — a steepening wing
between `±0.35` and `±0.5` was previously read as trusted smile and now
reads flat at the `±0.35` band edge instead, which can only lower a
variance-swap fair strike (never raise one, by the same monotonic-tightening
identity an explicit `wing_clamp_k` override already has). Accuracy-mode
surfaces widen from 0.5 to 0.6 and move the other direction. Balanced-mode
surfaces are bit-identical either way (0.5 either way).

**Wired, not just available** (review fix round 1): the paragraph above
originally undersold this as an opt-in a caller had to reach for. It is now
also wired into every live production path that prices or marks a swap lot
against a stamped-provenance surface, so a Latency/Accuracy-fit surface's
own band is read automatically, with no caller-side opt-in required:

- `MarketSnapshot::provenance(uid)` (already plumbed end-to-end from
  `FittedSurface::quality_mode()` through `SurfaceArchive`) now feeds a new
  `certified_wing_band_for(snapshot, uid)` helper (`backtest.hpp`), used by
  `step_swap_lots` (`backtest.cpp`) to mark every open swap lot's PnL against
  its OWN surface's certified band, and by `DeclarativeStrategy` (`strategy.cpp`)
  to size and fair-strike every new swap leg the same way.
- `solve_cycle_swap` (`swap_leg.hpp`/`.cpp`) takes a new trailing
  `surface_certified_wing_band` parameter (default `std::nullopt`, so any
  caller not yet threading provenance is unaffected bit for bit) and forwards
  it to both the fair-strike solve and the entry-vega greeks; `SwapSignalProbe`
  passes it through as well.
- `price_deriv_book` (`deriv_book.hpp`/`.cpp`) takes a new trailing
  `WingBandResolver` callback (default empty, same no-op guarantee) so a
  caller holding per-uid provenance from any source can apply it per row.

A legacy archive with no independently-admitted `SurfaceProvenance` resolves
`FitQualityMode::Balanced` (`legacy_surface_provenance()`), the mode-blind
default band, so none of the above changes a mark for archives that predate
provenance stamping. It changes marks only for Latency/Accuracy-stamped
surfaces flowing through these paths, per the monotonic-tightening/-widening
direction described above.

### Fixed — two silent kind x engine mismatches now fail loud (PV-5)

`deriv_price` already rejected an engine that names no pricing formula for a
capped kind (`StripLogContract`/`VolCarrLee` on `CappedVarSwap`/
`CappedVolSwap`), but missed the same mismatch on the two uncapped kinds:

- `DerivKind::VarSwap` + `DerivConfig::engine == VolCarrLee` silently ran the
  variance strip anyway — `price_var_swap` never read `cfg.engine` at all, so
  every engine choice on a `VarSwap` behaved exactly like `Auto`.
- `DerivKind::VolSwap` + `DerivConfig::engine == StripLogContract` silently
  fell into the same unaged Carr-Lee branch `Auto`/`VolCarrLee` take — that
  branch only tested `cfg.engine != RvDistributionProxy`, not which engine it
  actually was.

Both combinations now return `InvalidArgument` ("engine cannot price a var
swap" / "engine cannot price a vol swap") at dispatch, before any pricing
runs. The full matrix `deriv_price` enforces: `VarSwap` -> {`Auto`,
`StripLogContract`}; `VolSwap` -> {`Auto`, `VolCarrLee` (unaged only, as
before), `RvDistributionProxy`}; `CappedVarSwap`/`CappedVolSwap` -> {`Auto`,
`RvDistributionProxy`} (unchanged).

**Migration**: `cfg.engine` defaults to `Auto`, so no default-config caller is
affected — this changes zero prices. A caller who explicitly pinned
`VolCarrLee` on a `VarSwap` contract, or `StripLogContract` on a `VolSwap`
contract, was silently getting the SAME number `Auto` already returns for
those inputs (a strip quote / an unaged Carr-Lee quote respectively) under an
engine name that did not describe what ran; it should switch to `Auto` (or
the matching valid engine) — no mark changes for anyone reading the number
`Auto` already gave them.

### Fixed — an interior bad node silently contributed 0 with no trace (PV-4)

The variance strip's OTM integrand treats a non-finite or non-positive
surface IV as 0 at that node. Only the two ENDPOINT nodes of the whole grid
were ever checked (`bad_first`/`bad_last`, driving `StripTruncatedLeft`/
`Right`) — a bad node strictly inside the grid, including the `k = 0`
put-call-parity kink the C-3 panel split reads as its own node, contributed 0
to the integral with no trace anywhere in the returned quote.

`var_swap_fair_strike` now counts interior bad nodes. One or more sets the
new `DerivFlags::InteriorBadNodes = 1u << 13` (still priced — a handful of
gap quotes is business as usual on a real fitted surface). More than
`max(2, n_nodes/100)` returns `Internal` instead of a quote: a surface with
that many holes across its middle is broken, not sparse, and a number built
mostly from zero-substitutions is worse than refusing to answer. Exempted
when the strip is wholly unusable — BOTH true grid endpoints (`bad_first`
and `bad_last`) read non-finite, e.g. a query T under the legacy short-T
extrapolation guard — that is the pre-existing, deliberately tolerated
"surface has nothing to say at this T" corner (`deriv_greeks` relies on it
to roll a theta/charm bump past expiry and get a NaN greek back, not a
failed call), a different failure from PV-4's target of an otherwise-usable
surface with a hole in it. (Review fix round 1: the exemption is decided on
the strip's own two endpoints, not on a fresh ATM read — an ATM-coupled
exemption would silently re-open PV-4's own named case, a single bad node
that happens to sit at the k = 0 kink.)

**Migration**: nothing changes for a clean surface — a well-formed fitted
surface was never producing interior bad nodes, so this is new accounting,
not a new failure mode. A caller whose surface adapter has genuine mid-grid
gaps will now see `InteriorBadNodes` (informational, same price as before) or,
past the threshold, an `Internal` error where it previously got a quote
computed mostly from zeros with no signal that anything was wrong.

### Fixed — `DerivDiscreteCorrection::Diffusion1OverN` was wrong (PV-1, PV-8)

The discrete-monitoring correction for the future implied-variance leg
multiplied `K_var_future` by `(1 + 1/n_obs_total)`: the wrong functional form
(the Broadie-Jain (2008) leading-order diffusion-drift term is ADDITIVE, not
multiplicative) applied with the wrong divisor (the contract's total
observation count, not the future leg's own remaining fixings).

Corrected formula, applied at all four call sites (`price_var_swap`,
`price_vol_swap_distribution`, `price_capped_var_swap`,
`price_capped_vol_swap`):

```
K_var_future += (T_resid / n_remaining) * (r_bar - q_bar - K_var_future/2)^2
n_remaining = n_obs_total - n_obs_done
r_bar - q_bar = ln(F/S) / T_resid   (read from the same CurveSet the strip
                                     already resolves F from)
```

**Magnitude**: for a daily-monitored (n=252) index contract at sigma=20%,
r-q=5%, T=1Y, the old code added ~1.6 variance points (`K_var/n`); the
corrected addend is ~0.036 variance points — smaller by about two orders of
magnitude. `DerivFlags::DiscreteCorrApplied` still marks whenever the
correction ran.

**Migration**: `discrete_correction_mode` defaults to `None`, so this changes
nothing for a caller who left it there. Anyone who had already opted into
`Diffusion1OverN` was marking discrete-monitored variance/vol swaps
materially rich (by roughly the magnitude above) and should re-mark against
the corrected engine.

Also fixed (PV-8): xi (vol-of-vol) auto-calibration was resolving against the
ALREADY-corrected strip mean whenever this mode was on, so
`resolve_vol_of_vol`'s "reproduces Carr-Lee exactly" guarantee silently broke
under the mode. xi is now always resolved against the UNCORRECTED strip mean;
the correction applies only to the mean actually fed to the distribution
model afterward.

Not covered: the residual O(1/n) jump term (Broadie-Jain sec 4) — jump-
diffusion discrete-monitoring bias needs the `FullMc` engine (reserved,
LIT-3).

### Fixed — short-tenor variance strip was under-resolved; `DerivFlags::LowT` now fires (PV-2, PV-3)

The E2 adaptive-wing logic only WIDENED the strip's span for a high-vol/
long-dated tenor (`kh = max(tier_span, width_sigmas*sigma_atm*sqrt(T))`) and
rescaled the node count to match — it never checked the OPPOSITE direction.
The tier grids are sized for a roughly-1Y reference vol scale, so a short-
tenor quote (e.g. T = 1 trading day) sits comfortably inside the tier's span
floor but resolves it far more coarsely than its own `sigma_atm*sqrt(T)`
calls for. The quadrature error this starves is dominated by the near-ATM
curvature the strip integrates through (the `price/(df*K)` integrand's kink
at `k = 0`), not by truncated wings.

`var_swap_fair_strike` now enforces the mirror rule after span resolution:
`dk <= sigma_atm*sqrt(T) / 4`, raising the node count when it does not
(rounded up to the next `4m+1` so the Richardson half-grid estimate stays
populated). `sigma_atm` is the same ATM-vol read the span logic already
resolves — no second surface read. A caller-pinned `strip_nodes` is never
overridden (pin semantics are load-bearing for `deriv_greeks`' grid pinning);
a pinned grid that violates the floor raises the now-live `DerivFlags::LowT`
instead of being silently corrected. `LowT` was declared in 1.0.0 with no
writer anywhere in the engine (PV-3); it now fires whenever the floor
engaged, or a pinned grid could not be corrected to satisfy it.

**Magnitude** (flat sigma=20%, T=1/252, truth K_var=0.04 exactly):

| Tier     | Nodes (pre→post) | K_var (pre→post)         | Move                    |
|----------|-------------------|----------------------------|-------------------------|
| Fast     | 97 → 637          | 0.042423 → 0.040000        | -6.06% (~-24.2 var pts) |
| Standard | 257 → 957         | 0.0400156 → 0.0400000      | -3.91bp (~-0.16 var pt) |
| High     | 769 → 1273        | ~0.04000000 (both)         | ~0 (already accurate)   |
| Audit    | 2049 → 2049       | 0.0400000002 (unchanged)   | none (floor unneeded)   |

The node counts in that column are this fix's own arithmetic. The kink-split
entry below provisions 16 further intervals of apportionment headroom, so the
counts 1.1.0 actually ships at this tenor are **653 / 973 / 1289 / 2049**
(pinned in `StripResolution.PanelSpacingRespectsCeilingAtTheFloorBoundary`).
The K_var column is unaffected — both grids are far past convergence here.

**Migration**: this moves the DEFAULT Fast-tier mark at short tenors — the
old default was a verified quadrature bug (PV-2), not an intentional choice,
so per the sprint's correctness-first rule it is corrected rather than kept
for compatibility. A caller marking short-dated (sub-week) variance/vol
swaps at `DerivQuality::Fast` was pricing them materially rich/cheap by
roughly the magnitude above and should re-mark against the corrected engine.
Standard/High/Audit move by ≤4bp at the same tenor and are unaffected at
ordinary (multi-week+) tenors, where the floor was already satisfied. A
caller that pins `strip_nodes` explicitly is unaffected numerically (the pin
still holds exactly) but may now see `DerivFlags::LowT` where it previously
never could.

### Fixed — the variance strip's Simpson panels now straddle no C1 kink (LIT-10)

The strip's OTM integrand `OTM(K)/(df*K)` is only PIECEWISE smooth. It kinks
in C1 at `k = 0` — put-call parity makes the two branches agree in value at
`K = F` but their `K`-derivatives differ by the discount factor, a slope jump
of exactly 1, some 25x the integrand's own ATM value at a 3M 20-vol — and at
`±wing_clamp_k` whenever the clamp binds, where `d(iv)/dk` drops to zero.

Composite Simpson is O(h^4) on a smooth panel but only O(h^2) on one that
STRADDLES such a kink, and the Richardson `|I_h - I_2h|/15` estimate assumes
the h^4 law. Before this change `k = 0` landed on a panel boundary only
because every DEFAULT grid happens to be symmetric with `4m+1` nodes — an
accident of the defaults, never asserted, and broken by any caller-pinned
asymmetric span. The clamp edges sat mid-panel even on the defaults.

`var_swap_fair_strike` now splits the composite integration at every interior
kink (`detail/strip_grid.hpp`'s `plan_strip_split`), apportioning the resolved
node budget across the sub-intervals in proportion to length so the kinks are
panel boundaries BY CONSTRUCTION on any grid. The total span and total node
count are unchanged, so `strip_k_lo_used`/`strip_k_hi_used`/`strip_nodes_used`
keep their meaning and `deriv_greeks`' grid pinning replays a quote exactly.
The estimate is populated whenever every panel is `4m+1`, which every default
budget is; a starved caller-pinned budget gives up the clamp edges first, the
estimate next, and the `k = 0` alignment last.

**C-2's resolution floor is enforced per panel, not per nominal `dk`.** The
floor (`dk <= sigma_atm*sqrt(T)/4`) was sized against the spacing of one
uniform lattice. The split retires that lattice — integer apportionment cannot
divide a span evenly, so a panel's own spacing runs above the nominal
`span/(n-1)` (1.6% at Standard's 256 intervals). Left alone that would have
made the floor's guarantee approximate: a tenor whose nominal `dk` sat just
under the ceiling cleared the check while a panel breached it. `dk_floor_nodes`
now provisions the excess analytically — the apportionment bound is
`dk_i < span/(intervals - 4*n_panels)`, so requiring
`intervals >= span/dk_max + 4*n_panels` makes `dk_i < dk_max` hold for **every**
panel, exactly — **up to a ceiling.** That demand grows without bound as
`sigma_atm*sqrt(T) -> 0`, since the tier SPAN floor does not shrink with `T`, so
it is capped at `kMaxStripNodes` (2049, the Audit tier's own node count;
`e883930`). Where the cap binds — short-tenor, near-zero-vol quotes — the floor
is NOT met and `DerivFlags::LowT` says so, which is the honest report; without
the cap those quotes resolved hundreds of thousands of nodes per evaluation,
with no error and no flag. `DerivFlags::LowT` is likewise decided on the widest
panel rather than the nominal `dk`, which is what makes it honest for a
caller-pinned count (never overridden, so flagging is all it can do).

Cost: at most 16 intervals (`kMaxStripPanels == 4`). At ordinary tenors it is
inert — every tier still resolves exactly its default budget (97/257/769/2049),
so **no default mark moves by even one ulp** on this account. Where the floor
already engaged, node counts rise by 16 plus `4m+1` rounding (at `T = 1/252`,
`sigma = 20%`: Fast 637→653, Standard 957→973, High 1273→1289, Audit unchanged),
which only makes those quotes more accurate.

**Magnitude — default grids move by at most 2.8e-7 relative**, and toward
truth (3M skew fixture, `make_skew_surface(0.20, -0.40, 0.35)`; flat fixture
is truth `= 0.04` exactly):

| Tier     | flat K_var move | skew K_var move | Accuracy         |
|----------|-----------------|-----------------|------------------|
| Fast     | 8.5e-16 rel     | 3.1e-16 rel     | unchanged grid   |
| Standard | 2.4e-9 rel      | 2.8e-7 rel      | 8.5x more exact  |
| High     | 1.1e-15 rel     | 0 (exact)       | unchanged grid   |
| Audit    | 2.5e-12 rel     | 4.2e-9 rel      | ~unchanged       |

Fast and High do not move at all: their proportional apportionment happens to
reproduce the un-split uniform spacing exactly (96 intervals over four equal
panels; 768 over 1.5/0.5/0.5/1.5).

**Asymmetric-pin values are corrected, not merely nudged.** A pinned
`k_min_log = -0.714, k_max_log = 0.686, strip_nodes = 101` on the flat
sigma = 20%, T = 3M fixture returned `0.0402613` against a truth of `0.04` —
off by 2.6e-4 (6.5e-3 relative), which is exactly the `J*h^2/6*(2/T)` straddle
term. It now returns `0.0400000082`, matching the symmetric reference to
1.4e-13. Across a 16-step sweep sliding a 257-node grid through one panel, the
worst case improves from 1.8e-4 to 1.4e-9.

**The error estimate is now an estimate.** Measured `integration_error_est`
over the true error on the 3M skew fixture: the default Standard grid went
0.689 → 1.000; a symmetric 101-node pin (whose `k = 0` is a full-grid boundary
but an ODD HALF-grid index, so the /15 difference was measuring the half
grid's own straddle) went 574 → 1.158; an asymmetric 101-node pin went
0.133 → 1.161.

**Migration**: nothing to do for a caller on default grids — the moves above
are far below any mark's resolution. A caller that pins an ASYMMETRIC span was
being served an O(h^2) quote and a meaningless error estimate; it should
re-mark. `integration_error_est` was the one number that could previously be
wrong by four orders of magnitude while looking plausible, and any consumer
gating on it will now see a much smaller (and truthful) value.

### Added — `DerivConfig::carr_lee_form`: opt-in Remark 6.4/6.5 convexity refinement (LIT-4, Task C-5)

`vol_swap_fair_strike`'s naive Carr-Lee formula (`K_vol ~= sqrt(2 pi / T) *
C_ATMF(T) / (F * df)`) reads only the ATMF point of the smile — it is Carr &
Lee's own Prop. 6.1 bound (a) / Remark 6.3, the approximation their rrvd.pdf
explicitly declines to endorse (Remark 6.5). Under equity skew it is biased
LOW relative to the true fair vol-swap value (the paper's own Heston BCC
example, Sec. 6.5, cites >40 vol bp at 6M for ρ ≈ −0.7). Because
`resolve_vol_of_vol`'s xi auto-calibration inverts against this same naive
number, the bias was propagating into every distribution-model consumer
(capped var/vol swaps, mid-life vol swaps) too.

New `enum class CarrLeeForm { Naive = 0, Refined = 1 }` and
`DerivConfig::carr_lee_form` (appended field, arity pin raised 12 → 13)
select between the naive formula (default) and the paper's Remark 6.4/6.5
refinement evaluated against the variance strip's own `K_var`:

```
K_vol_refined = K_vol_naive *
    (1 + T*(K_var - K_vol_naive^2) / (8 + 2*T*K_vol_naive^2))
```

**This is NOT the T-dropped paraphrase a from-summary read of Remark 6.4
suggests.** The paper states `VOL0 ≈ IV0*(1 + (VAR0²−IV0²)/(8+2·IV0²))` for
UN-annualized total-horizon quantities (IV0, VAR0 scale like σ√T); restating
it directly against this codebase's ANNUALIZED `K_vol`/`K_var` without
re-inserting `T` silently assumes `T == 1` always. The formula above is the
annualization-consistent substitution (`IV0 = K_vol_naive·√T`,
`VAR0² = K_var·T`, divided back through by `√T`); it collapses to the
T-dropped paraphrase exactly at `T == 1` and to `K_vol_naive` exactly
whenever `K_var == K_vol_naive²` (no convexity to recover). See Carr & Lee
(2009), Remark 6.4/6.5, and `derivatives.hpp`'s `detail::refine_carr_lee_
k_vol` doc for the full re-derivation.

**Wiring**: `resolve_vol_of_vol`'s three callers (mid-life vol swap, capped
var swap, capped vol swap) already have the strip's `K_var` in hand, so
`Refined` is free there. The standalone `vol_swap_fair_strike` entry does
not run a strip under `Naive` — under `Refined` it now pays for one
`var_swap_fair_strike` evaluation (propagating that call's own error
contract) and, since a strip now genuinely ran, populates
`uncapped_var_dec`/`integration_error_est`/the strip-grid fields instead of
leaving them at "no strip ran" (0.0 / NaN).

**Direction, verified against `resolve_vol_of_vol`'s own closed form (not
assumed from the task brief's paraphrase, which has this backwards): larger
K_vol_target (Refined, sitting closer to `sqrt(K_var)`) needs LESS inferred
lognormal dispersion to explain a SMALLER Jensen gap, so `vol_of_vol_used`
and every cap option value it drives move DOWN under Refined, not up** — the
naive formula's own approximation shortfall was being misattributed by the
auto-calibrator as real vol-of-vol, inflating `xi` (and cap prices) above
what the surface's actual convexity supports; `Refined` corrects part of
that inflation back down, same direction as the `K_vol` fix itself.

**Magnitude** (skewed fixture, `atm_vol=0.20`, ATM skew slope −0.40 bp/Δk at
the 3M pillar, ρ = −0.7, convexity 0.35, T = 0.5 = 6M, r=2%, q=1%):

| Quantity                         | Naive         | Refined       | Move             |
|-----------------------------------|---------------|---------------|------------------|
| `K_vol` (vol-swap fair strike)    | 0.199833458   | 0.199924092   | +0.906 vol bp    |
| `sqrt(K_var)` (Jensen bound)       | 0.217316377   | (unchanged)   | —                |
| xi (`vol_of_vol_used`, CappedVarSwap, T=6M, cap=0.05) | 1.158412278 | 1.155276537 | −0.271% rel |
| `cap_option_value_dec`            | 0.0141013933  | 0.0140619539  | −0.280% rel      |

The refinement recovers ≈0.9 vol bp of the naive-vs-Jensen-bound gap here
(≈174.8 vol bp total on this fixture) — a small, leading-order correction by
design (the paper does not endorse either approximation as globally
accurate; Remark 6.5), not a full close of LIT-4's cited >40bp bias. A
`Refined` caller should expect single-digit-vol-bp-scale `K_vol` moves and
sub-percent `vol_of_vol_used`/cap-value moves at ordinary skew, growing with
`T` and with the size of the naive-vs-`sqrt(K_var)` gap.

**Known residual, undisturbed by this change (review finding I-1):** Remark
6.5's `IV0` slot is the paper's ATM IMPLIED VOL (`sigma_atmf`) itself; this
implementation feeds it `K_vol_naive` (`carr_lee_k_vol`'s own ATMF-straddle
output), a SEPARATE, already-approximate stand-in for `sigma_atmf` biased low
by `sigma_atmf^3*T/24` (annualized) — on the fixture above, ~1.667 vol bp,
larger than the +0.906 vol bp this refinement adds. A `Refined` strike
therefore still lands below `sigma_atmf`, not just below the paper's true
`VOL0`. This is forced, not an oversight: `CarrLee.RefinementVanishesOnFlat`
pins refined == naive bit-exact whenever `K_var == K_vol_naive^2`, which only
holds with `K_vol_naive` (not `sigma_atmf`) as the refinement's base point —
substituting `sigma_atmf` would move `Refined` even on a flat surface and
break that pin. Fixing the underlying straddle-proxy bias is a separate,
un-scoped change (it would move the `Naive` default too).

**Migration**: `carr_lee_form` defaults to `Naive`, so this changes zero
prices for every existing caller — the v1.1 default is byte-compatible with
every pre-C-5 quote. **Planned 2.0 default: `Refined`.** A caller who wants
the closer (still not exact) approximation today can opt in explicitly; a
2.0 upgrade will move every unaged-vol-swap and distribution-model mark by
the small magnitude above, growing with skew and tenor, and should be
re-marked against the new default at that time.

## 1.0.0

The first release with a stability promise. Everything below happened during the
production-v1 release sprint, on the way from an internal library to one that
installs into a prefix and can be depended on.

**What 1.0.0 actually promises** is a *tier*, not the tree: the 57 headers
`atx/vol/vol.hpp` includes are frozen for 1.x, and the manifest that says which
those are is machine-checked (`kTierA` in `atx-vol/tests/vol_umbrella_test.cpp`).
(The set said 56 until the release audit re-derived it. Since the release gate's
pre-flight, the *count* is machine-checked too —
`VolUmbrella.TierCountsMatchTheReadmeTable` asserted 57 **as of this release**,
alongside Tier-B 31 and `detail/` 28 — so this digit can no longer rot silently
the way it did. Those three literals move as headers land; the live values are
the README tier table, not this entry, which records only what 1.0.0 shipped.)
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
  `main` at `1` and was moved to `2` in this release (v1 closeout sprint Task
  4.8, plan item 6.7) on a paired measurement: one binary with the depth
  alternated inside a single session, 12 interleaved rounds on the 135-session
  SPY-dispersion replay, medians and win-counts only — `1 → 2` **+15.2 % (11/12
  rounds)**, then `1 → 4` +19.8 % (10/12) and `1 → 8` +19.6 % (10/12), while `2 → 4` (+1.9 %,
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

### NEW — every public batch kernel is now reachable from Python

Three `batch.hpp` entries were bound, which completes the set — verified by
enumerating `batch.hpp`'s six entries against the module rather than by
inspection, because an earlier draft of this section claimed completeness while
`black76_price_from_lnfk_batch` was still unbound:

* `black76_greeks_batch(F, K, T, sigma, r, df, side)` — a dict of SoA numpy
  columns (`delta`/`gamma`/`vega`/`theta`/`rho`/`vanna`/`volga`/`charm`/`price`).
* `black76_value_and_vega_batch(F, K, T, sigma, df, side, sqrt_t=-1.0)` —
  `(value, vega)` for one expiry slice (`T` and `sqrt_t` shared, as in the C++
  signature).
* `black76_price_from_lnfk_batch(F, K, T, sqrt_t, sigma, df, ln_fk, side)` — the
  bind-step shortcut for a caller that already holds `ln(F/K)` and `sqrt(T)`.
  `sqrt_t` is **required and has no sentinel** here: the scalar kernel consumes
  it verbatim, so there is no negative value meaning "recompute". Its scalar
  companion `black76_price_from_lnfk` is bound alongside it, so the batch's
  bit-exactness is checkable from the same interpreter.

Binding-only: no kernel changed, and all three go through the validated
`batch.hpp` entry points rather than the raw `simd::` kernels.

Three notes for callers:

* **None returns a per-lane `status` column, and that is the NaN + per-lane
  convention rather than a departure from it.** A parallel status exists where
  the kernel HAS a per-lane failure channel a binding must not erase
  (`implied_vol_batch`'s `span<Status>`; the American batch's `LaneStatus`).
  These three have none — `black76_greeks`, `black76_value_and_vega` and
  `black76_price_from_lnfk` are `noexcept` total functions whose degenerate lanes
  collapse to the documented intrinsic result — so an all-`STATUS_OK` column
  would advertise a diagnostic carrying no information. Only a malformed *call*
  raises.
* **Greeks and value+vega agree with their scalars to the SIMD gate, not
  bit-for-bit; from-lnFK is bit-identical.** The first two dispatch to AVX2 at
  `n >= 4`; from-lnFK has no vector kernel. The gate is per output column —
  ~1e-6 absolute + 1e-7 relative on prices and Greeks, ~1e-5 absolute on the
  fused batch's `vega`.
* **`side` now also accepts a single `Side`, broadcast across the batch**, on
  every binding that takes a `side` column (the American batches included). Pure
  widening: a per-lane integer column behaves exactly as before, and a float
  column is still refused with `ErrorCode::InvalidArgument` rather than
  truncated onto `Side::Call`.

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

### REMOVED / NEW — the bespoke strangle-vs-varswap strategy is now a declarative spec (swap-lane DSL sprint)

**Removed.** `StrangleVsVarswapStrategy` + `StrangleVarswapConfig`
(`strangle_varswap.hpp/.cpp`), the 600-line
`atx-vol-strangle-varswap-driver`, and their test suite. The class was a
one-analysis special: its cycle lifecycle, swap sizing and signal mirror were
generic machinery trapped in bespoke code.

**New, in its place — the grammar now expresses the whole analysis:**

* `LifecycleSpec::Holding::FixedExpiryRestrike` — a cycle fixes ONE expiry
  (ceil-snapped onto `StrategySpec::session_ts`; the legs' shared
  `tenor.target_T` is the cycle tenor) and every entry tick restrikes the
  option legs at it; keep-strikes on soft resolution failures with the
  `skipped_restrikes` / `unopened_entry_steps` counters; `Entry` is the
  restrike cadence.
* `StrategySpec::swap_legs` (`SwapLegSpec` + `SwapSizeSpec`:
  `FixedQty` / `TargetVega` / `MatchGroupVega`) — one fair-struck swap leg per
  cycle, opened on the cycle-open step only, every refusal counted in
  `skipped_swap_cycles`. Empty `swap_legs` is bit-identical to the old
  grammar (the additive-lane rule).
* `swap_leg.hpp` — the reusable toolkit: `swap_contract_for_lot` (the engine's
  `SwapLot`→`DerivContract` transcription), `solve_cycle_swap` (fixing-count +
  bridge-priced fair strike + vega-targeted qty, fail-soft by contract), and
  `SwapSignalProbe` (the engine-accrual mirror behind the
  `swap_delta/gamma/vega/theta/rho` signal columns, NaN discipline included).
* `DeclarativeStrategy::signals` — emitted only when the spec carries swap
  legs: the probe's five columns + `options_vega` (the old `strangle_vega`,
  renamed lane-agnostic; `tools/render_strangle_vs_varswap.py` reads either
  spelling) + cumulative `skipped_restrikes` / `skipped_swaps`.

**Migration.** `examples/varswap_compare_example.cpp` is the old driver's
replacement: the full comparison as a ~20-line spec. Old config fields map
1:1 (`target_abs_delta` → the strangle selectors, `tenor_years` →
`tenor.target_T`, `contracts` → `FixedContracts`, `enable_swap_leg` → the
`swap_legs` vector).

**Why the numbers are trustworthy.** The deletion sat behind a track-parity
gate: old vs new through the same engine on the full XOM 2026 fixed-db window
(137 sessions, both legs, delta-hedged) — per-row `nav`/`pnl_total` within
7e-9 dollars, `swap_pv`/`swap_pnl` bit-identical, identical lot-id watermarks
and skip counters; plus synthetic-corpus parity including a dark-session
keep-strikes run. The example reproduces the reference track to the cent
(combined −7499.69 / swap −6065.38 / strangle −1434.31).

### FIXED — calendar level repairs no longer fabricate slice levels from extrapolated-wing crossings (fit fidelity budget + tradeable-overlap band)

**The defect.** Every parametric calendar repair — `arb_project_calendar_svi_pair`
/ `_essvi_pair` / `_c8_pair` at the `fit_slice_curve` serving seam, and the
surface-level `arb_project_calendar_svi` / `_essvi` in `run_surface_parity`'s
`CalendarRepair::Project` branch — closed w(k)-crossings by shifting or scaling
the longer slice's LEVEL (`a` / `theta`) by the WORST-CASE deficit over a fixed
k-band (±0.6 at the pair seam, **±3.0** in the Project branch). For slices
quoted to |k| ≲ 0.1–0.3 (every weekly and most mid-tenor chains), nearly that
whole band is extrapolation on BOTH slices: the closed forms' wings there are
unidentified by any quote, so the "crossing" was fiction — and the repair
converted it into a real ATM error, slice after slice, since each repaired slice
becomes the next pair's floor. The result passed every downstream gate
(shape-preserving shifts keep butterflies clean; calendar is clean by
construction; risk admission checks geometry, not fidelity to quotes) and was
stored. On the sp100-2026 database this served XOM 2026-02-18 mid-tenors at up
to **+25 vol pts over their own quotes** (43d ATM: quotes 28.4, served 53.4; the
SVI `a` shifted 0.0091 → 0.0331 by a k=±0.6 wing deficit), and — because the
fitted wing shapes flip day to day — produced the ±8–11 pt one-day ATM
spike-reverts on XOM/CVX that surfaced as artificial P&L spikes in the
strangle-vs-varswap backtest.

**The fix, structural.**
1. **Pair projections act only on the tradeable overlap** of the two slices'
   data-supported k-ranges (∩ the risk band) — the exact rule
   `SplineVolCurve::project_calendar` already applied; the parametric branches
   now follow it. `fit_curve_surface` tracks each committed slice's observation
   k-range and hands it to the next pair (`prev_data_k_range`, previously
   populated only for SplineVol). A crossing with no traded witness no longer
   moves any level.
2. **Every level repair carries a fidelity budget**
   (`kCalendarRepairMaxAtmShiftFrac = 0.10` of the slice's pre-repair ATM total
   variance, floor `1e-6`): a repair that would move the ATM further returns
   `Unavailable` and leaves the slice/surface untouched (all five projectors are
   now transactional). Failure flows into the existing honest paths: the slice
   is dropped (soft) or the risk candidate walks the family ladder.
3. **`CalendarRepair::Project` repairs over the certified band** (±0.5,
   `static_assert`-tied to both `RiskSurfaceValidationConfig` and
   `strip::kCertifiedWingHalfBand`), not the ±3.0 diagnostic grid — repair now
   covers exactly what admission checks. The ±3.0 grid remains as the
   `calendar_arb_free` DIAGNOSTIC and may now honestly report unrepaired wing
   crossings.
4. **The SVI fallback ladder reaches the dense rung** (`kFromSvi` gains
   `LinearVariance`, which the risk path substitutes with `ConvexDense`) — the
   one family list that did not descend to the direct-variance curve its own
   contract promises. Invisible while the fabricated repair force-admitted
   every SVI candidate; load-bearing now that rejection is honest.

**Effect on existing callers.** Numbers move wherever a calendar repair used to
fire on out-of-overlap or beyond-budget crossings — deliberately: those numbers
were fabrications. Boards whose parametric candidates genuinely cannot
reconcile quotes with in-band calendar monotonicity now serve the dense model
via the ladder (XOM 2026-02-18 does exactly this) or drop the offending slice.
Re-fit of the poisoned XOM cell: every tenor within ~1 vol pt of its own
quotes (worst tenor was +25.0). Gates: `ArbProjectCalendarPair.*Refuses*`,
`ArbProjectCalendarSvi/Essvi.Refuses*`,
`VolCurve.SviPairProjectionActsOnlyOnTheTradeableOverlap` /
`SviPairProjectionStillRepairsInsideTheOverlap`.

**Databases built before this fix carry the fabricated levels** — the carry-over
fingerprint does not cover the fitter, so a resume will NOT repair them (see
"Re-running at the corrected --r does NOT repair the database" in
docs/surface-db-build.md for the identical mechanism). Rebuild affected
symbols' partitions from the hive, or rebuild into a fresh root.

**Validated on a full XOM+CVX 2026 rebuild** (release binary, fresh root
`scratch-fitfix-2026`, 140 sessions): CVX 140/140, XOM 137/140 with three
honestly-rejected boards (2026-02-13 / 02-18 / 06-11: butterfly- or
calendar-inadmissible on every ladder rung; the backtest engine's documented
dark-day drop handles them). Daily 3M ATM fit noise: XOM 3.28 → **0.93**
vol pts/day, CVX 5.28 → **0.92** — both now indistinguishable from AAPL/MSFT —
with the spike-revert autocorrelation gone (lag-1 −0.50 → −0.17) and the
largest one-day ATM move 11.6 → 2.6 pts. The strangle-vs-varswap reference
run's legs land on the same scale (strangle −$1,434, swap −$6,065 over the
window). An AAPL one-cell control rebuild serves IVs identical to ~1e-10:
surfaces whose repairs never fired are numerically untouched.

### CHANGED — the variance strip now reads flat-vol tails beyond the certified wing band (wing clamp)

**What changed.** `var_swap_fair_strike` (and everything that prices through it:
`deriv_price` on every swap kind, `deriv_greeks`, the backtest swap lane's daily
marks) now clamps its SURFACE READS to a wing trust band. Strip nodes beyond the
band keep their true strikes but price under the BAND-EDGE vol — flat-vol tails
over the wings, never a truncated span, so the log-contract replication stays
complete. The band is `DerivConfig::wing_clamp_k`: `0` (the default) selects the
certified validation band `strip::kCertifiedWingHalfBand = 0.5` — the |k| ≤ 0.5
band `RiskSurfaceValidationConfig` actually certifies, `static_assert`-tied so
the two cannot drift — `> 0` is an explicit half-band, `< 0` restores the old
unclamped reads, `NaN` is `InvalidArgument`. A new provenance flag
`DerivFlags::WingClamped` (structural: "tails were in effect", set even when the
smile is flat and the clamp moves nothing) records it on every quote. The strip
span, node count, adaptive widening and truncation flags are untouched: this
clamps *reads*, not the grid.

**Why.** A parametric eSSVI/SVI slice serves an *unbounded linear-in-|k|*
extrapolation at any k, no quote ever disciplines it beyond roughly the ATM
region, and the fit pipeline certifies nothing outside |k| ≤ 0.5 — yet the
Standard strip integrated it to ±1.5 with 1/K weighting. On the sp100-2026 XOM
corpus that fiction read 82 vol at k = −1.0 (swinging ±22 vol pts/day), inflated
the 3M fair strike to ~38 vol against a ~30 ATM and ~29.6 realized, and put
~98% of the daily mark variance beyond |k| = 0.25 — the mechanism behind the
XOM 2026 reference run's swap leg bleeding −$42k with ±$90k single-day marks
and sign-flipping FD gamma. With the default clamp the same corpus prices
~33.9 with ~3.5 vol pts/day of mark noise, in line with the ATM's own 3.3.

**Effect on existing callers.** This moves K_var on ANY surface with wing
structure beyond ±0.5 — deliberately. A flat or near-flat surface is unchanged
(band-edge vol == wing vol), so the analytic `K_var == σ²` contracts all hold;
a caller whose wings ARE quote-disciplined and who wants them integrated raw
sets `wing_clamp_k = -1.0` (or any negative) and gets the pre-clamp number
bit-for-bit. Gate: `WingClamp` (tests/derivatives_test.cpp), including a
node-for-node flat-tail oracle at 1e-12.

**The XOM 2026 reference run, re-taken under the clamp** (same corpus, gen 225,
same window/config; artifacts in `xom-strangle-varswap-2026-wingclamp/`):
strangle **+$1,501.64 — bit-identical**, the option lane never touches
`DerivConfig`; variance swap **−$10,436.00** (was −$42,468.19); per-cycle swap
P&L +$32,009 / −$35,471 / −$6,974 against strangle +$80,743 / −$50,173 /
−$29,068 — the two legs finally live on the same scale. Daily swap-mark noise
$18.0k (was $24.5k); what remains is the in-band sp100-2026 fit noise (ATM
itself moves 3.3 vol pts/day on this corpus), which is the refit's job, not the
pricer's. `swap_gamma`'s sign still flips day to day — that is a CONVENTION
fact, not a defect: marks read the surface at fixed log-forward-moneyness, so
the smile floats with a spot bump and a var swap's FD gamma is numerical noise
around zero.

### NEW — strangle-vs-varswap comparison backtest + the XOM 2026 reference run (strangle-vs-varswap sprint, Tasks 1-5)

**What shipped.** `strangle_varswap.hpp`/`.cpp` add
`StrangleVsVarswapStrategy`, an `IStrategy` that runs one fixed-expiry,
daily-restriked 40Δ strangle against one uncapped variance swap struck fair and
sized to that strangle's **entry** vega, both on a single clock and
delta-hedged daily. Equal vega is enforced in DOLLAR terms (per-share vega ×
contracts × multiplier) at each cycle open, and the swap's strike comes from
the `deriv_price` bridge's `fair_strike_dec` — not the `PricedSurface`
`var_swap_fair_strike` — so strike and engine mark share one carry and the swap
opens at a bitwise `+0.0` PV. Per-step `swap_delta/gamma/vega/theta/rho`,
`strangle_vega` and cumulative `skipped_restrikes`/`skipped_swaps` counters ride
out as strategy signals. `examples/strangle_varswap_driver.cpp` (target
`atx-vol-strangle-varswap-driver`, `ATX_BUILD_EXAMPLES`) drives it over a
`SurfaceDb` and writes `track.tsv`; `tools/render_strangle_vs_varswap.py`
renders the five-panel comparison report.

**Effect on existing callers.** Additive only — a new header, a new example
target, a new Python tool, and one new test module. No library type, engine
path or existing test changed; `write_backtest_pnl_tsv`'s frozen column set is
deliberately *not* widened (its `{name, order}` is `static_assert`-pinned to the
RunArchive registry that feeds `ra_schema_hash()`), so `swap_pv`/`swap_pnl`
reach the TSV as signal columns instead and the schema hash is untouched.

**Operational contracts worth knowing.** A live variance swap makes the engine
fail the whole run closed on any missing board — `unpriced = ExcludeAndReport`
governs option lots only and is no escape for the swap lane. The driver
therefore probes every partition in the window and builds its clock from the
sessions where the symbol's surface exists; a partition the manifest lists whose
*archive will not open* is reported as an inconsistent database (exit 1), not as
a dark session, so a broken db can never silently shorten a measured window. The
final cycle of a corpus whose calendar the tenor outruns may legitimately run
one-legged (`skipped_swaps ≥ 1`, no swap lot). `swap_theta` is legitimately NaN
within one bump width of expiry while the swap is live, so liveness is read off
`swap_vega`.

**The reference run** (`XOM`, `sp100-2026` gen 225, 2026-01-02 → 2026-07-24,
139 sessions, 1 dark, three 91d cycles) is recorded for provenance:
strangle **+$1,501.64**, variance swap **−$42,468.19**, combined
**−$40,966.55**. The equal-vega identity held to 0 ULP at all three cycle opens,
`reconcile_nav` held on every row, and the leg split closes back to NAV to
1.5e-11.

**Read that run's economics with the corpus in mind.** The two legs' daily P&Ls
are essentially *uncorrelated* on this data (ρ = +0.038), and the swap's path is
the *noisier* of the two (daily σ $24,478 vs $21,739). That is a property of the
corpus, not of the strategy: each leg is independently reproducible from the raw
surface — the strangle from the 40Δ/ATM implied-vol move (R² = 0.69) and the
swap from a 1/K²-weighted replication of the variance strip (R² = 0.81) — but in
this corpus those two regions of the surface are themselves only ρ = +0.24
related, because the deep put wing carries ~27 vol points/day of fit noise
(vs 3.3 at ATM) that is *anti*-correlated with the ATM move (ρ = −0.39), and
every strike's daily IV change has lag-1 autocorrelation ≈ −0.5 — the signature
of independent per-day fit noise rather than a coherent vol path. A variance
swap integrates exactly that region. Treat the sp100-2026 wings as unfit for
strip-sensitive P&L attribution until they are refit.

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
