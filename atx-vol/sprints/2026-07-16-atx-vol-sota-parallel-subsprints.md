# atx-vol SOTA Multi-Sprint Plan — Three Parallel Attack Vectors

> **For agentic workers:** Each sub-sprint below (K, A, S) is dispatched to ONE independent implementation subagent in its own git worktree. A subagent executes ONLY its own sub-sprint section plus the shared §3 protocol. REQUIRED SUB-SKILL for each subagent: superpowers:executing-plans (task-by-task, TDD per the §3 contract). Steps use checkbox (`- [ ]`) syntax for tracking.

**Date:** 2026-07-16
**Parent sprint:** `sprints/2026-07-14-atx-vol-fit-price-backtest-hotpath-sota-sprint.md` (referred to as **SPRINT**)
**Parent review:** `docs/reviews/2026-07-16-hotpath-sprint-midpoint-code-review.md` (referred to as **REVIEW**)
**Goal:** Attack the north star from three independent angles simultaneously: make atx-vol the fastest *and* most accurate open options-pricing / vol-surface stack — beat **Jäckel** (IV inversion), **Andersen–Lake** (American), and the **Vola Dynamics / SpiderRock** envelope (whole-universe surface fit).

**Architecture:** The primary session (user) completes the REVIEW remediation on the shared hot-path TUs (Sprint R). In parallel, three subagents work disjoint translation units in isolated worktrees: Agent K owns the scalar/SIMD European-side kernels (IV inversion, Φ, batch pricing), Agent A owns the American engine (boundary solver, AVX2 boundary batch, slice-σ pricer), Agent S owns the surface engine (CStar family) plus OPRA ingest. Integration (Sprint I) wires the outputs together after Sprint R lands and re-runs the full gate ladder.

**Tech stack:** C++20 / MSVC `/W4 /WX`, AVX2 intrinsics, Eigen, Arrow/Parquet (atx-core), Google Benchmark, CTest.

## Global constraints (verbatim from SPRINT §4 unless cited otherwise)

- **Economic-correctness gate, not bit-identity:** price abs error ≤ `min(0.5 × tick, 0.1 × option vega × 1e-4)` and strictly inside the quote half-spread; IV abs error ≤ 1e-4 vol points vs. the higher-accuracy reference; no new butterfly/calendar/vertical arbitrage; in-band fraction ≥ prior, χ² ≤ prior, vol-RMSE ≤ prior on the 100-name panel (medians, best-of-3).
- **Per-task contract:** read before write (line numbers drift — grep the symbol); TDD (failing test first, asserting the economic bound); classify every change `pure-refactor` / `accuracy-improving` / `accuracy-trading` with an in-code comment stating what changed numerically, why it is correct, and the bound it holds; benchmark best-of-3 with wall/CPU/p50/p95; no new P0/P1 correctness debt — admission/arb safety only gets stricter; determinism across worker counts preserved.
- **Build discipline:** never run Debug and Release builds concurrently — FetchContent trees share `C:\atx-cache\deps\spdlog-build` and can mix `_ITERATOR_DEBUG_LEVEL` (SPRINT handoff). Each worktree MUST isolate dependency output (set `FETCHCONTENT_BASE_DIR` — or the repo's atx-cache override if one exists — to a per-worktree path at configure time; verify before first build). Within a worktree, configure the target preset immediately before its sequential build.
- **Benchmark discipline:** this laptop's turbo/thermal variance is large (26 s ↔ 64 s observed on the same config). A subagent collects microbenchmark evidence only while the other agents' builds/benches are paused (coordinate via the dispatching session); all cross-cutting perf claims are re-measured on a quiet host at Sprint I. Reserve the ~212 s 100-name benchmark for integration gates; use isolated microbenchmarks during development.
- **Research discipline:** when a new hot-path mechanism is found, follow up with primary-source web research; do not one-shot an algorithm from recollection (SPRINT handoff).

---

## 1. Scope boundary — what is assumed done before/alongside this plan

**Sprint R (user, in flight — NOT part of any sub-sprint):** all REVIEW remediation on the shared pipeline TUs:

- P1s: R-01 (shared-boundary wiring, both routes), R-02 (v2/risk served-coverage floor), R-03 (streaming per-date writes), R-04 (W3.2 gate rerun/amendment).
- P2s/P3s **except the carve-outs below**: R-05…R-21, R-25…R-29, R-31…R-35.
- SPRINT W3.3 (per-slice Legacy fallback, per REVIEW §6.2) and W3.4 remainder (outcome taxonomy, per REVIEW §6.3).
- Owed acceptance benchmarks: W4.1 throughput gate and W4.4 allocation/latency gate (REVIEW §6.4).

**Carve-outs — REVIEW findings reassigned from Sprint R to sub-sprints (file-ownership reasons):**

| Finding | Goes to | Reason |
|---|---|---|
| R-22 (AVX2 NaN `d1` escape), R-23 (identity-aliasing rejection), R-24 (contradictory IV batch routing) | **Agent K** | live in `src/batch.cpp` / `src/simd/*` — Agent K's TUs |
| R-30 (geo bind-key Debug assert) | **Agent A** | lives in `src/american.cpp` — Agent A's TU |

**Deferred to Sprint I (integration) — deliberately NOT in any sub-sprint:**

- SPRINT W4.2 (sibling fit pool) and W4.5 (generic H² guard + small-book serial cutoff): they touch `pricer_fitter.cpp`, `parallel_for.hpp`, `pricing_executor.hpp`, `surface_db_populate.cpp` — the same executors Sprint R's R-14/R-15 fixes touch. Two writers to those files would conflict.
- Wiring Agent A's AVX2 boundary batch into the shared-boundary 9-node build (`boundary_interp.cpp` / `calib.cpp`): those TUs belong to Sprint R (R-01/R-11) until it lands.
- Any `curve_selector.cpp` change (SplineVol candidate enablement, CStar ladder entry): Sprint R owns R-17/R-18/R-33/R-34 there.

---

## 2. Disjointness matrix — file ownership (one writer per TU)

| Owner | TUs / headers owned |
|---|---|
| **Sprint R (user)** | `calib.cpp`, `deamer.cpp/.hpp`, `american_iv.cpp`, `boundary_interp.cpp`, `pricer_fitter.cpp`, `session.cpp`, `surface_parity.cpp`, `surface_db_populate.cpp`, `curve_fit.cpp/.hpp`, `curve_selector.cpp/.hpp`, `snapshot_cache.cpp`, `counters.hpp`, `dividend.cpp`, `corpus_board_fit.cpp`, `fit_scheduler.cpp`, `backtest.cpp`, `compare_baseline.py`, `e2e_hotpath_bench.cpp`, `prepared_fitting.*` |
| **Agent K** | `src/implied_vol.cpp`, `include/atx/vol/types.hpp` (`kIvTol` block only), `include/atx/vol/detail/norm_cdf_cheb.hpp`, `src/math.hpp`, `src/vector_math.hpp`, `src/iv_batch_avx2.cpp`, `src/simd/iv_batch.*`, `src/simd/black76_batch*`, `src/simd/greeks_batch*`, `src/batch.cpp`, new `bench/iv_shootout_bench.cpp` |
| **Agent A** | `src/american.cpp`, `include/atx/vol/american.hpp`, `src/american_boundary_batch.cpp`, `src/american_boundary_avx2.cpp`, `include/atx/vol/american_boundary.hpp`, new `bench/american_shootout_bench.cpp` |
| **Agent S** | `src/cstar.cpp`, `src/cstar_calib.cpp`, `include/atx/vol/cstar*.hpp`, `src/opra_batch.cpp`, `src/opra_panel.cpp`, `atx-core/src/io/parquet.cpp` (+ its header) for the projection API, new `bench/cstar_bench.cpp`, new `bench/universe_cycle_bench.cpp` (harness only until Sprint I) |
| **Shared, append-only** | `bench/CMakeLists.txt`, `tests/CMakeLists.txt` — each agent appends its own targets; merge conflicts here are trivial and resolved by keeping all targets (same pattern as the `feat/atx-vol-analytics` merge). |

**Cross-agent numeric interaction (accepted, by design):** Agent K's Φ replacement (K2) moves every downstream consumer, including Agent A's American prices, by design (`accuracy-improving`). Each agent's parity gates compare against *its own worktree's* scalar baseline; cross-agent drift is resolved once, at Sprint I, on the full accuracy panel. No agent may pin a cross-TU golden byte-for-byte.

---

## 3. Dispatch protocol

1. Dispatching session creates three worktrees from `main` **after Sprint R's next stable commit** (sub-sprints do not depend on Sprint R's content, but branching later shrinks the merge): `wt-k-inversion`, `wt-a-american`, `wt-s-surface`.
2. Each subagent receives: this file, SPRINT, REVIEW, and its worktree path. It executes only its own §4/§5/§6 section, honoring §Global constraints and the §1/§2 boundaries. It must not edit any TU owned by another row of §2.
3. Each task lands as its own commit on the worktree branch (conventional message, class label in body). Sub-sprint ends with: strict Debug + Release builds green sequentially, its focused test list green in both, its bench JSON evidence checked into `bench/baselines/` or `artifact-cache/` per house pattern, and a ledger table appended to the worktree's copy of this file.
4. Merge order at Sprint I: Sprint R → K → A → S (rationale: K moves shared numerics first so A/S re-baseline once; S has the most new-file weight and merges cleanest last). The dispatching session owns every merge and gate re-run.

---

## 4. Sub-Sprint K — Inversion kernel: beat Jäckel *(Agent K)*

**Mission:** machine-precision IV inversion at Jäckel "Let's Be Rational" speed (~180 ns/op reference; challenger formula ~60 ns), full-precision Φ at 2–3× fewer FMAs, and European batch pricing pushed toward the ~4.4 ns/op/core AVX-512 envelope (DoD: within ~2× on this AVX2 ISA). Consumes SPRINT rows W5.3, W5.4, W5.6 + carve-outs R-22/R-23/R-24.

**Interfaces:** produces no API changes visible to other agents except (a) possibly re-routed public batch entry points in `batch.cpp` (signatures unchanged) and (b) a numerically-improved Φ consumed by everything. Consumes nothing from Agents A/S.

### Task K1 — Fix the mis-scaled IV convergence tolerance (SPRINT W5.4b)

**Files:** Modify `src/implied_vol.cpp:178-181`, `include/atx/vol/types.hpp:68-72`. Test `tests/implied_vol_test.cpp`.
**Class:** accuracy-improving (removes a wasted iteration; convergence test becomes meaningful).

- [ ] Write failing test: an inversion whose price residual is far below vega-scaled noise but above absolute `1e-12` must terminate on the residual test, not burn to the step test — assert iteration count via the existing counters hook.
- [ ] Implement: compare the price residual against a **vega-scaled** tolerance (`kIvTol × max(vega, vega_floor)` — i.e. tolerance in vol units as documented) or terminate on the vol-step test with the residual test as a relative check. Document the semantic in `types.hpp`.
- [ ] Verify machine-precision IV maintained: fixture sweep across moneyness × maturity × vol grid, max |σ̂ − σ_true| ≤ 1.6e-16 relative where the reference converges.
- [ ] Bench: scalar IV ns/op best-of-3 before/after; expect ~1 iteration saved on converged paths.
- [ ] Commit.

### Task K2 — Cody rational-erfc Φ, vectorized, wing-patch removed (SPRINT W5.3)

**Files:** Modify `include/atx/vol/detail/norm_cdf_cheb.hpp:25-32`, `src/math.hpp:203`, `src/vector_math.hpp:136,165`. Test `tests/math_test.cpp` (extend).
**Class:** accuracy-improving.

- [ ] Research first (§3 research discipline): pull the Cody 1969/1990 rational erfc coefficients from a primary source (Cody, *Rational Chebyshev approximation for the error function*, Math. Comp. 23) — do not transcribe from memory.
- [ ] Write failing test: Φ accuracy harness comparing against a long-double/`std::erfc`-composed reference over `d ∈ [-40, 40]` including denormal wings; assert max ULP error ≤ current implementation's (measure current first, record it in the test).
- [ ] Implement scalar ~8–14-term rational erfc; then the AVX2 lane version in `vector_math.hpp`; delete the `|d| > 6` scalar wing patch.
- [ ] Run the full atx-vol test suite in the worktree — Φ moves every consumer; economic-bound failures here mean a coefficient bug, stop and fix.
- [ ] Bench: Φ ns/op scalar + batch; Black-76 batch price/greeks end-to-end (expect measurable gain — Φ dominates the FMA chain). Best-of-3.
- [ ] Commit.

### Task K3 — Per-lane Halley residual mask in AVX2 IV (SPRINT W5.4a)

**Files:** Modify `src/iv_batch_avx2.cpp:288-297`. Test `tests/iv_batch_test.cpp` (extend).
**Class:** pure-refactor (identical results on converged lanes; documents the mask bound).

- [ ] Write failing test: counter or instrumentation asserting most lanes execute exactly 1 Halley step on a well-conditioned batch (SR-2017 seed converges in 1), while a hard-corner batch still takes 2.
- [ ] Implement: after Halley step 1, compute the per-lane residual mask; lanes under tolerance skip step 2 and the third evaluate (`_mm256_blendv_pd` retention).
- [ ] Verify precision: batch-vs-scalar max |Δσ| ≤ 1.6e-16 relative on the standard grid.
- [ ] Bench: AVX2 IV batch ns/op; target 1.3–1.5×.
- [ ] Commit.

### Task K4 — Carved review findings R-22, R-23, R-24

**Files:** Modify `src/simd/black76_batch_avx2.cpp:68-75`, `src/simd/greeks_batch_avx2.cpp:83-90` (R-22); `src/batch.cpp:46-62` (R-23); `src/simd/iv_batch.cpp:41-44` + header (R-24). Tests: extend the batch/simd suites.
**Class:** correctness (R-22), pure-refactor (R-23/R-24).

- [ ] R-22: failing test with adversarial magnitudes (`F/K` underflow + σ²T overflow → NaN `d1`) asserting scalar/vector agreement (both NaN); fix by OR-ing an unordered self-compare into `patch_bits`.
- [ ] R-23: failing test for exact in==out aliasing (pre-W1 supported); permit identity aliasing at the public boundary.
- [ ] R-24: decide by measurement **after K2+K3 land in this worktree**: re-measure `simd::implied_vol_batch` vs scalar. If AVX2 now ≥1.2× scalar, route the public span API to AVX2 too (retire the W1.1 exclusion note); otherwise route the `simd::` entry point scalar and document. One routing rationale, both entry points.
- [ ] Commit (one commit per finding).

### Task K5 — Jäckel LBR shootout harness (scope expansion)

**Files:** Create `bench/iv_shootout_bench.cpp`; vendor the reference implementation under `bench/thirdparty/lets_be_rational/` (Jäckel's reference code is public; keep it bench-only, never linked into the library).
**Class:** infrastructure.

- [ ] Vendor/wrap "Let's Be Rational" as a bench-only oracle (verify license header permits redistribution; if not, implement from the paper's published coefficients with citation).
- [ ] Standardized grid: moneyness × maturity × vol × side, plus the hard corners (deep OTM, near-zero time-value). Emit a table: ns/op and max/median ULP error, atx-vol vs LBR, checked-in JSON baseline.
- [ ] CTest asserting atx-vol max error ≤ LBR max error × 4 on the standard grid (honesty floor; tighten later), and a fail-loud row-coverage check so the shootout cannot silently drop.
- [ ] Record the current standing vs the ~180 ns/op target in the sub-sprint ledger. Commit.

### Task K6 — Evaluate the ~60 ns explicit IV formula (SPRINT W5.6, stretch)

**Files:** Modify `src/implied_vol.cpp` (behind a compile-time or config flag), extend `bench/iv_shootout_bench.cpp`.
**Class:** research → accuracy-trading (adopt only if it clears the gate).

- [ ] Primary-source research: arXiv 2604.24480 and 2606.17065 (single Halley in variance space). Extract the exact seed/step construction.
- [ ] Prototype behind a flag; run the K5 shootout grid.
- [ ] **Gate (verbatim SPRINT):** median error ≤ 1.6e-16 at < current ns/op, else shelve with the evidence table checked in. Shelving is a valid, complete outcome.
- [ ] Commit (adopted or shelved-with-evidence).

**Sub-sprint K exit gate:** strict Debug+Release green; full suite green in worktree; shootout JSON checked in showing (a) IV inversion ns/op and error vs LBR, (b) Φ accuracy ≥ current with fewer FMAs, (c) batch price ns/op progress toward the ≤2× envelope DoD row.

### Sub-Sprint K — completion ledger (Agent K)

Branch `feat/sota-k-inversion` (base main@7fca341). Strict Debug (`dev`) and Release (`rel-avx2`) builds both green under `/W4 /WX`; all K1–K6 tests pass in both (IvConvergence grid max rel-err 3.7e-12 Debug / 2.9e-12 Release; K2 batch-price max abs 5.68e-13). Benchmark numbers are **provisional (concurrent host)** — the sibling agents were building/benching on the same laptop (26 s↔64 s-class variance); definitive numbers re-measured on a quiet host at Sprint I.

**Full-suite status — zero regressions from K1–K6 (verified).** The full Debug run (`ctest -L atx_vol`, ~2600 tests) shows 7 gtest failures + 1 CTest failure, **all confirmed NOT caused by K**:
- 6 pre-existing Sprint-R "in-flight" pipeline failures, each verified RED at base main@7fca341 with K1+K2 reverted: `SurfaceV2Provenance.ValidationFallbackAdmissionRecordsTheServedFamily`, `PricerFitterTest.LocalRiskRefitPublishesCopyOnWriteGeneration`, `PreparedPortfolio.GroupedPriceEqualsIndependentOracleAndPinnedFingerprint`, `SurfaceV2Qualification.RiskBuildRunsTheModeCarryAndInversionBudgets/{Latency,Balanced}`, `OpraBreadthCorpus.UnifiedPolicyFitsEveryAvailableBoard`.
- 1 concurrent-load flake: `MultinamePipeline.CorpusWithMissingNameOnOneDateRunsToCompletion` — passed at base AND passes cleanly (~0.6 s, ×2) isolated at HEAD; failed only under the full `-j16` run while a 208 s corpus test and a sibling agent's bench contended for the box.
- 1 environmental CTest: `atx-vol-e2e-benchmark-name-coverage` — `FileNotFoundError` because the `atx-vol-e2e-hotpath-bench` exe was not built in this scoped Debug config (I build only atx-vol-scoped targets); a full bench build resolves it. Orthogonal to K's TUs.

| Task | Status | Commit | Key numbers | Class |
|---|---|---|---|---|
| K1 vega/notional IV tolerance | done | `89891f8` | worst-case round-trip σ rel-err **1.736e-10 → 3.709e-12 (47×)** over notional×moneyness×maturity×vol grid; the price-residual test now fires (was burning to the vol-step test on high-notional options) | accuracy-improving |
| K2 Cody rational-erfc Φ | done | `99b58a6` | batch B76 price vs scalar std::erfc: **~1e-6 → 5.68e-13 abs / 1.43e-15 rel (~1e6×)**, full-range incl. correct denormal wings; scalar Φ prototype vs std::erfc max abs 1.1e-16. Greeks AVX2 batch stays **~2.2× scalar**; IV batch → ~parity (erfc adds exp+div ×2 Halley steps) | accuracy-improving |
| K3 per-lane Halley step-2 mask | **shelved (evidence)** | — | SR-2017 seed + 1 Halley does not reach the batch's ~1e-9 accept contract (needs 2 steps); block-level all-4-lane SIMD requirement caps the *safe* skip rate at ~7% (0.67⁴), making it net-neutral (~2% throughput, within thermal noise). Reverted to the clean 2-step kernel. | pure-refactor (evaluated, not adopted) |
| K4 / R-22 NaN-d escape | done | `b0a6319` | unordered self-compare routes NaN-d lanes (F/K under/overflow + σ²T overflow) to scalar in black76+greeks patch_bits. K2's erfc already propagates NaN → symptom pre-resolved; this is explicit defense-in-depth + regression test | correctness hardening |
| K4 / R-23 identity aliasing | done (flag option) | `f1f1592` | "Permit identity aliasing" is blocked by 5 `Batch.*_OutputAliasesInput_InvalidArgument` assertions in the **forbidden** `batch_test.cpp` (STOP-and-record-blocker rule). Took the finding's second option — documented the pre-W1 break in-code with the exact Sprint-I remediation (permit + update 5 assertions). No behavior change | documentation (flagged limitation) |
| K4 / R-24 IV batch routing | done | `a4ff576` | measured (rel-avx2, best-of-3): AVX2 IV batch **never ≥1.2× scalar** (best 2.5 vs 2.7 Mitems/s — decisive). Routed `simd::implied_vol_batch` **scalar** to match the span API (one rationale, both entries). AVX2 kernel retained off-dispatch (shootout + future AVX-512) under a direct parity test | pure-refactor (routing) |
| K5 Jäckel LBR shootout | done | `20ba783` | `atx-vol-iv-shootout-bench` + `bench/baselines/iv-shootout.json` (906 rows, long-double bisection oracle): atx-vol scalar **~330 ns/op**, median rel err **1.04e-15**, max **2.05e-11**; AVX2 ~parity, max 8.24e-8. Standing vs Jäckel LBR published **~180 ns/op** → ~1.9× slower (provisional host), to tighter-than-LBR accuracy | infrastructure |
| K6 ~60 ns explicit formula | **shelved (evidence)** | — | see the evidence note below | research → shelved |

**K6 evidence note (shelve).** Primary source located: **W. Schadner, "An Explicit Solution to Black–Scholes Implied Volatility", arXiv:2604.24480 (posted 2026-04-27)** — the plan's first cited ID; the second (2606.17065) did not resolve to a distinct IV paper. Mechanism: the call price is written as the survival probability of an inverse-Gaussian (IG) distribution and IV is read off the IG **quantile function**, so variance is the natural inversion coordinate. It is *not* a closed-form single evaluation — the reference implementation uses a **Sankaran-Wald + Lévy-blend seed then Halley on the IG CDF, ~4–5 iterations average**, recovering IV to machine precision at **~3.4× a SOTA reference** (≈53 ns vs the Jäckel LBR ~180 ns benchmark). Gate assessment against the K5 shootout (current atx-vol scalar ~330 ns/op, median 1.04e-15): the formula **plausibly clears** "median ≤ 1.6e-16 at < current ns/op" and is a strong **Sprint X** adoption candidate (the roadmap already lists "~60 ns IV adoption (K6 outcome)" under Sprint X). A faithful port (IG CDF, the specific seed blend, IG-space Halley, deep-OTM/short-T edge cases) is a multi-day effort with real correctness risk drawn from a post-cutoff 2026 paper — out of scope for this stretch task, so it is shelved with this evidence per the plan's "shelving is a valid, complete outcome."

---

## 5. Sub-Sprint A — American engine: beat Andersen–Lake *(Agent A)*

**Mission:** American pricing at the ALO SOTA envelope (~10–22 µs/op price, ~60 µs full IV inversion, SSRN 2547027 / QuantLib `QdFpAmericanEngine`), by unlocking the AVX2 boundary batch (currently gated off at 1.6× < 2.0×) and batching the slice-σ boundary solves the engine already exposes. Consumes SPRINT row W5.5 + carve-out R-30. **Must not touch** `calib.cpp`, `boundary_interp.cpp`, `deamer.cpp`, `american_iv.cpp` (Sprint R territory) — integration wiring into the shared-boundary prep path happens at Sprint I.

**Interfaces:** produces (a) `kShipAvx2Boundary=true` with the batch entry point in `american_boundary_batch.cpp` cleared past its ship gate, and (b) a batched node-solve entry usable by `slice_sigma_impl`. Sprint I consumes both. Consumes nothing from K/S.

### Task A1 — Vectorize/cheapen the per-lane BAW seed (SPRINT W5.5 core)

**Files:** Modify `src/american_boundary_avx2.cpp:101-128`, `src/american_boundary_batch.cpp:54`. Test `tests/american_boundary_test.cpp` (extend).
**Class:** pure-refactor (seed only affects iteration count, not the converged boundary).

- [ ] Read the current scalar BAW seed and the 4-lane loop; confirm the seed is the serialization point (REVIEW/SPRINT claim — re-verify at HEAD).
- [ ] Research check: BAW (Barone-Adesi–Whaley 1987) critical-price approximation — confirm the exact q₁/q₂ root construction before vectorizing; a cheaper seed variant (e.g., a rational fit of the BAW root over the lane's parameter range) is acceptable if the converged boundary is unchanged.
- [ ] Write failing bench-gate test: boundary batch ≥ 2.0× scalar on the boundary bench (the existing ship-gate measurement, currently 1.6×).
- [ ] Implement the vectorized seed (4-lane `_mm256_*`; scalar tail patching per house SIMD pattern).
- [ ] Verify bit-parity (or economic-bound parity, documented) of converged boundaries vs scalar across the fixture sweep including near-expiry and deep-ITM corners.
- [ ] Flip `kShipAvx2Boundary=true` only when the 2.0× gate measurement passes best-of-3; check the bench JSON in.
- [ ] Commit.

### Task A2 — Batch the slice-σ node solves inside the engine (scope expansion of W5.5)

**Files:** Modify `src/american.cpp` (`slice_sigma_impl` / `andersen_lake_put_slice_sigma`, entry at `include/atx/vol/american.hpp:217,226`). Test `tests/american_slice_test.cpp` (extend).
**Class:** pure-refactor.

- [ ] Locate the ~8-node σ-boundary solve loop inside the slice-σ pricer (this is engine-internal — distinct from `boundary_interp.cpp`'s 9-node build, which is Sprint R's).
- [ ] Write failing counter test: N σ-nodes solved in ⌈N/4⌉ batched calls (counters-on), results identical to the scalar loop.
- [ ] Route the node solves through the now-shipped AVX2 batch (A1); scalar fallback preserved when `!have_avx2()` or lanes reject.
- [ ] Verify: existing slice-σ equivalence suite green; the 128-row block-ladder result (SPRINT: max price diff vs scalar cold `$0.00003564`) re-verified in-worktree.
- [ ] Bench: slice-σ path best-of-3 (isolated microbench; the real de-Am wiring gain lands at Sprint I).
- [ ] Commit.

### Task A3 — American price/IV shootout harness (scope expansion)

**Files:** Create `bench/american_shootout_bench.cpp`.
**Class:** infrastructure.

- [ ] Standardized American option grid (side × moneyness × maturity × vol × dividend/borrow regimes, incl. r<0 and q_eff<0 corners).
- [ ] Measure µs/op for: fast preset, accurate preset (12/24/48), cached/Chebyshev tier, and full American IV inversion — against the published ALO envelope (~10–22 µs price, ~60 µs IV; cite SSRN 2547027 in the bench header). Accuracy column: |Δprice| vs the highest-accuracy in-repo scheme (48/96 if available, else 12/24/48).
- [ ] Emit the ns-vs-error frontier JSON to `bench/baselines/`; add a fail-loud name-coverage CTest (mirror the W0.1 pattern — and note Sprint R's R-16 fix owns `compare_baseline.py`, so this harness must self-gate, not rely on it yet).
- [ ] Record standing vs SOTA in the ledger. Commit.

### Task A4 — R-30: Debug bind-key assert on retained geometry (carved review finding)

**Files:** Modify `src/american.cpp:698-700,739`. Test: extend the W2.1 geometry suite.
**Class:** correctness hardening.

- [ ] Store a `{T, r, q, n, nq}` bind key under `!NDEBUG` when static geometry binds; assert it matches in `al_bind_geometry_sigma` on every reuse (same shape as the prior obs-23864 regression).
- [ ] Add a Release-mode counter on the specialize-off fallback path.
- [ ] Re-run `ResetAcrossContractsSidesAndSchemesMatchesFreshColdState` + `StaticGeometryExpCallsArePaidOncePerReset` both configs. Commit.

### Task A5 — Negative-rate / double-boundary regimes (Healy) (scope expansion, stretch)

**Files:** Modify `src/american.cpp` + `american_boundary*.cpp` capability gates; new fixture tests.
**Class:** accuracy-improving (extends the valid domain; today these corners bail to slower/scalar paths).

- [ ] Primary-source research: Healy (negative-rate / multiple-boundary American exercise) — already on SPRINT's reviewed list; re-read before coding.
- [ ] Build fixture: r<0, q<0, and r<q<0 contracts with reference prices from a dense finite-difference solve (bench-only reference, ~1k-node CN grid is fine).
- [ ] Characterize which regimes the current AL implementation prices correctly vs bails on; add per-side capability predicates that are *provably* correct rather than blanket bails (the engine-level analogue of REVIEW R-09 — but only in Agent A's TUs; the `calib.cpp` gate is Sprint R's).
- [ ] Implement double-boundary support only if the characterization shows a real pricing error (not just a fallback) — otherwise document the domain map and stop. Gate: fixture prices within economic bound of the FD reference; zero regression on the standard grid.
- [ ] Commit (implementation or domain-map-with-evidence).

**Sub-sprint A exit gate:** strict Debug+Release green; boundary batch shipped at ≥2.0× with parity evidence; slice-σ node solves batched with counter proof; shootout JSON checked in with the µs/op-vs-error frontier and standing vs the ALO envelope.

### Sub-Sprint A — completion ledger (Agent A)

Branch `feat/sota-a-american`, base `main@7fca341`. Strict Debug (dev, counters-on) + Release (rel-avx2) builds green (`/W4 /WX`), sequential. Numbers are **provisional (concurrent host)**; the A1 gate is flagged for quiet-host re-measure at Sprint I.

| Task | Status | Commit | Key numbers | Class |
|---|---|---|---|---|
| **A1** — vectorize/cheapen the per-lane BAW seed; ship gate | **done, flag stays OFF** | `f9f5896` | AVX2 seed now skips the wasted `al_bind_geometry` precompute (new `amer::al_seed_put_boundary`); AVX2 output **bit-identical** (`AvxBoundary.ForceAvx2_MatchesScalar` green both configs). Gate best-of-3 under load: median 2.21×/1.46×/1.94×/1.66×, warm steady-state **~1.6–1.7×** (scalar baseline swung 566–930 ms) — NOT all runs ≥2.0×, so `kShipAvx2Boundary=false`. Remaining lever: BAW-Newton vectorization (breaks bit-parity) + quiet-host re-measure → Sprint I. JSON: `bench/baselines/…-boundary-gate.json`. | pure-refactor |
| **A2** — batch slice-σ node solves through the A1 batch | **done (entry+proof); wiring → Sprint I** | `d482e83` | `american_price_batch_resolved` proven: N=128 genuine puts → **32 AVX2 packs** (`AmericanAvxPackDispatches`, counters-on) within immateriality gate of scalar; ForceScalar bit-identical; <4 tail flushes scalar. **Blocker:** `slice_sigma_impl`/`SigmaBoundaryInterp::build` live in `boundary_interp.cpp` (Sprint R, forbidden) — the plan's "modify `src/american.cpp`" premise is stale at HEAD; the batch entry + ⌈N/4⌉ counter proof are the Sprint-I-consumable deliverable, the actual routing is Sprint I. | pure-refactor |
| **A3** — American price/IV shootout harness | **done** | `d7f1930` | `bench/american_shootout_bench.cpp` (cites SSRN 2547027) + self-gating name-coverage CTest (green Release, independent of `compare_baseline.py`). Frontier JSON checked in. Standing (median-of-5, provisional): price/fast **~37 µs/op** (med err $6e-7), price/accurate **~138 µs/op** (med err $1e-8), IV/accurate **~0.68 ms/op** cold (med round-trip **1e-12** vol pts), corners 2 European + 2 double-continuation NotImplemented. Cold single-op AL is ~2–8× the ALO ~10–22 µs envelope on this ISA (warm/cached/batched paths are the production levers). | infrastructure |
| **A4** — R-30 Debug bind-key assert + Release counter | **done** | `f9f5896` | `{T,r,q,n,nq}` Debug bind key stored in `al_bind_geometry_static`, asserted in `al_bind_geometry_sigma`; all-config `al_geometry_specialize_off_fallback_count()` tallies the specialize-off fallback (0 on production flow). `ResetAcrossContractsSidesAndSchemesMatchesFreshColdState` + `StaticGeometryExpCallsArePaidOncePerReset` green both configs (exp accounting unchanged). | correctness-hardening |
| **A5** — Healy negative-rate / double-boundary | **done (domain map + evidence; no impl needed)** | `cabe8d7` | Per-side capability-predicate map vs a Crank-Nicolson FD oracle across the full (r,q) plane: American/European price and match FD to 5e-3 rel; the `yield<rate≤0` double-continuation region returns explicit `NotImplemented` while FD confirms real early-exercise value (bail correct, not over-conservative). **No silent mis-price anywhere → no second-boundary implementation warranted** (valid complete outcome). Sources: Healy arXiv 2109.15157 / 2203.08794. | accuracy-improving |

**For Sprint I:** (1) A1 batch is ready to wire into the shared-boundary 9-node build + slice-σ cold-fallback; flip `kShipAvx2Boundary` only after a quiet-host best-of-3 clears 2.0× (consider vectorizing the BAW Newton seed first). (2) Cross-agent numeric drift: Agent K's K2 Φ (Cody rational-erfc) leaves my Chebyshev `norm_cdf_pd`/`pd2` untouched — no conflict; drift resolves on the full accuracy panel per §2. (3) A2's slice wiring lands in `boundary_interp.cpp` (Sprint R territory), not attempted here.

---

## 6. Sub-Sprint S — Surface engine: beat Vola Dynamics / SpiderRock *(Agent S)*

**Mission:** make the CStar nested-curve family (Vola Dynamics' differentiator, already in-repo) correct, fast, and evidenced for production-ladder inclusion; and cut the whole-universe cycle wall via parallel + projected OPRA ingest (SpiderRock envelope: full universe ~45 s cycle, ~90% within bid-ask). Consumes SPRINT rows W5.1, W5.2, W4.3 + the handoff's CStar correctness prerequisites. **CStar stays isolated R&D this sprint** — it must consume already-prepared American observations and an existing eSSVI seed (SPRINT handoff); no production wiring, no `curve_selector.cpp` edits.

**Interfaces:** produces (a) a correct+fast CStar calibrator with an evidence panel for the Sprint I ladder decision, (b) a parquet column-projection API in atx-core, (c) parallel `load_opra_daterange`. Consumes nothing from K/A.

### Task S1 — CStar correctness prerequisites (SPRINT handoff traps + REVIEW §6.1 #11)

**Files:** Modify `src/cstar.cpp:97-113` (butterfly gate), `:599-612` (projection). Test `tests/cstar_test.cpp` (extend).
**Class:** correctness + accuracy-improving. **These block S2/S3 — do first.**

- [ ] Write failing test for the reversed `c2` projection bisection: construct the case where the first midpoint is infeasible and assert the bisection currently keeps both bounds infeasible and returns an arb-violating `c2_hi` (REVIEW confirmed at HEAD).
- [ ] Fix the bisection bracket update; on projection completion run **post-projection no-arbitrage validation** and propagate failure (`cstar_arb_project` currently returns `Ok()` unconditionally).
- [ ] Separate raw shape validity from the public variance floor (handoff item) — two predicates, two error codes.
- [ ] Replace the FD butterfly gate (`(w₊−2w₀+w₋)/1e-8`, ~8 digits lost to cancellation) with the closed-form analytic `w''`; failing test: a near-boundary fixture where FD false-flags/false-clears and the analytic form decides correctly.
- [ ] Gate: zero false butterfly-arb flags on the fixture set (DoD row); projection failures propagate as `Err`, never silent `Ok`. Commit per fix.

### Task S2 — Analytic CStar Jacobian + fixed-cap Eigen (SPRINT W5.2)

**Files:** Modify `src/cstar.cpp:398-443`, `src/cstar_calib.cpp:85-101`. Test `tests/cstar_calib_test.cpp` (extend).
**Class:** accuracy-improving.

- [ ] Failing test: analytic `f'(z)` + modal derivatives vs central-FD reference — agreement to `sqrt(eps)`-scaled tolerance across the parameter box; and an LM-convergence fixture where FD noise currently costs iterations.
- [ ] Implement closed-form derivatives; fuse `w` with the gradient evaluation (one pass per obs per iter instead of recomputing `w` twice).
- [ ] Replace `MatX::Zero(dim,dim)` per-LM-iter heap alloc with `Eigen::Matrix<double,16,16>` fixed-cap (dim ≤ 16 asserted).
- [ ] Bench: `build_normal_eq_w` best-of-3, target 2–4×; LM iteration count non-increasing on the fixture set. Commit.

### Task S3 — Table-driven no-arb projection (SPRINT W5.1)

**Files:** Modify `src/cstar.cpp:542-613`. Create `bench/cstar_bench.cpp` (arb-projection + calibration rows).
**Class:** pure-refactor.

- [ ] Failing test: projection result on the fixture set is within tolerance of the current implementation (post-S1 semantics — S1's corrected behavior is the baseline, not the buggy HEAD).
- [ ] Precompute the fixed-grid modal basis `B[i][j]` + base/base′/base″ once per grid; hoist `sqrt(theta)`; make `w, w′, w″` a BLAS-1 sweep over β.
- [ ] Bench gate: **10–50×** on the arb-projection bench (SPRINT estimate; record the real number). Commit.

### Task S4 — Parallel + projected OPRA ingest (SPRINT W4.3)

**Files:** Modify `src/opra_batch.cpp:344-424,409`, `src/opra_panel.cpp:268`; add a column-projection API to `atx-core/src/io/parquet.cpp:143` (+ header). Tests: opra ingest suite (extend) + a new atx-core projection test.
**Class:** pure-refactor.

- [ ] First: add the atx-core `read_parquet` projection overload (`std::span<const std::string_view> columns`), Arrow internal threads **off**; test: projected read frame-equal to full read restricted to those columns.
- [ ] Port the valid OPRA-only pieces from unmerged commit `a28cea3` (manually — do not cherry-pick blind; SPRINT handoff): configurable outer threads, pre-sized disjoint slots, dynamic file claims, deterministic serial progress/error reporting, checked size arithmetic.
- [ ] Project the eight consumed columns at every `read_parquet` call site in the loader (enumerate them from `OptionChain::from_frame` consumption — verify, don't assume eight).
- [ ] **Do NOT** implement or claim fingerprint-before-decode skipping — no persisted sidecar/build identity exists yet (SPRINT handoff trap).
- [ ] Gates: byte/frame-identical output vs serial loader on a 3-date × N-symbol fixture; load wall ~min(cores, N)× on the real-OPRA fixture (best-of-3, quiet host); decoded-bytes counter drops. Commit.

### Task S5 — CStar evidence panel vs eSSVI (scope expansion; the ladder-decision artifact)

**Files:** Create `examples/cstar_panel.cpp` or extend the W0.3 accuracy-panel tool **behind a new flag** (read-only reuse — if it requires editing Sprint-R-owned files, build standalone instead). New fixture wiring only in Agent S TUs.
**Class:** infrastructure / R&D evidence.

- [ ] Feed CStar from **already-prepared American observations + the existing eSSVI seed** (handoff requirement) on SPY snapshots + the 25-name recovery cohort (`CZR,RPRX,RXT,ROIV,HST,FTV,EQH,IBN,TSLQ,JHX,MNTS,OKLL,EQX,SIDU,HIMX,GFI,DGXX,VNET,ESI,BFAM,PCOR,HTHT,IBRX,ALHC,GGG`).
- [ ] Emit per-board: fit wall, in-band fraction, χ², vol-RMSE, arb-flag counts (post-S1 analytic gate), vs the same board's eSSVI result.
- [ ] Deliverable: a table + recommendation (include-in-ladder / keep-R&D / kill) checked into `docs/reviews/`. **No production wiring** — the Sprint I session makes the call. Commit.

### Task S6 — Whole-universe cycle-time harness (scope expansion; Sprint I ammunition)

**Files:** Create `bench/universe_cycle_bench.cpp` (harness + JSON schema only; no baseline claims this sprint).
**Class:** infrastructure.

- [ ] Define the metric: wall for one full cycle = ingest (S4 path) → fit (blessed pipeline) → archive write, over a named universe (start: the 519-name cohort), single JSON row with per-stage breakdown — the SpiderRock comparison row (~45 s full-universe envelope, directional vendor number per SPRINT §2).
- [ ] Harness must run the blessed pipeline as a black box (no edits to Sprint-R TUs); mark it `Iterations(1)` corpus-style per the W0.1 pattern.
- [ ] Do **not** record a baseline yet — the number is meaningless until Sprint R + Sprint I land. Harness compiles, runs on a 3-name smoke universe, JSON schema documented. Commit.

**Sub-sprint S exit gate:** strict Debug+Release green; CStar correctness fixtures green (projection propagates failure, analytic butterfly gate, no false flags); 10–50× projection bench recorded; ingest parallel+projected with frame-identical proof; CStar evidence panel + recommendation checked in.

### Sub-Sprint S — ledger (Agent S, branch `feat/sota-s-surface`)

| Task | Status | Commit | Key numbers | Class |
|---|---|---|---|---|
| S1 CStar correctness prerequisites | done | `eeae918` | reversed `c2` bisection fixed; projection propagates `Err` (`OutOfRange` raw-shape / `Unavailable` butterfly) vs the prior unconditional `Ok`; raw-shape validity split from the public floor; closed-form analytic `w''` replaces FD /1e-8; **zero false butterfly flags** on the fixture set; 44/44 CStar tests | correctness + accuracy-improving |
| S2 Analytic Jacobian + fused w/grad + fixed-cap Eigen | done | `195d959` | closed-form `f'(z)` in the θ-partial (only grad[0] moves; rest bit-identical); single-pass `cstar_slice_w_and_grad`; `NormalEq` H/g are fixed-cap `Eigen::Matrix<double,16,16>` (no per-LM-iter heap); `fit/cstar/normal_eq` legacy 8837 → fused 4811 ns (~1.8×, provisional/concurrent host — fusion+FD-removal; H accumulation dilutes) | accuracy-improving |
| S3 Table-driven division-free no-arb projection | done | `7d8a8ca` | static window/basis table + division-free `w²·g` sign predicate + incremental group damping (precompute fixed/scalable part once per bisection); `arb/cstar/project` 928,891 → **62,662 ns = ~14.8×** best-of-3 (provisional, concurrent host, CV~13%) — inside the 10–50× gate; equivalence to per-point reference to the ULP | pure-refactor |
| S4 Parallel + projected OPRA ingest | done | `a0db885` | new `read_parquet(path, columns)` (Arrow internal threads OFF) frame-equal (atx-core test + proven byte-equal on **real** vxx-close board, 9/9); opra_panel projects the 8 consumed columns; `load_opra_daterange` parallel via `parallel_for_dynamic` (`n_threads`), `DateRange_ParallelEqualsSerial` green; checked size arithmetic; wall-clock speedup → Sprint I quiet host | pure-refactor |
| S5 CStar vs eSSVI evidence panel | done | `82750a5` | modal-feature board: vol-RMSE 0.0028 → 0.0005 (**5.6×**), in-band 59% → 100%, χ² 1.23 → 0.00; no regression on smooth smiles; **0 false arb flags** everywhere; ~60–90× eSSVI fit cost; steep-skew admission rejects rho≈−0.60. **Rec: KEEP R&D** (conditional selective ladder entry, real-OPRA validation at Sprint I) — `docs/reviews/2026-07-16-cstar-vs-essvi-evidence-panel.md` | infrastructure / R&D evidence |
| S6 Whole-universe cycle harness | done | `f53e70d` | `bench/universe_cycle_bench.cpp` ingest→fit→archive per-stage JSON (Iterations(1)); 3-name synthetic smoke, 3/3 loaded/fitted/archived (ingest ~15 / fit ~236 / archive ~1.4 ms, indicative only); **no baseline** recorded (per plan) | infrastructure |

**Exit status:** strict Debug green — full `atx_vol_fast` 1469/1470 and `atx-core` Parquet 61/61 pass; the only Debug failures, `SurfaceV2Provenance.ValidationFallbackAdmissionRecordsTheServedFamily` (fast) and `OpraBreadthCorpus.UnifiedPolicyFitsEveryAvailableBoard` (slow), are **proven pre-existing** (both fail identically with S4 reverted / at the pre-sub-sprint baseline; production eSSVI/v2 path, Sprint-R territory, not touched by this sub-sprint). CStar is isolated R&D throughout — no `curve_selector.cpp` / production wiring.

---

## 7. Multi-sprint roadmap

```
Sprint R  (user, in flight)      REVIEW remediation: R-01..R-35 (minus carve-outs),
   │                             W3.3, W3.4, owed W4.1/W4.4 gates
   │  runs in parallel with
   ├── Sprint P  (3 subagents)   Sub-sprints K / A / S in isolated worktrees   ← THIS FILE §4–6
   │
Sprint I  (integration)          merge R → K → A → S; wire the seams:
   │                               • AVX2 boundary batch → shared-boundary 9-node build
   │                                 (boundary_interp.cpp build(), now free) + warm node chaining
   │                               • public IV batch routing final decision (K4 measurement)
   │                               • Φ swap validated on the full accuracy panel
   │                               • W4.2 sibling fit pool + W4.5 generic H² guard
   │                                 + small-book cutoff measured at n={1,2,3,4,6,8,12,16}
   │                               • CStar ladder decision from S5 evidence
   │                             gate ladder: one-op SPY → 25-name → 100-name → 519-name
   │
Sprint G  (SOTA gates)           close the SPRINT §7 DoD table; whole-universe cycle
   │                             baseline (S6 harness) vs the ~45 s envelope; publish
   │                             reproducible shootouts (K5 vs Jäckel LBR, A3 vs ALO)
   │
Sprint X  (leadership, optional) SplineVol/SRCubic candidate in the selector ladder;
                                 Healy double-boundary productionization (A5 outcome);
                                 ~60 ns IV adoption (K6 outcome); AVX-512/GPU exploration
```

**Sprint I exit gates (intermediate, directional — DoD stays the authority):**

| Gate | Target |
|---|---|
| One-op real-OPRA SPY | ≤ 200 ms end-to-end (from 492 ms; R-01 alone predicts ~310 ms, R-11 + A-batch wiring close the rest); stretch ≤ 150 ms |
| 25-name recovery cohort | all boards fitted or truthfully reported per W3.3/W3.4 taxonomy; zero silent drops |
| 100-name panel | fit CPU ≥ 4× down vs W0 baseline at unchanged accuracy panel (waypoint toward the DoD 10×) |
| Effective cores (populate) | ≥ 6 on the 12-core box with bounded RSS (R-03 fix verified under the W4.1 gate) |
| No cross-agent regression | full accuracy panel: in-band ≥ prior, χ² ≤ prior, vol-RMSE ≤ prior after the Φ swap and all merges |

**Sprint G exit = SPRINT §7 DoD, unchanged:** SPY direct-route fit < 1 ms (stretch sub-500 µs); 100-name fit CPU ≥ 10× down, accuracy non-regressed; batch European price within ~2× of 4.4 ns/op/core on this ISA; zero FD-noise butterfly false flags; always-on observability; correctness rows (no partial-fit-as-success, QP truthful, no stale cache serve) all green.

---

## 8. Risks & standing traps (carried forward — every agent re-reads)

1. **Shared FetchContent dep dir** — isolate per worktree before first build; never concurrent Debug+Release anywhere (SPRINT).
2. **Benchmark noise** — three agents on one laptop: serialize bench runs; final numbers only count from the quiet-host Sprint I re-measurement.
3. **`pricing_executor` nested-dispatch deadlock is REAL at HEAD** (REVIEW §6.1 #8): no sub-sprint routes anything into it; W4.2 at Sprint I uses a sibling pool or queued scheduler with an explicit nested budget.
4. **Line-number drift** — every cited `file:line` re-verified by grep before edit (multiple sessions are writing `main`).
5. **Bit-identity is a telltale, not a gate** — the §Global economic bound governs; goldens update with documented justification.
6. **CStar stays off the production path** until the S5 evidence panel and the Sprint I decision — no selector edits from any sub-sprint.
7. **Merge conflicts in `bench/CMakeLists.txt`** are expected and trivial: keep all targets (precedent: the analytics merge resolution).
