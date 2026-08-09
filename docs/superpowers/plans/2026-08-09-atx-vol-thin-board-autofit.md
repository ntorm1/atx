# atx-vol: fit any valid American equity snapshot, and resolve config automatically

**Branch:** `feat/vol-thin-board-autofit`  ·  **Worktree:** `C:\atx-wt\pool-10`
**Agent profile:** every implementation task follows `C:\atx\.agents\cpp\agent.md`.
**Date:** 2026-08-09

## Goal

Two properties, stated as acceptance criteria rather than aspirations:

1. **Universality.** Given any market snapshot that contains a well-formed
   American equity option chain, the fitter returns a usable surface or an
   explicit, typed refusal. "Well-formed" is defined below. Silent degradation
   and untyped hard failures both count as violations.
2. **Automatic configuration.** When the caller supplies no curve family and no
   preset, the library resolves a configuration that is *defensibly near-optimal
   for that board*, and reports what it chose and why. Today it resolves a
   configuration that is near-optimal for a mega-cap board and applies it to
   everything.

## 1. Evidence

Measured on real OPRA `cbbo-1m` snapshots. Two corpora:

- **Control:** the S&P 100, 4 sessions (2026-07-20..23), 416 board-sessions,
  836,509 legs, snapshot 19:55Z. Root `C:\atx-data\opra-hive-bench`.
- **Test:** `lqbench`, a 240-name liquidity-stratified universe (tier A mega/ETF,
  B large, C mid, D small, E dividend/borrow probes, V vol products), snapshot
  minutes 15:55 and 10:30 ET. Root `C:\atx-data\opra-hive-lqbench-*`.
  225 of 240 names have listed options.

### 1.1 The sparse floor is a strike-count test, not a liquidity test

`sparse_validation_floor = 600` two-sided legs force-demotes a board to
`IlliquidSmallCap` (Fast preset, SVI, cross-validation off). Share of boards
demoted:

| corpus | A | B | C | D | E | V | S&P 100 |
|---|---|---|---|---|---|---|---|
| demoted | 0% | 6.7% | 64.0% | 70.1% | 100% | 50% | **8.7%** |

The ten S&P 100 names it demotes are DUK (250 two-sided legs), KHC (403),
SYK (421), AMT (517), CMCSA (541), T (550), CL (565), SO (565), MO (572),
BMY (599) — dividend-heavy mega caps with few listed strikes, not illiquid
names. Meanwhile two thirds of everything below mega-cap is demoted regardless
of how well quoted it is. The floor is also steep: 300 → 1.0% of the S&P 100,
600 → 8.7%, 1000 → 39.4%.

### 1.2 Spread tiers cannot discriminate below large cap

| tier | median legs | median two-sided | % two-sided (p10) | median rel spread | p90 rel spread |
|---|---|---|---|---|---|
| A | 3,784 | 3,595 | 93.5 (84.8) | 0.049 | 0.50 |
| B | 1,334 | 1,193 | 89.5 (82.3) | 0.103 | 0.67 |
| C | 542 | 477 | 86.9 (79.6) | 0.203 | 0.89 |
| D | 306 | 248 | 83.5 (70.7) | 0.270 | 1.02 |
| V | 868 | 756 | 88.8 (84.6) | 0.326 | 1.14 |

Even the S&P 100 has a median relative spread of 10.1% (p90 21%, max 72% for
KHC) — "penny-wide" is false for the control corpus, let alone the test one.
Tier D's median board is at 27% and its p90 exceeds 100%. Fixed tiers at
0.02/0.05/0.15/0.40 put nearly every C and D name in the widest bucket, so the
classifier's spread signal carries almost no information exactly where it is
being asked to make the hardest call.

### 1.3 The one-sided-quote rule removes most data from the thinnest boards

Requiring `bid > 0 && ask > bid` discards a median 11% of S&P 100 legs (p90 19%,
max 29%) and progressively more as boards thin: two-sided share falls 93.5% (A)
→ 83.5% (D), p10 70.7%. Worst single boards on 2026-08-03: ANVS keeps 32 of 128
legs (25%), GNK 32 of 56 (57%), KPTI 54 of 88 (61%), KURA 77 of 123 (63%).

There are zero crossed or locked quotes and zero literal zero-bids in the corpus:
an unset side is stored as `INT64_MIN`, so "one-sided" and "zero bid" are the
same case, and it is the only case.

### 1.4 The per-expiry floors are untested by the large-cap corpus and fatal on some small caps

Slices surviving each floor:

| filter | control (1,676 slices) | test (2,779 slices) |
|---|---|---|
| `two_sided >= 5` (kMinPreparedFitRows) | 100.0% | 99.28% |
| `two_sided >= 8` (selector prep floor) | 100.0% | 98.20% |
| paired strikes >= 3 (carry solve) | 100.0% | 98.27% |
| `T > 0.019` (selector short-dated cut) | 92.5% | 93.4% |
| all combined | 92.5% | 91.36% |

The smallest slice anywhere in the S&P 100 is 17 two-sided legs, against floors
of 5 and 8 — **the floors are unreachable on the control corpus.** Tier D's 5th
percentile slice is 6 legs, and 6.9% of tier-D slices fall below the selector's
floor of 8.

Consequences at the board level: boards with fewer than 5 selector-eligible
expiries are 0% of A and B, 6.7% of C, and **20.9% of D**. Two boards have *zero*
selector-eligible expiries — ANVS (12 listed expiries, all ineligible) and OGI
(4, all ineligible). Median expiry count falls from 24 (A) to 9 (D), and the 5th
percentile board carries only 4 expiries in total, which collides directly with
`min_expiry_coverage = min_holdout_coverage = 1.0`: there is nothing left to hold
out.

### 1.5 Reading

The floors, tiers and coverage requirements were all calibrated against boards
that cannot exercise them. Every one of them is invisible on the S&P 100 and
binding on tier C/D. This is why the failure mode is not "the fit is slightly
worse on small caps" but "the routing decision is made on a signal that has gone
constant, and the fallback behaviour has never been exercised."

### 1.6 Measured fit outcomes

`universe_autofit --preset robust`, lqbench 2026-08-03 15:55 ET, 240 names:

| tier | n | % ok | median rows | failures |
|---|---|---|---|---|
| A | 15 | 100.0 | 3,595 | 0 |
| B | 45 | 100.0 | 1,193 | 0 |
| C | 96 | 91.7 | 436 | 1 |
| D | 75 | 77.3 | 215 | 9 |

Every failure is tier C/D. The control corpus (S&P 100, 520 board-fits) returns
519 successes; its one failure is `NEE` on 2026-07-23 with
`select_curve: no common prepared holdout keys` — **the same defect** that kills
CIFR (1,157 rows) and VKTX (605 rows) in the test corpus. It is a latent bug that
thin boards merely hit far more often.

Three terminal failure classes:

1. `select_curve: no common prepared holdout keys` — CIFR, VKTX. Not thin boards.
2. `fit_curve_surface: no expiry produced a usable slice` — OGI (23 rows),
   ALLO (27), ANVS (32), SANA (32), CHGG (40), KPTI (54). Exactly the boards the
   per-expiry floor analysis predicts.
3. `Unavailable: no well-conditioned co-terminal expiry to imply spot` — GNK,
   ATAI. Fails at chain construction, before any fitting.

### 1.7 The diagnostics blackout

Quality fields are populated **only on the eSSVI path**. Separation is perfect on
both corpora — 414 eSSVI boards have diagnostics, 105 SVI/linear-variance boards
are all-zero on the control; 98 of 215 (45.6%) are all-zero on the test corpus.
These are zero-*filled*, not bad fits: SPY (12,893 rows) is in the zero set.

Cause (confirmed in code): the default Mark `FitAdmissionPolicy` has all-zero
floors, so `fit_admission_consumes_parity` is false and `score_parity` resolves
to false. A second-order effect is that `quote_coverage` then collapses to a
constant 1.0, silently defeating the `min_served_quote_coverage = 0.50` floor.

Consequence: **for 45% of the test universe the library reports no fit-quality
information at all**, concentrated exactly on the boards whose quality is most in
doubt. Any aggregate median over "all ok boards" is contaminated by these zeros.

> **Correction (W3-A, §4.6).** This section originally went on to guess that the
> `calendar_arb_free == 0` rate was "almost certainly the same reporting gap
> rather than real arbitrage." That guess was WRONG and is withdrawn. The flag is
> computed unconditionally on both the eSSVI driver (`surface_parity.cpp:557`)
> and the curve route (`session.cpp:1246`), and never reads `score_parity`.
> W3-A measured it on 218 boards with the scoring fix and with it stashed: 196
> boards `0 -> 0`, 22 boards `1 -> 1`, zero transitions. The rate is real
> arbitrage on the served surface, and it needs its own wave — see §6.

### 1.8 Ablations — establishing causality

Same session, same binary, one flag changed:

| config | ok | zero-diag | measurable | median in-band | fit CPU | real failures |
|---|---|---|---|---|---|---|
| base (`robust`) | 215 (89.6%) | 98 (45.6%) | 117 | 0.987 | 119.5 s | 10 |
| `--sparse-floor 40` | 198 (82.5%) | 7 (3.5%) | 191 | 0.988 | 131.3 s | **27** |
| `--preset accurate` | 215 (89.6%) | 95 (44.2%) | 120 | 0.990 | 153.1 s | 10 |
| `--min-direct-confidence 1.01` | 214 (89.2%) | 107 (49.8%) | 107 | — | 162.5 s | 11 |

**Lowering the sparse floor to 40** moves 79 boards from SVI to eSSVI. All 79
gain diagnostics; none lose them. They fit *well* — tier C in-band 0.996,
chi2 0.163; tier D in-band 0.982, chi2 0.224. Aggregate measurable quality is
unchanged (0.987 → 0.988), so nothing regresses in quality; the library simply
measures 191 boards instead of 117, for +13% fit CPU. The guard was routing
perfectly fittable boards onto a degraded, unmeasurable path.

**But 17 boards go from `ok` to `fit_error`, every one of them
`select_curve: no common prepared holdout keys`** (n_rows 51 to 590, median 208;
11 tier D, 5 tier C, 1 vol product). The sparse guard is *load-bearing*: it is
masking the selector's inability to fall back. This dictates the order of work —
**the fallback must be fixed before the guard is relaxed.**

**`--preset accurate`** changes nothing structural: identical routing, identical
failures, 28% more CPU for a marginal chi2 improvement. SparseGuard fires 106
times regardless of preset because it runs before preset resolution.

**Forcing cross-validation on every board** (`--min-direct-confidence 1.01`)
eliminates the ticker-prior and board-feature routes but leaves **SparseGuard
firing all 106 times**, because the guard sets `needs_cross_validation = false`
and so bypasses the confidence mechanism entirely. It costs 36% more CPU and
yields one *extra* failure and *more* unmeasured boards.

Neither of the two knobs a caller can actually reach — preset and confidence
threshold — is the lever. The sparse guard is, and it is gated behind the
selector-fallback bug. That is the whole sprint in one sentence.

### 1.9 The resolved configuration is not reproducible

Comparing two sessions two days apart (2026-08-03 and 2026-08-05, same binary,
same flags, 218 names present in both), **37 symbols change curve family or
profile.** Flip rate by tier:

    A 0%   B 53.3%   C 8.0%   D 9.8%

The instability is *worst in tier B* — the tier that runs cross-validation most
often (62% of boards) — and lowest in C/D, which are pinned by SparseGuard and so
are stably degraded rather than stably correct. The control corpus shows the same
effect: at fixed preset the eSSVI share moves 93 → 79 → 72 → 77 across four
consecutive S&P 100 sessions.

Two distinct patterns are visible. Boards like ORCL, AMAT, HOOD and COIN change
*profile* without changing family, with in-band moving by less than 0.05 — that
is classification noise around the 0.5-confidence tie, harmless in itself but a
sign the classifier is reading a signal that carries no information. Boards like
SNDX, RSI, PLAY, LAZR, GENI, MS, MCD and LOW change *family* with an in-band
spread of exactly 1.000, which is not a quality swing at all: it is the
diagnostics blackout switching on and off as the board crosses between the eSSVI
and SVI routes.

This matters for the second goal of the sprint. "Automatically resolve an optimal
configuration" is not satisfied by a resolver whose answer changes between
adjacent snapshots of the same underlier for reasons unrelated to the board.

### 1.9b The terminal failure is brittle, not a thinness threshold

Comparing the 10:30 ET and 15:55 ET snapshots of the *same session* makes the
`select_curve` defect unambiguous. These boards are far above the 600-leg sparse
floor, so they escape SparseGuard, reach the selector, and are killed by it:

| symbol | session | 10:30 ET | 15:55 ET |
|---|---|---|---|
| **FCEL** | 2026-08-05 | **ok**, 787 rows, eSSVI, 10 slices | **fit_error**, **787 rows** |
| VKTX | 2026-08-03 | ok, 138 rows, SVI | fit_error, 605 rows |
| CIFR | 2026-08-03 | ok, 1,175 rows, eSSVI, 16 slices | fit_error, 1,157 rows |
| WULF | 2026-08-03 | **fit_error**, 1,175 rows | **ok**, 1,171 rows, 20 slices |
| OSCR | 2026-08-05 | ok, 909 rows, 15 slices | fit_error, 902 rows |
| ROKU | 2026-08-05 | fit_error, 663 rows | ok, 736 rows, 14 slices |

Every failure is `NotFound: select_curve: no common prepared holdout keys`; every
success is `decision_source = CrossValidation`.

**FCEL is the decisive case: an identical 787-row board fits at 10:30 and hard-fails
at 15:55 on the same day.** WULF and ROKU fail in the morning and succeed in the
afternoon; CIFR and OSCR do the reverse. A row-count change on the order of 1%
flips the outcome in both directions.

This is not a thin-board capacity limit — it is a brittle condition in the
even/odd holdout key construction, and it makes the failure effectively arbitrary
with respect to anything a caller can observe or control. It also rules out
"these boards are too thin to fit" as an explanation, since the *same* board fits
hours earlier with the same data.

Secondary observation: the morning snapshot is systematically worse. On both
sessions, 10:30 yields ~205 successes against ~215 at 15:55, and SparseGuard
fires more often (117 vs 106 on 2026-08-03) because fewer legs are two-sided
early in the day. Boards like CRON, ARCT and KURA have literally zero rows at
10:30 and 30-77 rows by 15:55. Auto-config is therefore time-of-day sensitive,
which matters for anyone fitting near the open.

Intraday family/profile flip rate is 19.2% (2026-08-03) and 18.2% (2026-08-05),
i.e. comparable to the two-day interday rate — the instability is not a
day-over-day drift, it is present within a single session.

### 1.10 What remains once routing is fixed — and a metric warning

Re-measuring the `--sparse-floor 40` run over its 191 *measurable* boards:

| tier | n | median in-band | p10 in-band | median chi2 | **median rmse_vol (σ units)** | median slices |
|---|---|---|---|---|---|---|
| A | 12 | 0.773 | 0.464 | 1.438 | **0.047** | 23 |
| B | 43 | 0.980 | 0.894 | 0.238 | **0.022** | 16 |
| C | 82 | 0.994 | 0.953 | 0.163 | **0.035** | 12 |
| D | 46 | 0.994 | 0.946 | 0.164 | **0.056** | 10 |
| V | 3 | 0.949 | 0.943 | 0.290 | **0.061** | 12 |

Only 6 of 191 boards remain below 0.75 in-band, and **every one of them is a mega
cap**: TSLA 0.356, NVDA 0.451, AMZN 0.577, PLTR 0.615, AVGO 0.672, AAPL 0.724.
All three boards with chi2 > 5 are tier A (AVGO 48.0, TSLA 14.2, NVDA 8.6).

**Do not read this as "small caps fit better than mega caps."** In-band is
measured against the bid-ask width, so it is spread-relative and systematically
flatters wide markets: a tier-D board with a 27% median relative spread has an
enormous target to hit. The absolute measure tells the opposite and correct
story — median `rmse_vol` is 0.022 for tier B against **0.056 for tier D**, i.e.
2.2 vs 5.6 vol points, so small-cap surfaces really are about 2.5× less accurate.
(`rmse_vol` is `sqrt(Σ r² / N)` with `r = σ_model − σ_mkt` in σ units —
`include/atx/vol/fit_metrics.hpp:123` — so it is unweighted across all legs and
is not comparable to the vega-weighted ~1.0 vol pt SPY figure in the README.)
The two metrics
disagree because they ask different questions, and only RMSE is comparable across
liquidity tiers. The repo README already warns that in-bid-ask is a metric trap
at penny spreads; the converse trap applies here.

The substantive conclusion stands regardless of metric: **the small/mid-cap
problem is a routing and plumbing problem, not a pricing-math problem.** Once
boards are routed to a family that can represent them and prepared with a filter
appropriate to their width, they fit. What is left afterwards is a mega-cap
accuracy question, which is a different sprint.

## 2. Code-level defect inventory

A full line-cited audit of 38 findings is the companion to this plan. The ones
that drive the work below, with the mechanism each explains:

| ID | Site | Defect | Explains |
|---|---|---|---|
| F01 | `src/fit_policy.cpp:221-233` | `n_live_quotes < 600 && source==BoardFeatures` → `IlliquidSmallCap`, `needs_cross_validation=false`, conf forced to ≥0.80 | 49% SparseGuard rate; §1.1 |
| F02 | `src/fit_policy.cpp:57-59`, `src/session.cpp:1228`, `src/curve_fit.cpp:300` | eSSVI ⇒ **permissive** `LegacyEssviCompatibility` prep; every other family ⇒ the **strict** full `CalibOpts` cascade. So the thin-board route (SVI) gets the harshest quote filter | why sf40 (SVI→eSSVI) rescues boards |
| F03 | `src/curve_selector.cpp:55-61` | `production_selector_config()` has exactly one candidate, `Essvi`. `default_selector_candidates()` (5 families) has no production caller | the "multi-family selector" is a one-family admission gate |
| F04 | `src/pricer_fitter.cpp:725-734` vs `:1308-1331` | legacy path returns `Err` on selector failure; v2/risk path falls back to `decision_->curve` | the 17 regressions, CIFR, VKTX, NEE |
| F05 | `include/atx/vol/curve_selector.hpp:128-129` | `min_expiry_coverage = min_holdout_coverage = 1.0` | one failed deep-wing re-Americanization sinks the whole board |
| F06/F31 | `src/curve_selector.cpp:265`, `detail/prepared_fitting.hpp:73`, `src/calib.cpp:53` | three independent hard-coded floors: selector 8, `kMinPreparedFitRows` 5, `kMinObs` 5; none configurable. `CalibOpts::min_obs_per_slice{4}` exists but is unreachable from the auto-fit path | slices of 5-7 rows silently dropped from selection |
| F07 | `src/curve_selector.cpp:194` | expiries with `T <= 0.019` (≈6.9 days) discarded unconditionally | front expiry lost on 4-expiry boards |
| F11 | `src/pricer_fitter.cpp:647-656`, `include/atx/vol/fit_policy.hpp:122-147` | `score_parity=false` by default; also forces `quote_coverage ≡ 1.0`, defeating `min_served_quote_coverage=0.50` | §1.7 blackout |
| F21 | `src/calib.cpp:119-124`, `src/prepared_fitting.cpp:59-61` | only the OTM leg per strike is ever used; a strike whose OTM leg is unquoted contributes nothing even when its ITM leg is a clean two-sided quote | up to half a thin board invisible |
| F22 | `src/profile.cpp:518`, `src/calib.cpp:110`, `src/curve_selector.cpp:290`, `src/deamer.cpp:386` | zero-bid/one-sided legs invisible to classification, FitObs, holdout and carry — and inconsistently so (classification rejects a locked market, legacy prep accepts it) | §1.3 |
| F23 | `include/atx/vol/deamer.hpp:321-326` | `carry_atm_band = 0.06` of spot; on an \$18 stock with \$2.50 spacing that is ±\$1.08 — often zero strikes. `min_confident_borrow_pairs = 3` | carry failures |
| F30 | `include/atx/vol/surface_parity.hpp:237`, `include/atx/vol/calib.hpp:299` | `per_slice_legacy_prep_fallback` and `per_slice_linear_fallback` — two purpose-built thin-slice rescues — default false and are **unreachable from `PricerConfig`** | `no expiry produced a usable slice` |
| F34 | `src/pricer_fitter.cpp:162-164` | invariant grid fixed at 97 points over k ∈ [-0.60, +0.60] | a board quoting ±12% is judged on ~80 extrapolated points |
| F35 | `src/opra_panel.cpp:255-301` | spot implication *does* accept zero-bid legs, and picks the ATM anchor by `argmin abs(call_mid - put_mid)` — so a zero-bid put is *preferentially* chosen as the anchor | GNK, ATAI |
| F09 | `src/pricer_fitter.cpp:636`, `:1275` | only `profile.calib` is copied; **14 of 16 `Profile` fields have no production reader**, including the whole `FilterOpts` thin-board accommodation. `arb_filter_quotes_ex` / `prefit_filter_underlier` have no non-test caller | every per-profile thin-board knob is inert |
| F27 | `src/profile.cpp:561` | `kUflagHtb` is never written in production, so `HtbDividendName` is unreachable from classification | HTB handling is dead |

## 3. Design

Four principles, each traceable to the evidence above.

**P1 — No terminal failure without a documented refusal.** Any board that
reaches the fitter either yields a surface or returns a typed refusal naming the
board condition that caused it. Selector failure is advisory, never fatal
(mirroring the v2 path that already does this correctly).

**P2 — Route on identifiability, not on volume.** The question "can this board
identify this many parameters?" is a function of near-money strike count, k-span
and per-expiry depth — not of absolute leg count. Every absolute-count and
absolute-spread threshold becomes relative (per expiry, in ticks, against strike
spacing).

**P3 — Preparation strictness must not ride on family choice.** F02 is an
accident of implementation history; it makes the thin-board route the strict one.
Prep policy becomes an explicit, profile-driven input.

**P4 — Always measure.** A published surface always carries fit diagnostics.
Unmeasured is treated as a defect, not as a pass.

## 4. Task breakdown

All tasks execute in the worktree `C:\atx-wt\pool-10` on branch
`feat/vol-thin-board-autofit`, following `C:\atx\.agents\cpp\agent.md`: TDD
test-first, `/W4 /permissive- /WX`, `// SAFETY:` on every deviation,
clang-format enforced, **clang-tidy is disabled — do not run it**. Build only
through the wrapper (`powershell scripts\atx-build.ps1 ...`), never raw
cmake/ninja, and use `powershell`, not `pwsh`. Run **targeted** tests
(`-Ctest -R <Suite>`), not full suites.

Tasks are ordered by dependency. **W1-A must land before W2-A**: the ablation
proves that relaxing the guard without the fallback converts 17 successes into
hard failures.

### Wave 1

**W1-A — Make curve selection advisory (F04, F03, F05).**
Mirror the v2/risk behaviour at `src/pricer_fitter.cpp:725-734`: on
`select_curve` failure fall back to `decision_->curve` and let
`FitAdmissionPolicy` and the existing fallback ladder decide, instead of
returning `Err`. Expose the selector coverage floors and relax them for the
single-candidate case (`curve_selector.hpp:128-129`); a 1.0 coverage requirement
is a cross-candidate comparability guarantee and is meaningless with one
candidate. Restore a bounded candidate ladder in `production_selector_config()`
(`Essvi`, `Svi`, `LinearVariance`) under the existing `time_budget_ms`.
*Acceptance:* CIFR, VKTX and NEE fit; no `select_curve` error survives in the
benchmark; the 17 sf40 regressions disappear.

**W1-B — Admit one-sided and zero-bid quotes (F21, F22, F35).**
Fall back to the ITM leg via put-call parity when a strike's OTM leg is unquoted,
rather than skipping the strike. Admit zero-bid legs as one-sided price bounds
(upper bound = ask) in the loss and count them separately in classification.
Reconcile the locked-market inconsistency between classification/holdout (reject
`ask == bid`) and legacy prep/carry (accept it) — pick one and document it.
Require both legs two-sided for the PCP spot anchor so a zero-bid put can no
longer be preferentially selected.
*Acceptance:* the median board recovers a measurable share of the 11% (large cap)
to 17% (tier D) of legs currently discarded; GNK and ATAI construct a chain.

### Wave 2

**W2-A — Replace the sparse guard with an identifiability test (F01, F13, F14, F29).**
Depends on W1-A. Replace the `n_live_quotes < 600` demotion with a test on
near-money depth and per-expiry strike count. Use `n_atm_quotes`, which is
already computed at `profile.cpp:522` and then discarded, and narrow its ±50%
band to something that means "near the money". Normalise the quote-density tiers
(`profile.cpp:397-411`) by expiry count. Replace the first-256-leg spread
reservoir (`profile.cpp:486`) with an exact median or an expiry-stratified sample.
*Acceptance:* SparseGuard rate falls from 49% to under 10%; DUK/KHC/SYK/AMT/CMCSA
and the other seven S&P 100 names are no longer classified `IlliquidSmallCap`;
no new fit failures against the W1 baseline.

**W2-B — Decouple prep strictness from family (F02, F30).**
Make the preparation policy an explicit input rather than a consequence of
family choice. Expose `per_slice_legacy_prep_fallback` and
`per_slice_linear_fallback` through `PricerConfig` and default the legacy-prep
rescue on for non-eSSVI families.
*Acceptance:* the six `no expiry produced a usable slice` boards (OGI, ALLO,
ANVS, SANA, CHGG, KPTI) either fit or return a typed refusal naming the
condition.

### Wave 3

**W3-A — Close the diagnostics blackout (F11, F12).**
Force `score_parity` on whenever a selector routed the board or the profile is a
thin-board profile; fix the `quote_coverage ≡ 1.0` collapse at
`pricer_fitter.cpp:437-443` so `min_served_quote_coverage` actually applies. Add
a `Degraded` surface state (or a Mark-level `min_expiry_coverage`) so a 1-of-6
expiry surface cannot report `Healthy` with `reasons = None`.
*Acceptance:* zero-diagnostics rate below 5% on the benchmark; a board with 5 of
6 expiries starved no longer reports `Healthy`.

**W3-B — Unify the observation floors (F06, F31, F07).**
Thread `CalibOpts::min_obs_per_slice` through all four enforcement sites; make
the selector floor `>= fit floor` by construction rather than by coincidence.
Make the `T > 0.019` cut relative — drop the front expiry only when enough others
survive.
*Acceptance:* one configurable floor; no expiry is dropped from selection that
the fit path would have accepted.

### Deferred (evidence-backed but not on the critical path)

W4 — carry/spot robustness (F23: scale `carry_atm_band` by strike spacing,
report a wider confidence interval instead of failing closed); bound the
invariant grid to the board's quoted k-range (F34); classifier realism —
tick-based spread tiers, weeklies gate, tie-break toward the less liquid bucket,
a real confidence score instead of one that can only take the values 0.5 and 1.0
against a 0.70 threshold (F15-F20); wire or delete the 14 dead `Profile` fields
(F09); populate the HTB flag from the carry solve's implied borrow (F27).

Also fix `examples/universe_autofit.cpp`: `--symbols-file` does not skip `#`
comment lines, so comment text is parsed as ticker symbols.

## 4.1 Terrain notes and hazards

Established by a separate control-flow audit. Read these before starting any task.

**The design of record is stale.** `docs/superpowers/specs/2026-07-05-atx-vol-breadth-expansion-design.md:172-198` is wrong in several load-bearing ways, and the code is the authority:

- **C8 is fully wired** as `VolCurveKind::C8 = 4` and is reachable from the auto-route at `src/fit_policy.cpp:44-46`. It is not a pending wiring task.
- A sixth kind, **`SplineVol = 5`**, exists and the spec never mentions it.
- **`LinearVariance = 3`** is live and is the dominant production route for index/ETF and dense-event boards, yet the spec omits it from its "live" table.
- **CStar and Wing are genuinely unwired.** Wing is an empty C-ABI numeric-parity tag with no calibrator at all.
- **S3 is not a curve family** — it is a synthetic known-truth generator for fixtures (`include/atx/vol/s3.hpp:110`). "Wiring S3" is not a task.
- The spec's claim that the candidate menu is keyed by `ProfileKind` is false: **`selector_candidates_for_profile` does not exist.** The profile→family decision happens before and outside the selector, at `src/fit_policy.cpp:28-78`, and resolves to a single kind.

**Hazard for W2-A:** `src/corpus.cpp:483-484` folds `sparse_validation_floor` and `dense_node_cap` into the corpus fit-config **fingerprint**. Changing either invalidates every stored manifest, and it surfaces as `corpus_test.cpp` / `surface_db_*` mismatches rather than as a policy-test failure. Budget for that and say what you did about it.

**Hazard for W2-B:** the two per-slice rescues are not merely defaulted off — `per_slice_linear_fallback` (`include/atx/vol/calib.hpp:299`) and `per_slice_legacy_prep_fallback` (`include/atx/vol/surface_parity.hpp:237`) are set nowhere outside `tests/curve_fit_parallel_test.cpp` and one example. There is no `PricerConfig` route to either.

**The selector prepares more strictly than the fitter it selects for.**
`src/curve_selector.cpp:256-262` pins candidate scoring to the strict `Configured`
preparation policy unconditionally, while the default eSSVI serve route prepares
under the permissive `LegacyEssviCompatibility`. The site's own comment defends
the pin as giving a fair comparison between families, which is sound in
isolation — but the consequence is that on a thin board the strict cascade can
leave too few prepared rows per expiry for the even/odd holdout split to find a
key present in both halves, so **selection fails on a board the fitter would have
fitted without complaint**. This is the most likely mechanism behind
`no common prepared holdout keys`.

W2-B widens the gap (it lets thin boards be *served* under the permissive
population), so after both tasks land the selector's held-out comparison no
longer measures the fit that actually ships. Someone must own this explicitly:
either keep `Configured` for comparability and rely on the W1-A fallback to make
selection failure survivable, or score each candidate under the policy it will
actually be served with. It should not be left implicit.

**Diagnostic reach:** `run_surface_parity` — the eSSVI/default driver — can only ever emit 6 of the 10 `ExpiryFitOutcome` values. It can never report `FittedFallbackCurve`, `FittedLegacyPrep`, `PrepUncovered` or `FitRefusedCalendar`. Any acceptance criterion phrased in terms of those outcomes is unobservable on the default path.

**Silent breadth loss worth knowing about:** negative-carry Andersen-Lake returns `NotImplemented` (`src/american.cpp:2043`, `:2052`, `:2057`, `:2125`). It never surfaces as its own outcome — each failing quote drops with `ObsRejectionReason::Deamericanization` and the slice degrades into `PrepStarved`. On HTB and negative-rate names this looks like thinness but is not.

**Testing caveat:** the real-data corpora at `C:/atx/data/spy_fit_slices` and `data/vol_breadth_slices` **do not exist on this machine**, so `SpyFitCorpus.*` and `OpraBreadthCorpus.*` currently `GTEST_SKIP`. A green local run has therefore not exercised the real-board gates — which is precisely why every task in this sprint must verify against the staged OPRA benchmark, not only against ctest.

**Narrow test invocation.** `atx-build.ps1` has no test-filter argument of its own; unrecognised args are forwarded verbatim to ctest. Run the copy of the script *inside the worktree* — the wrong-tree guard at `atx-build.ps1:98-112` refuses otherwise. `PricerFitterPolicy.*` is fast; `PricerFitterTest.*` is slow.

```powershell
powershell -File C:\atx-wt\pool-10\scripts\atx-build.ps1 build atx-vol-tests
powershell -File C:\atx-wt\pool-10\scripts\atx-build.ps1 -Ctest -R 'FitPolicy\.|CurveSelector\.|FitAdmission\.|PricerFitterPolicy\.'
```

PowerShell prefix-binding steals `-P`, `-D`, `-J`, `-G`, `-B`; `-R`, `-L`, `-E`, `-N` are safe.

**Tests that pin current auto-config behaviour** and will need deliberate updating: `tests/pricer_fitter_test.cpp:286-290` (asserts the production selector has exactly one candidate), the five routing pins in `tests/fit_policy_test.cpp:57,66,79,91,108`, `tests/spy_bidask_regression_test.cpp:163-166`, and `tests/surface_db_populate_policy_test.cpp:48-62`. Note also that `fit_policy_test.cpp` uses a 400-leg fixture, so several of its thresholds are coupled to that geometry, and the `0.25` spread arm of the opening guard has **no test coverage at all**.

## 4.2 W1-A outcome (commit `d591457`) — root cause corrected

**The premise in §1.9b was wrong about the mechanism, and the correction matters.**
`no common prepared holdout keys` is not a holdout-split problem. Instrumenting
the refusal to name its own counts shows every real-data occurrence is identical:

    sampled=8 prepared=0 holdout_keys=0

Zero of the eight sampled expiries survive the selector's `prepare_expiry`, so
the even/odd split never runs at all. Confirmed by experiment: `--pin essvi`
fits CIFR and VKTX cleanly, while `--pin svi` and `--pin linear-variance` both
die with `no expiry produced a usable slice`.

The mechanism is exactly the asymmetry flagged in §4.1 — `curve_selector.cpp`
pins `PreparedObservationPolicy::Configured` (the strict `CalibOpts` cascade)
while the eSSVI route it is selecting *for* is **served** under permissive
`LegacyEssviCompatibility` prep. On wide-spread boards the strict cascade drops
each expiry below the selector's 8-row floor. That is why FCEL flips on a 1%
row-count change: its expiry counts sit right on the floor. The sharpest case is
WULF at 10:30 — the selector prepared 0 of 8 expiries on a board the fitter then
fit with **20 slices**.

The prep pin was deliberately **kept** as `Configured`, with the cost documented
at the site: scoring each candidate under its own serving policy is the honest
fix but destroys cross-candidate comparability, and it belongs to W2-B. The
fallback is what makes the mismatch non-fatal in the meantime.

**Results.** `select_curve` errors go to zero on all four lqbench snapshot cells
(15:55 and 10:30 × 2026-08-03 and 08-05), with ok counts 215→216, 215→218,
205→207, 206→208 and **zero regressions and zero family changes**. Recovered:
CIFR, FCEL, OSCR, VKTX, UVXY, WULF, ROKU. Large-cap control **415 → 416/416**,
NEE 2026-07-23 recovered as predicted, zero family changes across 415 boards,
eSSVI in-band median unchanged at 0.9448.

**W2-A is unblocked**: `--sparse-floor 40` now gives 198 → **216** ok with all 17
named regressors recovered and zero regressions against either baseline.

**F03 resolved against the brief, on evidence.** Making the three-family ladder
the production default moves 26 boards (ORCL, UNH, MA, HD, CAT, AVGO, NFLX, WFC,
VZ, PLTR…) off eSSVI onto SVI/LinearVariance, and every one then reports all-zero
diagnostics — measurable boards fall 117 → 91 for 1.37× CPU with no in-band gain.
That is a direct regression against W3-A and principle P4. The ladder is
therefore implemented and available but **the production default stays
single-candidate**, and `time_budget_ms` stays 0 so the chosen family cannot
depend on host load.

**Two new facts.** (1) `select_curve` has two refusal sites, not one; only the
`prepared=0` site fires on real data, so the coverage relaxation (F05) is
latent-robustness rather than a measured fix. (2) VKTX at 15:55 on 2026-08-03 now
surfaces a previously-masked defect: `arb_project_calendar_essvi: slice 10
(T=0.5477) needs a cumulative ATM level scale of 1.2799, beyond the fidelity
budget 1.100000`. It fits at the other three cells and fits under `--pin essvi`,
so this is the profile's `CalibOpts` interacting with eSSVI calendar repair —
F09/W2-B territory, not selection.

**Not done:** the `hygiene` include-clean preset was not run (it needs a fresh
PCH-off build dir on a shared box). The only added include is `<string>` in
`curve_selector.cpp`, used directly; no header gained an include. Someone should
run that gate before merge.

**Pre-existing failures, proven:** `SurfaceV2Provenance.ValidationFallbackAdmissionRecordsTheServedFamily`
and `SurfaceDbPopulate.PropagatesStoredSurfacePolicyAndPersistsServedProvenance`
fail on pristine `main` (verified by stash, rebuild, restore). Unrelated to this
work, but they need an owner.

## 4.3 W2-B outcome (commit `4e88207`, branch `feat/vol-thin-prep-decouple`)

**The negative-carry hypothesis is refuted by measurement.** Folding
`ObsSet::provenance` into the starve errors (the builders were discarding that
evidence exactly when a slice starved) gives the real reason histogram for the
six refusing boards:

| board | rows | Configured prep, widest non-fitting expiry | after permissive rescue | carry failures |
|---|---|---|---|---|
| OGI | 23 | kept 0/4 — `InvalidBidAsk x3, SpreadVol x1` | 1/4 | 3 of 4 chains |
| ALLO | 27 | kept 0/7 — `InvalidBidAsk x5, SpreadVol x2` | 2/7 | 2 of 4 |
| ANVS | 32 | kept 0/3 — `InvalidBidAsk x2, SpreadVol x1` | 1/3 | 10 of 11 |
| SANA | 32 | kept 1/9 — `InvalidBidAsk x5, SpreadVol x3` | 4/9 | 1 of 5 |
| CHGG | 40 | kept 0/10 — `InvalidBidAsk x6, SpreadVol x4` | 4/10 | 1 of 5 |
| KPTI | 54 | kept 0/8 — `InvalidBidAsk x7, SpreadVol x1` | 1/8 | 3 of 6 |

`Deamericanization` is **0** on every board, so Andersen-Lake refusing negative
carry is not the cause. `fit_failed` is **0** on every board, so they die in
*preparation* and never reach fitting. `InvalidBidAsk` dominates everywhere, and
even the maximally permissive predicate keeps only 1-4 of 3-10 strikes.
**Loosening preparation is exhausted as a lever; the remaining data is discarded
by the one-leg-per-strike and two-sided rules, which is W1-B.** A typed refusal
is the honest outcome for these boards.

**Results**: zero status-column diffs on lqbench, serial fit CPU **126.8 s →
111.2 s (−12.3%)**, NaN model prices **−19.7%**, calendar-arb-free boards 51 → 55.
Twelve boards (ACB ARCT BEAM CGC CHPT CRON DNA EDIT EVGO KURA LAZR RARE) now fit
with their routed primary SVI instead of falling back to eSSVI — one fit instead
of two, which is where the CPU saving comes from. Honest cost: five of them lose
slices (DNA 9→6, RARE 8→6, CHPT 5→3, BEAM 6→4, LAZR 2→1), −10 total. Family mix
moves 117/95/3 → 105/107/3 eSSVI/SVI/linear, which *increases* exposure to the
diagnostics blackout and raises the stakes on W3-A.

**Two design calls, both measured rather than assumed.** The per-slice-always
legacy rescue was built first and **broke 2 of 3 boards on the AAPL/GOOGL/NVDA
populate lane** — a rescued slice carries quotes the cascade rejected and drags
worst-slice quality under a strict admission floor. It was replaced with a
last-resort form that runs only when the board would otherwise produce zero
slices, so it can only convert a refusal into a fit. The per-slice **linear**
fallback stays off because it fires on *fit* failure and therefore structurally
cannot rescue a prep-starved slice (`fit_failed = 0` on all six), and because its
union-grid floor absorbs the calendar refusals `n_slice_calendar_unsupported`
exists to surface.

**Correction:** the `PrepUncovered`-after-rescue hazard in §4.1 is narrower than
stated — the coverage gate is `kind == ConvexDense` only, so SVI thin boards are
never affected.

## 4.4 W2-A outcome (commit `00ebef4`)

**New rule.** A board-voted underlier keeps its own verdict iff (a) at least
`min_identifiable_expiries` (default 1) expiries each carry at least
`kMinIdentifiableSliceStrikes` (**4**) distinct two-sided strikes inside
`|ln(K/S)| <= 0.40`, and (b) the board carries at least
`sparse_validation_floor` (**24**) two-sided legs inside that band as a
dead-board backstop. `sparse_validation_floor` keeps its name and its
"0 disables" contract but **changes unit**: total legs → near-money legs.
Strikes rather than legs, because a call and a put on one strike are one point of
the smile; four strikes buys level, skew, curvature and one residual degree of
freedom. C8 capacity moved to the same footing (`>= 10` near-money strikes on the
deepest slice).

Calibrated, not guessed: over 216 boards fitted with the guard disabled, **every**
board whose best expiry carried fewer than 4 near-money strikes returned
`rmse_vol` 0.115-0.61 against a corpus median of 0.038, and all four boards above
0.15 were in that set.

**Results across the four lqbench cells:**

| metric | before | after |
|---|---|---|
| ok | 849 | **851** (zero regressions) |
| SparseGuard rate | 435 (**51.2%**) | 27 (**3.2%**) |
| zero-diagnostics | 413 (**48.6%**) | 49 (**5.8%**) |
| fit CPU (load-normalised) | — | 1.05-1.27×, inside budget |

S&P 100 control: **416/416**, eSSVI in-band median 0.9448 → 0.9451, eSSVI boards
321 → 351, zero-diag 95 → 65, SparseGuard **36 → 0**, `IlliquidSmallCap` **36 →
0**, fit CPU **0.82×**. The ten dividend mega caps (DUK, KHC, SYK, AMT, CMCSA, T,
CL, SO, MO, BMY) were `IlliquidSmallCap` on 36 of 40 board-sessions and are now 0
of 40.

Paired quality (the fair comparison, since the measurable population grew from
118 to 209 boards): against `sf40` over the 208 measurable in both — A unchanged,
B 0.0222 → **0.0168**, C 0.0362 → **0.0340**, D 0.0572 → 0.0574 (noise).

Two supporting changes were required, both discovered by measurement: quote
density is now **per quoted expiry** (200/110/50/15, boundaries at the geometric
midpoint of measured per-tier medians — pure quantile-matching was tried and
*cost* a board), and a **wide-book veto** sends any board with median spread
≥ 0.40 down to `OrdinarySingleName`, because fixing the spread median pushed UVXY
and BKNG over the tier boundary where the F19 tie-break defect then filtered their
entire boards away.

**Corpus fingerprint deliberately invalidated twice** (unit change plus a new
field at `corpus.cpp:490`) — two configs that route differently must not share a
cache key, and surfaces behind existing manifests were fitted under the old rule.

### Corrections to this plan, from measurement

- **§1.1's reservoir claim was backwards.** Over 1,544 board-sessions the
  first-256-leg sample is systematically **wider** than the true median at every
  tier (mega 0.067 vs 0.050, large 0.131 vs 0.103, mid 0.233 vs 0.204, small
  0.312 vs 0.279) — a front expiry is mostly cheap far-OTM legs whose *relative*
  spread is large. Fixing it makes measured spreads tighter, which is why it
  pushed BKNG and UVXY over the 0.40 boundary.
- **"Use an exact median, n is small" was wrong.** SPY carries ~13k two-sided
  legs and `classifier_inputs_from_underlier` is `noexcept` on the fit path, so a
  heap allocation whose failure would `std::terminate` is unacceptable. Replaced
  with a single-pass stride-decimated sample in the existing fixed buffer.
- **"Re-routed to Fast + SVI" overstated the family effect**: about two thirds of
  demoted boards still reached eSSVI via the post-build fallback ladder. The
  guard's real damage was the *diagnostics* loss on the boards where SVI built.
- §4.1's list of tests needing rebaselining was incomplete by four fixtures.

**Deferred and flagged:** the demotion's *consequence* is still
`IlliquidSmallCap → Fast + SVI`. On a genuinely unidentifiable board, SVI's five
free parameters per slice is arguably the wrong target and eSSVI's globally
coupled backbone the more parsimonious one — but changing the profile→family map
moves every vote-classified illiquid board too.

**Correction to W2-A's own report:** it states that W1-A "restored the
3-candidate ladder". Verified in code — it did not.
`production_selector_config()` still returns exactly one eSSVI candidate;
`bounded_selector_candidates()` has no production caller. The residual
zero-diagnostics are therefore not the selector choosing SVI (it cannot), but
most likely W1-A's fallback serving the profile curve after a selector refusal.
W3-A owns confirming this.

## 4.5 W1-B outcome (commit `a2d43e0`, branch `feat/vol-thin-prep-decouple`)

**Corpus facts that settle three claims in this plan.** A scan of all 1,128 files
and **1,222,719 rows** of both lqbench roots:

    unset_bid = 109,531 (9.0%)   unset_ask = 0
    zero_bid_kept = 0   locked_kept = 0   crossed_kept = 0

- **Every one-sided leg in the corpus is ask-only** — a genuine upper bound on
  value. There are no bid-only legs.
- **§2's spot-anchor concern (F35) is vacuous on real data.** `opra_panel.cpp:762`
  drops any row with an unset side *before* spot implication, and there are zero
  literal zero-bids, so "a zero-bid put is preferentially selected as the ATM
  anchor" cannot occur. The two-sidedness guard added here is latent hardening,
  provably a no-op on this corpus.
- **GNK and ATAI are not an anchor problem.** Neither board has *any* strike, on
  any expiry, carrying a two-sided call and a two-sided put. Requiring
  two-sidedness is a tightening, not a fix.
- F22's "the carry solve accepts `ask == bid`" is false in practice: `data_install`
  stamps `kQFlagLocked` when `bid >= ask` and `deamer.cpp:376` checks the kill
  mask. The single site that genuinely admitted a locked market was
  `prepared_fitting.cpp` `quote_valid`, now `ask > bid`.

**The change that mattered** is the ITM-leg fallback: when a strike's OTM leg is
rejected as `InvalidBidAsk` and its ITM leg is two-sided, the ITM leg is admitted
instead of the strike being discarded. Ordering was measured, not assumed —
ITM-first perturbed 13 boards W2-B had recovered and *cost* slices, so the ladder
runs `policy/OTM → permissive/OTM → policy/ITM → permissive/ITM` and is purely
additive.

| cell | ok before → after | recovered | regressions |
|---|---|---|---|
| 1555 2026-08-03 | 215 → **219** | ALLO CHGG KPTI SANA | 0 |
| 1555 2026-08-05 | 215 → **221** | ALLO ATAI CHGG GNK KPTI KURA | 0 |
| 1030 2026-08-03 | 205 → **211** | DNA EDIT EVGO SANA SNDL TXG | 0 |
| S&P 100 07-20 | 104 → **104** | — | 0 (zero boards perturbed) |

`rmse_vol` is bit-identical for tiers A, B, C and V; tier D *improves*
0.05058 → 0.05004 while gaining four boards. CPU median ratio 0.968 measured by
interleaved 3-round A/B with two saved binaries (naive back-to-back was
misleading — the box drifts +32% over a session).

OGI and ANVS remain refused, now with specific messages: their surviving expiries
carry 4 and 3 strikes against a hard 5-row floor, and their carry solves kill 3
of 4 and 10 of 11 chains. That is W3-B (floor unification) and W4 (carry), not a
filter artefact.

**A real bug fixed incidentally:** a golden value moved 0.35316 → 0.23954 because
a locked-market row was carrying a ~1e16× weight and pulling the fit 11 vol
points off a board priced at flat 24% vol.

**Explicitly not done:** one-sided quotes are still not admitted as *price
bounds*. `opra_panel.cpp:762` drops half-quotes before they reach a `Chain`, and
admitting them needs both a chain representation for a half-quote and a
hinge/inequality term in the loss of `svi_calib`, `essvi_calib`, `c8_calib`,
`spline_curve` and `LinearVariance` — every one of which is a weighted
least-squares on a single scalar per row. That is a separate piece of work, not a
knob. Given `unset_ask = 0`, every such leg is an upper bound, which makes the
change well-posed whenever it is taken on.

**A third pre-existing failure** was found and proved by stash:
`VolUmbrella.TierCountsMatchTheReadmeTable` (tier_b 34 against a pinned 31) —
README drift, unrelated to this sprint.

## 5. Acceptance gates

The sprint is done when, on the lqbench corpus:

1. **No untyped terminal failures.** Every board either fits or returns a refusal
   naming the board condition. Target: zero `select_curve` errors, and the six
   starved-slice boards resolved either way.
2. **Tier C/D success rate ≥ 95%** excluding names with no OPRA data (from 91.7%
   and 77.3%).
3. **Zero-diagnostics rate < 5%** (from 45.6%).
3a. **Quality measured in vol points, not in-band.** Median `mean_rmse_vol` for
   tiers C and D must not worsen against the `sf40` reference (0.035 and 0.056).
   Do not report in-band as a cross-tier quality comparison — it is
   spread-relative and flatters wide markets by construction (§1.10).
4. **No regression on the control corpus**: the S&P 100 stays at ≥ 519/520 with
   eSSVI in-band median ≥ 0.94.
5. **Fit CPU within 1.3× of today's** on the same corpus (the sf40 ablation cost
   1.13×, so this is achievable).
6. **Routing reproducibility**: the cross-session family/profile flip rate falls
   from 17% overall (53% in tier B) to under 10% overall, measured the same way —
   two sessions, same flags, symbols present in both. A resolver whose answer
   changes between adjacent snapshots has not "resolved" anything.
7. Targeted test suites pass; hygiene preset clean.

### 4.6 W3-A outcome — diagnostics blackout closed (`f242f65`)

Landed on `feat/vol-thin-board-autofit`. Five files, 394 insertions, 304 of them
tests. Eight new tests, all passing; targeted run `98% tests passed, 3 tests
failed out of 153`, the three being the known pre-existing main failures, proved
byte-identical to a stashed build after normalising timing tokens.

What landed:

* **Always measure a board the library routed.** `pricer_fitter.cpp:681` forces
  `score_parity` on when the caller pinned neither a family nor the Hft dense
  route, regardless of whether the admission policy consumes the evidence. A
  caller-pinned curve under a floor-free Mark policy keeps the opt-out, because
  an explicit pin carries its own latency budget.
* **`quote_coverage` no longer collapses to 1.0.** `pricer_fitter.cpp:393,
  449-453`. Previously an unscored slice was credited as fully served, which
  pinned coverage at exactly 1.0 and made `min_served_quote_coverage = 0.50`
  unfireable. Now only `Disabled` (scoring opted out board-wide) still credits
  the fit-observation count; scoring requested but no scored row counts the
  quotes as attempted and not served.
* **`Degraded` instead of a false `Healthy`.** `pricer_fitter.cpp:543-556` adds
  one shared `surface_has_expiry_gap` predicate used by both the cold publish
  (`:920`) and the incremental republish (`:1968`), so a refit of a surviving
  slice cannot launder the gap away. It reuses `CarryGap` rather than minting a
  new bit, because `CarryGap` is already the publish-with-Degraded reason for a
  dropped expiry and every persisted mask already round-trips it.

Measured on the 240-name universe at 2026-08-05, same binary with and without:

| | baseline `00ebef4` | with W3-A |
|---|---|---|
| ok boards | 218/240 | 218/240 |
| zero-diagnostics | 17 (7.8%) | **0 (0.0%)** |
| `calendar_arb_free` | 22 (10.1%) | 22 (10.1%) |
| boards lost to admission | — | none |
| served-family changes | — | none |

Note the baseline is 7.8%, not the 45.6% in §1.7: the W1/W2 routing commits had
already returned 201 of 218 boards to eSSVI, which scores unconditionally. The
residual blackout was concentrated on the 17 SVI and LinearVariance boards.

**A hypothesis of mine was falsified here.** I had written that the low
`calendar_arb_free` rate was probably the same reporting gap. W3-A checked
instead of assuming: the flag never reads `score_parity`, and across 218 boards
the two binaries agree on every single one (196 `0->0`, 22 `1->1`). See §6.

### 4.7 One predicate that looked wrong and was not

W3-A was asked to settle whether hoisting

    const bool auto_routed = next_decision.has_value() && !cfg_.curve.has_value() && !pinned_hft;

to a bare `next_decision.has_value()` had silently widened the force-scoring
condition and the fallback ladder to caller-pinned boards. It had not. The
conjunction was exactly redundant: `next_decision` has exactly one assignment
(`pricer_fitter.cpp:641`) and it sits inside `if (!cfg_.curve.has_value() &&
!pinned_hft)`, while `cfg_` has no mutator that can change either term
(`set_threads` touches `n_threads` only, and `config()` returns `const&`). The
v2 path at `:1449` does still need its extra term, because it tests the
persistent member `decision_` rather than a local, and was correctly left alone.

Recording this because the reasoning is the reusable part: a redundant-looking
conjunction is only safe to drop once you have shown the single assignment site
AND that nothing can mutate the terms in between.

## 6. Follow-on wave, not in this sprint's scope

**Calendar arbitrage on the served surface.** `calendar_arb_free` is a genuine
check over log-moneyness `[-0.60, +0.60]` on a 64-point grid, true only if the
check ran and found zero violations (`session.cpp:1296`). Measured rates:

    lqbench, merged tree      10.4%
    S&P 100 control, before    2.9%
    S&P 100 control, after     2.9%

So roughly 90% of published surfaces carry a real calendar violation, and the
control corpus of mega caps is *worse* than the thin-board universe. This is
therefore not a small/mid-cap problem and does not belong to this sprint. Two
things are needed before anyone acts on it: export `n_calendar_viol_pre` so
"violations found" can be separated from "check failed" (the CSV cannot
currently distinguish them), and characterise whether the violations are
concentrated in the far wings where the 64-point grid samples thinly.
