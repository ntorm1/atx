# atx-vol backtest sprint — status 3

**Branch:** `sprint/atx-vol-backtest-waves-cde` @ `ca74f68` · **38 commits ahead of `main`** · `main` untouched at `2858cab`
**Date:** 2026-07-25 · **Supersedes:** `2026-07-25-atx-vol-backtest-sprint-status-2.md`

---

## 1. Where the sprint stands

| Wave | State |
|---|---|
| A, B, C | Closed in earlier sessions |
| **D** | **Closed.** T1–T8, gate PASS, whole-branch review Approved, evidence-channel fix `0622d52` |
| **E** | **6 of 8 tasks closed. T7 mid-fix-round. T8 held. T9 + final review not started.** |

### Wave E task ledger

| Task | Pass | State | Commits |
|---|---|---|---|
| T1 | measurement substrate + 7 goldens | **closed** — Approved | `d844f26`, `93e5c4d` |
| T2 | P5 shared SnapshotCache | **DROPPED on evidence** | — |
| T4 | P3(a)+(c) `trade_end` memo + lazy fingerprint | **closed** — 1 fix round | `79b2fa6`, `ba06428` |
| T6 | widen `run_identity_hash` | **closed** — 2 fix rounds | `f805655`, `ad3a6b5`, `0939ef7` |
| T5 | P3(b) single forward scan | **closed** — 1 fix round, 1 parked | `18ee3cb`, `2d5b74f` |
| T3 | P2 leg-key-filtered join | **closed** — 2 fix rounds | `cff8f8e`, `fd52934`, `2fc29fc` |
| **T7** | **P1 GO/NO-GO + ATXDEFS1 cache** | **fix round 1 PARTIAL — see §3** | `5948772`, `35e8e80`, `ca74f68` |
| T8 | P1 stale-input detection + CLI wiring | **HELD — deliberately not started** | — |
| T9 | wave gate | not started | — |

Suite at last full run (`5948772`): **2089 ran / 2042 passed / 44 skipped / 3 failed / 7 disabled.**
The 3 failures are the three sanctioned pre-existing REDs, confirmed from complete output every time, never a `tail`.

---

## 2. The result that matters: P1 is probably not worth shipping

This is the sprint's most consequential finding and it is **not yet acted on**.

T7 built the `ATXDEFS1` cache and its GO/NO-GO returned **GO at 88.0%** — `definitions_parse` over the two command wall totals, against a 15% floor, measured on current HEAD rather than inherited. That gate was sound. Then its review found the headline was measuring the wrong thing.

**Critical C1.** The seam `read_listed_definitions_cached` re-slurps the full 730 MB source **on the hit path**, and the measurement harness put that slurp in the numerator but not the denominator. Corrected, the reported **2.183×** is really **0.97–1.09×** on the implementer's own reps and **1.20–1.45×** on the reviewer's independent ones.

**And the re-slurp is not a bug.** A content-derived cache key *must* hash the source bytes, so a hit can never avoid **reading** 730 MB — only avoid **parsing** what it read. That is P1's premise, and it is much weaker than the plan assumed.

**Meanwhile the cheap alternative is bigger.** `read_listed_definitions_file` slurps via `istreambuf_iterator` at ~197 MB/s, costing 3.59–3.86 s of a 7.07–9.75 s parse. The reviewer verified both the decomposition and the byte-identical claim (the stream is already `ios::binary`, so `fread` loses nothing). **`fread` alone is ~1.5–1.9× on `definitions_parse` — one function, no new on-disk format, no stale-serve failure mode — larger than what the cache delivers at the seam.**

Reviewer's verdict, which I accepted: **P1 is not worth its risk as committed.** Do `fread` first, re-measure the seam end-to-end, and only then decide whether to wire it.

**This is why T8 is held and not merely unstarted.** T8's Step 5 measures cold-disabled / cold-with-cache / warm-with-cache — a table that is a direct function of the read path's cost, which is exactly what the pending changes move. Dispatching T8 first would produce a table to be thrown away.

The precedent exists: P5 was already dropped this wave on measurement, when `archive_load` turned out to be 0 ms / count 0 because Wave D had deleted its target. **P1 being dropped on evidence would be the plan working, not failing.** Six of the original seven perf passes have now been dropped or reordered by measurement.

---

## 3. Stop point — exactly what is and is not done

T7's fix round was dispatched in a **mandated order**, because the order was load-bearing. It completed the first two steps and stopped during the fifth.

**Landed and committed:**
- `35e8e80` — **I1, the compile-time ABI pin** (`listed_definitions_cache.hpp` +77, test +69). This had to come first: it is what makes a flagged-off fingerprint check safe.
- `ca74f68` — **the fingerprint check made opt-in** (default off in Release, forced on in tests; 3 files, +167/−26).

**Uncommitted in the working tree** — 5 files, **+190/−13**, the `fread` + dedupe work (I2) in progress:
```
atx-vol/include/atx/vol/detail/archive_util.hpp   +35
atx-vol/src/detail/archive_util.cpp               +83
atx-vol/src/listed_definitions_cache.cpp          +24/-…
atx-vol/src/listed_opra.cpp                       +18/-…
atx-vol/tests/listed_definitions_cache_test.cpp   +43
```
`fread` now appears in both `listed_opra.cpp` and `listed_definitions_cache.cpp`, and a shared helper is being introduced in `detail/archive_util`. **This work is unverified** — not built, not tested, not measured, and no `## Fix round 1` section exists in `task-7-report.md`.

**Not started at all:** the C1 harness rebuild (the honest seam measurement), the I3/I4 measurement corrections, I5 (the ~730 MB transient), and — most importantly — **I6, the cross-process hash-stability question.**

> **I6 is a possible BLOCKED and must be resolved before anything else in T7.** `hash.hpp:13-15` explicitly disclaims cross-process hash stability. If `hash_bytes` is not stable across processes, the cache misses **100% of the time, forever**, while still writing ~300 MB per run — and with no hit/miss logging, that is completely invisible. It must be determined **empirically** (compute the key in one process, again in a separate invocation, compare), not by reading the header.

**To resume:** finish or discard the uncommitted `fread`/dedupe work, then take T7's fix round from I6 → C1 → I3/I4 → I5, and let the resulting number decide T8. Then T9 (wave gate) and the final whole-sprint A–E review.

Working tree is otherwise clean for sprint files. The substantial unrelated uncommitted work (Python bindings split, `atx-core` sqlite, `atx-db/`, `atx-kb/`, docs) was **never staged at any point** — every commit used explicit paths.

---

## 4. What the wave actually delivered

Measured, corrected, and stated as intervals rather than point estimates — because two of the three original headlines did not survive audit.

| Pass | Effect on `definitions_parse` | Confidence |
|---|---|---|
| **T4** P3(a)+(c) | **order 5–30%, median ~15%** | sign 16/17 (p≈2.7e-4); 95% CI [6.5%, 30%] and [5.9%, 25%] |
| **T4** peak RSS | **−991 MB (2797.5 → 1806.4 MB)**, attributable to (c) alone | the cleanest result of the wave |
| **T5** P3(b) | pooled median **−23.7%**, build-schedule leg −28.4% | sign 14/16; run-backtest leg **directional only** |
| **T5** peak RSS | **−99.9 MB** | sign 16/16, matches the removed 104.7 MB line index arithmetically |
| **T3** P2 | `quote_join` **~2×**, `reconcile` **~12.7×** | sign 5/5 both |
| **T3** narrowed gates | 3 fail-closed gates preserved, proven by 2-way mutation | the deliverable, not the speedup |
| **T6** | merge-write cache key widened 2 → 5 folded inputs | stale carry-forward demonstrated then closed |

**Two headline corrections are load-bearing and must not be quietly dropped:**

1. **`79b2fa6`'s commit message contains numbers now known to be wrong.** It claims "≈33% off the dominant phase" with the win attributed mostly to (a). Re-measurement across 127 pooled runs disqualified it under the page-cache carve-out — the discarded warm-up vs settled median swung 10003 ms against a 9927 ms threshold — and a single variant re-run *against itself* spreads 51%/101%. The corrected figure is **order 5–30%, median ~15%**, and **(c) carries the larger share, not (a)**; (a) is below the noise floor outright (negative in 5/17 and 7/16 paired reps). History was not rewritten. Anyone reading only `git log` will carry the overturned claim forward.

2. **T3's own predicted mechanism failed, and the reason is measured.** Quotes emitted fell **741×** and the join loop **6.4×**, yet the phase moved only ~2× — because `load_opra_daterange` is ~880 ms and **78% of the filtered side**. The phase is now parquet-load bound. A prediction failing with a measured cause is a better outcome than a prediction succeeding by luck.

---

## 5. Byte-gate hazards — now seven, and two were mine

The sprint's governing rule: **a comparison that cannot be shown to fail has not been run.** Every byte gate this session printed computed *and* expected values and carried at least one negative control; several carried four.

| # | Hazard | How it was caught |
|---|---|---|
| 1 | `diff` reporting two files identical when their sha256 differs | Wave D T2, measured on a purpose-built case |
| 2 | A gate comparing a file against itself | earlier wave |
| 3 | PowerShell `>` re-encoding native stdout (UTF-8 BOM + CRLF) | produced one false FAIL; bash `>` is now mandatory |
| 4 | A hash gate structurally blind to the defect it appeared to cover | Wave D T8 — a mutant unfixed binary yields a byte-identical section hash |
| 5 | Merge-write carrying a stale section into a "fresh" comparison | **controller error, mine** — see §6 |
| 6 | An empty section passing a gate that cannot distinguish empty-because-correct from empty-because-broken | Wave D whole-branch review |
| **7** | **A pre-existing pipeline OUTPUT turning its own gate into a copy-integrity check** | **found twice** — see below |

**Hazard #7 bit twice.** Wave D T7 found `trade_schedule.tsv` matching its pin only because it was the un-regenerated copied file. The remedy never made it into the shared helper. Wave E T6's re-reviewer then showed the consequence had teeth: `trade_schedule.tsv` is now a folded input of `run_identity_hash`, and `build_schedule_command` writes it five lines *before* its own `write_run_archive`. Reorder those two statements and the identity is computed from the stale copied file — identity never moves, no section is dropped, all seven goldens still reproduce, **and no gtest executes `build_schedule_command` either.** The reorder passes both gates.

Fixed in `relocate-fixture.sh`, which now deletes `run.atxrun`, `trade_schedule.tsv` and `projected_schedule.tsv`. **Self-tested with `snap-ad3a6b5`:** five pipeline steps EXIT 0, both files regenerate, all seven goldens PASS with byte sizes matching T1's record exactly, negative control False. T5's reviewer independently confirmed the effect — `trade_schedule 8d4f223f3b83b8bf` now reproduces *from a copy where the file was deleted first*, so that golden is a real regeneration gate at last.

**A tooling alarm was investigated and rejected rather than propagated.** A re-reviewer reported the rtk proxy rewriting bash `grep` output and asked for the constraints file to be revised. Not reproduced: piped `git diff`, the same diff as a file, and `rtk proxy` all return 37 lines. The 8-vs-9 discrepancy was `grep` being case-sensitive against `Select-String`'s case-insensitive default, on `…BlindToDefinitionsContent`. The constraints now record the **case asymmetry** as the real hazard and explicitly refuse to record "grep is unreliable" — a false hazard would make every future agent distrust a working tool.

---

## 6. Controller errors this session

Recorded because the pattern matters more than any single instance: **four of six are me asserting a plausible mechanism before reading the code.**

| # | Error | Consequence |
|---|---|---|
| 4 | `relocate-fixture.sh` hard-coded an all-backslash path constant against a mixed-separator stored path, so the rewrite matched nothing and the script aborted *before* deleting `run.atxrun` | **Live.** A "relocated" copy could keep a stale archive, which `write_run_archive` merges. Caught by Wave D T8's reviewer before it corrupted a result |
| 5 | Told T4's reviewer the `trade_end` memo's correctness rests on rows being sorted, Critical if not airtight | The code never had that dependency — it is compare-then-refresh, exact under any input order. I specified the wrong failure mode and would have accepted a fix for a defect that never existed |
| 6 | Argued T7's runtime `fingerprint()` check was redundant, since CRC + `abi_fold` + content hash + the round-trip test cover everything | **Wrong, and the reviewer refuted it as instructed.** A new field added to `ListedContractDefinition` with the encoder not updated passes CRC, passes the content hash, and passes `CacheRoundTripReconstructsTableExactly` — because `sample_rows()` uses 9-value aggregate init, so a trailing field defaults on *both* sides. Only a production read catches it |

Error 6 is the fourth time this sprint a test was shown blind to the defect it appeared to cover — and the first time **I** was the one arguing from the blind test. The adopted ruling is the reviewer's, not mine: land the compile-time pin **first** (it makes that drift a build error for free), *then* gate the runtime check. Ordering, not redundancy.

Two dispatch errors from the previous session (a wrong `--schedule` claim, a wrong justification for a mandatory full build) are recorded in the ledger.

---

## 7. Process notes

**The measurement protocol earned its keep.** The page-cache carve-out was added after T1 showed a 2.16× swing between consecutive runs of the same binary. Its first real use **invalidated a headline** — T4's. Its second and third uses were rulings that a caveat was *not* triggered (T3's threshold arithmetic: 3267 ms vs an observed 2810 ms swing) and that a leg should be demoted to directional rather than withdrawn (T5's). A control that only ever confirms is not a control.

**Reviewers repeatedly did better than the briefs.** T3's implementer proved the brief's filter spec was not literally implementable and built a two-stage version whose coarse first stage is a *superset* of the exact-key set — widening gate coverage rather than narrowing it. It then corrected both the brief and its own reviewer on the fatal-exit count (seven, split 1 panel-wide / 6 narrowed, not 1/3), and found the wrong count had propagated to three further sites. T5 corrected the brief's residual-allocation figure from ~8.7M to a measured 6,545,634. T7 found the brief's "13.2 MB `definitions.tsv`" is really 730,526,177 bytes — the fixture **is** the production case, so the brief's instruction that I measure production separately was already satisfied.

**Three plan-mandated findings were adjudicated rather than deferred**, per the standing instruction not to stop for questions: T6's false transitive-coverage claim (exclusion stands, safety claim goes, gap documented and pinned by a test named to say *delete me, don't revert me* when the fold closes); T3's four-vs-seven contract error; and T7's fingerprint ordering.

**Minors normally roll to the final review. One exception was made** — T3 fix round 2 — because round 1's own renumbering left two "NARROWED GATE 2" labels on different gates. Newly introduced self-contradiction inside the exact file the task was about, for ten minutes of a cheap model.

**Agent stops with completed work are now routine (four this session).** Every time, the work was intact and uncommitted. Checking the tree before re-dispatching is what keeps that cheap; a controller assuming loss would have re-run whole rounds.

---

## 8. Open follow-ups

1. **Resolve I6 (cross-process hash stability) empirically — it can invalidate P1 outright.**
2. **Decide P1 on the post-`fread` number**, and drop it on evidence if the marginal value is small. Do not ship a stale-serve surface for a win `fread` already took.
3. **`79b2fa6`'s commit message is wrong** and history was not rewritten. The wave-gate commit and any release note must carry the corrected interval.
4. **Close `run_identity_hash`'s documented `definitions.tsv` gap** — T7 confirmed its key's `content_hash` is directly reusable at no extra I/O. Available only if P1 ships.
5. **`atx-vol-tests` does not link the example binary**, so several behaviours have no standing test — T5's arena staging, T8's fail-closed gate, T1's Step 3, and now `build_schedule_command`'s statement-order invariant. Wave D T8's reviewer disputed that no cheap fix exists, pointing at `CMakeLists.txt:353-372`.
6. **`ParseRejectsZeroNewlineInputWithoutThrowing` is a positive control, not a gate**, and cannot be made into one — the guard's own unreachability is why it exists. Label it in the source so nobody counts it.
7. **Only ~14 of ~40 parse assertions discriminate.** The nine row-0 drop cases and the `"\t"×7` case must not be credited as coverage.
8. **Python is still out of scope** and the parity report's "reconciliation dominates" note is now measurably wrong.
