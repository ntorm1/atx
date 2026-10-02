# Task MINE-MEM report: the mining verb's memory (PM5-10) and MINE-16

Lane MINE-MEM, wave AG, worktree `C:/atx-wt/pool-8`, branch `feat/platform-v8-minemem-20261001` from `20e7bd19`.
Nothing was built in C++ (lane rules): every C++ line is written for clang-cl `/W4 /WX`, re-read against the owning
headers, and is compiled first by root. No real data, nothing dated 2024-01-01 or later, no subagents, no push.

## Status

DONE_WITH_CONCERNS. Both tasks are done. The PM5-10 target of 2,560 MiB is **not** reached inside this lane's files.
The 4-year default campaign now peaks at **5,319 MiB at 1 and at 4 workers** (the model was 11,111 MiB at 4 workers
and 8,421 MiB at 1). The section "What stands in the way of 2,560 MiB" names the remaining terms.

The first real campaign (MINE-RUN's `mine-c1`) needs **6,296 MiB at 1 and at 4 workers** with 53 members. That is
inside the runner's 8,192 MiB cap and the spec's 7,680 MiB ceiling. The old model (9,426 MiB at 4 workers) refused
it. See "The first real campaign".

| task | commit | what |
|---|---|---|
| 1 memory (PM5-10) | `449a78a8` | phase model `mine_memory`; members loaded after the search; fitness released before the promotion; full engines released for each race; rung panels released after each race |
| 2 MINE-16 | `86be4ab1` | `SearchResult::rung_failed_hashes`; trial status `rung-failed`; `campaign.json` `trials.rung_failed`; the fixture asserts 0 |
| 3 report | this commit | this file. Also two `RecordProperty` lines in `SameSeedSameChainHeadAtOneAndFourWorkers` that help root check identity. Also the PM note: the shortlist term is now the cap plus one panels (Ruling PM5-9), and the first-campaign shape is pinned in the memory test. |

## 1. Memory (PM5-10)

### What was built

- `atx-impl/src/strategy_mine.hpp`, `.cpp`:
  - New `struct MineMemory`, one field per term. Its methods are `resident()`, `search()`, `promotion()` and `peak()`.
  - New `mine_memory(const MineFootprint &) -> Result<MineMemory>`.
  - `mine_working_bytes(footprint)` now returns `mine_memory(footprint)->peak()`.
  - The verb still refuses before any payload when the peak is above `--max-memory-mib`.
  - `campaign.json` gains `search.memory {resident, search, promotion}` beside `search.required_bytes`.
- `strategy_mine.cpp`:
  - New `run_search` (anonymous namespace) runs both stages and owns the discover fitness. The fitness is destroyed
    when `run_search` returns, which is before the promotion.
  - `run_mine` loads the pool members after `registered_trials` and before `create_directory` and
    `record_campaign`.
- `atx-impl/src/strategy_mine_pool.hpp`, `.cpp`: `load_mine_pool` is replaced by two calls.
  - `bind_mine_pool(manifest, role)` loads the regressors. It also streams each member payload once through a 1 MiB
    buffer and keeps nothing: it checks the extent, the SHA-256 and that no value is infinite, in
    `load_pinned_f64` / `load_rows` order and words.
  - `load_mine_pool_members(manifest, role, pool)` loads the members, verifying each one again as it reads it.
- `atx-engine/src/factory/search_driver.cpp` (header comments in `search_driver.hpp`):
  - New `make_full_engine(panel, mask)` is used by `run()` and by the rebuild below.
  - On the signal-fitness path with racing on, `evaluate_generation` releases the full-pass engines before
    `fidelity_reject`. After the race it clears `rung_panels_` / `rung_keys_` and rebuilds the engines.

### The model, term by term

Notation: C = dates x names; H = dates x ceil(names / 2); F = 3 + extras; S = 8; W workers; R rungs; G regressors;
M members; K shortlist; T trials; P prior records. A workspace is 128 B per name plus 80 B per date.

| phase | term | bytes | allocation it stands for |
|---|---|---|---|
| resident | metadata | 64 MiB | library, catalogue, populations, genomes, I/O buffers, JSON (flat allowance, unchanged) |
| resident | role | `research_role_bytes` | role reader (26 B/cell), extras 8 B/cell each, guard 4, overlay mask 1 |
| resident | regressors | G C 8 | regressor payloads; the fitness and the confirm read borrow them |
| resident | trial_reads | T (8 dates + 16 KiB) | each full read's daily h21 IC, genome, log rows |
| resident | registry | 512 KiB + 128 (P + T) | Gram and record index (alive only while recording; counted throughout) |
| search | discover_cache | 49 C + 32 dates | IC labels and ranks (3 horizons x 16 B), member rows (1 B), per-row counts (4 x 8 B) |
| search | discover_workspaces | W workspaces of names | `ResearchIcWorkspace` |
| search | rung_caches | R (48 H + 32 dates) | the rung scorers' IC caches (no member rows) |
| search | rung_workspaces | W R workspaces of ceil(names / 2) | |
| search, bind | bind_transient | H (8 F + 6) | one rung's strided panel, member and guard while `prepare_rungs` builds its cache; no engine exists yet |
| search, race | race_panels | R H (8 F + 1) | the driver's strided rung panels (fields + universe) |
| search, race | race_engines | W R H (8 S + 1) | rung engines: slot pool + mask copy |
| search, race | race_signals | W H 8 | one rung signal set per worker |
| search, full | full_engines | W C (8 S + 1) | full-pass engines: slot pool + mask copy |
| search, full | full_signals | W C 8 | one signal set per worker |
| promotion | members | M C 8 | loaded after the search |
| promotion | shortlist | (K + 1) C 8 | the signals the rho step and the confirm read hold at once. PM note: Ruling PM5-9 streams the rho step over the whole above-hurdle list with at most cap + 1 panels held. **To reconcile at merge** with MINE-STAT. |
| promotion, engine | promotion_engine | C (8 S + 1) | `evaluate_signals`' engine |
| promotion, rho | rho_rows | (M + K) names 8 + (M + K)^2 16 + 16 names | rank rows, pair sums and counts, sort buffer |
| promotion, confirm | confirm_cache | 49 C + 32 dates + one workspace | the confirm scorer |

The phases combine as follows:

- **Search** = fitness (the four `discover_*` / `rung_*` terms) + max(bind, race, full).
- **Promotion** = members + shortlist + max(engine, rho, confirm).
- **Peak** = resident + max(search, promotion).

### Before and after: the 4-year default (1,405 x 6,100, 16 fields, G 3, M 32, R 1, K 16, T 272)

Before is the MINE-10 sum at `20e7bd19`:

| term (old) | W = 1 | W = 4 |
|---|---|---|
| metadata | 67,108,864 | 67,108,864 |
| role | 1,379,569,236 | 1,379,569,236 |
| pool (regressors + members) | 2,399,740,000 | 2,399,740,000 |
| 2 IC caches | 839,909,000 | 839,909,000 |
| rung scorers | 882,761,500 | 882,761,500 |
| rung panels | 655,643,250 | 655,643,250 |
| engines (W + 1 full, W R rung) | 1,392,706,250 | 3,899,577,500 |
| signal sets | 102,846,000 | 411,384,000 |
| scratch | 2,679,600 | 8,038,800 |
| trials | 7,513,728 | 7,513,728 |
| registry | 559,104 | 559,104 |
| shortlist | 1,097,024,000 | 1,097,024,000 |
| rank rows | 2,342,400 | 2,342,400 |
| **total (sum)** | **8,830,402,932 (8,421.3 MiB)** | **11,651,171,382 (11,111.4 MiB)** |

After is the phase peak at this report's commit, with the shortlist term at K + 1 (PM note). At `86be4ab1` it was
K: shortlist 1,097,024,000, promotion 3,848,154,500, peak 5,508,597,432 (5,253.4 MiB). Values are bytes, with MiB in
brackets.

| term (new) | W = 1 | W = 4 |
|---|---|---|
| metadata | 67,108,864 | 67,108,864 |
| role | 1,379,569,236 | 1,379,569,236 |
| regressors | 205,692,000 | 205,692,000 |
| trial_reads | 7,513,728 | 7,513,728 |
| registry | 559,104 | 559,104 |
| **resident** | **1,660,442,932 (1,583.5)** | **1,660,442,932 (1,583.5)** |
| discover_cache | 419,999,460 | 419,999,460 |
| discover_workspaces | 893,200 | 3,572,800 |
| rung_caches | 205,736,960 | 205,736,960 |
| rung_workspaces | 502,800 | 2,011,200 |
| bind_transient | 677,069,500 | 677,069,500 |
| race_panels | 655,643,250 | 655,643,250 |
| race_engines | 278,541,250 | 1,114,165,000 |
| race_signals | 34,282,000 | 137,128,000 |
| full_engines | 557,082,500 | 2,228,330,000 |
| full_signals | 68,564,000 | 274,256,000 |
| **search** | **1,595,598,920 (1,521.7)**, race binds | **3,133,906,420 (2,988.7)**, full pass binds |
| members | 2,194,048,000 | 2,194,048,000 |
| shortlist (K + 1) | 1,165,588,000 | 1,165,588,000 |
| promotion_engine | 557,082,500 | 557,082,500 |
| rho_rows | 2,476,864 | 2,476,864 |
| confirm_cache | 420,892,660 | 420,892,660 |
| **promotion** | **3,916,718,500 (3,735.3)** | **3,916,718,500 (3,735.3)** |
| **peak** | **5,577,161,432 (5,318.8 MiB, 5.19 GiB)** | **5,577,161,432 (5,318.8 MiB, 5.19 GiB)** |
| resident + search | 3,256,041,852 (3,105.2) | 4,794,349,352 (4,572.2) |

The old sum overstated what the old code held at once. Counted phase by phase, the old code peaked at 8,621,077,602 B
(8,221.7 MiB) at 4 workers and 6,135,729,852 B (5,851.5 MiB) at 1. Of the drop from 11,111 to 5,319 MiB:

- about 2,900 MiB is real allocation the code no longer holds at once (8,222 to 5,319 MiB at 4 workers);
- the rest is a sum replaced by the peak of the phases.

The test `StrategyMine.WorkingBytesAreThePeakOfThePhases` pins every term in the W = 4 table, the phase sums, and
the peak at W = 1 and W = 4. It also pins:

- the first real campaign's shape (below);
- the fixture campaigns: 95,671,872 B at 1 worker and 101,584,960 B at 4;
- the configuration bounds: 517,571,694,848 B, refused.

### The first real campaign (PM note; MINE-RUN `scripts/specs/v9/mine-c1.json`)

The shape: 1,405 x 6,100, 12 fields, 1 regressor (the book composite), 49 to 53 members, the 132 stage-1 templates
(stage 2 off, racing off), cap 16, a fresh registry. Values are MiB.

| | before (old sum), W = 1 | before (old sum), W = 4 | after, W = 1 | after, W = 4 |
|---|---|---|---|---|
| 49 members | 7,371.4 | 9,163.9 | **6,034.4** | **6,034.4** |
| 53 members | 7,633.1 | 9,425.6 | **6,295.9** | **6,295.9** |

Phases at 53 members:

- resident 1,187.5 MiB: the role is 1,054.1 MiB, of which the 12 extras are 785 MiB;
- search 998.1 MiB at W = 1 and 2,790.6 MiB at W = 4 (full engines 2,125.1);
- promotion 5,108.4 MiB: members 3,465.5, shortlist (17 panels) 1,111.6, engine 531.3.

The promotion is the peak at both worker counts, and the members dominate it.

- Under the new model the campaign is admitted at 4 workers within the runner's 8,192 MiB cap and the spec's
  "at most 7,680" `max_memory_mib`. Set `--max-memory-mib` to at least 6,296.
- The old model gave 9,426 MiB at 4 workers and would have refused it.
- The room left at 4 workers under 7,680 MiB is about 74 members.
- The test pins resident 1,245,173,652, search 1,046,539,160 (W = 1) and 2,926,158,260 (W = 4), promotion
  5,356,562,500, and peak 6,601,736,152 at both worker counts.
- Streaming the members by date in the rho step (see below) would bring this campaign's peak to 3,978 MiB, which is
  then search-bound at 4 workers.

### Why no bit can move (one argument per change)

1. **`make_full_engine` in `run()`.** It performs the same operations as before, in the same order:
   `std::make_unique<alpha::Engine>(panel_)`, then `apply_mask(*engine, cfg.cross_section_mask, panel_, Rung{})`. A
   failure gives the same `signal_path_error` text. On the legacy path the mask is empty, so `apply_mask` returns at
   once.
2. **Full engines released before each race and rebuilt after it** (signal-fitness path only).
   - `Engine::evaluate`'s output depends only on (program, panel, mask, eval mode, kernel policy). This is the
     driver's EVAL-PATH NOTE. Worker invariance already relies on it: which worker's engine evaluates a program
     depends on scheduling, so no result can depend on an engine's history.
   - The rebuilt engine gets the same panel and mask, through the same function, with the default mode and policy.
     Those are the values `run()` gives, and nothing in the driver changes them.
   - The release and the rebuild are serial, between parallel regions. They draw no RNG and reorder nothing.
   - The race's own engines are untouched.
3. **Rung panels cleared after each race.** `strided_panel` is a pure function of (`panel_`, strides). Before this
   change, stage 2's new driver already rebuilt the panels from scratch, so a rebuild per generation yields the same
   bytes.
4. **Members checked before the search, loaded after it.**
   - No input of the search refers to a member. `ResearchIcFitnessInputs` holds the panel, window, membership mask,
     guard and regressors.
   - The promotion reads the same bytes, verified by SHA-256 as they are read.
   - A bad member is still refused before the search, with the same words. The streamed check runs extent, read,
     extent again and SHA-256, then the infinity check, which is `load_rows`' order.
   - The only new refusal point is a member file that changed between the check and the load. It is refused before
     anything is written: no OUTPUT directory and no registry append.
5. **Fitness released before the promotion.**
   - `PromotionContext` holds role, pool, regressors, windows and floors. Nothing in it points into the fitness.
   - The trials point into `StageRun`, which `run_mine` owns through `MineSearch` and which is never moved after
     `classify`.
   - The `Library` (which genome ops borrow) stays in `run_mine` and outlives both stages.
   - `run_search` repeats the old statements in the old order, so the progress lines are byte-identical.
6. **The model.** It is arithmetic only, and it changes two things:
   - the admission threshold;
   - `campaign.json` `search.required_bytes`, plus the new `search.memory` block.

   Neither value enters the recipe (`recipe_sha256`), `trials.csv`, the registry or `mined_members.json`.

### Engine golden

`0x889874a3b9b29c55` is the legacy ScalarRaw search digest (`NsgaSearch.ScalarRaw_ReproducesGoldenDigest`,
`SignalFitnessDefaults.*`). It is pinned at 1 and 4 workers. Every change in `search_driver.cpp` is gated on
`signal_on` / `signal_fitness != nullptr`, except `make_full_engine` in `run()`, which is identical (item 1). The
golden is not edited, and by construction it holds.

The mining fixture's own registry head is pinned nowhere to a value. It is pinned equal across runs and at 1 and 4
workers (`SameSeedSameChainHeadAtOneAndFourWorkers`). That test now records `fixture_registry_head` and
`fixture_trials_csv_sha256` as gtest properties, so root can compare this build with a later one.

### What stands in the way of 2,560 MiB

- **Promotion (3,735 MiB + 1,583 resident = the peak at both worker counts).** The terms are
  `strategy_mine_promote.cpp`'s, owned by MINE-STAT:
  - the 32 members as full panels: 2,092 MiB, although `rho_check` reads only the discover rows;
  - the cap + 1 signals as full panels: 1,112 MiB;
  - the promotion engine (531 MiB), which coexists with the members although the rho step comes after it.

  If `rho_check` streamed each member's discover rows date by date from its verified file (O(M x N) held), the
  promotion would fall to 1,643 MiB. The peak would then be 3,226 MiB at 1 worker and 4,572 MiB at 4: the search
  becomes the peak.
- **Search at 4 workers (2,989 MiB + resident).** The VM slot pools are W x 8 slots x 65.4 MiB = 2,092 MiB. Both
  the bound (`kMineMaxProgramSlots` 8) and float64 are fixed by plan section 13 and MINE-10.
  - A shared slot budget would not change any bit. Under it, each worker acquires `num_slots` panels before
    `evaluate` and releases them after; results do not depend on worker scheduling. Its floor is S + 1 panels.
    It is not built: it needs a blocking admission inside `DetPool` jobs and a VM pool release, which is too
    invasive for an uncompiled lane.
- **Resident (1,583 MiB).** The 16 extra fields are copied: 1,046 MiB. The IC runner does **not** map fields. It
  copies them through `load_pinned_f64`, and only for the fields a candidate reads (`FieldResidency`). The brief's
  "mapped where the IC runner already does so" therefore has no precedent to follow.
  - Mapping (`atx::tsdb::Mapping`, borrowed overlay columns) would take them off the private commit. They would
    still be in the working set, because every evaluation touches them.
  - It is a change to the shared loader `strategy_research_role.cpp`, outside this lane.
  - Whether page-cache bytes count against the cap is an owner question.
- **Estimated ladder (not built, not proven).**

  | step | peak at 1 worker | peak at 4 workers |
  |---|---|---|
  | members streamed by date in promote | 3,226 MiB | 4,572 MiB |
  | ... plus the extras mapped and not counted | 2,180 MiB (fits) | 3,526 MiB |
  | ... plus a 9-panel slot budget | — | about 2.2 GiB |

**Smallest footprint reachable within this lane without changing a bit:** 5,319 MiB at 1 and at 4 workers
(promotion-bound; 5,253 MiB with the K-panel shortlist term of `20e7bd19`). The search phase alone is 3,105 MiB at
1 worker and 4,572 MiB at 4.

### The brief's suggestions, item by item

| suggestion | outcome |
|---|---|
| one read-only role panel shared by all workers | Already so: every engine borrows the one role panel. What each worker copies is the eligibility mask (C bytes per engine, 33 MiB at W = 4 in the full pass). Sharing it needs a borrowed-mask API in `alpha/vm.hpp`, which is included almost everywhere; not done, and not at the peak. |
| engine scratch sized by the slots a program uses | The pool already grows to the program's slots. The admission cannot know a stage-2 program's slots before the payload, so S = 8 stays the bound. Done instead: the full engines no longer coexist with the race (saves W C (8 S + 1) during every race). |
| per-worker buffers reused across trials | Already so: IC workspaces, VM pools and scratch are per worker and reused. A signal set is one per worker at a time. Reusing its buffer would not lower the peak. |
| the pool stored once | Already stored once. Now the members are held only for the promotion, and the search holds regressors only. |
| fields mapped rather than copied where the IC runner does so | The IC runner does not map (see above). Not done. |

## 2. MINE-16

- **Engine.**
  - `signal_rung_score` is now `signal_rung_read -> RungRead {score, failed}`. `failed` is set on a compile failure,
    a VM `evaluate` failure (including an empty signal set) and a functor Err. A rejected or non-finite score is a
    read that lost, not a failure.
  - The race still receives exactly the NaN it received before.
  - Each worker appends to its own list (`failed_by_worker[wid]`), which has a single owner. The lists are merged
    after the race.
  - `SearchResult::rung_failed_hashes` holds the rejected candidates whose read failed. It is sorted at the merge
    and is a subset of `fidelity_rejected_hashes`. It is empty when a race rejects nothing (fail-open).
  - The legacy evaluator is unchanged.
- **Verb.**
  - `TrialStatus` gains `RungFailed`, placed between `RacingRejected` and `Failed`. The order means best first, so
    an expression seen in both stages keeps racing-rejected.
  - `classify_one` checks `rung_failed_hashes` before `fidelity_rejected_hashes`.
  - The status is `rung-failed` with reason `rung-failed`. The registry record is screened at fidelity 1, as a
    racing rejection is.
  - `Counts` gains `rung_failed`, and `campaign.json` `trials` gains `rung_failed`.
  - The registry identity is n_raw = evaluated + screen_rejected + racing_rejected + rung_failed + failed.
- **Tests.**
  - The fixture's five-seed test asserts the identity with `rung_failed` and `rung_failed == 0` (failed stays 0).
  - `StrategyMine.RungFailuresAreTheirOwnTrialStatus` covers `classify`, counts, the CSV row and the two-stage
    merge.
  - Engine `SignalFitnessPath.RungFailuresAreListedApartFromRacingRejections` covers a VM failure (`rank(absent)`)
    and a functor Err at the rung against a scored rejection. It also checks that both runs reject the same set and
    fold the same digest.
- **Golden.**
  - The legacy path is untouched, so `0x889874a3b9b29c55` holds.
  - On the fixture, a trial's registry record changes only if it becomes rung-failed (reason tag; fidelity stays 1).
    The fixture asserts none does, so its registry bytes and head are those of the code before this task.
  - If root's run fails `rung_failed == 0`, that is the case the brief says to STOP on. It would mean the fixture
    has a rung VM or functor failure that MINE-16 now exposes. Report it and do not edit the assertion.

## How root verifies

- **`check`** (single TU):
  - `atx-engine/src/factory/search_driver.cpp`
  - `atx-impl/src/strategy_mine.cpp`, `strategy_mine_pool.cpp`, `strategy_mine_trials.cpp`
  - `atx-impl/tests/strategy_mine_test.cpp`
  - `atx-engine/tests/factory/factory_signal_fitness_test.cpp`
- **Build:**
  - `atx-engine-factory-tests atx-impl-strategy-mine-tests atx-equity-strategy-mine`.
  - `search_driver.hpp` changed (a new `SearchResult` field), so every target that includes it rebuilds. Also
    build `atx-impl-tests`, which includes `stage_discover`.
- **gtest:**
  - `atx-engine-factory-tests --gtest_filter=SignalFitness*:NsgaSearch.*:FactoryFidelity*:ResearchIc*`, then the
    whole binary.
  - The whole `atx-impl-strategy-mine-tests`, in particular `StrategyMine.WorkingBytesAreThePeakOfThePhases`,
    `StrategyMineCampaign.ModelTermsAreTheFixtureAllocations`, `StrategyMine.RungFailuresAreTheirOwnTrialStatus`,
    `StrategyMineCampaign.PromotesThePlantedSignalsOnlyInFiveSeeds` and
    `StrategyMineCampaign.SameSeedSameChainHeadAtOneAndFourWorkers`.
  - Add `--gtest_output=xml:<file>` to the last of these to keep the fixture head.
- **Golden at 1 and 4 workers:**
  - `SignalFitnessDefaults.ExplicitDefaultsKeepTheGoldenDigestAtEveryWorkerCount` (1 and 4),
    `SignalFitnessDefaults.ImplicitDefaultsKeepTheGoldenDigest` and `NsgaSearch.ScalarRaw_ReproducesGoldenDigest`.
  - The fixture's heads are equal at 1 and 4 workers in `SameSeedSameChainHeadAtOneAndFourWorkers`.
  - The signal path's rebuild is equal at 1 and 4 workers and on a re-run in
    `SignalFitnessPath.RebuiltEnginesKeepTheMaskAndTheRunAcrossGenerations`.
- **Fixture allocations:**
  - `ModelTermsAreTheFixtureAllocations` measures the real objects. These are equal to their terms: pool payloads,
    the strided rung panel, the strided member and guard (bind transient), and a signal set.
  - These are at most their terms: the discover, confirm and rung IC caches with member rows, and each template's VM
    slot pool plus mask.
- **pytest:** no Python changed. The MINE-FIX files still pass: `scripts/tests/test_research_ledger.py`,
  `atx-impl/tools/test_trial_ledger_rules.py`, `atx-impl/tools/test_mine_overlap_factor.py`: 16 passed.

## Deviations from the brief

- The target of 2,560 MiB is not met. The smallest reachable footprint and its blockers are stated above.
- "The model equals the sum of the allocation sizes on the fixture" is met term by term:
  - equality where an allocation is a function of the geometry;
  - an upper bound where it depends on the window rows (an IC cache: label rows <= dates, unknowable before the
    payload) or on the program (slots <= S).

  The total is the phase peak, not a sum.
- The fixture test assertions: MINE-16 is asserted in `PromotesThePlantedSignalsOnlyInFiveSeeds`, and the memory
  admission in `SameSeedSameChainHeadAtOneAndFourWorkers`. Both are lines in existing fixture tests, outside the
  memory and rung-failure sections.
- The shortlist term is K + 1 panels (PM note, Ruling PM5-9).
  - Against `20e7bd19`'s `promote`, which holds K, it is one panel over: 65.4 MiB on the 4-year role.
  - Against MINE-STAT's in-progress streamed `rho_step` (pool 9, read only), whose batches keep held + batch <= K
    signals, it is also one panel over.
  - The term is therefore a bound, not an equality, for this one panel.

## Term to reconcile at merge (PM note)

`MineMemory::shortlist = (max_promotions + 1) x C x 8` and `rho_rows` with M + K rows (`rho_check`'s
`PairwiseRowCorrelation` over members + held + batch <= M + K rows). At the MINE-STAT merge, root checks two things
against the merged `strategy_mine_promote.cpp`:

- No point in `rho_step`, `evaluate_signals` or `confirm_reads` holds more than K + 1 signal panels.
- `rho_check` ranks at most M + K rows.

If either check fails, the term changes with it in `mine_memory` and in the pinned test values: shortlist,
promotion, peak, the first campaign, and fixture W = 1. Otherwise the admission is no longer a bound.

## Cross-lane edits

- `atx-impl/tests/strategy_mine_test.cpp` is shared with MINE-STAT:
  - My new tests sit in the memory section, after the arithmetic test, and in a new rung-failure section that
    follows it.
  - Includes are added in one marked block after the existing ones, plus `<unordered_map>`.
  - One assertion line is changed in each of two fixture tests (above).
  - The rule section, where MINE-STAT appends, is untouched.
- No edit to `strategy_mine_rule.*`, `strategy_mine_promote.cpp` or any Python.

## Open risks

1. **MINE-14 / MINE-STAT (PM5-9).** The promotion terms bound at most K + 1 signals, the engine during
   `evaluate_signals`, then rho, then confirm. See "Term to reconcile at merge".
2. The C++ is uncompiled. The fixture expectation `rung_failed == 0` and every pinned byte count rest on reading and
   on the Python mirror of the formulas. The mirror is in the session scratchpad and not committed; the C++ test
   pins its values.
3. **Performance on the signal path with racing.** Each generation now re-allocates the full engines' slot pools and
   rebuilds the strided rung panels: about 2.1 GiB and 625 MiB of first-touch per generation at 4 workers. That is
   seconds against the evaluation, and it does not affect the legacy path.
4. **Terms still not derived.**
   - The 64 MiB metadata stays a flat allowance.
   - Engine scratch (O(dates + names) per engine) and `sizeof` overheads are folded into it.
   - The registry term is counted in every phase although it lives only while recording (0.5 MiB).
