# P9 code review — orchestration, runner, ledger, statistics (sub-reviewer, pool-2 @ d7c1c520, read-only)

Supplement to `2026-10-02-p9-code-review.md`. **Biggest risk: the committed wave `y-s` will hard-stop in its screen
stage** — manifest marginal cap 720 s (y-s.json:41; y-s.head.json:17) vs runner maximum 600 s
(run_bounded_research.py:92-94). Validators accept any positive value (wave_manifest.py:225-229;
research_cycle.py:369-376; wave_steps.py:181-183); test_wave_speed.py:80-86 asserts 720 against fakes. Runner exits 2 with
no receipt, screen stops after u/fit/card (research_cycle.py:1683-1687); needs a new manifest commit. Fix: one RUNNER_MAX
constant in research_tree checked at manifest and spec load; then a ruling to raise the runner cap or the C++
`--candidates` marginal subset.

## 1. Pipeline map (wave driver)

Manifest `scripts/specs/v8/waves/<w>.json` (wave_manifest.py:267-278); nine stages assembled wave_stages.py:46-54, run
serially by stage_chain.py:196-238 under a per-state-dir O_EXCL lock (:180-194); `w.run` = subprocess.run with no
timeout (wave_context.py:29-30, 158-165).

| # | stage | runs | notes |
|---|---|---|---|
| 1 | preflight | git only | pins manifest, parent digest, fields, template (wave_stage_preflight.py:23-31); checks :136-184 |
| 2 | register | one `add-alpha ... --save-plan --root R` per candidate (wave_steps.py:54-73) | add-alpha runs the IC exe `--plan-only` with 300 s timeout, no RSS cap (generate_library.py:481-500); `lock` dry; commit of exactly its paths (wave_context.py:220-235) |
| 3 | screen | `research_cycle.py run lib-NAME.json --screen` | u (IC exe), fit, card, marginal all via run_bounded_research; gate in-process appends admission lines (research_cycle.py:1722-1731; cycle_admission.py:83-91) |
| 4 | spec | add-alpha into NAME+b, or rule cell + `lock --write`, commit | wave_stage_library.py:141-187 |
| 5 | run | optional b-library `--screen`, then `run CELL --stop-after nav`; cycle_binding.json | research_cycle.py:972-990, 1707-1708 |
| 6 | match | mechanics reader under runner 180 s / 1,536 MiB / 512 MiB free (wave_steps.py:37, 80-97); if gross off: `-gm` spec, lock, commit, NAV, second reader | wave_stage_cell.py:92-130 |
| 7 | verify | mechanics rule, C-13 binding, seal scan | wave_stage_record.py:25-57 |
| 8 | judge | `run CELL` (monitor; summ = nav_summ `--protocol v8 --ledger --dsr-ledger --json`, research_cycle.py:1079-1114), bundle, book reader | wave_stage_record.py:94-115 |
| 9 | record | ledger re-read, seal re-scan, wave-result.json, wave-log.md, queue commit, copy_to | wave_stage_record.py:134-179 |

Runner caps in v8 specs: default 180 s / 1,536 MiB / 512 MiB free; u 300 s / 2,560 MiB; w 300 s / 3,072 MiB; card
300 s / 2,560 MiB; marginal 360 s (lib-v8x7.json:10-31). Receipts: stage `<out_dir>/receipts/NN-stage.json`
(stage_chain.py:95-96, 240-249); runner start.json / receipt.json incl. executable_sha256 and binds
(run_bounded_research.py:121-132, 177-185); C-13 binding (cycle_resume.py:60-69); cycle_verdict.json (cycle_verdict.py:121-125).
wave-result.json wave_result.py:28-66 (+ receipt digests :93-97); ledger line written by nav_summ in judge; scoreboard
reads results + ledger only (wave_scoreboard.py:122-132).

**research_cycle.py is a god-file (1,931 lines)**: 1-150 usage; 173-235 constants (INPUT_KEYS :186-192 incl. SEC/holdings;
BUILDS with absolute C:/atx-cache DLL paths :223-227); 256-615 spec load/validation + ruling validators (E-27b :403-423,
C-2 :484-503, marginal :506-535, compare :562-593); 618-659 Resolver; 663-687 Step + `--help` probe; 689-1355 Cycle
(naming 726-771, pins 785-806, caps 832-864, attempt state machine 866-917, 12 phase builders 920-1355); 1359-1437 fields
check/gate; 1441-1536 identity compare; 1539-1587 plan/status; 1590-1620 git; 1626-1719 run loop; 1722-1790 ledger hooks;
1793-1835 lock; 1839-1931 CLI, 8 verbs (:1854-1877).

Generic: stage_chain.py, run_bounded_research.py. Hard-coded: phase ladder (research_cycle.py:174), INPUT_KEYS (new data
input = code change in fields_step :1210-1262), two wave kinds (wave_manifest.py:188-194), named rules (wave_rules.py:55,
77, 107, 132-143), b-library / marginal-reuse logic (wave_stage_library.py:161-187).

Spec -> argv: only `marginal.flags` whitelisted (MARGINAL_SPEC_FLAGS research_cycle.py:211, 524-532); `w_flags` may not
touch W_BUILT (:209-210, 440-450); `ic/fit/nav/card/monitor.flags` and `summ.extra` verbatim (:953-956, 1342-1345,
1167-1175, 1288, 1302, 1095); effective_exes (:543-552); templates edit flags via change.flags (research_spec.py:201-204).

Tests: stage_chain test_stage_chain.py (154); wave tests test_research_wave.py (316), test_wave_review.py (344),
test_wave_review_minor.py (179), test_wave_speed.py (107), test_wave_queue.py (138), test_wave_scoreboard.py (87) — all
fakes (research_wave.py:12-13); research_cycle test_research_cycle.py (2,010), _roles (475), _label_role (84),
test_cycle_resume.py (167), test_cycle_scoring.py (216), test_cycle_e2e.py (267; live part skipped without ATX_EQUITY_BIN
:44, :182); test_research_spec.py (1,389); test_research_ledger.py (405); run_bounded_research: NO dedicated test (one
real `--no-git` probe test_research_cycle.py:1363-1375; limits only via fakes :197, :363); add-alpha no dedicated file.

## 2. Hard constraints — where enforced

- Memory: runner samples tree RSS every 0.25 s, kills on RSS > cap or system free < floor (run_bounded_research.py:145-165);
  launch floor :138-140; ranges 32-8,192 MiB RSS / 64-8,192 free (:92-94); defaults 1,024 / 256 (:80-81). Nothing caps
  memory across processes; sampling can overshoot.
- Build serialisation: none in orchestration; atx-build.ps1 `-Jobs 1` per call (:56, 137-152), no cross-process lock; a
  rebuild mid-wave is undetected (finding 2).
- Timeouts: runner max 600 s (:92); wave executor none (wave_context.py:29-30); `--help` probe 30 s (research_cycle.py:681);
  exe plan 300 s (generate_library.py:492).
- Seal: research_window.hpp:11-19 / research_window.py:88-121 single source; preflight checks `seal.exclusive_end` and
  path names (wave_stage_preflight.py:115-133, 159-162); readers via nav_summ.load_daily (wave_readers.py:51); log scan
  iso/compact/year/quarter, seeds allow-listed, guard refusals classified (wave_seal.py:32-37, 94-121) at verify and record;
  `--seal-allow` is a CLI flag carried in the verify receipt (wave_seal.py:87-91; research_wave.py:47-57).
- Identity: compare steps from add-alpha (research_add_alpha.py:197-210); ref identity SKIPPED when fields equal the
  parent's (research_cycle.py:1015-1019); rule cells drop every identity section (research_spec.py:53, 177-180); identity
  re-run caught at record (wave_stage_record.py:142-148). "Root only" is a docstring; `candidates pin --by` is free text.
- Registration before read: manifest must be committed and clean (wave_stage_preflight.py:138-141); add-alpha refuses a
  library with outputs (research_add_alpha.py:265-271); rule cell refuses an existing NAV (wave_stage_library.py:148-152);
  prereg "Ruling:" placeholder never checked (research_add_alpha.py:255); N must equal expect.n_before and advance by one.
- Exe pins: runner records executable_sha256 (run_bounded_research.py:123); specs pin exes by path only
  (lib-v8x7.json:33-36); nothing compares exe SHAs parent vs cell; no build provenance (C4 open).

## 3. Correctness gaps

- Exe identity not pinned/compared (finding 2); wave drops executable_sha256 from phase rows (wave_stage_util.py:58-68);
  `--help` probing changes argv and silently skips the marginal verb (research_cycle.py:676-687, 950-952, 1046-1048);
  `tool()` falls back to an absolute path in another repo whose HEAD is not recorded (wave_steps.py:41-46); interpreter
  pinned by path only; numpy version unrecorded; spec_digest resolves against research_tree.REPO not the wave root
  (wave_context.py:100-101; cycle_resume.py:52).
- Nondeterminism: stage receipts embed started_utc / seconds (stage_chain.py:240-243) so chain hashes and wave-result
  receipt digests (wave_result.py:93-97) are time-dependent; rerun record differs and the scoreboard refuses two copies
  (wave_scoreboard.py:49-51); queue history uses today's date (wave_stage_record.py:165).
- Stale reuse: u/fit/w/card/marginal done by existence alone (finding 3); reader/bundle reuse on daily-CSV SHA without the
  reader code SHA (wave_stage_cell.py:67-82; wave_stage_record.py:66-91); cycle_verdict.json overwritten every run
  (cycle_verdict.py:122-125) so the screen receipt's verdict_sha256 can dangle.
- Ledger: hash chain verified on read (backtest_integrity.py:1017-1084), heads re-folded (research_cycle.py:1754-1771);
  append has no lock (finding 5); ledger in gitignored build-equity (y-s.json:30).
- DSR/PBO duplicated: Python nav_summ.py:420-463 and backtest_integrity.py:160-224, 273-327, 353-486 vs C++
  deflated_sharpe.hpp:84-151, min_trl.hpp:27, pbo.hpp:52-63, trial_clusters.hpp:108; tests tie Python to Python only;
  different DSR definitions (Python N = construction count, V = window cell variance, backtest_integrity.py:1254-1277;
  C++ RawNCrossVarV2 / ClusterMcFloorV2, MonteCarloMaxV2 documented as the calibrated gate, deflated_sharpe.hpp:193-211,
  247-300). Under pm7-34 the DSR decides nothing (wave_rules.py:140-141).

## 4. Modularity

Copy-paste: fmt_argv (research_cycle.py:243-245 / wave_steps.py:113-115); sha256 helpers x6 (research_cycle.py:248,
stage_chain.py:72, research_spec.py:225, run_bounded_research.py:40, backtest_integrity.py:563, wave_manifest.py:86);
execute (research_cycle.py:1622 / wave_context.py:29); receipt-to-timing rows (wave_stage_util.py:58-68 /
cycle_verdict.py:48-58); ledger head/lines/N readers x3; run-dir allocation x2; dirty-check x2; REGISTRY path x2;
MARGINAL_KEYS means two things (wave_manifest.py:74 vs wave_stage_util.py:17).
Implicit coupling via paths: Wave.phase_bases / run_dirs re-derive research_cycle naming (wave_context.py:120-141 vs
research_cycle.py:946-948, 1030, 1166, 1284, 1304) — a rename silently shrinks the seal scan; add_alpha_files mirrors
add-alpha outputs (wave_stage_library.py:44-53); lib-spec path x2; library rescreens (wave_stage_preflight.py:19, 51-58);
**admission budget counted by cycle-name prefix (wave_stage_preflight.py:72): y-s's prefix "v8x" does not cover its own
library "v8ys" (y-s.json:13, 27), so the next wave will not count Y's lines as used**; scoreboard glob; verdict-head glob;
sys.path order via wave_context.
Flags bypassing the manifest: wave `--root --until --seal-allow`; research_cycle `--runner-override --ledger --suffix
--attempt --reuse-fields --no-git` (:1883-1893, only the plan header shows them); add-alpha `--plan-json` replaces exe
plan validation (research_add_alpha.py:408); `candidates pin --by` any name.

## 5. Performance

Measured (v8y-research-loop.md:124-133): marginal 134.6-175.5 s per pass, run twice on a sign-rule drop = 57-59% of phase
wall; NAV 41-47 s; IC 16-41 s; card 33.7-37.2 s; summ 28.4-30.5 s.
Process churn: one add-alpha Python process per candidate, each running the exe `--plan-only` on a growing library then
`lock`; b-library repeats; each cell invokes research_cycle 3-5 times, each re-hashing pins (:785-806), re-probing `--help`,
re-running compare steps (:1157, 1667-1668); daily CSVs re-read by summ, bundle, book reader, mechanics readers
(wave_readers.py:47-53); everything serial.
Parallelisable: card with marginal (peaks 968 / 252 MiB); ref with u; judge's summ + bundle + reader; batching all
candidates into one add-alpha call. Prerequisite: a host memory budget (semaphore over declared caps) — floor kills are
not resumable (finding 4).
Timings incomplete: wave-result timings only from run/match/judge rows (wave_result.py:44-51); miss screen u/fit/card/
marginal in b-library waves, register, readers, bundle, git, per-stage seconds; `scoreboard --timings` same
(wave_scoreboard.py:135-144). Fix (S): fold in stage receipt seconds and the screen spec's phase rows.

## 6. Can a manifest express each experiment kind today?

| experiment | today |
|---|---|
| new signal | yes (library wave, kind add); new VM operator needs C++ |
| replaced signal | yes (kind replace, optional rescreen) |
| new field | NO: fields dir must be pre-built and pinned (wave_manifest.py:9; wave_stage_preflight.py:129-132); Python producer + hand build + new pin |
| new combination rule | only if it exists (template `fit.flags --composition`); new rule = Python fitter + C++ composition + template |
| new NAV rule | template on nav.rule/nav.flags if the exe has it; else C++ |
| leverage change | yes via constants.set nav.leverage with gross_match "none" (wave_rules.py:107) |
| new cost model | template flag if exposed; else C++; paired reference stays under old costs |
| new universe | partly: template replaces inputs.role; role/fields build by hand; rule cell fields not cross-checked; ledger kind always construction (LEDGER_KINDS has "universe", backtest_integrity.py:88) |
| new horizon / holding period | template ic/nav flags but must rename u/fit/w outputs or they are silently reused (finding 3; research_spec.py:31-33, 205) |
| walk-forward refit | NO: single TRAIN window (research_window.hpp:11-13); `roles` era pooling (research_cycle.py:67-71) not in manifest schema (wave_manifest.py:69-72); needs C++ refit loop + manifest kind |

## 7. Top 6 findings (impact x effort)

1. **y-s marginal cap 720 > runner max 600 (S)** — see top. Fix: RUNNER_MAX constant checked at load; ruling or C++ `--candidates` subset.
2. **Exe identity not pinned; ref identity skipped when fields match (S-M)** — research_cycle.py:228, 543-552, 1015-1019; wave_stage_util.py:58-68. Fix: `lock` writes exes_sha256, cycle checks it; verify compares parent vs cell NAV executable_sha256 and forces ref when they differ.
3. **Stale output reuse (S)** — u/fit/w/card/marginal done by existence (research_cycle.py:883-911, 1071-1077, 1323-1332); only nav/ref bound (cycle_resume.py:60-101). Fix: argv_sha256 binding in every bounded run dir, checked on resume.
4. **Failed bounded attempts not resumable through the wave (M)** — failed state hard-stops (research_cycle.py:1646-1647, 906-910); wave never passes --attempt/--suffix (wave_steps.py:76-77); one floor kill strands the wave. Fix: auto-advance to `-run<k>` on runner refusal with absent output; record the attempt.
5. **Ledger append unlocked, anchored locally (S)** — backtest_integrity.py:1094-1118; ledger in gitignored build-equity. Fix: O_EXCL lock around verify-and-append; record commits the ledger head into the sprint dir.
6. **Python statistics duplicate the engine (M)** — two DSR definitions, no cross-test. Fix: one engine `eval` verb (DSR, PBO, ONC) called by nav_summ, shared golden fixture, delete Python copies; then split research_cycle.py along §1 ranges.

## Status of earlier findings
review-yinfra.md #1-#18: all FIXED (evidence per finding in the reviewer's transcript; register still has no own inputs,
wave_stages.py:47). code-review-v8-platform.md: P-8 FIXED; P-12 STANDS for the wave (all tests fakes; live e2e optional);
P-14 STANDS (ledger / out_dir under build-equity); P-4 PARTIAL (build key exists, DLL paths hard-coded, Release not
adopted); P-10 STANDS and grew (statistics mirrors).
