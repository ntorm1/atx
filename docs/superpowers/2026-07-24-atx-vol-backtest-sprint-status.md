# atx-vol backtesting sprint — status

**As of** 2026-07-24, HEAD `2858cab` on `main`.
**Stopped mid-Wave-C at the user's request.** Nothing is in flight; one task's edits sit
uncommitted (see [Uncommitted work](#uncommitted-work)).

The sprint's charter: productionize the backtesting pipeline — get the economics out of a
~1000-line example file into the library, give results a real storage format, and improve
correctness and performance. Waves A and B are closed. Wave C is half done. D and E are
planned but not started.

---

## Where each wave stands

| Wave | Scope | Status |
|---|---|---|
| **A** | `run_diagnostics` + `run_archive` + the RunArchive binary format + schema single-source + pure-Python reader | **CLOSED** `0f485e6` |
| **B** | `listed_dispersion_pipeline` — extract the dispersion economics under test, fix M1, guard the I1 two-route parity, thin the CLI | **CLOSED** `587ee97` |
| **C** | `backtest_driver` spine; migrate the example drivers | **4 of 7 tasks**, stopped |
| **D** | `StepObserver` (L10) retires the divergence shadow loop; de-SPY `dispersion_workflow` (L12) | Planned, not started |
| **E** | Perf — 4 passes kept of the original 7 | Planned, not started |

### Commit chain this session

```
2858cab  refactor(vol): migrate spy_strangle_backtest onto run_timed      (C T4)
18dc1ef  refactor(vol): migrate the two zero-arg synthetic drivers        (C T3)
b3c10d3  test(vol): capture every driver's pre-migration byte golden      (C T2)
e996f2c  feat(vol): backtest_driver spine — RunOutcome + run_timed        (C T1)
4b57451  docs(vol): plan Waves C/D/E — and correct the design spec
587ee97  docs(vol): pin the run-projected-var route — capture its golden
97ad356  docs(vol): Wave B closeout
1157a03  fix(vol): Wave B post-review round — 3 Important findings fixed
```

---

## The two findings that should change your plans

### 1. Wave C's justification does not hold up

Finding **L11** claimed a strategy-agnostic 9-stage spine was copy-pasted across 5 of 6
drivers. Measured stage by stage before any code was written:

| Stage | Shared across the 5 drivers |
|---|---|
| `Args`/`split`/`join`/`fmt_num` | 2/5 |
| RunConfig overlay | 2/5 |
| Clock source | **0/5 identical** |
| Strategy construct | **0/5 identical** |
| Output shape | **0/5** (four genuinely distinct shapes) |
| `EngineRunStats` | 1/5 |
| Timed engine call → `tearsheet` → stats | **5/5** — but 4 of its 6 nodes were already library calls |

So the wave extracts one small function, not the `BacktestJob` the design spec specifies.
The migrations bear this out — measured line deltas:

```
dispersion_backtest   209 → 208   (−1)
strategy_examples     246 → 246   ( 0)
spy_strangle          635 → 641   (+6)   exec −6, comments +8
```

**Wave C is a wash on code size.** Every byte gate passed, so nothing is broken; there
simply is not much duplication to remove. `mag7` (T6, unstarted) is the one genuine case —
the only driver of five that hand-builds `EngineRunStats`.

Wave C's real deliverables, stated honestly, are: the measurement that disproved L11, and
**10 driver byte-goldens that did not previously exist**. Both are worth having. Neither is
what the wave was chartered to do.

### 2. Three statements in design spec §4.3 are wrong

Recorded as errata in the design spec itself (`4b57451`):

- There is **no third "tradeable manual evaluator" engine slot**. `spy_strangle_tradeable.cpp`
  has no `run_backtest`, no `BacktestResult`, no `tearsheet` — it emits three comparison
  series over date pairs. Two slots, not three.
- The **"sixth driver"** is Wave B's thin `run_surface_backtest_command`, which has neither a
  tearsheet nor stats, so it is not a spine instance either.
- The **return type** `{BacktestResult, TearSheet, EngineRunStats}` is correct and is what was
  built; only the `BacktestJob` *input* struct is unsupported by the code.

---

## Wave C detail

| Task | What | Result |
|---|---|---|
| T1 | `RunOutcome` + two `run_timed` overloads + tests | `e996f2c`. RED confirmed as a build error. 5/5 green. **Gate proven live**: a probe mutating `cash` and `n_steps` failed 3 of 5 tests, then reverted. |
| T2 | Capture pre-migration byte goldens | `b3c10d3`. **10/10 artifacts gated, 0 ungated.** Each hash confirmed over 3 runs, one after wiping all temp dirs. |
| T3 | Migrate `dispersion_backtest`, `strategy_examples` | `18dc1ef`. 3/3 hashes unmoved. |
| T4 | Migrate `spy_strangle_backtest` | `2858cab`. 2/2 hashes unmoved; unfiltered CSV audited line-by-line, exactly 2 lines differ and both are in-filter. |
| T5 | Migrate `spy_dispersion_pnl` | **Incomplete, uncommitted.** |
| T6 | Migrate `mag7` | Not started — the wave's one real dedup. |
| T7 | Controller gate | Not started. |

**Goldens captured in T2** (`sha256[0..16)`), all recorded in
`.superpowers/sdd/backtest-wave-c/progress.md`:

```
dispersion.tsv            87DA84887A2793AE     example_a.tsv    59A8C0174510C8D8
example_b.tsv             5647023F4B98FEC8     spy_short.tsv    57A351D477E84F10
spy_short.csv (filt)      1B632185037D31B5     mag7 series      128DBD4E99118D36
mag7 strategy_metrics     D49500348A9E5B3C     mag7 engine (f)  7A56EA26F3EC5395
mag7 db_stats             6916983A49E258C5     pnl_track (filt) CC90B900A7116CC3
```

Only `wall_clock_ms` and its derived `steps_per_s` drift anywhere — the filters are exactly
`^# (wall_clock_ms|steps_per_s)=` and `^(wall_clock_ms|steps_per_s),`. `# snapshot_preload_ms`
and all four `# pricing_*` lines are stable and stay **inside** the gate.

### Plan errors found during execution

Each was reported by the implementer rather than worked around:

1. My claim that `expect_result_bit_identical` covered only 10 columns is wrong for
   `tearsheet_test.cpp:199-246`, which already covers all 25 (T1).
2. `db->symbols()` is **empty** — `write_partition` only refreshes provenance on
   already-registered symbols and never adds any (`src/surface_db.cpp:1122-1135`) (T2).
3. mag7 emits **four** files, not five — `populate_stats.csv` does not exist (T2).
4. A specified test was already implemented verbatim by T1's suite; correctly rescoped to
   the emitted TSV bytes instead of duplicated (T3).
5. My T4 "RED" was a **positive control** — its success criterion was that the hash still
   matches, which cannot demonstrate a gate can fail. A real negative control was added:
   5 one-byte perturbations each moved the hash, with `wall_clock_ms` correctly not moving
   it (T4).

Error 5 is the same category that let Wave B's M1 defect ship. It was caught this time.

### A reporting inaccuracy in T4

T4 reported `spy_strangle_backtest.cpp` as `605 → 611`. The file was **635** before and is
**641** now. The delta (+6) is correct; the absolute numbers were off by 30. The byte gates
are unaffected — they hash artifacts, not source.

---

## Uncommitted work

**Wave C T5 was stopped mid-task.** It had passed its artifact gate (`pnl_track.tsv` hash
reproduced on a second run) and was running the full suite when it was killed. Left in the
tree, unverified and uncommitted:

```
atx-vol/examples/spy_dispersion_pnl.cpp    +30 / −12   (554 → 560)
atx-vol/tests/backtest_driver_test.cpp     +95         (new test)
```

Your call whether to finish T5, revert those two files, or leave them. I have not reverted
anything.

**The Python test refactor** is also uncommitted and **unverified** — it never got a timed
green run before Python work was dropped. It trims the suite to a minimal example
(committed archive fixture replacing 6 C++ pipeline spawns; `test_backtest.py` cut to a BAW
fixture and one backtest instead of 96 Andersen-Lake surfaces and six). Files:
`atx-vol/python/tests/test_{backtest,report,parity,runarchive,dispersion_runarchive_e2e}.py`,
`pyproject.toml`, `README.md`, plus two new fixtures under `tests/data/runarchive/`.

Unrelated pre-existing work in the tree (untouched throughout): the Python binding split
(`src/bindings/*.cpp`), surface-db work, `atx-db/`, `atx-kb/`, `atx-core` sqlite changes.

---

## Tooling hazards discovered — these affect any future session

1. **`diff` via the Bash tool returns exit 0 even when the files differ.** Measured on a
   purpose-built fixture: the proxied `diff` prints the *correct* textual difference, but
   its **exit status is always 0**, while `rtk proxy diff` correctly returns 1. So reading
   its output is safe; branching on `$?`, `&&`, `||`, or `if diff a b; then` silently
   concludes "identical". Use PowerShell (`Get-FileHash`, `Compare-Object`) or `sha256sum`
   for every byte comparison, and print both values.
   *(Correction: an earlier version of this document said `diff` and `grep` "return WRONG
   answers" and that `diff` reported two files identical when their sha256 differed. That
   overstated it. `grep` was re-tested and is clean — correct counts and correct exit codes,
   including through a pipe. The hazard is exactly one thing: `diff`'s exit code.)*
2. **The Grep *tool*'s content output is also unreliable** — it rendered `//` as `\` in a
   header (T3). Use Read when exact characters matter.
3. **`std::getenv` trips `/WX`** — use `_dupenv_s`.
4. Full gtest **must** run from CWD `C:\atx\build-rel`; a stale repo-root `artifact-cache/`
   causes ~11 false failures otherwise.

---

## Test and gate baselines

- **gtest**: 2020 ran / 1974 passed / 43 skipped / **3 failed** / 7 disabled, from
  `build-rel` CWD. The 3 failures are documented pre-existing reds, unrelated to this
  sprint: `BoundaryHoist.PriceBitIdenticalToPrechange` (1-ULP SSE2 golden-pin drift) and the
  two `SurfaceV2Qualification` budget tests (cap lowered by an unrelated on-main perf commit;
  the re-pin lives on an unmerged branch).
- **135-session parity** (`C:\atx-data\spy-dispersion\runs\parity-full`): `dates=135 rolls=7`,
  listed `final_nav=125026.0592`, projected-cold `123243.1172`, `corr=0.99718`,
  `mark_divergence rows=0`. Four goldens exact: `a05470c7…` `cbabca44…` `b640b3ab…` `d6793d46…`.
- **projected-VaR** (pinned this session): `projected_risk_scenarios.tsv` `0cf8ac4b50f34ea6`,
  `projected_risk_legs.tsv` `0a8b38984c7b6064`, `projected_var.tsv` **field 7 excluded**
  `d370c78dbb01b513`. The exclusion is required — `projections_per_second` is a wall-clock
  rate and moved 31800.9 → 32822.3 across back-to-back runs.

---

## What remains

**Wave C** — T5 (finish or revert), T6 mag7, T7 gate. Given the measured line deltas, the
defensible options are to finish it for consistency, or to stop and keep T1+T2 (the spine
and the goldens are useful regardless of whether every driver adopts them).

**Wave D** — 7 tasks, planned in full. The `StepObserver` signature is settled and every one
of its four members is read in-tree. Its equivalence proof deliberately rides on
`--execution configured` rather than the cold route, because the cold route's
`mark_divergence rows=0` **cannot falsify anything** — an observer emitting nothing at all
would pass it. Task 4 (135-session equivalence, controller-owned) blocks deletion of the
shadow loop.

**Wave E** — 9 tasks. Three of the original seven perf passes were dropped on evidence: P4's
parallelism already exists (`n_threads=0` = auto), P6 would re-baseline a byte-golden
`surface_fingerprint` rather than speed anything up, and P7's stated precondition is provably
false. Of the four kept, P5 is 2.2× larger than the review assumed and P3 is understated.
P1 is gated on P3's measurement — if P3 removes most of the parse cost, P1's persistent cache
may not be worth its staleness risk, and the plan says to report that and stop.

**Sprint close** — one fresh whole-branch review over A–E, per your choice of per-wave gate
plus a single final review. Wave B's final review is what caught a real defect that nine
task-level reviews had missed, so this one is worth its cost.

---

## Deferred, carried out of Wave B

16 minor findings in `.superpowers/sdd/backtest-wave-b/minors-triage.md`. The largest is the
`run_report.cpp` CSV dedup, which is a schema decision rather than a cleanup: the CSV column
order is a published contract pinned in `run_report_test.cpp` and `mag7_dispersion_report_test.py`,
it is a permutation of the canonical order with `nav` displaced from index 14 to index 1, and
deduping the `.cpp` would still leave three more copies of the list.
