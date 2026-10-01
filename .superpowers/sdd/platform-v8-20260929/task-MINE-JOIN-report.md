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
