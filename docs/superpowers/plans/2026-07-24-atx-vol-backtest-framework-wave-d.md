# Backtest Framework — Wave D: engine `StepObserver` (L10) + de-SPY `dispersion_workflow` (L12)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Close the two in-place header changes the design spec reserves for Wave D.
**L10** — add an optional `StepObserver` to `RunConfig` (`atx-vol/include/atx/vol/backtest.hpp`) fired after each
`IStrategy::on_step` with strategy access, so mark-divergence capture rides the **one real engine run** instead of the
`collect_mark_divergence_replay` shadow loop. That deletes a whole second engine pass over ~60 archives (L4/P5) **and**
deletes the shadow-vs-engine drift risk (the shadow settles/erases nothing before `on_step`, the engine does).
**L12** — add `RunSpec.index_symbol` to drop the hardcoded `"SPY"` in `all_symbols` / `universe_at`
(`atx-vol/src/dispersion_workflow.cpp:224,238,240`), keeping `dispersion_workflow` the pure config/input front-end.

**Value-preserving, non-negotiable.** `mark_divergence` is a **pinned economic artifact**: the 135-session production
run asserts `mark_divergence rows=0`, and the projected-cold route's divergence is part of the Wave B **I1** parity
closure. An observer path that produces even slightly different divergence rows is a **REGRESSION, not an
improvement**. This plan therefore proves equivalence *before* deleting anything: T3 lands a dual-run comparator that
runs the observer **and** the shadow and aborts on any bit-level difference, T4 is a blocking controller run of that
comparator on the real 135-session corpus on **both** execution routes, and only T5 deletes the shadow. Likewise
`index_symbol` defaults to `"SPY"` so every existing caller and every golden is bit-unchanged — L12 removes a
hardcode, it does not change behaviour.

**Architecture:** `StepObserver` is a strategy-agnostic engine hook: a `std::function<Status(const StepEvent&)>` on
`RunConfig`, fired from the B1 (strategy) `run_backtest` overload immediately after `on_step` returns `Ok`, before the
`validate_strategy_transition` / hedge / `execute` stage. The **dispersion-specific** consumer — the leg match, the bps
metric, the row shape — lands in `listed_dispersion_pipeline` (Wave B's listed-route home) as
`make_mark_divergence_observer`, per the review's L10 fix direction ("move the bps metric into the pipeline module").
The example keeps its `MarkDivergenceArena` / `build_mark_divergence_section` encoder verbatim (`mark_divergence` is
deliberately example-owned with no library encoder — Wave A/B decision), so **no `run_archive` / registry / schema
surface is touched anywhere in Wave D**. For L12, `all_symbols` / `universe_at` gain a **trailing defaulted**
`std::string_view index_symbol = "SPY"` parameter, so `dispersion_workflow` stays independent of `RunSpec` layout and
**every existing call site — including the pybind11 bindings — compiles and behaves unchanged**.

Reference the design spec, do **not** restate it:
[`docs/superpowers/specs/2026-07-21-atx-vol-backtest-framework-design.md`](../specs/2026-07-21-atx-vol-backtest-framework-design.md)
§4.5 (`backtest.hpp` CHANGED), §4.6 (`dispersion_workflow.hpp` CHANGED), §8 (Wave D = L10 + L12), §9 (testing).
Grounding review [`docs/superpowers/specs/2026-07-21-atx-vol-backtest-review.md`](../specs/2026-07-21-atx-vol-backtest-review.md)
**L10** (row at §2/LOW), **L12**, plus **L4/P5** (the double-deserialize this removes) and **L2** (the zero-denominator
bps collapse — knowingly PRESERVED here, see T2).

**Tech Stack:** C++20 (MSVC, Release preset `build-rel`, AVX2), `atx::vol`; `run_backtest` B1 overload
(`atx-vol/src/backtest.cpp:1571`-end), `ListedDispersionStrategy` (Record policy, `last_mark_divergences()` /
`next_roll_index()`), `listed_dispersion_pipeline` (Wave B), `RunDir` / `runarchive dump` (Wave A), CMake, gtest target
`atx-vol-tests`, example target `atxvol_spy_dispersion_backtest`.

---

## Global Constraints

- Work directly on local `main`, **in place**. Explicit-path `git add` ONLY — **never** `git add -A` / `-u` / `.`
  (the tree carries unrelated uncommitted work: surface-db, sha256, `atx-db/`, `atx-kb/`, python bindings split,
  research dirs). Base commit for Wave D: `main` @ `587ee97`.
- **ONE build at a time.** Release preset only: `cmake --build C:\atx\build-rel --target <tgt>`. Shared deps at
  `C:\atx-cache\deps`. `parquet.dll` requires `C:\atx\build-rel\bin` on `PATH` to run any example or test
  (`$env:PATH = "C:\atx\build-rel\bin;$env:PATH"`).
- **Full gtest MUST run from `C:\atx\build-rel` CWD** (a stale repo-root artifact cache otherwise poisons fixtures):
  `cd C:\atx\build-rel; .\bin\atx-vol-tests.exe`.
- Do **NOT** modify golden fixtures (`atx-vol/python/tests/data/runarchive/wave_a_fixture.atxrun`,
  `dispersion_paired.atxrun`). Do **NOT** touch `C:\atx-data` run dirs — **controller-only**; subagents use the
  3-session fixture copy recipe from `.superpowers/sdd/dispersion-parity/task-9-report.md` and **NEVER** modify
  `scratchpad\paired`. **Never read `C:\atx\.env`.**
- **RunArchive schema is FROZEN**: `schema_hash` `0xdcce47781ac8390d`, `kRaMinor` 0. Wave D touches **no** section,
  **no** column, **no** on-disk struct, and **not** `run_archive_schema.hpp` / `_schema.py`. If any task appears to
  need a section/column change, **STOP and escalate to the controller** — never a silent bump. (The one adjacent edit
  is T6's `encode_meta_section` gaining one **ScalarKV row**, not a column — see T6's freeze note.)
- **`backtest.hpp` is a WIDELY included core header and `RunConfig` is used across the whole library plus examples and
  benches.** 46 files `#include "atx/vol/backtest.hpp"`: 10 in `include/`, 6 in `src/`, 17 in `tests/`, 7 in
  `examples/`, 2 in `bench/`, 4 in `python/src/bindings/`. Blast-radius rules, all verified against the tree:
  - The new field **MUST be APPENDED at the struct tail**, after `settlement_mark_memo` (`backtest.hpp:340`), with a
    default member initializer — mirroring the two existing "*Appended for positional aggregate source
    compatibility*" comments at `:326` and `:330`. Do not insert mid-struct.
  - Verified: **zero** positional aggregate-init sites (`RunConfig{a,b,…}`) exist; every one of the ~120 sites is
    `RunConfig cfg;`, `RunConfig{}`, a copy, or `const RunConfig&`. **Zero** `static_assert(sizeof/offsetof)` on
    `RunConfig`. **Nothing** hashes or serializes `RunConfig` (`run_identity_hash` folds `run_spec.tsv` *file bytes*,
    never this struct). So a defaulted appended field is source- and default-compatible at every site.
  - It is **NOT** ABI-compatible in the linker sense — `sizeof(RunConfig)` grows. `backtest.hpp:157` already declares
    this struct a layout-fragile ABI surface ("Rebuild the atx-vol DLL and every consumer together"). Therefore **T1
    requires a full `cmake --build C:\atx\build-rel` (all targets), not a `--target atx-vol-tests` build alone** —
    library, 7 examples, 2 benches and the python module must be rebuilt in the same pass.
  - The pybind11 `RunConfig` binding (`atx-vol/python/src/bindings/backtest.cpp:213-223`) is an exhaustive
    hand-kept 11-line `.def_readwrite` list. **Deliberately NOT extended**: a `std::function` is not sensibly
    bindable and **Python is out of scope this wave**. T1 documents `step_observer` as C++-only in the header, so the
    binding file is **untouched** and the omission is a recorded decision, not drift.
- **Concurrency hazard (coordination, read this before T1).** Other worktrees are actively appending fields to this
  same struct tail — `RunConfig::book_entry_fill_slippage` (F2) and an F3 discrete-dividend field exist on other
  branches and **not** on `main` @ `587ee97`. Wave D must append **one** field and keep the diff to the tail minimal
  so those merges resolve as adjacent additions. Wave C's `backtest_driver` `RunConfigOverlay` (§4.3) is being planned
  concurrently: if Wave C lands first, its overlay MUST be able to carry `step_observer`; note it to the controller
  rather than reshaping the hook.
- **Do NOT chase the three known pre-existing gtest reds** (`.superpowers/sdd/backtest-wave-a/t10-failure-triage.md`;
  zero file/include intersection with Wave D): `BoundaryHoist.PriceBitIdenticalToPrechange`,
  `SurfaceV2Qualification.RiskBuild…/Latency`, `…/Balanced`. A green Wave D = all-green **modulo these three**.
- **Python is out of scope.** No pytest task, no binding edit, no `_schema.py` edit, no `parity.py` edit. If a Python
  test breaks as a side effect, that is a **STOP-and-escalate**, not a fix-it-yourself.
- Windows/PowerShell: `$ErrorActionPreference='Continue'` (native stderr wraps in ErrorRecords). Use
  `git commit -F <file>` or a bash heredoc for multi-line messages (here-strings mangle).
- Commit trailer on every commit: `Co-Authored-By: Claude <model> (1M context) <noreply@anthropic.com>` using the
  implementer's own model name (Wave A/B used Opus 4.8).

---

## Anti-vacuity discipline (why this plan is shaped this way)

Wave B's **final whole-branch review** caught a defect that nine task-level reviews and a 135-session gate all missed:
the M1 fix worked at the new seam and the production path still aborted one line downstream, because **every M1 test
drove the seam in isolation and none reached the production consumer**, and the production corpus had a zero lead-in
so the fixed path never executed. Wave D is built against exactly that failure class:

1. **Every gate must be falsifiable, and the plan says how it would fire.** The Vacuity Ledger below is a required
   artifact: each task's report must fill in the "proven RED by" column with a real command and its real output.
2. **The L10 equivalence check runs in the PRODUCTION code path on a REAL corpus** (T3 comparator + T4 controller run),
   not only in gtest. A gtest that constructs a `StepEvent` by hand can prove the collector's arithmetic; it can never
   prove the engine fires the hook where the shadow looped.
3. **The cold route cannot carry the proof alone.** On `--execution cold` the pinned truth is `mark_divergence
   rows=0` — an empty-vs-empty comparison is vacuous. The primary equivalence evidence is therefore
   `--execution configured`, the route where divergence rows are genuinely **nonzero**; T3/T4 **assert row count > 0**
   there and fail the gate as vacuous if it is 0.
4. **Green-lock tests are disclosed as such.** Where a test passes on first write (a regression lock, not a RED), the
   task report must say so explicitly, as Wave B's T6/T7/T8 reports did.
5. **No dead fields.** Wave B had to delete four dead `ListedDispersionMethodology` fields post-review ("an active
   trap"). Every one of `StepEvent`'s four members is read by in-tree code — T1 and T2 name where.

---

## File Structure

**Modified — library:**
- `atx-vol/include/atx/vol/backtest.hpp` — (T1) `struct StepEvent`, `using StepObserver`, `RunConfig::step_observer`
  appended at the tail.
- `atx-vol/src/backtest.cpp` — (T1) fire the observer after **both** `on_step` sites in the B1 overload
  (`:1862` inception, `:1999` per-step); fail-closed rejection in the B0 fixed-book overload (`:1361`-`:1560`).
- `atx-vol/include/atx/vol/listed_dispersion_pipeline.hpp`, `atx-vol/src/listed_dispersion_pipeline.cpp` —
  (T2) `ListedMarkDivergenceRow`, `listed_mark_divergence_bps`, `make_mark_divergence_observer`;
  (T6) thread `index_symbol` at `listed_dispersion_pipeline.cpp:205` and `:224`.
- `atx-vol/include/atx/vol/dispersion_workflow.hpp`, `atx-vol/src/dispersion_workflow.cpp` — (T6) `RunSpec.index_symbol`,
  defaulted `index_symbol` parameter on `all_symbols` / `universe_at`, parse + resolved-spec emit.
- `atx-vol/src/run_archive.cpp` — (T6) `encode_meta_section` (`:960-991`) echoes the new key; `pairs.reserve(21+…)` → `22`.

**Modified — example (the thin CLI):**
- `atx-vol/examples/spy_dispersion_backtest.cpp` — (T3) install the observer alongside the retained shadow +
  bit-exact comparator; (T5) delete `collect_mark_divergence_replay`, observer becomes the sole source;
  (T6) thread `spec.index_symbol` at `:320`, `:541`, `:883`, `:960`.

**Modified — tests:**
- `atx-vol/tests/strategy_test.cpp` — (T1) engine-level observer tests (already registered,
  `atx-vol/tests/CMakeLists.txt:104`).
- `atx-vol/tests/listed_dispersion_pipeline_test.cpp` — (T2) collector tests (already registered, `:102`).
- `atx-vol/tests/run_archive_test.cpp` — (T6) extend `RunArchiveEncoders.MetaSectionEchoesResolvedSpec` (`:1059-1098`).

**New:**
- `atx-vol/tests/dispersion_workflow_test.cpp` + registration in `atx-vol/tests/CMakeLists.txt` — (T6). This module has
  **zero direct unit coverage today** (`read_run_spec`, `all_symbols`, `universe_at`, `batch_spec` are exercised only
  through the example binary); the de-SPY is the right moment to close that.

**Deliberately NOT touched:** the RunArchive on-disk format / `run_archive_schema.hpp` / `_schema.py` / any committed
fixture; the `mark_divergence` section registry and the example-owned `MarkDivergenceArena` encoder; all pybind11
bindings (`python/src/bindings/backtest.cpp`, `dispersion.cpp`); `parity.py` / `io.py` / `runarchive.py`; the L2
zero-denominator bps behaviour; `backtest_driver` (Wave C); perf passes P1–P7 (Wave E).

---

## Task 1: `StepObserver` on `RunConfig` + engine firing (L10 substrate)

**Files:**
- Modify: `atx-vol/include/atx/vol/backtest.hpp`, `atx-vol/src/backtest.cpp`
- Test: `atx-vol/tests/strategy_test.cpp`

**Interfaces (Produces)** — add to `backtest.hpp` immediately **above** `struct RunConfig` (`:300`), after the existing
`class IStrategy;` forward declaration at `:52` (backtest.hpp **cannot** include `strategy.hpp` — `strategy.hpp`
includes `backtest.hpp`; the forward declaration plus a reference member is the only legal shape):

```cpp
// One observed engine step, handed to RunConfig::step_observer immediately after
// IStrategy::on_step returns Ok and BEFORE the transition validation / hedge /
// execute stage. Fires once per clock step INCLUDING inception (step_index 0), in
// step order, on EVERY step regardless of RunConfig::record_every_n (the recorded
// BacktestResult rows are downsampled; these events are not). Every reference is
// borrowed and valid only for the duration of the call.
struct StepEvent {
  std::size_t step_index;             // index into Clock::refs(); 0 == inception
  const SnapshotRef &ref;            // this step's clock entry (date + archive_path)
  const MarketSnapshot &snapshot;    // the base the strategy just stepped on
  const IStrategy &strategy;         // post-on_step strategy state
};

// Optional per-step observation hook. Returning Err aborts the run with that error
// (the engine propagates it verbatim), so an observer may enforce its own
// invariants fail-closed. C++-only: deliberately NOT exposed through the pybind11
// RunConfig binding (python/src/bindings/backtest.cpp) — a std::function is not
// bindable and the Python surface stays unchanged.
using StepObserver = std::function<Status(const StepEvent &)>;
```

and, **appended at the RunConfig tail** after `settlement_mark_memo` (`:340`):

```cpp
  // Appended for positional aggregate source compatibility. Empty by default =>
  // zero cost beyond one predictable branch per step and byte-identical output.
  // Ignored by no overload: the fixed-book (B0) overload has no strategy, so
  // setting this there is a fail-closed InvalidArgument, never a silent drop.
  StepObserver step_observer{};
```

**Engine wiring** (`atx-vol/src/backtest.cpp`), three edits:
1. Inception, after the `on_step` error check at `:1864-1866` and **before** `before_lots.clear()` at `:1867`:
   `if (cfg.step_observer) { ATX_TRY_VOID(cfg.step_observer(StepEvent{0, refs[0], *base, strat})); }`
2. Per-step loop, after the `on_step` error check at `:2001-2003` and **before**
   `validate_strategy_transition` at `:2004`:
   `if (cfg.step_observer) { ATX_TRY_VOID(cfg.step_observer(StepEvent{i, refs[i], *base, strat})); }`
3. B0 fixed-book overload (body `:1361`-`:1560`), at the top beside the other config validation:
   `if (cfg.step_observer) { return Err(ErrorCode::InvalidArgument, "run_backtest: RunConfig::step_observer requires the strategy overload (the fixed-book run has no on_step)"); }`

**Notes:** placement before `validate_strategy_transition` is deliberate and load-bearing — the shadow loop observed
strategy state with **nothing** between `on_step` and the read, so this is the only position that is
definitionally equivalent to it. **`backtest.hpp` does NOT currently include `<functional>`** (verified: its include
block is `<cstddef> <cstdint> <memory> <optional> <span> <string> <string_view> <utility> <vector>` at `:33-41`) —
add `#include <functional>` in sorted position, and note it is a new standard-header cost paid by all 46 including
translation units. **All four `StepEvent` members are read in-tree**: `step_index` by
`StepObserverFiresEveryStepAtStride` (the only way to correlate events with downsampled rows), `ref` by T2's collector
(`.date`), `snapshot` by T2's collector (a fail-closed `ts_ns()` cross-check), `strategy` by T2's downcast.

- [x] **Step 1: Write the failing tests** — in `atx-vol/tests/strategy_test.cpp`, reusing that file's existing
      `make_surface` / `write_archive` / `make_manifest` / `Clock::from_manifest` scaffolding and the 7-date
      `Strategy.OverlappingClips` corpus pattern (`:1087-1101`):
  - `TEST(Strategy, StepObserverFiresOncePerStepInOrder)` — `DeclarativeStrategy` over the 7-date clock;
    `RunConfig cfg; cfg.step_observer = [&](const StepEvent &e){ seen.push_back({e.step_index, e.ref.date,
    e.snapshot.ts_ns(), &e.strategy}); return Ok(); };`. Assert `seen.size() == 7`; `seen[i].step_index == i` for all
    i; `seen[i].date` equals the manifest date for index i (so the ref is the *right* ref, not merely a ref);
    `seen[i].ts_ns` strictly increasing and equal to `kBaseNow + day_off[i]*kDayNs`; every `&strategy == &strat`.
  - `TEST(Strategy, StepObserverFiresEveryStepAtStride)` — same corpus, `cfg.record_every_n = 3`. Assert the observer
    fired **7** times while `result->size() < 7` (the stride downsamples rows, not events). This is what makes
    `step_index` load-bearing.
  - `TEST(Strategy, StepObserverErrPropagatesAndStopsTheRun)` — observer returns
    `Err(ErrorCode::InvalidArgument, "observer stop")` on `step_index == 2`. Assert `run_backtest` returns
    `!has_value()`, `code() == InvalidArgument`, `message()` contains `"observer stop"` (verbatim propagation, no
    wrapping), and the observer fired **exactly 3** times (0,1,2) — the abort is immediate, not deferred.
  - `TEST(Strategy, StepObserverAbsentIsBitIdentical)` — run the same clock + a freshly constructed identical strategy
    twice, once with no observer and once with a pure-recording observer; assert **bit-equality** with `EXPECT_EQ` on
    the raw doubles of every `BacktestResult` column (`nav`, `cash`, all ten `pnl_*`, `pnl_settlement`, `pnl_shares`,
    `financing`, `cost`, the four `gross_*`, both `turnover_*`, `n_open_lots`, both `n_unpriced_*`, and
    `step_pnl_total`), plus `date` and `ts_ns`. This is the zero-impact guarantee the whole wave rests on.
  - `TEST(Backtest, FixedBookRejectsStepObserver)` — B0 overload (`run_backtest(clock, PortfolioState{…}, cfg)`) with
    an observer set → `Err`, `code() == InvalidArgument`, message contains `"step_observer"`. Fail-closed, because a
    silently dropped divergence capture is precisely the Wave B failure class.
- [x] **Step 2: Run tests to verify they fail** — `cmake --build C:\atx\build-rel --target atx-vol-tests`
      → expected FAIL, compile error: `'step_observer': is not a member of 'atx::vol::RunConfig'` (and
      `'StepEvent': undeclared identifier`). Record the exact compiler diagnostic in the task report.
- [x] **Step 3: Implement** `StepEvent` / `StepObserver` / the appended `RunConfig::step_observer` in `backtest.hpp`
      and the three `backtest.cpp` edits. Do **not** touch `python/src/bindings/backtest.cpp`. Do **not** reorder any
      existing `RunConfig` field.
- [x] **Step 4: Run tests to verify they pass** — **full build** (`cmake --build C:\atx\build-rel`, all targets — the
      struct grew, so the library, all 7 examples, both benches and the python module must rebuild together; a
      `--target atx-vol-tests` build would hide a broken example or bench). Then
      `$env:PATH = "C:\atx\build-rel\bin;$env:PATH"; C:\atx\build-rel\bin\atx-vol-tests.exe
      --gtest_filter=Strategy.StepObserver*:Backtest.FixedBookRejectsStepObserver` → 5/5 PASS. Then the regression
      sweep that proves the header change broke nothing:
      `--gtest_filter=Strategy.*:Backtest*:ListedDispersion*:Tearsheet*:RunDir.*:RunArchive*` → all PASS.
- [x] **Step 5: Commit** — `git add atx-vol/include/atx/vol/backtest.hpp atx-vol/src/backtest.cpp atx-vol/tests/strategy_test.cpp`

---

## Task 2: mark-divergence collector in `listed_dispersion_pipeline` (L10 consumer)

**Files:**
- Modify: `atx-vol/include/atx/vol/listed_dispersion_pipeline.hpp`, `atx-vol/src/listed_dispersion_pipeline.cpp`
- Test: `atx-vol/tests/listed_dispersion_pipeline_test.cpp`

**Interfaces (Produces):**

```cpp
// One mark_divergence row: the frozen schedule mark vs the live seed mark for one
// leg on one session. Field order and names mirror the kMarkDivergenceCols registry
// order the example's encoder emits, so the CLI stages a row 1:1 with no reordering.
struct ListedMarkDivergenceRow {
  std::string date, symbol, raw_symbol;
  double strike{0.0};
  std::int64_t expiry_ts_ns{0};
  Side side{Side::Call};
  double schedule_mark{0.0}, live_mark{0.0}, diff{0.0}, abs_diff_bps_of_mark{0.0};
};

// |live - schedule| / |schedule| * 1e4, and EXACTLY 0.0 when |schedule| == 0.
// Lifted verbatim from spy_dispersion_backtest.cpp:733-735. The zero-denominator
// collapse is finding L2 — a KNOWN understatement for deep-OTM legs with a frozen
// mark of 0. It is PRESERVED bit-for-bit here on purpose: the metric feeds a pinned
// artifact, so changing it is a separate, deliberate economic decision, not a
// refactor side effect.
[[nodiscard]] double listed_mark_divergence_bps(double schedule_mark, double live_mark) noexcept;

// Build a StepObserver that appends one row per leg whose live mark diverged from
// its frozen schedule mark on the step just observed. Requires the observed strategy
// to be a ListedDispersionStrategy (Record policy); anything else is a fail-closed
// InvalidArgument. `schedule` and `out` MUST outlive the run_backtest call that
// consumes the returned observer.
[[nodiscard]] StepObserver
make_mark_divergence_observer(const ListedDispersionSchedule &schedule,
                              std::vector<ListedMarkDivergenceRow> &out);
```

**Implementation** — a verbatim lift of `spy_dispersion_backtest.cpp:714-747`, minus the loop and the load:
`dynamic_cast<const ListedDispersionStrategy *>(&ev.strategy)`, null → `Err(InvalidArgument, "mark divergence
observer: strategy is not a ListedDispersionStrategy")`; `last_mark_divergences()` empty → `Ok()`; else
`roll = schedule.rolls[strategy->next_roll_index() - 1u]`; fail-closed cross-check
`ev.snapshot.ts_ns() == roll.valuation_ts_ns` (guaranteed true — `on_step` errors otherwise — so it is a
belt-and-braces guard in the codebase's existing style, and it is what makes `StepEvent::snapshot` load-bearing);
per divergence, match the leg on the same four-way `uid && strike && expiry_ts_ns && side` predicate, `matched ==
nullptr` → `Err(ErrorCode::NotFound, "mark divergence leg not found in roll")` (**message byte-identical** to
`:731`); push a row with `date = ev.ref.date`, `symbol`/`raw_symbol` from the matched leg, `diff = live - schedule`,
`abs_diff_bps_of_mark = listed_mark_divergence_bps(schedule_mark, live_mark)`.

**Notes:** `listed_dispersion_pipeline.hpp` already includes `backtest.hpp` (`:24`), so `StepObserver` / `StepEvent`
are in scope; add `#include "atx/vol/listed_dispersion_strategy.hpp"` for `MarkDivergence` /
`ListedDispersionStrategy`. **No `run_archive.hpp` dependency** — the library returns rows, the example keeps its
`MarkDivergenceArena` + `build_mark_divergence_section` encoder untouched, so no format surface moves.

- [x] **Step 1: Write the failing tests** — in `atx-vol/tests/listed_dispersion_pipeline_test.cpp`:
  - `BpsMetricMatchesTheFrozenFormula` — pin exact values: `(schedule=2.0, live=2.02)` → `100.0`;
    `(schedule=2.0, live=1.98)` → `100.0` (absolute value); `(schedule=0.0, live=0.5)` → **`0.0`** with a comment
    naming L2; `(schedule=-1.0, live=-1.01)` → `100.0`. `EXPECT_EQ` on raw doubles, not `EXPECT_NEAR`.
  - `MarkDivergenceObserverCapturesThePerturbedLeg` — reuse the fixture shape of
    `ListedDispersionStrategy.RecordPolicyAcceptsPerturbedMarkAndRecordsDivergence`
    (`atx-vol/tests/listed_dispersion_strategy_test.cpp:394-441`): build the synthetic surfaces, perturb exactly one
    leg's `model_mark` by `+0.01`, `MarketSnapshot::load(write_archive(dir, source))`, create the strategy with
    `ScheduleMarkPolicy::Record`, call `on_step(*snapshot, 0u, book, next_id)` directly, then invoke
    `make_mark_divergence_observer(schedule, rows)` on a hand-built
    `StepEvent{0u, ref, *snapshot, *strategy}` (with `ref.date = "2026-07-11"`). Assert `rows.size() == 1`; `date`,
    `symbol`, `raw_symbol` come from the matched leg; `strike` / `expiry_ts_ns` / `side` equal the perturbed leg's;
    `schedule_mark == perturbed.model_mark`, `live_mark == the unperturbed mark`, `diff == live - schedule` bit-exact,
    `abs_diff_bps_of_mark == listed_mark_divergence_bps(schedule_mark, live_mark)` bit-exact.
  - `MarkDivergenceObserverIsSilentWhenNothingDiverged` — unperturbed schedule → `on_step` records no divergence →
    the observer returns `Ok()` and appends **zero** rows (the `rows=0` production truth, at unit scale).
  - `MarkDivergenceObserverRejectsAForeignStrategy` — pass a `DeclarativeStrategy` (or any non-listed `IStrategy`) →
    `Err`, `code() == InvalidArgument`, message contains `"not a ListedDispersionStrategy"`.
  - `MarkDivergenceObserverRejectsAnUnmatchableLeg` — mutate the schedule copy the *observer* sees so the roll's leg
    keys no longer match the strategy's recorded divergence → `Err`, `code() == NotFound`, message **exactly**
    `"mark divergence leg not found in roll"` (assert the string, not just the code — Wave B's T2 minor).
- [x] **Step 2: Run tests to verify they fail** — `cmake --build C:\atx\build-rel --target atx-vol-tests` → expected
      FAIL: unresolved / undeclared `make_mark_divergence_observer`, `listed_mark_divergence_bps`,
      `ListedMarkDivergenceRow`. Record the diagnostic.
- [x] **Step 3: Implement** the three interfaces in the pipeline header + `.cpp`, lifting `:714-747` verbatim
      (preserve the comments explaining that divergences populate only on a roll step).
- [x] **Step 4: Run tests to verify they pass** — `cmake --build C:\atx\build-rel --target atx-vol-tests`; then
      `C:\atx\build-rel\bin\atx-vol-tests.exe --gtest_filter=ListedDispersionPipeline.*:ListedDispersionStrategy.*`
      → all PASS (the 5 new plus every Wave B pipeline test).
- [x] **Step 5: Commit** — `git add atx-vol/include/atx/vol/listed_dispersion_pipeline.hpp atx-vol/src/listed_dispersion_pipeline.cpp atx-vol/tests/listed_dispersion_pipeline_test.cpp`

---

## Task 3: dual-run equivalence comparator in the CLI — observer AND shadow, bit-compared (the proof, shadow RETAINED)

**Files:**
- Modify: `atx-vol/examples/spy_dispersion_backtest.cpp` **only**

**Interfaces (Consumes):** T1's `RunConfig::step_observer`, T2's `make_mark_divergence_observer`.

**This task deletes nothing.** `collect_mark_divergence_replay` stays exactly as it is and keeps sourcing the
`mark_divergence` section, so the emitted artifact is provably unchanged by construction. What lands is the arbiter:

In `run_projected_backtest_command` (`:773-873`), when `!skip_divergence`:
1. Keep the existing shadow call at `:829` filling `*divergence_arena` (unchanged).
2. Declare `std::vector<ListedMarkDivergenceRow> observed;` and set
   `config.step_observer = make_mark_divergence_observer(schedule, observed);` **before** the priced
   `run_backtest(clock, strategy, config)` at `:839`. The observer must be installed on the config used by the
   **priced** run only — never on the shadow's own `on_step` calls.
3. After the priced run and its `all_rolls_consumed()` gate (`:840-842`), compare **bit-exactly**:
   `observed.size() == arena.n_rows`, and for every row i: `date`, `symbol`, `raw_symbol` string-equal (decoding the
   arena's dict codes), `strike`, `schedule_mark`, `live_mark`, `diff`, `abs_diff_bps_of_mark` compared with `==` on
   the raw doubles (NOT a tolerance — a tolerance would hide exactly the drift this is looking for), `expiry_ts_ns`
   and the `side` code equal. First mismatch → `Err(ErrorCode::Internal, "mark divergence observer/shadow mismatch at
   row <i> field <name>: observer=<%.17g> shadow=<%.17g>")`.
4. Print the evidence line unconditionally:
   `std::printf("mark divergence equivalence: observer=%zu shadow=%llu rows MATCH\n", observed.size(), arena.n_rows);`
   — this is the string T4 greps for.

**Vacuity guard (mandatory, in this task):** an all-empty comparison proves nothing. The comparator must therefore be
exercised on `--execution configured`, where divergence rows are genuinely nonzero (the fast tier interpolates the
cached surrogate instead of reproducing the cold archive marks — `:813-819`), **and** the comparator itself must be
proven able to fail.

- [x] **Step 1: Write the failing test** — this task's gate is the CLI on a real fixture, not a gtest. Prepare the
      3-session fixture per `.superpowers/sdd/dispersion-parity/task-9-report.md` into a scratch dir (NEVER modify
      `scratchpad\paired`; NEVER touch `C:\atx-data`). The RED is structural: before the change the example has no
      `step_observer` reference at all, so `grep step_observer atx-vol/examples/spy_dispersion_backtest.cpp` is empty
      and no `mark divergence equivalence:` line can be produced. Record that as the pre-state.
- [x] **Step 2: Run to verify it fails** — `cmake --build C:\atx\build-rel --target atxvol_spy_dispersion_backtest`
      on the pre-change tree, run `run-projected-backtest --run <fixture> --execution cold`, confirm **no**
      `mark divergence equivalence:` line is emitted. (Pre-state evidence; the comparator does not exist yet.)
- [x] **Step 3: Implement** the four edits above. Change **nothing** about the shadow loop, the arena, the section
      build, the PhaseTimer phase list, or the priced run's config apart from `step_observer`.
- [x] **Step 4: Run to verify it passes** — rebuild the example, then on the 3-session fixture:
  - `run-projected-backtest --run <fixture> --execution cold` → exits 0, prints
    `mark divergence equivalence: observer=0 shadow=0 rows MATCH`, prints `projected backtest complete [cold]: dates=3
    rolls=1 final_nav=…`, and `runarchive dump <fixture> mark_divergence --tsv` is header-only. Capture its sha256 as
    **`MD-3S-COLD`**.
  - `run-projected-backtest --run <fixture> --execution configured` → exits 0, prints
    `mark divergence equivalence: observer=N shadow=N rows MATCH` with **N > 0**. If N == 0 on this fixture, the
    3-session proof is vacuous: say so in the report and rely on T4's 135-session configured run for the primary
    evidence — do **not** paper over it. Capture `runarchive dump <fixture> mark_divergence --tsv` sha256 as
    **`MD-3S-CFG`** and record N.
  - **RED-PROVE the comparator** (the discipline whose absence let the Wave B defect ship): temporarily add
    `observed.front().live_mark += 1.0;` immediately before the comparison, rebuild, re-run the **configured** route,
    and confirm it aborts with `mark divergence observer/shadow mismatch at row 0 field live_mark: …`. Then **remove
    the probe, rebuild clean, re-run** and confirm MATCH again. Paste both outputs in the report. If N == 0 so the
    probe cannot be placed, instead force it by comparing `observed.size() + 1` against `arena.n_rows` and prove the
    size branch fires.
  - `run-projected-backtest --run <fixture> --execution cold --no-divergence` → exits 0, no observer installed, no
    equivalence line, `projected_nodiv` written and no `mark_divergence` section — unchanged behaviour.
  - Regression: `cd C:\atx\build-rel; .\bin\atx-vol-tests.exe --gtest_filter=ListedDispersion*:Strategy.*:RunDir.*:RunArchive*`
    → all PASS.
- [x] **Step 5: Commit** — `git add atx-vol/examples/spy_dispersion_backtest.cpp`

---

## Task 4: CONTROLLER — 135-session equivalence proof, both routes (BLOCKING gate before any deletion)

**Files:** none. Controller-only; owns `C:\atx-data`. **T5 MUST NOT start until this task is green.**

This is the equivalence proof the design demands: the observer-derived divergence must equal the shadow-loop
divergence **on a real corpus** before the shadow loop is deleted. The comparator from T3 is the instrument; this task
is the run. The named artifact is the `mark_divergence` section, compared two ways: in-process (observer vs shadow,
bit-exact, by the T3 comparator) and out-of-process (`runarchive dump mark_divergence --tsv`, sha256, before vs after).

**[CONTROLLER ERRATUM — Wave D's 6th plan error, measured.** Steps 1 and 2 omit
`--schedule projected_schedule.tsv`. Without it the run defaults to `trade_schedule.tsv` and Step 1's own acceptance
line is unreachable: measured `final_nav=125026.0592`, not `123243.1172`, and its `projected_cold` dump hashed
`a05470c7a6f6572f` — byte-identical to the sprint's **`backtest`** golden, because replaying the trade schedule cold
just reproduces the plain listed backtest. The contract is in code at `spy_dispersion_backtest.cpp:677`
("projected_schedule.tsv stays a text INPUT: run-projected-backtest reads it back via `--schedule`") and the authority
is `parity_full_run.ps1:30-33`, whose Step 4 passes it. **The same omission is in Task 7 Step 3 and Wave E Task 9
Step 3.** Both steps below were run WITH the flag; that is what the checked boxes attest.**]**

- [x] **Step 1:** On the idle box, on the parity-full 135-session corpus, run
      `run-projected-backtest --run <parity-full> --execution cold` with the T3 build. Require: exit 0;
      `mark divergence equivalence: observer=0 shadow=0 rows MATCH`; `projected backtest complete [cold]: dates=135
      rolls=7 final_nav=123243.1172`; `dump projected_cold --tsv` sha256 first-16 == `cbabca44`;
      `dump mark_divergence --tsv` is header-only. Record the `mark_divergence` sha256 as golden **`MD-COLD`**.
- [x] **Step 2:** **The primary, non-vacuous proof.** Run
      `run-projected-backtest --run <parity-full-copy> --execution configured` with the T3 build. Require: exit 0 and
      `mark divergence equivalence: observer=N shadow=N rows MATCH` with **N > 0**. Record N and the
      `dump mark_divergence --tsv` sha256 as golden **`MD-CFG`**. Use a **copy** of the run dir (or a dedicated
      configured-route dir) so the parity-full cold state and its merge-write union are not disturbed.
      **Escalation, do not improvise:** if the configured route cannot complete on this corpus (it is a diagnostic
      route with a known fast-tier accuracy gap), or if N == 0, then the corpus cannot falsify equivalence. In that
      case the controller must EITHER construct a corpus that does produce nonzero divergence (e.g. one leg's frozen
      `model_mark` perturbed in a copied `trade_schedule.tsv`, run cold) OR record explicitly that the only
      falsifiable evidence for L10 is T2's gtest plus T3's RED probe, and decide — on the record — whether that is
      sufficient to authorize the T5 deletion. Silence here is the Wave B failure mode.
- [x] **Step 3:** *(No MISMATCH on either route — this step was not triggered. Recorded so its unchecked state is not
      read as skipped.)* If either comparison reports a MISMATCH, **STOP**. Do not "fix" the observer to match the shadow and
      do not re-pin the golden. Diagnose which side is right: the shadow loads each session with the 2-argument
      `MarketSnapshot::load(path, tier)` while the engine loads through `snapshot_cache->load(path, tier,
      cfg.query_cache_build_policy)`, so a fast-tier / cache-build-policy difference is the most likely cause and the
      **engine's** prices are the economically real ones. Any golden move is an explicit controller economic decision
      recorded in the ledger with the reason, not a task step.
- [x] **Step 4:** *(Recorded in `.superpowers/sdd/backtest-wave-c/progress.md`, the single ledger this sprint uses for
      Waves C/D/E. `MD-COLD` = `c9a04d1bcf0e3c07`, `MD-CFG` = `9e958a90ae15ac74`, N = 137.)*
      Record in `.superpowers/sdd/backtest-wave-d/progress.md`: `MD-COLD`, `MD-CFG`, N, both console
      transcripts, and the explicit statement **"observer-derived divergence == shadow-derived divergence, bit-exact,
      on N > 0 real rows — the T5 deletion is authorized."** Commit the ledger.

---

## Task 5: delete the shadow loop — the observer becomes the sole `mark_divergence` source (L10 payoff)

**Files:**
- Modify: `atx-vol/examples/spy_dispersion_backtest.cpp` **only**

**Preconditions:** T4 green and its authorization statement recorded. Do not begin otherwise.

**Changes:**
1. Delete `collect_mark_divergence_replay` (`:692-757`) entirely, and its call at `:829`.
2. Fill `MarkDivergenceArena` from the observer's `std::vector<ListedMarkDivergenceRow>` after the priced run, in
   `kMarkDivergenceCols` registry order, using the existing `dict_intern` for the three dict columns and
   `side == Side::Call ? 0 : 1` for the enum — i.e. the identical staging code the shadow used, now reading rows
   instead of `MarkDivergence` + a leg lookup. `build_mark_divergence_section` is **unchanged**.
3. Delete the now-dead comparator from T3 **and its `Err(Internal, …)` branch** (there is nothing left to compare) —
   but keep an equivalent evidence line: `std::printf("mark divergence rows: %llu\n", arena.n_rows);`.
4. Retain the shadow's post-loop invariant: the priced strategy's `all_rolls_consumed()` gate already exists at
   `:840-842`, and the comment at `:749-754` ("an empty `mark_divergence` section must mean every roll fired and none
   diverged, never that the replay silently skipped rolls") now applies to that gate. Move the comment there verbatim
   — the evidence-channel contract must not be lost with the loop.
5. **Keep the PhaseTimer phase list byte-stable**: `{"setup_read", "divergence_replay", "archive_load", "priced_run",
   "write_outputs"}` unchanged, because the `diagnostics` section emits one row per pre-declared phase and changing
   the list changes that section's row set. `divergence_replay` now accumulates the observer-callback time
   (`timer.add("divergence_replay", cb_start)` inside the observer wrapper); `archive_load` legitimately reads `0/0`
   because the loads now belong to `priced_run` (the same benign pattern Wave B already accepted for
   project-schedule's `archive_load`). State this in a code comment.
6. Update the `--no-divergence` and `write_mark_divergence_replay` comments at `:692-697`, `:759-772`, `:822-826` —
   they describe a replay that no longer exists. `--no-divergence` now means "do not install the observer".

- [x] **Step 1: Write the failing test** — the gate is byte-identity of the pinned artifact against T3's captured
      hashes. Assert the RED first: on the pre-change tree the shadow is still the source, so
      `grep collect_mark_divergence_replay atx-vol/examples/spy_dispersion_backtest.cpp` matches — that grep going
      empty is the change's signature. Also record the pre-change wall time of `run-projected-backtest --execution
      cold` on the 3-session fixture (the double-pass baseline).
- [x] **Step 2: Run to verify it fails** — n/a as a compile failure; the honest RED here is the T3/T4 comparator,
      which has ALREADY proven the observer equals the shadow. State in the report that this task's safety comes from
      T4's authorization plus the Step-4 byte-identity gate, and that no new RED test is claimed. **Do not invent a
      tautological test to fill this slot** (Wave B minor: `TwoRouteColdParity_LegMarksEqual` was `f(x) == f(x)`).
- [x] **Step 3: Implement** the six changes above.
- [x] **Step 4: Run to verify it passes** — rebuild the example; on the 3-session fixture, both routes:
  - `--execution cold`: exits 0, `dates=3 rolls=1 final_nav=…` identical to T3's line, `dump mark_divergence --tsv`
    sha256 **== `MD-3S-COLD`**, and `dump projected_cold --tsv` sha256 unchanged from T3 (the priced run must be
    untouched by removing the shadow).
  - `--execution configured`: exits 0, `dump mark_divergence --tsv` sha256 **== `MD-3S-CFG`**, row count == N.
  - `--execution cold --no-divergence`: exits 0, `projected_nodiv` written, no `mark_divergence` section.
  - `dump diagnostics --tsv`: **5 phase rows + the total row**, same phase names in the same order as T3's dump
    (`archive_load` may read 0 — that is the documented, expected delta; `wall_ms` values move, they are wall-clock).
  - Report the wall-time delta vs the Step-1 baseline as an observation (the L4/P5 win), **not** as a gate.
  - Regression: `cd C:\atx\build-rel; .\bin\atx-vol-tests.exe --gtest_filter=ListedDispersion*:Strategy.*:RunDir.*:RunArchive*`
    → all PASS.
- [x] **Step 5: Commit** — `git add atx-vol/examples/spy_dispersion_backtest.cpp`

---

## Task 6: L12 — `RunSpec.index_symbol`, de-SPY `all_symbols` / `universe_at`

**Files:**
- Modify: `atx-vol/include/atx/vol/dispersion_workflow.hpp`, `atx-vol/src/dispersion_workflow.cpp`
- Modify: `atx-vol/src/run_archive.cpp` (`encode_meta_section`, `:960-991`)
- Modify: `atx-vol/src/listed_dispersion_pipeline.cpp` (`:205`, `:224`)
- Modify: `atx-vol/examples/spy_dispersion_backtest.cpp` (`:320`, `:541`, `:883`, `:960`)
- Modify: `atx-vol/tests/run_archive_test.cpp` (`RunArchiveEncoders.MetaSectionEchoesResolvedSpec`, `:1059-1098`)
- Create + register: `atx-vol/tests/dispersion_workflow_test.cpp` (`atx-vol/tests/CMakeLists.txt`, beside
  `listed_dispersion_pipeline_test.cpp` at `:102`)

**Interfaces (Produces / changes):**
- `RunSpec` (`dispersion_workflow.hpp:16-37`) gains, **appended after `core_mode`**:
  `std::string index_symbol{"SPY"};` — with a comment stating it is the always-kept index leg, that `"SPY"` is the
  default **so every existing spec, caller and golden is bit-unchanged**, and that new *methodology* knobs go to
  `ListedDispersionMethodology` (`listed_dispersion_pipeline.hpp`) and **not** here, per design §4.6. Wave B's
  post-review round reduced `ListedDispersionMethodology` to exactly the three floors a consumer reads — **do not
  re-add dead fields there**; `index_symbol` is a *universe/input* knob, which is why it belongs on `RunSpec`.
- `all_symbols(std::span<const UniverseRow> rows, std::string_view index_symbol = "SPY")` — the seed vector becomes
  `{std::string(index_symbol)}`, replacing the literal at `dispersion_workflow.cpp:224`. Dedup + sort unchanged.
- `universe_at(std::span<const UniverseRow> rows, std::string_view date, std::string_view index_symbol = "SPY")` —
  `universe.index = DispersionMember{std::string(index_symbol), 0u, 0.0}` and the name filter becomes
  `symbol != index_symbol`, replacing the two literals at `:238` and `:240`.
- `read_run_spec`: `optional_text("index_symbol", spec.index_symbol);` beside the other optional-text keys
  (`:105-107`), **plus** a new guard `if (spec.index_symbol.empty()) return Err(ErrorCode::InvalidArgument, "run spec
  index_symbol must be non-empty");`. Safe by construction: the key did not exist before, so no existing spec can
  reach the new rejection.
- `write_resolved_spec`: `<< "index_symbol\t" << spec.index_symbol << '\n'` **appended last, after `core_mode`**
  (`:177`). Appending last is a binding decision: an old-vs-new `run_spec.tsv` then differs by exactly one trailing
  line, which is trivially auditable.
- `encode_meta_section` (`run_archive.cpp:960-991`): `pairs.emplace_back("index_symbol", spec.index_symbol);`
  **appended last, after `core_mode`** — its doc comment (`:962-965`) requires it to mirror `write_resolved_spec`'s
  key vocabulary and order, so the two appends must match. Bump `pairs.reserve(21 + extra.size())` → `22`.

**Why a defaulted trailing parameter and not `const RunSpec&`:** the pybind11 bindings call
`all_symbols(std::span<const UniverseRow>{rows})` (`python/src/bindings/dispersion.cpp:132`) and
`universe_at(std::span<const UniverseRow>{rows}, date)` (`:123`). A defaulted parameter leaves both **compiling and
behaving identically with zero edits**, which is what keeps Python out of scope. A `RunSpec` parameter would break the
binding build and would also make `dispersion_workflow`'s pure front-end functions depend on `RunSpec` layout.
Do **not** add a `.def_readwrite("index_symbol", …)` to `dispersion.cpp:66-95` — Python is out of scope; record the
omission in the task report so it is a decision, not drift.

**Call sites to thread (all four the reviewer found, plus the two library ones):**

| Site | Change |
|---|---|
| `examples/spy_dispersion_backtest.cpp:320` | `all_symbols(universe_rows, spec.index_symbol)` (`spec` from `read_run_spec` at `:318`) |
| `examples/spy_dispersion_backtest.cpp:541` | `all_symbols(universe_rows, spec.index_symbol)` (`spec` at `:514`) |
| `examples/spy_dispersion_backtest.cpp:883` | `universe_at(universe_rows, clock.refs().front().date, spec.index_symbol)` (`spec` at `:876`) |
| `examples/spy_dispersion_backtest.cpp:960` | `universe_at(universe_rows, clock.refs().front().date, spec.index_symbol)` (`spec` at `:943`) |
| `src/listed_dispersion_pipeline.cpp:205` | `all_symbols(universe_rows, <the builder's RunSpec param>.index_symbol)` |
| `src/listed_dispersion_pipeline.cpp:224` | `universe_at(universe_rows, ref.date, <the builder's RunSpec param>.index_symbol)` |
| `python/src/bindings/dispersion.cpp:123,132` | **UNCHANGED** — defaulted parameter, `"SPY"` behaviour preserved |

**Freeze note (byte-stability, enumerate exactly these deltas and no others):**
1. **The four economic goldens are UNCHANGED** — `dump backtest --tsv` `a05470c7…`, `dump projected_cold --tsv`
   `cbabca44…`, `trade_schedule.tsv` `b640b3ab…`, `projected_schedule.tsv` `d6793d46…`. This is the gate. Defaulted
   `index_symbol == "SPY"` reproduces the previous `all_symbols` / `universe_at` outputs element-for-element, so no
   selection, sizing or pricing input moves.
2. `run_spec.tsv` gains exactly one trailing line `index_symbol\tSPY` — **only when `build-corpus` is re-run**.
   `write_resolved_spec` has exactly one caller (`build_corpus_command`); every later stage only *reads*
   `run_spec.tsv`. So existing `C:\atx-data` run dirs keep their bytes, `RunDir::run_identity_hash()` (which folds the
   **file bytes**, `run_archive.cpp:1503-1504`) is unchanged for them, and merge-write continues to union rather than
   drop sections. Verify this explicitly — it is the difference between a no-op and silently invalidating the parity
   corpus.
3. The `meta` ScalarKV section gains one **row** (`index_symbol=SPY`) in every newly written archive. A row, **not a
   column**: `schema_hash` stays `0xdcce47781ac8390d`, `kRaMinor` stays 0, no registry or `_schema.py` edit. `meta` is
   not a golden, and `RunDir::verify`'s cardinality gate compares `backtest` vs `reconciliation` dates, not `meta`
   rows. Confirm both facts in the report.
4. Nothing else. `batch_spec`, `read_universe`, the `UniverseRow` schema, `universe_schedule.tsv` — all untouched.

- [ ] **Step 1: Write the failing tests** — create `atx-vol/tests/dispersion_workflow_test.cpp` (this closes the
      module's zero-direct-coverage gap) with, at minimum:
  - `RunSpecIndexSymbolDefaultsToSpy` — `EXPECT_EQ(RunSpec{}.index_symbol, "SPY")`.
  - `AllSymbolsDefaultIsUnchanged` — over a fixture row set `{ "AAPL", "MSFT", "SPY" }`, `all_symbols(rows)` (no
    second argument) equals the sorted deduped `{"AAPL","MSFT","SPY"}` — the pre-change behaviour, including the
    dedup when SPY is itself a constituent row.
  - `AllSymbolsHonoursIndexSymbol` — `all_symbols(rows, "QQQ")` contains `"QQQ"` exactly once, still contains
    `"SPY"` (because SPY is a *constituent* here, which the hardcode conflated with the index), size == 4, sorted.
  - `UniverseAtHonoursIndexSymbol` — `universe_at(rows, date, "QQQ")` → `index.symbol == "QQQ"`; `names` **includes**
    `"SPY"` with its authored `raw_weight` and **excludes** `"QQQ"`. This is the behavioural bug the hardcode causes
    (with a non-SPY index the old code both mislabels the index *and* silently drops SPY from the names), so this test
    genuinely fails before the change rather than merely locking behaviour.
  - `UniverseAtDefaultIsUnchanged` — `universe_at(rows, date)` → `index.symbol == "SPY"`, names exclude `"SPY"`,
    weights and ordering identical to the pre-change expectation.
  - `ReadRunSpecDefaultsIndexSymbolWhenAbsent` — write a minimal valid `run_spec.tsv` (the four required keys:
    `date_lo`, `date_hi`, `opra_root`, `universe_schedule`) with **no** `index_symbol` row → parses `Ok`,
    `index_symbol == "SPY"`. The backward-compatibility lock for every existing spec on disk.
  - `ReadRunSpecParsesIndexSymbol` — add `index_symbol\tQQQ` → `index_symbol == "QQQ"`.
  - `ReadRunSpecRejectsEmptyIndexSymbol` — `index_symbol\t` (present, empty) → `Err`, `code() == InvalidArgument`.
  - `WriteResolvedSpecEmitsIndexSymbolLast` — `write_resolved_spec` a default-ish spec, read the file back as text,
    assert the **final** line is exactly `index_symbol\tSPY` and that `read_run_spec` on that file round-trips the
    value (write→read closure).
  - In `atx-vol/tests/run_archive_test.cpp`, extend `RunArchiveEncoders.MetaSectionEchoesResolvedSpec` (`:1088-1096`)
    with `EXPECT_EQ(value_of("index_symbol"), "SPY")` — the meta echo must not silently omit a resolved knob.
- [ ] **Step 2: Run tests to verify they fail** — register the new file in `atx-vol/tests/CMakeLists.txt`, then
      `cmake --build C:\atx\build-rel --target atx-vol-tests` → expected FAIL: `'index_symbol': is not a member of
      'atx::vol::RunSpec'`, and `all_symbols`/`universe_at` "does not take 2/3 arguments". Record the diagnostics.
- [ ] **Step 3: Implement** the header field, the two defaulted signatures, the three literal replacements
      (`dispersion_workflow.cpp:224,238,240`), the parse + empty-guard, the resolved-spec append, the
      `encode_meta_section` append + `reserve` bump, and the six call-site threads in the table. Touch **no** pybind11
      file.
- [ ] **Step 4: Run tests to verify they pass** — **full build** (`cmake --build C:\atx\build-rel`, all targets — a
      public library signature changed and the bindings must be proven to still compile against the default). Then:
  - `C:\atx\build-rel\bin\atx-vol-tests.exe --gtest_filter=DispersionWorkflow.*` → all PASS.
  - `--gtest_filter=RunArchive*:RunDir.*:ListedDispersion*:Tearsheet*` → all PASS, including
    `MatchesCommittedPythonFixture` (proves the frozen fixture bytes and `schema_hash` did not move).
  - `grep -n '"SPY"' atx-vol/src/dispersion_workflow.cpp` → **no matches** (the hardcode is gone).
  - `grep -n 'index_symbol' atx-vol/python/src/bindings/dispersion.cpp` → **no matches** (Python untouched).
  - On the 3-session fixture: re-run the full stage sequence `build-schedule → run-backtest → project-schedule →
    run-projected-backtest --execution cold`; assert `final_nav=-456.5769067`, `dates=3`, `rolls=1`, and that
    `dump backtest --tsv`, `dump projected_cold --tsv`, `trade_schedule.tsv` and `projected_schedule.tsv` are
    **byte-identical to their pre-T6 values on that fixture** (capture the four sha256s before the change).
  - Confirm the enumerated deltas and nothing more: `run_spec.tsv` unchanged in a dir whose `build-corpus` was not
    re-run; `dump meta --tsv` gains exactly one row `index_symbol\tSPY`.
- [ ] **Step 5: Commit** — `git add atx-vol/include/atx/vol/dispersion_workflow.hpp atx-vol/src/dispersion_workflow.cpp atx-vol/src/run_archive.cpp atx-vol/src/listed_dispersion_pipeline.cpp atx-vol/examples/spy_dispersion_backtest.cpp atx-vol/tests/dispersion_workflow_test.cpp atx-vol/tests/run_archive_test.cpp atx-vol/tests/CMakeLists.txt`

---

## Task 7: Wave-D integration gate (controller) + final whole-branch review

**Files:** this plan (check boxes); `.superpowers/sdd/backtest-wave-d/progress.md` (controller-owned ledger).

- [ ] **Step 1: Full build.** `cmake --build C:\atx\build-rel` — every target (library, `atx-vol-tests`, all 7
      examples, both benches, the python module). Exit 0, no new warnings-as-errors. A `--target` build does not
      satisfy this step: `sizeof(RunConfig)` changed in T1 and a public `dispersion_workflow` signature changed in T6.
- [ ] **Step 2: Full gtest from `C:\atx\build-rel` CWD.**
      `cd C:\atx\build-rel; $env:PATH = "C:\atx\build-rel\bin;$env:PATH"; .\bin\atx-vol-tests.exe`
      → all green **modulo exactly** the three documented pre-existing reds
      (`BoundaryHoist.PriceBitIdenticalToPrechange`, `SurfaceV2Qualification…/Latency`, `…/Balanced`). Any fourth
      failure blocks the wave. Record the pass/skip/fail counts (Wave B's post-fix baseline: 3 failed).
- [ ] **Step 3: Parity-full economics — UNCHANGED.** On the 135-session corpus (`C:\atx-data`, controller-only) run
      `build-schedule → run-backtest → project-schedule → run-projected-backtest --execution cold`, then
      `run-projected-var`. Require:
  - listed `backtest complete: dates=135 rolls=7 final_nav=125026.0592`
  - `projected backtest complete [cold]: dates=135 rolls=7 final_nav=123243.1172`
  - daily-pnl `corr=0.99718`; **`mark_divergence rows=0`** (now observer-sourced — this is the L10 payoff assertion)
  - `runarchive dump <DIR> backtest --tsv` sha256 first-16 == **`a05470c7`**
  - `runarchive dump <DIR> projected_cold --tsv` == **`cbabca44`**
  - `trade_schedule.tsv` == **`b640b3ab`**; `projected_schedule.tsv` == **`d6793d46`**
  - projected-VaR anchors from the Wave B ledger: `projected_risk_scenarios.tsv` == **`0cf8ac4b50f34ea6`**,
    `projected_risk_legs.tsv` == **`0a8b38984c7b6064`**, `projected_var.tsv` **with field 7 excluded** ==
    **`d370c78dbb01b513`** (field 7 `projections_per_second` is a wall-clock rate — exclude it via
    `awk -F'\t' 'BEGIN{OFS="\t"}{ $7=""; sub(/\t\t/,"\t"); print }'`), and the pinned values
    `reference_value 280232.52872350701`, 95% `VaR 164113.53597877346` / `ES 169286.48040274251`,
    99% `VaR 172540.63396786354` / `ES 174814.16710811283`, `n_scenarios 135`, `n_positions 22`.
  - `dump mark_divergence --tsv` sha256 == **`MD-COLD`** from T4.
  - `RunDir::verify` / `validate_all()` pass; the merge-write union still holds (9 sections in the shared dir); no new
    loose result TSVs; `kRaMinor` still 0; `schema_hash` still `0xdcce47781ac8390d`.
- [ ] **Step 4: Configured-route equivalence, post-deletion.** On the configured-route dir from T4 Step 2, re-run
      `run-projected-backtest --execution configured` with the **T5/T6** build and require
      `dump mark_divergence --tsv` sha256 == **`MD-CFG`** with the same N rows. This is the one gate that proves the
      deletion preserved the *nonzero* divergence channel, not just the empty one. If T4 Step 2 escalated, record
      here what substituted for it.
- [ ] **Step 5: Fresh-reviewer final whole-branch review**, read-only, over `587ee97..HEAD`, with an explicit charter
      derived from Wave B's miss: **verify the PRODUCTION path, not the seam.** Specifically require the reviewer to
      answer, with file:line evidence: (a) is the observer fired at a point definitionally equivalent to the shadow's
      read (nothing between `on_step` and the read)?; (b) does any consumer downstream of `mark_divergence` still
      assume the shadow's row set, cardinality or ordering?; (c) is every new test falsifiable, and which ones are
      green-locks?; (d) does any `StepEvent` member or new `RunSpec` field go unread by in-tree code (the dead-field
      trap Wave B had to undo)?; (e) does the L12 default reproduce the pre-change `all_symbols` / `universe_at`
      outputs element-for-element, argued from the code and not from the goldens alone?; (f) is `record_every_n > 1`
      handled consistently between events and rows?
- [ ] **Step 6: Commit** the ledger + this checked plan.

---

## Vacuity Ledger (each task's report MUST fill the last column)

| # | Gate | What makes it falsifiable | Proven RED by |
|---|---|---|---|
| T1 | Observer fires once per step, in order, with the right refs | A wrong insertion point, an off-by-one on `step_index`, or a missed inception fire all break it | compile error: `step_observer` is not a member |
| T1 | `record_every_n>1`: 7 events vs <7 rows | Firing inside the `record` branch would fail | (same build) |
| T1 | `Err` propagates verbatim and halts | Swallowing or wrapping the error fails; a deferred abort fails the "exactly 3 fires" assert | (same build) |
| T1 | Absent observer ⇒ bit-identical `BacktestResult` | Any accidental state touch in the hook fails on raw-double `EXPECT_EQ` | (same build) |
| T1 | B0 rejects an observer | A silent drop passes nothing — the test demands `InvalidArgument` | (same build) |
| T2 | bps metric exact, incl. the L2 zero-denominator `0.0` | A "fix" to L2 flips this test — deliberate, so drift is loud | undefined symbol |
| T2 | Row fields from a real perturbed-leg `on_step` | Not `f(x)==f(x)`: `live_mark` comes from the archive, `schedule_mark` from the perturbed schedule | undefined symbol |
| T2 | Foreign strategy / unmatchable leg ⇒ fail-closed, exact messages | A `dynamic_cast` returning null and being ignored would pass a weaker test | undefined symbol |
| **T3** | **observer rows == shadow rows, bit-exact, in production code** | **Explicit `live_mark += 1.0` probe must abort the run; probe then removed** | **the probe run's mismatch output** |
| T3 | `--execution configured` N > 0 | An all-empty compare is declared vacuous in the report | N recorded |
| T4 | 135-session both-route equivalence | Cold alone is vacuous (rows=0); configured carries the proof; escalation path if N==0 | controller transcript |
| T5 | `dump mark_divergence --tsv` == `MD-3S-COLD` / `MD-3S-CFG` | Any row, ordering or formatting drift changes the sha256 | no new RED claimed — T4 authorizes |
| T5 | `projected_cold` dump unchanged | Proves removing the shadow did not perturb the priced run | (same run) |
| T6 | `universe_at(rows, date, "QQQ")` keeps SPY in `names` | Fails before the change: the hardcode drops SPY *and* mislabels the index | compile error: not a member |
| T6 | `read_run_spec` without the key ⇒ `"SPY"` | Making the key required would fail every existing spec | (same build) |
| T6 | Four economic goldens byte-identical | Any behavioural change in the default path moves a sha256 | pre/post sha256 pair |
| T7 | Full gtest = exactly 3 known reds | A fourth failure blocks | count recorded |
| T7 | 8 goldens + 6 pinned VaR values + `rows=0` | Any economic drift moves one | controller transcript |

---

## Batching (for parallel Opus subagents)

The ONE-build-slot rule serializes every C++ task; there is no Python lane this wave, so the schedule is essentially
linear. Read-only reviewers shadow each commit in parallel.

- **Batch 1: T1** (build slot; `backtest.hpp`/`.cpp` + `strategy_test.cpp`) — must be first, everything depends on it.
- **Batch 2: T2** (build slot; pipeline module + its test) ∥ T1 reviewer (read-only).
- **Batch 3: T3** (build slot; example only) ∥ T2 reviewer.
- **Batch 4: T4** — **controller only** (owns `C:\atx-data`), never a subagent. BLOCKING.
- **Batch 5: T5** (build slot; example only) ∥ T3 reviewer.
- **Batch 6: T6** (build slot; workflow + run_archive + pipeline + example + new test) ∥ T5 reviewer.
- **T7** = controller gate + fresh whole-branch reviewer.

T6 is file-disjoint from T1–T5 except for `spy_dispersion_backtest.cpp`, so it *could* be authored during Batches 2–5,
but it must be **built and committed after T5** to keep each commit's byte-stability gate attributable to one change.

---

## Self-Review

**Spec coverage (Wave D slice of design §4.5/§4.6/§8/§9):**
- §4.5 `backtest.hpp` CHANGED — optional `StepObserver` on `RunConfig`, fired after each `on_step` with strategy
  access → **T1**. ✓
- L10 payoff: mark-divergence reuses the single real engine run; the shadow loop and its second engine pass are gone
  → **T2** (consumer) → **T3** (proof) → **T4** (real-corpus proof) → **T5** (deletion). ✓
- L4/P5 side benefit (the divergence replay's ~60 duplicate `MarketSnapshot::load`s bypassing the shared cache)
  disappears with the shadow → **T5**, reported as an observation, not a gate (perf is Wave E). ✓
- §4.6 `dispersion_workflow.hpp` CHANGED — `RunSpec.index_symbol`, SPY hardcode gone from `all_symbols`/`universe_at`,
  module stays the pure config/input front-end, methodology knobs stay in `ListedDispersionMethodology` → **T6**. ✓
- §9 testing strategy — unit tests for the extracted economics (T2's bps metric + observer), invariant regression
  (T1's bit-identity, T6's default-path identity), end-to-end 135-session reproduction of the validated economics
  incl. zero mark divergence (T7), byte-stability gates on artifacts whose consumers are not ported (T5, T6, T7). ✓
- Blast radius of the `RunConfig` change named and bounded: 46 including files, zero positional-init sites, zero
  layout assertions, no serialization path, pybind11 list deliberately not extended, full-build requirement. ✓
- **Correctly OUT of Wave D:** `backtest_driver` spine (Wave C/L11), perf P1–P7 (Wave E), L2's zero-denominator bps
  (preserved on purpose, T2), the `mark_divergence` registry/encoder (frozen), all Python, the three known reds.
  Noted, not gaps.

**Freeze integrity:** no task touches `run_archive_schema.hpp`, `_schema.py`, any on-disk struct, or any committed
fixture. The single format-adjacent edit is T6's one extra `meta` **ScalarKV row**, which cannot move `schema_hash`
(rows are data, columns are schema) — asserted by `MatchesCommittedPythonFixture` staying green.

**Anti-vacuity:** the Vacuity Ledger is a required deliverable; the L10 equivalence proof runs in production code on a
real corpus with a mandated RED probe; the cold route is explicitly declared insufficient on its own; T5 refuses to
invent a tautological test for its RED slot and instead points at T4's authorization; T7's reviewer charter names the
six questions Wave B's review had to discover for itself.

**Placeholder scan:** every seam names a real site — `backtest.cpp:1862`/`:1999` (the two `on_step` calls),
`:1361-1560` (B0 body), `backtest.hpp:52`/`:300`/`:340` (forward decl / struct / tail),
`spy_dispersion_backtest.cpp:692-757`/`:829`/`:839`/`:320`/`:541`/`:883`/`:960`/`:733-735`/`:731`,
`dispersion_workflow.cpp:224`/`:238`/`:240`/`:105-107`/`:177`, `run_archive.cpp:960-991`/`:1503-1504`,
`listed_dispersion_pipeline.cpp:205`/`:224`, `listed_dispersion_strategy_test.cpp:394-441`,
`run_archive_test.cpp:1059-1098`, `strategy_test.cpp:1087-1101`, `tests/CMakeLists.txt:102`/`:104`. Every golden is a
literal hash. No "TBD" and no "similar to". ✓

## Open questions for the controller (decide before dispatching T1)

1. **`StepEvent` shape.** Four members, all read in-tree (T1/T2 name where). Confirm no `PortfolioState` member is
   wanted — the shadow never read the book, and Wave B had to delete four dead fields from
   `ListedDispersionMethodology` because "a field nothing reads is an active trap". Add it later, with a reader.
2. **Downcast vs virtual.** T2 reaches `last_mark_divergences()` / `next_roll_index()` through
   `dynamic_cast<const ListedDispersionStrategy *>`. The alternative — a virtual
   `std::span<const MarkDivergence> last_divergences()` on `IStrategy` defaulting to empty — widens the strategy
   vtable (a documented ABI surface, `backtest.hpp:157`) for one consumer. Recommend the downcast; confirm.
3. **T4 Step 2 fallback.** If `--execution configured` cannot complete on the parity corpus or yields N == 0, choose
   between the perturbed-`model_mark` copied-schedule corpus and an on-the-record decision that T2's gtest plus T3's
   RED probe suffice. Decide **before** T5 is dispatched, not during it.
4. **`index_symbol` key position.** This plan appends it last in both `write_resolved_spec` and
   `encode_meta_section` (single trailing added line, trivially auditable diff). Confirm, since the two must match by
   `encode_meta_section`'s own mirror contract.
5. **Wave C ordering.** Design §8 sequences Wave C before Wave D, but Wave C is only being planned now and Wave D is
   independent of it. Confirm Wave D lands first, and that Wave C's `RunConfigOverlay` will be required to carry
   `step_observer`.
6. **Concurrent `RunConfig` tail edits.** F2 (`book_entry_fill_slippage`) and F3 (discrete dividends) append to the
   same tail on other branches. Confirm Wave D appends exactly one field and that the eventual merge is the
   controller's to resolve.

---

## Controller decisions (pre-dispatch, 2026-07-24)

The plan is **accepted as written**, including its task order and the
`StepObserver`/`StepEvent` signature. Points made binding:

1. **T4 is the controller's, and it BLOCKS T5.** The 135-session equivalence proof
   runs on `C:\atx-data` (controller-only). The shadow loop is not deleted until the
   observer-derived `mark_divergence` is proven bit-identical to the shadow-derived
   one on BOTH routes. No implementer may delete it, and T5 is not dispatched until
   T4's comparison is recorded in the ledger.
2. **The anti-vacuity requirement is binding, not advisory.** The cold route's
   `mark_divergence rows=0` cannot falsify anything — an observer that produced
   nothing at all would pass it. The proof therefore rests on
   `--execution configured`, where rows > 0, and the comparator itself gets a RED
   probe showing it fails when the two sources genuinely differ. Wave B shipped a
   defect precisely because a test could not fail; a Vacuity Ledger entry per task is
   required, and a task whose entry says "cannot fail" is not done.
3. **`index_symbol` defaults to `"SPY"`.** L12 removes a hardcode; it does not change
   behaviour. Every existing caller and every golden must be bit-unchanged with the
   default in place, and the byte-stability gate must show it.
4. **T1 mandates a FULL `cmake --build C:\atx\build-rel`**, not a single target.
   `sizeof(RunConfig)` grows and 46 files include `backtest.hpp`; the field is
   appended at the tail and defaulted, which the planner verified is source-compatible
   (no positional aggregate-init sites, no `sizeof`/`offsetof` asserts, nothing hashes
   `RunConfig`) — but "source-compatible in principle" is not evidence, and a full
   build is.
5. **Do not extend the pybind11 `.def_readwrite` list.** Python is out of scope for
   this sprint. Leaving `StepObserver` unexposed is deliberate, not an oversight.
