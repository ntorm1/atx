# Hot-Path Sprint Mid-Point Code Review — Correctness, Performance, Feature Gaps

**Date:** 2026-07-16
**Reviewed at:** `main` @ `5ba7fe4` (working tree clean)
**Sprint under review:** `sprints/2026-07-14-atx-vol-fit-price-backtest-hotpath-sota-sprint.md`
**Commits in scope:** `d3aa285` (pre-W0), `4b769dd` (W0), `f1dd590` (W1), `e7d5ebb` (W2), `c485081` (W3.1/W3.2), `52324e5` (W4.1/W4.4-partial)
**Method:** six parallel adversarial review agents (W3.1 boundary sharing, W3.2 selector, W4.1/W4.4 threading, W2 de-Am spot-check, W0/W1 spot-check, pipeline-connection gap analysis), every finding re-verified against HEAD source before inclusion. The headline pipeline finding (R-01) was independently re-verified by the dispatching session against `src/calib.cpp:653-656` and `src/session.cpp:711`.

---

## 1. Executive summary

The landed work is high quality: no P0s anywhere, the W2 de-Am mechanics (geometry hoist, reusable `AloPricer`, closed-form PCP borrow, trusted-accurate audit skip) all check out against the code and are genuinely test-pinned; the W4.1 concurrency core is correct, deterministic, and the nested-executor deadlock trap was *not* violated; the W1 SIMD wiring and bracket work are clean including tail/NaN patching in every case but one adversarial-magnitude corner.

Four P1s remain, and they cluster around one theme: **the optimizations are landed but not connected, and two acceptance gates were recorded as passed without being run or runnable.**

1. **R-01 — The W3.1 shared-boundary de-Am path is unreachable from both production routes**, and the handoff's diagnosis of why is imprecise. SPY does *not* go through `LegacyEssviCompatibility` prep — it prepares `Configured`, and the Hft preset's `max_otm_shortcut_premium_spread_frac = 0.50` trips the disable-gate inside `prepare_shared_boundary_proposals`. The eSSVI-published majority (63/94 boards on the 100-name row) bypasses it differently, via the hard-coded Legacy prep in `run_surface_parity`. Two distinct wiring fixes are required; neither is W3.3.
2. **R-02 — The W3.2 served-coverage floor is only enforced on the legacy mark path.** The v2/risk fit path admits exactly the MU failure mode (46%-coverage rebuild published as success) that W3.2 was built to kill.
3. **R-03 — W4.1's global queue now retains every fitted surface in memory until all fitting completes.** Peak RSS is O(all boards' surfaces) instead of O(one date); the still-pending throughput gate runs on exactly the workload that will expose this.
4. **R-04 — The W3.2 acceptance gate was never run, and as written it is unattainable by construction** (production CV can only shrink the ok-set relative to pinned eSSVI). The ledger marks W3.2 "complete" against it.

The single highest-leverage next edit is not W3.3 — it is the ~30-line R-01 fix that lets the already-landed, already-verified 1.78× shared-boundary machinery reach the 411.8 ms that is 84% of the SPY gate.

---

## 2. Consolidated findings index

| ID | Sev | Area | Location (HEAD) | One-line |
|---|---|---|---|---|
| R-01 | P1 | W3.1 wiring | `src/calib.cpp:653-656`, `src/session.cpp:711`, `src/surface_parity.cpp:257-258`, `src/pricer_fitter.cpp:1540-1542` | Shared-boundary de-Am unreachable from both production prep routes |
| R-02 | P1 | W3.2 | `src/pricer_fitter.cpp:1284-1292` vs `:640-643` | Served-coverage floor not applied on v2/risk fit path |
| R-03 | P1 | W4.1 | `src/surface_db_populate.cpp:168,190-264` | Peak memory O(all boards); fit-all-then-write-all |
| R-04 | P1 | process | sprint ledger W3.2 row | W3.2 acceptance gate unrun and unattainable as written |
| R-05 | P2 | W2.6/IV | `src/american_iv.cpp:270-305` | Bracket-cap clamp returns `Ok(kIvMin)` without evaluating the floor; default-on warm starts widen reachability |
| R-06 | P2 | W2.3 | `src/deamer.cpp:36` vs `:43` | Carry fixed-point tol 1e-8 sits below inner-solver jitter (1e-4) |
| R-07 | P2 | W3.1 | `src/calib.cpp:489-492` | Lane acceptance admits up to 2× the stated economic price budget |
| R-08 | P2 | W3.1 | `src/calib.cpp:429-430` | σ-monotonicity checked at bracket endpoints only; low-vega multi-root risk |
| R-09 | P2 | W3.1 | `src/calib.cpp:655` | Blanket `q_eff < 0` bail needlessly disables the put side on negative-carry boards |
| R-10 | P2 | W3.1 gap | `src/deamer.cpp:~108-135`, `src/american_iv.cpp:113-158` | Promised per-carry-leg `AloPricer` persistence absent; TLS slot thrashes call/put each FP iteration |
| R-11 | P2 | W3.1 perf | `src/calib.cpp:417-458,502`, `src/boundary_interp.cpp:247-262` | Root-finder residuals explain the 1.78× vs 2–4× shortfall |
| R-12 | P2 | W4.1 | `src/surface_db_populate.cpp:204-206` | One throwing board discards every completed fit; date-granular durability regressed |
| R-13 | P2 | W4.1 | `src/surface_db_populate.cpp:170-184` | No LPT ordering; late-claimed heavy board extends makespan by its full serial cost |
| R-14 | P2 | W4.1 | `src/surface_db_populate.cpp:188-198` | Core starvation when eligible boards < worker budget (all inner fits pinned to 1) |
| R-15 | P2 | W4.2-adjacent | `src/pricer_fitter.cpp:941-949` | Dual-output `std::async` fan-out unflattened under outer queue → up to 2× oversubscription |
| R-16 | P2 | W0.1 | `bench/compare_baseline.py:48-60`, `bench/e2e_hotpath_bench.cpp:488-501` | `fit/e2e/*` rows invisible to the baseline regression gate (both ratio and missing-name checks) |
| R-17 | P2 | W3.2 | `src/pricer_fitter.cpp:606-614` | Mark path hard-fails the board on selector failure; v2 path falls back — availability asymmetry |
| R-18 | P2 | W3.2 perf | `src/curve_selector.cpp:493-501` → `src/pricer_fitter.cpp:649,1209` | Selector's prepared slices discarded; sampled expiries de-Americanized twice per CV board |
| R-19 | P2 | cache | `src/snapshot_cache.cpp:24-43,80-95` | Snapshot cache key still `(path, tier)`; default cache never evicts → indefinite stale serve on same-path rewrite |
| R-20–R-35 | P3 | various | §5 | Sixteen P3 hardening/contract/test findings, tabulated below |

No P0 findings. The prior-review F-04 (`qp_active_set` Ok-on-exhaustion) is **fixed at HEAD** — `src/dense_slice.cpp:268-272` returns a KKT certificate and both callers reject non-converged as `Internal` (`:888-890, :926-929`), with `n_active` populated (`:130-139`). F-02 (partial-fit-as-success) is fixed at the admission layer but still present at the driver layer (see W3.4 gap, §6.3).

---

## 3. P1 findings in detail

### R-01 — Shared-boundary de-Am is unreachable from production (and the handoff misdiagnosed why)

**The verified SPY decision trace** (legacy single-surface branch, e2e bench config):

1. `select_fit_policy`: "SPY" hits the ticker seed table (`src/profile.cpp:252`) → `IndexEtfUltraLiquid` at confidence 0.95 ≥ `min_direct_confidence = 0.70` → direct route, **preset `Hft`, curve `LinearVariance`** (`src/fit_policy.cpp:30-33`; pinned by `tests/fit_policy_test.cpp:52-60`).
2. `make_session_inputs(Hft)` sets `in.calib.max_otm_shortcut_premium_spread_frac = 0.50` (`src/session.cpp:711`).
3. Non-Essvi curve → `VolaSession::build` dispatches to `fit_curve_surface` (`src/session.cpp:839-840`) with default `fit_prep_policy = Configured` → `prepare_configured` → `build_observations_european` (`src/prepared_fitting.cpp:211-216`).
4. `prepare_shared_boundary_proposals` early-returns because the gate excludes any board with the shortcut knob enabled:

```cpp
// src/calib.cpp:653-656
if (!opts.use_shared_boundary_deam || opts.audit_accurate_inversions ||
    opts.anchor_kind != CalibAnchorKind::Mid ||
    opts.max_otm_shortcut_premium_spread_frac > 0.0 || ...) { return; }
```

Every retained row falls to scalar `american_implied_vol` (`src/calib.cpp:896-902`). That is the 411.783 ms. Not rows-below-floor, not `NotFound`, not Legacy prep — a preset/config gate. (`published_override_boards=1` in fact *proves* SPY's published kind ≠ Essvi, i.e. it never took the Legacy-prep eSSVI driver.)

**The genuine Legacy bypass** applies to the eSSVI-published majority: `run_surface_parity` hard-codes `PreparedObservationPolicy::LegacyEssviCompatibility` (`src/surface_parity.cpp:257-258`), and `prepare_legacy` de-Americanizes per-row scalar via `european_equiv_iv` (`src/prepared_fitting.cpp:383-386`) — it never calls the shared-boundary code. Same for facade refit (`src/pricer_fitter.cpp:1540-1542`).

**Fix (two edits, both small):**

1. *Configured route (unblocks SPY/Hft):* compute the per-row OTM-shortcut mask first (`use_otm_shortcut_deam`, `src/calib.cpp:856`), run `prepare_shared_boundary_side` over the **non-shortcut subset only**, and remove `max_otm_shortcut_premium_spread_frac > 0.0` from the function-level gate. Leave the per-row priority order (`shortcut → shared_proposal → scalar`, `src/calib.cpp:897-902`) untouched; the existing sentinel certification (`src/calib.cpp:617-638`) still gates acceptance. ~30 lines in calib.cpp.
2. *Legacy/eSSVI route:* extract the shared-boundary lane solver into a helper taking `std::span<FitObs>` (it already operates only on `FitObs`) and invoke it from `prepare_legacy` after row population — or flip `run_surface_parity` to Configured prep behind the existing `SurfaceParityInputs::fit_prep_policy` knob (`surface_parity.hpp:192`) with a thin-slice legacy predicate retained.

**Expected effect:** SPY observation de-Am 411.8 → ~230 ms at the already-measured 1.78× → single-op gate ~492 → ~310 ms; the same mechanism then reaches the 100-name eSSVI majority. Verify against the §4 economic bound on the SPY gate plus the 25-name cohort before the 519-name run, per the handoff.

### R-02 — Served-coverage floor missing on the v2/risk fit path

Both branches of `PricerFitter::fit` (mutually exclusive, `src/pricer_fitter.cpp:475`) run CV via `select_curve`. The legacy mark branch tightens admission when a selector decision exists:

```cpp
// src/pricer_fitter.cpp:640-643
publication_admission = next_selection
    ? detail::selector_served_admission_policy(cfg_.admission, cfg_.selector)
    : cfg_.admission;
```

The v2/risk branch's `admission_attempt` lambda uses raw `cfg_.admission` (`src/pricer_fitter.cpp:1284-1292`) — no tightening — even for selector-chosen curves. Defaults make the hole live: `FitAdmissionPolicy::min_quote_coverage{0.0}` (`fit_policy.hpp:124`), and `risk_admission_policy()` only sets `min_expiry_coverage = 1.0` (`fit_policy.hpp:155-163`), which a 46%-of-keys-per-slice fit still satisfies. The exact MU failure — a CV-chosen family whose serving rebuild abstains on 54% of the board — is rejected on the mark path but **admitted on the risk path**, contradicting the header invariant at `curve_selector.hpp:128-131`.

**Fix:** before the `admission_attempt` lambda (`:1284`), compute the same tightened policy from `selection_` (reset at `:749`, set at `:1165` — no staleness):

```cpp
const FitAdmissionPolicy publication_admission =
    selection_.has_value()
        ? detail::selector_served_admission_policy(cfg_.admission, cfg_.selector)
        : cfg_.admission;
```

pass it through `completed_attempt_report` for the primary and every fallback rung, and add a v2-path analogue of `ServedCoverageFloorRejectsNarrowFamilySpecificRebuild`.

### R-03 — W4.1 peak memory is O(all boards' surfaces)

`std::vector<FitSlot> slots(n)` (`src/surface_db_populate.cpp:168`) spans every board across all dates; the scheduler joins globally before the first `write_partition` (fits `:190-203`, writes from `:208`), and each Ok slot owns a full `PricedSurface` (`corpus_board_fit.hpp:44`). Pre-commit code scoped slots per date. `with_uid` deep-clones each surface again during the write pass. On the sprint's target workloads (multi-year backfill, 519-name cohort) that is thousands-to-10⁵ live dense surfaces — plausibly GBs — and the pending throughput gate runs exactly there.

**Fix:** keep the single global queue; add a per-`DateRange` `std::atomic<std::size_t> remaining` decremented by each task after storing its slot, notify a condvar; the caller drains dates in ascending order, aggregating + writing + releasing (`slots[pos] = FitSlot{}`) each date as soon as its counter hits zero and all earlier dates are written. Write order stays date-asc/symbol-asc (deterministic), live memory bounds at the in-flight window, and date-granular durability returns (fixes R-12 too). Cheap interim mitigation: a `with_uid(PricedSurface&&, uid)` overload + move `slot.surface` during writes kills the transient double-hold, but not the peak.

### R-04 — W3.2 acceptance gate unrun and unattainable

The sprint gate reads: "selector ok-set ⊇ pinned-essvi ok-set OR CV disabled; fit CPU on CV boards drops ≥2×; no quality regression." Recorded evidence is only "focused Release tests pass"; no panel artifact exists, and `bench/fitting_throughput_bench.cpp` (touched in the same commit) contains zero selector references, so the ≥2× CPU claim also has no artifact. Structurally worse: production CV (a) still runs the CV machinery (so "OR CV disabled" doesn't apply), (b) requires 100% expiry and holdout coverage on the sampled expiries (`curve_selector.hpp:117-118` defaults, admission at `curve_selector.cpp:601-602`), and (c) adds the 0.50 served floor — all failure modes pinned eSSVI does not have, with no other families in the ladder. Hence `selector ok-set ⊆ pinned-essvi ok-set`, potentially strictly — the opposite inequality.

**Fix:** run the 100-name panel comparing HEAD production selector vs pinned eSSVI and record ok-set/coverage/CPU deltas, or amend the ledger gate to the invariant actually intended ("production CV ok-set = pinned-eSSVI ok-set minus boards failing the served floor; every exclusion lists its coverage") and note why the original inequality cannot hold. Until then the ledger's "complete" is unearned.

---

## 4. P2 findings in detail

### De-Americanization / Wave 2

**R-05 — IV bracket-cap clamp (pre-existing sprint finding, unfixed, now more reachable).** When `f(seed) > 0`, the down-bracket loop steps 7% at most 16 times (0.93¹⁶ ≈ 0.313×seed) and on exhaustion **above the floor** returns `Ok(kIvMin)` without ever evaluating the floor (`src/american_iv.cpp:285-287`); the symmetric up-loop returns `Err(OutOfRange)` at 9.36×seed (`:303-304`) for a perfectly invertible quote. W2.6 turned `warm_start_deam_adjacent_strikes` on by default (`calib.hpp:163`), and after `cap_observations_for_deam` thins a dense board, "adjacent" surviving rows can be far apart in moneyness — a smile dropping >3.2× across the gap silently yields `Ok(kIvMin)`. Blast radius today: calib rows are silently dropped by the strict band check (`calib.cpp:903`, fail-safe, but valid quotes lost and `drop_fraction` can trip certification); carry's `deam_pcp_step` accepts `Ok(0.005)` unchecked (`deamer.cpp:125-132`), biasing an iterate and then erroring the pair via the up-cap — low reachability while `warm_start_carry` defaults false, non-zero once enabled.
*Fix:* on down-loop exhaustion above the floor, evaluate `residual(kSigmaLo)`; if negative, set `xl = kSigmaLo; bracketed = true;` else return `Ok(kIvMin)` legitimately. Simplest structural form: replace both cap returns (`:287`, `:304`) with fall-through into the existing wide-bracket block at `:309-334` (≤2 extra solves, pathological path only). Additionally reject a warm seed further than ~2× from `euro_seed` when the latter exists.

**R-06 — Carry fixed-point tolerance inversion.** `kInnerIvTol` moved 1e-6 → 1e-4 (`deamer.cpp:36`) but `kBorrowFpTol` stayed 1e-8 (`:43`). Convergence to |Δb| < 1e-8 requires successive `deam_pcp_step` evaluations reproducible at 1e-8, yet each leg's IV is a 1e-4-terminated Newton whose value depends discontinuously on bracket-branch decisions; one branch flip moves `b_next` by ~1e-5 — a permanent limit cycle above the gate. Non-convergence is fail-safe (`Err(Unavailable)` → pair dropped) so this is availability/latency: a cycling pair burns the full 64×(2–4 AL solves) budget before failing. Warm-start seed determinism is what makes it converge in practice.
*Fix:* accept when final |Δb| < max(kBorrowFpTol, 1e-6) (still 100× inside the 1e-4 economic target and the loosened rmse_pcp ≤ 1e-4 contract), or detect a 2-cycle (|Δbₙ| ≈ |Δbₙ₋₁|, alternating sign) and exit converged at the midpoint.

### Shared boundary / W3.1 numerics

**R-07 — Acceptance gate admits 2× the stated budget.** `src/calib.cpp:489-492` allows the interpolated-map residual and the 9-vs-5 interpolation-error estimate up to `budget` *each*, so `|price_true(σ̂) − mid|` can approach 2×`budget`. In practice the 9-vs-5 gap over-estimates, and the fixture test passes the single budget — but the gate does not prove the documented bound. *Fix (one line):* `> 0.5 * budget` per term, or gate `fabs(price - mid) + fabs(price - embedded) > budget`.

**R-08 — Endpoint-only monotonicity check.** `lane.f_lo < 0 && lane.f_hi >= 0 && price_hi >= price_lo` (`calib.cpp:429-430`) is the entire shape check on a 9-node Chebyshev interpolant of the boundary; in near-zero-vega regions interpolation wiggle can produce multiple roots, bisection converges to *a* root, and both the certified price residual and the embedded 9-vs-5 gap derive from the same 9 solves (shared wiggle invisible). Sentinels sample strike ranks {first, middle, last}, not worst-case σ regions. The per-lane vega-scaled budget mostly saves the economics, but the sprint's unconditional "IV ≤ 1e-4" is not enforced per lane.
*Fix:* after `build()`, evaluate `price_internal_put` on a ~17-point σ refinement grid per side and require strict monotonicity (quadrature-only, one pass); or require `f_hi − f_lo ≥ vega_floor·(hi−lo)` before activating a lane. Fallback semantics unchanged.

**R-09 — `q_eff < 0` blanket bail.** The function-level gate (`calib.cpp:655`) disables both sides for slightly negative implied borrow — common on single names and exactly the 25-name recovery cohort's population. Only the *call* side (internal rate = q_eff) needs q_eff > 0; the put side under r > 0, q_eff < 0 is a regular single-boundary regime already guarded by the per-side `internal_rate > 0` check (`:600-601`) and `build()`'s xmax check. *Fix:* drop `q_eff < 0.0` from the function-level gate; per-side gates already cover the corners. Update the pinning test (`SharedSigmaBoundaryKeepsNegativeRatesOnScalarPath`) to assert put-side sharing survives.

**R-10 — Promised carry-leg AloPricer persistence not done.** The W3.1 sprint row's intermediate step ("persist one `AloPricer` per carry leg across FP iterations") is absent — c485081 does not touch `deamer.cpp`. Both legs of `deam_pcp_step` share the single TLS slot (`american_iv.cpp:113-158`), so each leg's `reset` clobbers the other's boundary every fixed-point iteration, with zero reuse even though only `q_eff` moves by a shrinking delta and `al_solve_put_boundary_warm` (`american_boundary.hpp:106-111`) exists for exactly this. Carry is ~56 ms of the 492 ms gate.
*Fix:* in `imply_term_borrow_from_base`, hold two stack `AloPricer`s (one per leg); rebind `q_eff` per iteration with a warm boundary seed (`pricer.reset_carry(q_eff)` → `al_solve_put_boundary_warm(K,T,σ,r,q', sch, prev_bnd, bnd, ws)`).

**R-11 — Root-finder residuals explain 1.78× vs the 2–4× estimate.** Three compounding costs:
(a) *Bisection to 1e-7:* `solve_tol = min(max(iv_tol,1e-9), 2.5e-5)` (`calib.cpp:502`) with Robust/Accurate `iv_tol=1e-7` → ~20–48 evals/lane under the 25%-shrink guard (`:452-458`, which degenerates to pure bisection once the root hugs an end); each eval is 12 barycentric interps + a full 48-node premium quadrature. The scalar path it replaced was ~4–8-eval safeguarded Newton. *Fix:* Illinois/Anderson–Björck modification (halve the retained endpoint's residual — ~3 lines in `iterate_shared_lanes`), or Newton with B76 vega as slope proxy → ~5–7 evals/lane.
(b) *No warm bracket from the adjacent strike:* lanes iterate in ascending-strike order but each starts from the full `[sigma_lo, sigma_mkt]` bracket (`calib.cpp:417-424`). Seed lane i's bracket as `[σ_{i-1}−δ, σ_{i-1}+δ]`, sign-validated with wide-bracket fallback.
(c) *Cold node solves in `build()`:* `boundary_interp.cpp:247-262` solves each of the 9 σ-nodes cold although `al_solve_put_boundary_warm` exists; chaining warm seeds across adjacent nodes roughly halves build cost (also benefits the pre-existing `slice_sigma_impl`).
Together these plausibly recover most of the 2–4× target. W5.5's AVX2 boundary batch (still gated off) is the step after.

### Threading / Wave 4

**R-12 — Exception discards all completed fits.** Ordinary fit failures stay non-fatal, but a worker exception (`bad_alloc` in `fit_board`, or the slot move) makes `run_bounded_fit_tasks` return `Internal` and populate returns at `surface_db_populate.cpp:204-206` having written zero partitions — hours of finished fits gone. Old code wrote each date immediately (crash left earlier dates durable under `skip_existing` resume). The R-03 streaming writer restores this.

**R-13 — No LPT ordering.** `fit_positions` claims in (date asc, symbol asc) order (`surface_db_populate.cpp:170-184`; queue claims in-order per `fit_scheduler.cpp:77`). An SPY-sized board claimed last — now pinned to `fit_workers=1` (`:197`) — extends the makespan by its full serial cost while 11 workers idle; this directly threatens the ≥6-effective-cores acceptance. Output order is fully decoupled from claim order (slots indexed by `pos`), so sorting claims is determinism-free. *Fix:* one `std::stable_sort` of `fit_positions` by descending `boards[order[pos]].frame` row count, tie-broken by `pos`.

**R-14 — Core starvation for small board counts.** `parallel_outer` is true whenever eligible boards > 1 and budget > 1, and then every fit is pinned to `fit_workers = 1` (`surface_db_populate.cpp:188-198`). With 2–4 boards on a 12-thread budget, 8–10 cores idle. `parallel_for`'s contract guarantees bit-identical results for any worker count, so a shared budget is determinism-safe. *Fix:* `pc.fit_workers = max(1, worker_budget / min(worker_budget, fit_positions.size()))`, or fold into W4.2/W4.5's shared-budget work.

**R-15 — Dual-output fan-out unflattened.** When a symbol's policy requests both MarketMark and Risk, each `fit_board` spawns a `std::async` mark-build thread regardless of `fit_workers=1` (`src/pricer_fitter.cpp:941-949`) — up to 2× the budget in concurrent threads under the 12-wide queue. This is the handoff's "flatten dual-output fan-out" item, now firing per-claimed-board. Not a deadlock (verified — see §7). Belongs with W4.2: run the mark build as a second task in the same bounded queue, or inline it when `parallel_outer`.

### Observability / selector / cache

**R-16 — `fit/e2e/*` rows invisible to the regression gate.** `compare_baseline.py`'s `aggregates()` keeps only `run_type == "aggregate"` rows (`bench/compare_baseline.py:48-60`); the corpus fit rows are `Iterations(1)` with no repetitions (`bench/e2e_hotpath_bench.cpp:488-501`) so they exist only as `iteration` rows — absent from *both* sides of every comparison, hence neither ratio-gated nor caught by the missing-benchmark fail-loud path. A crash or 10× regression in `fit/e2e/*` passes silently. The name-coverage CTest checks names only and duplicates them as CMake literals. *Fix:* in `aggregates()`, fall back to the iteration row's `real_time` (median-of-1) when a run_name has no aggregates; and/or derive the CTest `--required` list from the baseline JSON so the sources of truth cannot diverge.

**R-17 — Mark-path selector hard-fail.** On `select_curve` failure the mark path returns `Err` outright (`src/pricer_fitter.cpp:606-614`) while the v2 path falls back to `decision_->curve` (`:1166-1175`). With production's single eSSVI candidate and 1.0 coverage floors, one non-finite wing key or failed holdout American price zeroes the board (LKG retained). Pre-commit, four other families could satisfy admission. *Fix:* mirror v2 semantics — on `NotFound`/`Unavailable` (not `InvalidArgument`), proceed with `next_decision->curve` and record the selector error in the attempt report; consider `min_holdout_coverage = 0.98` in production so one bad key cannot zero a board.

**R-18 — Selector prep discarded before serving.** Within one `select_curve` call sharing is real (prep loop outside the candidate loop, `curve_selector.cpp:493-501`), but the `PreparedSlice`s die with the call and `VolaSession::build` re-prepares the whole board (`pricer_fitter.cpp:649`, `:1209`) — the sampled expiries are de-Americanized twice per CV board. With one production candidate, selector prep is now roughly half the CV overhead. The prep loop is also serial despite `sp.fit_workers` being copied in and never consumed. *Fix:* return the `PreparedExpiry` slices (or a keyed cache) in `SelectorResult` and let `VolaSession::build` accept pre-prepared slices for matching `(chain_index, calib, deam)` keys; at minimum parallelize `prepare_expiry` over `sampled_expiry_indices`.

**R-19 — Snapshot-cache staleness (pre-existing, confirmed at HEAD, now with a concrete fix).** Key is still `(lexically_normal(path), tier)` (`src/snapshot_cache.cpp:24-43`) and the default cache never evicts (`:80-95`), so a same-path rewrite serves the stale surface indefinitely. All four fields for a cheap identity exist in the fixed 464-byte archive header (`surface_archive.hpp:108-129`): `file_size`, `created_ts_ns`, `header_crc32c` (verified on open), `metadata_crc32c`. *Fix:*

```cpp
struct ArchiveIdentity {
  std::uint64_t file_size{}, created_ts_ns{};
  std::uint32_t header_crc32c{}, metadata_crc32c{};
  friend bool operator==(const ArchiveIdentity&, const ArchiveIdentity&) noexcept = default;
};
[[nodiscard]] Result<ArchiveIdentity> read_archive_identity(std::string_view path);
// pread first sizeof(ArchiveHeader) bytes; verify magic + header CRC;
// cross-check h.file_size == fs::file_size(path).
```

Add `identity` to `SnapshotCacheKey`; on identity-read failure bypass the cache so the real load error surfaces; eagerly erase superseded identities for the same path (the default cache never trims). Cost: one ~464-byte pread per load/prefetch. Regression test: write A to P, load; rewrite P with B (include the same-byte-length, different-`metadata_crc32c` case); load must return B (`stats().loads == 2`); byte-identical rewrite may hit. Steps 2–3 fail at HEAD.

---

## 5. P3 findings (compact)

| ID | Area | Location | Finding → fix |
|---|---|---|---|
| R-20 | W0.2 | `counters.hpp:328-357` | First event per thread always sampled (target init 0); fixed shared xorshift seed → identical phase across threads/processes; low-volume threads over-report up to 64×. Seed per thread (mix TLS address), roll target before first use. |
| R-21 | W0.2 | `counters.hpp:408-441` | Nested inversion under an *unsampled* outer counts as root → `american_iv_samples` inflated by ~nested×63/64. Gate sampling on a TLS depth counter, not the sampled-outer pointer. |
| R-22 | W1.1 | `simd/black76_batch_avx2.cpp:68-75`, `greeks_batch_avx2.cpp:83-90` | NaN `d1` from `F/K` underflow + σ²T overflow escapes ordered wing compares → finite garbage where scalar gives NaN (adversarial magnitudes only). OR an unordered self-compare into `patch_bits`. |
| R-23 | W1.1 | `src/batch.cpp:46-62` | Overlap rejection also rejects exact in==out aliasing the pre-W1 scalar batch supported (and which is safe even on the vector route). Permit identity aliasing or flag the break in release notes. |
| R-24 | W1.1 | `simd/iv_batch.cpp:41-44`, public header `simd/iv_batch.hpp:38-41` | `simd::implied_vol_batch` still dispatches AVX2 (the measured-slower route) while the span API is scalar — two public entry points with contradictory routing rationale. Route scalar or document. |
| R-25 | W1.5 | `corpus_board_fit.cpp:199-214` | `retain_consumed_fit_parity` silently overrides an *explicit* `score_parity=false` that is documented to fail closed. Only default when unset. |
| R-26 | W2.4 | `dividend.cpp:66-89` | Closed-form borrow ignores its `tol` parameter but still validates it; header doc says "endpoint-roundoff allowance". Honor as endpoint slack or deprecate. |
| R-27 | W2.3 | `deamer.hpp:241-245` | Default-constructed `DeAmOptions` silently moved all library callers to fast-AL/1e-4/5-pair carry. Documented and disableable, but carry is unaudited by design and pairs are |K−S|-selected — add a high-dividend fixture (F/S ≈ 0.90) pinning \|borrow − b_true\| ≤ 1e-4 fast-vs-accurate. |
| R-28 | W2.5 | `calib.cpp:914-927`, `american_iv.cpp:399-417` | Trusted-accurate skip converts a measured audit guarantee into an argued one; cold-polish early exits (vega ≤ 0, step past zero) skip residual re-verification at near-degenerate corners. Keep the audit when `o.vega < min_otm_shortcut_vega` or any polish early-exit fired. |
| R-29 | W2.6 | `calib.cpp:857-865` | `score_sigma_mkt` under Mid anchor + default-on warm start now aliases the fit solve (parent forced a cold scoring inversion). Document on `FitObs::score_sigma_mkt`; consumers of score-parity evidence should know. |
| R-30 | W2.1 | `american.cpp:698-700,739` | `geo_static_bound` revalidation is trust-based — no Debug check that retained geometry matches the current contract (same shape as the prior obs-23864 regression). Store a `{T,r,q,n,nq}` bind key under `!NDEBUG` and assert in `al_bind_geometry_sigma`; counter on the Release specialize-off fallback. |
| R-31 | W3.1 | `calib.cpp:393-406` vs `boundary_interp.cpp:126-130` | McDonald–Schroder internal-put mapping duplicated; drift risk. Single-source into `detail::SigmaBoundaryInterp::price_side(...)`. |
| R-32 | W3.1 | `calib.cpp:617` vs `:559-567` | `n_shared_boundary_solves` counts only the 9 build solves; sentinel inversions/reprices do additional internal boundary solves — "18 boundary solves" in the handoff undercounts real boundary work (~2×, still O(1)/side). Diagnostics-semantics caveat, document. |
| R-33 | W3.2 | `curve_selector.hpp:171-173` | `select_curve(..., const SelectorConfig &sel = {})` silently grants the unlimited research config to any caller that forgets the argument. Drop the default; update test call sites. |
| R-34 | W3.2 | `curve_selector.cpp:549-580,608-618` | chi²/RMSE tie-break populations are only common at coverage floors == 1.0; relaxed research floors reintroduce non-comparable sub-populations. Restrict tie-break to the intersection mask or document the constraint on `CandidateScore`. |
| R-35 | W3.2/W4.1 | `pricer_fitter.cpp:371-378`; `curve_selector.cpp:245`; `fit_scheduler.cpp:84-88`; `surface_db_populate.cpp:178-179,188`; `backtest.cpp:94-100,518,556` | Grab-bag, each small: coverage denominator counts never-fittable strikes (floor is conservative — quantify on the panel before tightening); selector hardcodes `Configured` prep while `sp.fit_prep_policy` is copied and dead; scheduler exception failures lose task index/board identity (build message on caller thread); `db.symbol_config` hard errors degrade to `cfg.fallback` (propagate non-NotFound); `worker_budget` unclamped vs `hardware_concurrency`; `reset_alive_scratch` exact-reserve defeats amortized growth + `prepare(alive_, ...)` self-alias fragility (assert or comment). |

---

## 6. Feature gaps — what needs building next

### 6.1 Ranked by expected end-to-end impact

Baseline: single-op SPY gate 492 ms = 469.8 fit / **411.8 obs de-Am** / 19.2 value. Curve solving is 0.15 ms/board. Only work that changes the preparation route or its inner loop moves the headline.

| # | Work | Size | Expected effect |
|---|---|---|---|
| 1 | **R-01 pipeline connection** (shortcut-mask + gate edit; legacy-prep wiring) | ~30 lines calib.cpp + one wiring decision | SPY ~492 → ~310 ms at the measured 1.78×; same mechanism reaches the 100-name eSSVI majority |
| 2 | **W3.3 per-slice Legacy fallback** (§6.2) | prepass + flag + report fields | Recovers the failed-board cohort (57.17 s of the 211 s 100-name row is failed/unreported fit wall) |
| 3 | **W3.4 remainder** (§6.3) | outcome taxonomy + hard-error propagation | Correctness/trust; prerequisite discipline for #2 |
| 4 | **R-11 root-finder upgrades** (Illinois step, warm brackets, warm node solves) | ~50 lines | Closes most of 1.78× → 2–4× on the now-connected path |
| 5 | **R-03/R-12/R-13 populate fixes** (streaming writes, LPT) + run the two owed benchmarks (§6.4) | moderate | Unblocks the W4.1 acceptance claim; bounds RSS |
| 6 | **W4.3 parallel + projected ingest** | needs an atx-core parquet projection API (none exists — `atx-core/src/io/parquet.cpp:143`); port list from unmerged `a28cea3` | ~min(cores,N)× on corpus load wall |
| 7 | **R-19 snapshot-cache identity** | small | Closes the stale-serve footgun |
| 8 | **W4.2 sibling fit pool** | new pool | 10–30% sustained backfill. Deadlock trap re-verified REAL at HEAD: `State::dispatch` holds `dispatch_mtx` across inline block-0 execution *and* the `cv_done` join (`pricing_executor.cpp:292-336`); the TLS guard (`:66`) is not inherited by `std::async` children (`:375-380`). Do NOT route fit fan-out into it unchanged. |
| 9 | **W5.5 → W5.4/W5.3 kernels** | simd/ | Only pays after #1 wires the batch's consumer. `kShipAvx2Boundary=false` confirmed (`simd/american_boundary_batch.cpp:54`, measured 1.6× < 2.0× gate); AVX2 IV still does 2 unconditional Halley steps + a 3rd evaluate (`simd/iv_batch_avx2.cpp:288-292`); scalar `kIvTol=1e-12` price-vs-vol-units bug intact (`implied_vol.cpp:178-181`, `types.hpp:68-72`); Φ still degree-48 Chebyshev + wing patch (`norm_cdf_cheb.hpp:25-32`). |
| 10 | **W4.5 measured small-book cutoff** | executor/backtest | Partially moot: executor already inlines `n < 4` with an in-file measured table showing dispatch wins at n=4 (`pricing_executor.cpp:32-60,366`). Measure `n={1..16}` before changing anything; the generic inner-`fit_workers` H² guard remains open. |
| 11 | **W5.1/W5.2 CStar** | isolated R&D | Confirmed at HEAD before any speed work: the c2 projection bisection keeps both bounds infeasible when the first midpoint is infeasible and finishes at an arb-violating `c2_hi`, and `cstar_arb_project` returns `Ok()` unconditionally with no post-projection validation (`cstar.cpp:599-612`); butterfly w'' still FD /1e-8 cancellation (`cstar.cpp:97-103`); `f'(z)` still central FD (`:406-425`); `MatX::Zero(dim,dim)` per LM iter (`cstar_calib.cpp:85-86`). |

### 6.2 W3.3 implementation proposal (per-slice Legacy fallback)

**Actual drop sites at HEAD** (sprint line numbers have drifted): the silent slot drop is `src/curve_fit.cpp:251-257` — `if (!prepared || prepared->fit_observations().size() < kMinPreparedFitRows) return;` **discards the error object**, so `NotFound` (genuinely thin) is indistinguishable from `Internal`/`InvalidArgument` (real defects). Row floors that produce `NotFound`: `calib.cpp:805-807` (`kMinObs=5`), `prepared_fitting.cpp:475-478` (legacy floor), `:620-622` (`prepare_expiry` floor). Whole-board consequence: all slices dropped → `Err(NotFound, "no expiry produced a usable slice")` (`curve_fit.cpp:569-571`); the product ladder then retries other curve families, but each rung re-runs identical Configured prep and starves identically — the "80% failure" mechanism.

**Error taxonomy** (atx-core `error.hpp:29-41`): fallback-eligible = `NotFound`, `Unavailable` (non-positive forward, audit-rejected rows); must-propagate = `InvalidArgument`, `Internal` (QP certification failure), `OutOfRange`, `Unknown`.

**Sketch** (anchor in `run_deam_prepass`; the eSSVI driver already runs Legacy):

```cpp
enum class SlicePrepOutcome : std::uint8_t {
  Prepared, PreparedLegacyRescue, Starved, CarryFailed, Failed };

// extend ChainPrepass (curve_fit.cpp:85):
SlicePrepOutcome outcome{SlicePrepOutcome::Starved};
std::optional<atx::core::Error> prep_error;   // retained, never discarded
std::uint32_t legacy_fit_rows{0}, legacy_audit_dropped{0};

[[nodiscard]] bool prep_error_is_expected(const atx::core::Error &e) noexcept {
  return e.code() == ErrorCode::NotFound || e.code() == ErrorCode::Unavailable;
}
```

Replace the drop at `curve_fit.cpp:251-257`: on hard error → `outcome = Failed`, retain error, return; on thin/expected-failure and a new `CalibOpts::per_slice_legacy_prep_fallback` (default off) → retry `PreparedSlice::create` with `policy = LegacyEssviCompatibility`, `audit_fit_inversions = true` (handoff: "explicitly audited"), diagnostics into `legacy_fit_rows`/`legacy_audit_dropped`; a hard error from the rescue also propagates; success ≥ floor → `PreparedLegacyRescue`; else truthful `Starved`. Both `create` calls timed into `ms_obs_eu` so `observation_deam_ms` stays honest.

Provenance mostly exists already: `SlicePreparationProvenance::policy` (`prepared_fitting.hpp:110-123`) is stamped by both builders and reaches the report; a rescued slice truthfully carries `LegacyEssviCompatibility` with a **default `DeAmAuditDiagnostics`** — certification must not claim Configured-grade de-Am for rescued slices. Extend `CurveSurfaceReport` (`curve_fit.hpp:68-87`) with `n_slices_legacy_rescued`, `n_slices_starved`, `std::vector<SliceBuildOutcome> slice_outcomes`. In phase 2 (`curve_fit.cpp:367-380`), a `Failed` slot makes `fit_curve_surface` return `Err` (propagating the retained hard error) instead of `continue` — that is what makes the product ladder run for real defects rather than converting them into missing coverage.

Gate per handoff: one-op SPY gate → 25-name recovery cohort (`CZR,RPRX,RXT,ROIV,HST,FTV,EQH,IBN,TSLQ,JHX,MNTS,OKLL,EQX,SIDU,HIMX,GFI,DGXX,VNET,ESI,BFAM,PCOR,HTHT,IBRX,ALHC,GGG`) → 519-name cohort.

### 6.3 W3.4 remainder (admission correctness)

Status at HEAD: **F-04 fixed** (QP KKT certificate + `Internal` on non-convergence + `n_active` populated — nothing left in `dense_slice.cpp`). **`SurfaceBuildReport` partially exists** (`pricer_fitter.cpp:380-411`, `fit_policy.hpp:165-193`) with two gaps: `ExpiryBuildOutcome` distinguishes only `Fitted`/`Missing` (no reasons), and default Mark policy (`min_fitted_expiries=1`, `min_expiry_coverage=0.0`) still publishes a 1-of-24 partial fit. **F-02 still present at the driver level**: `curve_fit.cpp:477-488` (`continue` on slice-fit failure, reason only to env-gated stderr) and `:569-571` (fails only at zero slices); `surface_parity.cpp:296-298`, `:322-324` same shape.

Proposal: widen the outcome enum —

```cpp
enum class ExpiryBuildOutcome : std::uint8_t {
  Fitted, FittedFallbackCurve, FittedLegacyPrep,
  CarryFailed, PrepStarved, PrepFailed, FitFailed, Missing };
struct ExpiryBuildReport {
  std::size_t chain_index; double maturity;
  ExpiryBuildOutcome outcome; std::size_t n_observations;
  atx::core::ErrorCode error{atx::core::ErrorCode::Unknown};  // for *Failed
};
```

populated from W3.3's `slice_outcomes` (so `completed_attempt_report` stops inferring "Missing" by maturity matching), plus a `SurfaceAdmissionReason::SliceHardFailure` fed by any `PrepFailed`/`FitFailed` carrying a hard code. A board that fit one easy expiry after a QP `Internal` on the rest then fails admission and walks the ladder, without changing the deliberate Mark-policy tolerance for genuinely thin expiries.

### 6.4 Owed acceptance benchmarks

1. **W4.1 throughput gate** (claimed pending, required before "complete"): real OPRA multi-date board set behind the 1.9-cores evidence, fresh DB per run; measure `wall(n_threads=1) / wall(n_threads=12)` (outputs deterministic → equal work); acceptance ratio ≥ 6. Record per-slot ms to compute utilization `Σ board_ms / (wall × 12)` and confirm the SPY tail is gone (R-13 shows up here if not). **Record peak RSS — it will expose R-03.** Evidence into `artifact-cache/` per house pattern.
2. **W4.4 allocation/latency gate** (required by handoff): counting `operator new` hook; fixed-book `run_backtest`, ~250 synthetic snapshots preloaded into a caller-supplied `cfg.snapshot_cache`, stable 64-lot book, no mid-run expiries, hedging off, `record_every_n=1`; assert allocations/step == 0 after a 10-step warmup. Second variant with trading + Daily hedge to baseline the known remaining allocations (`execute()`'s `uids` vector at `backtest.cpp:1325`; single-slot `RetainedBookPricer` thrash between pruned and post-trade books — the sprint's remaining 2-slot-cache work). Latency p50/p99 vs `52324e5^`.

### 6.5 Test additions

- Shared boundary: smile-stress fixture (σ ∈ [0.15, 0.8] across strikes) asserting economic bounds + fallback-lane behavior; failed-certification → `invalidate_shared_side` → results identical to scalar; per-lane embedded-gate rejection mid-side. Current bench fixture is flat 0.24-vol — it measures mechanics, not interpolation stress.
- Populate: ~12 boards / 3 dates (middle date pre-written for skip), one corrupted board, `n_threads=3`, assert bit-equal vs serial. Current test (4 boards / 4 threads) degenerates the dynamic-claim path.
- Selector: v2-path analogue of `ServedCoverageFloorRejectsNarrowFamilySpecificRebuild` (R-02).
- Cache: same-path rewrite regression (R-19), including the same-byte-length/different-CRC case.
- Carry: high-dividend (F/S ≈ 0.90) borrow-accuracy fixture, fast vs accurate preset (R-27).

---

## 7. Verified clean (coverage statement)

Things explicitly checked and found sound, so future reviews need not re-derive them:

- **W4.1 concurrency core:** exactly-once claim (`fetch_add`, exception caught inside the loop — no leaked claim, no wedged join); publication via join happens-before; deterministic write order decoupled from claim order (pre-sorted `order`, single-threaded post-join aggregation, `std::map` per-symbol, fixed FP accumulation order); serial vs 4-thread bit-equality test passes; `n_threads=0` serial semantics preserved. **The pricing-executor deadlock trap is not violated by 52324e5** — outer fan-out is raw jthreads; `fit_workers=1` makes inner paths fully inline; the `std::async` mark child can wait behind an independent dispatcher but never on its own waiting parent.
- **W4.4 retention safety:** dropping `workspace_ = PortfolioWorkspace{}` is sound — `ensure_prepared` invalidates on process-unique `logical_id_` + retime revision (ABA closed); the retained P&L base bundle is stamped with book id + revision + surface instance ids; scratch vectors resize to active size every call; nothing retains pointers into `alive_`/`key_` across steps; `same_book` compares full economic identity.
- **W2.1 geometry split:** `geo_zc`/`geo_weru`/`geo_wequ` verified genuinely σ-independent against the generic kernel bit-for-bit including the `u_eff` clamp; skip guards arithmetically identical across static bind, sigma bind, and specialized kernel — no uninitialized-read hole despite deliberately uninitialized 4×512 arrays; the accurate scheme (12,24) is on the hoisted path and pinned by `ResetAcrossContractsSidesAndSchemesMatchesFreshColdState` + `StaticGeometryExpCallsArePaidOncePerReset` (obs-23864 regression covered).
- **W2.2 TLS lease:** nested same-thread inversion gets an isolated heap fallback — never hands out the leased state twice; released on every exit path; cached/BAW routes bypass the slot (zero-alloc pinned by tests); `reset()` covers every field `price()` reads.
- **W2.4 closed-form borrow:** algebra verified against the old objective; sign conventions consistent; no hidden bisection fallback — tests exercise the closed form directly against an 80-iteration bisection reference across β ∈ {0, 0.4, 1}.
- **W2.5 skip condition:** reads the same `iv_tol`/`iv_max_iter` that drove the fit inversion (cannot read a different config); requires `route == Accurate` + no `al_opts` + no cache — fast presets and accurate *fallbacks* still cold-audit; both directions test-pinned.
- **W2.6 warm-start hygiene:** seeds strictly per-side and per-slice (function locals); updates only after full accept; deterministic ordering via the `(k, source_strike_index)` tie-break; no converged-value poisoning path except through R-05.
- **W3.1 numerics:** bracket mathematics (raw B76 IV of the American mid is a correct upper bracket via Am ≥ Eu); fallback exactness and determinism (rejected lanes → identical scalar branch in source order); no cross-call retained state (stack-local per side per call — staleness impossible by construction); thread-safety; embedded 9-vs-5 Lobatto grid math (even-index node identity, correct barycentric weights); edge cases (thin sides, near-expiry, degenerate σ-box, build failure, deep ITM); buffer bounds (`n_boundary ≤ 32` vs `series_` 32×16); allocation-free lane path.
- **W3.2 scoring:** common-population construction (frozen denominators; a candidate's failure only hurts itself); MU rejection by construction at two independent layers (selector floors at 1.0; tighten-only served floor `max(base, 0.50)` — 0.4600 rejected, 0.8722 admitted, no off-by-one); prepared-row sharing across heterogeneous candidates sound (prep depends only on board-level calib/deam); `Accum::reset()` covers all 12 fields; stratified sampling deterministic and regression-tested; production config reaches all fit paths and is test-pinned; `select_curve` stateless.
- **W1.1 SIMD:** tail handling (`i+4 ≤ n` + scalar tail) for all n; invalid-lane patching through the exact scalar kernel before stores; eSSVI parity operation-for-operation; `sqrt_t` sentinel double-guarded; all rejections surfaced as `[[nodiscard]] Status` (Python bindings raise).
- **W1.2 bracket:** mirror invalidation complete (`push`/`replace`/`clone` are the only mutators, exception-safe); bracket unforgeable (private + friend, re-validated defensively); exact-node collapse is an intentional documented improvement, numerically identical when both neighbors finite; `interp_forward` bit-identical to the old scan.
- **W1.4 term rates:** misclassification requires |Δr|·T ≤ 1e-12; non-finite/≤0 df fails safe to the term-rate branch.
- **W1.5 elision:** every consuming policy (Risk/Quote, bid/ask floors, qualified corpus, auto/eSSVI fallback, fast tiers) verified to retain or independently recompute its evidence; explicit-false + consuming policy fails closed; accuracy-panel retention (`config.score_parity = true`) confirmed at HEAD; calendar/strike-shape evidence comes from independent recomputation, not the elided pass.
- **W0 harness:** corpus rows genuinely one-operation-per-process (`Iterations(1)`, no warmup); name-coverage CTest + suffix-stripping regex correct and self-tested; W0.2 counters race-free (single relaxed `fetch_add` per publish, thread-owned TLS accumulator, constant-initialized `ThreadState` — no TLS guard on the hot path).

---

## 8. Sprint-ledger corrections

Record these in the sprint doc so the next agent inherits accurate state:

1. **Handoff §"What is proven now" bullet 3 is imprecise:** SPY publishes a non-eSSVI override and prepares under `Configured`; the shared-boundary bypass on SPY is the Hft `max_otm_shortcut_premium_spread_frac=0.50` config gate (`calib.cpp:653-656`), not `LegacyEssviCompatibility` prep. The Legacy bypass is real but applies to eSSVI-published boards.
2. **W3.2 ledger row over-credits c485081:** liquidity-stratified `sample_expiry_indices` and fixed-denominator common-population scoring predate the commit (present at `c485081~1`). The commit's actual content: prepared-row reuse, accumulator reuse, eSSVI-first ladder, `production_selector_config()`, budget→`Unavailable` semantics, `min_served_quote_coverage` floor + (mark-path-only) plumbing.
3. **W3.2 "complete" status:** the written acceptance gate was neither run nor attainable (R-04).
4. **"18 retained boundary solves":** the named counter excludes sentinel-internal boundary solves; real boundary work is ~2× that (still O(1) per side) — R-32.
5. **W2.6 side effect:** `score_sigma_mkt` under Mid anchor now aliases the fit solve rather than an independent inversion (R-29); consumers of score-parity evidence should know.
6. **time_budget_ms divergence** (sprint asked "enforce a real budget"): landed semantics — checked between candidates, armed after first scored candidate, fail-closed `Unavailable` on expiry, production bounded by candidate count instead of a deadline — are documented in `curve_selector.hpp:109-114` and test-pinned; judged reasonable. Residual sharp edge: one slow candidate can overrun any budget by its full cost.

---

*Review artifacts: six agent reports synthesized 2026-07-16; every file:line citation independently verified against HEAD `5ba7fe4` by the reporting agent, with the headline R-01 gate additionally re-verified by the dispatching session.*
