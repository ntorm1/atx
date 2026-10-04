# P9 code review: the research stack, architecture level (input to the P9 plan)

Read-only review of `C:/atx-wt/pool-2`, branch `feat/platform-v8-20260929`, head `d7c1c520`, 2026-10-02. Nothing was
edited (this file aside), built or run on data. Nothing dated 2024-01-01 or later was opened; no 2020-2023 return / IC /
NAV output was opened (only receipts, specs, manifests and the integration log's mechanics and timing rows).

P9 priorities, in order: Sharpe, gross return, capacity, significance; cross-cutting: a modular C++ research core so a
new idea is a spec entry, not a script (Ruling PM8-12, `docs/plans/2026-10-02-platform-v8-status-7.md:56-57`).

**Method and limits.** Read in full: the status, YARCH's audit and migration plan, the v8y research loop, the three
v8 code reviews (engine, signal, platform), the YINFRA / YCOMB / X-5 reviews, the integration log's last 300 lines plus
every phase-timing row of waves X-2..X-7, `run_bounded_research.py`, `research_tree.py`, the research window sources,
the build script, the CMake target lists and the cell / wave specs. Five stage deep-reads (fields, DSL/IC, admission +
composition, NAV + cost, orchestration) were dispatched but had not reported when this review was closed; where a
C++ line inside `strategy_ic_runner.cpp`, `strategy_nav_replay.cpp` or `strategy_target_replay.cpp` is cited, it is
cited **through** an earlier review (marked "via <review>:line") and must be re-checked by the lane that acts on it.

---

## 1. Pipeline map

### 1.1 Stages, owners, language, size, tests

| # | stage | owning files (lines) | lang | duplicated in the other language | tests |
|---|---|---|---|---|---|
| 1 | role (universe) | `atx-engine/tools/prepare_recent_research.py` (993), `scripts/research_roles.py` (284) | Py | membership rule computed in Py, re-validated by C++ `read_strategy_role` (audit `platform-core-audit.md:53`) | pytest `atx-engine/tools` |
| 2 | fields | `prepare_research_fields.py` (3,199) + 13 `research_fields_*.py` (7,127: price 784, sec 996, holdings 1,475, v8 609, v9 569, xdata 594, ohlc 291, gold 374, divevent 250, deals 347, ivshape 362, connected 488, mgr13f 535) + 5 `prepare_research_fields_{draft,xdata,ohlc,ydata,engine}.py` shims (409); C++ `atx/engine/research/fields/*.hpp` (12 headers) | Py, C++ (slice 1/3) | FINRA as-of x3, factor-break x2 (audit `:118-119`); vol_126, si_* ported to C++ with fixture identity (migration `:95`) | pytest `atx-engine/tools` (348, log "Python suites after the YINFRA merge"); gtest `atx-engine-research-fields-tests` (31/31, status-7 `:43`) |
| 3 | library / registry | `atx-impl/strategies/alphas/registry.json`, `generate_library.py` (543), `generate_from_spec.py` (slice 2); 11 legacy generators (5,069, class C) | Py + IC exe `--plan-only` (K1) | Py DSL parser in legacy generators only (audit `:125`) | pytest `atx-impl/strategies` (163) |
| 4 | DSL -> VM -> IC (u / w passes), candidate cache | `atx-engine/include/atx/engine/alpha/*` (VM header-only ~6,000 lines, via engine review `:223`), `atx-engine/src/factory/ic_screen.cpp`, `objective_ic.cpp`, `op_catalog.cpp`; `atx-impl/src/strategy_ic_runner.cpp` (884), `strategy_ic_signal_cache.cpp` (619), `strategy_ic_result_cache.cpp` (297), `strategy_ic_library.cpp` (306) | C++ | none on the hot path | `atx-impl-strategy-ic-tests` (`atx-impl/tests/CMakeLists.txt:95-112`; 159/159 at v8-16a), `atx-engine-alpha-tests` (771/771), `atx-engine-factory-tests` (392/392) |
| 5 | fit + admission gate | `atx-impl/tools/fit_composition_weights.py` (2,645; `screen_v3/v4`, NW t, greedy redundancy), `scripts/cycle_admission.py` (91); consumer `atx-impl/src/strategy_ic_admission.cpp` (942) | **Py decides**, C++ consumes | admission decision exists only in Py (audit `:127-129`) | pytest `atx-impl/tools` (612) |
| 6 | card | `atx-impl/tools/alpha_report_card.py` (1,215) | Py | partial DUP of engine `eval/cross_section_ic` (audit `:48`) | pytest |
| 7 | marginal IC (report only) | `atx-impl/src/strategy_marginal_ic.cpp` (748) | C++ | none | `strategy_marginal_ic_test.cpp` in ic-tests |
| 8 | composition (w pass) | `strategy_ic_composition.cpp` (488), `_shrink` (59), `_theme_resid` (199), `_theme_erc` (58), `_theme_tsmom` (123), `_two_speed` (50); engine `combine/group_*`; fit side `composition_rules.py` (390), `_resid` (365), `_theme_erc` (296), `_ic_shrink` (205), `_theme_tsmom` (228), `_two_speed` (51) | C++ apply, **Py fit** | 4 mirrors tied by Py-equals-C++ fixtures (audit `:120`); 2 more files added by YCOMB | ic-tests (`CMakeLists.txt:96-111`); `test_composition_*.py` |
| 9 | NAV / target replay, cost, leverage rules | `strategy_nav_replay.cpp` (3,653), `strategy_target_replay.cpp` (1,963), `strategy_nav_v7.cpp` (1,154), `strategy_cost_v2.cpp` (326), `strategy_vol_target.cpp` (43), `strategy_risk_target.cpp` (258) | C++ | `horizon_stats.py` mirrors aim-partial theta (audit `:69`) | `atx-impl-strategy-target-tests` (`CMakeLists.txt:114-135`; 321/321) |
| 10 | summ / monitor / statistics of record | `atx-impl/tools/nav_summ.py` (1,204), `backtest_integrity.py` (1,295), `dsr_total.py` (158), `book_monitor.py` (584) | **Py decides acceptance** | PSR / DSR / MinTRL / PBO / ONC exist in engine `eval/*` with **no test tying them** (audit `:121-122`) | pytest `atx-impl/tools` |
| 11 | ledger | `scripts/research_ledger.py` (344), `build-equity/trials.jsonl` | Py | 3 ledgers: engine `TrialRegistry`, `atx-impl/src/trial_ledger.cpp`, Py JSONL (engine review `:225`) | `test_research_ledger.py`, `test_trial_ledger_rules` |
| 12 | cycle driver | `scripts/research_cycle.py` (1,931: constants `:173-235`, spec validation `:291-596`, `Resolver` `:618`, `Cycle` `:689-1358`, gate `:1399`, compare `:1441-1538`, `run_cycle` `:1626`, lock `:1793`, `main` `:1852`) | Py | - | `scripts/tests/test_research_cycle.py` (fake tools) |
| 13 | wave driver | `scripts/research_wave.py` (84), `wave_*.py` (14 files, 2,483), `atx-engine/tools/stage_chain.py` (249), `wave_queue.py` (345), `wave_scoreboard.py` (213) | Py | - | `test_research_wave.py`, `test_wave_queue.py`, `test_wave_scoreboard.py`, `test_wave_speed.py`, `test_stage_chain.py` (v8y loop `:7-9`) |
| 14 | bounded runner | `scripts/run_bounded_research.py` (191), `research_tree.py` (83) | Py | - | `scripts/tests` |

Totals of the research Python: 37,997 non-test lines at YARCH's base (audit `:30`), 58.8% of it engine-bound logic
(`:27`). Since that base the field-builder Python grew by 2,050 lines (the five YDATA modules divevent, deals, ivshape,
connected, mgr13f and the `ydata` shim, merge `cd90e6c6`, log "Merges" row 3): **the directive was breached inside the
same sprint that issued it** (finding F-6).

### 1.2 Wall clock per wave (Debug build, bounded-runner receipts)

| phase (s) | X-2 (log 5617-5652) | X-3 (5720-5746) | X-4 (5812-5834) | X-7 (6171-6255) |
|---|---|---|---|---|
| u (screen / b) | 15.6 / 6.0 | 16.2 / 26.6 | 18.2 / 26.3 | 58.5 / 13.3 |
| fit (screen / b) | 3.1 / 0.8 | 4.4 / 0.8 | 5.0 / 0.8 | 14.9 / 2.2 |
| card (screen / b) | 19.2 / 14.5 | 22.0 / 15.0 | 22.6 / 14.6 | 60.0 / 35.3 |
| **marginal** (screen / b) | **141.1 / 134.6** | **175.5 / 167.0** | **157.7 / 143.8** | **331.5 / 253.1** |
| marginal failed attempts | 0.3 (refusal) | - | - | **0.5 + 259.9 (host free < 512 MiB, 6188) + 360.5 (time cap, 6221)** |
| ref NAV | 43.3 | skipped | skipped | 59.0 |
| w | 31.2 | 34.7 | 41.5 | 53.5 |
| nav (calibration + cell) | 41.5 | 41.6 + 45.5 | 45.9 | 55.3 + 57.4 |
| monitor / summ | 1.0 / 28.4 | 1.3 / 30.5 | 1.3 / 30.2 | 1.6 / 44.7 |
| sum of completed phases | ~460 | 581 | ~474 | **1,040** (+ 620 s lost to failed attempts) |
| marginal share | ~60% | 59% | ~64% | 56% of completed; **73% incl. failures** |

Other costs: fields v13 44.0 s (log 5528), v14 72.8 s (6107); build v8-16 601.6 s / 180 TUs, v8-16a 535.4 s / 135 TUs
(log section "Build v8-16 / v8-16a"); mine campaign 232.6 s at 4 workers, 2,211 MiB peak (log "Step 8, run").

Where the integrator's time goes (not wall clock of a process): 17 distinct hand scripts in the integration log
(`scratchpad/x2_add5.sh` 5605 ... `x7_add.py` 6127, `x7_wq099_variants.py` 6149, `gmspec.py` 5751 / 6242, `mech.py`
3790 / 6239, `bundle.sh` 5882, `fv13.sh` 5524, `fv14.py` 6095, `mine_fill.py` 6324, `probe12.py` 6340, `mine_go.py`
6357); one spec edit + relock + commit per retried phase because the runner refuses an existing output dir
(`run_bounded_research.py:120`; log 6188-6195 `-poolonly-b` -> `-poolonly-c`); one tests-only commit per new cell spec
(`scripts/tests/test_research_spec.py:60-91`; log "Tests after the cell (tests only, `a80e2c47`)", `a1d2ef74`); waiting for
a quiet host (log 6221-6233). The wave driver (v8y loop) removes the add / gmspec / mech / bundle scripts; it has not
yet run a real wave (Y-S is next, status-7 `:65`).

---

## 2. Findings, ranked by (impact on velocity or correctness) x (effort)

Lanes used below: **SPEED** (wall clock), **SPEC** (spec-driven experiments, rule registries), **CORE** (C++ migration
per PM8-12), **STAT** (statistics of record, ledger), **TEST** (closed-form and identity tests), **OPS** (runner, host).

### 2.1 Correctness

**F-1 Acceptance statistics are computed only by Python, with no tie to the engine's tested copies.** (H x M; STAT)
`atx-impl/tools/backtest_integrity.py` (1,295), `nav_summ.py` (1,204), `dsr_total.py` (158) compute PSR / DSR / MinTRL /
CSCV PBO / ONC; engine `eval/deflated_sharpe.hpp`, `min_trl.hpp`, `pbo.hpp`, `trial_clusters.hpp` implement the same and
"none" keeps them equal (audit `platform-core-audit.md:121-122`). Every verdict (dSR, ledger DSR .7309, PBO .0916, log
X-7 "Statistics of record") comes from the untied copy. Consequence: a numerical slip in the decision statistic is
invisible to every gtest. Fix: first a cross-language fixture test (Python value == engine value on a committed series,
tolerance 0 where reduction order allows, else 1e-12), then the `research/ledger` library (migration `:39,102`) with a
`atx-research-ledger` verb that `nav_summ.py` calls. Effort M (tie test S).

**F-2 Composition weights are one full-TRAIN estimate; no per-date (walk-forward) weights exist.** (H x L; CORE/SPEC)
The runner re-applies the recorded covariance and never re-estimates it (`review-x5-theme-erc.md:18-21`, citing
`strategy_ic_admission.cpp:599-679`); "a real-time ERC ... would need per-date weights, which the runner does not
support" (`:121`). X-5's gain is judged ~+.15 out of sample against +.35 in sample (`:3-7`, `:131`). Consequence: every
fitted rule (theme-erc, ic-shrink, inv-vol, theme-tsmom shares) is in-sample by construction; the significance priority
cannot be met by more fitted rules. Fix: a time-indexed weights file (`atx.composition-weights/v2`: rows keyed by
decision date, each fitted on decisions <= d-2), a C++ fit verb that produces it from the cached factor series, and a
fold driver (engine review O7 `code-review-v8-engine.md:239`, still open). Effort L.

**F-3 Admission is measured at a one-day horizon the book does not trade.** (M x S; STAT) The gate reads a one-day,
zero-lag factor return while the book holds theta .05 (mean lag ~19 sessions) (`code-review-v8-signal.md:30-31`, S-3
`:54`). Still stands: the wave's screen uses the same gate. Fix: print IC / HAC t at the traded horizon (21-session
overlapping, HAC) beside the gate row, then rule whether the gate moves. Effort S (print), M (rule).

**F-4 The memory admission is hand-probed per wave and the next screen is above the cap.** (H x M; CORE/SPEED)
Y-S screen library ~3,200 MiB estimated against the 2,560 IC cap (status-7 `:64`, `:89`); 70 members needed
2,024.2 MiB at 4 workers (log X-7 "IC-pass memory"). The in-exe formula is whole-panel `cells x (72 + 8 slots) + cells x
(8 capacity + 1)` (via engine review `:88-89`, `strategy_ic_runner.cpp:631-638`). Consequence: a larger book or a longer
history blocks the screen; each wave needs a probe and possibly a cap ruling. Fix now (S): split the screen into
candidate-only u passes against the parent's cached payloads (the parent's 58 members are cache hits; only new strings
need the VM arena). Fix later (L): date-blocked evaluation with carried streaming state (engine review O6 `:238`,
platform P-5 `code-review-v8-platform.md:54`).

**F-5 Free-memory kills and the time cap make outcomes depend on other sessions.** (M x S; OPS)
`run_bounded_research.py:158-160` kills on `free < min_free`; X-7's marginal was killed at 259.9 s by other sessions'
builds (log 6188) and hit the 360 s cap under load (6221). Runs are not wrong (no output is written), but each costs
260-360 s and a spec edit. Fix: (a) a launch-time admission that waits (bounded) for `free >= peak_estimate + floor`
and for no `cl.exe` / `clang-cl` (the mine launcher already does this by hand, log "launched by `scratchpad/mine_go.py`
only when free physical memory was >= 3,776 + 1,536 MiB"); (b) attempt sub-dirs `<output>/attempt-k/` so a retry needs no
spec edit. Effort S.

**F-6 Hash-pin gaps at the runner boundary.** (M x S; OPS) The runner binds only files <= 16 MiB
(`run_bounded_research.py:110-111`), so data payloads are pinned only if the spec's `locked` block names their manifest
(e.g. `x-theme-erc-gm.json` `locked.reference_*`); the YINFRA review found the readers bound only `summary.json`, not the
daily CSV or capacity curve (`review-yinfra.md:23`, MINOR 14) and that resumed stages did not re-check recorded digests
(`:11`, MAJOR 5). Status reports all 18 fixed (status-7 `:40`); not re-verified here. The exe is pinned by SHA-256
(`run_bounded_research.py:123`) but `vm_identity` carried no build type (via platform review `:192`), so Debug and
Release share candidate-cache entries by design; that is safe only while u / w stay byte-identical across builds (log
3344-3358, cited in v8y loop `:163`). Fix: add build type to the cache key or keep the identity test as a standing
canary per build tag. Effort S.

**F-7 Nondeterminism found by tests, not by design.** (L x S; TEST) A wave test passed or failed by `PYTHONHASHSEED`
(set-order path masking, log 6526, fix `3a146fe7`). Fix: run `scripts/tests` under two hash seeds in the suite command
(or `-p randomly` with fixed seeds). Effort S.

### 2.2 Modularity

**F-8 New rules enter as raw exe flags in a free-form spec block.** (H x M; SPEC) A cell spec is a diff over a parent
whose experiment content is `change.flags.<phase>.<--flag>` (e.g. `scripts/specs/v8/y-vol-target.json` `change.flags.nav`
`--vol-target`, `--risk-model`; `x-theme-erc-gm.json` `change.flags.fit --theme-erc`), with the registration in a prose
`description`. Only the marginal verb's flags are whitelisted (`research_cycle.py:211` `MARGINAL_SPEC_FLAGS`). Rule
compatibility is checked inside each exe, and the YCOMB review found two silent-composition bugs exactly there
(`review-ycomb.md:9-10`: `validate_nav_config` accepted per-name rate with two-speed, `strategy_nav_replay.cpp:336`;
cadence guard missing for two-speed, `:2282`). Consequence: every new rule = new flag + new validation clause + new
template + new test pin; composability is discovered by review. Fix: a typed rule registry in C++ (`research/composition`
and a `book/rules` table: name, version, parameter schema, phase, declared incompatibilities) printed by
`--list-rules --json`; the spec names `rules: [{name, params}]`; research_cycle validates against the printed table and
emits argv. Effort M.

**F-9 Research specs are pinned by file name in a unit test.** (M x S; TEST) `scripts/tests/test_research_spec.py:60-91`
lists 23 spec files in `NULL_PINS` / `ADD_ALPHA_COPIES` ("an add-alpha spec copied by hand", `:91`). Each new cell or gm
spec breaks the suite until a tests-only commit (log `a80e2c47`, `a1d2ef74`). Fix: derive the expected null-pin set
from the spec's own kind (template / child / add-alpha copy / gm) and test the rule, not the file list. Effort S.

**F-10 One impl library for two pipelines.** (M x S; SPEED) `atx-impl-core` holds 86 sources including 17 `stage_*`
files (16,405 lines) the research exes never call (`atx-impl/CMakeLists.txt:4`; exes `:179-196`). Still stands from
engine review (`:221`). Builds take 535-602 s for 6 targets (log "Build v8-16"). Fix: `atx-impl-strategy` (strategy_* and
config) vs `atx-impl-pipeline` (stage_*). Effort S.

**F-11 god-files on the hot path.** (M x M; CORE) `strategy_nav_replay.cpp` 3,653 lines (2,732 at the engine review,
`:200`: +34% in one sprint), `strategy_target_replay.cpp` 1,963 (1,197 then, `:210`: +64%), `research_cycle.py` 1,931,
`fit_composition_weights.py` 2,645, `prepare_research_fields.py` 3,199. Every Y rule landed in the two replay files
(`review-ycomb.md:9-27` cites both throughout). Fix: when F-8's registry lands, each NAV rule becomes one TU
(`book/rules/<name>.cpp`) behind the registry; the replay file keeps the loop. Effort M (per rule extraction S).

**F-12 Field registration by shim modules.** (M x S; CORE) Five `prepare_research_fields_<x>.py` shims (46-191 lines;
audit `:105-107` "registration shim (becomes a registry spec entry)") plus one Python module per data lane. This is the
versioned-copy pattern PM8-12 names. Fix: a JSON field registry both languages read (migration `:116-118`, "slice 3
work"); a new field is a registry row naming a C++ builder kind and parameters. Effort M.

**F-13 Theme lists in four places.** (L x S; SPEC) Adding `merger_arbitrage` touched the registry themes table,
`fit_composition_weights.V7_APPENDED_THEMES`, `strategy_ic_theme_resid.hpp:24-29` and pin literals in three test files
(log "Theme merger_arbitrage", `:243-245`, `:2811-2815`, `:212`), plus a pending YCOMB half-life row. The marginal verb
refuses more than 10 themes (log 6174), so every wave on X-5 needs ruling PM6-8 (i). Fix: theme table only in
`registry.json`, read by the fitter and passed to the exe; marginal theme cap lifted or derived. Effort S.

### 2.3 Performance (three largest costs per wave, and per cell)

| rank | cost per wave | evidence | what removes it | expected |
|---|---|---|---|---|
| 1 | marginal IC verb, 134-332 s per pass, twice, plus failures | table 1.2; open item "near its 360 s cap at 68-70 members" (log tail "Open items") | (a) `reuse_screen_marginal` (done, v8y `:136-140`); (b) `--candidates` subset: residualise only new ids (design v8y `:152-157`); (c) cache each member's residual series by (payload SHA, pool SHA, role, window); (d) Release IC exe | 2 passes -> 1, and that one ~10x smaller: ~300 s -> ~30 s |
| 2 | NAV replays 41-59 s each, 2-3 per wave (ref, calibration, cell) + 5 capacity books inside each | table 1.2; v8y `:129-130,158-162` | in-process `--calibrate-gross` (load once, replay at L, rescale once); skip capacity books in calibration; skip ref when fields are unchanged (done, v8y `:145-146`) | ~100 s -> ~55 s |
| 3 | Python card (14-60 s) + summ (28-45 s) + fit (1-15 s) | table 1.2 | card: cache the per-candidate invariant block by signal key (platform P-2 `:146-149`, still open); summ: statistics in C++ (F-1) | ~80 s -> ~25 s |

Per cell (one IC pass): Release is "59% / 82% faster in CPU" for u / w and byte-identical (v8y `:163-164`, log
3344-3358), yet the Release tree was deleted in cleanup (status-7 `:48`; `ls build-equity*` shows only `build-equity/`).
Platform P-4 (`code-review-v8-platform.md:53,175-200`) therefore still stands. NAV must stay Debug: Release differs by
one ULP in modeled cost columns (v8y `:162`). Parallel screens: the host has 16 GiB and 16 logical CPUs (measured
2026-10-02), other sessions took the free memory to 506 MiB (log 6188), so parallelism must be admitted by the runner
(F-5), not assumed; the IC exe's 4-worker cap (engine O2 `:234`) is secondary to memory.

### 2.4 Testability

**F-14 No end-to-end identity canary per build tag.** (M x M; TEST) The X-5 identity under each new build is a hand run
on real TRAIN data (log 6536-6537: fit / w / NAV by SHA, with path substitutions listed by hand). Platform P-12
(`:60`) proposed a tiny-world fixture; the strategy gtests are now ctest-visible under `atx_equity_strategy`
(`atx-impl/tests/CMakeLists.txt:147-155`, the ctest half of P-12 FIXED), but no fixture cycle runs the real exes end to
end. Fix: a committed synthetic role + fields + 3-member library whose u / fit / w / NAV SHA-256s are pinned; run by
`research-build.ps1` after every tag. Effort M.

**F-15 Closed-form tests missing where identity is proven only by hand.** (M x M; TEST) Gross matching (PM6-6) is a
hand calculation recorded in spec prose (`x-theme-erc-gm.json` description: "L = 1.1414 x .9861733264 / .9604183310");
the ERC fit's equality with the runner is a 1e-12 runtime check (`review-x5-theme-erc.md:18-21`); YCOMB's two-speed
closed form was a tautology at fixed L (`review-ycomb.md:24`). Fix: for each registered rule, one synthetic test with an
analytic answer (ERC on a 2x2 covariance; vol-target L_t on a step-variance series; gm on a linear-in-L book). Effort M.

### 2.5 Python-to-C++ migration order (YARCH's top 5, confirmed or amended)

| YARCH rank (`audit :186-192`) | verdict here | reason |
|---|---|---|
| 1 legacy generators -> spec | **confirm, finish now** | deletion only after root's `--check` (audit `:174-178`); 5,069 lines |
| 2 field builders -> C++ | **confirm, plus a freeze** | 2,050 more Python lines arrived after the audit (section 1.1); freeze new Python builders now; first port the registry (F-12) so new fields land as C++ |
| 3 composition mirrors | **amend: move to 4** | mirrors are tied by fixtures (audit `:120`); lower risk than #4; do it together with F-2 (per-date weights) so the fit verb is written once, time-indexed |
| 4 integrity statistics + ledger | **amend: move to 2** | decides every verdict with no tie test (F-1); smallest effort for the risk (engine eval exists) |
| 5 admission screens | **confirm, bundle with the marginal verb** | both live in `research/admission` (migration `:38`); the marginal is the #1 wall cost, so the admission library should own the residual cache of section 2.3 |

---

## 3. "One spec, no script": the gap list

| experiment | today | needs code or a hand plan | module that makes it a spec entry |
|---|---|---|---|
| new signal (DSL) | `candidates new` -> `pin` -> `emit` -> `wave run` (v8y `:24-75`) | **spec only**, if the DSL ops and fields exist; a missing op is C++ (YOPS opcodes 105-111, status-7 `:41`) | done (wave driver); op catalog stays code |
| replaced signal | `candidates new --kind replace --replaces ID` (v8y `:29`) | spec, plus a ruling for the marginal mode (PM6-8 (i), log 6174) | marginal theme cap from registry (F-13) |
| new field | Python builder module + shim + registry row + fields rebuild + manifest pin (status-7 `:38`; audit `:105-107`) | **code** (a Python module per field group) | C++ field registry + builder kinds (migration slices 3-5; F-12) |
| new combination rule | C++ rule TU + flag + fitter mirror (`composition_*.py`) + spec template + test pin (YCOMB: `strategy_ic_theme_tsmom.cpp` 123, `composition_theme_tsmom.py` 228, `y-theme-tsmom.json`, `test_research_spec.py:89`) | **code in 4-5 places** | composition rule registry + C++ fit verb (F-8, YARCH 3) |
| new NAV rule | C++ flag in the nav exe + validation clause + template (`y-vol-target.json`; `review-ycomb.md:9-10`) | **code**, composability found by review | book rule registry with declared incompatibilities (F-8) |
| leverage change | `nav.leverage` in `change.set` (`y-vol-target.json`); gm by the wave's match stage (v8y `:93`) | spec only; gm still two replays | in-process calibration (2.3 rank 2) |
| new cost model | C++ (`strategy_cost_v2.cpp` 326 lines) + flag | **code** | cost-model registry entry with parameters (part of F-8) |
| new universe | role build by `prepare_recent_research.py` (membership rule in Python, audit `:53`), fields rebuilt for the role, candidate cache cold | **hand plan** (log "Fields v14" and role sections) | `research/roles` (migration slice 9) + a `role:` spec block that chains role -> fields -> cycle |
| new horizon / holding period | `trade_fraction` (theta) flag; admission horizon fixed at one day (F-3) | holding: spec flag; admission horizon: **code** | gate horizon as a gate parameter in `research/admission` |
| walk-forward refit | not supported (F-2; engine O7 open) | **code (L)** | time-indexed weights + fold driver |

---

## 4. Hard constraints the plan must respect

| constraint | value | enforced by |
|---|---|---|
| bounded runner for every real-data process | <= 600 s, RSS <= 8,192 MiB, free floor 64-8,192 MiB, sampled every .25 s over the process tree | `scripts/run_bounded_research.py:92-94,145-165`; `global-constraints.md` ("Real data runs only through") |
| phase caps | default 180 s / 1,536 MiB; IC 300 / 2,560; w 300 / 3,072; marginal 360 -> 720 s (PM8-15); mine runner 600 / 8,192 with in-verb 7,680 | `research_cycle.py:220-235` (`CAP_KEYS`, `RUNNER_PHASE_RULES`); log 5617-5652; `scripts/specs/v8/waves/y-s.head.json` `marginal.seconds`; log "mine probe" |
| host memory | 16,068 MiB total, 16 logical CPUs, shared with other sessions | measured 2026-10-02; log 6188 (free fell to 506 MiB) |
| build serialisation | one integrator builds; target-scoped; memory admission free >= 1,000 MiB and commit >= 2,500 MiB; jobs 4 / 3 / 2 by free memory; single-use tags | `scripts/research-build.ps1:11-12,87-104`; `integrator-rules.md` rule 2; no file lock exists (a convention, not a mechanism) |
| clean source | no dirty path in `atx-core, atx-tsdb, atx-engine, atx-impl, scripts, CMake*` | `scripts/research_tree.py:20-21`; `run_bounded_research.py:116-119` |
| immutable outputs | the output dir must be new; receipt opened with `"x"` | `run_bounded_research.py:120,137,183` |
| seal | TRAIN [2020-01-01, 2024-01-01); nothing at or after 2024-01-01 | `atx-impl/strategies/research_window.json`; `atx-engine/include/atx/engine/data/research_window.hpp:10-17` pinned by `atx-engine/tests/data/data_research_window_test.cpp`; log scan `scripts/wave_seal.py:32-35` |
| registration before read | wave manifest committed before `wave run`; `expect.n_before` equals the ledger's N; candidates pinned | v8y loop `:88` (preflight), `y-s.head.json` `expect`; `global-constraints.md` ("pre-registered in v8-prereg.md before it runs") |
| identity discipline | flag absent = byte-identical outputs; Python deleted only after fixture identity (pytest + gtest) and root's TRAIN identity run; never edit an expected hash | `platform-core-migration.md:68-89`; `integrator-rules.md` "Tests"; `global-constraints.md` ("reproduce named accepted outputs byte for byte") |
| Debug NAV | NAV stays Debug (1 ULP in cost columns under Release) | v8y loop `:162` (log 3359-3365) |
| no network / no push / no atx-db | - | `global-constraints.md` |

---

## 5. Top 10 recommendations

| id | title | lane | effort | depends on |
|---|---|---|---|---|
| P9-R1 | Rebuild the Release IC exe (u, w, marginal) and adopt it after the flag-absent identity; build type in the cache key or a per-tag canary | SPEED | S | - |
| P9-R2 | Marginal verb: `--candidates` subset + residual cache by content hash, owned by `research/admission` | SPEED / CORE | M | R1 |
| P9-R3 | In-process gross calibration in the NAV exe (load once, replay at L and L'), capacity books off in calibration | SPEED | M | - |
| P9-R4 | Runner: waiting launch admission (free memory, no compiler) and attempt sub-dirs, so a retry needs no spec edit | OPS | S | - |
| P9-R5 | Tie then move the statistics of record (DSR / PSR / PBO / ONC / paired dSR) to engine `eval` via `research/ledger` | STAT / CORE | M (tie S) | - |
| P9-R6 | Typed rule registry (composition, NAV, cost) printed by the exes; specs name `rules:` instead of raw `change.flags` | SPEC | M | - |
| P9-R7 | Time-indexed composition weights + C++ fit verb + fold driver (walk-forward refit as a spec) | CORE / SPEC | L | R6 |
| P9-R8 | Field registry JSON + freeze of new Python builders; next fields as C++ builder kinds (slices 3-5) | CORE | M | - |
| P9-R9 | Screen memory: candidate-only u pass on cached parent payloads now; date-blocked evaluation later | CORE / SPEED | S now, L later | R1 |
| P9-R10 | Tests: spec rules instead of the file list (F-9), tiny-world end-to-end canary per build tag (F-14), split `atx-impl-core` (F-10) | TEST | S-M | - |

Not re-verified here and owned by the lane that picks each item up: every C++ line cited "via" an earlier review, and
the YINFRA / YCOMB fix claims (status-7 `:39-40`).
