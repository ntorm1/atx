# Pipeline SOTA Sprint — status as of 2026-07-25

Plan: `atx-vol/sprints/2026-07-21-atx-vol-pipeline-sota-sprint.md`
Integration trunk: **`feat/pipeline-m` @ `99d10c0`**. Local `main` has not been touched
and will not be — that decision is the user's.

Supersedes `2026-07-24-pipeline-sota-sprint-status.md`, which was written at an earlier
stop and is stale in its §1 and §3.

This is a checkpoint written at a deliberate stop, not a completion report. Nothing below
is claimed as finished unless it says so, and §4 lists what is still unverified.

Since the fork point `d4ade5b`: **171 commits, 31 merges, 204 files, +32032 / −1856.**

---

## 1. Where the work stands

Every workstream is implemented, independently reviewed, its findings closed, and merged.

| Workstream | Review outcome | Findings closed by | Merged |
|---|---|---|---|
| M keystone | approved | — | gated @ `5e2c31a` |
| A pricer | approved-with-minors (0C/0I) | FIX-1 | ✅ `e35cddf` |
| B fitting | approved-with-minors (0C/2I process) | — | ✅ `264b2fe` |
| C storage | approved-with-minors (0C/1I) | FIX-1 | ✅ `9390a15` |
| G greeks | needs-work (0C/3I) | FIX-1, FIX-2 | ✅ `96172e5` |
| Y python | **needs-work (2C/3I/5m)** | FIX-Y (6 commits) | ✅ `2f51797` |
| T corpus | needs-work → approve-with-follow-ups | FIX-T, FIX-T2 (4+4) | ✅ `51f95c0` |
| F backtest | approve-with-follow-ups ×2 (0C/3I/8m) | FIX-F (6, incl. a history rewrite) | ✅ `a91765f` |
| E analytics | **needs-work (1C/5I/12m)** | FIX-E (5 commits) | ✅ `6b6aa7e` |
| FIX-3 | executed revert-proof in commit | — | ✅ `f383511` |
| FIX-4 | executed revert-proof in commit | — | ✅ `f6a60d4` |
| FIX-5 | closes the final review | — | ✅ `99d10c0` |
| **Final whole-branch review** | **ship-with-follow-ups (0C/7I/11m)** | FIX-5 + re-pin | — |

Wave 1 merged with zero conflicts. Wave 2 merged in the order Y → T → F → E, which a
file-level overlap audit had shown was safe; only one merge conflicted, and it was the
sprint tracker.

## 2. What is genuinely verified

- **The golden re-pin is done, and it is stable.** Each artifact was produced **three times
  with the artifact deleted before each run**, and all three digests matched before anything
  was pinned. Six companion artifacts likewise.
  - 82-session `5e7ca065…` → `1b99512ad6c7049aa9e41bd9002ae933c502d9ce4b7d5d58e19b6efdbacad2bd`
  - 135-session `141173fd…` → `61da2ef78cf0d6de36baf0ac3bbe400eb13ae09cdea0f021a8224e184747f914`
- **Every moved column is attributed; nothing was pinned unexplained.** WS-E's E1 ×100 unit
  correction accounts for the entire move (residual ≤ 1.43e-13), confirmed by a **control run
  with E1 backed out** that reproduces the old pins to ≤ 8.02e-14 relative.
- **WS-T's byte gate reproduces at its merged tip**: 82/82 archives `cmp`- and
  sha256-identical against the `fit_workers=1` baseline, digest-of-digests equal across all
  three arms, admitted=902 / source_failed=407 both sides. Independently re-verified.
- **The suite is safe to run concurrently**, repo-wide. FIX-3's census found **124** fixture
  files across five projects — not the ~35 first estimated — and all four non-`atx-vol` suites
  also `remove_all` on entry, so the collision was destructive there too.
- **All 18 wave-2-era merges were reconstructed with `git merge-tree`** by the final reviewer:
  15 byte-identical to the auto-merge, 3 hand-resolutions correct, no test TU lost, and no
  second instance of the AVX2 merge-damage class.
- **The whole-repo serial gate at `6b6aa7e` was 5702 tests with 2 failures**, both proven
  pre-existing (§3).

## 3. Two long-standing unknowns, both settled

**The reconciliation `NotFound` is pre-existing, not a sprint regression.** Re-run against the
full 1.4 GB run directory copied to scratch — not the five-file subset used earlier —
`run-backtest` still writes `backtest.tsv` and then fails identically at `listed_opra.cpp:306`,
3/3 runs. It fails identically with `definitions-orig.tsv` substituted, and both attempts emit
a byte-identical `backtest.tsv`. The incomplete-copy hypothesis is refuted. A separate data
hygiene finding did fall out: the two definitions files are the **same size** (730,526,177 B)
with **different content**, so that input really was swapped at some point — but that is not
this failure's cause.

**The two red `RobustPipelineE2E` tests are pre-existing.** `atx-engine/tests/risk` asserts a
pure-noise panel admits zero alphas; it admits one, "noise seed 59 admitted a fluke". An A/B
against the fork point `d4ade5b` reproduces both failures identically — same seed, same
assertions. Not this sprint, and specifically **not** FIX-3's temp isolation, which was the
live hypothesis because the test opens its library under `tmpdir(...)`.

## 4. Open — what is NOT verified

1. **The final trunk `99d10c0` has no completed whole-repo gate.** What exists is the gate at
   `6b6aa7e` (5702 tests, 2 pre-existing failures) plus two branch gates above it — FIX-5's
   2185/0-failed and the re-pin's 2180/0-failed. Those cover the changes individually but not
   the merged result. **This must be run before the sprint can be called closed.**
   `atx-vol-tests` does **build clean** at this tip, verified after the stop: a first attempt
   at `-j 4` across all 19 test targets died in a clang crash with no code error, and a retry
   completed at exit 0 — consistent with the RAM ceiling in §6 rather than with any defect.
   What is missing is the test run, not the build.
2. ~~**The deferred benches were never run**~~ — **partly closed by the BENCH pass, see §9.**
   The utilization row, A7's solve-count half, A5's routing evidence and G4's policy half are
   now measured; **B7's baseline JSON, G4's A/B row and A5/A6's timing halves remain unmeasured**
   because the box was never quiet long enough. Every throughput number produced *before* the
   BENCH pass was measured on a contended box and **none of those is citable.**
3. **Carried open with reasons**, from FIX-5: WS-F minors M9, M10, and the third part of M11.
4. **FIX-3's coverage loss stands**: closing the poisoned-seed mint on both routes left the
   staging finite-sweep unreachable through the public API, because `FullGreekSeed`'s
   constructor is private. The final review adjudicated this **ship** — the replacement test
   asserts a stronger property — but it is a real loss and it is recorded in the test itself.
5. **Two reclaim mechanisms coexist** (WS-T's elastic budget, FIX-4's `boards_outstanding`).
   The reviewer verified they cannot compound, so this is tidiness, not correctness.

## 5. What the reviews caught that the tests did not

This is the argument for the review layer, and it is worth reading before deciding how much
of this trunk to trust.

- **A use-after-free four lines from a crash.** `PricerFitter::surface()` returned a raw
  pointer bound `reference_internal`, keeping the *fitter* alive but not the generation.
  `f.fit(c); s=f.surface(); f.fit(c); s.iv(...)` → access violation. The Python suite was
  green.
- **A fourth Ok-stamp nobody knew about.** Four commit bodies and a header comment in this
  sprint say "the three portfolio Ok-stamps". The adjoint-greeks route was a fourth, using the
  exact pre-FIX-1 predicate, fourteen lines above the correctly guarded one.
- **A claim I had been repeating as reassurance was false.** This document previously stated
  "`ForceAvx2` stays guarded by `have_avx2()`". True of `avx2_greeks_selected()`, false of
  `use_avx2()`, which ~15 AVX2 dispatch sites consult — and env-seeding, new this sprint, made
  it reachable from a production process. It survived because on an AVX2 host both answers
  coincide, so the bug is not observable through that function at all.
- **Three knobs that parsed, reported, and did nothing** on the production path — F5, F6, and
  `american_price_batch`'s `method`/`opts`, which returned the Andersen-Lake price for
  `method=BAW`, 0.45% off, with `status == STATUS_OK` on every lane.
- **A commit that did not compile.** `6142699` wrote a member that arrived only in its child,
  so bisect broke across it — and the GREEN counts in its body could not have come from that
  tree. Both old bodies quoted an identical figure although the child registers one more test
  than its parent, so at most one was ever right.
- **A guard that replicated production instead of calling it**, with the same blind spot that
  produced the finding it was closing.
- **Two vacuous tests caught by their own author** — FIX-T2 mutated `parallel_for.hpp` to check
  its five new gates and found two passed under their own mutation, because thread-identity
  assertions lose to scheduling latency.
- **A fix that over-corrected**: WS-E's own convergence gate traded a silent wrong answer for a
  refusal on usable steep-smile wings.

## 6. Process findings, which matter more than any single defect

**The sprint's evidence base was narrower than its commit messages implied.** Every workstream
and every reviewer gated `-L atx_vol`. **2186 of 5702 tests carry that label, so 62% of the
repo went unobserved for the sprint's entire duration.** The first whole-repo run was the final
gate. It came back clean modulo two pre-existing failures — but that was luck, not process.
Nothing would have caught an `atx-engine`, `atx-impl` or `atx-tsdb` regression at any point
before the end.

**The `atx-vol` Python suite had zero ctest registration and had never run in any gate.** Eight
test files; `grep` for pytest across every `CMakeLists.txt` returned nothing repo-wide. The
concrete casualty: the assertion hand-edited during the WS-F merge to track that merge's own
engine-default flip was executed by nothing. Registering it (FIX-5 I6) immediately caught a
stale binding — the tripwire working on its first run.

**Filtered gates are not sufficient, and this was proven twice.** WS-E's per-task `--gtest_filter`
gates were all green while three real defects sat underneath, including an unconverged fixed
point silently returning a wrong number. Every brief after that point mandated the full label.

**Two environment defects invalidated earlier measurements.** The host is RAM-bound (16 cores,
15.7 GB), so every `LLVM ERROR: out of memory` written off as a "transient shared-machine
artifact" was the box running out of memory — and that held even with the box otherwise idle.
And the test suite destroyed its own scratch directories under concurrency, which is why the
symptom was an `IoError` rather than an assertion. Every full-serial count taken while siblings
were running is superseded.

**A re-pin can bake in a stale binary.** `build-rel-avx2` was four days old and was rebuilt at
the tip before measuring. Had it not been, the pins would have encoded the wrong tree.

## 7. Scoreboard (plan §1)

| # | Exit criterion | Status |
|---|---|---|
| 1 | disp-hotpath reconciled; new golden SHAs pinned; serial gate ≤ known-failure set | **Met, pending §4.1** — pins re-pinned and 3×-stable; the known-failure set is now exactly the two pre-existing engine tests |
| 2 | Zero known silent-wrong-number paths, each closed by a named test | **Met** — and three further instances were found and closed beyond the plan's list |
| 3 | Archive round-trip contract true | **Met** |
| 4 | Backtest artifacts carry a mandatory friction regime end-to-end | **Met** |
| 5 | Canonical risk frame exposes bucketed vega + dP/dq | **Met** |
| 6 | Python fits a surface from quotes, batch-prices with NaN+status | **Met** — and now actually gated |
| 7 | Fitting bench re-pointed; corpus worker utilization ≥ 14/16 or a written explanation | **Met via the written-explanation clause (§9).** Re-point verified in code, not on assertion. Utilization **measured: 13.30/16** (5 reps, spread 13.17–13.56), target 14/16 **missed by 0.70 cores**, with the mechanism identified and evidenced. The clause is satisfied; the ≥14/16 threshold itself is not, and B7's baseline JSON is still outstanding |
| 8 | No stale contract docs | **Met** — 13 prose sites carrying a stale `final_nav` were found and corrected, two of them in shipped headers |

## 8. Next steps, in order

1. Finish the whole-repo serial gate on `99d10c0`, full label, `-j 1`. Expect the two
   pre-existing engine failures and nothing else.
2. ~~Quiet-window bench run for criterion 7 and the other deferred rows.~~ Done for the rows
   §9 lists as measured. Still owed, and **only** on a box with no other session on it:
   B7's `i7-1260p-clang18-avx2-fitting.json`, G4's A/B row, A5/A6's timing halves.
3. Decide M9, M10, M11-part-3.
4. Append the sprint report to the plan; delete `scratch-m2/` (~260 MB) and trim the remaining
   worktree build directories.
5. The merge to `main` is the user's call. This trunk is verified to be internally consistent,
   independently reviewed end to end, and free of regressions attributable to the sprint across
   the whole repo — with the one gap in §4.1 stated plainly.

## 9. BENCH pass — the deferred perf rows, measured (2026-07-25)

Binaries: `build-rel-avx2` **rebuilt from `0be71e7`** (the tip at the time), clang-cl 18,
`/arch:AVX2`, `-DATX_BUILD_BENCH=ON -DATX_VOL_COUNTERS=ON -DATX_VOL_PROFILE=ON`. The dir was
found four days stale (7/21) **and** configured `ATX_VOL_COUNTERS=OFF ATX_VOL_PROFILE=OFF
ATX_BUILD_BENCH=OFF` — i.e. the Google Benchmark targets did not exist in it at all — so every
number below comes from a fresh build, not the stale tree.

**Box conditions, stated because they decide citability.** The box was NOT exclusively
available: another Claude session ran repeated `ctest` sweeps (`atx-db-tests`,
`atx-core-tests`, `atx-vol-tests`, `atx-engine-*`) across the entire window, at times two
`ctest` processes at once. Host: 16 logical cores (4 P + SMT, 8 E), 15.7 GB RAM, 4.9–7.7 GB
free. Rows were therefore split by whether their quantity is contention-immune.

| row | quantity | measured | spread (n) | target | verdict |
|---|---|---|---|---|---|
| **T1** | mean busy cores over the fit fan-out, 11-name × 20 sessions = 220 boards | **13.30 / 16** | 13.17–13.56 (5) | ≥ 14/16 | **missed by 0.70**, mechanism below |
| **A7** | `sl_al_boundary_solves` per scenario grid | **896** | **0** (3) | count drop | **drop = 0** — warm-start never landed |
| **A5** | `cnt_american_avx_pack_dispatches` in the cache build | **0** | 0 (3×2) | ≥ 2× time | **not landed**; timing not run |
| **A6** | — | — | — | ≥ 8% | **not landed**; named gate row does not exist |
| **G4** | policy: which route does `Auto` take | **laned AVX2, live** | — | measured decision | policy half **closed**; A/B row not run |
| **B7** | re-point verified in code | **verified** | — | baseline JSON | re-point **confirmed**; **JSON not produced** |

### T1 — the headline, and why it falls short

The restructure works and the number proves it. Forcing the pre-B1 shape with
`ATX_VOL_CORPUS_DATE_BATCH=1` (one fan-out per date, 20 drains) reproduces **10.06 / 10.26 /
10.29** — the plan's cited ~9/16 starting point, on the same binary and box. Shipped batching
(`date_batch=8`, 4 fan-out calls) gives **13.30**. The spreads do not overlap: **+3.0 cores,
+30%, demonstrated.**

The residual 2.7 cores are **not pool drain**, and that is measured rather than assumed.
`ATX_VOL_CORPUS_DATE_BATCH=20` removes 90% of the remaining drains (20 → 2 fan-out calls,
`reclaimed` 159 → 16) and yields 13.27 / 13.50 / **13.91** — a median move of +0.20 inside a
0.64 spread. A spread that swallows its own effect has not demonstrated it, so that lever is
spent and the idle time lives *inside* the fan-out.

It is **per-board scaling loss**. At `fit_workers=8` the fan-out reaches 7.78 of a budget of 8
on the least-contended rep — **97% of budget** — against 13.17/16 = **82%** at full width. The
scheduler fills a 16-wide pool correctly; the work does not scale linearly to 16 threads on a
host whose upper 8 logical CPUs are SMT siblings and E-cores and cannot each return a full core.
Reaching 14/16 = 87.5% of budget is a per-board scaling problem, not a fan-out scheduling one.

Phase-split log (criterion 7's named artifact), shipped default:
```
PHASE ingest_s=0.24 build_s=33.08 fit_fanout_s=32.40 archive_write_s=0.19 checkpoint_s=0.09
      other_s=0.41 fanout_calls=4 boards=220 date_batch=8 reclaimed=31 inner_slots=5320
```
The fan-out is 98% of build time, so the occupancy figure is not being diluted by serial phases.

**On the two coexisting reclaim mechanisms (§4.5):** the utilization shortfall is not evidence
against them. `reclaimed` falls 159 → 32 → 16 as batching widens, i.e. the elastic budget fires
mostly in tails that batching has already removed — consistent with the reviewer's finding that
they cannot compound.

### What is NOT measured, and why that is the honest answer

**B7's baseline JSON is not committed.** The re-point itself is verified in code rather than on
WS-B's word: `fit/facade/hft_mark/spy_synth` calls `PricerFitter::fit`
(`fitting_throughput_bench.cpp:377`) and emits per-phase counters (`:402-405`); the
`essvi_calib_surface` row is renamed `fit/surface_cold_altdriver/*` and carries an explicit
"do NOT gate fit-perf work on it". Two caveats for whoever finishes it:
- The plan names phases *carry / de-Am / cache / calib / diag* via a `FitTimings` struct. **No
  such symbol exists.** The shipped struct is `FitPhaseTimings` (`pricer_fitter.hpp:348-358`)
  and splits by surface *purpose* (market-mark / risk build / risk validation / total). The
  per-phase requirement is met in form, not in the decomposition the plan names.
- A baseline recorded on a contended box would be *slow*, and `compare_baseline.py` only gates
  on ratio > 1.10 with CV ≤ 5%. A slow baseline therefore does not raise a false alarm — it
  **permanently weakens the gate**, hiding real regressions underneath it. That is why one was
  not committed rather than committed-with-a-caveat. Record it with the same
  `ATX_VOL_COUNTERS`/`ATX_VOL_PROFILE` setting used for comparison runs.

**A5 and A6 did not land**, so their targets are not evaluable — there is no change to be 2×
or 8% faster than. A5's evidence is machine-level, not a source reading:
`cnt_american_avx_pack_dispatches = 0` on both `correction/cache/build/{put,call}` across 3
reps, with `sl_al_boundary_solves = 96 = expected_boundary_solves` — the 4-lane pack path is
never entered and every (T, σ) node row still pays one cold scalar AL solve.
`boundary_interp.cpp` has not been touched since 2026-07-17, before this sprint. For A6,
`al_cheb_eval_t` (`american.cpp:526-548`) still computes `w[i]/dz` and accumulates `den` inside
the loop on every call from `:1010`, and the workspace (`american_boundary.hpp:85-88`) holds
only `geo_zc/geo_v/geo_weru/geo_wequ` — no `geo_bary`/`geo_den`, so the specified ~28 KB
workspace growth is absent. Separately, **A6's gate names a bench row that does not exist**: no
registered benchmark name in `atx-vol/bench/*.cpp` contains "sweep". The nearest real rows are
`american/ladder/*` (which does emit `n_sweeps`) and `american/price/*`. That gate cannot be
evaluated as written and needs re-specifying.

**G4's A/B row was not run**, but the policy question it exists to settle needed no stopwatch
and is closed — see the `[BENCH G4]` commit. `Auto` rides the laned AVX2 greeks
(`kShipAvx2Greeks = true`, and the surface caller flipped with it at
`priced_surface.cpp:1111-1134`). README.md:329-341 already stated this correctly; three other
sites did not and were corrected.
