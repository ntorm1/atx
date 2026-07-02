# Sprint 2 — Mega-Alpha Meta-Book

**Goal:** wire the already-built `fund::MetaAllocator` (inverse-vol / ERC / HRP risk-budgeting +
fractional-Kelly vol-target) and `fund::MetaBook` (two-pass sleeve netting, trailing cross-sleeve
risk budget, Euler attribution, Meucci effective-bets) into the runnable `atx-impl` pipeline as a new
deploy stage `stage_metabook`. The runnable path today combines every admitted alpha into ONE linear
blend over ONE combined panel and hands that to a single MVO/position-deploy — there is no
fund-level sleeve allocation, no cross-sleeve netting, no robust portfolio-of-books. This is the
**literal "combination into mega-alphas" centerpiece** of the module: split the admitted library into
`N` sleeves, run each independently (net-after-optimize), build a TRAILING cross-sleeve return
covariance Ω *strictly before* each period, allocate per-sleeve capital `c[s]` off Ω, and net the
per-sleeve books into ONE fund book — measuring the crossing turnover win and reporting Euler
attribution-by-sleeve. All opt-in behind an inert `MetaBookConfig` default (= single sleeve == today's
whole-panel book, byte-identical); the no-flag path stays byte-identical.

**Owns (exclusive):**
`atx-engine/include/atx/engine/fund/meta_book.hpp`,
`atx-engine/include/atx/engine/fund/meta_allocator.hpp`,
`atx-engine/include/atx/engine/fund/cross_sleeve_risk.hpp`,
`atx-engine/include/atx/engine/fund/netting.hpp`,
`atx-engine/include/atx/engine/fund/sleeve.hpp`
(WIRING / adapter surface only — the estimation & linear-algebra math in `src/fund/*.cpp` is FROZEN;
S2 CALLS `MetaBook::run` / `MetaAllocator::allocate` / `net_fund_book` / `sleeve_return_cov`, it does
not re-derive them),
`atx-engine/include/atx/engine/combine/combined_source.hpp` (the per-sleeve mega-alpha seam),
NEW `atx-impl/src/stage_metabook.{cpp,hpp}` + the `SleeveSpec` sleeve-assignment seam it defines;
tests under `atx-engine/tests/fund/` and `atx-impl/tests/`.

**Must NOT touch:** `alpha/oracle.hpp` (untouchable every sprint); `src/fund/*.cpp` estimation /
allocator bodies (FROZEN — S2 calls them, it does not re-derive them); `risk/*`
(`factor_model`/`stat_factor_model`/`dead_factor`/`shrinkage`/`eigen_adjust`/`optimizer`/`capacity`/
`garleanu_pedersen` — Sprints 1 & 4); `cost/*`, `loop/*`, `exec/*` (Sprint 4); `learn/*`,
`combine/regime_combiner.hpp`, `combine/combiner.hpp`, `atx-impl/src/stage_combine.cpp` (Sprint 3);
`atx-impl/src/stage_optimize.cpp` (Sprint 1 owns the covariance-source swap); the four hub files
`atx-impl/src/{config.hpp,config.cpp,stage_discover.cpp,stage_run.cpp}` + `library/library.hpp` +
`factory/factory.cpp` (Sprint 5).

> **`stage_run.cpp` note (binding):** `run_all` is Sprint-5-owned. S2 does NOT insert `metabook` into
> the `run_all` orchestration or add a CLI subcommand; it ships the stage + a direct-call integration
> test and hands the `run_all` wiring seam to S5. `stage_metabook` is reachable in S2 only through its
> own dispatch-free entrypoint and its tests (mirrors S1's "flag threaded in Sprint 5" discipline).

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

## The orphan gap (verified file:line)

The entire `fund/` mega-alpha combination layer — `fund::MetaAllocator` (HRP / ERC / inverse-vol +
fractional-Kelly, `meta_allocator.hpp`, ~524 impl lines), `fund::MetaBook` (two-pass netting driver,
`meta_book.hpp`, ~506 lines), `cross_sleeve_risk`, `netting`, `sleeve` (~1411 impl lines total,
100+ tests) — is **built, tested, and green** but **never called from `atx-impl`**. The runnable
pipeline combines every admitted alpha into ONE linear blend over ONE combined panel
(`stage_combine.cpp`) and hands that single combo to a single MVO / position-deploy
(`stage_optimize.cpp`). There is no sleeve layer, no cross-sleeve netting, no risk-parity
portfolio-of-books.

| Gap | File:line | Evidence |
|---|---|---|
| No sleeve/meta-book symbol anywhere in the runnable tree | grep `atx-impl/src` for `MetaAllocator`\|`MetaBook`\|`stage_metabook`\|`Sleeve`\|`cross_sleeve`\|`netting` | **zero hits** (2026-07-02 recon, obs 17720). The whole `fund/` layer is dead weight in the runnable pipeline. |
| Combine collapses ALL alphas to ONE linear blend | `stage_combine.cpp:662-678` | `std::vector<atx::f64> combined(D*N, …)`; `acc += combo.weights[a]·rows[a][i]` — one mega-alpha panel, no sleeve grouping. |
| Optimize consumes that ONE combo panel | `stage_optimize.cpp:36-37`, `:218-229` | `read_panel(cfg.combo)`; `alpha_at` reads the single `alpha` field; `mpo.run(sched, alpha_at, model_at, cost)` — a single-panel MVO, no portfolio-of-books. |
| `run_all` wires exactly `combine → optimize → report` | `stage_run.cpp:70-112` | no `metabook` step between combine and optimize — the pipeline never assembles sleeves. |
| `MetaBook::run` (the driver) never invoked | `meta_book.hpp:170-175` | the two-pass `run(sched, sources_at, model_at, returns_at, cost)` entrypoint has no `atx-impl` caller. |
| `MetaAllocator::allocate` (Ω → c) never invoked | `meta_allocator.hpp:125-127` | `allocate(Omega, sleeve_vol, caps)` — the HRP/ERC/inverse-vol → capital-weights kernel — has no `atx-impl` caller. |
| `net_fund_book` (crossing) never invoked | `netting.hpp:104-107` | the per-period cross-sleeve netting + crossing-benefit measurement — no `atx-impl` caller. |
| `sleeve_return_cov` / `fund_risk` never invoked | `cross_sleeve_risk.hpp:101-120` | the trailing Ω builder + Euler risk report — no `atx-impl` caller. |

**Why this matters (measured):** Phase-D found 30 admitted alphas collapse to `N_eff = 8.76`
(crowding) with turnover ~74%/week
(`atx-engine/research/2026-06-21-phaseD-conviction-breadth-oos-findings.md`, ROADMAP §"one fact").
Cross-sleeve **netting** (offsetting sleeve flow crosses internally, so the fund trades the NET, not
the gross — `netting.hpp:11-16`) directly counters the turnover; **HRP/ERC risk-budgeting** off a
trailing cross-sleeve Ω directly counters the crowding by allocating capital to *decorrelated* sleeves
instead of a single crowded blend. Both mechanisms are BUILT and GREEN; S2's only job is to reach them.

---

## Architecture note — what "wire the meta-book" actually means

`MetaBook` is a **two-pass DRIVER over `N` sleeves** (`meta_book.hpp:11-22`); it re-implements NONE of
the S2 math — it COMPOSES `Sleeve` (pass 1), `sleeve_return_cov` + `MetaAllocator` (the pass-2 budget),
and `net_fund_book` (the pass-2 crossing) into one fund. S2's job is three thin seams, none of which
touch the frozen kernels:

1. **Define sleeves** (S2-1): map the admitted-alpha set → `N` `SleeveConfig`s. A `Sleeve`
   (`sleeve.hpp:56-85`) bundles a library-`AlphaId` subset (`SleeveConfig::members`), free-form
   universe×family tags (`SleeveTag`), a per-sleeve capacity ceiling (`capacity_gross`), and a wrapped
   S1 `MultiHorizonConfig` (`mh`). `Sleeve::run` is PURE DELEGATION to
   `MultiHorizonOptimizer{cfg.mh}.run(...)` (`sleeve.hpp:79-81`) — the structural source of the R7
   boundary pin. S2 assembles the members/tags/config from the panel + library; it writes no optimizer.

2. **Build & drive** (S2-2): a NEW `stage_metabook` producer that constructs the `MetaBook`
   (`cfg.alloc` + `risk_lookback`, `meta_book.hpp:90-93`; the `sleeves` vector) and calls
   `MetaBook::run(sched, sources_at, model_at, returns_at, cost)` (`meta_book.hpp:170-175`). PASS 1 runs
   each sleeve independently (`net-after-optimize`); PASS 2 builds the TRAILING Ω from sleeve P&L
   *strictly before* `s` (`sleeve_return_cov`, the caller-sliced window — `cross_sleeve_risk.hpp:107-120`),
   allocates `c[s]` (`MetaAllocator::allocate`), and nets the period-`s` sleeve books into ONE fund book
   (`net_fund_book`). The driver already does all of this; S2 supplies the four callbacks and the cost
   model.

3. **Net & report** (S2-3 / S2-4): the crossing turnover win (`NetResult.turnover_net ≤
   turnover_gross`, `crossing_benefit_bps`, `netting.hpp:60-72`) and the fund report — Euler
   attribution-by-sleeve (`SleeveAttribution`: `return_contrib` / `risk_contrib` / `crossing_credit`,
   each SUMMING to the fund total, `meta_book.hpp:98-108`) + the Meucci effective-bets gauge
   (`FundReport::effective_bets`, `meta_book.hpp:129`). These come OUT of `MetaBook::run` in the
   `MetaBookResult`; S2 surfaces them as stage telemetry — it computes none of them.

The load-bearing inputs the single-blend path is missing are (a) the SLEEVE PARTITION (which alphas go
in which sleeve) and (b) the four `MetaBook::run` callbacks assembled from the runnable panel/library.
No new estimator, no new allocator, no new netting math — S2 assembles inputs and calls the frozen
driver, exactly as S1 assembles exposure columns and calls the frozen `build_components`.

**The `sources_at`/`model_at`/`returns_at` seam (as-built, confirmed):** `Sleeve::run` and
`MetaBook::run` take the S1 `HorizonSources`/`FactorModel` callback pair
(`multi_horizon.hpp:116-118`, `:156-158`), and `MetaBook` additionally takes `returns_at(period) ->
span<const f64>` (the S2-5 as-built resolution — the realized per-instrument simple return, drives Ω
and the report, NOT the books — `meta_book.hpp:25-40`, `:164`). S2's producer assembles these three
callbacks from the runnable panel: `sources_at(sleeve j, period p)` yields sleeve `j`'s per-member
signal cross-sections at `p` (an identity `SignalHorizon` per member for the H=1 boundary case),
`model_at(p)` returns the shared risk model (the S1 `FactorModelArtifact` when present, else
`diag_risk.hpp:29`'s diagonal model — see the cross-sprint seam), and `returns_at(p)` yields the
realized per-instrument return from the research panel's `close` field (the same TRI-return convention
`diag_risk.hpp:44-52` already computes).

---

## Determinism contract (Sprint 2)

S2 follows the **p8 opt-in / default-byte-identical** contract (ROADMAP §Determinism). The whole
capability lives behind a new `MetaBookConfig` field surface with an inert default that reduces the
driver to today's single whole-panel book. The engine `MetaBook`/`MetaAllocator` are deterministic by
construction — order-fixed ascending-index reductions, FIXED-iteration ERC (`solve_iters`, NO
early-exit, `meta_allocator.hpp:99`), no RNG, no clock, no `std::unordered_*`
(`meta_allocator.hpp:60-66`, `meta_book.hpp:64-69`) — so the on-path result is reproducible run-to-run
and seq==parallel.

**The inert default = the R7 boundary pin (the load-bearing regression).** ONE sleeve whose members are
the WHOLE admitted set, a `MetaAllocatorConfig` that yields `c == [1.0]` every period
(single sleeve + `fractional_kelly = 1`, `target_vol = 0`, `max_gross ≥ 1`, large `capacity_gross` ⇒
`c = [1]`; the `s == 0` empty-Ω fallback also gives `1`), and one-sleeve netting (net == gross, `W =
1·w_0 = w_0`, no crossing) ⇒ the driver's `fund_books[s]` is **BYTE-IDENTICAL** to that sleeve's
`MultiHorizonResult.books[s]` (`meta_book.hpp:51-61`). At the inert default, `stage_metabook` therefore
produces the byte-identical book the single-blend optimize path produces today.

**CAUSALITY — the central S2 trap (R2, no look-ahead).** The capital weights `c[s]` at period `s` are
allocated from the TRAILING window of sleeve P&L `{ r_s[p] : max(0, s − risk_lookback) ≤ p < s }` —
STRICTLY periods `p < s` (`meta_book.hpp:41-49`). At `s == 0` the window is EMPTY ⇒ Ω is 0×0 ⇒ the
allocator degenerate/empty fallback fires (`meta_allocator.hpp:51-56`). `c[s]` therefore depends only on
P&L realized BEFORE `s`. **Truncating the schedule after period `t` MUST leave every fund book at
`p ≤ t` byte-identical** (the trailing budget read no future) — the R2 integration gate, and a
mandatory PIT guard test (S2-2).

**Four test classes per opt-in path (mandatory):** (a) off-path byte-identity — single-sleeve
`MetaBookConfig` default ⇒ the fund book digest equals the pre-S2 optimize-stage golden; (b) on-path
RED→GREEN — ≥2 sleeves compose into one fund book with a measured crossing turnover reduction the
single-blend path cannot produce; (c) twice-run — same panel → same fund books + same report bytes;
(d) seq==parallel — the pass-1 sleeve walks are independent (each sleeve is blind to the others,
`meta_book.hpp:13-14`) and the per-period Ω build reads only `p < s`, so sleeve-order and
period-parallel execution are book-invariant.

---

## Dependency / wiring map

```
MetaBookConfig (engine, meta_book.hpp:90)      ← S2-0 plumb into the impl config seam (inert default:
                                                  single sleeve == whole-panel book, byte-identical)
NEW SleeveSpec (stage_metabook.hpp)            ← S2-1 the sleeve-assignment seam: admitted AlphaIds → N SleeveConfigs
fund/sleeve.hpp:SleeveConfig                    ← S2-1 populate members / tag / capacity_gross / mh
combine/combined_source.hpp                     ← S2-1 (optional) per-sleeve mega-alpha source seam
NEW atx-impl/stage_metabook.cpp                 ← S2-2 the deploy stage: panel + library → MetaBookResult
  ├─ builds sources_at(sleeve,period) / model_at(period) / returns_at(period) / CostInputs
  └─ calls MetaBook::run (meta_book.hpp:170)    ← FROZEN driver (PASS 1 sleeves, PASS 2 Ω→c→net)
       ├─ Sleeve::run (sleeve.hpp:79)           ← PASS 1: MultiHorizonOptimizer{cfg.mh}.run (FROZEN)
       ├─ sleeve_return_cov (cross_sleeve_risk.hpp:119) ← PASS 2: TRAILING Ω from p<s P&L (FROZEN, PIT-safe)
       ├─ MetaAllocator::allocate (meta_allocator.hpp:125) ← PASS 2: Ω → c[s] (ERC default) (FROZEN)
       └─ net_fund_book (netting.hpp:104)        ← PASS 2: Σ_s c_s·w_s + crossing benefit (FROZEN)
MetaBookResult.report (meta_book.hpp:113,136)   ← S2-4 Euler attribution + Meucci effective-bets surfaced as kvs
model_at source (S1 seam)                        ← FactorModelArtifact when kind==Factor, else diag_risk.hpp:29
tests/fund/fund_metabook_wire_test.cpp           ← S2-2/S2-3 (auto-globbed)
tests/fund/fund_sleeve_assign_test.cpp           ← S2-1
atx-impl/tests/metabook_test.cpp                 ← S2-0/S2-2/S2-4/S2-5
atx-impl/tests/metabook_netting_test.cpp         ← S2-3
```

---

## Tasks

### S2-0 — Open ledger + `MetaBookConfig` plumbing + public-API confirmation (do first; all units depend on this)

**Goal:** create the sprint ledger (marker commit); thread the engine `MetaBookConfig`
(`meta_book.hpp:90-93`) into the impl config seam with an inert default that means "single sleeve ==
today's whole-panel book"; confirm the exact public signatures of the four kernels S2 will CALL
(`MetaBook::run`, `MetaAllocator::allocate`, `net_fund_book`, `sleeve_return_cov`/`fund_risk`) and the
`Sleeve`/`SleeveConfig` surface. **No behavior change** — the config type exists and the stage's
default reduces to the byte-identical single-blend book.

**Root cause:** the runnable pipeline has no fund-config surface at all — `RunConfig`
(`atx-impl/src/config.hpp`, Sprint-5-owned) carries `--gross`/`--name-cap`/`--rebalance` etc. but no
sleeve/allocator knobs, and `stage_run.cpp:70-112` wires `combine → optimize → report` with no
metabook step. S2 cannot edit `config.hpp`/`config.cpp`/`stage_run.cpp` (Sprint 5). So S2-0 introduces
its OWN small `MetaBookStageConfig` POD in the S2-owned `stage_metabook.hpp` that carries the engine
`MetaBookConfig` + a sleeve-assignment policy, and S5 later maps CLI flags onto it (the same pattern S1
uses: engine config-struct fields with inert defaults, consumed by an S2-owned stage, threaded to CLI
in Sprint 5).

**Wiring:**
- NEW `atx-impl/src/stage_metabook.hpp`: the stage's own config seam, inert-default:
  ```cpp
  // How the admitted-alpha set is partitioned into sleeves (S2-1). SingleSleeve is the
  // INERT default: one sleeve == the whole admitted set == today's whole-panel book.
  enum class SleeveAssignment : atx::u8 {
    SingleSleeve = 0, // inert => byte-identical to the single-blend optimize book (R7 pin)
    ByLibraryGroup,   // one sleeve per library segment / provenance group
    ByCorrCluster,    // data-driven single-linkage clusters of the alpha-return corr matrix
    BySignalFamily,   // one sleeve per SleeveTag.family label (momentum/reversal/carry/…)
  };
  struct MetaBookStageConfig {
    atx::engine::fund::MetaBookConfig meta{};              // engine driver knobs (alloc + risk_lookback)
    SleeveAssignment assignment = SleeveAssignment::SingleSleeve; // inert => one sleeve
    atx::u32 max_sleeves = 8U;                             // cap on N (ByCorrCluster / ByLibraryGroup)
    // meta.alloc defaults: method=EqualRiskContribution, fractional_kelly=0.3, target_vol=0,
    // max_gross=4 (meta_allocator.hpp:92-99). For the SingleSleeve inert path the stage
    // OVERRIDES alloc to the c==[1] boundary config (fractional_kelly=1, target_vol=0,
    // max_gross>=1, large caps) so fund_books == the sleeve's MultiHorizonResult.books
    // byte-for-byte (the R7 pin). Documented in the ledger.
  };
  ```
- Confirm (read, do not modify) the FROZEN call surfaces at kickoff and record them in the ledger:
  `MetaBook::run` (`meta_book.hpp:170-175`), `MetaAllocator::allocate` (`meta_allocator.hpp:125-127`),
  `net_fund_book` (`netting.hpp:104-107`), `sleeve_return_cov`/`fund_risk`
  (`cross_sleeve_risk.hpp:101-120`), `Sleeve`/`SleeveConfig` (`sleeve.hpp:56-85`).

**Determinism:** pure addition. No aggregate-initializer breakage (append fields at struct end).
`SleeveAssignment::SingleSleeve == 0` is frozen (a `static_assert`/test pins it). Nothing wires
`SleeveAssignment != SingleSleeve` yet, so the optimize/report goldens are untouched.

**Accept:**
- Project compiles (debug + release); all existing `fund_*`, `optimize_*`, `combine_*`, `report_*`
  suites green.
- `metabook_config_defaults` (new `atx-impl/tests/metabook_test.cpp`): `MetaBookStageConfig`
  default-constructs to `SingleSleeve` + the engine `MetaBookConfig` defaults; a `static_assert` pins
  `SleeveAssignment::SingleSleeve == 0`.
- Ledger records the four confirmed FROZEN call signatures (the API contract the later units depend on).

---

### S2-1 — Sleeve definition: admitted alphas → `N` `SleeveConfig`s (the `SleeveSpec` seam)

**Goal:** map the admitted-alpha set to `N` sleeves deterministically, defining the `SleeveSpec`
seam — the sole place that decides which `AlphaId`s land in which sleeve, with which tags and capacity.
This is the producer of the `std::vector<Sleeve>` `MetaBook` consumes; S2-2 drives it.

**Root cause:** there is no sleeve partition anywhere. `stage_combine.cpp:377-405` enumerates ALL
admitted library records in `AlphaId` order into ONE flat DSL list and blends them into ONE mega-alpha
(`:662-678`); nothing groups them. `Sleeve` (`sleeve.hpp:56-85`) exists and can carry a member subset +
tags + capacity + a wrapped `MultiHorizonConfig`, but nothing assembles those from the runnable
library.

**Wiring:**
- In `stage_metabook.cpp`, a deterministic `assign_sleeves(const library::Library& lib, const
  MetaBookStageConfig& cfg) -> Result<std::vector<fund::SleeveConfig>>`:
  - `SingleSleeve` (inert): one `SleeveConfig` whose `members` = ALL admitted `AlphaId`s in ascending
    order (mirrors the combine enumeration, `stage_combine.cpp:401-405`), tags `{"US","all"}`,
    `capacity_gross` = a large sentinel, and the `mh` config that reproduces the single-blend book.
  - `ByLibraryGroup`: one sleeve per library segment / provenance group (the library already carries
    per-segment `base_alpha_id`/`n_alphas` boundaries — `library.hpp:461-475`), capped at
    `cfg.max_sleeves`.
  - `ByCorrCluster`: single-linkage clusters of the alpha-return correlation matrix over the admitted
    pool (the SAME corr convention combine's breadth path uses, `stage_combine.cpp:753-756`), truncated
    to `cfg.max_sleeves`. Deterministic: order-fixed distance, stable tie-break.
  - `BySignalFamily`: one sleeve per `SleeveTag.family` derived from the alpha's provenance/label
    (`rec.provenance`, `stage_combine.cpp:403`); a family map keyed by a canonical family string.
  - Every non-single mode falls back to `SingleSleeve` when it would produce `< 2` non-empty sleeves
    (a one-sleeve partition IS the inert path; documented, not an error).
  ```cpp
  // Deterministic sleeve assignment. SingleSleeve => the whole admitted set as ONE sleeve
  // (the R7 boundary pin). Multi-sleeve modes partition by library group / corr cluster /
  // signal family, each capped at cfg.max_sleeves, each falling back to SingleSleeve when
  // it cannot form >=2 non-empty sleeves. AlphaId order is ascending within each sleeve.
  Result<std::vector<fund::SleeveConfig>>
  assign_sleeves(const library::Library& lib, const MetaBookStageConfig& cfg);
  ```
- The per-sleeve `mh` `MultiHorizonConfig` (`multi_horizon.hpp:123-132`) is derived from the same
  gross/name-cap/rebalance/risk-aversion the single path uses today (H=1, identity horizon, minimal
  constraint set ⇒ the S1 reduction), so a single sleeve reduces to the deployed book.

**Determinism:** assignment is a pure function of `(lib, cfg)` — order-fixed enumeration, stable
cluster tie-break, no RNG, no clock. Same library + same cfg ⇒ same `std::vector<SleeveConfig>` (member
lists, tags, capacities identical).

**Accept:**
- `sleeve_assign_single_is_whole_set` (new `atx-engine/tests/fund/fund_sleeve_assign_test.cpp` OR
  `atx-impl/tests/metabook_test.cpp` — pick the home matching where `assign_sleeves` lives):
  `SingleSleeve` over a 6-alpha fixture library yields exactly ONE `SleeveConfig` whose `members` are
  `[0..5]` ascending.
- `sleeve_assign_corr_cluster_deterministic`: on a fixture with two constructed correlation clusters
  (three alphas each), `ByCorrCluster` yields exactly 2 sleeves with the expected memberships, and a
  second call yields byte-identical `members` (stable tie-break).
- `sleeve_assign_degenerate_falls_back`: a library where a mode would produce a single non-empty sleeve
  falls back to `SingleSleeve` (documented, `Ok`, not an error).
- Twice-run: same `(lib, cfg)` → identical `SleeveConfig` vector.

---

### S2-2 — NEW `stage_metabook` producer: two-pass drive (PASS 1 sleeves → PASS 2 Ω→c)

**Goal:** the NEW deploy stage that consumes the research panel + the combined/per-sleeve signals +
the library sleeve partition (S2-1) and produces a `MetaBookResult` by calling `MetaBook::run`. PASS 1
runs each sleeve's optimize independently (net-after-optimize, reusing the FROZEN per-sleeve
`MultiHorizonOptimizer` via `Sleeve::run`); PASS 2 builds the TRAILING cross-sleeve Ω (strictly-prior
sleeve P&L — CAUSALITY is the central trap) → `MetaAllocator` (ERC default; HRP + inverse-vol options)
→ capital weights `c[s]`. This is the producer; S2-3 (netting measurement) and S2-4 (report) surface
its outputs.

**Root cause:** `MetaBook::run` (`meta_book.hpp:170-175`) — the entire two-pass driver — has no
`atx-impl` caller (orphan table). The runnable path reads the ONE combo panel and runs a single MVO
(`stage_optimize.cpp:36-37`, `:229`); nothing builds sleeves, no trailing cross-sleeve covariance, no
per-sleeve capital allocation.

**Wiring:**
- NEW `atx-impl/src/stage_metabook.{cpp,hpp}`: `run_metabook(const RunConfig&, const
  MetaBookStageConfig&) -> Result<StageResult>` following the existing stage shape
  (`stages.hpp:14-27`: a `StageResult` with a digest + ordered kvs), plus an internal
  `build_metabook_result(...) -> Result<fund::MetaBookResult>` the tests call directly.
- Assemble the four `MetaBook::run` callbacks from the runnable panel + library (the load-bearing
  seam):
  - `sources_at(sleeve j, period p)` → `fund::/risk::HorizonSources` (`multi_horizon.hpp:116-118`):
    sleeve `j`'s per-member signal cross-sections at `p`, each paired with an identity `SignalHorizon`
    (H=1 boundary). The member signals come from evaluating the sleeve's member alphas over the panel —
    reuse the combine stage's evaluate path shape (`stage_combine.cpp:451-476`: `compile_batch` →
    `Engine::evaluate` → `extract_streams`) restricted to the sleeve's `members`.
  - `model_at(p)` → `const risk::FactorModel&`: the shared risk model. Default = `diagonal_risk_model`
    (`diag_risk.hpp:29`) — the SAME model the single MVO path uses (`stage_optimize.cpp:202`) — so the
    single-sleeve path pins to today's book. When the S1 `FactorModelArtifact` is present, use it (the
    cross-sprint seam below).
  - `returns_at(p)` → `span<const f64>`: the realized per-instrument simple return at `p` from the
    panel's `close` field, the SAME TRI-return convention `diag_risk.hpp:44-52` computes (drives Ω +
    the report, NOT the books — `meta_book.hpp:35-40`).
  - `cost` → `book::CostInputs`: the ONE calibrated cost model the sleeves + netting share, built
    exactly as `stage_optimize.cpp:213-215` builds it (`kappa` = turnover penalty,
    `round_trip_cost_bps` guarded by `--cost-bps`).
  ```cpp
  // PASS 1 runs each sleeve independently (net-after-optimize) via the FROZEN
  // MultiHorizonOptimizer; PASS 2 builds the TRAILING Ω (p<s ONLY — the causality trap),
  // allocates c[s] off Ω (ERC default), and nets. The stage supplies the four callbacks;
  // the driver owns the two passes. model_at defaults to diag_risk (== today's MVO model)
  // so SingleSleeve pins to the deployed book.
  fund::MetaBook mb;
  mb.cfg     = cfg.meta;                       // alloc + risk_lookback (meta_book.hpp:90)
  mb.sleeves = std::move(sleeves);             // from assign_sleeves (S2-1)
  ATX_TRY(auto result, mb.run(sched, sources_at, model_at, returns_at, cost));
  ```
- Serialize the netted `result.fund_books` to a books panel via the SAME `write_panel` +
  `.meta.txt` sidecar path `stage_optimize.cpp:79-125` uses, so `stage_report` can consume the fund
  book unchanged (a drop-in for the optimize output).

**Determinism / causality (R2):** PASS 1 sleeves are independent (`meta_book.hpp:13-14`). The trailing
Ω at `s` reads sleeve P&L STRICTLY before `s` (`meta_book.hpp:41-49`); at `s==0` the empty window ⇒ the
allocator empty/degenerate fallback (`meta_allocator.hpp:51-56`). The ERC solve is fixed-iteration
(`meta_allocator.hpp:99`). No RNG, no clock.

**Accept:**
- `metabook_two_sleeve_composes` (new `atx-engine/tests/fund/fund_metabook_wire_test.cpp`): on a
  fixture with two decorrelated sleeves, `run` returns a `MetaBookResult` whose `fund_books` are the
  netted `Σ_s c_s·w_s`, `capital` is length-`S` per period with `Σ|c| ≤ max_gross`, and
  `sleeve_results` has one `MultiHorizonResult` per sleeve.
- `metabook_single_sleeve_byte_identical` (the R7 pin — the single most important test): a ONE-sleeve
  `MetaBook` with the `c==[1]` boundary config produces `fund_books` byte-identical (via
  `std::bit_cast<u64>` element-wise, matching signed zeros — `meta_book.hpp:58-61`) to a standalone
  `MultiHorizonOptimizer::run` over the same fixture, AND to the pre-S2 `stage_optimize` book digest on
  the same combo.
- `metabook_pit_causality_guard` (the central trap): truncating the schedule after period `t` leaves
  every fund book at `p ≤ t` byte-identical; perturbing sleeve returns at `p ≥ s` does NOT change
  `c[s]` (no look-ahead in the trailing Ω).
- `metabook_allocator_method_dispatch`: with `meta.alloc.method` ∈ {InverseVol, ERC, HRP}, `allocate`
  is invoked with the matching kernel and `Σ|c| ≤ max_gross` holds for each; ERC is the default.
- Twice-run + seq==parallel (sleeve-order-independent PASS 1; period-parallel Ω build).

---

### S2-3 — Cross-sleeve netting: measure and assert the turnover crossing win

**Goal:** net the per-sleeve target books into ONE fund book per period and MEASURE the gross-turnover
reduction vs naive sleeve concatenation — the crossing benefit, a concrete turnover win that the
single-blend path (which never had separate sleeve books to cross) structurally cannot produce.

**Root cause:** `net_fund_book` (`netting.hpp:104-107`) — the crossing measurement — has no `atx-impl`
caller. The single-blend path fits ONE weight vector and trades the ONE resulting book; there is no
notion of offsetting sleeve flow crossing internally. Phase-D measured ~74%/week turnover; netting is
the built mechanism that reduces it (offsetting sleeve buys/sells cross INTERNALLY and never hit the
market — `netting.hpp:11-16`).

**Wiring:**
- `MetaBook::run` already calls `net_fund_book` per period internally (PASS 2), producing
  `FundReport.turnover_net` / `turnover_gross` / `crossing_benefit_bps` (`meta_book.hpp:120-122`). S2-3
  SURFACES these as stage telemetry and ADDS the assertion that netting reduces gross turnover vs the
  naive sum-of-sleeve-turnovers baseline:
  ```cpp
  // The crossing win: fund turnover_net (traded book) <= turnover_gross (sleeves traded
  // separately). The naive baseline is the sum of each sleeve's OWN turnover with NO
  // crossing == turnover_gross (netting.hpp:63). crossing_benefit_bps = (gross-net)*rt_bps.
  // Assert turnover_net <= turnover_gross per period (the triangle inequality, R3) and
  // report the total crossing benefit as a measured saving.
  const double net_total   = sum(report.turnover_net);
  const double gross_total = sum(report.turnover_gross);   // == naive sleeve-concat turnover
  ```
- Emit `fund_turnover_net` / `fund_turnover_gross` / `crossing_benefit_bps` / `crossed_fraction`
  (`netting.hpp:60-72`) into the stage `kvs` (telemetry only — never folded into the fund-book digest,
  mirroring the combine breadth/capacity telemetry convention, `stage_combine.cpp:735-736`).

**Determinism:** `net_fund_book` is a pure same-timestamp aggregation of already-known target books
(`netting.hpp:99-102`) — no realized return, no future bar, order-fixed ascending name then sleeve
(`netting.hpp:41-45`). Same inputs ⇒ byte-identical `NetResult`.

**Accept:**
- `metabook_netting_reduces_turnover` (new `atx-impl/tests/metabook_netting_test.cpp`): a
  two-sleeve fixture where sleeve A and sleeve B hold OFFSETTING positions in a shared name ⇒
  `turnover_net < turnover_gross` strictly (the crossing bites) and `crossing_benefit_bps > 0`.
- `metabook_netting_triangle_invariant`: on every fixture, `turnover_net ≤ turnover_gross` per period
  and `crossing_benefit_bps ≥ 0` (the R3 invariants, `netting.hpp:29-34`).
- `metabook_netting_single_sleeve_no_crossing`: one sleeve ⇒ `turnover_net == turnover_gross`,
  `crossing_benefit_bps == 0`, `crossed_fraction == 0` (no crossing possible; the R7 pin's netting
  branch).
- Twice-run on the netting telemetry.

---

### S2-4 — Fund report: Euler attribution-by-sleeve + Meucci effective-bets

**Goal:** surface the fund-level report — Euler attribution-by-sleeve (return / risk / crossing
components, each SUMMING to the fund total) + the Meucci effective-bets diversification gauge over
(Ω, c) — as stage telemetry, so the assembled mega-book reports WHY it holds what it holds and HOW
diversified it is.

**Root cause:** the fund report (`FundReport` + `SleeveAttribution`, `meta_book.hpp:98-131`) is
computed inside `MetaBook::run` but nothing consumes it. The single-blend path reports a scalar book
Sharpe; there is no by-sleeve decomposition and no effective-bets gauge because there are no sleeves.

**Wiring:**
- Surface `result.report.attribution` (`meta_book.hpp:130`) — `return_contrib` / `risk_contrib` /
  `crossing_credit`, one entry per sleeve — as stage telemetry, and ASSERT the SUM identities the
  driver guarantees:
  ```cpp
  // R4 sum identities (guaranteed by the driver; asserted here so a wiring regression is
  // caught): Σ_s return_contrib[s] == R_fund (the linear attribution key, meta_book.hpp:99);
  // Σ_s risk_contrib[s]   == sqrt(cᵀΩc) (Euler exactness, meta_book.hpp:103-104);
  // Σ_s crossing_credit[s] == total crossing benefit (meta_book.hpp:106-107).
  ```
- Surface `result.report.effective_bets` (`meta_book.hpp:129`) — the Meucci N_Ent diversification gauge
  over the FINAL (Ω, c) (0 when Ω is empty/degenerate) — and `fund_metrics.sharpe`
  (`meta_book.hpp:126`) into `kvs`.
- Emit `sleeve_return_contrib` / `sleeve_risk_contrib` / `sleeve_crossing_credit` (comma-joined,
  sleeve order) + `fund_effective_bets` + `fund_sharpe` into the stage `kvs` (telemetry — never folded
  into the fund-book digest).

**Determinism:** the report reductions are order-fixed; the Meucci PCA uses Eigen's deterministic
`SelfAdjointEigenSolver` (`meta_book.hpp:66-69`). Same inputs ⇒ byte-identical report.

**Accept:**
- `metabook_euler_attribution_sums` (in `atx-impl/tests/metabook_test.cpp`): on a two-sleeve fixture,
  `Σ_s return_contrib[s]` equals the realized fund return, `Σ_s risk_contrib[s]` equals `sqrt(cᵀΩc)`,
  and `Σ_s crossing_credit[s]` equals the total crossing benefit — each to a tight tolerance (the R4
  identities).
- `metabook_effective_bets_gauge`: a fund of TWO decorrelated equal-capital sleeves reports
  `effective_bets ≈ 2`; a fund whose two sleeves are perfectly correlated reports `effective_bets ≈ 1`
  (the diversification gauge behaves correctly at the boundaries).
- `metabook_report_single_sleeve`: one sleeve ⇒ `effective_bets` degenerate handling matches the
  empty/degenerate-Ω contract, and the report Sharpe equals the single-book Sharpe.

---

### S2-5 — Allocator-method config + off-path byte-identity + twice-run + seq==parallel (close)

**Goal:** expose the allocator method (HRP vs ERC vs inverse-vol) as a `MetaBookStageConfig` knob, land
the full four-class determinism battery for the on-path (multi-sleeve) mode, and record the NCO stretch
as explicit future work.

**Root cause:** the allocator method already dispatches inside `MetaAllocator`
(`meta_allocator.hpp:82-86`, `:149-155`) but nothing selects it from the runnable stage; and the
determinism battery (off-path byte-identity, twice-run, seq==parallel) must be proven end-to-end
through `stage_metabook`, not just in the engine unit tests.

**Wiring:**
- `MetaBookStageConfig.meta.alloc.method` selects `InverseVol` / `EqualRiskContribution` (default) /
  `HierarchicalRiskParity` (`meta_allocator.hpp:82-86`). No new code — the field already exists on the
  engine config; S2-5 confirms the stage threads it and adds the tests.
- Ledger records the NCO stretch (below) and the cross-sprint seam resolution (S1 covariance).

**Determinism (the mandatory close battery):**
- **off-path byte-identity** — `SleeveAssignment::SingleSleeve` (default) ⇒ the fund-book digest equals
  the pre-S2 optimize-stage golden (the R7 pin at the stage boundary).
- **on-path RED→GREEN** — a ≥2-sleeve `ByCorrCluster` run produces a book with a measured crossing
  turnover reduction (RED before the wire: no metabook stage exists; GREEN after).
- **twice-run** — same panel + same `MetaBookStageConfig` ⇒ byte-identical fund books + report kvs.
- **seq==parallel** — sleeve-order-independent PASS 1 and period-parallel Ω build yield identical
  fund books.

**Accept:**
- `metabook_alloc_method_erc_default`: default `method == EqualRiskContribution`; explicit
  `HierarchicalRiskParity` routes to `hrp_weights` (`meta_allocator.hpp:147`) and NEVER inverts Ω (a
  singular-Ω fixture that would trap an inverting allocator still returns a valid `c` via HRP).
- `metabook_offpath_byte_identical`: `SingleSleeve` default ⇒ digest unchanged vs the pinned
  optimize golden.
- `metabook_twice_run` + `metabook_seq_eq_parallel`: byte-identical books across two runs and across
  sleeve-order / period-parallel execution.
- Ledger records the NCO future-work stretch verbatim (see Out of scope).

---

## Sequencing

1. **S2-0 first** (config seam + FROZEN-signature confirmation + ledger marker) — every unit reads
   `MetaBookStageConfig` and the confirmed call surfaces.
2. **S2-1** (sleeve assignment) — the `SleeveSpec` seam; S2-2 consumes the `std::vector<SleeveConfig>`
   it produces.
3. **S2-2** (the producer / two-pass drive) — builds the `MetaBookResult`; S2-3 and S2-4 surface its
   outputs. S2-2 is the spine; it must land before S2-3/S2-4.
4. **S2-3** (netting measurement) and **S2-4** (report) in parallel after S2-2 (disjoint: S2-3 asserts
   turnover/crossing kvs, S2-4 asserts attribution/effective-bets kvs — different report fields, no
   shared edit).
5. **S2-5** (allocator-method config + close battery) last — proves the full determinism contract
   end-to-end through the stage and closes the sprint.

---

## Risks / guardrails

| Risk | Impact | Guardrail |
|---|---|---|
| `MetaBook::run` / `MetaAllocator::allocate` signature differs from assumed | S2-2 won't compile | Read the FROZEN signatures at kickoff (S2-0) and record them in the ledger: `run(sched, sources_at(sleeve,period), model_at(period), returns_at(period), cost)` (`meta_book.hpp:170-175`); `allocate(Omega, sleeve_vol, caps)` (`meta_allocator.hpp:125-127`). Assemble the callbacks to the ACTUAL signature. |
| Meta-book changes the default book | Golden drift; determinism contract broken | `SleeveAssignment::SingleSleeve` MUST reduce to the `c==[1]`, no-crossing, single-`MultiHorizonOptimizer` path — the R7 boundary pin (`meta_book.hpp:51-61`). The off-path byte-identity test (S2-2 `metabook_single_sleeve_byte_identical`, S2-5) is the gate; if it fails, the single-sleeve reduction leaked. |
| Trailing Ω leaks future sleeve P&L | Silent look-ahead inflates the scorecard (the CENTRAL trap) | Ω at `s` reads sleeve P&L STRICTLY `p < s` (`meta_book.hpp:41-49`); the PIT guard (S2-2 `metabook_pit_causality_guard`) truncates the schedule after `t` and asserts every `p ≤ t` book is byte-identical, and perturbs `p ≥ s` returns and asserts `c[s]` is unchanged. This is the mandatory causality gate. |
| An allocator that inverts Ω traps on a singular sleeve covariance | Crash / NaN capital weights | HRP NEVER inverts Ω (`meta_allocator.hpp:40`, `:144-147`); a degenerate Ω falls back to inverse-vol and still returns `Ok` (`meta_allocator.hpp:51-56`). S2-5's singular-Ω fixture pins that the fallback fires, not a throw. |
| Sleeve member signals diverge from the combine evaluate path | Sleeve book ≠ the single-blend book on the pin | S2-2 reuses the combine stage's evaluate shape (`compile_batch`→`Engine::evaluate`→`extract_streams`, `stage_combine.cpp:451-476`) restricted to the sleeve members, and `model_at` defaults to `diag_risk.hpp:29` (the SAME MVO model). The single-sleeve-whole-set case must reproduce the deployed book — the byte-identity test enforces it. |
| S2 accidentally edits a Sprint-3/4/5 file | Merge conflict / ownership violation | S2 owns ONLY `fund/*.hpp` (wiring), `combine/combined_source.hpp`, and NEW `stage_metabook.{cpp,hpp}` + tests. It does NOT edit `stage_combine.cpp` (S3), `stage_optimize.cpp` (S1), `stage_run.cpp`/`config.*` (S5), or any `src/fund/*.cpp` (frozen). The `run_all` wiring is handed to S5 as a seam note. |
| Library holdings / provenance unavailable for `ByCorrCluster`/`BySignalFamily` | Sleeve assignment can't form multi-sleeve partition | Each non-single mode falls back to `SingleSleeve` when it cannot form `≥2` non-empty sleeves (S2-1, documented `Ok`, not an error). The multi-sleeve modes are opt-in; the inert default never needs them. |

---

## Bench / acceptance (sprint close)

- **Default byte-identity:** the pinned `stage_optimize`/`stage_report` goldens are unchanged with the
  default `MetaBookStageConfig` (`SingleSleeve`) — the fund book equals the single-blend deploy book
  byte-for-byte (the R7 stage-boundary pin).
- **Per-task RED→GREEN:** each opt-in mode (multi-sleeve compose, netting, method dispatch) has a test
  RED before the wire and GREEN after.
- **Netting win, measured:** on the offsetting-sleeve fixture, record `turnover_gross` (naive
  sleeve-concat) vs `turnover_net` (crossed) and `crossing_benefit_bps` — the concrete, quantified S2
  claim that cross-sleeve netting reduces gross turnover (the Phase-D ~74%/week counter-mechanism).
- **Diversification, measured:** record `effective_bets` for {single-sleeve, two-decorrelated-sleeve,
  two-correlated-sleeve} funds — the two-decorrelated fund must show `effective_bets ≈ 2`, the
  correlated fund `≈ 1` (the crowding counter-mechanism, vs Phase-D `N_eff = 8.76`).
- **Euler exactness, measured:** record the three attribution sum-identities to a tight tolerance
  (`Σ return_contrib == R_fund`, `Σ risk_contrib == sqrt(cᵀΩc)`, `Σ crossing_credit == total benefit`).
- **Twice-run + seq==parallel** on the multi-sleeve path.
- **Dev-panel smoke ≤5 min** through `run_metabook` with a two-sleeve `ByCorrCluster` config (the CLI
  flag itself is threaded in Sprint 5; S2 proves the engine path via a direct-call integration test,
  not the CLI).

---

## Cross-sprint seam — the S1 covariance dependency (state explicitly)

S2's `MetaBook`/`MetaAllocator` take Ω as an INPUT and build it LOCALLY from sleeve-return P&L
(`sleeve_return_cov`, `cross_sleeve_risk.hpp:119`: a sample/pairwise covariance over the trailing
window). **S2 does NOT hard-depend on Sprint 1.** The `model_at(period)` callback (the SHARED
`FactorModel` every sleeve's PASS-1 optimize sees) is where S1 helps:

- **If S1 has landed:** `model_at` returns the S1 `FactorModelArtifact`'s per-window `FactorModel`
  (the cleaned factor covariance) — the sleeves size against a real cross-sectional covariance.
- **If S1 has NOT landed:** `model_at` returns `diagonal_risk_model` (`diag_risk.hpp:29`), the exact
  model the single MVO path uses today (`stage_optimize.cpp:202`). S2 builds the *sleeve-return* Ω
  locally regardless (Ω is a SLEEVE-level covariance, distinct from the instrument-level `FactorModel`
  V — `cross_sleeve_risk.hpp:33-47` documents the two distinct risk views), so the meta-book is fully
  functional without S1.

S2-2 wires `model_at` to prefer the S1 artifact when present and fall back to the diagonal model, and
records the dependency in the ledger. This mirrors the ROADMAP §Sequencing Wave-2 contract: "S2 and S3
benefit from S1's covariance but do not hard-block on it."

---

## Out of scope

- CLI flags `--metabook` / `--sleeve-assignment` / `--allocator-method` / `--risk-lookback` and the
  `run_all` `metabook` step — Sprint 5 (hub; owns `config.*` + `stage_run.cpp`). S2 ships the stage +
  a direct-call integration test + a `run_all` seam note.
- The covariance-source swap in `stage_optimize` and the S1 `FactorModelArtifact` producer — Sprint 1.
  S2 only CONSUMES the artifact through `model_at` when present.
- Editing `stage_combine.cpp` (the linear combiner) or `combine/combiner.hpp`/`regime_combiner.hpp` —
  Sprint 3. S2 may add a per-sleeve source seam in `combine/combined_source.hpp` (S2-owned) but does not
  touch the combine stage.
- The optimizer cap-clip-renorm dollar-neutrality fix, capacity-in-selection, and √-impact charging —
  Sprint 4. S2 uses the ONE calibrated `book::CostInputs` for netting-benefit pricing exactly as the
  optimize path builds it; it does not re-price impact.
- Re-deriving any `src/fund/*.cpp` estimation / allocation / netting math — FROZEN; S2 calls it.
- **NCO (Nested Clustered Optimization, López de Prado 2019a)** as the meta-allocation successor to
  HRP/ERC — inner-cluster weights × cross-validated outer-cluster weights, reducing estimation error
  further than HRP. It would consume Sprint 1's cleaned covariance + clustering. **Explicit future-work
  stretch**, deferred to a p8-S2 stretch unit or the next module (ROADMAP §Future-work backlog); HRP/ERC
  ship first, honestly measured. S2 ships nothing NCO in the critical path.
- Meta-labeling / triple-barrier / sample-uniqueness weighting — greenfield second-stage build, not
  wiring; a successor module (ROADMAP anti-roadmap §7).
