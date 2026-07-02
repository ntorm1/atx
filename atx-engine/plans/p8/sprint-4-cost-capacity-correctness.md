# Sprint 4 — Cost, Capacity & Execution Correctness

**Goal:** make S1's covariance and S2/S3's mega-book *honest*. Today every net/capacity number the
pipeline reports is dishonest for one of two reasons: (a) a family of P0 correctness bugs in the
cost/capacity/execution path — a participation **unit bug** that inflates capacity by ~price, a
cap-clip-renorm that leaves a net long/short bias on a "neutral" book, limit orders that fill
*through* their limit after costs, delisted names that keep **stale executable volume**, borrow that
is never accrued, and a permanent-impact mark shift that is clobbered by the next bar's close — and
(b) the √-law impact cost is charged in the *NSGA* objective but **never in the ScalarRaw selection
scalar**, and it is OFF by default, so the GA ranks admitted alphas on **gross** returns
(`total_pnl_cost=0.000`; a42 nets 0.37 at 10bps vs 1.93 gross; turnover ~74%/week). S4 is the
**honesty floor**: it fixes the correctness bugs (each with a RED test on a fixture where the right
answer is known by construction) and charges impact in the *selection* objective so admitted alphas
are ranked net-of-cost — then makes the Garleanu-Pedersen partial-trade turnover-native and emits a
first-class book-level capacity curve. This is a big-ticket win, not a tweak: these bugs bias
backtests by 10–1000 bps and the gross-vs-net gap **flips alpha rankings**.

**Owns (exclusive):**
`atx-engine/include/atx/engine/risk/{capacity,optimizer,garleanu_pedersen}.hpp`,
`atx-engine/include/atx/engine/cost/{temp_perm,cost_aware,capacity,calibration,borrow}.hpp`,
`atx-engine/include/atx/engine/loop/{market,weight_policy,backtest_loop}.hpp` (`backtest_loop.hpp`: the borrow-accrual settle-sequence wire only — S4-3c),
`atx-engine/include/atx/engine/exec/execution_sim.hpp`,
`atx-engine/src/factory/fitness.cpp` (impact-in-**SELECTION** objective ONLY — not the admission gate),
`atx-impl/src/stage_report.cpp` (first-class capacity curve emission);
tests under `atx-engine/tests/{risk,cost,loop,exec}/` and `atx-impl/tests/`.

**Must NOT touch:** `alpha/oracle.hpp` (untouchable every sprint); `risk/factor_model.hpp`,
`risk/{stat_factor_model,dead_factor,shrinkage,eigen_adjust,specific_risk,psd_repair,exposures}.hpp`,
`atx-impl/src/stage_optimize.cpp`, `stage_riskmodel.*` (Sprint 1); `fund/*` (Sprint 2);
`learn/*`, `combine/*`, `atx-impl/src/stage_combine.cpp` (Sprint 3); the four hub files
`atx-impl/src/{config.hpp,config.cpp,stage_discover.cpp,stage_run.cpp}` + `library/library.hpp`
+ `factory/factory.cpp` + `factory/fitness.hpp` (Sprint 5).
**NOTE (binding ownership split):** `factory/fitness.cpp` is S4-owned (the impact-in-selection
*objective body*); `factory/fitness.hpp` + `factory/factory.cpp` are **Sprint-5-owned**
(cumulative-N deflation). S4 keeps its edits to the selection-objective body of the `.cpp` and
exposes the new toggle via a field Sprint 5 threads (see S4-0). S4 does **not** re-derive the
admission-gate cost of p6-S4 (`combine/gate.hpp`, `cost_adjusted_fitness`) — that is the *gate*, a
different decision point from the *search selection scalar* S4-4 fixes.

---

## Implementation-quality handoff block (paste verbatim into every subagent brief)

```text
Implementation quality standard:
Use ats-core/include/ats_orderbook.h as the style reference. Prefer clear module-level intent,
grouped constants/types/APIs, explicit ownership and lifecycle rules, named error contracts, and
concise comments that explain invariants, non-obvious control flow, or domain semantics. Do not
follow weaker patterns that expose constants/structs/prototypes without enough API contract.

Prioritize full end-to-end implementation over partial stubs. A unit is not done until the public
API, implementation, tests, docs/ledger row, and build/test gate are complete. Do not leave TODO
placeholders, fake success paths, unused APIs, or untested skeletons.

Comments should be intelligent and sparse: explain why, invariants, ownership, ordering,
crash/recovery semantics, and tricky domain rules. Do not comment obvious assignments or wrap
every field in noise.

Before commit, self-review for:
- Public headers explain purpose, ownership, valid inputs, return codes, and lifecycle.
- Names are domain-accurate and consistent with nearby ATS code.
- Error paths fail closed and clean up owned resources.
- No hidden partial implementation or "will wire later" stubs.
- Tests prove the end-to-end behavior, not only helper functions.
- The implementation follows existing local patterns before inventing new abstractions.
```

---

## Bug-verification pass (the most important part of this sprint — 2026-07-02, current tree)

A prior engine survey flagged these P0/P1 candidates from code-review docs dated **2026-06-14/16**,
which PREDATE p6 (2026-06-27) and p7 (2026-06-28). Each candidate was re-read against the **current**
code before scoping. The result is below; S4 ships work for the **LIVE** rows only. Two candidates
turned out to be *partially* fixed (the framework landed in p6/p7 but the load-bearing path is still
broken), and one turned out to be *already wired* in a different form. **Do not write a unit to fix
an already-fixed bug.** The RED tests below encode the CORRECT behavior on tiny fixtures where the
right answer is known by construction; each fails on the current code.

| # | Candidate (survey) | Verdict | Current file:line evidence |
|---|---|---|---|
| B1 | Capacity participation **unit bug** (shares ÷ dollar-ADV) | **LIVE — 3 sites** | `risk/capacity.hpp:234-240` (`adv=dollar_adv`=Σclose·vol, then `part = shares/adv`); **same bug copied** into `factory/fitness.cpp:518-523` (`part=(aum·|w|/price)/adv`) and re-derived in `atx-impl/src/stage_combine.cpp:289,301` (`part_per_aum=|w|/(price·ADV)`). `dollar_adv` returns **dollars** (`capacity.hpp:165-177`, `sum += c*v`) — confirmed. So participation is `shares/dollar-ADV`, off by a factor of `price`; it understates participation and **inflates capacity**. |
| B2 | Cap-clip-renorm breaks dollar-neutrality (no re-center after clip) | **LIVE — 2 sites** | `risk/optimizer.hpp:389-398` (`project`: `demean_live`→`gross_normalize`→`cap_clip_renorm`, **no demean after**) + `:422-458` (`cap_clip_renorm` ends on clip+deficit-renorm, no re-center). `loop/weight_policy.hpp:319-327` (`truncate_renorm` after demean+gross_normalize, "No gross_normalize follows") + `:480-529` (`truncate_renorm`/`finalize_truncation`, **no demean after**). Asymmetric clipping of a longer side leaves net ≠ 0. |
| B3 | Limit orders fill THROUGH their limit after costs | **LIVE** | `exec/execution_sim.hpp:292` gates on raw `ref` via `limit_marketable(order, ref)` (`:327-337`); `emit_fill` (`:362-389`) computes `fill_px = ref·(1+dir·slip)·(1+dir·temp)` (`:374-375`) with **no re-clamp to the limit**. A buy-limit passing `ref ≤ limit` can fill at `fill_px > limit`. |
| B4 | Absent (delisted) instruments retain STALE executable volume | **LIVE** | `loop/market.hpp:113-123` (`update_prices` touches only slice rows; header `:22-23` documents "absent … keep their prior values" as intended). A name absent from a frontier keeps prior `volumes_[idx]` → `bar_volume()` feeds `volume_capped_qty` (`execution_sim.hpp:342-357`) → phantom fills on a delisted name. |
| B5 | Borrow accrual not in the main loop | **LIVE** | `cost/borrow.hpp:142-145` (`accrue_borrow`) built + tested, but **zero call sites in any `src/`** (verified: only `tests/core/borrow_test.cpp`, `bench/cost_bench.cpp`). `loop/backtest_loop.hpp` settle sequence (`:304-316`) accrues turnover, never borrow. Short book pays no financing → net P&L is overstated. |
| B6 | Permanent impact not persisted across bars | **LIVE** | `exec/execution_sim.hpp:377,425-435` (`apply_permanent_impact` → `market.shift_mark(id, delta)`) shifts `marks_[idx]` in place, but the **next** `update_prices` overwrites `marks_[idx] = r.bar.close` (`loop/market.hpp:120`) for any name that trades next bar → the permanent shift is **clobbered**, so "permanent" impact is effectively temporary. |
| B7 | Impact charged only as TELEMETRY, not in the SELECTION objective | **PARTIALLY LIVE** | The √-law cost **is** computed (`factory/fitness.cpp:477-532` `book_cost_bps`) and pushed into `objectives[4] = -cost_bps` for **NSGA/MultiObjective** ranking (`:453-457`), gated by `target_aum>0`. BUT: (a) **ScalarRaw** selection returns early ranking on `raw` alone (`src/factory/search_driver.cpp:1308-1314`), and `raw = wq·diversify·robust` has **no cost term** (`fitness.cpp:416`); (b) default `target_aum=0.0` (`fitness.hpp:356`) ⇒ cost never computed on the default path. So on the measured run every Sharpe is gross. The `book_cost_bps` participation **also carries B1** (`:523`). |
| B8 | Garleanu-Pedersen partial-trade — wire from scratch? | **ALREADY WIRED (as a linear blend); make it turnover-native** | `risk/garleanu_pedersen.hpp` ships `gp_aim_and_value` (the scalar-Λ aim `aim_pos=(2λV)⁻¹ᾱ`), called only from `src/risk/multi_horizon.cpp:160`. `atx-impl/src/stage_optimize.cpp:159-172` already does a **linear** partial step `w := prev + rate·(w-prev)` behind `--trade-rate` (byte-identical at `rate==1`). It does NOT trade toward the *aim in front of the moving target* — it linearly blends toward the freshly-shaped book. S4's GP work is **turnover-native** (aim-tracking) in the S4-owned header, NOT wiring from scratch, and NOT editing `stage_optimize.cpp` (S1-owned). |
| B9 | First-class capacity curve emitted by `stage_report`? | **NOT EMITTED (scalar footprint only)** | `atx-impl/src/stage_report.cpp:356-531` emits per-name **%ADV participation footprint** scalars at one `report_aum` (`max/p95/p99/median_participation_pct`, `pct_gross_over_5pct_adv`) — no `aum_grid`, no vector of `(AUM, net-edge)` points, no zero-crossing. `p7-S4`'s per-alpha capacity is a **scalar** AUM in `stage_combine.cpp` telemetry (`:871-884`), also under B1. The book-level edge-vs-AUM curve `risk::capacity_curve` / `cost::capacity_point` build exists (`cost/capacity.hpp`) but nothing emits it into the report. |

**Verified-already-fixed / out-of-scope (documented so no unit is wasted):**
- **p6-S4 admission-gate cost** (`combine/gate.hpp`, `cost::cost_adjusted_fitness`, `min_holding_days`,
  `rt_cost_bps` gate ceiling) is **shipped and merged** (`Sprint 4 Cost-Aware Admission Gates`,
  memory obs 15708). That is the *gate* — a threshold applied to a candidate the search already
  picked. S4 must NOT re-implement it; S4-4 fixes the **search selection scalar**, a strictly
  earlier and different decision point. The gate and the selection scalar are complementary.
- The √-law cost **model** (`cost/cost_aware.hpp:118-127` `round_trip_cost_bps` = `2·(temp+slip)+perm`)
  is the ONE cost model and is correct; S4 does not add a second impact formula (C6 invariant). S4
  fixes its *inputs* (participation, B1) and its *reach* (ScalarRaw + default-on-profile, B7).

---

## Architecture note — what "the honesty floor" actually means

There are exactly **two** ways the reported numbers lie today, and S4 closes both:

1. **The cost/capacity/execution *simulation* is wrong** (B1–B6). These are not knobs — they are bugs
   in code that already runs on every backtest. A dollar-neutral book that clips to a net long bias
   (B2) books a spurious market-beta return; a limit order that fills above its limit (B3) books a
   better price than any real fill; a delisted name with stale volume (B4) books fills that could
   never happen; an un-accrued borrow (B5) omits a real cost; a clobbered permanent impact (B6) lets
   a large order's footprint evaporate for free; and the participation unit bug (B1) makes every
   capacity number ~`price`× too large. Each biases the scorecard by 10–1000 bps.

2. **The *selection* is blind to cost** (B7). Even with a perfect simulation, if the GA ranks on
   gross returns then it admits the highest-*gross* alphas — which are disproportionately the
   high-turnover ones that die net-of-cost. Charging the √-law temp+perm impact in the **selection
   scalar** (not just the NSGA vector, and not just the post-hoc gate) flips a high-turnover in-sample
   winner *below* a low-turnover one. This is the ranking-flip that makes admitted alphas tradeable.

The correctness fixes (B1–B6) **change the number because the old number was wrong**; they are the
one documented exception to the byte-identity contract (see the determinism section). The
selection/GP/capacity-curve work (B7–B9) follows the **opt-in inert-default** contract: OFF ⇒
byte-identical.

---

## Determinism / correctness contract (Sprint 4)

S4 carries **two** contracts because it mixes opt-in features with correctness fixes (ROADMAP
§"Shared determinism contract" — the P0-bug exception is called out there explicitly).

**(A) Opt-in / default-byte-identical** (B7 impact-in-selection, B8 GP turnover, B9 capacity curve):
each new capability is gated behind a new config field with an inert default, so the pinned goldens
(`NsgaSearch.ScalarRaw_ReproducesGoldenDigest`, `FactoryOos.MineIntoOffPathDigestUnchanged`, the
optimize/report OOS goldens) stay UNCHANGED on the no-flag path. `oracle.hpp` is untouched. Each
opt-in ships four test classes — (a) off-path byte-identity, (b) on-path RED→GREEN, (c) twice-run,
(d) seq==parallel where an admission path is touched. S5's build profile turns the opt-ins on as an
explicit non-default profile, never a golden re-baseline.

**(B) Correctness fix — the documented exception** (B1–B6): these change the number *because the old
number was wrong*. Each such unit ships:
- **(a) a RED test** encoding the correct behavior on a tiny deterministic fixture where the right
  answer is known by construction — it FAILS on the buggy code and PASSES on the fix;
- **(b) a documented before/after** on that fixture (the wrong number, the right number, why);
- **(c) a ledger note** that any golden which ENCODED the bug is re-baselined **with the bug-fix
  commit SHA as the authority** — NO silent golden drift. If an optimize/report golden shifts, the
  ledger row names the golden, the old digest, the new digest, and the fixture proving the new one is
  correct.

Determinism of the fixes themselves: every touched reduction stays order-fixed (ascending
row/instrument), NO RNG, NO clock, NO hash-order iteration — the fixes are pure arithmetic/logic
changes, reproducible run-to-run and seq==parallel.

---

## Dependency / wiring map

```
NEW CostSelectionConfig (S4-0)         ← the inert-default toggle every S4-4 test reads; a field
    (impact_in_selection=false, charge=0) Sprint 5 threads into FitnessCfg + the CLI hub
risk/capacity.hpp:238-240              ← S4-1 fix participation: shares/dollar-ADV → notional/dollar-ADV
factory/fitness.cpp:518-523            ← S4-1 fix the COPY of the same bug (S4-owned .cpp)
  └─ SEAM: stage_combine.cpp:289,301   ← S3-owned COPY of the same bug — S4 documents; S3 fixes (S4-3 seam note)
risk/optimizer.hpp:389-458             ← S4-2 re-center (demean_live) after cap_clip_renorm
loop/weight_policy.hpp:480-529         ← S4-2 re-center after truncate_renorm/finalize_truncation
exec/execution_sim.hpp:362-389         ← S4-3a clamp fill_px to the limit after slippage+temp
loop/market.hpp:113-123                ← S4-3b zero absent instruments' executable volume each slice
loop/backtest_loop.hpp: settle seq     ← S4-3c accrue borrow in the loop (S4-owned? NO — see S4-3c note)
exec/execution_sim.hpp + loop/market.hpp ← S4-3d persist the permanent mark shift across update_prices
factory/fitness.cpp: finish_report/raw ← S4-4 charge -cost in the ScalarRaw `raw`, gated by CostSelectionConfig
risk/garleanu_pedersen.hpp             ← S4-5a turnover-native aim (aim-in-front-of-target)
atx-impl/src/stage_report.cpp          ← S4-5b emit the book-level capacity curve (aum_grid + zero-crossing)
tests/risk/capacity_participation_test.cpp   ← S4-1
tests/risk/optimizer_recenter_test.cpp       ← S4-2
tests/loop/weight_policy_recenter_test.cpp   ← S4-2
tests/exec/limit_fill_clamp_test.cpp         ← S4-3a
tests/loop/market_absent_volume_test.cpp     ← S4-3b
tests/loop/backtest_borrow_test.cpp          ← S4-3c
tests/exec/permanent_impact_persist_test.cpp ← S4-3d
tests/factory/fitness_cost_selection_test.cpp← S4-4
tests/risk/gp_turnover_test.cpp              ← S4-5a
atx-impl/tests/stage_report_capacity_curve_test.cpp ← S4-5b
```

> **`backtest_loop.hpp` ownership caveat (S4-3c):** `loop/backtest_loop.hpp` is NOT in S4's owned set
> (S4 owns `loop/{market,weight_policy}.hpp` and `exec/execution_sim.hpp`, plus the five `cost/*`
> headers). Accruing borrow requires a call in the loop's settle sequence. **Resolution:** S4 ships
> the borrow-accrual as an *opt-in* driven from the ExecutionSimulator/Market seam it owns, or (if a
> loop edit is unavoidable) coordinates the single settle-sequence line with the loop owner and
> records it as a cross-sprint seam in the ledger — the borrow **model** and its determinism are
> S4-owned; the one loop call site is a documented seam. Confirm the exact owner of
> `backtest_loop.hpp` at kickoff (recon shows it is not claimed by S1/S2/S3/S5); if unclaimed, S4
> takes the single additive settle-sequence line under the correctness-fix contract.

---

## Tasks

### S4-0 — Open ledger + bug-verification pass + `CostSelectionConfig` plumbing (do first)

**Goal:** create the sprint ledger (marker commit); freeze the LIVE/FIXED verdict table above into
the ledger with the current file:line evidence (so no downstream unit re-litigates a candidate); and
define `CostSelectionConfig` — the inert-default toggle S4-4 reads and Sprint 5 threads. No behavior
change: the field exists, nothing reads it non-inertly yet.

**Wiring:**
- Write the ledger `sprint-4-progress.md` with the LIVE/FIXED table (the verification pass is a
  *deliverable*, not a preamble — future agents must be able to see which candidate was dropped and
  why without re-reading the code).
- Add `CostSelectionConfig` to the S4-owned engine cost config surface (prefer an existing home near
  `cost/cost_aware.hpp` or a small new `cost/cost_selection_config.hpp` if no home fits). Fields,
  all inert-default:
  ```cpp
  // Charge the √-law temp+perm impact in the SEARCH SELECTION scalar (ScalarRaw `raw`),
  // not only the NSGA objective vector. Inert default ⇒ selection is byte-identical.
  struct CostSelectionConfig {
    bool     impact_in_selection = false;  // inert ⇒ raw unchanged (gross selection, today)
    atx::f64 selection_aum       = 0.0;    // AUM at which the selection cost is priced; 0 ⇒ off
    atx::f64 cost_weight         = 1.0;    // multiplier on the net-of-cost penalty when on
  };
  ```
- `CostSelectionConfig` is a *field Sprint 5 threads* into `FitnessCfg` — S4 does NOT edit
  `fitness.hpp` (S5-owned). S4 exposes the struct and consumes it in `fitness.cpp` via the field S5
  adds; for S4's own on-path tests, construct the config directly and call the objective body.
  Document this exact seam in the ledger (what field, where S5 adds it).

**Determinism:** pure addition. Append fields at struct end (no aggregate-init breakage). No golden
reads the new field yet ⇒ all existing suites green, digests unchanged.

**Accept:**
- Project compiles (debug + release); all existing `risk_*`, `cost_*`, `loop_*`, `exec_*`,
  `factory_*`, `stage_report_*` suites green.
- `cost_selection_config_defaults` (new `tests/cost/`): default-constructs to
  `{false, 0.0, 1.0}`; a `static_assert`/test pins the inert values.
- Ledger row present with the full LIVE/FIXED verification table.

---

### S4-1 — Fix the capacity participation UNIT bug [CORRECTNESS — B1]

**Goal:** correct the participation dimension everywhere S4 owns it: participation must be
**dollars ÷ dollar-ADV** (or equivalently shares ÷ share-ADV), not **shares ÷ dollar-ADV**. The bug
divides a share count `notional/price` by a *dollar*-ADV, so participation is off by a factor of
`price` (a $100 stock understates participation 100×), which understates √-impact cost and
**inflates capacity** by ~`price^δ`.

**Root cause (verified):** the participation is computed as `shares/dollar_adv` in three places, all
copied from the original `risk::capacity.hpp`:
- `risk/capacity.hpp:238-240`: `notional = aum·|w|; shares = notional/price; part = shares/adv`
  where `adv = dollar_adv(...)` returns `mean(close·volume)` = **dollars** (`:165-177`).
- `factory/fitness.cpp:518-523`: `part = (target_aum·|w|/price) / adv`, `adv = dm_dollar_adv` = dollars.
- `atx-impl/src/stage_combine.cpp:289,301` (S3-owned): `part_per_aum = |w|/(price·ADV)` — same shape.

The dimensionally-correct participation is `notional/dollar_adv = (aum·|w|)/adv` (drop the `/price`),
which is unitless (dollars/dollars) — exactly what the √-impact law `Y·σ·part^δ` expects, and what
the `execution_sim` charges (`part = fillable_shares/st.adv` where `st.adv` is a **share** ADV, so
the sim itself is consistent; only the *research-cadence* re-derivations mis-divide).

**Fix:**
- `risk/capacity.hpp:238-240`: replace `shares = notional/price; part = shares/adv` with
  `part = notional/adv` (drop the price division). Update the header contract comment (`:54-60`,
  `:214-217`) so the documented formula matches (`part_i = notional_i / dollar_ADV_i`).
- `factory/fitness.cpp:518-523`: replace `part = (target_aum·|w|/price)/adv` with
  `part = (target_aum·|w|)/adv`. Keep the guards (dead/NaN weight, non-positive price still gates the
  *name* out because an unpriced name has no book value, but the price no longer enters the ratio).
- **SEAM (S4-3 ledger note, do NOT edit here):** `stage_combine.cpp:289,301` carries the identical
  bug and is **Sprint-3-owned**. S4-1 records the exact fix (drop `price` from
  `part_per_aum = |w|/(price·ADV)` → `|w|/ADV`) as a cross-sprint seam for Sprint 3, with the fixture
  below as the shared proof. S4 must not edit `stage_combine.cpp`.

**Determinism (correctness fix — contract B):** the fix changes capacity/cost numbers because the old
ones were wrong. Any capacity or cost golden that encoded the inflated number is re-baselined with
this unit's commit SHA as the authority; the ledger names the golden + old/new digest.

**Accept (RED→GREEN on a fixture where the right answer is known by construction):**
- `capacity_participation_dimension` (new `tests/risk/`): a hand-built single-name book — `aum = $1e6`,
  `|w| = 1.0`, `price = $100`, dollar-ADV = `$1e6` (i.e. 10,000 shares/day). Correct participation =
  `notional/adv = 1e6/1e6 = 1.0` (the book trades 100% of a day's dollar volume). The buggy code
  yields `shares/adv = (1e6/100)/1e6 = 1e-4` — off by exactly `1/price`. The RED test asserts
  `part == 1.0` (fails at `1e-4` on the buggy code, passes on the fix). A second name at `price=$10`
  with the same notional proves the error scales with price (buggy `part` differs between the two
  identical-notional names; fixed `part` is identical).
- `capacity_curve_price_invariance`: two books with identical *notional* participation but different
  price levels produce the **same** √-impact cost after the fix (they differ before).
- `fitness_book_cost_price_invariant`: `book_cost_bps` for two identical-notional/different-price
  books is equal after the fix.
- Documented before/after in the ledger: for the fixture, old capacity AUM vs new capacity AUM (the
  new one is ~`price^(1/δ)`× smaller — the honest, lower capacity).

---

### S4-2 — Cap-clip-renorm must not break dollar-neutrality (re-center after clip) [CORRECTNESS — B2]

**Goal:** a dollar-neutral book must stay net ≈ 0 **after** the cap clip + renorm. Today both
projection paths demean *before* clipping and never re-center *after*, so asymmetric clipping (the
long side clipped harder than the short, or vice-versa) leaves a residual net long/short bias on a
book the caller declared neutral — a spurious market-beta exposure that books a fake return.

**Root cause (verified):**
- `risk/optimizer.hpp:389-398` `project()`: `demean_live(v)` → `gross_normalize(v)` →
  `cap_clip_renorm(v, cap)`. The clip-renorm (`:422-458`) clamps `|v_i| ≤ cap` and renorms L1 to `L`
  but performs **no demean**; its final deficit-renorm scales only sub-cap names, which can be
  asymmetric across sign → `Σv ≠ 0`.
- `loop/weight_policy.hpp:319-327`: after `demean` + `gross_normalize`, `truncate_renorm(dense)` runs
  and the comment explicitly says "No gross_normalize follows"; `truncate_renorm`/`finalize_truncation`
  (`:480-529`) clip+renorm with **no demean**. Same residual-bias failure.

**Fix:**
- After the clip-renorm settles the binding set and hits the gross budget, apply a **final
  dollar-neutral re-center of only the LIVE cells** that preserves the cap and the gross budget to the
  documented tolerance. The correct order is: clip-renorm to settle bindings → demean the live cells
  → (a tiny gross-renorm of the sub-cap names to restore `Σ|w| = L`). Because the cap normally does
  not bind symmetrically, the re-center is a small, bounded correction; guard the degenerate case
  where re-centering would push a name back over the cap (iterate the settle a fixed extra pass, as
  the existing `kCapIters`/`kTruncateIters` do — NO convergence early-exit, determinism §3.2).
- Implement in `risk/optimizer.hpp` (`project`/`cap_clip_renorm`) and mirror in
  `loop/weight_policy.hpp` (`truncate_renorm`/`finalize_truncation`). Gate behind `dollar_neutral`
  (optimizer) / the existing dollar-neutral flag (weight policy) so a book that WANTS net exposure is
  untouched.

**Determinism (correctness fix — contract B):** the fix changes the clipped book only when the cap
binds asymmetrically on a neutral book. Any optimize golden encoding the biased book is re-baselined
with this unit's SHA; the ledger names it. Order-fixed reductions throughout.

**Accept (RED→GREEN, answer known by construction):**
- `optimizer_neutral_after_clip` (new `tests/risk/`): a 4-name dollar-neutral book with a cap that
  binds the two longs harder than the two shorts (e.g. raw targets `[+0.6, +0.4, −0.5, −0.5]`,
  `cap = 0.30`, `L = 1.0`). The prior bug returned **net ≈ 0.234** (the documented failure number);
  the RED test asserts `|Σw| ≤ 1e-9` after `project()` — fails at `0.234`, passes on the fix — while
  `max|w_i| ≤ cap` and `Σ|w_i| = L` still hold to tolerance.
- `weight_policy_neutral_after_truncate` (new `tests/loop/`): the same fixture through
  `WeightPolicy::to_target_weights` with `dollar_neutral=true` + a binding `truncation`; asserts
  `|Σw| ≤ 1e-9`, `max|w| ≤ truncation`, `Σ|w| = gross_leverage`.
- `clip_renorm_non_neutral_unchanged`: with `dollar_neutral=false` the returned book is byte-identical
  to the pre-fix code (the re-center is a no-op when neutrality is not requested).
- Documented before/after: net exposure `0.234 → 0.000` on the fixture; cap + gross invariants
  preserved.

---

### S4-3 — Execution correctness: limit clamp, absent volume, borrow, permanent impact [CORRECTNESS — B3–B6]

**Goal:** four independent execution-realism fixes, each with its own RED test. Bundled because they
are all small, all in S4-owned exec/loop headers, and each is a "the sim let something impossible
happen" correction.

#### S4-3a — Limit orders must never fill THROUGH their limit after costs [B3]

**Root cause:** `exec/execution_sim.hpp:292` admits a fill when `limit_marketable(order, raw_ref)`
(`:327-337`, buy iff `ref ≤ limit`), but `emit_fill` (`:362-389`) then applies slippage+temp impact
(`fill_px = ref·(1+dir·slip)·(1+dir·temp)`, `:374-375`) with no re-clamp — a buy-limit that passed
at `ref ≤ limit` can execute at `fill_px > limit`.

**Fix:** in `emit_fill`, after computing `fill_px`, clamp it to the limit for a Limit order: a buy
fills at `min(fill_px, limit)`, a sell at `max(fill_px, limit)`. (Market orders unchanged.) This is
the marketable-limit contract: you never pay worse than your limit — if costs would push the fill
through it, the fill price is the limit (a real venue would fill at the limit or not at all; the
conservative research convention is fill-at-limit). Document that partial marketability (fill less
than full size when the limit binds) is the deferred LOB residual — S4 only guarantees the *price*
never crosses the limit.

**Accept:** `limit_fill_never_through` (new `tests/exec/`): a buy-limit at `limit = 100.0`,
`ref = 100.0`, slip+temp = +0.5% → buggy `fill_px = 100.5 > limit`; RED asserts `fill_px ≤ limit`
(fails at 100.5, passes at 100.0). A sell-limit mirror asserts `fill_px ≥ limit`. A Market order is
byte-identical (no clamp path). A limit with `ref` strictly inside the limit and small costs that
keep `fill_px` on the good side is unchanged (clamp inert).

#### S4-3b — Absent instruments must not provide phantom liquidity [B4]

**Root cause:** `loop/market.hpp:113-123` `update_prices` refreshes only slice-present rows; an
instrument absent from the frontier keeps prior `volumes_[idx]` (and `marks_[idx]`), so
`bar_volume()` (`:131-133`) feeds `volume_capped_qty` (`execution_sim.hpp:342-357`) a stale
non-zero volume → a delisted name can still be "filled."

**Fix:** on each `update_prices`, zero the executable volume of any universe member NOT present in the
slice (the mark may legitimately persist as a last-known reference for MTM, but *executable volume*
must not — an absent name did not trade, so its per-bar fillable budget is 0). Track slice presence
with a per-call scratch mask (universe-indexed, cleared each call, no allocation on the steady-state
path — mirror the existing dense-store discipline). Keep marks as-is (MTM of an open position in a
halted name is a separate concern; this fix is scoped to *executable volume* only).

**Accept:** `market_absent_zeroes_volume` (new `tests/loop/`): a 2-name universe; slice 1 sets both
volumes; slice 2 carries only name A. RED asserts `bar_volume(B) == 0.0` after slice 2 (fails —
stale volume persists — passes on the fix). A follow-on assert: an order on B settles to **zero
fillable** after slice 2 (`volume_capped_qty(B) == 0`). Name A's volume is refreshed normally;
`mark(B)` is unchanged (MTM preserved — the fix is volume-scoped).

#### S4-3c — Borrow must be accrued in the loop sequence [B5]

**Root cause:** `cost/borrow.hpp:142-145` `accrue_borrow` is built + tested but has **zero call sites
in `src/`** (verified); the loop's settle sequence (`backtest_loop.hpp:304-316`) never charges short
financing.

**Fix:** accrue the daily borrow charge once per bar in the settle sequence, opt-in behind a
`BorrowModel` with an inert default (`annual_rate = 0.0` ⇒ zero charge ⇒ byte-identical). Wire via the
S4-owned exec/market seam per the ownership caveat above; if the single settle-sequence call must
land in `backtest_loop.hpp`, coordinate with its owner and record the seam. The charge rides
`Portfolio::accrue_financing` (a pure cash debit, NOT a synthetic fill — `apply_fill` asserts
`qty != 0`).

**Accept:** `backtest_borrow_accrued` (new `tests/loop/`): a 1-day hold of a short position with
`annual_rate = 0.05`, `day_count = D360`; the RED asserts ending cash is debited by exactly
`short_notional · 0.05 / 360` (fails at 0 debit — borrow never accrued — passes on the fix). A
long-only book pays exactly 0. `annual_rate = 0.0` ⇒ ending cash byte-identical to the pre-fix run
(inert default).

#### S4-3d — Permanent impact must persist across bars [B6]

**Root cause:** `apply_permanent_impact` (`execution_sim.hpp:377,425-435`) shifts `marks_[idx]` via
`shift_mark`, but the next `update_prices` overwrites `marks_[idx] = r.bar.close`
(`loop/market.hpp:120`) for any name that trades next bar → the "permanent" shift lives only until
the next close, i.e. it is effectively temporary.

**Fix:** persist the accumulated permanent shift as a separate additive offset the Market carries and
re-applies on top of each incoming close (`mark = incoming_close + cumulative_perm_offset`), so a
large order's footprint survives the next bar's price update. The offset is per-instrument, additive,
and accumulates across fills; `update_prices` adds the offset to the fresh close rather than
discarding it. (Physical justification: permanent impact is a *lasting* repricing — the venue's next
print already embeds it in the real world; in the sim we must carry it because our close feed is the
unimpacted historical close.) Keep the offset bounded/documented; a zero offset (no fills) ⇒
byte-identical marks.

**Accept:** `permanent_impact_persists` (new `tests/exec/`): drive one large buy that applies a
known permanent shift `+δ`; on the NEXT bar's `update_prices` (with a fresh close equal to the
pre-impact price), assert `mark == close + δ` (fails — the offset is discarded, `mark == close` —
passes on the fix). A no-fill bar leaves the offset unchanged (temp does not re-accrue — reuse the
`temp_perm.hpp` round-trip harness's no-trade-bar assertion). A run with zero fills is byte-identical
(offset always 0).

**Determinism (all four — contract B):** each fix changes the number because the old one was
impossible/wrong. Goldens encoding a through-limit fill, a phantom fill, a borrow-free short book, or
an evaporating footprint are re-baselined with this unit's SHA; the ledger names each. All reductions
stay order-fixed; the absent-volume mask and the perm-offset are deterministic per-instrument state.

---

### S4-4 — Charge √-law impact in the SELECTION objective (ScalarRaw), behind `CostSelectionConfig` [B7]

**Goal:** make the search *select* net-of-cost. Today the √-law cost enters only the **NSGA objective
vector** (`objectives[4] = -cost_bps`, gated by `target_aum>0`); the **ScalarRaw** path ranks on
`raw = wq·diversify·robust` alone (no cost term), and the default `target_aum=0` means cost is not
even computed on the default run. Charge the net-of-cost penalty into the ScalarRaw `raw` behind an
**inert** `CostSelectionConfig` so a high-turnover in-sample winner ranks BELOW a low-turnover one on
a net basis — flipping the GA's preference toward tradeable alphas. Inert default (`charge=0`) ⇒
selection byte-identical.

**Root cause (verified):**
- `raw` has no cost term: `fitness.cpp:416` `raw = core.wq * diversify * core.robust`.
- ScalarRaw returns before touching `objectives`: `src/factory/search_driver.cpp:1308-1314` ranks by
  `raw_ordered_indices`; the tournament uses `.selection` (== raw), not `objectives`
  (`:1406-1408`).
- `book_cost_bps` (`fitness.cpp:477-532`) computes the correct-form √-law round-trip cost (after
  S4-1 fixes its participation) but its only consumer is `objectives[4]` (NSGA).

**Fix (S4-owned `fitness.cpp` selection-objective body ONLY):**
- In `finish_report`, when `CostSelectionConfig::impact_in_selection` is true, subtract a net-of-cost
  penalty from `raw` in raw-fitness units, using the SAME `book_cost_bps` figure already computed
  (post-S4-1, dimensionally correct) at `selection_aum`, scaled by `cost_weight`. Reuse the existing
  `combine::cost_adjusted_fitness` / `kFitnessCostScale` convention (the ONE fitness-cost scale) so
  the selection penalty and the p6-S4 *gate* penalty share one formula — S4 does NOT invent a second
  scale. The penalty enters `raw` (so ScalarRaw sees it) AND leaves the existing `objectives[4]`
  intact (so NSGA is unchanged); the two are consistent.
- Inert default: `impact_in_selection=false` (and/or `selection_aum=0`) ⇒ the branch is never
  entered ⇒ `raw` byte-identical ⇒ every ScalarRaw/NSGA golden unchanged.
- **Ownership:** the toggle lives in `CostSelectionConfig` (S4-0). `FitnessCfg` gains the field in
  **Sprint 5**; S4 consumes it via that field and its own on-path tests construct the config
  directly. Do NOT edit `fitness.hpp` or `factory.cpp` (S5-owned).

**Determinism (opt-in — contract A):** OFF ⇒ byte-identical (goldens unchanged). ON is deterministic
(pure function of genome/panel/cfg; NO RNG). Ships all four opt-in test classes.

**Accept (on-path RED→GREEN, ranking-flip known by construction):**
- `selection_flips_high_turnover` (new `tests/factory/`): two hand-built candidates — A: higher
  **gross** `raw` but high turnover (heavy √-impact at `selection_aum`); B: slightly lower gross but
  low turnover. With `impact_in_selection=false`, ScalarRaw ranks **A above B** (gross). With
  `impact_in_selection=true` at a `selection_aum` where A's net penalty exceeds the gross gap, the
  ScalarRaw rank **flips to B above A**. The RED test asserts the flip (fails when cost is not in
  `raw`, passes after the wire). Numbers chosen so the flip is unambiguous (e.g. gross gap 0.05,
  A's net penalty 0.20, B's 0.02).
- `selection_cost_off_byte_identical`: `impact_in_selection=false` ⇒ `raw` and the ScalarRaw ordering
  are byte-identical to the pre-S4-4 pinned digest (off-path).
- `selection_uses_fixed_participation`: the selection penalty uses the S4-1-corrected participation
  (a price-invariant net penalty for two identical-notional/different-price books) — proves S4-1 and
  S4-4 compose.
- Twice-run + seq==parallel on the on-path selection (the search selection touches the admission
  path).
- Documented before/after: the measured gross-vs-net gap that flips the ranking (tie to the a42
  0.37-net-vs-1.93-gross observation as the motivating real case; the unit proves the mechanism on a
  fixture).

---

### S4-5 — Garleanu-Pedersen turnover-native partial-trade + first-class capacity curve [B8, B9]

**Goal:** two related tradeable-alpha finishers. (a) Make the GP partial-step **turnover-native**:
trade partway toward the *aim in front of the moving target* (Garleanu-Pedersen 2013 — "aim in front
of the target; trade partially toward the aim") so turnover is reduced while still tracking the
moving alpha, rather than the current *linear* blend toward the freshly-shaped book. (b) Emit a
**first-class book-level capacity curve** (edge-vs-AUM zero-crossing under √-impact, monotone) from
`stage_report`, replacing the scalar %ADV footprint as the honest capacity statement.

**Root cause (verified):**
- **B8:** `stage_optimize.cpp:159-172` (S1-owned) does `w := prev + rate·(w-prev)` — a linear blend
  toward the *target*, NOT toward the GP *aim*. The real GP aim `aim_pos=(2λV)⁻¹ᾱ`
  (`garleanu_pedersen.hpp:118-145`) is only used in `multi_horizon.cpp:160`. The aim-in-front
  behavior — where the aim leads the target because the signal is mean-reverting/decaying — is not
  expressed as a turnover-native step in the S4-owned header.
- **B9:** `stage_report.cpp:356-531` emits per-name %ADV participation scalars at one `report_aum`; no
  `aum_grid`, no `(AUM, net-edge)` vector, no zero-crossing. The build blocks exist unused:
  `risk::capacity_curve` (`risk/capacity.hpp:264-288`, corrected by S4-1) + `cost::capacity_point`
  (`cost/capacity.hpp:63-91`, the interpolated zero-crossing).

**Fix:**
- **S4-5a (`risk/garleanu_pedersen.hpp`, S4-owned):** add a turnover-native step that, given the aim
  `aim_pos`, the prior book, and a target turnover budget, trades a partial fraction toward the
  **aim** (not the target) — `w := prev + κ·(aim_pos − prev)` with `κ` derived from the GP scalar
  trade-rate so that the realized turnover `Σ|w−prev|` respects the budget while tracking the aim.
  Keep the scalar-Λ reduction (no Riccati — the recorded lift). Byte-identical at full trade-rate
  (aim == target ⇒ the step reduces to today's book). This makes the header the turnover-native
  producer; the actual stage_optimize wire is an S1/S5 seam (do NOT edit `stage_optimize.cpp`) —
  record it in the ledger.
- **S4-5b (`atx-impl/src/stage_report.cpp`, S4-owned):** compute the book-level capacity curve over
  an `aum_grid` via `cost::capacity_for_book` (→ `risk::capacity_curve`, now S4-1-correct) and emit
  (i) the full `(AUM, net_edge_bps)` grid, (ii) the interpolated zero-crossing `cost::capacity_point`,
  as additive report kvs. Additive/opt-in (present only when an `aum_grid`/`--capacity-curve` config
  is supplied; absent otherwise ⇒ report byte-identical). The curve is monotone non-increasing (the
  `capacity_point` C4 guard asserts it) and, post-S4-1, the zero-crossing is the honest (lower)
  capacity, not the price-inflated one.

**Determinism (opt-in — contract A):** GP full-rate ⇒ byte-identical; capacity-curve absent ⇒ report
byte-identical. Both are pure/order-fixed (the GP apply is the cached-Cholesky Woodbury; the capacity
sweep is a cold order-fixed scan). ON is reproducible run-to-run and seq==parallel.

**Accept:**
- `gp_turnover_reduces` (new `tests/risk/`): with a moving alpha target across two periods and a
  mean-reverting signal, the turnover-native GP step produces **strictly lower** `Σ|w−prev|` than the
  linear blend at equal tracking error to the aim — and at full trade-rate the step is byte-identical
  to the single-period Markowitz target (the boundary pin). The RED test asserts the turnover
  reduction and the full-rate byte-identity.
- `gp_full_rate_byte_identical`: `trade_rate == 1` ⇒ aim step == target step (inert boundary).
- `stage_report_capacity_curve_monotone` (new `atx-impl/tests/`): on a fixture book, the emitted curve
  is non-increasing in AUM, `net_edge(AUM→0) ≈ gross` (impact→0), and the zero-crossing is finite for
  a positive-gross book; a larger sim `ImpactCfg.Y` shifts the crossing to a **lower** AUM (proves the
  one-cost-surface read). With S4-1's fix, the crossing AUM is ~`price^(1/δ)`× lower than the buggy
  value — assert against the by-construction correct number.
- `capacity_curve_absent_byte_identical`: no `aum_grid` supplied ⇒ the report is byte-identical to
  the pre-S4-5 pinned golden.

---

## Sequencing

1. **S4-0 first** (ledger + verification table + `CostSelectionConfig`) — every unit reads the
   verdict table; S4-4 reads the config.
2. **S4-1** (participation unit fix) — it is the ROOT of the capacity/cost error and S4-4/S4-5 both
   depend on the corrected participation. Land it before S4-4 (which uses `book_cost_bps`) and S4-5b
   (which emits the corrected curve).
3. **S4-2** and **S4-3** in parallel after S4-0 (disjoint files: S4-2 = optimizer/weight_policy;
   S4-3 = execution_sim/market/loop). Each is an independent correctness fix.
4. **S4-4** after S4-1 (the selection penalty consumes the corrected `book_cost_bps`).
5. **S4-5** last (S4-5a in the GP header; S4-5b consumes the S4-1-corrected `capacity_curve`).

---

## Risks / guardrails

| Risk | Impact | Guardrail |
|---|---|---|
| A correctness fix silently drifts a golden without a documented re-baseline | Contract violation; the next agent can't tell a fix from a regression | Every B1–B6 unit ships (a) a RED test, (b) a by-construction before/after, (c) a ledger row naming the golden + old/new digest + the fixture proving the new number. NO silent drift. |
| The participation fix (S4-1) is applied in `risk/capacity.hpp`/`fitness.cpp` but the S3-owned `stage_combine.cpp:301` copy is left buggy | Capacity numbers still inconsistent between report and combine | S4-1 records the exact `stage_combine.cpp` fix as a cross-sprint seam for Sprint 3 with the shared fixture; the ledger row flags it as an open dependency until S3 lands it. |
| S4-4 edits leak into `fitness.hpp`/`factory.cpp` (S5-owned) | Ownership collision with Sprint 5's cumulative-N work | S4-4 confines edits to the `fitness.cpp` selection-objective body; the toggle lives in `CostSelectionConfig` (S4-owned) and S5 threads the `FitnessCfg` field. The ledger names the exact field/line S5 adds. |
| Borrow accrual (S4-3c) needs a line in `backtest_loop.hpp` (not clearly S4-owned) | Ownership ambiguity on the one call site | Confirm the owner of `backtest_loop.hpp` at kickoff; if unclaimed, S4 takes the single additive settle-sequence line under contract B and records the seam; the borrow model + determinism are S4-owned regardless. |
| The re-center (S4-2) pushes a name back over the cap | Cap invariant breaks | Re-center is a bounded correction inside the fixed clip-renorm loop (extra settle pass, NO early-exit); the test asserts `max|w| ≤ cap` AND `|Σw| ≤ 1e-9` AND `Σ|w| = L` simultaneously. |
| The permanent-impact offset (S4-3d) double-counts or grows unboundedly | Marks diverge from reality | The offset is a single per-instrument accumulator added to the fresh close; the no-fill-bar test asserts it does not re-accrue; a bounded/documented magnitude; zero-fill run byte-identical. |
| GP turnover step (S4-5a) breaks the boundary pin at full trade-rate | Golden drift on the default path | The `gp_full_rate_byte_identical` test pins aim-step == target-step at `rate==1`; the scalar-Λ reduction guarantees it (aim == single-period Markowitz target). |

---

## Bench / acceptance (sprint close)

- **Default byte-identity (opt-in units B7–B9):** the pinned optimize/report/NSGA goldens
  (`NsgaSearch.ScalarRaw_ReproducesGoldenDigest`, `FactoryOos.MineIntoOffPathDigestUnchanged`,
  `stage_report` golden) unchanged with `CostSelectionConfig` off, no `aum_grid`, GP full-rate.
- **Correctness re-baselines (B1–B6):** each documented in the ledger with the fixture, the old and
  new digest, and the bug-fix commit SHA as the re-baseline authority. NO silent drift.
- **Per-task RED→GREEN:** every unit has a test that is RED on the current code and GREEN after — the
  tests ARE the sprint (this is a correctness sprint).
- **The three headline claims, measured on fixtures where the answer is known by construction:**
  (1) participation dimension `1e-4 → 1.0` on the S4-1 fixture (capacity de-inflated);
  (2) dollar-neutral net `0.234 → 0.000` on the S4-2 fixture;
  (3) ScalarRaw ranking flips A↔B when impact enters selection (S4-4).
- **Twice-run + seq==parallel** on the S4-4 selection path and the S4-5 capacity sweep.
- **Dev-panel smoke ≤5 min** with the S4 opt-ins on (impact-in-selection + capacity curve): the
  book's reported Sharpe drops from gross to net and the capacity curve is monotone with a finite
  zero-crossing. (The CLI flags are threaded in Sprint 5; S4 proves the engine path via direct-call
  integration tests, not the CLI.)

---

## Out of scope

- CLI flags (`--impact-in-selection`, `--selection-aum`, `--capacity-curve`, `--borrow-rate`,
  `--trade-rate` turnover-native) — Sprint 5 (hub).
- The p6-S4 **admission-gate** cost (`combine/gate.hpp`, `cost_adjusted_fitness`, `min_holding_days`)
  — already shipped; S4 fixes the *selection scalar*, a different decision point.
- Editing `stage_combine.cpp` to fix its participation copy (B1 third site) — Sprint 3 owns the file;
  S4-1 ships the seam note + shared fixture.
- Editing `stage_optimize.cpp` to wire the turnover-native GP step — Sprint 1 owns the file; S4-5a
  ships the header producer + seam note.
- The full matrix-Riccati GP value function (`A_xx ≠ 2λV` for H>1) — the recorded GP lift; S4 keeps
  the scalar-Λ reduction.
- Full limit-order-book / partial-marketability realism — deferred exec residual; S4-3a guarantees
  only that the fill *price* never crosses the limit.
- Survivorship/delisted security-master with exit dates — future-work backlog; S4-3b scopes the fix
  to zeroing executable volume for absent names, not a full delisting model.
- A second impact formula — forbidden (C6, ONE cost model); S4 fixes inputs and reach only.
