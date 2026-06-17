# p4 S1 — Spine Hardening — Implementation Plan (frozen *how*)

**Module:** [`../forward-plan-2026-06-16.md`](../forward-plan-2026-06-16.md) — atx-engine v5: Engine Hardening & Productization (Block **A** of that forward plan). A formal `p4/ROADMAP.md` is cut at H1-0; until then the forward-plan doc is the strategic parent.
**Sprint theme:** Close the six confirmed correctness defects in the execution → cost → portfolio-projection spine so that **every downstream backtest number is trustworthy before the real-data scorecard (P3 S2) runs**. At a RenTech 50.75%-hit-rate edge "the entire P&L lives in the last fraction of a basis point of cost accuracy" (`research/renaissance-technologies-systems-deep-dive.md` §9.2.1) — these bugs silently bias fills, cost, capacity, and neutrality. Fix them, test-first, with zero new features.
**Base:** main @ `<SHA recorded at H1-0>` — must contain the merged **p3 S1** real-data path (digest `0x2a22a873483d9157`) and the **p2 S7** distributed digests. **Coordinate with the in-flight `atx-wt/p2-s8` optimizer worktree** (it is actively editing `risk/`): see §0.6 + §4.
**Status:** ⏳ frozen, not opened. This plan is a fossil of intent; mid-sprint scope changes go in `sprint-1-progress.md` "Plan adjustments", never here (`sprint.md` §"The plan moved").
**Discipline:** [`../docs/sprint.md`](../docs/sprint.md) · [`../docs/module.md`](../docs/module.md) · [`../docs/implementation-quality.md`](../docs/implementation-quality.md). Marker + per-unit + close commits; `--no-ff` merge; **no push unless the user asks**.

**Source review:** [`../../reviews/atx-engine-code-review-2026-06-14.md`](../../reviews/atx-engine-code-review-2026-06-14.md) (the multi-agent review that found these) + the 2026-06-16 spot-checks recorded in the forward plan. Each unit below quotes the finding and its as-built line-refs.

**Unit-ID note:** units are `H1-N` (H = Hardening) with commit scope `h1-N`, to avoid log collision with the existing `s1-N` scopes in p2/p3. Each unit maps to a forward-plan block letter (A1–A6).

---

## §0 — As-built reconnaissance (the seams S1 hardens)

**H1-0 must verify every signature + line-ref in this section against the actual code on the worktree base and correct any drift in the ledger before any fix unit starts.** The line numbers below are from the 2026-06-16 read of `main`; if the in-flight p2-s8 work or any merge has shifted them, the marker reconciles. **Trust the code over this doc** where they diverge.

### 0.1 Execution simulator — the cost/fill core (`exec/execution_sim.hpp`) — HARDEN

| Seam | As-built signature / location | Role | Defect touching it |
|---|---|---|---|
| `limit_marketable` | `static bool limit_marketable(const OrderPayload&, f64 ref)` @ `:327-337` — buy passes when `ref ≤ limit`, sell when `ref ≥ limit` | Marketability gate on the **raw** reference | **H1-2 (A2):** gate ignores post-cost `fill_px` |
| `volume_capped_qty` | `i64 volume_capped_qty(const OrderPayload&, const Market&)` @ `:342-358` — **mutates** the per-bar tally via `add_vol_filled` | Per-bar volume cap | **H1-2:** must split peek (no mutate) from commit |
| `emit_fill` | `void emit_fill(order, i64 fillable, f64 ref, Timestamp, Market&)` @ `:362-389` — `fill_px = ref·(1+dir·slip)·(1+dir·temp)` @ `:374-375` | Price the fill, apply perm impact, append `FillPayload` | **H1-2:** this is where the limit can be pierced |
| `slippage_fraction` | `f64 slippage_fraction(i64 fillable, f64 ref, f64 bar_vol, const InstrumentStats&)` @ `:395-411`; spread floor = `(st.spread * 0.5) / ref` @ `:409` | Slippage + spread floor | **H1-5 (A6):** `*0.5` on a half-spread field = quarter spread |
| `temporary_impact` | `f64 temporary_impact(f64 part, const InstrumentStats&)` @ `:415-420` = `Y·σ·part^δ` | √-law temp impact | — (correct; A2 reads it) |
| `apply_permanent_impact` | `void apply_permanent_impact(InstrumentId, f64 part, f64 ref, int dir, const InstrumentStats&, Market&)` @ `:425-435` → `market.shift_mark(id, ref·0.5·γ·σ·part·dir)` | Permanent footprint | **H1-6 (A5):** shift not persisted / not re-marked |
| per-bar vol tally | `reset_vol_accumulator_if_new_bar` @ `:481-487`, `vol_filled_for` @ `:491-498`, `add_vol_filled` @ `:501-509` (linear scan over `vol_for_bar_`) | Deterministic per-bar budget | **H1-2:** peek/commit split lands here |

### 0.2 Market book (`loop/market.hpp`) — HARDEN

| Seam | As-built | Role | Defect |
|---|---|---|---|
| `InstrumentStats` | `{f64 adv, f64 sigma, f64 spread}` @ `:82-86`; **`spread` documented as half-spread** @ `:76` | Per-instrument cost params | **H1-5:** the spread-convention pin anchors here |
| `update_prices` | `void update_prices(const MarketSlice&)` @ `:113-123` — touches only instruments **present** in the slice; `marks_[idx]`, `volumes_[idx]` set from the bar | Refresh marks + volume | **H1-3 (A4):** absent instruments keep stale `volumes_`; **H1-6:** overwrites the perm-impact mark shift |
| `bar_volume` | `f64 bar_volume(InstrumentId) const` @ `:131-133` → returns stored `volumes_[idx]` | Per-bar executable volume | **H1-3:** returns stale volume for absent names → phantom liquidity |
| `mark` | `f64 mark(InstrumentId) const` @ `:127` | Valuation mark | **H1-6:** perm-impact offset must compose here |
| `shift_mark` | `void shift_mark(InstrumentId, f64 delta)` @ `:141+` (asserts a priced mark) | Perm-impact hook | **H1-6:** today the shift is transient (overwritten next slice) |

### 0.3 Capacity (`risk/capacity.hpp`) — HARDEN

| Seam | As-built | Role | Defect |
|---|---|---|---|
| `dollar_adv` | `inline f64 dollar_adv(const PanelView&, usize i, usize w_adv)` @ `:165` = mean(`close·volume`) | **Dollar** ADV | — |
| participation calc | @ `:234-240`: `adv = dollar_adv(...)`; `shares = notional/price`; `part = shares / adv` | Capacity participation | **H1-1 (A1):** shares ÷ dollars — understates impact ≈ `price×` |

### 0.4 Portfolio projection (`risk/optimizer.hpp` + `loop/weight_policy.hpp`) — HARDEN

| Seam | As-built | Role | Defect |
|---|---|---|---|
| `PortfolioOptimizer::project` | @ `:291-300`: `demean_live` → `gross_normalize` → `cap_clip_renorm` (only if `cap < gross`) | Project onto {Σw=0, Σ\|w\|=L, \|w\|≤cap} | **H1-4 (A3):** `cap_clip_renorm` @ `:324-360` never re-imposes Σw=0 |
| `WeightPolicy::to_target_weights` | @ `:197-265`: winsorize → transform → `group_demean?` → `demean(dollar_neutral)` → `gross_normalize` → `truncate_renorm?` @ `:257-259` | Signal → target weights | **H1-4:** `truncate_renorm` after demean breaks Σw=0 identically |

> Note: `WeightPolicy::to_target_weights` already takes `group_map` @ `:199` and asserts it when `industry_neutral` @ `:206`. **Wiring `group_map` through `BacktestLoop` is Block B (p4 S2), NOT this sprint.** H1-4 only fixes the neutrality-preserving projection; it does not touch loop wiring.

### 0.5 The green pipeline S1 must not break — DONE, consume/preserve only

`factory::*`, `eval::*`, `combine::*`, `risk::{FactorModel, MultiPeriodOptimizer}`, `book::BookPipeline`, `parallel::DetPool`, and all real-data path (`data::build_real_panel`, digest `0x2a22a873483d9157`). **S1 adds no feature code here.** The full suite stays green per unit. Any digest that moves does so only because a fix intentionally changed a fill/weight, and is re-pinned per §0.7 #2.

### 0.6 Build + coordination (do NOT hand-author blindly)

- Build with the `dev` preset via the PowerShell tool + env wrapper; `/W4 /permissive- /WX` + `/fp:precise` must stay clean; **do NOT run clang-tidy**; **never hand-edit the test CMake glob** (`tests/*.cpp` auto-globbed via `CONFIGURE_DEPENDS`). These are header-mostly edits (exec/market/capacity are `.hpp`); no new `src/*.cpp` registration expected except if H1-6 extracts an impl unit.
- **p2-s8 conflict guard:** the `atx-wt/p2-s8` worktree is actively building the augmented-QP / `kkt_ldl` solver under `risk/`. **H1-4 edits `risk/optimizer.hpp`.** Before H1-4, confirm whether p2-s8 has touched `optimizer.hpp::project`/`cap_clip_renorm`. If yes → sequence H1-4 **after** p2-s8 merges, or confine H1-4 to a new free-standing `neutral_cap_project()` helper that p2-s8's KKT path does not touch, and rebase. Record the chosen path in the H1-0 ledger. The other five units do not touch `risk/` and are unaffected.

---

## §0.7 — Resolved conventions (frozen at kickoff)

1. **Test-first is mandatory.** For every fix: add a regression that pins the *correct* behavior, run it, watch it **fail** against the current code (record the red in the ledger row), then fix, then watch it pass. No behavior change ships without a prior red test. This is the whole point of the sprint — the prior review noted the existing targeted suites *passed* over these bugs, so new regressions are the only proof.
2. **Golden-digest re-pin policy.** A fix that changes a fill price, a weight, or a mark will move pinned digests. When a digest moves: (a) confirm the move is *caused by the intended fix only* (diff the inputs), (b) update the golden literal in-place, (c) record old→new digest + the one-line cause in the ledger row and the H1-close summary. The watched pins are the p3 S1 real-data digest `0x2a22a873483d9157` and the p2 S7 5-path digest. A digest moving for an *unexplained* reason is a defect — stop and investigate (`systematic-debugging`).
3. **Limit-order contract = REJECT (not cap).** When the post-cost `fill_px` would pierce the limit (buy: `fill_px > limit`; sell: `fill_px < limit`), the slice **does not fill** this bar (the order rests). We do **not** cap the fill price to the limit. Rationale: rejecting is the truthful marketable-limit semantics and avoids fabricating a fill at a price the cost model says is unreachable. Capping is a deferred option (note it in the ledger as a future knob), not this sprint.
4. **Permanent-impact model = PERSISTENT.** Perm impact is a lasting, information-driven mark move (matches the cost research). Implement via a **per-instrument cumulative impact offset** that `Market::update_prices` *adds on top of* each incoming bar mark (the offset survives slice updates), and **re-mark the portfolio after settlement, before sampling equity**. The transient "shift then get overwritten" behavior is the bug. The no-impact path (γ=0 or part=0) must stay byte-identical.
5. **Spread field stays half-spread.** `InstrumentStats::spread` keeps its as-built documented meaning (half-spread, `market.hpp:76`). The defect is the extra `* 0.5` in `slippage_fraction` @ `:409`, which charges a *quarter* spread. Fix = drop the `* 0.5` so the floor crosses the full half-spread: `half_spread_frac = (ref > 0) ? st.spread / ref : 0`. We do **not** rename the field to `full_spread` (that would churn calibration + tests for no semantic gain). Pin the convention with a test + a one-line comment.
6. **Scope guard — correctness only.** H1 changes execution/cost/projection **correctness** exclusively. **Out of scope this sprint:** the CLI/`run_backtest` facade and loop-wiring of `group_map`/factor-model/borrow (that is Block B = p4 S2); the P1 performance items from the review (dense market accessors, O(fills²) accumulator, VM panel copy — a later perf sprint); any new strategy/feature. Each fix is the *minimal* change that makes its regression pass.

---

## §1 — Per-unit plan

Each unit: **one sub-agent, one commit (or a tight series), tests green before stop.** The chain that touches `exec/execution_sim.hpp` + `loop/market.hpp` (**H1-2 → H1-3 → H1-5 → H1-6**) is dispatched **sequentially** — they edit the same two headers and a sub-agent cannot see an uncommitted predecessor. **H1-1** (`risk/capacity.hpp`) and **H1-4** (`risk/optimizer.hpp` + `loop/weight_policy.hpp`) touch disjoint files and may run in parallel with the chain (H1-4 subject to the §0.6 p2-s8 guard). Every brief ends with the verbatim marker-commit reminder (§3).

---

### H1-0 — Marker (orchestrator, not a sub-agent)

**Scope:** Open the sprint; freeze scope; reconcile §0 line-refs; resolve the p2-s8 coordination.

**Do:**
1. Create worktree + branch per `sprint.md` (`worktree-phase-p4s1-spine-hardening` / matching branch) off the chosen base on `main`.
2. **Verify §0 line-refs** against the actual base; correct drift in a ledger "as-built recon" subsection. Re-confirm each of the six defects is still present (the forward plan confirmed A1/A2/A6 + the engine.hpp placeholder open on 2026-06-16; re-confirm A3/A4/A5).
3. **Resolve the p2-s8 conflict (§0.6):** inspect whether `atx-wt/p2-s8` has modified `risk/optimizer.hpp::project`/`cap_clip_renorm`. Record the H1-4 strategy (sequence-after-merge vs free-standing helper + rebase) in the ledger.
4. Cut `p4/ROADMAP.md` (minimal: the Block A–F table from the forward plan as the status spine) and open `sprint-1-progress.md` (skeleton from `sprint.md` §"Progress ledger"). Point the ROADMAP S1 row at the ledger; set `Last reviewed`.
5. Capture a **baseline suite count** (`<total>/0/0`) so per-unit deltas are measurable, and the current values of the two watched digests.

**Commit:** `docs(h1-0): open p4 spine-hardening ledger + as-built recon + p2-s8 coordination record`
**Acceptance:** ledger + minimal `p4/ROADMAP.md` exist; base SHA, baseline suite count, watched-digest baselines, and the H1-4/p2-s8 strategy recorded; §0 drift reconciled. No code yet.

---

### H1-1 — Capacity participation unit fix  *(A1; P0; smallest; parallel-eligible)*

**Finding (review §"Capacity Impact Divides Shares By Dollar ADV"):** `risk/capacity.hpp:234-240` computes `part = shares / dollar_adv` — shares over dollars, understating participation by ≈ instrument price → capacity curves too optimistic, admitting capacity that should fail.

**Files:** `include/atx/engine/risk/capacity.hpp` (edit `:234-240`); `tests/capacity_test.cpp` (extend — auto-globbed).

**Fix:** participation in dollar units against the dollar denominator:
```cpp
// adv is dollar-ADV (mean of close*volume). notional is dollars. participation is
// dimensionless: dollars traded / dollars available. (Was: shares / dollar_adv.)
const atx::f64 part = (adv > 0.0) ? notional / adv : 0.0;
```
Delete the `shares = notional/price` intermediate (or keep only if used elsewhere; if unused after the fix, remove it so the unit is explicit). Add a one-line comment naming the unit convention.

**Test plan (`capacity_test.cpp`, `ATX_TEST`):**
1. `ParticipationIsNotionalOverDollarAdv` — hand-computed: price=$50, volume + window giving a known `dollar_adv`, a known `notional`; assert `part == notional/dollar_adv` exactly. **Must fail** under the old `shares/adv` (the $50 price makes the two differ ~50×).
2. `CapacityBpsCostMatchesHandComputation` — feed the corrected `part` through the impact form; assert the bps cost equals a hand calc; this is the end-to-end guard the old test lacked.
3. `ZeroAdvYieldsZeroParticipation` — degenerate `adv==0` ⇒ `part==0` (no div-by-zero), matching the existing guard style.

**Commit:** `fix(h1-1): capacity participation = notional/dollar_adv (was shares/dollar_adv)`
**Acceptance:** 3 tests pass; the new test demonstrably red on the pre-fix code (recorded); full suite green; `/W4 /WX` clean. Capacity curves shift *down* (expected, correct) — note any capacity-dependent golden that moves per §0.7 #2.
**Depends:** H1-0. Disjoint from the exec/market chain → parallel-eligible.

---

### H1-2 — Limit orders must not fill through their limit  *(A2; P0)*

**Finding (review §"Limit Orders Can Fill Through Their Limit"):** `limit_marketable` (`execution_sim.hpp:327-337`) gates on raw `ref`; `emit_fill` (`:374-375`) then applies slip+temp impact, so a buy-limit can fill **above** its limit (and a sell-limit below). The in-code comment admits it is a "deferred residual."

**Files:** `include/atx/engine/exec/execution_sim.hpp` (`limit_marketable`, `volume_capped_qty`, `emit_fill`, and the per-bar vol tally helpers); `tests/execution_sim_test.cpp`.

**Fix (peek/commit split + post-cost gate):**
1. **Compute the candidate post-cost `fill_px` before consuming bar volume.** Factor the price math out of `emit_fill` into a pure `[[nodiscard]] f64 price_fill(order, i64 fillable, f64 ref, const Market&) const` (slippage + temp impact; **no** perm-impact side effect, **no** volume mutation).
2. **Split `volume_capped_qty` into peek + commit.** A pure `[[nodiscard]] i64 peek_fillable(order, const Market&) const` (the current budget math, **no** `add_vol_filled`) and a `void commit_fill_volume(InstrumentId, i64)` that does the tally mutation. The accumulator is only touched **after** the price is accepted.
3. **Gate at settle:** for a Limit order, compute `peek_fillable` → `price_fill`; if buy and `fill_px > limit` (or sell and `fill_px < limit`) → **reject** (no fill, order rests; per §0.7 #3). Otherwise `commit_fill_volume` then `emit_fill`. Market orders keep the current path. Use a small epsilon consistent with the Decimal price boundary so an exact-equal fill at the limit passes.

**Test plan (`execution_sim_test.cpp`):**
1. `BuyLimitRejectsWhenSlippagePushesAboveLimit` — buy limit at `L`, `ref==L`, FixedBps slippage > 0 ⇒ `fill_px > L` ⇒ **no fill**.
2. `SellLimitRejectsWhenSlippagePushesBelowLimit` — symmetric.
3. `BuyLimitRejectsWhenTempImpactPushesAboveLimit` — same with `ImpactCfg` temp impact (part>0) instead of bps.
4. `MarketableLimitFillsAtOrInsideLimit` — buy limit with `ref` comfortably below `L`; fills, and `fill_px ≤ L`.
5. `RejectedLimitDoesNotConsumeBarVolume` — after a rejected limit, `vol_filled_for(id) == 0` (proves peek didn't mutate); a subsequent marketable order on the same bar gets the full budget.
6. `MarketOrderPathUnchanged` — a market order's fills are byte-identical to pre-fix (guards the refactor).

**Commit:** `fix(h1-2): gate limit fills on post-cost price (peek/commit split); reject through-limit fills`
**Acceptance:** 6 tests pass; new tests red pre-fix; market-order digests unchanged; full suite green; `/W4 /WX` clean.
**Depends:** H1-0. First in the exec/market sequential chain.

---

### H1-3 — Stale bar volume → phantom liquidity  *(A4; P0)*

**Finding (review §"Stale Bar Volume Creates Phantom Liquidity"):** `market.hpp:113-123` updates only instruments present in the current slice; `bar_volume` (`:131-133`) returns the stored `volumes_[idx]`, so an absent/delisted instrument keeps prior volume and an order can fill against liquidity that isn't there.

**Files:** `include/atx/engine/loop/market.hpp` (`update_prices`, `bar_volume`, add an epoch field); `tests/market_data_test.cpp` (or the existing market test file — verify the name at H1-0); a focused assertion in `execution_sim_test.cpp` for the end-to-end no-fill.

**Fix (per-slice volume epoch — O(touched), not O(universe)):**
- Add `u64 volume_epoch_ = 0;` and a parallel `std::vector<u64> vol_stamp_` (per instrument, sized with `volumes_`).
- In `update_prices`: `++volume_epoch_;` at entry; for each present instrument set `volumes_[idx]` **and** `vol_stamp_[idx] = volume_epoch_`.
- `bar_volume(id)` returns `volumes_[idx]` only if `vol_stamp_[idx] == volume_epoch_`, else `0.0`.
- **Marks are untouched** — valuation keeps the last mark (`mark()` unchanged); only *executable volume* goes to zero for absent names. This is the mark-vs-volume separation the review asked for.

**Test plan:**
1. `AbsentInstrumentHasZeroExecutableVolume` — slice t1 has volume for X; slice t2 omits X; `bar_volume(X) == 0` at t2 while `mark(X)` still returns the last close.
2. `OpenOrderDoesNotFillAgainstStaleVolume` (`execution_sim_test.cpp`) — order on X rests at t1, X absent at t2 ⇒ no fill at t2.
3. `DelistedFinalBarFillsThenStops` — X has a valid final bar (fills allowed) then is absent on later slices (no fill) — the delisting case.
4. `PresentInstrumentVolumeUnchanged` — an instrument present every slice behaves exactly as before (epoch is a no-op for it).

**Commit:** `fix(h1-3): zero executable volume for instruments absent from the current slice (volume epoch)`
**Acceptance:** 4 tests pass; new tests red pre-fix; present-instrument digests unchanged; full suite green; `/W4 /WX` clean.
**Depends:** H1-2 (same files: `market.hpp` / `execution_sim_test.cpp`). Second in the chain.

---

### H1-4 — Neutrality-preserving cap projection  *(A3; P0; parallel-eligible per §0.6)*

**Finding (review §"Cap Projection Can Break Dollar Neutrality"):** `optimizer.hpp::project` (`:291-300`) runs `demean → gross_normalize → cap_clip_renorm` (`:324-360`) but `cap_clip_renorm` rescales by **absolute** mass and never re-imposes `Σw=0`; `weight_policy.hpp` (`:243-259`) has the identical `demean → gross_normalize → truncate_renorm` shape. The reviewer found a feasible 4-name capped-neutral case returning gross 1.0 with net ≈ 0.234.

**Files:** a new shared helper `include/atx/engine/risk/neutral_cap_project.hpp` (free function, header-only, no state) consumed by **both** `risk/optimizer.hpp` and `loop/weight_policy.hpp`; `tests/risk_neutral_cap_project_test.cpp`; extend `tests/optimizer_test.cpp` + `tests/weight_policy_test.cpp`.

**Fix (separate-sides simplex projection):** when dollar-neutral is requested, project the positive and negative sides **independently** each onto a capped simplex of mass `gross/2`:
```
neutral_cap_project(w, gross, cap):
  split live names into P = {i: w_i > 0}, N = {i: w_i < 0}
  scale P so Σ_{P} w_i =  gross/2, with per-name clip to +cap   (fixed clip-renorm, as today but per side)
  scale N so Σ_{N} w_i = -gross/2, with per-name clip to -cap
  ⇒ Σw = 0 exactly, Σ|w| = gross exactly, |w_i| ≤ cap  (for a feasible cap)
infeasible (cap·|P| < gross/2 or cap·|N| < gross/2): pin that side at the cap;
  return a typed `Result::Err(Infeasible)` rather than silently de-neutralizing.
```
Refactor `cap_clip_renorm`/`truncate_renorm` to call the per-side helper when `dollar_neutral` is on; keep the **non-neutral** path (no `Σw=0` requirement) on the existing single-simplex clip-renorm so its digests are unchanged. Factor the per-side clip-renorm so optimizer + weight_policy share one implementation and cannot drift.

**Test plan:**
1. `FourNameCappedNeutralStaysNeutral` (`risk_neutral_cap_project_test.cpp`) — the reviewer's case: assert `|Σw| < 1e-9`, gross == 1.0, `max|w| ≤ cap+1e-9`. **Red** pre-fix (net ≈ 0.234).
2. `InfeasibleCapReturnsErrNotSilentDeneutralize` — caps too tight for the long/short counts ⇒ typed error.
3. `PropertyRandomFeasibleStaysNeutralAndCapped` — N random feasible inputs ⇒ `|Σw|<1e-9` ∧ `max|w|≤cap`.
4. `NonNeutralPathByteIdentical` (`optimizer_test.cpp`) — with `dollar_neutral` OFF, output is byte-identical to pre-fix (the existing 16 optimizer tests stay green).
5. `WeightPolicyNeutralCapMatchesOptimizer` (`weight_policy_test.cpp`) — same inputs through both call sites give the same weights (shared-helper proof).

**Commit:** `fix(h1-4): neutrality-preserving per-side cap projection (shared by optimizer + weight_policy)`
**Acceptance:** 5 tests pass; #1 red pre-fix; the 16 existing optimizer tests + weight_policy tests green; non-neutral digests unchanged; neutral-mode digests re-pinned per §0.7 #2 with cause noted; `/W4 /WX` clean.
**Depends:** H1-0 **and the §0.6 p2-s8 resolution** recorded at H1-0. Disjoint from the exec/market chain (parallel-eligible) but gated on the optimizer-conflict strategy.

---

### H1-5 — Spread-convention pin  *(A6; P1)*

**Finding (review §"Spread Semantics Are Inconsistent"):** `InstrumentStats::spread` is documented as a half-spread (`market.hpp:76`), but `slippage_fraction` (`execution_sim.hpp:409`) computes the floor as `st.spread * 0.5 / ref` — charging a *quarter* of the full spread.

**Files:** `include/atx/engine/exec/execution_sim.hpp` (`:409`); a clarifying comment at `market.hpp:76`; `tests/execution_sim_test.cpp`.

**Fix (per §0.7 #5 — field stays half-spread, drop the erroneous `*0.5`):**
```cpp
// Spread floor: always cross the (half-)spread. st.spread IS the half-spread
// (see InstrumentStats), so the floor as a fraction of ref is st.spread/ref.
const atx::f64 half_spread_frac = (ref > 0.0) ? st.spread / ref : 0.0;
```
Update the `market.hpp:76` comment to state the contract once, unambiguously: `spread` = **half**-spread in price units; execution crosses it whole as the slippage floor.

**Test plan (`execution_sim_test.cpp`):**
1. `SpreadFloorCrossesFullHalfSpread` — `st.spread = h`, FixedBps below the floor, `ref` known ⇒ fill slippage fraction `== h/ref` (not `0.5·h/ref`). **Red** pre-fix.
2. `SlippageAboveSpreadFloorWins` — when modelled slippage exceeds the floor, the floor is inert (guards we only changed the floor term).
3. `ZeroSpreadZeroFloor` — `spread==0` ⇒ floor 0 (degenerate).

**Commit:** `fix(h1-5): spread floor crosses the full half-spread (drop erroneous *0.5)`
**Acceptance:** 3 tests pass; #1 red pre-fix; cost-dependent digests re-pinned per §0.7 #2 with cause noted; full suite green; `/W4 /WX` clean.
**Depends:** H1-3 (same files: `execution_sim.hpp` / `market.hpp`). Third in the chain.

---

### H1-6 — Permanent-impact persistence + post-settlement re-mark  *(A5; P1; decision-bearing; last)*

**Finding (review §"Permanent Impact Does Not Persist Or Mark Equity Reliably"):** `apply_permanent_impact` (`execution_sim.hpp:425-435`) calls `Market::shift_mark` during fill emission — *after* `Portfolio::mark_to_market` has already run for the slice (`backtest_loop.hpp:251-259`), so the equity sample (`:318-322`) misses it; and the next `update_prices` (`market.hpp:119-121`) overwrites the shifted mark, so "permanent" impact neither persists nor marks reliably.

**Decision: PERSISTENT (§0.7 #4).**

**Files:** `include/atx/engine/loop/market.hpp` (cumulative impact offset + compose in `update_prices`/`mark`/`shift_mark`); `include/atx/engine/loop/backtest_loop.hpp` (re-mark after settlement, before equity sample); `tests/market_data_test.cpp`, `tests/execution_sim_test.cpp`, `tests/backtest_loop_test.cpp`.

**Fix:**
1. **Market carries a cumulative per-instrument impact offset:** add `std::vector<f64> impact_offset_` (sized with `marks_`, zero-init). `shift_mark(id, delta)` accumulates into `impact_offset_[id]` **and** applies to `marks_[id]`. `update_prices` sets `marks_[idx] = bar_close + impact_offset_[idx]` (the offset survives the new bar instead of being overwritten). `mark(id)` returns the composed value. *(Decide at implementation: whether the offset is in absolute price units and persists indefinitely, or decays — for this sprint it persists undecayed; a decay knob is a documented future residual.)*
2. **Loop re-marks after settlement:** in `backtest_loop.hpp`, after the execution/settlement step that can emit perm impact, call `Portfolio::mark_to_market` again (or move the existing mark to *after* settlement) so the equity/gross/net sample at `:318-322` reflects the shifted marks for the current slice.
3. **No-impact invariance:** when `γ==0` or every `part==0`, `impact_offset_` stays zero and the whole path is byte-identical to pre-fix (assert via digest).

**Test plan:**
1. `PermImpactShiftsCurrentSliceEquity` (`backtest_loop_test.cpp`) — a buy that emits perm impact ⇒ the same slice's post-settlement equity reflects the shifted mark (was missed pre-fix).
2. `PermImpactPersistsAcrossNextBar` (`market_data_test.cpp`) — after a shift, the next `update_prices` yields `mark == new_close + offset`, not the raw close (was overwritten pre-fix).
3. `BuyAndSellImpactSignsCompose` — a buy then a sell on the same name compose offsets with correct signs.
4. `ZeroGammaByteIdenticalToPreFix` — `ImpactCfg.gamma==0` ⇒ the backtest digest is unchanged (the no-impact firewall).
5. `RepricedFillsRespectShiftedMark` (`execution_sim_test.cpp`) — a later order on the same name prices against the shifted mark.

**Commit:** `fix(h1-6): persistent permanent-impact offset + post-settlement re-mark`
**Acceptance:** 5 tests pass; new tests red pre-fix; `gamma==0` digest byte-identical; any impact-on golden re-pinned per §0.7 #2 with cause noted; full suite green; `/W4 /WX` clean. **This is the most behaviorally-invasive unit — its digest movements get the most scrutiny in the close summary.**
**Depends:** H1-5 (same files: `execution_sim.hpp`/`market.hpp`, plus `backtest_loop.hpp`). Last in the chain.

---

### H1-close — Sprint close ceremony

Run in one close commit (or a tight series), per `sprint.md` §"Sprint close ceremony":
1. **Lift residuals** → `p4/ROADMAP.md` future-work backlog: the limit-order *cap* alternative (§0.7 #3), the perm-impact *decay* knob (H1-6), and the deferred Block-B/P1-perf items (CLI/runner, `group_map` loop wiring, dense market accessors, O(fills²) accumulator, VM panel copy).
2. **Update `p4/ROADMAP.md` status table** `⏳ → ✅ <sha>` for S1 (Block A); record each unit's commit SHA; bump `Last reviewed`.
3. **Write the digest-movement audit:** a table in the ledger of every golden that moved (old→new, unit, cause), per §0.7 #2 — the single most important close artifact for trust.
4. **Write `phase-p4s1.md` user reference** — the corrected cost/projection contracts (limit-reject semantics, half-spread floor, persistent perm impact, neutrality-preserving cap) + the capacity-unit correction, so downstream users know the numbers changed and why. Link the ledger; no number duplication.
5. **Write the ledger "What S1 proves / Next sprint priorities" baton** — hand p4 S2 (Block B: Unified Runner) a hardened spine, and explicitly state: *the real-data scorecard (P3 S2) is now safe to run on trustworthy cost/risk numbers.*
6. **`--no-ff` merge** into `main` with `merge: p4 S1 — spine hardening`. **No push unless the user asks.**

**Commit:** `docs(h1-close): close p4 S1 spine hardening — 6 fixes, M tests, digest-movement audit` then the merge.

---

## §2 — Dependency graph + dispatch order

```
H1-0 (marker, orchestrator; resolves p2-s8 coordination + baselines)
  ├── H1-1 capacity unit ................ parallel (disjoint: risk/capacity.hpp)
  ├── H1-4 neutral cap projection ....... parallel (disjoint: risk/optimizer + weight_policy)  ← gated on §0.6 p2-s8 strategy
  └── exec/market chain (sequential — shared headers):
        H1-2 limit post-cost gate  (execution_sim.hpp)
          └── H1-3 stale volume epoch  (market.hpp + execution_sim_test)
                └── H1-5 spread-convention pin  (execution_sim.hpp + market.hpp)
                      └── H1-6 perm-impact persistence  (execution_sim + market + backtest_loop)  ← decision-bearing, most invasive
H1-close (after H1-1..H1-6 land)
```

**Dispatch:** **H1-2 → H1-3 → H1-5 → H1-6** sequential (they edit `execution_sim.hpp`/`market.hpp`; a sub-agent can't see an uncommitted predecessor). **H1-1** and **H1-4** dispatched in parallel with the chain (disjoint files) — send both at kickoff in a single message — with **H1-4 held until the H1-0 p2-s8 strategy is recorded**. Verify each unit's commit + green suite (+ the new test's pre-fix red, recorded) before dispatching its dependent. Fix order across the whole sprint: cheapest/most-isolated first (H1-1), decision-bearing/most-invasive last (H1-6).

---

## §3 — Sub-agent brief template (every fix unit)

Each brief MUST include (per `sprint.md` §"What each sub-agent brief must include"):

1. **Worktree path + branch** — `cd` target.
2. **Scope** — quoted verbatim from this plan's unit section (the finding + the fix), not paraphrased.
3. **Test-first acceptance:** the named regression(s) must exist and be shown **red on the pre-fix code** (paste the red output into the ledger row), then green after the minimal fix; the full suite stays green; `/W4 /permissive- /WX` + `/fp:precise` clean.
4. **Predecessor SHAs** — what they build on (e.g. H1-3 gets H1-2's SHA).
5. **Expected commit message** — the `fix(h1-N): …` from the unit.
6. **Ledger row to write** — the `sprint.md` row shape (status, SHA, one-paragraph notes **with test counts, the recorded pre-fix red, and any digest that moved + its cause**), committed as part of the unit.
7. **Build instructions** — PowerShell tool + env wrapper, `dev` preset; **`atx-shm-worker` must be built** if any Parallel test is touched; **do NOT run clang-tidy**; **never hand-edit the test CMake glob**; **never reformat third-party/submodule code**.
8. **Implementation-quality standard** — paste the handoff block from [`../docs/implementation-quality.md`](../docs/implementation-quality.md): smart comments (invariants not noise), explicit unit conventions on the changed lines, **no partial stubs**, functions ≤ ~60 lines, exhaustive enum switches with **no `default`**. **Minimal-diff rule:** change only what the regression requires; do not opportunistically refactor (that reopens digests).
9. **The verbatim marker-commit reminder:**

   > **Marker-commit pattern is mandatory: commit before stopping or the work is lost.** Your unit is not done until its regression (shown red pre-fix, green post-fix), the minimal fix, and the ledger row are committed and the full test suite is green. Do not return uncommitted.

A brief that omits any of these produces work that doesn't fit the seam — reject and re-open.

---

## §4 — Risks + how this plan de-risks them

| Risk | Mitigation in-plan |
|---|---|
| **A fix changes fills/weights and silently moves a golden digest** | §0.7 #2 re-pin policy + the H1-close digest-movement audit table: every moved golden is old→new with a named cause; an *unexplained* move stops the sprint. |
| **The "fix" passes existing tests that passed over the bug** | Test-first is mandatory (§0.7 #1): each regression must be shown **red on the pre-fix code** before the fix — proves the test actually pins the bug. |
| **H1-4 collides with the in-flight p2-s8 optimizer worktree** | §0.6 + H1-0 resolve the strategy up front (sequence-after-merge or free-standing `neutral_cap_project` helper + rebase); the other 5 units don't touch `risk/`. |
| **Perm-impact rework (H1-6) breaks the no-impact path** | §0.7 #4 + H1-6 test #4: `gamma==0` must yield a **byte-identical** backtest digest — the no-impact firewall. |
| **Scope creep into CLI/wiring/perf** | §0.7 #6 scope guard: H1 is correctness-only; Block B (runner/wiring) and the P1 perf items are explicitly out and lifted to the p4 backlog at close. |
| **Limit-reject semantics surprise a downstream strategy** | §0.7 #3 freezes REJECT (truthful marketable-limit), documents the cap alternative as a future knob, and `phase-p4s1.md` announces the contract change. |
| **Breaking the green p2/p3 pipeline** | S1 adds no `factory::`/`eval::`/`data::` feature code; full suite green is a per-unit gate; the real-data digest `0x2a22a873483d9157` is a watched pin. |

---

*This plan is frozen. Scope changes during execution go into `sprint-1-progress.md` "Plan adjustments", not here. On close, the spine is trustworthy and the real-data scorecard (P3 S2) is cleared to run.*
