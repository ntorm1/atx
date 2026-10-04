# Wave-1 whole-wave review (P9)

Reviewer: wave-1 whole-wave reviewer, 2026-10-03. Read-only. Range `6a68d7f9..b52a5de7` (8 lanes merged E1, T1, A1,
A2, S1, B1, D1, C1; 199 files, +43,078 / -9,783). All file:line references are at `b52a5de7`. Read from git objects
only; nothing built, run or committed. Per-lane reviews (`task-<LANE>-review.md`) were not repeated; this covers what
crosses lanes, plus the wave's identity, seal, trial, determinism, Windows and C++ safety seams.

Context read: `root-wave1-merge-brief.md`, `root-wave1-merge-report.md` (M1a-M1d, M1d resume), plan section 0.4
(G-P rows), 0.6 (global constraints), 2.1-2.3 (lane scopes, owned files, contracts), `.agents/cpp/agent.md` section 10.

## Verdict: READY_WITH_FIXES

No Critical finding. Nothing found that silently changes a flag-absent output beyond the ruled re-pins (C1 sqrt / NAV
receipt, A1/A2 manifest metadata, D1 plan-only print), opens sealed data, or appends a trial twice. Two Important
findings are cross-lane seams that no per-lane review could see; neither blocks merging wave-2 code, but I-1 should
be decided before AL-SIG / AL-COMB / PRE register candidate ids, and I-2 before the first P9 wave runs.

## Critical

None.

## Important

### I-1. Release IC cache paths cap candidate ids at ~33 characters; the registration contract admits 64 (S1 x E1 x T1 x AL lanes)

- `atx-engine/include/atx/engine/build_flavor.hpp:78-86` (S1): every non-legacy flavour (equity-rel) appends
  `_opt_md_ndebug_xs13.0.0`, so `cache_root` (`atx-impl/src/strategy_ic_signal_cache.cpp:142-145`) adds the
  directory `dslvm1_clang18.1_opt_md_ndebug_xs13.0.0/` (40 characters) that the Debug (legacy) root does not have.
- The deepest cache file is the IC-result entry: `<cache dir>/<identity>/<role sha 64>/fp_<16>/ic1_<16>/<id>.<dsl16>.json`
  (`strategy_ic_signal_cache.cpp:167-169, 179`, `strategy_ic_result_cache.cpp:134`, `strategy_ic_runner.cpp:446`).
  For the v8 spec cache in root (`C:/atx-wt/pool-2/build-equity/mega-candidate-cache-v8-lo3`) that is
  **226 + len(id)** characters in Release (186 + len(id) in Debug). Windows without long-path opt-in allows 259, so a
  Release run fails for any field candidate whose id is longer than **33** characters (32 in a two-digit pool tree;
  about 27 once S2's audit-exact token adds its 6 characters, progress.md S2 line). This matches M1d's measurement
  ("longest 244", i.e. a current longest id of 18, margin 16).
- `scripts/wave_manifest.py:105` admits candidate ids `[a-z][a-z0-9_]{0,63}` (64 characters). Nothing in
  `research_cycle.py`, the wave manifest or the IC runner checks the derived path length before compute.
- The T1 canary cannot catch it: tiny_world ids are at most 9 characters; the Release canary passes.
- Failure scenario: AL-SIG registers `russell_recon_reconstitution_drift` (34 characters), or a 28+ character id once
  audit-exact lands. Debug waves and the Debug identity runs pass; the first Release u / w pass of that wave computes the candidate, then fails publishing its
  IC-result entry (`IoError ... candidate cache partial output`, `strategy_ic_signal_cache.cpp:416`). That is a mid-wave
  stop after the compute, Release only. The retry needs an id or spec edit, which G-P1 forbids ("0 spec edits on a
  retry").
- Ruling M1d-RED-2 logs the margin as an SQL3 risk. This finding ties it to the K-P9-11 registration contract that
  wave-2 lanes write *now*, before SQL3 exists.
- Fix (smallest): cap `CAND_ID_RE` at a length derived from the deepest Release path, e.g. `{0,23}` with headroom for
  audit-exact and two-digit pools. Or add a plan-time refusal in research_cycle / the IC runner's `cache_plan` when
  the deepest planned cache path is over 259 characters. Either goes in before AL-SIG / AL-COMB / PRE ids are pinned.
- A smaller margin of the same kind: the Release canary root lives directly under `%TEMP%`
  (`scripts/tests/test_cycle_e2e.py:212-217, 449-452`). On this host its deepest field-candidate IC-result path is
  about 242 characters, which leaves about 17 characters for a longer `%TEMP%` (another user name or host).

### I-2. After C1, a P9 cell is judged against a parent NAV built by the pre-sqrt exe; no code points at the P9-B0 re-based reference (C1 x E1 x DEC-20)

- C1 changes NAV bytes by design. `atx-engine/src/book/replay_cost.cpp:26, 101` and
  `atx-impl/src/strategy_cost_v2.cpp:34` replace pow with sqrt. P9-B0 re-ran each parent:
  - Y-F0 S2 daily is now `e7eb5720` (was `73b69bcc`);
  - X-5 S2 daily is now `75a54774` (was `529062d6`).

  The re-based references exist only as `build-equity/p9-b0-*` directories and as log lines. No spec, template or
  manifest names them.
- Add-alpha cells: the cell spec's `ref-s2-daily` compare (mode `file`, `scripts/research_cycle.py:1709-1713`)
  compares the ref re-run under the new exe with the parent's pinned pre-C1 daily. For example,
  `scripts/specs/v8/lib-v8ysb-gm.json` `inputs.reference_daily` pins `529062d6`. The first P9 add-alpha wave on any
  v8 parent therefore HARD-STOPs with `IDENTITY MISMATCH` (loud, no trial), and the retry needs a spec re-pin.
- Rule cells (NAV-only templates, the core Y/P9 cell shape per ruling E1-REUSE-a2) have no ref phase, and no check
  catches the old-exe parent:
  - `scripts/wave_stage_library.py:141-145` requires `reference_nav` to be the parent spec's NAV directory, which the
    old exe wrote.
  - `scripts/wave_stage_record.py:68-80` (`exe_problem`) records a NAV-exe change and does not refuse it when
    `ref == "none"`.
  - The judge's bundle and paired dSR (`wave_stage_record.py:154`) and the gross-match calibration
    (`scripts/wave_stage_cell.py:196`) then compare a new-build cell with an old-build parent.
- The numeric effect is tiny: P9-B0 found cost columns in 1-8 rows of 1,006, 8 CSVs. But it breaks DEC-20 ("paired
  tests use the re-based reference"), and verify's exe guard is blind to it.
- Fix before the first P9 wave: a ruled re-pin of the lineage parent. Point `reference_cell` / `reference_daily` and
  the parent's `nav.output` at the P9-B0 directories, or give the parent a re-based spec. Alternatively, `exe_problem`
  can refuse a cell whose NAV exe differs from the parent NAV's receipt exe when `ref == "none"`, unless the parent's
  NAV came from a P9-B0 re-base. E2 / PRE own the wiring. After every later wave that moves NAV bytes (C2, C3, COV),
  the same re-pin is needed.

## Minor

- **M-1 (C1, determinism coverage).** C1 opens `--book-workers > 1` and construction grids to every v7 rule except
  spo (`atx-impl/src/strategy_nav_replay.cpp:2422, 3443`). This newly admits aim-partial-v6 per-book state,
  `--cost-v2`, `--capacity-curve` with per-multiple lockstep groups, and `--adv-hold-q` on a pool.
  - By inspection the paths are race-free. Each book plans through its own `BookState` / `BookLeverage`. The decision
    liquidity and the shared construction (`decide_construction`, `form_desired_target` -> `v7::nav_multiple`, which
    reads the thread_local extension) are formed on the calling thread before `par::for_each_lane`.
  - Parity at 1 vs 4 workers is tested only under vol-target-v1 (`NavBookRule.*`). The engine golden
    `0x889874a3b9b29c55` (SignalFitness / NSGA) does not cover NAV at all.
  - Add one 1-vs-4 byte test with `--rule aim-partial-v6 --cost-v2 --capacity-curve --adv-hold-q` before any spec
    sets `--book-workers`.
- **M-2 (D1 x AL lanes, theme table).** The fitter reads the registry's themes at import
  (`atx-impl/tools/fit_composition_weights.py:456-468`). The IC runner's flag-absent table is the 13-theme built-in
  prefix (`atx-impl/src/strategy_ic_rules.hpp:43`), and `research_cycle.py` never passes `--theme-registry`. A theme
  appended to `registry.json` in wave 2 (AL-COMB / AL-SIG) is admitted and weighted by the fit, then refused by the w
  pass ("outside the registered theme order"). The failure is loud, not silent. Carry for E2: pass
  `--theme-registry` + SHA in the w step (or wherever the registry is longer than the built-in prefix).
- **M-3 (E1, reproducibility of "content" digests).** With `driver.timings` on, `wave_stages.timed` writes each stage's
  processes, with their `seconds`, into the stage receipt. Under `host_budget_mib` the judge runs three threads
  (`scripts/wave_stage_record.py:162`) and appends rows in completion order (`scripts/wave_context.py:176`). Console
  file numbers also follow completion order (`wave_context.py:167`). `stage_chain.content_sha256`
  (`atx-engine/tools/stage_chain.py:93`) drops only the top-level `started_utc` / `seconds`. So
  `receipt_digest: "content"` is not reproducible when `timings` is on, which defeats OR section 3's purpose for that
  combination. Fix: sort `fold_processes` output and exclude process seconds from the content digest, or document
  the exclusion.
- **M-4 (A2 / C1, configure-time provenance).** `engine_git_sha` and the fields `git_sha`
  (`atx-engine/CMakeLists.txt:291-319`) are taken at configure time, from `git status --porcelain`, which counts
  untracked files.
  - Without a reconfigure they name a stale commit. M1c saw `1239a5ff...-dirty` at a later head.
  - Root trees are almost always "-dirty".
  - Fields reuse requires `git_sha` equality (`atx-engine/src/research/fields/reuse.cpp:140-186`), and the SHA is baked
    into the exe. Every reconfigure, even one with no fields code change, therefore invalidates every engine-field
    reuse.

  This is provenance and speed only, not correctness. Note it for SQL2's catalog (it will index these keys).
- **M-5 (process).** The `atx-impl/CMakeLists.txt` comment for D1's block says the /O2 flag set keeps "the inline
  variables of strategy_ic_detail.hpp" one definition. Those variables depend only on ISA macros (`vm_fp_flavor`,
  `vm_compiler`, `strategy_ic_detail.hpp:78-97`), not on /O2. Four TUs outside the /O2 set already include the
  header: `strategy_marginal_ic.cpp`, `strategy_marginal_pair_cache.cpp` (S1), `strategy_mine_pool.cpp` and
  `strategy_research_role.cpp`. So there is no ODR hazard, but the comment overstates the rule; correct it on the next
  touch so a later lane does not copy a non-rule.

## Cross-lane seams checked and found sound

- **CMake list tails.** The three-way merges kept every block.
  - `atx-impl/CMakeLists.txt` has 6 `if(` / 6 `endif()`.
  - `atx-engine-research-admission-tests` is registered once, although both deferred functions list it: the
    `CTEST_DISCOVERED_TEST_COUNTER` guard holds, and M1d counted 106 = 46 + 16 + 44.
  - No orphan target.
- **`load_pinned_f64` (S1's TU, used by B1).** It is declared in D1's `strategy_ic_detail.hpp:327` with a stable
  signature (`verify = true` default unchanged). Its other callers (`strategy_ic_library.cpp:271`,
  `strategy_mine_pool.cpp:66`) are unaffected.
- **Test ODR in glob-built `atx-impl-tests`.** Every new or changed test TU keeps its helper types (`Directory`,
  `Role`, `Lcg`, `Fixture`, ...) inside anonymous namespaces, so there is no silent duplicate-type ODR violation.
- **Thread-local v7 extension vs book workers.** Every `active_state` read in the replay is on the calling thread:
  configure, make_book_state, capacity_book, nav_multiple via the shared construction, observe, run_records. The
  `RiskStore::read` that pooled `BookScaler`s share is const and opens per call.
- **Record order.** `merged()` (session, then book) reproduces the pre-C1 append order for every case a single run
  could produce. Y-1 `vol_target.csv` is byte-identical in P9-B0.
- **CRLF / eol** (`core.autocrlf=true` from the system gitconfig on this host).
  - Byte-pinned files carry `-text` or `eol=lf`: `field_registry.json`, `eval_tie/*`, `strategies/alphas/registry.json`.
  - The fixtures checked out CRLF (`composition_rules_list.json`, `factors_verb_v1.json`, `screen_v4_v1.json`,
    `tiny.json`) are only parsed, never hashed.
  - Source pins normalise CRLF to LF.
  - Every new C++ writer opens in binary: fields `wbx`, factors, pair cache, NAV.
  - New Python writers pass `newline="\n"` or write bytes.
- **Seal / PIT.**
  - The B1 factors verb goes through `ResearchRole::load` (seal), and its admission CLI refuses sealed sessions.
  - A2 drops source rows at `kSealBeginDate` and refuses a sealed role.
  - Reuse refuses a different seal.
  - C1's leverage rule reads store row d and the book's own weights (truncation test).
  - No new reader opens data on or after 2024-01-01.
- **Trial accounting.**
  - summ (the ledger phase) is never auto-attempted (`resolve_attempts` skips check / summ) and never paired.
  - `ledger_append` dedups by `trial_id` under its lock.
  - The B1 / S1 verbs are not wired into waves.
  - Root's M1b-M1d runs were re-runs of ledgered cells (0 trials, ledger `27e40f9f` unchanged).
- **Unordered containers / hash seeds.** No `unordered_*`, `std::hash` or RNG feeds an output in new C++. The only RNG
  is the pre-existing partial-file nonce. Python sets feed only sorted output or validation.

## Not covered (explicitly)

- **Line-by-line logic** of:
  - S1 `strategy_marginal_ic.cpp` (read for threading, I/O and identity only);
  - B1 `research/admission/{screen,admission_cli,admission_csv}.cpp` (numeric parity with the fitter);
  - A2 `registry.cpp` / `field_registry.cpp` parsing;
  - D1 `strategy_ic_rules.cpp` / `strategy_ic_composition.cpp` table semantics;
  - C1 `strategy_nav_replay.cpp` / `strategy_nav_v7.cpp` outside the diff hunks;
  - A1's Python builders (`prepare_research_fields*.py`, `research_fields_holdings.py`);
  - E1's `wave_stage_cell.py`, `wave_stage_library.py` and `cycle_resume` binding internals.
- **Tests.** I read only the namespace-scope scan and C1's parity tests. Test coverage beyond M-1 was not assessed.
- **Generated and fixture content.** Not read: `field_registry.json`, `screen_v4_v1.json`, `eval_tie/*`,
  `composition_rules_list.json`, tiny-world goldens, the eval-tie generator, docs and sprint documents.
- **Not re-verified.** Any numeric claim (1e-12 admission parity, Debug = Release digests, P9-B0 lists) is taken from
  the merge report.
- **Presets.** Non-equity presets (full `dev` build, `hygiene` include-cleanliness, non-`ATX_EQUITY_ONLY` CMake paths)
  were not checked.
- **Concurrent work.** The two test fixes another agent is making in this tree after `b52a5de7` (M1d new reds 1 and 2)
  were not reviewed.
