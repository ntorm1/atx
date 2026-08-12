# atx-vol: surface fitting — breadth and depth

**Date:** 2026-08-09 · **Base:** `main` @ `7195aba` · **Predecessor:** `docs/superpowers/plans/2026-08-09-atx-vol-thin-board-autofit.md`
**Agent profile:** every implementation task follows `C:\atx\.agents\cpp\agent.md`.

## Goal

Two properties, as acceptance criteria:

1. **Breadth.** Any well-formed American equity board fits, or returns a typed
   refusal that names a property of the *board* rather than of our plumbing.
   ~~Today 9.3% of tier-D boards still refuse~~ — **withdrawn, see §1.5 (6):
   the figure does not reproduce; it is 1.8% (4/223)**. Several mechanisms built
   to prevent refusals are wired up but never execute. The real breadth loss is
   **carry**, not refusal count — §1.6.
2. **Depth.** A served surface is arbitrage-free where it is used, the region
   where that holds is stated rather than implied, and the quality numbers we
   publish measure the surface we actually serve.

The predecessor sprint fixed *routing*. This one is about the *fit itself* and
about measuring it honestly.

---

## 0. How to read the evidence in this document

Three sources, weighted differently:

- **Measured** — numbers I produced from real OPRA runs. Reproduce with the
  commands in §9.
- **Verified in code** — `file:line` claims I read directly. Every citation in
  §2, and the ones marked (v) elsewhere, were opened and confirmed.
- **From the literature** — external results, cited in §8. Where a source is
  paywalled or a claim is an inference rather than a quotation, it says so.

**Two of my own earlier conclusions are withdrawn in this document** (§1.4).
That is the third and fourth time this codebase has falsified one of my
hypotheses about arbitrage. Treat confident causal stories here — including
mine — as things to measure, not things to build on.

> **Update, mid-sprint.** Six more of this document's own claims have since been
> falsified by measurement — WITHDRAWN (3)–(8) in **§1.5**, which also restates
> the baseline on the production fit path. §1.6–§1.8 record where the sprint's
> target actually moved. **Read §1.5 before §1.4**: §1.4's table was measured
> through a code path production never takes. The advice in the paragraph above
> turned out to apply to this document more than to its predecessors.

---

## 1. Measured baseline

### 1.1 Where the predecessor sprint left us

240-name liquidity-stratified OPRA universe, 4 snapshots; S&P 100 control,
4 sessions. Tier C fit success 99.2%, tier D 90.7%, control 416/416, fit CPU
0.50× pre-sprint, zero-diagnostics 0.0%, targeted tests 213/215.

Still open from that sprint: tier D short of 95%, and cross-session routing
flips at 10.4% / 14.9% against a 10% bar.

### 1.2 `calendar_arb_free` is populated from two different grids

There are two fit lanes, dispatched at `session.cpp:1268`
(`if (eff.curve.kind != VolCurveKind::Essvi)`), and they write the *same*
boolean from *different* domains (v):

| lane | families | check band | grid / pair | repair band |
|---|---|---|---|---|
| `run_surface_parity` | eSSVI only | **k ∈ [−3.0, +3.0]** (`surface_parity.cpp:62-64`) | 25 | ±0.50 (`surface_parity.cpp:78-79`) |
| `fit_curve_surface` | all others | k ∈ [−0.6, +0.6] (`session.cpp:1284-1286`) | 65 | tradeable overlap (`vol_curve.cpp:44-60`) |

k = ±3.0 is a strike 20.1× and 0.0498× the forward. The source itself labels
that grid **"DIAGNOSTIC ONLY"** (`surface_parity.cpp:60-61`) and explains at
`:70-77` that the repair was deliberately narrowed to ±0.50 in 2026-08 because
repairing over ±3.0 had been fabricating served levels — the XOM/CVX defect,
+8..25 ATM vol points.

### 1.3 A separate oracle already certifies the band that matters

`RiskSurfaceValidationConfig` (`detail/risk_surface_validation.hpp:51-68`) (v):
k ∈ [−0.50, +0.50], tolerance **1e-8**, plus price bounds, strike monotonicity,
butterfly via call-price convexity, and a Lee wing slope ceiling of 2.0. It runs
on every risk build (`pricer_fitter.cpp:1508`, `:1950`, `:2135`) and calendar
failure is **not** in the degradable set (`surface_policy.cpp:32-33`) (v), so it
hard-rejects.

> **CORRECTED (grid size).** "129 calendar points" was the struct default, which
> is what Accuracy mode uses. Production runs **Balanced**, which samples **97
> strike / 65 calendar** points (`pricer_fitter.cpp:1795-1800`) (v). Confirmed
> empirically by T1b: `oracle_n_calendar_samples == 65 × (n_slices − 1)` on
> 192/192 lqbench boards.

`PricerFitter::surface()` returns the risk surface when Risk is in `outputs`,
the default (`pricer_fitter.cpp:2221-2229`, `surface_policy.hpp:146-147`).

**Consequence, as originally stated:** boards reporting `calendar_arb_free =
false` were, on the same run, admitted as calendar-arbitrage-free on |k| ≤ 0.5
at 1e-8. **T1 has now reported and this inference was wrong on the path it was
measured on. See §1.5, WITHDRAWN (3).**

### 1.4 Corrected measurements

Instrumented `universe_autofit` to export `n_calendar_viol_pre` and
`n_price_bound_violations`. lqbench 15:55 2026-08-03 (221 served); S&P 100
2026-07-22 (104 served); `--preset robust --r 0.043`.

The check never fails: **0** sentinel-1 rows on either corpus, consistent with
`arb_check_calendar` having no `Err` path (`arb.cpp:347-383`) (v). Every
reported `false` is a found crossing.

| corpus | lane | n | % clean | violations / sampled point (mean, median) |
|---|---|---|---|---|
| lqbench | eSSVI ±3.0 | 203 | 5.4% | 0.153, 0.152 |
| lqbench | poly ±0.6 | 18 | 66.7% | 0.051, 0.027 |
| S&P 100 | eSSVI ±3.0 | 100 | 2.0% | 0.156, 0.164 |
| S&P 100 | poly ±0.6 | 4 | 25.0% | 0.033, 0.009 |

In the certified band the rate is ~3% and two thirds of boards are clean; out to
±3.0 it is ~15% and almost nothing is. The apparent corpus gap (10.4% vs 2.9%)
is mostly lane composition — 91.9% vs 96.2% on the ±3.0 lane — plus more slices
per board (median 15 vs 13), i.e. more adjacent pairs to test.

Fit quality does not predict the flag: violating boards median `rmse_vol` 0.0338
vs 0.0352 for clean ones. The violating surfaces fit their quotes *better*.

> **WITHDRAWN (1):** I previously reported "~90% of served surfaces carry a real
> calendar violation." The crossings are real but sit in a band the library
> explicitly declines to certify or repair, on surfaces separately certified in
> the traded band. The framing was wrong.
>
> **WITHDRAWN (2):** I previously reported "price-bound violations are 0
> everywhere, so this is purely calendar." `n_price_bound_violations` is only
> populated on the polymorphic lane (`session.cpp:1302`); the eSSVI path copies
> only the calendar fields (`session.cpp:1434-1438`) (v), so it is structurally
> zero for 92–96% of boards and reads identically to "verified clean."
> **Butterfly is unmeasured, not clean.**

### 1.5 What T1/T1b/T2/T8 measured, and what it withdrew

Everything in §1.1–§1.4 was measured through `universe_autofit` as it stood
before T1. That harness constructed a `PricerConfig` naming neither
`quality_mode` nor `outputs`. `PricerFitter::is_v2_request()`
(`pricer_fitter.cpp:2174`) (v) returns
`cfg_.quality_mode.has_value() || cfg_.outputs.has_value()`, and **both are
`std::optional`, default unset** (`pricer_fitter.hpp:273-274`). Naming *either*
opts into the v2 dual mark/risk pipeline with the independent oracle. Naming
neither takes the legacy single-surface branch (`pricer_fitter.cpp:621`), which
publishes into the market_mark slot and **never validates**.

Production names both, unconditionally: `pricer_config_for_symbol`
(`surface_db_populate.cpp:52-68`) (v) assigns `out.quality_mode` and
`out.outputs` from `SurfacePolicy`, whose fields are non-optional. **So the
benchmark and production were never running the same code path**, and §1.4's
table describes the legacy one. T1b (`5b41cb9`) added `--fit-path
production|legacy`, defaulting to production, mirroring
`pricer_config_for_symbol`.

**Re-measured on the production path** — lqbench 2026-08-03 (192 boards) and
S&P 100 2026-07-22 (104 boards), `--preset robust --r 0.043`:

| property | production path |
|---|---|
| boards served | 85.3% |
| slices served | 57.7% |
| calendar violations, |k| ≤ 0.5 | **0** across 155,285 samples |
| strike/butterfly/price-bound violations | **0** across 308,817 samples |
| `mm_state` | `rejected` on 192/192 and 104/104 |
| boards publishing `degraded` on `CarryGap` | 177/192 |
| eSSVI rejected as **primary** curve family | 60% (lqbench) / 68% (control) |

> **WITHDRAWN (3)** — §1.3. "Every `status=ok` board was certified
> arbitrage-free on |k| ≤ 0.5." The oracle never ran on the path §1.4 measured.
> The inference was sound about production and simply untested; the sentence
> asserted it as established.
>
> **WITHDRAWN (4)** — §D1. "The number that gates arbitrage is computed on every
> board and never exported." Half wrong: on the benchmark path it was **not
> computed at all**. A default-constructed `ValidationDigest` reads as
> "0 violations", which is indistinguishable from "verified clean" — the same
> defect as WITHDRAWN (2), one layer up. T1 (`91904d3`) exports an `oracle_ran`
> column derived from `candidate_generation != 0` so the two cannot be confused
> again.
>
> **WITHDRAWN (5)** — §B4 row 1. "0DTE index boards are silently dropped because
> `expiry_close` defaults to `MidnightUtc`." Stale. `opra_panel.cpp:784-786` (v)
> maps every convention except `UsIndexAmOpen` to `SettlementSession::Pm`, fixed
> in `7b402ae` before this sprint. Worth **0 expiries**. Pinned by a test in
> `9777b7c` so the claim cannot drift back.
>
> **WITHDRAWN (6)** — the **9.3% tier-D refusal** figure that motivates the Goal
> and gate 3. It does not reproduce: **4/223 = 1.8%** on this document's own §9
> corpus and command. Every downstream sizing that rests on 9.3% is overstated
> by ~5×.
>
> **WITHDRAWN (7)** — §T2's normalisation. The plan says `r_i = p_i/DF`; the
> correct reduction is `r_i = p_i/(DF·F)`. Corrected in `77960f4`.
>
> **CORRECTED (8)** — §T9. "No assignment to `.rho_scale` anywhere in `src/`."
> `batch.cpp:279` assigns `.rho_R`. The ingress inventory in T9 must be
> re-derived, not trusted.

### 1.6 The target moved: carry, not geometry

Geometry is **clean** on the served production surface — 0 violations, both
constraint classes, both corpora. The sprint's depth premise is therefore
satisfied *for what is served*, and the binding constraint on breadth is
**carry**:

- **29 of 29 lost boards** are lost to carry. The mechanism is two lines in
  `apply_risk_policy()` (`pricer_fitter.cpp:1247`) (v):
  `deam.require_carry_confidence = true` (`:1251`) and
  `deam.audit_fit_inversions = true` (`:1256`).
- On boards that **do** serve, carry still costs **42% of slices and 47% of
  quotes**.
- **177/192** boards publish `degraded` on `CarryGap`.

This reshapes T5 into a carry workstream (T5c) combining T5 + B3(ii) + B4 row 4.
**Setting `require_carry_confidence = false` is forbidden**: recovery must come
from carry being *known*, not from carry being *unchecked*.

### 1.7 The clean oracle result describes a substitute, not the intended fit

eSSVI is rejected as the **primary** family on 60% / 68% of production boards and
survives only via ladder substitution (`used_fallback` 1 → 114). So §1.5's zero
violations certify the substitute surface. Which constraint rejects the primary
is the open question T1c answers, and **T3 cannot be scoped until it does** —
T3's stated acceptance criterion ("calendar violations inside the certified band
go to zero") is already vacuously satisfied. T1b's recommended restatement:
*the eSSVI-primary rejection rate falls from 60% / 68%.*

### 1.8 Scope note: no engine consumer exists

Nothing in `atx-engine/` or `atx-impl/` consumes `atx/vol/`. The surface DB is
read only by the dispersion backtests, `surface_db_main`, and the Python
bindings. The coverage numbers in this document gate the **research layer**, not
a live signal path. Engine integration is a real gap and this plan does not
address it.

### 1.9 Execution log — what each landed task actually measured

Written after execution. Every number here was produced on the **production fit
path** (`--fit-path production`, both `quality_mode` and `outputs` named) against
the two corpora this document uses: `lqbench-auto` and `sp100-auto`. Figures are
quoted as *lqbench / sp100* where they differ. Nothing in this section is a
projection.

**T1c — where eSSVI's primary fit actually dies.** Primary rejection 63.1% /
67.6%. Split: BUILD 29.7% / 49.3%, ADMISSION 70.3% / 50.7%. Oracle bits on the
rejected population: `CarryGap` 100% / 97.1%, `InversionResidual` 82.2% / 31.4%,
`Butterfly` 30% / 80%, `StrikeMono` 12.2% / 34.3%, `Calendar` 7.8% / 34.3%,
**`Wing` 0.0% / 2.9%**. The two corpora are different regimes, not two samples of
one: median slices per board 4 (lqbench) vs 10 (sp100).

**T3 — residual layer is add-only.** The residual correction may now only *add*
variance (closed-form KKT on a block-diagonal normal system, `assert`-checked),
plus the deferred Lee–Roper density projection. `arb_repair_calendar_residual`
failures 14 → 0 / 23 → 0. Survival 36.9% → 41.9%.

**T3b — (N1)+(N2) as a projected LM, not a post-fit repair.** `essvi_fit_slice`
takes `const EssviParams* calendar_prev`; trial cubes are pushed onto the
feasible set *before* SSE scoring. `arb_project_calendar_essvi` failures 12 → 1 /
11 → 0; survival 32.4% → 51.0%; build+admission survival 90.6% / 95.1%. Two
premises of the brief were falsified in the doing: (N2) is not expressible as a
post-fit projection, and (N1)+(N2) are not jointly sufficient — (S1) is bilinear
and was deferred to T3c. In-band calendar violations were zero before and after,
which is exactly why §1.7's restatement of the acceptance criterion mattered.

**T4 — exact crossing detection.** The pairwise crossing test reduces to a
quartic. Over 4,000 slice pairs: sampled grid found 2,940 crossings, a 200k-point
oracle 2,946, the quartic 2,946 — the grid misses 6 and the closed form is
exact. One board (COIN) had `calendar_arb_free` flip 1 → 0 under the exact test.
The magnitude is term-structure-driven: the crossing delta at k = +0.60 is
1.6e-14 at 1 week against 0.084 at 2 years.

**T5c — the target moved a second time.** Boards ok 192 → **203**, zero lost;
slices/expiries 1,425/2,524 → 1,468/2,626. Serving boards bit-identical,
`mean_rmse_vol` ratio 1.0000; sp100 byte-identical. Carry confidence
distribution: `Solved` 70.6%, `TermStructureExtrap` 15.9%, `TermStructureInterp`
13.1%, `MoneynessBounded` 0.5%. The new absolute round-trip metric (de-Am → fit →
re-Americanize → vol points) reads CORZ mean 0.0388 / worst **0.550** at 100%
in-band against SPY 0.0158 / 0.169 at 8.1% in-band. Three premises of the brief
died here: the "29 of 29 lost boards" carry story, the 42%-slice-loss story, and
the attribution of the 177 degraded boards. What replaced them: **preparation
starvation** — `Starved` on 30.2% of 2,707 chain outcomes, with `CarryFailed`
exactly 0 on a 40-board serving sample. That is what T6 attacks.

**T9 — the ρ-blend was already dead.** Binary scan of 5,981 archive files,
162,793 eSSVI slices, cross-checked against file size: **0 armed** (`rho_scale >
0 && rho_R != rho`). Retired rather than fixed.

**T10 — D3 closed, gate and projector together.** `kSviWingSlopeGate` 4.0 → 2.0
in both `arb.cpp` and `svi_calib.cpp`'s `mm_project_admissible`. The projector is
provably still unconditionally admissible: `rho` is clamped into
(−1+1e-4, 1−1e-4) *before* the Lee clamp, so `lee_max = (2−1e-9)/(1+|ρ|) ≥ 1 >
edge_b` always. Zero slices lost; 221/221 and 104/104 bit-identical on
auto-selection.

**A trap this sprint nearly walked into.** D3's derivation is that eSSVI's
`essvi_phi_max` bound of 4 and Lee's bound of 2 are the same number: factoring φ
out of eSSVI's evaluator with `m = −ρ/φ`, `σ = √(1−ρ²)/φ` gives raw SVI with
`b = θφ/2` *exactly*, under which `θφ(1+|ρ|) ≤ 4` is `b(1+|ρ|) ≤ 2`. Raw SVI's
gate was loose by exactly 2×. That argument covers the **SVI-only** constant.
`RiskSurfaceValidationConfig::max_abs_wing_total_variance_slope`
(`risk_surface_validation.hpp:67`, enforced `risk_surface_validation.cpp:384`) is
already 2.0 and is **family-agnostic** — it runs on the sampled variance grid
with no knowledge of which family produced it, binding eSSVI, SVI and
convex-dense alike. Anyone "harmonising" that knob on the D3 derivation would be
applying an argument that does not cover the families it would hit. Corroborating
measurement: eSSVI never trips the `Wing` bit across 296 served boards (0/90 and
1/35 at baseline; 0 and 0 after T3).

**Two methodological failures worth recording**, both caught, both now standing
requirements in every brief:

1. *A poisoned baseline.* T4's `base_*.csv` was not at the commit it appeared to
   be — it differed on 7 lqbench and 1 sp100 board. Rule: **do not inherit a
   baseline you did not generate.**
2. *A test that could not fail.* T10's first projector test passed with the
   projection assignment deleted; its fixture never tripped the gate. Rule:
   **mutation-check every behavioural test** — delete the production line it
   pins, confirm red, restore.

**Open decision — T5d, the units of the carry gate.** `max_carry_leave_one_out =
0.005` is an annualized *rate* applied flat, but a borrow error `db` displaces
`k = ln(K/F)` by `db·T`. At 4 days a `db` of 0.005 moves `k` by 5.5e-5; at 18
months by 7.5e-3 — a factor of ~137, which matches T5c's measured spread. That
reads as a units correction, but it changes what `require_carry_confidence`
certifies, so it is not taken on the algebra alone. **Acceptance test, fixed in
advance:** slices admitted under the invariant `db·T` gate but refused under the
rate gate are measured on T5c's absolute round-trip metric. Comparable error ⇒ it
is a correction and it ships. Worse error ⇒ it is a relaxation and the gate
stays. `MoneynessBounded` already exists and is calibrated to coincide with
today's gate at the 1-year / 50-vol pillar.

**Standing prohibitions for the rest of this sprint.** Setting
`require_carry_confidence = false`, disabling `audit_fit_inversions`, and
widening `max_certified_deam_drop_fraction` are all forbidden. Every recovery in
this sprint must come from carry or nodes becoming *known*, never from a check
being removed.

### 1.10 Execution log, continued — the half where the premises kept dying

Same conventions as §1.9: production fit path, lqbench / sp100, nothing
projected. This half of the sprint is distinguished by how often the measured
answer contradicted the brief that commissioned it. Every such contradiction is
recorded as the result.

**T3c — the eSSVI gap was carry policy, not preparation.** Board-level census
plus board-level carry decision for the eSSVI lane, plus (S1) as the bilinear
cap it is. eSSVI expiry fit rate 0.492 → 0.766 / 0.653 → 0.926; served slices
1,384 → 1,813 / 1,202 → 1,448; `arb_project_calendar_essvi` failures 1 → 0 /
0 → 0; in-band calendar violations 0/105,365 and 0/87,360; zero boards lost
slices. Not met as stated: `mean_in_band` 0.9724 → 0.9690 / 0.9624 → 0.9526 and
survival 90.6 → 89.2% / 95.1 → 94.1% — new slices are thinner than incumbents.
Two artifacts died here: `attempted_quotes` accumulated only over expiries that
fitted (`pricer_fitter.cpp:446-453`), so the quote evidence behind the
starvation story was an accounting artifact; and `require_carry_confidence` is a
*board* policy applied per *expiry* in `run_surface_parity` — the eSSVI gap was
`CarryFailed`, not `PrepStarved`. The sprint's target moved a third time.

**T3d — the 0.35 worst-slice floor is vindicated, and the honest cost of
breadth is decomposed.** The floor sits inside an empty band: highest rejected
worst 0.3415 vs lowest served 0.3636 (lq), 0.2667 vs 0.3846 (sp100); zero
served boards lie below it; admitting all five rejected primaries would need
≤ 0.2581. The `mean_in_band` cost of T3c decomposes on sp100 (−0.0079 total):
43% new-slice dilution, 33% family switch, 25% retained-slice degradation —
and retained degradation is 0/842 and 0/684 on boards whose slice count did not
change. New ≤ 7-day slices average 0.8082 in-band vs 0.9473 for retained.

**T10b — D4's naive fix was a regression; what shipped is dof honesty with the
evidence kept.** The one-line fix (score against the fitted family's dof,
refuse when n ≤ dof) lost in-band evidence on 9/240 boards and moved corpus
`mean_in_band` 0.9652 → 0.9293. Worse, blanking chi2 to 0.0 on refusal
re-created the exact W3-A blackout this plan's §4 documents — an exact zero
chi-square reads as a *perfect* fit. Shipped instead: under-determined slices
re-score at dof 0 (chi2/N), keep their band evidence, and set
`chi2_dof_underdetermined`. Measured: `chosen_kind` unchanged on 344/344
boards, `mean_in_band` bit-identical, only `mean_chi2` moves (17 / 4 boards),
well-posed SVI rising by exactly (N−3)/(N−5). The framing correction matters:
the selector was never dof-wrong (`curve_selector.cpp:600` already accumulated
`curve.dof()`); the hardcoded 3 fed `chain_parity`'s published `mean_chi2`.
Alongside: FitDiag wired at the three production sites (bit-identical on 344
boards) and C8's LM given a real termination verdict — it exhausts damping at
an optimality cosine of 0.177, i.e. it stops *near* optima, not *at* them.

**T5d — the carry-gate units correction is measured and rejected.** §1.9's
pre-registered test was run: slices admitted under the invariant `db·T` unit
but refused under the flat rate gate (n = 93, round-trip median 0.02619) are
indistinguishable from the still-refused population (n = 278, median 0.02311),
and both are ~70% worse than slices both gates admit (n = 214, median 0.01547).
The algebra was right and the gate change would still have been a relaxation —
85 of the 93 new admissions were under three months, because the *measured*
`db = −log(F_pair/F_base)/T` amplifies quote noise by 1/T; the units argument
cuts both ways. Reverted byte-clean. Two premises died in the doing:
`OrdinarySingleName` caps `max_spread_vol` at 0.12 (not 0.25), and the profile
feedback loop is open, not closed.

**T6 — one-sided quotes as bounds, and the starvation number re-based.** T5c's
30.2% `Starved` did not reproduce: the true baseline is 600/4,647 = 12.91%,
falling to 12.35% with bounds admitted. Only 8 bound legs are admitted from 94
armings; boards 203 → 203, slices 1,468 → 1,482; healthy-with-gaps boards
15 → 0. Of the 14 recovered slices, 11 were IREN — which T6d then showed was
profile reclassification, not bounds at all.

**T6d — the asserted loader defect does not exist; the real one was the
sampler.** The brief asserted a last-wins clobber at `data.cpp:508-521`
(a `bid == 0` row overwriting a two-sided quote). Direct corpus probe: **zero**
duplicate (expiry, strike, side) keys across all 225 lqbench boards and 105,347
sp100 rows — the collision class is unpopulated. The real mechanism: admitting
bounds *permutes* the quote stream, and the 256-cap stride median over spreads
was order-dependent — pure permutation (two-sided multiset bit-identical) moved
the estimate on 104 boards and pushed IREN and PLTR across classifier edges.
Sharper: IREN's *true* median is the post-T6 value — the pre-T6 baseline was
subsampling error (the stride sample misclassified 12/225 boards before T6, 10
after). Fixed with an exact histogram median (2,000 bins on the bounded (0,2)
domain; every classifier edge is a bin boundary; order-invariant by
construction), plus a defensive install-rank guard (two-sided outranks
one-sided; corpus-neutral since the class is unpopulated). Honest re-basing:
T6's lqbench headline falls **+14 → +10** (1,482 → 1,478; AVGO and STLD were
also sampled-median errors, in the tight direction); bounds proper are worth +3
lqbench slices and are a byte-identical no-op on sp100. sp100 improves
outright: rmse ×0.952, GM/NKE/SPGI +8 slices. UPS's ×1.71 rmse degradation —
Part 2's whole question — was the same defect (a spurious hop across the 0.15
classifier edge) and resolves to rmse 0.008863 with no special case. Two
lqbench boards get *worse* by being correctly classified (AVGO ×4.7, ETSY
×7.6): the bucket definition being honored, recorded not hidden.

**T3e — the calendar instrument is honest, and the oracle's rejections are not
harvestable at the tick.** Q1: all 27 wing degradations under T3b/T3c are
*forced* by (N2) — old wings sit a median 21.3% below the (N2) floor
(range 1.8%–68.7%); 0/27 changed `forward` or `n_used`; (S1) never armed; 16/27
inherit the binding predecessor through chain propagation. The instrument
verifies: (N1) violations 0/443 and 0/459, worst (N2) residual 1.6e-16. Q2: of
mark-admitted primaries, **99/185 (lq) and 50/98 (sp100) are oracle-rejected
and silently substituted** — all eSSVI. Split: `InversionResidual` 73 / 9,
`Butterfly` 26 / 40, `Calendar` 2 / 4. All 66 butterfly rejections are
economically unharvestable: the oracle grid recovers as Δk = 1/96, break-even
full width is `slack·h·F/2`, and the violations sit at a median ~3% of a penny
tick, with 62/66 needing tighter than a tenth of a tick. The cost of
substitution is coverage: obs/slice 36.5 → 20.5 (sp100) and 25.8 → 13.4 (lq),
and on **20/20** sp100 boards where the substitute's *worst* improved, coverage
fell — the min improved by fitting less. That is the defect T7a attacks: the
substitution decision (`pricer_fitter.cpp:1601-1606`) is made with both records
live and no comparison between them.

**T5e — the term-structure fallback borrow is exonerated.** On the strongest
control — same expiry, same board, served under fabricated borrow vs its own
gate-rejected solve, deferral set held identical — 723 paired lqbench expiries
show rmse ratio 1.001 (paired median +9e-06); the 431-pair sp100 control is not
significant. The motivating 45× deltas (ACHR, SRPT) were admission-set
composition on boards where the fallback **never executed** — both have zero
confident anchors, so nothing was ever fabricated. Mechanism of the null: a
borrow error shifts every observation's k by ~db·T nearly uniformly and the
slice is refit in k, so served fit quality is largely invariant to the
coordinate shift. Recorded for the future: the 1/T amplification law is
confirmed on 870 leave-one-out expiries (unbiased; median |db·T| 0.0008 at
T/T_a < 1 rising to 0.009 at 5–20×) — it just doesn't damage the served
metric. Scoping fact integration must know: Decision B is unreachable for ~92%
of boards, because dispatch (`session.cpp:1268`) sends eSSVI to a driver with
no fallback at all — non-confident expiries are hard-dropped
(`deamer.cpp:668/790`).

**T10c — D4 extended to the eSSVI lane, where dof varies within one surface.**
The remaining `n_curve_params = 3` hardcodes (`surface_parity.cpp:585`,
`session.cpp:2280`) were wrong twice over: eSSVI calibration is per-slice
(3 backbone parameters), but the *served* curve adds an armed wing residual —
HingeQuad carries 4 fitted coefficients, C2Bspline `resid_n_basis` — and
calendar-floored slices are refit residual-off, so one surface legitimately
serves dof-7 and dof-3 slices side by side. Shipped as `essvi_slice_dof` read
off the served `EssviParams`. Measured: `mean_chi2` is the only column that
moves — 48 / 54 boards, every one on a residual-armed profile, all upward (the
removed optimism, typically 1.1–1.8×). Zero admission or gate flips: no
production config sets `corpus.cpp:322`'s ceiling and no board crosses the
reference 3.0 (closest: INTC 2.2802 → 2.2902); the gate itself remains
un-re-based per the standing decision. Banded parity-evidence counters
(`n_parity_scored` / `n_parity_in_band` / `n_parity_out_of_band`) now flow
through both lanes and the refit path, proven additive by a
bit-identical-except-new-columns corpus diff — closing the W3-A hole where a
default-zeroed report was indistinguishable from verified-clean.

**Methodological additions this half forced into every brief.**
(3) *Confirm the mutation build succeeded* before believing red or green — a
`-Werror` failure produced a false green once. (4) *No timing claims without a
bit-identical control arm* — ambient load produced 1.212× and 0.693× on
provably bit-identical work. (5) *Pin the served artifact, not a parallel
re-derivation* — a chi2 test passed under mutation because it recomputed the
value it claimed to pin. (6) *Pre-register the acceptance test before
measuring* — T5d and T5e both ended as measured rejections of their own briefs,
and both were correct outcomes.

**Known pre-existing reds** (both confirmed by stash-and-rebuild at multiple
commits, neither introduced by this sprint):
`SurfaceDbPopulate.PropagatesStoredSurfacePolicyAndPersistsServedProvenance`
(`surface_db_populate_test.cpp:1364`, n_ok 0 vs 1) and
`SurfaceV2Provenance.ValidationFallbackAdmissionRecordsTheServedFamily`
(red at 547a466 with all lane-C work absent). The second names provenance at
exactly T7a's seam and is assigned to it.

---

## 2. Root cause of the calendar crossings

Not a hypothesis — a two-line proof from sourced asymptotics.

For SSVI/eSSVI, `w(k) = (θ/2){1 + ρψk/θ + √((ψk/θ + ρ)² + 1 − ρ²)}` with
ψ = θφ has asymptotic wing slopes

```
right:  ∂w/∂k    → ψ(1+ρ)/2
left:   ∂w/∂|k|  → ψ(1−ρ)/2
```

(Gatheral–Jacquier Remark 4.3: `w(k,t) = ((1±ρ)/2)·θ_t·φ(θ_t)·|k| + O(1)`.)

**If slice i+1 has a smaller wing slope than slice i, the two total-variance
curves must cross at some finite k**, because `w_{i+1} − w_i → −∞`.

Our sequential eSSVI fit couples slices **only through θ** (v):
`essvi_calib.cpp:1075-1079` floors the θ band at the previous slice's θ and does
nothing else. ψ is bounded only by the per-slice butterfly cap
`essvi_phi_max(theta, rho)` (`vol_surface.cpp:172-174`). Nothing orders the wing
slopes across maturities.

Flooring θ pins the slices at k = 0 and leaves the wings free. So a board with
N expiries needs N−1 consecutive pairs to satisfy the wing ordering **by luck** —
pass rate falls roughly as (1−p)^(N−1). That is why dense mega-caps do worse
than sparse small-caps, and it matches §1.4: the per-point rate is flat while
the per-board pass rate collapses with slice count.

Independent practitioner confirmation, same failure, same diagnosis — Wystup
(MathFinance FX Column, ~2020): *"They are non-intersecting in the region of
quoted volatilities… if we zoom out… the curves for 2Y and 1Y intersect… this
isn't a market-based arbitrage, but a model-based arbitrage. The problem is that
each SVI-fit is done for one maturity, and these tenor-wise calibrations don't
know of each other."*

### 2.1 The fix is a set of linear constraints

In coordinates (θ, ψ, χ := ρψ), for consecutive slices 1 (near) and 2 (far):

```
NECESSARY
  (N1)  θ₂ ≥ θ₁
  (N2)  ψ₂ + χ₂ ≥ ψ₁ + χ₁        (right wing slope non-decreasing)
        ψ₂ − χ₂ ≥ ψ₁ − χ₁        (left  wing slope non-decreasing)
        equivalently |χ₂ − χ₁| ≤ ψ₂ − ψ₁

SUFFICIENT (add)
  (S1)  ψ₂/θ₂ ≤ ψ₁/θ₁            (φ decreasing in maturity)  — bilinear
```

(N1) and (N2) are **linear**. Sources: Hendriks & Martini (2019) Prop 3.1;
Corbetta, Cohort, Laachir, Martini (2019); Mingone (2022). There is a genuine
discrepancy in the literature over whether (N1)+(N2) alone are sufficient —
Corbetta et al. say yes, Mingone and Pasquazzi say no. Pasquazzi (2023) Prop
4.14 resolves it against sufficiency. **Implement (N1)+(N2)+(S1).**

Mingone's **look-ahead cap** matters and a purely sequential scheme lacks it:
`C_ψi = min( (ψ_{i−1}/θ_{i−1})θ_i , f_i , f_{i+1}/p_{i+1} , … , f_N/∏_{j>i} p_j )`
with `f_i = min(4/(1+|ρ_i|), √(4θ_i/(1+|ρ_i|)))` and
`p_i = max((1+ρ_{i−1})/(1+ρ_i), (1−ρ_{i−1})/(1−ρ_i))`. Without it, slice *i* can
paint slice *i+3* into an infeasible corner.

### 2.2 A dormant hazard, not a current cause

`rho_eff` (`vol_surface.cpp:40-49`) blends ρ across k when `rho_scale > 0`. Every
published butterfly/calendar condition for SSVI/eSSVI assumes a **single ρ per
slice**, so a k-dependent ρ would void them.

**It is dormant** (v): `EssviParams::rho_scale{}` defaults to 0
(`vol_surface.hpp:106`), the function short-circuits at `:41`, and grep finds
**no assignment** to `.rho_scale` anywhere in `src/` or `include/` outside
archive round-trip. So this is not causing today's crossings. It is a loaded
gun: anyone who enables the blend silently invalidates the constraints T3 adds.
Either delete it or derive its conditions — see T9.

---

## 3. Breadth findings

### B1 — The rescue ladder is inert on the dominant route *(highest value)*

The predecessor sprint's 4-rung preparation ladder (`a2d43e0` ITM leg,
`4e88207` explicit strictness) lives entirely in `fit_curve_surface`. eSSVI —
the route taken by `LiquidSingleName`, `OrdinarySingleName` and non-event
`MegaCapEvent`, i.e. the plurality of the universe — dispatches instead to
`run_surface_parity` at `session.cpp:1430` (v).

`surface_parity.cpp` contains **zero** occurrences of `itm`,
`per_slice_itm_leg_fallback`, `board_starved_itm_leg_fallback`,
`per_slice_legacy_prep_fallback`, `board_starved_legacy_prep_fallback`, or
`linear_fallback` (v, grep over the whole file). The session sets those flags
unconditionally (`session.cpp:1199-1202`, `:1210`, `:1211`) and they are
silently dropped.

Rungs 2 and 4 (permissive predicate) are genuinely moot on that lane — it
already prepares permissively. **Rung 3 is not**: ITM-leg selection is
orthogonal to prep policy (`curve_fit.cpp:170-174`), and `per_slice_linear_
fallback` is likewise unavailable.

This partially undercuts W1-B from the predecessor sprint: its measured gains
came from non-eSSVI boards only. **Size: 2–4 days.** Prefer making
`run_surface_parity` delegate preparation to `prepare_fit_slice_into_slot` so
there is one preparation body, over porting the ladder twice.

### B2 — `FitContext` is never populated, disabling C8 and two profiles

The router's richest branches read `FitContext` (`fit_policy.hpp:54-63`) and no
production path writes a single field (v). The struct is plumbed intact —
`CorpusBoard::fit_context` ← `panel.fit_context` ← `spec.fit_context` ←
`market->fit_context` — but the only production producer of those cells sets
provenance strings (`dispersion_run.cpp:811`).

Consequences, each independently checkable:
- `is_event_window` (`fit_policy.cpp:11-16`) is always false ⇒ **the C8 route
  (`fit_policy.cpp:66`) is unreachable despite having a complete calibrator**,
  as is the dense-event LinearVariance route (`:58-63`).
- The Opening-phase C8 demotion (`fit_policy.cpp:95-98`) is dead code.
- `ProfileKind::VolProduct` unreachable: `profile.cpp:717` hardcodes
  `in.vol_product = false` (v), no vol products in the ticker-seed table, and
  the wide-book veto then forces them to `OrdinarySingleName`
  (`profile.cpp:527-529`). **The predecessor sprint's "vol products 100%" was
  achieved with those names routed as ordinary single names.**
- `ProfileKind::HtbDividendName` unreachable: needs `under.flags & kUflagHtb`,
  and `Underlying::flags` is written only by `tests/profile_test.cpp:203`.
- Earnings-proximity (`profile.cpp:487`) and forward-dispersion
  (`profile.cpp:493`) classifier axes never fire.

**Size: 3–5 days** to feed earnings dates, a borrow flag and session phase into
`FitContext` on the batch path. The consumer side is already built.

### B3 — The 13 residual refusals need two changes, not one

Class (a), `no strike carries a two-sided call and a two-sided put`
(`opra_panel.cpp:394-398`): reached only when **no** `(expiry, strike)` cell has
both legs strictly two-sided (`:262-266`, `:292-294`). A genuine board-shape
fact, not a tolerance.

The PCP spot is **not** necessary — it is an artifact of the loader having no
spot feed. `OpraLoadSpec::spot_override` exists (`opra_panel.hpp:186`, consumed
`opra_panel.cpp:1031-1032`) and the batch plumbing exists
(`opra_batch.hpp:68` → `opra_batch_detail.hpp:237`), but **nothing in production
fills it**.

**An external spot alone will not fix these boards.** With zero two-sided pairs,
every expiry's carry solve also fails ⇒ `CarryFailed`
(`curve_fit.cpp:603-609`), and phase-1.5 repair needs ≥1 confident anchor
(`:758-762`). The escape hatch exists — `DeAmOptions::imply_borrow=false` +
`borrow_fixed` (`deamer.hpp:319-320`) returns `confident = true`
(`deamer.cpp:523-531`) — with **no production caller**.

So: (i) a market-input source populating `spot_override` per (date, symbol),
1–2 days; (ii) a carry policy falling back to a supplied borrow when no
co-terminal pair exists, 2–3 days. Both, or neither helps.

### B4 — Instrument conventions that are simply not wired

| shape | defect | size |
|---|---|---|
| ~~**0DTE index boards**~~ | **WITHDRAWN — §1.5 (5).** Stale claim. `opra_panel.cpp:784-786` maps every convention except `UsIndexAmOpen` to `SettlementSession::Pm` (`7b402ae`, pre-sprint). Worth **0 expiries**; pinned by a test in `9777b7c`. | **0** |
| **Cash-settled European index (SPX/NDX/VIX)** | `exercise_style` defaults to `American` (`opra_panel.hpp:208`); the European branches (`deamer.cpp:814`, `prepared_fitting.cpp:247`, `parity.cpp:123`) are unreachable in production. Index boards run American de-Am on European options. **LANDED `9777b7c`** — both fields become `std::optional` overrides resolved from the board's OSI root through a curated registry (keyed on root, not index: SPX is AM-settled but SPXW is PM; OEX is cash-settled *American* while XEO on the same index is European). Measured: exercise style Amer→Euro takes `n_quotes_used` 576 → 1702 and `mean_rmse_vol` 5.8e-5 → 0 on a real SPX board. | done |
| **Boards spanning a dividend** | No dividend source on the surface path; `cash_divs` only reaches the fit via the dispersion runner's TSV. A discrete dividend is absorbed into implied borrow, and phase-1.5 then **interpolates linearly across the ex-date step** (`curve_fit.cpp:765`). | 2–3 days |
| **Hard-to-borrow** | PCP borrow solve brackets `[-0.5, +0.5]` (`deamer.cpp:66-67`); a genuine squeeze cannot be resolved and surfaces as `CarryFailed`. | 1 day + B3(ii) |
| **Few expiries (1–3)** | **Supported.** Calendar floor skipped while empty (`curve_fit.cpp:935`), `min_fitted_expiries` 1, selector refusal advisory. Wants a regression test, not a fix. | — |

### B5 — Curve-family inventory

`VolCurveKind` (`vol_curve.hpp:78-94`) has six members. Corrected taxonomy: the
`Wing`/`CStar16M` names I referred to previously live in the separate legacy
`Parametrization` enum (`vol_surface.hpp:69-76`), not in `VolCurveKind`.

- **CStar** has a *complete, compiled* calibrator (`cstar_calib.hpp:101`,
  `src/cstar.cpp`, `src/cstar_calib.cpp`, both in `CMakeLists.txt:48-49`) but
  **no `VolCurveKind` tag**, so `fit_slice_curve` cannot reach it under any
  configuration. Dead capital.
- **Wing** has no calibrator at all; every dispatch arm is a no-op or NaN
  (`vol_surface.cpp:238`, `projection.cpp:59,79`, `arb.cpp:49`).
- **S3** is a fixture generator (`s3.hpp:71-110`), not an `IVolCurve`.
- **SplineVol** is half-wired: gated on `CurveConfig::spline_candidate`
  (default false), refit returns `NotImplemented`, excluded from the DB
  (`surface_db.cpp:348`, `curve_kind <= 4`).
- **SABR is absent entirely.** A genuine gap, but not a defect — and eSSVI
  dominates it for thin slices (3 params, butterfly-certified, whereas Hagan's
  expansion admits negative densities at low strikes / long maturities).

Hygiene: stale comment at `vol_curve.hpp:77` ("C8 / CStar are deferred");
no `parse` for `VolCurveKind` — the only string→kind map is an ad-hoc chain at
`universe_autofit.cpp:371-377` that silently defaults unknown strings to
`ConvexDense` and does not round-trip `to_string`.

### B6 — The selector is a one-horse race, blocked by a prep pin not a list

`production_selector_config()` (`curve_selector.cpp:77-83`) pushes exactly one
candidate, eSSVI. `bounded_selector_candidates()` (`:56-75`) has **three test
callers and zero production callers** (v).

But widening the list buys nothing yet: the selector hardcodes
`PreparedObservationPolicy::Configured` (`curve_selector.cpp:303`) for
cross-candidate comparability while the served eSSVI path prepares permissively,
so on real data every refusal is `sampled=8 prepared=0` (`:285-294`). **Unpin
preparation first, then widen.** Two further caveats: the ranking metric
`oos_vw` has no complexity penalty (a 48-node dense curve is compared to a
5-parameter eSSVI on raw held-out fit), and the relaxed 0.5 coverage floors that
`d591457` introduced deliberately break cross-family comparability.

---

## 4. Depth findings

### D1 — ~~The number that gates arbitrage is computed on every board and never exported~~ → it was not computed at all

**WITHDRAWN as stated — see §1.5 (4).** `universe_autofit` read 7 of ~30
`SessionDiagnostics` fields (`:416-423`) and never touched `risk_health`; the
`ValidationDigest` (`surface_policy.hpp:111-142`) carries 11 counters and 5
slacks and is reachable at `pricer_fitter.hpp:492-495`. All true. But the digest
was not *computed* on the benchmark path, because that path never engaged v2 —
so the counters were default-constructed zeros, which read identically to
"verified clean". Fixed by T1 (`91904d3`, `oracle_ran` guard column) and T1b
(`5b41cb9`, `--fit-path production`).

### D2 — `CalendarRepair` is inert on every non-eSSVI board

The risk policy sets it (`pricer_fitter.cpp:1250`) and it is copied into parity
inputs (`session.cpp:1165`), but non-eSSVI dispatches to `fit_curve_surface`,
and **`curve_fit.cpp` never reads `in.repair`**. So `arb_project_calendar_essvi`
and the whole `MonotoneFit`/`Project` machinery are unreachable for
ConvexDense / SVI / C8 / SplineVol / LinearVariance. Either wire a repair stage
or make the field an error on that lane, so it stops reading as
configured-and-working.

### D3 — The SVI Lee gate is loose by 2×

Lee's bound in total variance is `limsup w(k)/|k| ≤ 2`. For raw SVI the wing
slope is `b(1+|ρ|)`, so the bound is **`b(1+|ρ|) ≤ 2`**. Our gate is
`b(1+|ρ|) ≤ 4` (`arb.cpp:632-641`), chosen "matching the eSSVI convention" —
but eSSVI carries an explicit `θ/2` prefactor that raw SVI does not. The
codebase's own oracle uses 2.0 (`risk_surface_validation.hpp:67`), so two gates
in the same library disagree by 2×.

The backstop that would catch this doesn't: the oracle measures a *local* slope
over the last 0.0078-wide grid interval at k = ±0.5
(`risk_surface_validation.cpp:390-391`), and SVI/eSSVI approach their asymptotes
slowly, so a slice with asymptotic slope 3.5 passes. For SVI/eSSVI the
asymptotic slope is closed-form — no sampling needed.

SVI is the auto-route for `IlliquidSmallCap` / `HtbDividendName` / `VolProduct`.

### D4 — Reported quality is not the served surface's quality

- `CandidateScore::rmse_vol` is an **out-of-sample** number from a
  **half-density** fit: `curve_selector.cpp:322-334` splits strike-ordered rows
  alternately, fits on evens, scores on odds. Production serves a full-board
  fit whose error is never measured. Wrong data, wrong fit, wrong weighting.
- `chi2_reduced` hardcodes **dof = 3 for every family**
  (`curve_fit.cpp:1098`). C8 has 8 parameters; ConvexDense has up to `node_cap`
  nodes. Systematically optimistic — a 40-node fit on 60 quotes reports N−dof =
  57 instead of 20.
- `max_weight = 1e3` (`calib.hpp:155`) **collapses European-exercise weights to
  uniform**: with `max_spread_vol = 0.05` every surviving row has
  `weight_w ≥ 400/(2σT)²`, exceeding 1e3 for σT < 0.316 — every listed tenor to
  ~1.5y at 20 vol. The de-Am builder deliberately skips this clip and says why
  (`calib.cpp:1490-1495`); the European branch never got the same fix.
- No robust loss in ConvexDense or SplineVol.

Weighting itself is correct (`obs_weight_w` = vega²/spread²/(2σT)²). The defect
is entirely on the reporting side.

### D5 — Served-but-bad surfaces are structurally unreportable

`fit_slice_curve` (`vol_curve.hpp:517-523`) has **no diagnostics out-param** —
unlike `refit_slice_curve`, which takes `FitDiag*`. So RMSE, iteration counts
and every signal below cannot reach the caller on the serving path. Downstream
of that:

- **eSSVI can serve the seed cube as a "fit."** When damped normal equations
  stay non-PD until λ > 1e8, `lm_step` returns −1.0
  (`essvi_calib.cpp:340-344`) and the caller keeps the last good cube
  (`:881-883`) — on the first inner step that is the *seed*, returned `Ok`.
- **"Totally stuck" is indistinguishable from "converged":** when every
  backtrack is rejected the step routine returns unchanged parameters
  (`svi_calib.cpp:770-775`), the caller sees step norm exactly 0 and breaks on
  the convergence test (`:1160-1163`). `inner_iters_total` reads *small* — the
  counter moves the wrong way.
- **Every polytope projection is silently swallowed** — `mm_project_admissible`
  returns `touched` and all six call sites discard it. A slice pinned at
  b = 1e-8 (flat), |ρ| = 1−1e-4, or the Lee cap is served as an ordinary fit.
- **C8 revert-to-seed is unflagged**: an inadmissible or butterfly-violating C8
  fit is discarded for the eSSVI-derived JW seed (`vol_curve.cpp:703-716`) and
  the caller sees a "C8 curve" that is a plain SVI-JW smile.
- Every calibrator except the ConvexDense QP returns `Ok` at the iteration cap.
  Only ConvexDense fails closed and carries a real certificate.
- eSSVI is **warm-started from the previous surface** within 5 days
  (`essvi_calib.cpp:993-1011`) with a sequential θ-floor loop-carry, and
  `FitDiag` has no cold-vs-warm field — two runs producing different parameters
  look identical in diagnostics.

### D6 — De-Americanization: the audit cannot see the error that matters

- `de_americanize_chain` has **zero production callers** (v) — tests only. The
  docs at `surface_parity.hpp:23` claiming each expiry runs it are stale.
- The accuracy gate is **spread-normalised**: `residual / (0.5 * spread)`,
  budget 0.25 (`deamer.cpp:110-116`), i.e. an absolute budget of `spread/8`. A
  wide-spread board cannot produce a large number by construction. **This is
  why "zero de-Am rejections on thin boards" and large silent error coexist.**
- The audit is **structurally blind to carry error**: it reprices with the same
  `q_eff` the inversion used (`deamer.cpp:144-145`), so a wrong borrow yields
  σ such that `price(σ, q_wrong) = mid`, and the audit passes.
- **Missing IV-band guard on the Legacy path.** `american_iv.cpp:240-242`
  returns 0.005 for a price at/below intrinsic; `calib.cpp:1337` rejects that
  but `prepared_fitting.cpp:499-541` only checks `has_value()`. Combined with
  the board-wide ITM-leg fallback and no |k| cutoff, **a deep-ITM rescued leg
  quoted at intrinsic enters the fit as a 0.5%-vol observation** — and passes
  the audit. Affects exactly the thin boards in scope.
- Fit and serve legs use different Andersen–Lake presets under `Bulk`
  (`session.cpp:1072` vs `:1084`), so the round trip is not the identity, and
  parity is scored at the *fit* rung.
- `n_atm = 1` under Fast/Hft makes every carry-confidence diagnostic report the
  most reassuring value for the least reliable carry: dispersion 0, LOO shift 0,
  confidence half-width 0.

The literature is harsher than our own diagnostics. Koster/Menn/Oeltz feed
*noiseless, model-consistent* American prices to exactly our iterate-on-parity
loop and it converges fast to **5.92% against a true 5.00% dividend** — +18%
relative — because the American call may exercise at the dividend date and so
prices partly off a shorter maturity; forcing call and put IVs to agree by
moving the forward **mis-attributes a term-structure effect to carry**.
Burkovska et al. measure ≤1% vanilla price error concealing a **50% error in
calibrated σ** and a Heston barrier repricing 8.75 → 4.54. And deep-ITM
de-Americanization is **ill-posed**, not merely inaccurate: their Remark 4.1
gives S₀=100, K=120, r=1%, P_Am=20.00 with two roots u≈1.036 and u≈1.112 giving
European prices 18.81 vs 19.69. Guard: `P_Am > (K−S₀)⁺·1.01`.

### D7 — `refit_slice` flips the surface-wide calendar flag from a 3-slice check

`session.cpp:2651-2661` builds an adjacent-only `[prev, candidate, next]`
surface, then `:2683` sets `diag_.calendar_arb_free = true` unconditionally for
the **entire** surface, without touching `n_calendar_viol_pre`. That breaks the
documented `calendar_arb_free == (n_calendar_viol_pre == 0)` invariant
(`session.hpp:355-359`). The price-bound check immediately below is re-run over
the full surface for exactly this reason; calendar didn't get the same
treatment.

---

## 5. Workstreams

Strict file ownership so tasks can run in parallel worktrees. A task must not
edit another task's files; if it needs to, stop and escalate.

### T1 — Export the oracle. **Do this first, alone.** *(S, ~1 day)*

**Owns:** `atx-vol/examples/universe_autofit.cpp`, `atx-vol/tools/analyze_universe_autofit.py`

Export from `risk_health.validation`: `n_calendar_violations`,
`n_butterfly_violations`, `n_price_bound_violations`, `n_wing_violations`,
`max_calendar_slack`, `first_calendar_k`, plus `risk_health.state` and the
already-exported `n_calendar_viol_pre` / `n_price_bound_violations`.

**Acceptance:** a run over lqbench + control reporting, for every board, the
oracle's verdict alongside the legacy boolean. Answer explicitly: **how many
served boards violate calendar inside |k| ≤ 0.5?** If that number is ~0, §2's
fix is about the wings only and T3 can be scoped down. If it is material, §2's
fix is load-bearing. **Nothing else in this sprint should start before T1
reports.**

### T2 — Davis–Hobson feasibility gate *(S, ~1–2 days)*

**Owns:** new `atx-vol/include/atx/vol/detail/quote_feasibility.hpp` + `src/`, `atx-vol/tests/quote_feasibility_test.cpp`

Before fitting a slice, test whether *any* arbitrage-free surface reproduces its
quotes. Normalise `r_i = p_i/DF`, `k_i = K_i/F`, add `(0, 1)`. Build the support
function R (largest decreasing convex minorant). Davis–Hobson Thm 3.1: prices
are consistent with no-arbitrage **iff** R is strictly decreasing on
`[0, k_{n₀∧n}]`, `R'(0+) ≥ −1`, and `R(k_i) = r_i` ∀i. One monotone convex-hull
pass, O(n) on sorted strikes.

**Acceptance:** classify every currently-refusing slice as *infeasible data*
(weak or model-independent arbitrage) vs *fitter limitation*. This re-scopes B3
and the tier-D gap. Report the split; do not change routing yet.

### T3 — Cross-slice wing-slope ordering in the eSSVI fit *(L, ~5–8 days)* — **LANDED** `3e1663e` (add-only residual) + `5c9e3d3` (projected LM); (S1) split out to T3c. Acceptance criterion replaced per §1.7; results in §1.9.

**Owns:** `atx-vol/src/essvi_calib.cpp`, `atx-vol/include/atx/vol/essvi_calib.hpp`, `atx-vol/tests/essvi_calib_test.cpp`

Implement §2.1. Add (N1)+(N2)+(S1) to the sequential path, in (θ, ψ, χ=ρψ)
coordinates where (N1)/(N2) are linear. Add Mingone's look-ahead cap so slice
*i* cannot make slice *i+3* infeasible.

Recommended shape, from Pasquazzi's 108-chain comparison: **Corbetta anchoring
as the warm start, Mingone's box as the refinement.** Corbetta anchors the slice
at the observed near-ATM point (k*, θ*) via `θ = θ* − ρψk*`, leaving 2 free
parameters and a closed-form ψ interval per ρ — which is also the best available
answer for thin slices (§B, T7). Mingone's box makes the whole N-slice
admissible region a product of intervals, so bounded LM with an analytic
Jacobian and no active-set logic suffices.

**Acceptance:** calendar violations inside the certified band go to zero and stay
there; `rmse_vol` on the control corpus does not worsen by more than 5%
like-for-like; fit CPU within 1.3×. Corbetta report 1.2 s in Python for 12
maturities × ~98 options, ~0.01 s estimated in C#, so this should be cheap.
**Do not start before T1.**

### T4 — Exact crossing detection; state the certified band *(M, ~3 days)* — **LANDED** `3f475f3`..`a97c2c2`; quartic is exact against a 200k oracle (§1.9). Banded in/out-of-band counters escalated to a follow-on.

**Owns:** `atx-vol/src/arb.cpp`, `atx-vol/include/atx/vol/arb.hpp`, `atx-vol/tests/arb_test.cpp`

Two changes:
1. Replace grid sampling with the **quartic root test**. Two raw-SVI (or eSSVI
   difference) slices cross exactly where a quartic has a real root
   (GJ Lemma 3.3 / eq. 3.7); Ferrari/Cardano closed form, ~50 flops, exact
   crossing locations, no missed narrow crossings. Back-substitute to drop
   spurious roots from the two squarings. Keep the grid scan as a test oracle.
2. Make the served-surface gate report **in-band and out-of-band violations as
   separate counters**, over one band and one grid for both lanes. A fixed
   |k| ≤ 0.60 window is delta-inhomogeneous by eleven orders of magnitude across
   a board — at 1 week / 35 vol the call delta at k = +0.60 is 0 to machine
   precision; at 2 years it is 0.084. Report in a delta band (Wystup's 1%–99%)
   or in standard-deviation moneyness `k/√w`. Keep an unbounded-k check as an
   *internal invariant*, because Roper (IV4) is quantified over all of ℝ and
   anything computing Dupire local vol off this surface needs it.

**Acceptance:** one metric, one band, documented; the two lanes comparable.

### T5 — Deep-ITM de-Am guard and a real round-trip metric *(M, ~3 days)* — **LANDED** as T5c `2e6163c..3e1d0dc`; follow-ups T5d (units re-base, measured and REJECTED, reverted) and T5e (fallback borrow, exonerated on control) in §1.10

**Owns:** `atx-vol/src/deamer.cpp`, `atx-vol/include/atx/vol/deamer.hpp`, `atx-vol/src/prepared_fitting.cpp`, `atx-vol/tests/deamer_test.cpp`

1. Hard-gate `P_Am > (K−S₀)⁺·(1+δ)`, δ = 1% (D6, Burkovska Remark 4.1). Below
   that the inversion is multi-valued and the answer depends on the bracket.
2. Close the IV-band hole at `prepared_fitting.cpp:499-541` so an intrinsic-priced
   deep-ITM rescued leg cannot enter as a 0.5%-vol observation.
3. Add an **absolute** round-trip metric in vol points — de-Am → fit →
   re-Americanize → compare to the original American mid — alongside the
   spread-normalised ratio. The current gate cannot fail on a wide board.

**Acceptance:** the round-trip error is reported per board on the benchmark, and
the thin-board population is measured rather than assumed clean.

### T6 — One-sided quotes as bounds, not discards *(M, ~4 days)* — **LANDED** `e8f09c5`+`01040c0`; headline re-based +14 → +10 by T6d (`8ea0605..544bf60`, exact-median sampler fix) in §1.10

**Owns:** `atx-vol/src/calib.cpp`, `atx-vol/include/atx/vol/calib.hpp`, `atx-vol/src/opra_panel.cpp`, `atx-vol/tests/calib_test.cpp`

Do **not** use hard inequality constraints — Cohen–Reisinger–Wang warn that
arbitrage-free prices may not exist inside the bid-ask box, making the problem
infeasible. Use their asymmetric soft penalty, with
`δ_j^a = ask_j − ref_j`, `δ_j^b = ref_j − bid_j`:

```
f_j(x) = max( −x − δ_j^b + ε₀ ,  −(ε₀/δ_j^b)·x ,  (ε₀/δ_j^a)·x ,  x − δ_j^a + ε₀ )
ε₀ = (1/N)·min_j(δ_j^a ∧ δ_j^b)
```

Slope `ε₀/δ` inside the band, unit slope outside — so it *is* liquidity
weighting, degrading to `1/spread` when both sides quote. A bid-only strike sets
`δ^a = +∞` and keeps a one-sided arm. Convex and piecewise-linear, so it drops
into the existing IRLS loop. Also: project a missing or incoherent bid onto the
lower arbitrage bound rounded up to the tick and **keep the strike**, rather
than discarding it.

**Acceptance:** strikes currently dropped for one-sidedness contribute; tier-D
`rmse_vol` does not worsen; the count of admitted-as-bound legs is reported.

### T7 — Thin-slice shape borrowing *(M, ~4 days)* — start after T3

**Owns:** `atx-vol/src/curve_fit.cpp` (prep/fallback region only), `atx-vol/tests/curve_fit_test.cpp`

For slices below the identifiability floor, freeze SVI-JW `{ψ, p, c, ṽ}` from
the neighbour or a board-level fit and fit only `v_t` — **a 1-parameter fit that
needs 1 quote**. Justification is Gatheral–Jacquier's own: *"If smiles scaled
perfectly as 1/√w_t (as is approximately the case empirically), these parameters
would be constant, independent of the slice t."* Pair with their closed-form
butterfly guarantee `c' = p + 2ψ`, `ṽ' = v·4pc'/(p+c')²`.

Depends on T3 because the borrowed shape must satisfy the ordering constraints.

### T8 — Breadth wiring *(M, ~4 days, parallelisable internally)*

**Owns:** `atx-vol/src/opra_panel.cpp` (loader defaults), `atx-vol/src/opra_batch*.cpp`, `atx-vol/include/atx/vol/opra_panel.hpp`

B4 rows 1–2 (0DTE PM close, European index exercise style) and B3(i)
(`spot_override` feed). Cheap, high-visibility, independent of the depth work.
Conflicts with T6 on `opra_panel.cpp` — sequence them or split the file
ownership by function.

### T9 — Decide the ρ-blend, prune the dead *(S, ~2 days)* — **LANDED** `be727d0`; 0 armed slices in 162,793, retired (§1.9).

**Owns:** `atx-vol/include/atx/vol/vol_surface.hpp`, `atx-vol/src/vol_surface.cpp`, `atx-vol/include/atx/vol/vol_curve.hpp`

Either delete `rho_R`/`rho_scale` or derive and enforce its no-arbitrage
conditions (§2.2). It is dormant today; leaving a switch that silently voids
T3's guarantees is the worst of the three options. Also: fix the stale comment
at `vol_curve.hpp:77`, add a real `parse_curve_kind` that round-trips
`to_string`, and decide whether CStar gets a `VolCurveKind` tag or gets deleted.

### T10 — Diagnostics out-param on the serving path *(M, ~3 days)* — **LANDED** `b8e8825` (D3 gate+projector) + `ef8e2a1` (FitDiag out-param, cold path). D4 and the three `nullptr` production sites are T10b.

**Owns:** `atx-vol/include/atx/vol/vol_curve.hpp`, `atx-vol/src/vol_curve.cpp`, `atx-vol/src/svi_calib.cpp`, `atx-vol/src/c8_calib.cpp`

Give `fit_slice_curve` a `FitDiag*` out-param mirroring `refit_slice_curve`, and
populate: converged flag, iteration count, final gradient norm, `projection_
touched`, `reverted_to_seed`, `warm_started`. Nothing in D5 is observable until
this exists. Conflicts with T3 on `essvi_calib.cpp` — T3 owns that file; T10
takes the others and T3 adds the eSSVI fields.

---

## 6. Sequencing

```
T1  ────────────────────────────────  (alone; gates everything)
     │
     ├── T2 ──────────  (re-scopes breadth; report before T7/T8 land)
     ├── T3 ──────────────────────── T7          (depth core)
     ├── T4 ──────────  (metric; independent of T3's implementation)
     ├── T5 ──────────
     ├── T6 ──────────  ┐ conflict on opra_panel.cpp
     ├── T8 ──────────  ┘ sequence or split by function
     ├── T9 ──────────
     └── T10 ─────────  (T3 owns essvi_calib.cpp)
```

T1 first and alone. T2 next, because it may show that a chunk of the residual
9.3% is data that no fitter can serve, which would change T6's and T8's value.
Everything else parallelises with the ownership above.

> **As executed.** T1 → T1b → T1c on lane A; T2 → T9 on lane B; T8 → T5c on
> lane C. T1b was inserted because T1 showed the benchmark was measuring the
> wrong code path (§1.5). T5 was reshaped into **T5c** (carry) because
> measurement moved the binding constraint off geometry (§1.6). T2 reported
> **0 of 4** residual refusals are Davis–Hobson-infeasible data — every one is a
> fitter or plumbing limitation, so gate 3 cannot be closed by classification
> and must be closed on the number.

---

## 7. Acceptance gates

1. **Calendar, stated honestly.** One certified band, one grid, both lanes.
   In-band violations on served surfaces: **zero**. Out-of-band reported
   separately and not treated as a defect unless something evaluates there.
   → **MET on the production path before any depth work landed** (§1.5): 0
   violations across 155,285 calendar samples. Restated so it cannot be read as
   a win this sprint earned. The live version of this gate is **1′** below.
2. **Butterfly measured.** A surface-level butterfly counter exists and is
   exported for both lanes. Currently it is structurally zero on 92–96% of
   boards and we cannot say anything about it. → **MET** by T1 (`91904d3`): the
   oracle's strike/butterfly counters are exported with an `oracle_ran` guard so
   "not computed" is distinguishable from "computed and clean". 0 violations
   across 308,817 strike samples.
3. **Tier D fit success ≥ 95%**, or a Davis–Hobson classification showing the
   residual is infeasible data rather than a fitter limitation. Either outcome
   closes the gate; a bare number does not. → T2 classified **0 of 4** as
   infeasible data, so this gate must be closed on the number. Baseline is
   1.8% refusing (§1.5 (6)), not 9.3%.

**Rebaselined gates, added after measurement:**

- **1′. eSSVI survives as PRIMARY.** The primary-rejection rate falls from
  60% (lqbench) / 68% (control). This replaces T3's vacuous stated criterion
  (§1.7). Scope set by T1c.
- **9. Carry breadth.** Of the 29 boards lost to carry, the count recovered
  **with a known, confidence-stamped carry** is reported — never by relaxing
  `require_carry_confidence`. The 42% slice / 47% quote loss and the 177/192
  `degraded`-on-`CarryGap` population are reported before and after.
- **10. Carry confidence is distributional, not binary.** A recovered board
  reports *which* fallback source supplied its carry and at what confidence. A
  board that returns with a fabricated carry is worse than a board that refuses.
4. **Control corpus holds:** 416/416, eSSVI in-band median ≥ 0.94, and
   like-for-like `rmse_vol` no worse than 1.05×.
5. **Fit CPU ≤ 1.3×** the current 0.50× baseline, load-normalised. Do not
   divide the fit ratio by the load ratio — see the predecessor plan's method
   notes.
6. **De-Am round-trip** reported in absolute vol points on every benchmark
   board, not only as a spread-normalised ratio.
7. **No served surface is silently degraded:** projection-touched,
   reverted-to-seed, and iteration-capped all reach the caller.
8. Targeted tests pass; `hygiene` preset clean.

---

## 8. Considered and rejected

| Approach | Why not |
|---|---|
| **Carr–Pelts / Ensemble CP** | 3.11 s per SPX surface, and every price evaluation contains a Newton solve for ẑ — nondeterministic cycle count on the hot path. No implied vol without a further numerical inversion. Genuinely good for an LV/LSV stack; wrong tool here. |
| **Neural surface smoothing** (Ackerer et al.; Zheng et al.) | 1.3–2.2 min per surface on K80s, no batching win since each board is its own fit. No-arbitrage enforced as a *soft penalty on a synthetic grid*, so you must run a checker anyway — at which point a cheap QP projection dominates. Non-deterministic and seed-sensitive. The one idea worth stealing is the multiplicative-corrector structure, which needs no network. |
| **Fengler constrained-spline QP** | Good nightly de-arbitrage sweep, not a tick-path fitter: a capped-iteration QP can return an *infeasible* point, the backward maturity chaining serialises expiries, and the output is a price spline needing a BS inversion per served strike. Keep in reserve. |
| **Andreasen–Huge / discrete local vol** | Viable — 3.2 ms/expiry measured, sub-ms achievable with a fixed grid and a hand-rolled Thomas solver — but grid selection is data-dependent (branchy hot path) and the output is a discrete price vector needing an arbitrage-preserving interpolant, with a documented staircase density and spurious ATM spike. Reserve as a fallback for boards where the parametric fit exceeds bid-ask. |
| **SABR for thin slices** | eSSVI is also 3 parameters *and* butterfly-certified; Hagan's expansion admits negative densities at low strikes and long maturities. Strictly dominated for this use. |
| **Widening the arb tolerance** | Treats the symptom. With (N1)(N2)(S1) the surface is calendar-free on all of ℝ, which makes the band debate moot. |

---

## 9. Reproducing the measurements

```
# instrumented example (T1 extends this)
cmake --build build-rel --target universe_autofit

build-rel/bin/universe_autofit.exe \
  --opra-root C:/atx-data/opra-v1-lqbench-1555 --date 2026-08-03 \
  --symbols-file C:/atx-data/universe/lqbench_clean.txt \
  --snapshot-suffix T19:55:00Z --r 0.043 --preset robust --out lq.csv

# control (all four sessions are stamped T19:55:00Z)
build-rel/bin/universe_autofit.exe \
  --opra-root <scratch>/opra-v1-hivebench --date 2026-07-22 \
  --symbols-file <scratch>/sp100.txt \
  --snapshot-suffix T19:55:00Z --r 0.043 --preset robust --out sp100.csv
```

Gate scorer and the tier map live in the predecessor sprint's artifacts.

---

## 10. References

**Calendar / SSVI constraints**
- Gatheral & Jacquier, *Arbitrage-free SVI volatility surfaces*, Quantitative Finance 14(1):59–71 (2014). [arXiv:1204.0646](https://arxiv.org/abs/1204.0646). Thm 4.1 (calendar, iff), Thm 4.2 + Lemma 4.2 (butterfly; cond. 1 necessary = Lee), Remark 4.3 (wing slopes), Lemma 3.3/eq. 3.7 (quartic), §5.1 (butterfly repair), §5.2 (crossedness), Thm 4.3, Lemma 5.1.
- Hendriks & Martini, *The extended SSVI volatility surface*, J. Computational Finance 22(3):25–39 (2019). [SSRN 2971502](https://papers.ssrn.com/sol3/papers.cfm?abstract_id=2971502). Prop 3.1.
- Corbetta, Cohort, Laachir, Martini, Decisions in Economics and Finance 42(2):665–677 (2019). [arXiv:1804.04924](https://arxiv.org/abs/1804.04924). Anchoring; ρ-sampling + Brent.
- Mingone, Quantitative Finance 22(12) (2022). [arXiv:2204.00312](https://arxiv.org/abs/2204.00312). Global box, Prop 3.1, look-ahead cap.
- Pasquazzi, *eSSVI Surface Calibration*, [arXiv:2304.02106](https://arxiv.org/abs/2304.02106) (2023). Prop 4.14 resolves the necessity/sufficiency discrepancy; 108-chain empirical comparison.
- Roper, *Arbitrage Free Implied Volatility Surfaces*, Univ. of Sydney (2010). Thm 2.9 (IV1–IV6), Thm 2.15.
- Wystup, *Calendar arbitrage in the FX volatility surface*, MathFinance FX Column (~2020).

**Wings**
- Lee, *The Moment Formula for Implied Volatility at Extreme Strikes*, Mathematical Finance 14(3):469–480 (2004). Thm 3.2/3.4; §5.1 on spline extrapolation.
- Benaim & Friz, *Regular Variation and Smile Asymptotics*, Mathematical Finance 19(1):1–12 (2009). [arXiv:math/0603146](https://arxiv.org/abs/math/0603146).

**Quotes, feasibility, repair**
- Davis & Hobson, *The Range of Traded Option Prices*, Mathematical Finance 17(1):1–14 (2007). Thm 3.1.
- Cohen, Reisinger & Wang, *Detecting and Repairing Arbitrage in Traded Option Prices*, Applied Mathematical Finance 27(5):345–373 (2020). [arXiv:2008.09454](https://arxiv.org/abs/2008.09454). Asymmetric penalty; infeasibility warning.
- Gerhold & Gülüm, [arXiv:1608.05585](https://arxiv.org/abs/1608.05585). Bid-ask consistency conditions.
- Echenim, Gobet & Maurice, [arXiv:2207.02989](https://arxiv.org/abs/2207.02989) (2022). Missing-bid projection.
- Le Floc'h, [arXiv:2004.08650](https://arxiv.org/abs/2004.08650); *Arbitrage in Zeliade's SVI example*.
- Cont & Tankov, J. Computational Finance 7(3) (2004), §4.3. Morozov discrepancy principle for the regularisation weight.

**De-Americanization**
- Burkovska, Gaß, Glau, Mahlstedt, Schoutens, Wohlmuth, *Calibration to American options: numerical investigation of the de-Americanization method*, Quantitative Finance 18(7):1091–1113 (2018). [arXiv:1611.06181](https://arxiv.org/abs/1611.06181). Remark 4.1 (ill-posedness); parameter vs price error. **No dividend results — the dividend claim is their conjecture.**
- Koster, Menn & Oeltz (RIVACON), *Forward Fitting to Quotes of American Options*. The +18% relative dividend error on noiseless input.
- Liu, Leitao, Borovykh & Oosterlee, Applied Mathematical Finance (2022). [arXiv:2001.11786](https://arxiv.org/abs/2001.11786), §2.3.2. Early-exercise differential.
- Andersen, Lake & Offengenden, J. Computational Finance 20(1):39–87 (2016). [SSRN 2547027](https://papers.ssrn.com/sol3/papers.cfm?abstract_id=2547027).
- Healy, [arXiv:2109.15157](https://arxiv.org/abs/2109.15157), Table 6. ~5.6 µs/price batched.
- Battauz, De Donno & Sbuelz, Management Science 61(5):1094–1107 (2015). Double continuation region under negative carry — relevant to HTB names.

**Rejected approaches**
- Andreasen & Huge, *Volatility Interpolation*, Risk 24(3) (2011). [SSRN 1694972](https://papers.ssrn.com/sol3/papers.cfm?abstract_id=1694972).
- Fengler, Quantitative Finance 9(4):417–428 (2009). [EconStor 10419/25038](https://www.econstor.eu/bitstream/10419/25038/1/496021680.PDF).
- Antonov, Spector & Konikov, Risk (Aug 2020). [SSRN 3403708](https://papers.ssrn.com/sol3/papers.cfm?abstract_id=3403708).
- Ackerer, Tagasovska & Vatter, NeurIPS 2020. [arXiv:1906.05065](https://arxiv.org/abs/1906.05065). Table 6 timings.

---

## 11. Unverified, flagged

- ~~§1.3's inference that every `status=ok` board passed the risk oracle rests on
  `surface()` returning the risk surface by default. T1 confirms it
  empirically.~~ **RESOLVED — and the flag was right to be there.** T1 falsified
  it on the path §1.4 measured; T1b re-measured on production. §1.5 (3).
- ~~All breadth reachability claims are static (grep + read); nothing was
  executed to observe a branch being taken.~~ **PARTLY RESOLVED.** Executing
  them falsified two: B4 row 1 was already fixed (§1.5 (5)), and the 9.3%
  refusal figure does not reproduce (§1.5 (6)). **Six plan hypotheses have now
  been falsified by measurement.** Any remaining static claim in this document
  should be assumed unmeasured until a run says otherwise.
- The 2× Lee-bound convention error (D3) is **verified**, not flagged:
  `arb.cpp:632-641` gates raw SVI at `b(1+|ρ|) ≤ 4`, justified in-comment as
  "matching the eSSVI/Mingone-cube convention". eSSVI's `w = (θ/2){…}` carries a
  ½ prefactor, so `ψ(1+ρ) ≤ 4` *is* Lee's slope-2 bound there; raw SVI has no
  prefactor and its bound is `≤ 2`. The convention was ported across a
  parametrization boundary that changes it by exactly 2×. Compare
  `risk_surface_validation.hpp:67`: `max_abs_wing_total_variance_slope{2.0}`.
- The Corbetta-vs-Mingone sufficiency discrepancy was resolved via Pasquazzi,
  not by reading Hendriks–Martini directly (paywalled).
- Frequency of the ConvexDense wing-fallback path (D-review F5): the counter
  exists but is compiled out by default.
- Hull eq. 11.11's dividend parity bound: three agreeing secondary sources, no
  primary fetch.
- No vendor's explicit minimum-quote count could be found. A data-sufficiency
  *gate* demonstrably exists (Cboe's VIX methodology refuses outright when all
  OTM puts are excluded); a numeric threshold could not be verified.
