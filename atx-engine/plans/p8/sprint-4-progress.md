# p8 Sprint 4 — Cost, Capacity & Execution Correctness — Progress Ledger

Base: feat/p8 @ a7838c6 (S1 complete: risk-model covariance spine).

Branch: feat/p8  Worktree: C:\atx-wt\p8

## Kickoff — bug-verification pass re-confirmed against the CURRENT tree (2026-07-03)

Every LIVE candidate below was re-read against the current file:line before scoping a unit (per
the spec's binding instruction: "S4 ships work for the LIVE rows only"). File:line evidence
re-confirmed at kickoff (line numbers may drift a few lines from the spec's own citation where the
file has since shifted; the CURRENT line is recorded here as the unit's authority):

| # | Candidate | Verdict | Confirmed file:line (kickoff re-read) |
|---|---|---|---|
| B1 | Capacity participation unit bug (shares÷dollar-ADV) | **LIVE — 3 sites** | `risk/capacity.hpp:238-240` (`part = shares/adv`, `adv=dollar_adv`=dollars, `capacity.hpp:165-177`); `factory/fitness.cpp:523` (`part=(target_aum·|w|/price)/adv`); SEAM `atx-impl/src/stage_combine.cpp:315` (`part_per_aum = abs_w/(price*adv)` — S3-owned, confirmed at kickoff to be the current line, not `:289,301` as the spec's earlier draft cited; the bug SHAPE is identical). |
| B2 | Cap-clip-renorm breaks dollar-neutrality | **LIVE — 2 sites** | `risk/optimizer.hpp:389-398` (`project`: demean→gross_normalize→cap_clip_renorm, no re-center after); `:422-458` (`cap_clip_renorm` ends on the deficit-renorm, no demean). `loop/weight_policy.hpp:311-327` (demean→gross_normalize→truncate_renorm, no re-center after); `:480-529` (`truncate_renorm`/`finalize_truncation`, no demean). |
| B3 | Limit orders fill THROUGH their limit after costs | **LIVE** | `exec/execution_sim.hpp` `emit_fill`: `fill_px = ref·(1+dir·slip)·(1+dir·temp)`, no re-clamp to the order's limit for `OrderType::Limit`. |
| B4 | Absent instruments retain stale executable volume | **LIVE** | `loop/market.hpp` `update_prices`: touches only slice-present rows; an absent id's prior `volumes_[idx]` survives, feeding `bar_volume()` → `volume_capped_qty` phantom fills. |
| B5 | Borrow accrual not in the main loop | **LIVE** | `cost/borrow.hpp` `accrue_borrow` built+tested, zero call sites in `src/`; `loop/backtest_loop.hpp` settle sequence never charges short financing. |
| B6 | Permanent impact not persisted across bars | **LIVE** | `exec/execution_sim.hpp` `apply_permanent_impact` → `market.shift_mark` shifts `marks_[idx]` in place; the next `update_prices` (`loop/market.hpp`) overwrites `marks_[idx] = r.bar.close` for any name present next bar, clobbering the shift. |
| B7 | Impact charged only as telemetry, not in the SELECTION objective | **PARTIALLY LIVE** | `factory/fitness.cpp` `book_cost_bps` feeds `objectives[4]` (NSGA) only, gated by `target_aum>0`; ScalarRaw ranks on `raw = wq·diversify·robust` (`fitness.cpp:416`), no cost term; `search_driver.cpp` ScalarRaw path never reads `objectives`. |
| B8 | Garleanu-Pedersen partial-trade | **ALREADY WIRED (linear); make turnover-native** | `risk/garleanu_pedersen.hpp` ships the scalar-Λ aim `gp_aim_and_value`; `atx-impl/src/stage_optimize.cpp:159-172` does a LINEAR blend `w := prev + rate·(w-prev)` toward the freshly-shaped TARGET, not the GP AIM. |
| B9 | First-class capacity curve emitted by stage_report? | **NOT EMITTED (scalar footprint only)** | `atx-impl/src/stage_report.cpp` emits per-name %ADV scalars at one `report_aum`; no `aum_grid`/zero-crossing vector. The build blocks (`risk::capacity_curve`, `cost::capacity_point`, `cost::capacity_for_book`, `cost::emit_capacity_scorecard`) already exist unused by the report.

**Verified-already-fixed / out-of-scope (no unit spent on these):** the p6-S4 admission-gate cost
(`combine/gate.hpp`, `cost_adjusted_fitness`) is shipped/merged — a different decision point
(threshold on an already-picked candidate) from S4-4's search-selection scalar. The √-law cost
MODEL (`cost/cost_aware.hpp` `round_trip_cost_bps`) is the ONE cost model (C6) — S4 does not add a
second impact formula; it fixes the model's INPUTS (participation, B1) and REACH (ScalarRaw +
default-on profile, B7).

## CostSelectionConfig (S4-0 plumbing)

New header `atx-engine/include/atx/engine/cost/cost_selection_config.hpp`:
```cpp
struct CostSelectionConfig {
  bool     impact_in_selection = false;  // inert => raw unchanged (gross selection, today)
  atx::f64 selection_aum       = 0.0;    // AUM the selection cost is priced at; 0 => off
  atx::f64 cost_weight         = 1.0;    // multiplier on the net-of-cost penalty when on
};
```
Pure addition, no existing call site reads it. `factory::FitnessCfg` (fitness.hpp, S5-owned) does
NOT gain a field yet — S4 must not edit fitness.hpp.

**SEAM (binding, for Sprint 5):** Sprint 5 adds `cost::CostSelectionConfig cost_selection{};` to
`FitnessCfg` (fitness.hpp, alongside the existing `target_aum`/`cost` fields, ~line 357) and one
call site in `detail::finish_report` (fitness.cpp) that forwards `cfg.cost_selection` + the
already-computed `core.cost_bps` (re-priced at `cfg.cost_selection.selection_aum` via
`book_cost_bps`, see S4-4) into `factory::apply_selection_cost` (the S4-4-shipped function,
`factory/fitness_cost_selection.hpp`). Until S5 lands that field+call, S4's own on-path tests
construct `CostSelectionConfig` directly and call `apply_selection_cost` — this is the documented,
intentional gap (S4-4 section below).

## Unit checklist
- [x] S4-0  Ledger + bug-verification table + `CostSelectionConfig`
- [x] S4-1  Capacity participation unit fix (B1)
- [x] S4-2  Cap-clip-renorm re-center (B2)
- [x] S4-3a Limit fill clamp (B3)
- [x] S4-3b Absent-instrument volume zeroing (B4)
- [ ] S4-3c Borrow accrual in the loop (B5)
- [ ] S4-3d Permanent impact persistence (B6)
- [ ] S4-4  Impact in ScalarRaw selection (B7)
- [ ] S4-5a GP turnover-native step (B8)
- [ ] S4-5b stage_report capacity curve (B9)

## Determinism contracts (both apply this sprint)
- **(A) Opt-in** (S4-4, S4-5): inert default ⇒ byte-identical. `NsgaSearch.ScalarRaw_ReproducesGoldenDigest`,
  `FactoryOos.MineIntoOffPathDigestUnchanged`, optimize/report goldens unchanged on the no-flag path.
- **(B) Correctness fix — documented exception** (S4-1/2/3): the fix changes the number because the
  OLD number was wrong. Any golden encoding the bug is re-baselined WITH the fix commit's SHA as the
  authority; recorded below (name, old digest, new digest, fixture). NO silent drift.

## Log
S4-0: complete. Ledger + bug-verification table written; `CostSelectionConfig` added
(`cost/cost_selection_config.hpp`, new file); `cost_selection_config_test.cpp` (`tests/core/`) pins
the inert defaults `{false, 0.0, 1.0}`. Pure addition — no existing call site changed, no golden
touched.

S4-1: complete [CORRECTNESS — B1]. Fixed the participation UNIT bug at its two S4-owned sites:
- `risk/capacity.hpp` `detail::impact_cost_bps` (was `shares = notional/price; part = shares/adv`,
  now `part = notional/adv`, dropping the price division). Header contract comment (the "capacity
  model" block) updated to match.
- `factory/fitness.cpp` `book_cost_bps` (was `part = (target_aum·|w|/price)/adv`, now
  `part = (target_aum·|w|)/adv`). `price` is still read to gate out unpriced names (no book value)
  but no longer enters the participation ratio.

**RED (by-construction fixture, `tests/risk/capacity_participation_test.cpp`, new):** single-name
book, `aum=$1e6`, `|w|=1.0`, `price=$100`, dollar-ADV=`$1e6`, sim `Y=1,delta=1` (isolates `part`
linearly), alternating ±1% returns (`sigma=0.01` exactly, `gross_edge_bps=0` exactly so
`net_edge_bps = -cost_bps`). Correct `part = notional/adv = 1.0` ⇒ `cost_bps=100.0` ⇒
`net_edge_bps=-100.0`. Buggy `part = shares/adv = 0.01` (off by exactly `1/price`) ⇒ `cost_bps=1.0`
⇒ `net_edge_bps=-1.0`. `CapacityParticipation.ParticipationIsNotionalOverDollarAdv` RED: asserted
`-100.0`, got `-1.0000000000000198` (diff 99.0, fails). A second name at `price=$10` (same
notional/ADV/sigma) proves price-scaling: buggy `net_edge_bps=-10.0` (10x A, matching the 10x price
ratio) vs A's `-1.0` — `CapacityParticipation.CostIsPriceInvariantForEqualNotional` RED: asserted
equal, got `-1.0` vs `-10.0` (fails). **Deviation note:** the sprint plan's own worked arithmetic
("`(1e6/100)/1e6 = 1e-4`") has a slip — the correct value is `0.01` (`1e4/1e6`), matching its own
prose ("off by exactly 1/price", `1/100=0.01`). This ledger's fixture uses the arithmetically
correct `0.01`; the FORMULA/conclusion (participation inflated capacity by ~`price`, understated by
`1/price`) is unchanged and is what both accept tests prove.

**GREEN:** both new tests pass after the fix (`atx-engine-risk-tests`: 15/15 incl.
`RiskCapacity.*`/`CapacityScorecard.*` unaffected). `atx-engine-core-tests`
(`CapacityScorecard.MonotoneCrossesNearAnalytic`, updated below) green.
`atx-engine-factory-tests` (`FactoryCostAwareFitness.*`, updated below) green, full-suite digest
check pending (see below).

**Existing tests that ENCODED the bug — updated in this commit (not a golden digest, but a
hand-computed "expected" pinned to the old formula; contract-B re-baseline):**
- `tests/factory/factory_cost_aware_fitness_test.cpp`
  `FactoryCostAwareFitness.HandCheck_BookCostMatchesRoundTrip`: hand-computation changed
  `part = shares/adv` (`shares = target_aum/price`) → `part = notional/adv`
  (`notional = target_aum·|w|`), matching the fixed `book_cost_bps`. Old/new VALUE at this
  fixture's inputs (`target_aum=5e5, price=99, adv=103000`): old `part = (5e5/99)/103000 ≈
  0.049023`; new `part = 5e5/103000 ≈ 4.854369`. `round_trip_cost_bps` scales with the new
  (100x-ish larger, price≈99x) part — the test re-derives `expected` from the SAME (now-corrected)
  formula the production code uses, so it stays a tautological/self-consistent hand-check, not a
  frozen golden; no separate digest to record.
- `tests/core/capacity_scorecard_test.cpp` `CapacityScorecard.MonotoneCrossesNearAnalytic`: analytic
  closed-form `analytic_aum = part_star * adv * price / 0.25` → `part_star * adv / 0.25` (drop the
  `* price` factor, `price=100` in this fixture — the old analytic capacity AUM was 100x the
  corrected one). Self-consistent re-derivation (not a frozen digest); test re-passes because the
  formula now matches the fixed production code exactly.
- `tests/core/capacity_vector_test.cpp` `CapacityVector.HighParticipationIsCapacityConstrained`:
  the fixed (larger, price-inclusive) participation makes the true capacity zero-crossing of the
  concentrated books ~100x SMALLER (crossing_new = crossing_old / price, price=100 in this
  fixture's panel) — both concentrated alphas' true crossings fell below the internal
  `compute_capacity_vector` grid's floor (`0.01*target_aum`), clamping both to the SAME grid-floor
  value (`999999.99999999953`) and breaking the `cap[3] < cap[1]` ordering assertion. Fix:
  `target_aum` 1e8 → 1e6 (empirically verified: re-centers the internal 0.01x-10x grid on the
  ~100x-smaller true crossings so both remain bracketed and distinguishable). The qualitative claim
  under test (concentrated-thin-ADV books are more capacity-constrained than diffuse ones) is
  unchanged; only the grid needed to be re-centered on the now-honest (smaller) numbers.

**No pinned golden digest moved.** No existing test in `tests/risk`, `tests/core`, or
`tests/factory` pins a byte-for-byte SHA/digest that reads `book_cost_bps`/`capacity_curve` output
at a nonzero AUM through the participation path (verified by full-suite run of
`atx-engine-risk-tests`, `atx-engine-core-tests`, `atx-engine-factory-tests` post-fix — all green).
If a downstream digest test is later found to have moved, it will be re-baselined here with this
unit's commit SHA as authority.

S4-2: complete [CORRECTNESS — B2]. Both S4-owned cap-clip-renorm sites now re-center after the
clip settles:
- `risk/optimizer.hpp` `PortfolioOptimizer::project`: after `cap_clip_renorm`, calls the new
  `recenter_after_clip(v, s, cap)` (gated: only when `cap < gross_leverage`, i.e. the SAME guard
  that gates `cap_clip_renorm` itself). `recenter_after_clip` alternates `demean_live` (a uniform
  shift over ALL live cells, driving `Σw` to exactly 0 each pass) with a full `cap_clip_renorm`
  re-settle (reclips any name the shift pushed back over the cap, restores `Σ|w|=L` via the
  unbound-only deficit-renorm), for a new fixed `kRecenterIters=32` passes (no early exit, §3.2).
  No-op when `dollar_neutral` is off (an explicit early return, in addition to `demean_live`'s own
  internal no-op).
- `loop/weight_policy.hpp` `to_target_weights`: after `truncate_renorm(dense_out)`, when
  `dollar_neutral` is true, calls the new `recenter_after_truncate(dense_out)` — the identical
  alternating-demean-then-resettle mechanism (`atx::core::stats::demean` + `truncate_renorm`,
  `kRecenterIters=32`). `dense_out` holds only LIVE cells already (no per-universe live mask needed,
  unlike the optimizer's `Scratch`).

**Why 32 iterations (not `kCapIters`/`kTruncateIters`=8):** each recenter pass is a CONTRACTION
(verified numerically — see below), converging geometrically at ~0.36x residual per pass on the
by-construction adversarial fixture; 8 passes only reaches ~2.2e-5 residual, 16 reaches ~5.9e-9, 24
reaches ~1.6e-12, 32 reaches float noise (~1e-16 — 3.6e-16 on the fixture below). Gated behind a
BINDING per-name cap (`name_cap < gross_leverage` / `truncation > 0.0`), which is opt-in
configuration, not the default (`OptimizerConfig::name_cap == gross_leverage == 1.0` by default —
the whole cap/recenter path never runs on the no-cap default path); cost is O(n) per pass, a bounded
cold-path multiple at rebalance cadence, not a hot-loop concern.

**RED (by-construction fixture, found by a small numeric search then hand-verified via a Python
transliteration of the exact algorithm):** 4-name book, `cap=0.30`, `L=1.0`, `dollar_neutral=true`,
raw targets `[0.40, 0.10, -0.35, -0.15]` (already `Σ=0`, `Σ|.|=1.0`, so demean+gross_normalize are
no-ops and the vector enters cap-clip-renorm unchanged). `0.40`/`-0.35` pin to `±0.30`; `0.10`/`-0.15`
stay unbound.
- `tests/risk/optimizer_recenter_test.cpp` `OptimizerRecenter.NeutralAfterClip` (drives the fixture
  through `PortfolioOptimizer::solve` at `risk_aversion=0`, the λ=0 pure-alpha path, so the smooth
  target is `demean(alpha)` exactly — `solve` calls `project()` 66 times total, so the buggy code's
  residual partially self-corrects across the 64-iteration outer loop, converging from a raw-fixture
  −0.08 to a smaller but still nonzero −0.0178): RED asserted `Σw≈0` (tol 1e-9), got
  `sum=-0.017768968192617307` (fails).
- `tests/core/weight_policy_recenter_test.cpp` `WeightPolicyRecenter.NeutralAfterTruncate` (a SINGLE
  `to_target_weights` call — one `truncate_renorm`, no outer loop): RED asserted `Σw≈0`, got
  `sum=-0.080000000000000043` exactly (matches the hand-derived single-pass value).

**GREEN:** both new suites pass after the fix. Full regression sweep, all green: `RiskOptimizer` (22
tests, incl. `NameCapNeverExceeded`/`CapBelowEqualWeightAllPinned`), `RiskConstraints` (35),
`RiskMultiHorizon` (16), `WeightPolicy` (22), `WeightPolicyNeutralize` (9, incl. the `Truncate_*`
cap tests), `WeightPolicyDecay` (4), `BacktestLoop`/`BacktestIntegration` (18). No existing test
encoded a specific post-clip net-exposure NUMBER as a pinned expectation (all pre-existing cap tests
assert cap/gross invariants, not net==0, since a bias requires the specific asymmetric-clip shape
this fix's fixture was constructed to hit) — **no golden re-baseline needed for S4-2.**

**Documented before/after (the spec's headline claim):** on the `weight_policy_recenter_test.cpp`
single-pass fixture, net exposure `−0.08 → 0.0` (well within the 1e-9 tolerance; float noise on the
`optimizer_recenter_test.cpp` full-solve fixture is `~1e-16`), while `max|w|=0.30` (== cap) and
`Σ|w|=1.0` (== L) hold throughout, matching the spec's "0.234 → 0.000" headline shape (this sprint's
own by-construction fixture, not literally the spec's illustrative number).

S4-3a: complete [CORRECTNESS — B3]. `exec/execution_sim.hpp` `emit_fill`: after composing
`fill_px = ref·(1+dir·slip)·(1+dir·temp)`, a `Limit` order now clamps `fill_px` to the order's
limit — buy `min(fill_px, limit)`, sell `max(fill_px, limit)` — before permanent impact / commission
/ the FillPayload are built. `Market` orders take no clamp path. Partial marketability (filling less
than the full requested size when the limit binds) remains the documented deferred LOB residual;
this fix only guarantees the fill PRICE never crosses the limit.

**RED (`tests/core/limit_fill_clamp_test.cpp`, new):** ref=limit=100.0 (boundary-marketable),
FixedBps slippage (25bps) + linear temp impact (Y=25, delta=1, sigma=1.0, part=1e-4) so the composed
cost is hand-computable and isolated from VolumeShare's nonlinearity: buy `fill_px=100.500625`
(RED: asserted `<=100.0`, fails), sell `fill_px=99.500625` (RED: asserted `>=100.0`, fails).

**GREEN:** both clamp to exactly 100.0. `ExecSim` (24 pre-existing tests, incl. the three
`LimitBuy_*`/`LimitSell_*` marketability tests) + all 4 new `LimitFillClamp` tests green — no
existing test pinned an exact through-limit price, so **no golden re-baseline needed.** A Market
order at the identical cost profile is confirmed byte-identical to the raw (unclamped) composition
(`100.500625`, `MarketOrder_UnaffectedByClamp`), and a non-binding limit (`limit=200`) leaves the
fill unperturbed (`NonBindingLimit_ClampIsNoOp`) — the clamp is provably inert off its binding case.

S4-3b: complete [CORRECTNESS — B4]. `loop/market.hpp` `Market::update_prices`: a new per-call
scratch mask `present_` (a `std::vector<bool>`, sized once at construction, `std::fill`-cleared each
call — no allocation) tracks which universe members appear in this slice; after the existing
mark/volume assignment loop, every index NOT marked present has `volumes_[idx]` zeroed. The mark is
deliberately left untouched for absent names (a persistent last-value MTM reference is a separate,
legitimate concern — `Market.UpdatePrices_InstrumentAbsentFromSlice_RetainsPriorMark` still passes
unmodified).

**RED (`tests/core/market_absent_volume_test.cpp`, new):** 2-name universe; slice 1 prices both (A
vol=1000, B vol=2000); slice 2 carries only A. `AbsentInstrument_BarVolumeZeroedNextSlice` RED:
asserted `bar_volume(B)==0.0`, got `2000` (stale, fails). A follow-on integration test drives a REAL
`ExecutionSimulator::settle_pending` order against B (still priced, mark unchanged) —
`AbsentInstrument_OrderNeverFillsOnStaleVolume` RED: asserted no fill, got a phantom 100-share fill
(the bug's real consequence — a delisted/halted name still "trades" on stale volume).

**GREEN:** both new tests pass. Full regression sweep green: `Market`/`MarketDeathTest` (13, incl.
`UpdatePrices_InstrumentAbsentFromSlice_RetainsPriorMark` — confirms the mark-persistence contract
is untouched), `ExecSim`/`LimitFillClamp` (18), `BacktestLoop`/`BacktestIntegration` (18, incl.
`Survivorship_DelistedTradesToFinalBar_NotRetroactivelyRemoved`). No existing test asserted a stale
absent-instrument volume as an expected value — **no golden re-baseline needed.**

**SEAM (Sprint 3, recorded per the spec's binding instruction — do NOT edit, S3-owned file):**
`atx-impl/src/stage_combine.cpp` carries the IDENTICAL participation bug at (confirmed at kickoff)
line 315: `const atx::f64 part_per_aum = abs_w / (price * adv);` inside the per-alpha capacity-bracket
loop (function computing the `C` cost-bracket constant, `cost_bps(aum) = C·aum^delta`). Exact fix
Sprint 3 should apply: `part_per_aum = abs_w / adv;` (drop `price` from the denominator) — dimensionally
identical to the `risk/capacity.hpp`/`factory/fitness.cpp` fix. Shared proof fixture: this ledger's
`capacity_participation_test.cpp` (single-name book, `aum=$1e6,|w|=1,price=$100,dollar-ADV=$1e6` ⇒
correct `part=1.0`, buggy `part=0.01`). **Note:** the spec's dependency map cited this bug at
`stage_combine.cpp:289,301` — at kickoff those exact lines are `gross_edge_bps` assembly and an
`n = min(insts, w.size())` clamp respectively (unrelated code shifted by prior p6/p7 commits); the
bug itself is real and unchanged in shape, just at line 315 in the current tree. This is an OPEN
dependency for Sprint 3 until landed.
