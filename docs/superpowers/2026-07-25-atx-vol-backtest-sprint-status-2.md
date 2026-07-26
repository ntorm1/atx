# atx-vol backtesting sprint — status update

**As of** 2026-07-25, HEAD `79b2fa6` on branch **`sprint/atx-vol-backtest-waves-cde`**.
**Stopped at the user's request, mid-Wave-E.**

Nothing has been merged into local `main`, per instruction. `main` still points at
`2858cab` (Wave C T4). Branch is **25 commits ahead**, 30 files, **+4588 / −231**.

**One landed commit is UNREVIEWED** — `79b2fa6`, see [Stop point](#stop-point).

This file supersedes `2026-07-25-atx-vol-backtest-sprint-status.md` for everything after
Wave C. That file's Wave C section and its residual-risk list are still current; its
tooling section is superseded by the table below.

---

## Where each wave stands

| Wave | Scope | Status |
|---|---|---|
| **A** | RunArchive binary format, `run_diagnostics`, schema single-source, pure-Python reader | **CLOSED** `0f485e6` |
| **B** | `listed_dispersion_pipeline` — dispersion economics under test, M1 fixed, CLI thinned | **CLOSED** `587ee97` |
| **C** | `backtest_driver` spine; all five example drivers migrated | **CLOSED** `cb875dc` |
| — | pre-Wave-D hygiene: full Release build green | **DONE** `017fc5c` |
| **D** | `StepObserver` (L10) retires the divergence shadow loop; de-SPY `dispersion_workflow` (L12) | **CLOSED** `876e0d5` + fix `0622d52` |
| **E** | Perf — now **3** live passes of the original 7 | **T1 closed, T2 dropped, T4 landed unreviewed**; T5/T3/T6/T7/T8/T9 not started |

### Commits added this session

```
79b2fa6  perf(vol): memoize trade_end, lazy table fingerprint        (E T4)  UNREVIEWED
0622d52  fix(vol): gate observer coverage                            (D T8)  reviewed ✅
93e5c4d  docs(vol): close Wave E T1
d844f26  perf(vol): split the two invisible dispersion phases        (E T1)  reviewed ✅
c1db597  docs(vol): status doc — Wave D closed, byte-gate hazard five deep
876e0d5  docs(vol): Wave D whole-branch review — Approved, 0 Critical
5ab3778  docs(vol): Wave D T7 gate — Steps 1-4 GREEN, six L12 sites executed
20d0c00  docs(vol): close Wave D T6
0a895b8  refactor(vol): RunSpec.index_symbol replaces the SPY hardcode (D T6) reviewed ✅
3e1db2b  docs(vol): close Wave D T5
d955e93  refactor(vol)!: delete the mark-divergence shadow loop       (D T5)  reviewed ✅
47c0d35  docs(vol): close the T3 fix round — re-review clean
819c442  docs(vol): Wave D T4 — 135-session equivalence proof GREEN
```

---

## Wave D — closed

Wave D retired a **shadow replay loop** that re-walked the clock and re-loaded every
archive to recompute mark divergence the engine already knew as it stepped. The payoff is
deleting working code, so the wave was built entirely around proving the replacement
equivalent *first*.

| Task | Result |
|---|---|
| T1 `StepObserver` substrate | `42c60d8`, approved first pass |
| T2 divergence collector | `c438a64`, clean after 1 fix round |
| T3 dual-run comparator | `5c227e8`, re-review 0 Crit / 0 Imp |
| **T4 135-session equivalence proof** | `819c442` — **N = 137 rows, bit-exact** |
| **T5 delete the shadow** | `d955e93` — **−193 lines (−150 code)** |
| T6 L12 de-SPY | `0a895b8`, Approved |
| T7 gate + whole-branch review | `5ab3778`, `876e0d5` — **PASS** |
| T8 evidence-gate fix | `0622d52`, Approved |

**Suite 2020 → 2047 ran, 1974 → 2001 passed.** Skipped/failed/disabled unchanged; the 3
failures remain exactly the documented pre-existing reds.

**Gate:** all four economic goldens MATCH on the 135-session corpus (`a05470c7`,
`cbabca44`, `b640b3ab`, `d6793d46`), plus 3 VaR anchors, all 6 pinned VaR values, and
`corr=0.99718` — the last computed directly from the two archive sections because the
Python parity report is out of scope. `parity-full` was never written: `D88BFEE04D3EF300`
before the first copy and after the last run.

**The central claim rests on three independent proofs, from three different agents:**
T4's 137-row bit-exact equivalence on the production corpus; T5's reviewer comparing
**per-section binary payload crc32c + sha256** inside the container (catching what a TSV
dump structurally cannot — a changed dictionary-encoding order would render identical text
and a different `run.atxrun`); and T7 reproducing `MD-CFG = 9e958a90ae15ac74` at N=137
*after* the shadow was gone.

### The finding that justified the whole-branch review

Wave D's fresh reviewer found the **evidence-channel contract had been silently weakened**.
The retained `all_rolls_consumed()` gate cannot distinguish *"the observer ran and found
zero divergences"* from *"the observer was never installed"* — but the comment T5 carried
onto it claimed exactly that distinction. The shadow's gate could make it, because it
interrogated the same object its collection loop read.

The T5 dispatch had named this as the single highest-value check in that review; T5's
reviewer examined it and accepted the carry; the overclaim still survived to a third pair
of eyes. Fixed in `0622d52` by counting observer callbacks and gating against
`clock.size()`. Its RED is the strongest shape available — the failure was demonstrated on
the **fixed** binary (exit 1, naming the real failure) *and* on the **unfixed** one (exit 0,
byte-identical 98-byte empty section).

That fix's reviewer then proved the point harder than the report had: the mutant-unfixed
section hashes `c9a04d1bcf0e3c07`, **byte-identical** to an honest run, diffcount 0. The
hash gate is *provably blind* to the defect, so the count gate is what makes an empty
section trustworthy.

---

## Wave E — the measurement changed the plan

**Wave E T1 (`d844f26`, reviewed ✅) split the previously-blended timer phases and measured
where the time actually goes.** That measurement, not the plan, now drives the wave.

| Phase | Share |
|---|---|
| `definitions_parse` | **95.2%** of `build-schedule`, **71.8%** of `run-backtest` |
| `quote_join` | 81.5% of the old `reconciliation` aggregate |
| `reconcile` | 17.4% |
| `snapshot_load` | **0.23%** |

Both figures independently reproduced by T1's reviewer. T1 also captured **seven section
goldens** — including `reconciliation` and `contract_marks`, which had **no hash anywhere
in the sprint** and are exactly what a changed join feeds. Landing P2 without them would
have been an unguarded economics change.

### Three passes dropped or reordered on evidence

- **P5 — DROPPED, already delivered.** P5 existed to route the mark-divergence replay's
  duplicate per-session load through the shared `SnapshotCache`. **That replay no longer
  exists** — Wave D T5 deleted it. Decisive evidence: `run-projected-backtest`'s
  `archive_load` phase now measures **exactly 0 ms / count 0**. *(I first cited
  `snapshot_load` at 0.23%; the reviewer correctly pointed out that is a different
  subcommand and therefore corroborating, not decisive. Recorded, because citing the wrong
  evidence for a right conclusion is the habit this sprint keeps catching.)*
- **P4, P6, P7 — dropped earlier on evidence** (parallelism already exists; `surface_fingerprint`
  is a byte-golden; P7's precondition provably false).
- **P3 reordered ahead of P2** — P3's target is 71.8–95.2% of the two subcommands, P2's is
  a slice of something much smaller and 0% of `build-schedule`. P3 first also feeds T7's
  P1 GO/NO-GO, which the plan already gates on P3's measurement.

**Six of the original seven perf passes are now dropped on evidence or reordered by it.**
That is the plan working, not failing.

### Wave E T4 — landed, unreviewed

`79b2fa6`, 3 files, +429/−5. Its commit message carries the full rationale and numbers.

- **P3(a)** — each of ~8.7M rows was building a 30-character timestamp string (past MSVC's
  15-char small-string capacity, so a heap allocate + free per row) to re-derive a date
  that only takes ~60 distinct values. Now formatted into a `char[32]` and memoized in a
  single `(date, bound)` slot — exact because the rows are sorted by `definition_key`,
  whose first field is `trade_date`, immediately above the loop.
- **P3(c)** — `create` eagerly re-serialized the whole table to hash it, so peak RSS carried
  the file bytes, the row vector and a throwaway ~700 MB serialization simultaneously, for
  a value the backtest read path never reads. Now computed on first call and memoized.

**Measured** (shared 696 MB fixture, both sides warmed with a discarded run, median of 3):

| | before | after (a) | after (a)+(c) |
|---|---|---|---|
| `definitions_parse` build-schedule | 20317 ms | 17509 ms | **13699 ms** |
| `definitions_parse` run-backtest | 20560 ms | 16892 ms | **13776 ms** |
| peak working set | 2797.5 MB | — | **1806.4 MB (−991 MB, −35.4%)** |

≈**33% off the dominant phase** and **−991 MB peak RSS**. The win is attributed to (a) and
(c) separately, as required, rather than reported as one blended number.

Two deliberate design notes from its message, both worth keeping: `std::once_flag` was
**not** used because it is neither copyable nor movable and would have deleted the class's
copy *and* move constructors, while every read path moves the table out of a `Result`; and
`fingerprint()` drops `noexcept` because its first call now allocates and can throw.

---

## Stop point

**`79b2fa6` is the only commit on this branch that no reviewer has seen.** The implementer
committed the code and was stopped while writing its report, so `task-4-report.md` does not
exist and its measurements survive only in the commit message.

Verified at the stop point: targeted `ListedOpra.*:StandardMonthlyClassifier.*` **24/24
PASS**; the working tree has **no modifications to any sprint source file**; the
pre-existing unrelated uncommitted work (Python bindings split, `atx-core` sqlite,
`atx-db/`, `atx-kb/`, surface-db docs) was never staged at any point.

**To resume:** review `0622d52..79b2fa6`, re-running the byte gate (all seven T1 section
hashes, both `final_nav` pins) and the full suite, since neither is on the record for this
commit. Then Wave E T5 (P3(b)), T3 (P2), T6, T7/T8 (P1), T9 (gate), and the final
whole-sprint A–E review.

---

## Tooling: six ways a byte gate has silently lied

The sprint's most transferable finding, and it kept growing. Every one was **observed**,
and each produced a verdict that was **wrong in a way that looked right**. All are now in
`SPRINT-CONSTRAINTS.md` and handed to every subagent.

| # | How it lied | Evidence |
|---|---|---|
| 1 | Bash **`diff` returns exit 0 on differing files** | measured on a purpose-built fixture |
| 2 | **PowerShell `>` on a native exe** re-encodes with BOM + CRLF | same dump: `E0C2ABB1…` FAIL vs `cbabca44…` PASS |
| 3 | **Unrecorded capture convention** — a CRLF-normalised pin reads as drift when re-checked raw | `AFD4C06E…` vs `39D47B8B…`, same artifact |
| 4 | **A helper named after a built-in alias** — `function H` → `Get-History` | six gates "passed" comparing `$null` to `$null` |
| 5 | **`printf` interpreting backslashes** in Windows paths | `C:\atx-data\…` → `C:tx-data\…` |
| 6 | **A hard-coded path constant vs a mixed-separator stored path** | see below |

**#6 was mine, and it was live.** I shipped a fixture-relocation helper to seven downstream
tasks with an all-backslash `PRISTINE` constant, while `build-corpus` records the path in
**mixed** form (`C:/…/run` + `\occ_ess\DATE.txt`). `String.Replace` matched nothing, the
script aborted before deleting `run.atxrun`, and a "relocated" copy could retain a stale
archive — which `write_run_archive` **merges**, so a carried-forward section would have
silently contaminated byte gates. Caught by Wave D T8's reviewer before it corrupted a
result. Now fixed to derive the prefix from the file itself, delete `run.atxrun` first and
unconditionally, report `REPLACED=n`, and fail loudly on zero replacements. Self-tested:
`REPLACED=49`, `build-schedule` EXIT 0 on the copy, pristine fixture's six pinned inputs
all MATCH.

**The unifying rule: a comparison that cannot be shown to fail has not been run.** Capture
bytes with bash `>`, hash with PowerShell `Get-FileHash`, print both computed and expected
values, record which convention a pin was taken under, and include a negative control that
demonstrably returns `False`.

### A seventh, different in kind: a net-zero check cannot prove a pairwise property

Wave E T1 offered a **coverage residual** (whole-command total minus the sum of phases) as
proof its phase split was disjoint. Its reviewer showed a residual is a **net** measure and
is therefore blind to a double-count cancelled by an equal gap. The partition was real — but
the proof had to be structural, walking the brackets to establish they are sequential and
lexically non-overlapping. Conclusion right, reasoning wrong.

### Page cache is not the same axis as CPU contention

The quiet-box measurement protocol was relaxed on user direction. That removed a
**CPU-contention** control. T1's reviewer then measured `definitions_parse` swinging
**24885 → 11506 ms (2.16×) between two consecutive runs of the same binary**, purely from OS
page-cache state on the 696 MB file — larger than any win Wave E is trying to demonstrate.
Under one-run measurement, a P3 result read against a table captured in another cache state
is indistinguishable from that swing: one could "prove" a 2× win by measuring cold-before
and warm-after.

Page cache is therefore explicitly **carved out** of the relaxation. Any task measuring
`definitions_parse`/`quote_join`/`reconcile` must take both sides itself, warm both with a
discarded run and say so, report the discarded cold number, and declare the claim
unsupported if cold-vs-warm exceeds ~1.5× the claimed win. One extra run per side; removes a
2.16× systematic error. T4 followed it.

---

## Systemic finding: the example binary has no test coverage

`atx-vol-tests` does not link `atxvol_spy_dispersion_backtest` — it is a standalone
`add_executable` (`CMakeLists.txt:321-322`). **Three separate tasks hit this wall**: Wave D
T5's arena staging, Wave D T8's new fail-closed gate, and Wave E T1's Step 3. T8's gate is
protected only by a reverted local mutation, so a future edit could delete it with the suite
fully green.

T8's reviewer disagreed with the claim that no cheap fix exists, pointing at
`CMakeLists.txt:353-372` where CTest already drives an example binary, plus a library-side
`require_full_observer_coverage(...)` predicate as a ~5-line partial closure. **Named item
for the final review.**

---

## Named follow-ups, out of this sprint's scope

1. **Python `RunSpec` binding omits `index_symbol`** while `read_run_spec` is bound and the
   bound `all_symbols`/`universe_at` take no index argument — a non-default index would
   silently give the C++ CLI and Python different universes. Inert today. Unlike the
   deliberate `step_observer` omission, this one was undocumented.
2. **`build-corpus` writes `occ_ess_inventory.tsv` only when `occ_ess_root` is set**, but
   `build-schedule` calls `verify_occ_ess_evidence` **unconditionally** — so a corpus
   legitimately built without an OCC ESS root can never run `build-schedule`.
3. **The parity report's "reconciliation dominates" note is now measurably wrong** —
   definitions parsing dominates both subcommands — and its lookup is dead code besides.
4. **No committed golden pins any driver's bytes** (carried from Wave C, still the largest
   residual risk).
5. **The `mark_divergence` positional row order has no in-tree consumer** — Python reads by
   column name then sorts by bps and takes the top 12. It is enforced solely by the
   out-of-process section sha256.

---

## Process notes

**Thirteen plan errors** have now been found and reported by implementers rather than worked
around — including three briefs pinning a fixture that no longer exists, pinned
floating-point literals unreachable under the `EXPECT_EQ` the same brief demanded, a byte
gate whose acceptance line was unreachable as written, and a task whose entire target had
already been deleted by an earlier wave.

**Four controller errors of mine were caught by subagents and are recorded as mine**: a
`--schedule` flag over-generalized from one corpus to a different fixture; a full-build
justification for pybind11 that could never have compiled a binding TU; citing
`snapshot_load` for a conclusion that needed `archive_load`; and the mixed-separator bug in
the relocation helper. Each was found by an implementer or reviewer pushing back rather than
complying.

**Reviewers repeatedly produced evidence stronger than the task asked for**: a per-section
binary payload comparison inside the `run.atxrun` container; a differential executable
running 20,000 row-sets / 76,630 `universe_at` pairs with a working negative control; and a
mutant-binary hash proving a gate was blind to the very defect it was thought to cover.

The full task-by-task record — every hash, every mutation, every ruling — is in
`.superpowers/sdd/backtest-wave-c/progress.md` (~1900 lines; it covers Waves C, D and E
despite the directory name).
