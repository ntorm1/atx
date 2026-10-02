# Task MINE-JOIN report: one reconciled SHA of MINE-MEM, MINE-STAT and MINE-RUN, members streamed by date

Lane MINE-JOIN (wave AG, Ruling PM5-15). Worktree `C:/atx-wt/pool-8`, branch `feat/platform-v8-minejoin-20261001`, from
`57bb5abb` (MINE-MEM's head). No C++ built (lane rules): every C++ line is written for clang-cl `/W4 /WX` and re-read
against the owning headers; root compiles it first. No real data, nothing dated 2024-01-01 or later, no subagents, no
push. `git merge` of the two lane SHAs ran inside this worktree only (PM5-15).

## Status

DONE_WITH_CONCERNS. All six tasks are done. The concerns: PM5-10's 2,560 MiB is still not met by the default 4-year
campaign (3,161 MiB at 1 worker, 4,572 at 4; see "What still stands in the way"); the C++ is uncompiled; the prereg's
D5 reason no longer holds under MINE-STAT's factors (flagged for the owner, value unchanged).

| task | commit | what |
|---|---|---|
| 1 merge MINE-STAT | `96f4422a` (merge), `3a5a7969` | one textual conflict (header comment); a full read of the merged files; four loop indices renamed |
| 2 merge MINE-RUN, align | `ead9e6bb` (merge), `93171677` | research_mine.py, its tests, the spec template, runbook and prereg on F / Fc, the ceiling, rung-failed, the footprint |
| 3 stream the members | `4c03bc8b` | `stream_mine_pool_members`; `rho_check` reads members date by date; same bits |
| 4 memory model | `11399a3f` | shortlist K panels, `member_rows` replaces `members`; pins recomputed |
| 5 pytest | (no commit) | 181 passed, 3 skipped |
| 6 report | this commit | |

## 1. Merge of MINE-STAT (`c4bd8d09`)

`git merge --no-ff c4bd8d09` conflicted in one file only:

- `atx-impl/src/strategy_mine.hpp`: the header comment block. Kept both paragraphs: MINE-STAT's label-overlap text (F
  by budget band, Fc by m; tables in the recipe, F in the hurdle, Fc per promotion) and MINE-MEM's memory-by-phase
  text.
- `strategy_mine.cpp`, `strategy_mine_detail.hpp` and `strategy_mine_test.cpp` merged without a textual conflict. They
  touch disjoint hunks: MINE-STAT's `check_config` ceiling text, `bands_json`, recipe keys, `context.overlap_factor`,
  `promotions_json(..., overlap_factor)`, and usage; MINE-MEM's `run_search`, the phase model and the member load.
  Both `PromotionContext` edits survive (MINE-MEM added none; MINE-STAT added `overlap_factor`).

Read afterwards, top to bottom, as a compiler would: `strategy_mine.{hpp,cpp}`, `strategy_mine_detail.hpp`,
`strategy_mine_rule.{hpp,cpp}`, `strategy_mine_promote.cpp`, `strategy_mine_pool.{hpp,cpp}` and
`strategy_mine_test.cpp` (all 1,680 lines). Declarations match calls:

- `promote(trials, hurdle, context)`;
- `promotions_json(trials, promotions, pool, context.overlap_factor)`;
- `mined_shortlist(reads, hurdle, factor)`;
- `mined_rho_select(rho, fixed, candidates, min_dates, cap)`;
- `bands_json(std::span<const MinedFactorBand>)` from `std::array`;
- `ATX_TRY` in loops.

Lambda captures are all used. Clang's `-Wunused-lambda-capture` is in `-Wall`, so `/W4` reports an unused one. There
are no sign conversions in the merged hunks.

One fix (`3a5a7969`): four MINE-STAT rule tests looped with `for (int day = 0; ...)`. That shadows the file-scope
`constexpr i64 day`, an error under the non-MSVC `-Wshadow -Werror` branch (clang-cl `/W4` does not enable
`-Wshadow`). They are renamed to `d`. Not fixed (pre-existing at `20e7bd19`, same class):

- `const auto missing` in `RefusesACampaignWithoutTheBook`;
- the 126-column line 23 of `strategy_mine.hpp`.

The MINE-MEM comment "95,435,840 B at 1 worker" in `SameSeedSameChainHeadAtOneAndFourWorkers` was stale already (it is
the K-panel value); task 4 restates it.

## 2. Merge of MINE-RUN (`6ea76460`) and alignment

The merge brought new files plus the five-line mine dispatch in `research_cycle.py`, with no conflict. Aligned to what
MINE-STAT and MINE-MEM produced:

- **`scripts/research_mine.py`**
  - `factor_tables()` imports `OVERLAP_BANDS` and `CONFIRM_BANDS` from `atx-impl/tools/mine_overlap_factor.py`, which
    `test_mine_overlap_factor.py` pins to `kMinedOverlapBands` and `kMinedConfirmBands`. The import is on first use,
    like `backtest_integrity`.
  - `band_factor`, `overlap_factor(budget)`, `confirm_factor(m)` and `raw_hurdle(budget)` mirror
    `strategy_mine_rule.cpp`.
  - `max_budget()` already reads `MINED_MAX_BUDGET`, now 10,000.
  - `plan` / `run` header: it prints F of the budget's band, the raw discover t, and Fc by m up to the cap.
  - `mechanics` reads the keys as MINE-STAT codes them:
    - `hurdle.t` = z(budget);
    - `hurdle.overlap_factor` = F(budget);
    - `hurdle.max_budget` = `recipe.max_budget` = 10,000;
    - `recipe.overlap_bands` and `recipe.confirm_bands` = the tables;
    - every promotion with `confirm_read` carries `confirm_factor` = Fc(m), m = the count of confirm reads, and every
      other promotion carries null. m is checked and never printed, because it is a result (runbook step 11).
  - The registry identity is now exactly the five statuses `evaluated`, `screen_rejected`, `racing_rejected`,
    `rung_failed` and `failed`. A verb without `rung_failed` is refused. With racing off, `racing_rejected` = 0 and
    `rung_failed` = 0.
- **`scripts/tests/test_research_mine.py`**
  - The fake `campaign.json` carries the MINE-STAT keys and `rung_failed`, and the recipe is hashed after any edit.
  - The mechanics line includes rung_failed.
  - The plan line shows F, the raw t and Fc.
  - 7 refusal cases of the rule constants and one test of the factors and hurdle are new: 40 tests.
- **`scripts/specs/v9/mine-c1.json`**: the `requires` line names the joined head, and the `max_memory_mib` fill gives the
  expected values (4,032 at 4 workers, 2,816 at 2 or 1).
- **Runbook**:
  - step 1: whole mine binary, the new `MembersStreamByDateAsStored`;
  - step 5: the expected probe 3,979 / 2,784 / 2,765 MiB, so W = 4 and cap 4,032; other numbers mean a wrong build;
  - step 7: header lines;
  - step 8: mechanics list, and free memory 5,568 MiB.
- **Prereg**:
  - status note;
  - D2, D4, D5 (restated), D9, D10, D11, D12, D13;
  - items 5, 7(d), 9, 12(3), 13;
  - section 3 (both columns), section 4 precondition 2, section 6.

**Raw hurdle t for budget 132.** z(132) = -Phi^-1(.05 / (2 x 132)) = -Phi^-1(1.8939e-4) = 3.554438. Budget 132 lies in
the band 101..1,000, F = 1.54. Raw t = z x F = 3.554438 x 1.54 = **5.4738** (5.47). At `20e7bd19`'s single F 1.55 it
was 5.5094 (5.51). Confirm: Fc(m) = 1.77 for every m <= 16 (the cap), so the gate t / Fc >= 2 needs a raw t of at
least 2 x 1.77 = **3.54** (was 3.10), and BY p <= .10 on t / 1.77.

**D5, flagged.** The recommendation keeps the 2023-01-01 split; this note covers its reason.

- At `20e7bd19` the two gates balanced per label row: .203 discover, .205 confirm. That was the reason.
- With F 1.54 and Fc 1.77:
  - discover 5.474 / sqrt(734) = .202;
  - confirm 3.54 / sqrt(228) = .234, so the confirm gate binds;
  - a 2022-07-01 split binds at .222, about 5% less per-row strength;
  - the balanced split (about 678 / 284 rows) binds at .210.
- The table states this and asks the owner to recheck.

## 3. The members streamed by date in the rho check

**Built.**

- `strategy_mine_pool.{hpp,cpp}`:
  - `MinePool::members` is now the pinned `MinePoolFile` list; `MinePool` gains `dates` and `instruments`.
  - `load_mine_pool_members` is gone.
  - New `check_mine_pool_members(pool)` streams each payload once through a 1 MiB buffer and keeps nothing.
  - New `stream_mine_pool_members(pool, begin, end, on_rows)`:
    - opens every member payload;
    - reads all of them in lockstep, date 0 to the last, one row of each into a per-member buffer of `instruments`
      values;
    - calls `on_rows(date, rows)` for each date in `[begin, end)` in order;
    - at the end checks each payload's extent, SHA-256 and infinity.
  - The check and the stream share one reader, `PinnedPayload`: open (extent), read (hash and infinity scan), close
    (extent again, SHA-256, infinity). Its order and words are `load_pinned_f64`'s and `load_rows'`.
- `strategy_mine_promote.cpp` `rho_check`: the member rows come from the stream; the candidate rows (kept, then
  batch) come from the signals in memory, as before.
- `strategy_mine.cpp` `run_mine`: the pre-write `load_mine_pool_members` becomes `check_mine_pool_members`. A member
  changed during the search is still refused before OUTPUT or the registry is written.

**Why no bit can move.**

1. **Same pairs.** `PairwiseRowCorrelation(rows, min_names)` keeps `rows = M + held + batch`, in the order members
   (manifest order: `load_rows` iterated `manifest.members`, the stream iterates `pool.members = manifest.members`),
   then kept signals, then the batch. `mined_rho_select(rho, fixed, batch, min_dates, slots)` is called with the same
   arguments.
2. **Same dates.** The old loop ran `d` from `discover.begin` up to `discover.end`. The stream calls `on_rows` exactly
   for `date` in `[begin, end)`, ascending, once each (`MembersStreamByDateAsStored` pins the sequence).
3. **Same input bytes.**
   - A member's row d is the payload's bytes `[d N 8, (d + 1) N 8)`, read raw into f64 storage. `load_pinned_f64` read
     the same file raw into a vector, with no conversion, so `values.subspan(d N, N)` held exactly those bytes. Both are
     verified against the same SHA-256 pin, so the bytes are the pinned bytes. `MembersStreamByDateAsStored` compares
     every streamed row with the stored payload byte for byte.
   - The candidate rows are the same `held` / `batch` vectors and `subspan(d N, N)` as before.
4. **Same order of floating-point operations per pair.** Per date:
   - `centred_tied_ranks` runs for r = 0 .. rows - 1 with identical inputs. It is a pure function: it fills `out`
     with NaN, clears `sorted`, sorts by (value, index) and assigns ranks. So `ranks[r]` is identical, and `sorted` is
     scratch carrying nothing across calls.
   - Then `rho.add_date(views)` runs on identical rank rows. Its pair loops are unchanged code.
   - Each pair's sum and count accumulate over dates in ascending order, as before. Nothing is reordered, merged or
     parallelised.
5. **Nothing kept from refused bytes.** `rho` is a local of `rho_check`, and `mined_rho_select` runs only after the
   stream returns Ok. A refusal at the end of the stream (a payload changed since the pre-write check) returns Err
   through `promote`, and no `campaign.json`, `mined_members.json`, `trials.csv` or ledger line is written.

Nothing outside the rho step reads a member. The search, the registry records and the recipe are untouched, so the
fixture's registry heads and trial logs are those of the merged MINE-STAT build. `atx-engine` is untouched since
`57bb5abb` (`git diff 57bb5abb HEAD -- atx-engine` is empty).

## 4. The memory model, reconciled

`mine_memory` is again term by term the merged code:

- `shortlist` = K C 8. MINE-STAT's `rho_step` keeps kept + batch <= K signals; the one being evaluated is part of the
  batch. The confirm read holds the <= K kept. It was (K + 1) C 8, one panel over.
- `members` (M C 8) is removed.
- `member_rows` = M names 8: one date of every member while the stream runs.
- `rho_rows` is unchanged: (M + K) rows, which `rho_check` reaches when kept + batch = K.
- `promotion()` = shortlist + max(engine, member_rows + rho_rows, confirm).
- The stream's file and SHA-256 state (at most 64 members) and the check's 1 MiB buffer are inside the 64 MiB metadata
  allowance, as the header now says.

Notation: C = dates x names, H = dates x ceil(names / 2), F = 3 + extras, S = 8 slots, W workers, R rungs, M members,
K the cap. The resident and search terms are unchanged from the MINE-MEM report.

**Default 4-year campaign** (1,405 x 6,100, 16 fields, 3 regressors, 32 members, 1 rung, K 16, 272 trials). Bytes,
with MiB in brackets.

| term | MINE-MEM `57bb5abb` | joined |
|---|---|---|
| shortlist | 1,165,588,000 | 1,097,024,000 |
| members | 2,194,048,000 | (removed) |
| member_rows | | 1,561,600 |
| promotion_engine | 557,082,500 | 557,082,500 |
| rho_rows | 2,476,864 | 2,476,864 |
| confirm_cache | 420,892,660 | 420,892,660 |
| resident | 1,660,442,932 (1,583.5) | 1,660,442,932 (1,583.5) |
| search, W = 1 | 1,595,598,920 (1,521.7) | 1,595,598,920 (1,521.7) |
| search, W = 4 | 3,133,906,420 (2,988.7) | 3,133,906,420 (2,988.7) |
| promotion | 3,916,718,500 (3,735.3) | 1,654,106,500 (1,577.5) |
| **peak, W = 1** | 5,577,161,432 (5,318.8) | **3,314,549,432 (3,161.0)**, promotion-bound |
| **peak, W = 4** | 5,577,161,432 (5,318.8) | **4,794,349,352 (4,572.2)**, search-bound |

**First campaign mine-c1** (1,405 x 6,100, 12 fields, 1 regressor, 53 members, no racing, K 16, 132 trials).

| term | MINE-MEM `57bb5abb` | joined |
|---|---|---|
| members / member_rows | 3,633,892,000 | 2,586,400 |
| rho_rows | 3,540,976 | 3,540,976 |
| resident | 1,245,173,652 (1,187.5) | 1,245,173,652 (1,187.5) |
| search, W = 1 / W = 4 | 1,046,539,160 / 2,926,158,260 | the same |
| promotion | 5,356,562,500 (5,108.4) | 1,654,106,500 (1,577.5) |
| **peak, W = 1** | 6,601,736,152 (6,295.9) | **2,899,280,152 (2,765.0)** |
| **peak, W = 2** | | 2,918,252,512 (2,783.1) |
| **peak, W = 4** | 6,601,736,152 (6,295.9) | **4,171,331,912 (3,978.1)** |

- The mine-c1 peaks are the same for any member count from 1 to 64. The test pins 1 and 64 against 53.
- The fixture: 95,199,808 B at 1 worker (was 95,671,872) and 101,584,960 at 4 (unchanged, search-bound).
- Unchanged: `small` 85,422,464 (plus 17,312 per trial and 128 per prior record) and the bounds 517,571,694,848
  (refused).
- The pins come from a Python mirror of the C++ formulas (session scratchpad `minejoin_mem.py`, not committed). It
  first reproduces every pin MINE-MEM committed (asserted) and then computes the joined values. The same arithmetic gives
  the 4-year and c1 tables above.

**What still stands in the way of 2,560 MiB (PM5-10)**, at the default 4-year shape:

- **1 worker, 3,161 MiB, promotion-bound.** Resident 1,583.5 MiB (the 16 extra fields 1,046) plus the promotion
  1,577.5: 16 signal panels (1,046) and the promotion engine (531).
  - Holding the kept signals' discover and confirm rows only would not help much, because those windows cover most of
    the panel.
  - Re-evaluating the kept signals for the confirm instead of holding them trades CPU for memory with no bit moved
    (given an Engine value-pure across instances, risk 1). It is not built.
- **4 workers, 4,572 MiB, search-bound.** The full-pass engines' slot pools (W x 8 x 65.4 MiB = 2,125 MiB) are
  MINE-MEM's named lever (a shared VM slot budget, not built).
- **Resident.** Mapping the extras needs an owner ruling on whether mapped pages count against the cap.

The smallest footprint reachable at this head without moving a bit is 3,161 MiB at 1 worker.

## 5. pytest

`"C:/Program Files/Python312/python.exe" -m pytest -q -p no:cacheprovider atx-impl/tools/test_mine_overlap_factor.py
atx-impl/tools/test_trial_ledger_rules.py scripts/tests/test_research_ledger.py scripts/tests/test_research_mine.py
scripts/tests/test_research_spec.py scripts/tests/test_research_cycle.py`: **181 passed, 3 skipped** (the live-root
skips) in 103 s, synthetic data only.

## 6. How root verifies

**check** (single TU), in this order:

- `atx-impl\src\strategy_mine_pool.cpp`
- `strategy_mine_promote.cpp`
- `strategy_mine.cpp`
- `strategy_mine_rule.cpp`
- `strategy_mine_trials.cpp`
- `strategy_mine_ledger.cpp`
- `atx-impl\tests\strategy_mine_test.cpp`
- MINE-MEM's `atx-engine\src\factory\search_driver.cpp` and `atx-engine\tests\factory\factory_signal_fitness_test.cpp`

**build**: `atx-engine-factory-tests atx-impl-strategy-mine-tests atx-equity-strategy-mine`.

- Add `atx-impl-tests`: `search_driver.hpp` gained a `SearchResult` field (MINE-MEM).
- Add `atx-shm-worker` if the factory binary's ProcessExecutor suites run.

**gtest**:

- `atx-impl-strategy-mine-tests`, the whole binary. In particular:
  - `StrategyMineRule.*` (MINE-STAT's tables, PM5-8, PM5-9);
  - `StrategyMine.WorkingBytesAreThePeakOfThePhases` (the joined pins);
  - `StrategyMine.RungFailuresAreTheirOwnTrialStatus`;
  - `StrategyMineCampaign.ModelTermsAreTheFixtureAllocations` (the stream holds exactly `member_rows`);
  - `StrategyMineCampaign.MembersStreamByDateAsStored` (new);
  - `StrategyMineCampaign.PromotesThePlantedSignalsOnlyInFiveSeeds`. It asserts **`rung_failed == 0`** and failed == 0
    on the fixture. If rung_failed is not 0, STOP (MINE-MEM's report); do not edit the assertion.
  - `StrategyMineCampaign.RulePinsOnTheTemplates`, `RhoStepReadsTheWholeShortlistBeforeTheCap` (two rho batches, so
    two promotion engines) and `UndefinedRhoAgainstAMemberFails`. These three exercise the streamed rho step
    end to end.
  - `StrategyMineCampaign.SameSeedSameChainHeadAtOneAndFourWorkers`. Run it with `--gtest_output=xml:<file>` to keep
    `fixture_registry_head` and `fixture_trials_csv_sha256`.
- `atx-engine-factory-tests --gtest_filter=SignalFitness*:NsgaSearch.*:FactoryFidelity*:ResearchIc*`, then the whole
  binary.
- **Golden `0x889874a3b9b29c55` at 1 and 4 workers**:
  - `SignalFitnessDefaults.ExplicitDefaultsKeepTheGoldenDigestAtEveryWorkerCount`;
  - `SignalFitnessDefaults.ImplicitDefaultsKeepTheGoldenDigest`;
  - `NsgaSearch.ScalarRaw_ReproducesGoldenDigest`.

  It must hold and is never edited. `atx-engine` has no change since `57bb5abb`.

**Identity.** No flag is added.

- The rho values are bit-identical by the argument in section 3.
- The model changes only the admission threshold and `campaign.json` `search.required_bytes` / `search.memory`, never
  the recipe, the registry, `trials.csv` or `mined_members.json`.
- The fixture's registry heads differ from `20e7bd19`'s because MINE-STAT's recipe gained keys (its report). No test
  pins them; they are equal across runs and worker counts.

## Where a compile error is most likely

1. **`strategy_mine_promote.cpp` `rho_check`, `on_date`.** `r < members ? member_rows[r] : candidate_row(r, d)` has
   operands `const span&` and a `span` prvalue; it yields a `const span` prvalue. Then the lambda is converted to
   `const MinePoolRowsFn &`, a temporary `std::function`.
2. **`strategy_mine_pool.cpp`:**
   - `std::vector<PinnedPayload>(members)`, an aggregate holding a `std::ifstream` (default-insertable, never moved);
   - `payload.in.open(std::filesystem::path, ...)`;
   - `static_cast<u64>(payload.in.tellg())` (as before);
   - `std::as_writable_bytes(std::span<f64>)` into `Sha256::update(std::span<const std::byte>)`.
3. **`strategy_mine_test.cpp`:**
   - `for (const auto &refused : {stream(...), check(...)})` (an initializer_list of `co::Status`);
   - `std::equal` over `std::as_bytes` spans;
   - `for (const usize members : {usize{1}, st::kMaxMinePoolMembers})`;
   - the test lambdas returning `core::Err(...)` / `core::Ok()` as `core::Status`.
4. **MINE-STAT, uncompiled until now:**
   - `std::array<MinedFactorBand, 3>{{{100U, 1.47}, ...}}` and the `static_assert` on `.back().top`;
   - `band_factor(std::span<const MinedFactorBand>)` from a `std::array`;
   - the `campaign_row` lambda in `rho_step`.
5. **MINE-MEM, not re-read here** (no merge touched it): `search_driver.cpp`'s rebuild path and `signal_rung_read`.
6. **Non-MSVC only, pre-existing:** `const auto missing` in `RefusesACampaignWithoutTheBook` shadows the file's
   `missing` under `-Wshadow`.

## Open risks

1. **MINE-STAT: a fresh promotion engine per batch.** `evaluate_signals` builds a new `al::Engine` for each rho batch.
   The values equal one engine's only if `Engine::evaluate` is value-pure across instances. This is the driver's
   EVAL-PATH NOTE. The search's worker invariance relies on it, and so does MINE-MEM's full-engine rebuild (its
   argument 2). It is not re-checked by a test that compares one engine with two on the same programs.
   `RhoStepReadsTheWholeShortlistBeforeTheCap` runs two batches but pins outcomes, not bits.
2. **MINE-MEM: per-generation rebuild on the racing path.** With racing on, each generation re-allocates the full
   engines' slot pools and rebuilds the strided rung panels: about 2.1 GiB and 625 MiB of first-touch per generation at
   4 workers. The cost is seconds; the legacy path is untouched. mine-c1 has racing off.
3. **A refusal moved after the write.** A member payload that changes between the pre-write check and a rho stream is
   now refused inside `promote`, after the registry and `registry_head.txt` are written (no `campaign.json`, no
   members, no ledger line). This is the state any promotion failure leaves (a crash included), so it is fail-safe. In
   MINE-MEM's load-before-write design such a change could not reach the promotion.
4. **I/O per rho batch.** Each batch reads and hashes every member payload: at C1, 53 x 68.6 MB, about 3.6 GB, plus
   one pre-write pass. One batch suffices when at most 16 trials clear the hurdle or none in a batch fails rho. In the
   worst case there is one batch per rho failure (MINE-STAT's cost note), against the runner's 600 s.
5. PM5-10's 2,560 MiB is not met at the default shape (section 4).
6. **D5.** MINE-STAT's confirm factor reverses the prereg's window-balance argument; this needs an owner recheck.
7. The C++ is uncompiled. The pinned byte counts come from the mirror. The fixture's `rung_failed == 0` and the rho
   fixtures rest on reading and on MINE-FIX's numpy replica.

## Cross-lane edits

Every edit in this lane is cross-lane by design (PM5-15):

- MINE-MEM's `strategy_mine.{hpp,cpp}`, `strategy_mine_pool.{hpp,cpp}` and `strategy_mine_test.cpp` (memory section,
  four rule-test loops);
- MINE-STAT's `strategy_mine_promote.cpp` (`rho_check` only);
- MINE-RUN's `scripts/research_mine.py`, `scripts/tests/test_research_mine.py`, `scripts/specs/v9/mine-c1.json`, the
  runbook and the prereg.

No other file is touched. `research_cycle.py`, `research_spec.py`, `backtest_integrity.py` and `atx-engine/` are
unchanged by this lane.

## Round 1

Base `40409e30`. Commits:

| task | commit |
|---|---|
| R1-1, concern 5: one engine vs a fresh engine per batch | `19f6765e` |
| R1-2, concern 4: verify each member once | `a11cca3b` |
| this section | the report commit |

pytest, six suites: 181 passed, 3 skipped (unchanged; no Python was touched).

### R1-1: the promotion's signals do not depend on the engine (concern 5)

Two new tests in `atx-impl/tests/strategy_mine_test.cpp`:

- **`StrategyMineCampaign.PromotionSignalsDoNotDependOnTheEngine`.** The fixture's 88 templates over 8 fields are
  evaluated the way `evaluate_signals` does it, in five ways:
  - a fresh `al::Engine` per program (the reference);
  - a fresh engine per batch of 5;
  - a fresh engine per batch of 16 (the free-slot size);
  - all 88 on one engine;
  - all 88 on one engine in reverse order.

  Every signal panel must be byte-equal to the reference.
- **`StrategyMineCampaign.RhoBatchingNeverChangesThePromotion`.**
  - Setup: pool members `{m2, p2}`, templates only, budget 22, `max_promotions` 2.
  - The full campaign is run with `MineConfig::rho_batch` set to 0 (the verb's value, the free slots), 1 and 64 (all
    at once).
  - It requires equal `promotions`, members, `trials.csv` and registry heads.
  - `ASSERT_GT(read, 2U)` proves that the default really runs more than one batch.

`MineConfig::rho_batch` is a test hook. It is not a CLI option and it is not in the recipe. It sets only how many
shortlist candidates one batch evaluates. `rho_check` still receives the free-slot count, so the memory bound it
counts is unchanged. At 0 the code path is the same as before.

**`al::Engine` state that outlives one `evaluate()`** (`atx-engine/include/atx/engine/alpha/vm.hpp`):

| state | where | carries? |
|---|---|---|
| `pool_` (SlotPool) | `vm.hpp:2154`, grown by `ensure_pool` `vm.hpp:1157-1163` | **Yes, the one real candidate.** A fresh pool is zero-filled (`panel.hpp:215`). A reused pool still holds the previous program's slot values. This matters only if an op leaves part of its destination slot unwritten. The ops were not audited one by one; the one-engine and reversed-order arms of the test would expose such an op among the fixture's 88 templates. |
| `field_remap_` | rebuilt by `resolve_fields` every call, `vm.hpp:545` | no |
| `state_` (recurrence buffer) | seeded at t = 0, `vm.hpp:1999-2004` | no; every read at date t comes before that date's write |
| `ts_exp_coeff_` | `vm.hpp:2158`; `ts_sliding.hpp:344-361` | a cache, but a pure function of (d, f) |
| scratch (`ts_scratch_*`, `ts_col_*`, `ts_dq_*`, `cs_valid_`, `cs_scratch_`, `lit_*`) | the `reset()` doc, `vm.hpp:474-528` | grow-only capacity, rebuilt by each op |
| "seeded" flags | `vm.hpp:2073-2141` | stack-local, not members |
| RNG | none | none |
| `cs_mask_` | set once per engine | constant across calls |
| `panel_digest_`, `root_buf_` | the cache and subset paths | not used by plain `evaluate` |

So the claim does not rest on reading alone. It rests on this table plus the two tests, plus the existing determinism
golden, which is equal at 1 and 4 workers.

### R1-2: each member verified once, held with writers denied (concern 4)

**Idiom copied.**
- The share mode: `CreateFileW(GENERIC_READ, FILE_SHARE_READ, OPEN_EXISTING)`, as in `Mapping::map_file_ro`
  (`atx-tsdb/src/mapping.cpp:109`).
- The class shape: a move-only RAII handle with a positional `read_at` and a POSIX `pread` twin, as `LogFile` in
  `atx-engine/src/eval/trial_registry.cpp:155-330`.

New TU `atx-impl/src/strategy_mine_pinned_file.{hpp,cpp}` (`PinnedReadFile`, `kPinnedReadDeniesWriters`), added to
`atx-impl/CMakeLists.txt`. I chose not to map the file: the rho stream touches about 2 GB a pass, and the touched mapped
pages would count in the working set that the runner's RSS cap measures.

**Design.**
- `bind_mine_pool` opens each member, writers denied, and verifies it once through that handle. The checks are
  extent, reads, extent again, SHA-256 and infinity, in `load_pinned_f64`'s order and words.
- `MinePool::members` becomes `std::vector<MinePoolMember{pin, payload}>`, held until `run_mine` returns.
  `MinePool` is now move-only; nothing copies it.
- `writers_denied` is true on Windows. With it:
  - `check_mine_pool_members` (the pre-write check) confirms extents only;
  - `stream_mine_pool_members` checks extents, then reads only the rows of [begin, end), positionally, with no
    hash.
- Without it (POSIX): the old lockstep whole-file verified pass, unchanged.

**I/O at mine-c1** (53 members x 68.6 MB):

| pass | before | now (Windows) |
|---|---|---|
| bind | about 3.6 GB, hashed | about 3.6 GB, hashed |
| each rho batch | about 3.6 GB, hashed | about 1.96 GB of discover rows, no hash |
| pre-write check | about 3.6 GB, hashed | extents only |

**No computed bit changes.** Each row buffer receives the same bytes from the same offset (date x instruments x 8)
as before. `on_rows` is called for the same dates, in ascending order, with the same views. So `rho_check` sees
exactly the input it saw before, and its arithmetic is untouched.

**The fail-safe holds.**
- While the pool holds a member, the OS refuses any open that would write, truncate, delete or rename it.
- The bind's own open fails if another handle can already write the file.
- So the verified bytes are the bytes every later pass reads. A member that changed before the bind is refused
  there, before the search: no `campaign.json`, no members.
- The test `MembersStreamByDateAsStored`, Windows branch:
  - while held, a write, a remove and a rename all fail, and the stream and the check still read the verified bytes;
  - once the pool is released, the write succeeds and the rebind is refused with "SHA256 mismatch: p1.f64".
- POSIX branch: the write succeeds, and both the stream and the check refuse it.
- This also closes concern 3 on Windows, since a member can no longer change between the pre-write check and a rho
  stream. On POSIX concern 3 stands as written.

**What the share check cannot see:** raw volume writes and kernel filters. On POSIX, `flock` is advisory, so
`kPinnedReadDeniesWriters` is false there and every pass re-verifies.

**Memory.** `mine_working_bytes` is unchanged; what is held does not change materially.
- The row buffers stay at members x instruments x 8.
- The denied path allocates no per-member SHA state.
- The held handles fall inside the metadata allowance. Only that comment was updated, in `strategy_mine.hpp`.

### Where a first compile is most likely to fail (uncompiled, per lane rules)

- `windows.h` in `strategy_mine_pinned_file.cpp`, with `WIN32_LEAN_AND_MEAN` and `NOMINMAX`;
- `ReadFile` with an `OVERLAPPED` offset on a synchronous handle;
- the move-only `MinePool` inside `tl::expected` (`ATX_TRY(auto pool, ...)`);
- `if constexpr (st::kPinnedReadDeniesWriters)` in a non-template test (both branches must compile everywhere);
- `MinePoolMember member{pin, {}}`;
- the ternary inside `ATX_TRY_VOID` in `check_mine_pool_members`.

### Round 1 concerns

1. The C++ is still uncompiled, and the Windows sharing behaviour in `MembersStreamByDateAsStored` is asserted, not
   observed. That covers the `std::ofstream` open failing, and MSVC STL's `fs::remove` and `fs::rename` failing
   with a sharing violation.
2. Engine slot reuse (`pool_`) is the one carry path. It is safe only while every op fully writes its destination
   slot. The new test pins this for the fixture's templates, not for every op.
3. A single rho pass still reads about 1.96 GB at mine-c1. Batches remain one per rho failure in the worst case.

## ENG-SLOT

Lane ENG-SLOT (wave AG). Base `3f35349f`. It answers Round 1 concern 2 for every op, not only the fixture's
templates. Tests only: no engine source changed.

| task | commit |
|---|---|
| the property test | `cc3e3c6e` |
| this section | the report commit |

### What root builds

- New file `atx-engine/tests/alpha/alpha_vm_slot_reuse_test.cpp`, in the `alpha` group. No CMake line is needed:
  `atx-engine/tests/CMakeLists.txt` globs `alpha/*_test.cpp` (CONFIGURE_DEPENDS) into `atx-engine-alpha-tests`.
  This is a deviation from the brief ("then the CMake line"), because the glob is the local idiom. A worktree
  configured with `-DATX_TEST_GROUPS` must include `alpha`.
- check: `powershell scripts\atx-build.ps1 check atx-engine\tests\alpha\alpha_vm_slot_reuse_test.cpp`
- build: `powershell scripts\atx-build.ps1 build atx-engine-alpha-tests`
- gtest: `atx-engine-alpha-tests --gtest_filter=AlphaVmSlotReuse.*`. Run the binary directly: one process builds
  the variant fixture once. Under ctest, each of the six tests is its own process and rebuilds the fixture.

The six tests:

| test | pins |
|---|---|
| `Catalogue_EveryRowAndOperator_HasAnEvaluatedVariant` | every catalogue row and operator reaches an evaluated program; it records `forms`, `variants`, `max_target_slots` (`--gtest_output=xml`) |
| `Poisons_SpanEveryTargetSlot_DistinctFromAFreshPool` | a one-slot poison exists; both poisons are wider than every target by 4; finite poison finite and non-zero; NaN poison has NaN, +inf, -inf and finite cells and no 0.0 |
| `EveryOp_DirtyPoolAuditExact_ByteEqualToAFreshEngine` | the property, AuditExact |
| `EveryOp_DirtyPoolResearchFast_ByteEqualToAFreshEngine` | the property, ResearchFast |
| `EveryOp_DirtyPoolMaskedAuditExact_ByteEqualToAFreshEngine` | the property, AuditExact with a cross-section mask |
| `EveryOp_DirtyPoolMaskedResearchFast_ByteEqualToAFreshEngine` | the property, ResearchFast with the mask |

Expected: all six pass, because the reading below finds no op that leaves stale cells. On a property failure the
first 25 lines read `<call> [<arm>]: alpha '<root>' date d name j: fresh Engine v (bits), dirty Engine v (bits)`.
After that the failures are only counted.

### What the test does

- **The ops are iterated, never listed.**
  - Every row of `alpha::detail::builtin_ops()` and `literature_ops()`: the 74 + 17 rows the Library registers,
    a superset of the factory `OpCatalog`, which drops record and hparam rows.
  - Every infix and prefix operator that `detail::binary_op_text` / `unary_op_text` (unparse.hpp) spell, over
    all 256 `OpCode` values.
  - The ternary, the one construct spelled by hand.
  - A new row or operator is covered without editing the test. If none of its variants is accepted,
    `Catalogue_...` fails and names it.
- **The variants.**
  - Each operand position is probed with four kinds: a numeric field (`x y z u`, rotated by position), a Group
    field (`grp_a grp_b`), a mask (`(x > y)`, `(z <= u)`, `(y >= z)`) and a literal (`5` or `0.5`). This covers
    every arity in `[min_arity, max_arity]`, so default-filled optionals and parser-made packs are included.
  - Every probe that `parse_program` + `analyze` accept is expanded. Each literal position takes
    `{1, 2, 3, 5, 20, 0.5}`: windows of one bar, short, 5, beyond the 12-date history, and hparams. The vector
    positions are either rotated or all `x` (ties, identical and collinear inputs).
  - A call of literals only is kept as probed.
  - A variant whose code does not run the op is dropped: the parser folds `abs(5)`, and the DAG
    strength-reduces `x ^ 2`.
  - Record ops are rooted once per pin (`split2(x).hi`, `kalman(..).alpha`, `pack2(x, y).p0`, ...). A pack is
    also reached through `ts_resid_on`, `ts_beta_on` and `cs_resid_on` with 2 to 4 regressors.
  - By hand count there are about 1,300 variants; `RecordProperty("variants")` gives the real number.
- **The keep root.** Each target carries a second root `keep = x + y + z + u`. The fields stay live past the op,
  so the op's destination (and a record op's block) is a slot that this target has not written before. A cell
  the op skipped would read 0.0 on the fresh Engine and a poison value on a dirty one.
- **The panel** is 12 dates x 7 names. It holds:
  - ties across names and dates, zeros, negatives and a flat run (zero variance);
  - single holes, an all-NaN name, and a name whose history starts at date 8, shorter than a 5-bar window;
  - a late listing and a universe gap;
  - group labels with NaN labels and a label change;
  - in the masked configs, a cross-section mask that excludes three names on some dates.
- **The arms.** The reference is a fresh `Engine`. The same `Program` then runs on:
  1. a new Engine after the tightest finite poison: the pool is reused at about the target's size and does not
     grow;
  2. the same with the NaN poison;
  3. a new Engine after the widest NaN poison: reused, larger;
  4. a new Engine after a one-slot poison: the pool grows. This arm passes by construction, because a new pool is
     zero-filled, but the growth is asserted;
  5. one long-lived Engine that ran the widest NaN poison and then every earlier variant in catalogue order.
     This arm also exercises the engine's grow-only scratch and `ts_exp_coeff_`.

  Arms 1 to 4 also assert `pool_capacity()`: unchanged for the reuse arms, larger for the growth arm. Every alpha
  of every arm must be byte-equal to the reference (`memcmp`; the first differing cell is reported with its bits).
- **The poisons.**
  - Finite: n distinct constants, each bound as a root and combined by nested `max`. A call is not folded, so n
    leaves fill n + 1 slots.
  - NaN: n leaves over a field `pz` that is NaN in every third cell. By leaf kind they give finite positive
    values, negated values (NaN with the sign bit set), `(pz - c) / (pz - pz)` (+inf, -inf, NaN) and
    `log(pz - c)`, combined by nested `+`.
  - Both are compiled at 1, 2, ... leaves until they reach the widest target + 4 slots.

### Ops whose destination write is conditional, found by reading

No op leaves stale cells on `Engine::evaluate`. The conditional writes are all covered by a prefill in the VM:

| ops | where the kernel skips cells | what covers them |
|---|---|---|
| every Cs* op: rank, zscore, scale, normalize, winsorize, indneutralize, group_neutralize, group_rank, group_zscore, group_count, group_mean, group_scale, cs_residualize, quantile, vec_sum, vec_avg | the row kernels write only the valid set. They return early on an empty set (`cs_ops.hpp:332`, `700`, `723`; quantile `535-542`), skip NaN group labels (`cs_ops.hpp:382`, `401`, `409`, `596-671`, `758-826`), and cs_residualize writes only its regression set (`474`, `515`) | `cs_one_date` sets every cell of the row to NaN before dispatch, `vm.hpp:1596-1598`. Every Cs op goes through it (`eval_cross_section` -> `cs_rows`) |
| bucket, cs_resid_on (W2) | `lit_bucket_row` (`lit_ops.hpp:443-460`) and `lit_cs_resid_row` (`465-501`; a degenerate date returns at `498`) write only the valid / regression set | `eval_lit_cs` sets the row to NaN first, `vm.hpp:1379` |

Every other kernel writes each cell of its range on every branch, and its gate cells are written as NaN
explicitly, not skipped:

- the Ts online sweeps: `ts_ops.hpp:545`, `624`, `651`, `720`, and the Welford `emit` at `923`;
- the order-stat sweep: `ts_order_stat.hpp:319`, `338`, `382`;
- the sliding sweeps, d == 0: `ts_sliding.hpp:707`, `746`;
- ResearchFast `ts_decay_exp` with d > dates: `vm.hpp:1714`;
- `eval_lit_ts`: `vm.hpp:1432`;
- `eval_ts_lookback`'s warm-up rows: `vm.hpp:1887`, `1891`;
- the recurrences, Kalman, Split2, ArgPack, Pin, Const, LoadField and the element-wise map, which loop over the
  whole range unconditionally.

So no fix is proposed. Out of scope: `evaluate_nodes` (Lane 2) skips dead instructions by design
(`vm.hpp:1021`). Their slots are never read.

**Carry paths not in the Round 1 table**, found while reading:

- `ordstat::sweep_scratch()` is `thread_local` (`ts_order_stat.hpp:269`). It is shared by every Engine on a
  thread, for ts_rank, ts_med and ts_quantile. It is rebuilt per column:
  - `SortedWindow::reset` sets n to 0;
  - `FenwickWindow::reset` zero-fills its tree;
  - `compress_column` rewrites every date's rank.

  So it does not carry. The test cannot reset it, so the fresh reference shares it with the dirty arms. It is
  exercised only by running different programs first (arm 5).
- `compile_cache()` is `thread_local` (`bytecode.hpp:398`). It is used only by `compile_cached`, with a
  structural key and an equality check; the mine calls `compile`.

### Where a first compile is most likely to fail (uncompiled, per lane rules; clang-cl `/W4 /WX`)

1. `constexpr std::array<FieldDef, 7> kFields`, whose member is `f64 (*)(usize, usize) noexcept` and whose
   values are the addresses of the `[[nodiscard]]` field functions.
2. The `<charconv>` calls:
   - `std::to_chars(double)`, with and without `std::chars_format::fixed`;
   - `std::to_chars(u64, 16)`.
3. `Fixture`:
   - an aggregate holding a `Panel`, whose default constructor is private: `Fixture fx{make_panel(), ...}`;
   - `return fx;` needs Fixture's implicit move constructor;
   - `static const Fixture fx = build_fixture();`.
4. The `Form{...}` braced inits: `sig.min_arity` (u8) into `usize` is widening, not narrowing; the literals
   `2, 2` and `{}` go to `std::span`.
5. In `configure`, `return s ? std::string{} : s.error().message();` on a `const tl::expected<void, Error>`.
6. `args.emplace_back(kLiteralValues[i])`: an explicit `std::string` from `std::string_view` through
   `emplace_back`.
7. `++(v > 0.0 ? c.pos_inf : c.neg_inf);`.
8. `return std::move(*prog);` into `std::optional<Program>`, and `return std::move(*p);` into `Panel`.
9. `RecordProperty(key, std::to_string(n))` inside TEST bodies in a named namespace.
10. `alpha::detail::binary_op_text` / `unary_op_text`, inline in `unparse.hpp`.

### ENG-SLOT concerns

1. **Uncompiled and unrun.** Neither the variant count (about 1,300) nor the runtime is measured. Each config
   runs about 10 evaluations per variant, and the fixture runs about 17k probe compiles. The cost is seconds in
   Debug, not measured.
2. **An abort is an engine finding.** A type-checker-accepted variant that trips an `ATX_ASSERT` or
   `ATX_UNREACHABLE` in a Debug kernel aborts the binary. This would mean "analyze-valid implies VM-safe" is
   broken. Variants include unusual but accepted programs: masks and groups in element-wise and pair-series
   operands, scalar-only calls, windows of 1 and of 20 > dates, `ts_decay_exp` with f = 20, and `ou_filter` with
   theta = 20.
3. **What the census test rests on.** It assumes the NaN poison's leaves hold no exact 0.0 and include +inf, -inf,
   NaN and finite cells. This is argued from the panel arithmetic in the file comment, not run.
4. **No poisoning of thread-local scratch.** The order-stat scratch is not poisoned between the reference and the
   arms. By reading it does not carry.
