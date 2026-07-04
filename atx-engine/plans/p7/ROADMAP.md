# Module p7 — Production Alpha Book + High-Performance DSL Pipeline

**Last reviewed:** 2026-06-28
**Started:** Wave 1 (S1 deflation · S2 breadth · S3 eval-VM) merged 2026-06-28 (`4a2113b`); Wave 2
(S4 turnover/capacity · S5 conviction sizing) merged 2026-06-28 (`d95ce04`). Spine S1–S5 all on main.
Next: Wave 3 (S6 incremental panel ⏳ open `feat/p7-s6`, S7 wire + dev-panel validate). V1 unblocked
(operator prod run after S7 wires the Wave-1/2 carry-forwards). See `TRACKER.md` for live status.
**Source:** p6 close-out (`atx-impl/research/2026-06-27-tradeable-alpha-results.md`) + a 4-explorer
survey of the post-p6 engine (2026-06-28); user direction 2026-06-28 (small targeted tests, parallel
agent streams, no hour-long production runs in the dev loop).
**Goal:** turn the *correct, cost-aware* p6 engine into a *production* one on two fronts — a robust,
deflation-surviving alpha **book** (not a single hit) and a DSL pipeline fast/operable enough to
search the breadth that book requires — moving incrementally toward RenTech/WorldQuant practice.

---

## Companion docs

| Doc | Covers |
|---|---|
| `sprint-1-deflation-gates.md` | S1 — DSR/PBO/split-stable admission gates + cumulative-sweep trial-count |
| `sprint-2-information-breadth.md` | S2 — FINRA short-interest + IV-surface + liquidity signal families |
| `sprint-3-evalvm-hotpath.md` | S3 — online variance family + cross-instrument parallelism + bench baseline |
| `sprint-4-turnover-capacity.md` | S4 — turnover penalty default + decay WeightPolicy + real capacity vector/curve |
| `sprint-5-conviction-sizing.md` | S5 — conviction wiring + fractional-Kelly + conviction-aware walk-forward |
| `sprint-6-incremental-panel.md` | S6 — incremental panel append + provenance |
| `sprint-7-wire-validate.md` | S7 — thread new knobs through CLI hub + dev-panel validation (no prod run) |
| `TRACKER.md` | live per-sprint status (created at first kickoff) |

**Pending (created at sprint kickoff/close):** per-sprint `phase-N-progress.md` ledgers; `p7.md` user
reference at module close.

**Sibling modules:** **p6** — Tradeable-Alpha Uplift, [../p6/ROADMAP.md](../p6/ROADMAP.md) (predecessor).

---

## Strategic positioning

p7 claims the identity **"a small WorldQuant-style alpha factory with RenTech-style honesty"**: many
weak, decorrelated, cost-aware signals combined into one book, every number deflated against the real
search it came from, sized by conviction. It is NOT a single-deep-alpha bet and NOT a HFT/live system.

| Dimension | WorldQuant | RenTech (public lore) | atx p7 target |
|---|---|---|---|
| Edge source | breadth: 10⁴–10⁶ weak alphas | breadth + deep information structure | breadth across ≥4 signal families, decorrelated |
| Overfit control | OS/IS, fitness, turnover | heavy multiple-testing discipline | DSR/PBO **gated** under cumulative-sweep trial-count |
| Combination | linear/risk-model blend | regime/state aware | shrinkage-MV → conviction → capacity, regime-guarded |
| Sizing | risk-normalized | Kelly-flavored, conviction-scaled | fractional-Kelly × per-name conviction |
| Pipeline | massive distributed | bespoke fast research substrate | single-box, online kernels + parallel eval, dev-panel loop |

When scope-creep argues, this table governs: if a proposal is not "more breadth, more honesty, or a
faster honest loop," it does not ship in p7.

---

## Phase 0 — Foundation (what p6 shipped; the gaps p7 closes)

**Solid (p6, merged):** eval/VM perf pass (S1), factory admission refactor (S2), turnover-aware
search + seed-elitism (S3), cost-aware gates `rt_cost_bps`/`min_holding_days` (S4), panel augmentation
`with_alpha101_fields` + `--adv-windows` (S5), sign-correct downstream book (S6), CLI threading +
capstone harness (S7). Determinism contract (inert defaults; byte-identical no-flag path) holds.

**Also already present (corrects the survey):** a full Google Benchmark harness exists at
`atx-engine/bench/` (25+ benches incl. `alpha_batch_bench`, `alpha_streams_bench`, `factory_bench`,
`parallel_bench`, `panel_read_bench`), gated behind `ATX_BUILD_BENCH=OFF`. p7 perf work *uses and
extends* it — it does not build a harness from scratch. A FINRA short-interest pipeline is built on
the `worktree-track-b-information-structure` branch (finra_short.hpp/cpp + stage_augment + 8 tests
green; carries an out-of-scope B4 regime-OOS analyzer to exclude on landing), unmerged. The S10
conviction/Kelly/breadth/regime **engine** infra is **already on main** — `risk/kelly_sizing.hpp/.cpp`,
`combine/conviction.hpp`, `combine/regime_combiner.hpp`, `eval/breadth.hpp` all exist; the gap is they
are **uncalled in the atx-impl deploy pipeline**. S5 is wiring, not new engine code.

**Critical gaps (each maps to a sprint):**

| Gap (evidence) | Closes in |
|---|---|
| DSR/PBO/split-stable computed but **not gated**; factory **voids** trial-count (`src/factory/factory.cpp:983` `static_cast<void>(trial_count)`) → DSR deflated at N=1, sweep invisible to anti-overfit | **S1** |
| Search axis is a **price monoculture**; FINRA/IV/liquidity families unshipped (most-cited edge-poverty cause) | **S2** |
| 29/34 time-series ops are **O(T·W) batch**, variance family reverted to batch (`alpha/ts_ops.hpp:304`), no cross-instrument parallelism | **S3** |
| `turnover_penalty_slope=0.0`, decay WeightPolicy not built (`loop/weight_policy.hpp:96`), `--capacity-floor` a **1.0 no-op stub**, no capacity curve | **S4** |
| `conviction()` computed but **uncalled** in the active pipeline; no fractional-Kelly; walk-forward measures the bare combiner, not the shipped conviction-weighted book | **S5** |
| Panel build is **full-rebuild only** (one new day ⇒ rebuild all segs); `config_json`/`engine_git_sha`/`wall_ms` provenance empty | **S6** |
| New S1/S4/S5 knobs need CLI threading + a fast validation that is **not** an hour-long prod run | **S7** |

**Wave-1 landed (2026-06-28, `4a2113b`):** S1 ✓ DSR/PBO/split-stable now gate in `AlphaGate::admit`
(carried on a non-serialized `GateDeflation` POD — `AlphaMetrics` is a frozen 56-byte on-disk record,
so the plan's "add fields to AlphaMetrics" was byte-identity-unsafe and was correctly rerouted). S2 ✓
FINRA + IV-surface + liquidity families + multi-family seeds (engine/core only; CLI deferred to S7
per decision D1). S3 ✓ online Welford variance family (ResearchFast) + cross-instrument column
parallelism (AuditExact) with recorded bench wins (TsVar 4.27×, TsZscore 4.70×, column-parallel w4
3.05×). Determinism held: oracle/golden/digest slice 18/18 + alpha 602/602 on merged main.
**S1-4 correction (do not overstate):** the cascade pre-gate threads cumulative N only in the *safe
(looser) direction* — it is a perf pre-filter, **not** a deflation mechanism. Cumulative-sweep
trial-count deflation is enforced at the **holdout DSR gate** (`prior_r1 + res.trial_count`,
pre-existing). A genuinely-stricter pre-gate cannot land under the frozen byte-identity contract
(it would change the admitted set ⇒ golden re-baseline). **Wave-1 carry-forward to S7:** wire
`GateDeflation` into `library::verdict_for` (else the new gate screens are dead code on live callers);
thread S2's augment CLI (`--short-interest`/`--augment-out`/`--si-publication-lag` + `augment`
subcommand + `run_augment` stage); fix the stale `stage_discover.cpp` "0..5" reject-histogram comment.

---

## Validation discipline (the load-bearing adjustment for p7)

p6 ended on an hours-long full-panel run that was killed twice — the exact "spin wheels on degenerate
code" failure the user named. **p7 forbids the hour-long production run as a sprint gate.** Every
sprint proves its claim with three small, fast instruments only:

1. **Unit tests on tiny deterministic fixtures** — the primary gate. Gate logic, kernel correctness,
   sizing math: proven on hand-built ≤50×≤20 panels where the answer is known by construction. (Per
   [docs/implementation-quality.md](../docs/implementation-quality.md): negative tests + edge cases.)
2. **Dev-panel smoke ≤5 min** — the integration gate. `scripts/build-tradeable-alphas.ps1 -Profile
   smoke` on the cached `work/dev/dev-panel.bin` (600×501) exercises the *whole* pipeline end-to-end
   (loose gates guarantee admits). Validates wiring, not edge.
3. **Microbench delta (perf sprints only)** — `ATX_BUILD_BENCH=ON` + the existing `bench/` targets,
   with a recorded before/after line per [implementation-quality.md](../docs/implementation-quality.md).

The full-panel **prod** run is a single, explicit **operator validation milestone (V1)**, run *once*
after the spine sprints land — never inside a sprint loop. See "Validation milestone V1" below.

**Determinism contract (every sprint, inherited from p6):** any output-changing capability sits
behind an engine-config field defaulting to today's value; the no-flag path stays byte-identical
(`NsgaSearch.ScalarRaw_ReproducesGoldenDigest`, `FactoryOos.MineIntoOffPathDigestUnchanged`, OOS
goldens; `oracle.hpp` untouched). Perf wins that change bits ship behind a `ResearchFast` tier and are
re-scored on the audit path before publication; byte-identical wins ship directly. Each opt-in ships
(a) off-path byte-identity, (b) on-path RED→GREEN, (c) twice-run, (d) seq==parallel where an admission
path is touched.

---

## The sprints (disjoint file ownership ⇒ parallel agent streams)

S1–S6 own **disjoint** file sets and dispatch in parallel waves. S7 owns the shared CLI hub and runs
last (mirrors the p6 S1–S6 ∥ / S7-last contract). `oracle.hpp` is untouchable by every sprint.

| # | Sprint | Goal metric (small-test gate) | Owns (exclusive) |
|---|---|---|---|
| **S1** | Deflation gates & honest selection | admitted PBO ↓; DSR monotone in cumulative N; goldens byte-identical off-path | `combine/gate.hpp`, `combine/metrics.hpp`, `eval/deflated_sharpe.hpp` (wiring), `src/factory/factory.cpp` (trial-count) + tests |
| **S2** | Information breadth | # evaluable signal families 1→≥4 (field-derivation unit tests on tiny fixtures) | NEW short-interest ingest, `alpha/datafields.hpp`, `alpha/augment.hpp`, seed fixtures + tests |
| **S3** | Eval-VM hot path + bench baseline | batch-Ts variance family online & numerically-safe; recorded bench delta; differential-vs-oracle green | `alpha/ts_ops.hpp`, `alpha/vm.hpp`, `bench/` additions + tests |
| **S4** | Turnover & capacity realism | decay ↓ turnover on fixture; capacity vector ≠ 1.0; capacity curve monotone | `loop/weight_policy.hpp`, `combine/combiner.hpp`, `cost/capacity.hpp`, `risk/capacity.hpp` + tests |
| **S5** | Conviction-scaled sizing | Kelly sizing math exact; WF measures the conviction-weighted book | `combine/conviction.hpp` (wiring), `risk/kelly_sizing.hpp` (exists on main, wire into deploy), `eval/regime_slice.hpp` (WF), atx-impl `stage_combine.cpp` call-site + tests |
| **S6** | Incremental panel + provenance | append == full-rebuild byte-identical; provenance populated | `atx-impl/src/{stage_panel,serialize_panel,stage_load,store_progress_sink}.cpp`, `data/history_panel.hpp` + tests |

> **Ownership reconciliation (binding):** `stage_discover.cpp` is owned by **S7 only** (the CLI hub).
> S6's provenance work splits: `wall_ms` lands in `store_progress_sink.cpp` (S6); the
> `config_json` / `engine_git_sha` fields on `PipelineRunRow` are threaded in **S7**'s stage_discover
> pass (they snapshot the resolved CLI config, which is S7's concern). S5's atx-impl call-site is
> `stage_combine.cpp` (S5-owned for this sprint), NOT the hub files. No two sprints edit the same file.
| **S7** | Wire + dev-panel validate | new knobs round-trip; dev-panel smoke green; determinism slice byte-identical | `atx-impl/src/{config.hpp,config.cpp,stage_discover.cpp,stage_run.cpp}`, harness + research doc |

Each sprint file decomposes into 4–7 ledger units (`SN-0..SN-k`) per
[docs/sprint.md](../docs/sprint.md), with per-unit marker/commit discipline and the
implementation-quality handoff block in every subagent brief.

---

## Validation milestone V1 (operator, out-of-loop)

After S1–S5 land (the spine), the operator runs the full-panel prod book *once*, overnight, to read
the real north-star number — this is the only hour-long run in p7, and it is a milestone, not a gate:

```
build-tradeable-alphas.ps1 -Profile prod -Stage augment,discover   # canonical-screened panel
build-tradeable-alphas.ps1 -Profile prod -Stage pipeline
```

Output → an `atx-impl/research/<date>-production-book-results.md` scorecard: book-level net-of-cost
OOS Sharpe, DSR (cumulative-N), PBO, CPCV, walk-forward, capacity curve, N_eff/IR breadth. If the bar
is missed, the reject-histogram dominant bucket names the next sprint's target. Honest null is valid.

---

## North star (unchanged from p6, raised to book level)

On the capacity-screened real panel, a **deployed book** (≥5 admitted, decorrelated, conviction-sized
alphas) that is simultaneously: net-of-10bps OOS Sharpe **> 1.0** (book-level), DSR > 0 under
**cumulative-sweep** trial-count deflation, PBO < 0.5, turnover < 0.30/day, capacity curve positive at
≥ $100M AUM, sign-correct sane footprint — OR a documented frontier naming the binding constraint.
Measured only at V1, never in a sprint loop.

---

## Sequencing

1. **Wave 1 (parallel) — the spine:** S1 (honest selection), S2 (breadth), S3 (fast eval). Dispatch
   together; disjoint files. These three are the minimum that changes *which* alphas the machine can
   find and *how fast* it finds them.
2. **Wave 2 (parallel) — book quality:** S4 (turnover/capacity), S5 (conviction sizing). Turn a
   positive gross edge into a tradeable, well-sized net one.
3. **Wave 3:** S6 (incremental panel — quality-of-life for the data loop), then S7 (wire + validate).
4. **V1** operator prod run once S1–S5 are merged.

**If you can only do one slice:** **S1 then S2.** Without honest cumulative-N deflation (S1) the
multi-seed sweep is an overfit generator; without breadth (S2) there is no real edge to deflate. If
two slices: add **S3** (the O(T·W) batch-Ts path is the throughput ceiling on breadth). Everything
else is multiplier, not enabler.

---

## Future-work backlog (roadmap-only; not yet a detailed sprint)

- **SIMD intrinsics** for cs_rank/zscore + hot Ts loops — open only after S3's bench baseline shows
  the auto-vectorizer ceiling; premature otherwise.
- **Regime-adaptive combine** (HMM posterior → `RegimeCombiner`) — guarded, low priority. Research
  warns regime is a tool, not the spine (`research/rentech-structure-signals-domain-mapping.md`).
- **Survivorship / delisted-symbol recovery** (`data/universe.hpp:47`) — correctness; needs a
  delisted-name security master with exit dates. Carries the scorecard caveat until done.
- **True GICS industry/subindustry ingestion** (`alpha/augment.hpp` I5-HOOK) — sub-industry
  neutralization is non-functional until real SIC/NAICS lands.
- **persistence-v2 Dev→UAT→PROD promotion + decay monitor** — the "operate the book" branch, after V1.

---

## Anti-roadmap (explicitly NOT in p7)

1. **No hour-long production run as a sprint gate** — V1 is the only full-panel run, operator-driven.
2. **No live broker / order-routing / LOB matching** — research engine only (inherited from parent).
3. **No distributed/cross-machine execution** — single-box; parallelism is intra-process DetPool only.
4. **No alt-data beyond price/vol/options/classifications/short-interest** — S2's families are the line.
5. **No golden re-baseline** — the build profile is always the explicit opt-in; `oracle.hpp` frozen.
6. **No HMM as the spine** — regime conditioning stays a guarded, optional combine path.

Sprint discipline: [../docs/sprint.md](../docs/sprint.md). Implementation quality (mandatory for
every coding unit): [../docs/implementation-quality.md](../docs/implementation-quality.md).
