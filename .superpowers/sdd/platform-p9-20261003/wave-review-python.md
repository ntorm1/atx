# Whole-wave review, area 5: Python wave / cycle (GATE-1 step 8, RESUME 11, 2026-10-15)

- **Range:** BASE `3c9eeb01` (wave-1 gate head) .. HEAD `d51d011e` (root after M6, every wave-2 + wave-3 merge but
  SQL4), branch `feat/platform-p9-20261003`, read by SHA in `C:/atx-wt/pool-2`. pool-2's checkout stood at `475f5d8f`
  (= `d51d011e` + root's M6 records commit; `git diff --name-only d51d011e 475f5d8f` outside `.superpowers/` is
  empty), so the tests below ran `d51d011e`'s code; HEAD was `475f5d8f` before and after every run.
- **Lanes in the area:** B2 (eval verb adapter, B2-11 pin, G-P5 deletion, fix 2 / B2-N37), C3b (Python `pm6-6`
  retired, C2-9 / C2-10 / C3-12), D3 (C3-3 / E2-17 guards, the walk-forward kind, C3-13, W3-8a), E2 and its
  follow-ups (cycle split, v2 kinds, rule guards, W1-I1 / W1-I2), OD3P (carry tool, `fit.carry.drop`, T2 on the
  verb), SQL3's call lines (item 5, SQL3-WQ), SIGN's stage call; plus every other lane's Python under the area's
  paths (A3 / A4 / T2 / AL-SIG field tools, D2 / AL-COMB / D3W / D3W-PB fitter modules).
- **Reviewer:** fresh, read-only in pool-2 (no git write, no build, no edit there; nothing committed). No 2020-2023
  statistic was read; the n37 test ran under the blind flags and only its pass / fail is recorded.
- **Verdict: APPROVE.** Required **0** (I 0, M 0). Suggested **7**.
- **Files:** 128 in the area's paths; 14 belong to the store (12) and fields (2) areas and are only cited; of the
  114 that are mine, **49 reviewed** (diff read; for the large new files the parts named below), **65 sampled**
  (12 run as tests + read in part, 53 by the sweeps of "Sampling" and their diffs' headers).

## Judged

| # | question | result |
|---|---|---|
| 1 | No new Python mirror of a C++ rule; every retired rule's G-P5 row gone with it | **Holds.** `test_no_python_mirror.py` green at seeds 0 and 1, printing "G-P5: 0 mirrored rule function(s) in 0 row(s) left for P9 lanes". Retirements: B2's statistics rows (`backtest_integrity.py`, `nav_summ.py`, `dsr_total.py`), C3b's gross match (`matched_leverage`, `gross_matches`), SIGN's `sign_pm7_35` / `screen_decision`, D3W-PB's `composition_resid` row: each function and its row are gone, the names stay detected through `FROZEN_PAIRS` (`test_no_python_mirror.py:80-95`). No caller of a retired name is left outside tests / fixtures (git grep at HEAD; the frozen copies live in `atx-engine/tests/fixtures/eval_tie/frozen_statistics.py`, an oracle, out of the scan by design). The remaining composition rows are `P10` by D3WPB-Q1. A blind spot of the scan's scope is S-1 |
| 2a | Stage chain contracts for the runs of record | **Holds.** `stage_chain.py`: exit codes `EXIT_STALE 3` / `EXIT_STOP 4` unchanged; `on_write` defaults to None and a raising callback is one stderr line (`:128-135`); the content digest now drops nested time keys (E2, digest="content" only; the default "file" rule is byte for byte as before). `run_bounded_research.py`: exit `0` / `1` as before, the `store` key only when the catalog is on or an argv dir holds an index (SQL-6). The P9-B0 hand runs (`research_cycle.py run` on the lineage specs, plan Appendix A) keep their summ argv: the lineage specs name explicit `exes {ic, nav}` and no `build` key, so `effective_exes` adds no `eval` and no `--engine-stats` appears until the re-pin names `exes.eval` (B2-10 (b), planned in the substitution list). `wave plan` / `scoreboard --timings` route as before (`cycle/cli.py:513-521`); see S-4 for Appendix A's `wave run` line |
| 2b | Wave kinds, W3-8a | **Holds.** `SCHEMA_ONLY == ()` (`wave_kinds.py:82`), `SCHEMA_ONLY_FIXTURES == ()` (`test_wave_kinds.py:78`), 10 kind fixtures, 10 runnable, 0 schema only (`:216-217`); 8 fixtures name `pm6-6-engine`, `leverage-change` and `new-nav-rule` name `none` |
| 2c | Walk-forward kind (D3), D3-Q1, D3-Q3 | **Holds.** Derived argv `--walk-forward expanding --walk-forward-lag 2 --walk-forward-step 21 --walk-forward-min-decisions 21` (`wave_kinds.py:311-319`, the fixture names all four, D3-Q3), names equal the fitter's (`composition_engine_fit.py:184-195`; fitter `allow_abbrev=False`, `fit_composition_weights.py:2763`). D3-Q1's precondition: preflight refuses a parent whose resolved `fit.flags` lacks `--engine-fit` or carries `--theme-tsmom` / `--theme-resid` / `--era` (`wave_kinds.py:341-358`, called on the resolved parent spec, `wave_stage_kinds.py:90-94`) |
| 3 | Look-ahead: cycle clocks, OD-3 tools, flagged members, absent-members contract | **Holds.** Roles are window-checked against TRAIN and the seal (`research_roles.py:151-157`, pre-existing); `fit.carry.drop` reaches only history roles' u / w IC passes (`research_roles.py:462-483`; my mutation below kills it in 6 tests); a drop with no history role, an exe without `absent-members-v1`, a pending rule, a base applying a listed rule, a stale `.pending` or foreign `fit.output` are refused at plan time; `carry_weights.py` changes no weight (OD3P-1) and opens only pinned files. `flagged_members.json`: 12 members, `rules` = the three registrations with `source_sample_end` >= 2015 (2017, 2016, 2019 by their files), `mv-aim-v1` (2010) still `rules_pending` = MV-FLAG-P0, root's at P0. No default data path at or after 2024-01-01 is opened by any Python file of the range (sweep below); the only 2024+ dates are synthetic fixtures (`t2_gold.py` under `superseded_window`, seal tests) |
| 4 | B2-N37 | **Holds.** `test_nav_summ_v8.py:66-118` compares every float at `math.isclose(rel_tol=1e-12, abs_tol=0)`, every other leaf exactly; the docstring names B2-N37, B2-R1 (b) and the B2-6 / B2-N37-B6 self-pair disclosure (`:121-131`); the golden is only read: `mega-nav-v71-summ-n37.json` `98b44b24...9f98`, `-pbo-n37.json` `bc112b38...b1ca`, mtime 2026-09-29 (= the pins). Assertions carry JSON paths and counts only; values could reach a console only through captured output, which the blind flags (`--tb=no --show-capture=no -rN`) suppress. My run: 10 passed, the n37 case among them (not skipped) |
| 5 | SQL3's call lines; hook off in the runs of record | **Holds.** Both `wave_queue.py` lines pass root-relative names (`:146` `Path(d or QUEUE_DIR) / p.name`; `:350` `a.output`, the same base as `out = root / a.output` at `:345`); `cycle_resume.py:193`, `cycle_verdict.py:183`, `:196` pass `cycle.res.root`-relative names; `wave_stage_record.py:348` an absolute path under `w.root` (the hook takes either). Off by default: `build-equity/research-store/` is absent in pool-2 (before and after my runs) and no `index.sqlite` exists under pool-2's `build-equity` to depth 7, so neither route of `research_store_hook.selection` (catalog, argv-dir index) turns on |
| 6 | Test hygiene | **Holds for the range.** Every RNG in the added lines is seeded (`default_rng(7 / 5 / 31 / 37 / 3 / 20261003 / seed)`); no network (one URL is a string inside a fixture CSV); no `atx-db` / `atx_db` read is added in the range (the only added hits are prose and a planted path inside a temp repo, `test_research_store_classes.py`); caches: my runs left no `__pycache__` or `.pytest_cache` in the tree and wrote nothing under `build-equity` (find `-newer`, depth 3). The pre-existing `atx_db` imports of `test_prepare_research_fields.py:1292` and `build_fundamental_events.py:53-61` (not added in range) bind this worktree's `atx-db/src` first (`prepare_research_fields.py:1797-1803`, `build_fundamental_events.py:338-339`) |

## Files (128; line counts are the range's +/-)

Tags: **R** reviewed (diff read; `cycle/*` by an AST comparison of E2's split plus the post-E2 diff; the four large
new files `wave_kinds.py`, `wave_stage_kinds.py`, `composition_engine_fit.py`, `scripts/cycle/phases.py` in the parts
the judgments above cite); **T+S** run as a test and read in part; **S** sampled (the sweeps of "Sampling", the diff
header and the hunks they hit); **O-store** / **O-fields** another area's (precedence of `r11-dispatch.md`), cited only.

| file | +/- | tag |
|---|---|---|
| `atx-engine/tools/conftest.py` | +19 / -17 | R |
| `atx-engine/tools/field_registry.json` | +106 / -20 | O-fields |
| `atx-engine/tools/prepare_research_fields_draft.py` | +10 / -4 | S |
| `atx-engine/tools/prepare_research_fields_engine.py` | +313 / -28 | S |
| `atx-engine/tools/prepare_research_fields_ohlc.py` | +10 / -4 | S |
| `atx-engine/tools/prepare_research_fields_xdata.py` | +10 / -4 | S |
| `atx-engine/tools/prepare_research_fields_ydata.py` | +10 / -4 | S |
| `atx-engine/tools/record_store.py` | +189 / -1 | O-store |
| `atx-engine/tools/research_store.py` | +268 / -0 | O-store |
| `atx-engine/tools/research_store_identity.py` | +384 / -0 | O-store |
| `atx-engine/tools/stage_chain.py` | +39 / -10 | R |
| `atx-engine/tools/t2_gold.py` | +424 / -0 | S |
| `atx-engine/tools/test_build_fundamental_events.py` | +10 / -10 | S |
| `atx-engine/tools/test_field_registry.py` | +40 / -7 | S |
| `atx-engine/tools/test_linked_operating_v2.py` | +6 / -0 | S |
| `atx-engine/tools/test_linked_operating_v3.py` | +6 / -0 | S |
| `atx-engine/tools/test_lo1_delisting.py` | +7 / -0 | S |
| `atx-engine/tools/test_prepare_identity_bridge.py` | +6 / -6 | S |
| `atx-engine/tools/test_prepare_research_fields.py` | +140 / -140 | S |
| `atx-engine/tools/test_prepare_research_fields_reuse.py` | +1 / -1 | S |
| `atx-engine/tools/test_prepare_research_fields_sec.py` | +147 / -144 | S |
| `atx-engine/tools/test_prepare_research_fields_sic.py` | +15 / -9 | S |
| `atx-engine/tools/test_prepare_research_fields_sv.py` | +23 / -23 | S |
| `atx-engine/tools/test_record_store_sqlite.py` | +217 / -0 | O-store |
| `atx-engine/tools/test_research_fields_connected.py` | +12 / -9 | S |
| `atx-engine/tools/test_research_fields_deals.py` | +2 / -1 | S |
| `atx-engine/tools/test_research_fields_divevent.py` | +1 / -0 | S |
| `atx-engine/tools/test_research_fields_gold.py` | +8 / -6 | S |
| `atx-engine/tools/test_research_fields_holdings.py` | +72 / -71 | O-fields |
| `atx-engine/tools/test_research_fields_mgr13f.py` | +17 / -14 | S |
| `atx-engine/tools/test_research_fields_v9_nt.py` | +9 / -8 | S |
| `atx-engine/tools/test_research_fields_xdata.py` | +6 / -6 | S |
| `atx-engine/tools/test_research_store.py` | +204 / -0 | O-store |
| `atx-engine/tools/test_research_store_blind.py` | +68 / -0 | O-store |
| `atx-engine/tools/test_research_store_classes.py` | +215 / -0 | O-store |
| `atx-engine/tools/test_research_store_fixtures.py` | +156 / -0 | O-store |
| `atx-engine/tools/test_research_store_identity.py` | +521 / -0 | O-store |
| `atx-engine/tools/test_stage_chain.py` | +53 / -2 | R |
| `atx-impl/tools/backtest_integrity.py` | +10 / -416 | S |
| `atx-impl/tools/book_diagnostics.py` | +22 / -3 | R |
| `atx-impl/tools/book_monitor.py` | +17 / -1 | R |
| `atx-impl/tools/carry_weights.py` | +276 / -0 | R |
| `atx-impl/tools/composition_engine_fit.py` | +834 / -0 | R (flags, walk-forward rows, post-step) |
| `atx-impl/tools/composition_ic_shrink.py` | +35 / -14 | S |
| `atx-impl/tools/composition_mom_volman.py` | +135 / -0 | R |
| `atx-impl/tools/composition_resid.py` | +85 / -154 | S |
| `atx-impl/tools/composition_rules.py` | +48 / -8 | R |
| `atx-impl/tools/composition_theme_erc.py` | +20 / -11 | S |
| `atx-impl/tools/composition_theme_hedge.py` | +67 / -0 | R |
| `atx-impl/tools/composition_theme_tsmom.py` | +28 / -12 | R |
| `atx-impl/tools/dsr_total.py` | +30 / -53 | R |
| `atx-impl/tools/equity_strategy_ic.cpp` | +2 / -0 | R |
| `atx-impl/tools/equity_strategy_risk.cpp` | +11 / -4 | R |
| `atx-impl/tools/fit_composition_weights.py` | +200 / -111 | S |
| `atx-impl/tools/nav_summ.py` | +286 / -256 | R (engine adapter outline, OD3P T2 hunk) |
| `atx-impl/tools/test_backtest_integrity.py` | +32 / -94 | S |
| `atx-impl/tools/test_book_diagnostics.py` | +2 / -0 | S |
| `atx-impl/tools/test_book_monitor.py` | +27 / -0 | S |
| `atx-impl/tools/test_composition_engine_fit.py` | +826 / -0 | S |
| `atx-impl/tools/test_composition_fit_fixture.py` | +293 / -0 | S |
| `atx-impl/tools/test_composition_mom_volman.py` | +266 / -0 | S |
| `atx-impl/tools/test_composition_resid.py` | +203 / -37 | S |
| `atx-impl/tools/test_composition_theme_hedge.py` | +213 / -0 | S |
| `atx-impl/tools/test_composition_theme_tsmom.py` | +5 / -3 | S |
| `atx-impl/tools/test_dsr_total.py` | +96 / -14 | S |
| `atx-impl/tools/test_fit_composition_rule_plugins.py` | +11 / -4 | S |
| `atx-impl/tools/test_fit_composition_weights.py` | +114 / -8 | S |
| `atx-impl/tools/test_fit_composition_weights_store.py` | +11 / -3 | O-store |
| `atx-impl/tools/test_fit_walk_forward.py` | +661 / -0 | S |
| `atx-impl/tools/test_nav_summ.py` | +66 / -84 | S |
| `atx-impl/tools/test_nav_summ_engine.py` | +287 / -0 | S |
| `atx-impl/tools/test_nav_summ_v8.py` | +79 / -9 | R + T |
| `atx-impl/tools/test_od3_tooling.py` | +724 / -0 | T+S |
| `atx-impl/tools/test_trial_ledger_rules.py` | +17 / -0 | S |
| `scripts/cycle/__init__.py` | +11 / -0 | R |
| `scripts/cycle/cli.py` | +585 / -0 | R |
| `scripts/cycle/compare.py` | +107 / -0 | R |
| `scripts/cycle/gate.py` | +169 / -0 | R |
| `scripts/cycle/lock.py` | +86 / -0 | R |
| `scripts/cycle/phases.py` | +1031 / -0 | R |
| `scripts/cycle/resolver.py` | +53 / -0 | R |
| `scripts/cycle/spec.py` | +495 / -0 | R |
| `scripts/cycle_resume.py` | +7 / -3 | R |
| `scripts/cycle_verdict.py` | +54 / -5 | R |
| `scripts/research_common.py` | +81 / -0 | R |
| `scripts/research_cycle.py` | +34 / -2289 | R |
| `scripts/research_roles.py` | +273 / -6 | R |
| `scripts/research_spec.py` | +2 / -8 | R |
| `scripts/research_store_hook.py` | +211 / -0 | O-store (read in full for item 5) |
| `scripts/research_wave.py` | +5 / -2 | R |
| `scripts/run_bounded_research.py` | +97 / -34 | R |
| `scripts/tests/test_cycle_scoring.py` | +62 / -1 | T+S |
| `scripts/tests/test_info_clock_rule.py` | +285 / -0 | T+S |
| `scripts/tests/test_no_python_mirror.py` | +22 / -24 | R + T |
| `scripts/tests/test_p9_al_sig_registrations.py` | +179 / -0 | S |
| `scripts/tests/test_research_cycle.py` | +38 / -8 | T+S |
| `scripts/tests/test_research_cycle_od3.py` | +779 / -0 | T+S (and the mutation's target suite) |
| `scripts/tests/test_research_ledger.py` | +5 / -7 | S |
| `scripts/tests/test_research_mine.py` | +16 / -4 | S |
| `scripts/tests/test_research_spec.py` | +52 / -15 | S |
| `scripts/tests/test_research_store_hook.py` | +559 / -0 | O-store |
| `scripts/tests/test_research_wave.py` | +181 / -22 | T+S |
| `scripts/tests/test_wave_driver.py` | +381 / -12 | T+S (SIGN-WD hunk and the W1-I1 test read) |
| `scripts/tests/test_wave_eval_verb.py` | +278 / -0 | T+S |
| `scripts/tests/test_wave_gross_match.py` | +284 / -0 | T+S |
| `scripts/tests/test_wave_kinds.py` | +1566 / -0 | T+S |
| `scripts/tests/test_wave_queue.py` | +28 / -1 | T+S |
| `scripts/tests/test_wave_review.py` | +4 / -2 | S |
| `scripts/tests/test_wave_review_minor.py` | +2 / -2 | S |
| `scripts/tests/test_wave_scoreboard.py` | +6 / -3 | S |
| `scripts/tests/test_wave_sign_verb.py` | +305 / -0 | T+S |
| `scripts/tests/test_wave_speed.py` | +3 / -3 | S |
| `scripts/tests/wave_fixture.py` | +112 / -11 | R |
| `scripts/wave_context.py` | +52 / -26 | R |
| `scripts/wave_kinds.py` | +750 / -0 | R (walk-forward kind, KIND_FLAGS, SCHEMA_ONLY) |
| `scripts/wave_manifest.py` | +33 / -14 | R |
| `scripts/wave_queue.py` | +6 / -0 | R |
| `scripts/wave_result.py` | +40 / -4 | R |
| `scripts/wave_rules.py` | +71 / -51 | R |
| `scripts/wave_scoreboard.py` | +3 / -7 | R |
| `scripts/wave_seal.py` | +6 / -2 | R |
| `scripts/wave_stage_cell.py` | +227 / -36 | R |
| `scripts/wave_stage_kinds.py` | +769 / -0 | R (preflight_kind, run_listing, the walk-forward branch) |
| `scripts/wave_stage_library.py` | +76 / -15 | R |
| `scripts/wave_stage_preflight.py` | +37 / -10 | R |
| `scripts/wave_stage_record.py` | +164 / -27 | R |
| `scripts/wave_stage_util.py` | +13 / -4 | R |
| `scripts/wave_steps.py` | +9 / -9 | R |

### Sampling (applied to all 128 files, by grep over the range diff and over the files at `d51d011e`)

- `atx-db` / `atx_db` / `ATX_DB` (added lines and whole files), absolute `C:/atx/` paths (none added; the one
  `C:/atx-wt/pool-2/...` string, `test_wave_driver.py:924`, is path-length arithmetic, never opened), `build-equity`
  in tests (env-gated or temp-root strings only), network imports / URLs, unseeded RNG, 2024+ dates and seal
  overrides in non-test code, `holdout`, `*_v<digits>.py` / `generate_*_v*` added (G-P6: none), callers of every
  retired rule name.
- The E2 split (`430c29e8^1:scripts/research_cycle.py` vs `430c29e8:scripts/cycle/*.py`, top-level AST): 85 of 95
  names identical, 3 moved to `research_common.py` (`fmt_argv`, `sha256_file`, `execute`), 7 changed (`INPUT_KEYS`,
  `find_spec`, `exe_capabilities`, `Cycle`, `git_scoped`, `ledger_state`, `main`) and 11 added (W1-I1's `CACHE_*`,
  `deepest_cache_path`, `cache_path_problem`, `library_ids`, `SCRIPTS_DIR`): E2's own items on top of the move,
  reviewed in E2's five rounds. After E2 only B2 (`exes.eval`), D2R / C3B-6 (text) and OD3P (the T1 refusal) touched
  `scripts/cycle/` (`git diff 430c29e8 d51d011e -- scripts/cycle/`: 3 files, 9 / 5 / 30 lines).

## Required

None.

## Suggested

| id | file:line | the input / the gap | fix |
|---|---|---|---|
| S-1 | `atx-impl/tools/composition_theme_tsmom.py:88-160` (`masses`, `theme_masses`, `sleeves`, `trailing`), `composition_mom_volman.py:93` (the sleeve via `composition_theme_tsmom.sleeves`); `scripts/tests/test_no_python_mirror.py:80-95` | Since D2 (kernels: "tsmom parent masses, trailing sums and block masses", `composition_engine_fit.py:6-9`) and ALCOMB-7 (the C++ sleeve), the C++ composition fit computes these numbers too, while the flag-absent fit still computes them in Python. None of these names is in `FROZEN_PAIRS`, so the G-P5 scan cannot see them; the guard is green by its definition (the frozen P9 list), not because the copy is gone | No P9 code change. The freeze report's P10 centralization item (D3WPB-Q1) names these two modules beside the 8 `P10` rows, so P10's retirement of the Python fit covers them |
| S-2 | `scripts/wave_stage_library.py:179` (`w.run([... exe, "sign", ...])`) with `wave_context.py:206-218` | The screen stage runs the IC exe's sign verb with `Wave.env()` (`os.environ`), where preflight's `run_listing` (`wave_stage_kinds.py:59`) and the cycle run the same exe with `Cycle.env()` (the spec's `env_path_prepend`). Probe: pool-2's p9-3b IC exe (`ef3974cf...`) prints `sign --help`, exit 0, with every vcpkg dir removed from PATH, so today's bin starts without it; a bin without app-local DLLs would fail only at a real wave's screen. No registered P9 cell has a screen stage (all are rule / walk-forward waves or hand cycles), so nothing in P9 runs this call | P10 (or a root slip): give `Wave.run` an `env` argument and pass `RC.Cycle(w.load_spec(spec), RC.Resolver(w.root), verify=False).env()`, as `run_listing` does |
| S-3 | `atx-impl/tools/composition_engine_fit.py:699-722` (`walk_forward_rows`) | The post-step checks the C++ rows' shape (keys, counts, increasing `decision_index`, finite weights) but not the K-P9-8 window itself: `last_fit_index + lag <= decision_index`, `fit_decisions >= min_decisions`, `from_session` increasing. A C++ look-ahead regression would be published into the v3 weights; today the only guard is the gtest `WalkForward.NoRowUsesFutureDecisions` (D3W, IC area) | P10: three fail-closed comparisons on the published rows (a contract check on the C++ output, not a second derivation of the fit) |
| S-4 | plan `docs/plans/2026-10-03-p9-sprint-plan.md:1040` (Appendix A) with `scripts/specs/v8/waves/y-s.json:23`, `y-s.head.json:11` | Appendix A's `research_cycle.py wave run scripts/specs/v8/waves/y-s.json --until screen` now stops at preflight (exit 3): the ledgered v1 manifests name the retired `"pm6-6"` (C2-11; `wave_stage_preflight.py:169-171`), and an `emit` from `y-s.head.json` writes a manifest that validates but cannot run. By ruling (C2-11: replay from a copy naming `pm6-6-engine`); `wave plan` and `scoreboard --timings` still work | Text only: the freeze report (or a plan erratum) says Appendix A's `wave run` example needs a `pm6-6-engine` copy |
| S-5 | `atx-impl/tools/carry_weights.py:236`; `scripts/cycle/cli.py:522` | Neither parser sets `allow_abbrev=False` (D2R-1 / D2R-2 (b) did it for the fitter and nav_summ): `--weights-s X` is read as `--weights-sha256 X`, `--host-budget N` as `--host-budget-mib N`. The cycle writes full names, so no planned argv is affected; only a hand-typed runbook line could be | P10: `allow_abbrev=False` on both, with one refusal test each |
| S-6 | `scripts/research_common.py:76-81` (`ledger_view`: `backtest_integrity.ledger_n`) | The wave's preflight N and the scoreboard's N are Python's `ledger_n`; the engine verb counts N from the same ledger in C++ (B2, `research/ledger`). Two producers of one count, nothing compares them | At the gate's scoreboard step (root-gate-brief step 7) print both and check they are equal; P10: one producer |
| S-7 | `scripts/wave_stage_preflight.py:44-60` (`cache_path_check`) | The W1-I1 bound fails open: a `ValueError` / `KeyError` / `CycleError` while reading the library or deriving the cache dir returns `[]` (the docstring says other checks refuse such a spec; true for the cases I traced, but not proven for every one) | P10: a refusal line instead of `[]` when the derivation itself fails, or a test per swallowed error |

## Tests I ran (pool-2 tree at `d51d011e` code; `PYTHONDONTWRITEBYTECODE=1`, `-p no:cacheprovider`, own `--basetemp` under the session scratchpad, `--tb=no --show-capture=no -rN`, one file per invocation, `C:/Program Files/Python312/python.exe`)

Env: `ATX_EQUITY_ROOT=C:/atx-wt/pool-2/build-equity`, `ATX_EQUITY_BIN=C:/atx-wt/pool-2/build-equity/bin` (read only).
Exes by SHA-256, equal to their receipts before and after: eval `47910191...893b` (p9-2z, source `138e1d2c`), IC
`ef3974cf...9b38` (p9-3b, `1dd9b9b1`), NAV `7e101b56...f0dc` (p9-3d, `1d003241`), store `d8a500d8...9d65` (p9-2r,
`fd084e78`), composition `212f8b99...e98e` (hashed only). Trial ledger 133 lines, `27e40f9f`, mtime unchanged.
ENV-1: 0 hits (exit 3221225794) in every log.

| file | seed 0 | seed 1 |
|---|---|---|
| `scripts/tests/test_no_python_mirror.py` | 7 passed | 7 passed |
| `scripts/tests/test_wave_kinds.py` | 192 passed (the live-table case ran on the p9-3d / p9-3b exes) | 192 passed |
| `scripts/tests/test_wave_driver.py` | 28 passed | 28 passed |
| `scripts/tests/test_research_wave.py` | 19 passed | 19 passed |
| `scripts/tests/test_wave_gross_match.py` | 12 passed | 12 passed |
| `scripts/tests/test_wave_eval_verb.py` | 7 passed | 7 passed |
| `scripts/tests/test_wave_sign_verb.py` | 21 passed (real exe) | 21 passed |
| `scripts/tests/test_wave_queue.py` | 6 passed | 6 passed |
| `scripts/tests/test_research_cycle.py` | 93 passed, 3 skipped (`RESEARCH_CYCLE_LIVE_ROOT` unset: the gate's) | 93 passed, 3 skipped |
| `scripts/tests/test_research_cycle_od3.py` | 50 passed | 50 passed |
| `scripts/tests/test_cycle_scoring.py` | 5 passed | 5 passed |
| `scripts/tests/test_info_clock_rule.py` | 5 passed | 5 passed |
| `atx-impl/tools/test_od3_tooling.py` | 46 passed (the OD3P-SKIP `real` cases ran) | not run |
| `atx-impl/tools/test_nav_summ_v8.py` (n37) | 10 passed, n37 not skipped | not run |
| **total** | **501 passed, 3 skipped, 0 failed** | **445 passed, 3 skipped, 0 failed** |

Not run (TEST-1, the gate's): whole `scripts/tests`, `atx-engine/tools/test_stage_chain.py`, the rest of
`atx-impl/tools` / `atx-engine/tools` / `atx-impl/strategies`, every C++ suite.

## Mutation (TEETH-RESTORE, TEETH-CRLF form)

- Where: a `git archive d51d011e` copy of `scripts/`, `atx-engine/tools/`, `atx-impl/tools/`, `atx-impl/strategies/`,
  `atx-engine/schemas/` in the session scratchpad (never pool-2's tree: root may merge there), tool
  `scratchpad/wave-python/teeth_od3.py`.
- Code under test: `scripts/research_roles.py` `RolesCycle.absent_members` (OD3P-1: the list reaches history roles
  only), line `history = [c for c, r in zip(self.eras, self.roles) if is_history(r)]` -> `history = list(self.eras)`.
- Baseline on the copy: 47 passed, 1 failed, 2 skipped. The 1 is
  `test_carry_replaces_the_pooled_fit_and_feeds_every_era`, which needs the eval verb (none in the copy; OD3P-ENV,
  green in pool-2 above); the 2 skips need git history (none in the copy).
- Mutant: **7 failed**, 6 new: `test_drop_passes_the_absent_list_to_the_history_ic_passes_only`,
  `test_drop_runs_the_real_carry_and_the_carried_weights_are_train_bytes`,
  `test_each_history_book_gets_its_own_fresh_fit_output_and_the_carry_runs`,
  `test_the_absent_flags_reach_only_the_history_roles_ic_passes[history_train_anchor]` and
  `[two_history_train_anchor]`, `test_a_history_ic_pass_without_the_runner_is_refused_not_skipped`. **Killed.**
- Restore (in `finally`): SHA-256 `be3c5393...` = the saved bytes; `git -C pool-2 hash-object` `270c2f8d...` =
  `git rev-parse d51d011e:scripts/research_roles.py`.

## Permission refusals

None. One harness block (not the permission system): `sleep 60; cat <log>` was refused by the tool's rule against a
foreground sleep; not retried, not substituted (I read the log when the background run's notice came).

## What root must do before any cell

1. Nothing of this area is Required. Run the gate's whole suites (TEST-1): `scripts/tests` at seeds 0 and 1, the
   stage chain on its own, `atx-impl/tools` (the n37 test on the Release verb at 1e-12, B2-N37 item 4),
   `atx-engine/tools`, `atx-impl/strategies`. My subset does not replace them.
2. SQL3-2 before P9-B0: `build-equity/research-store/` absent (it is now) AND no `index.sqlite` in, beside or one
   level under any directory a run of record names (none to depth 7 now): `research_store_hook.selection` turns the
   `store` block on for either route, and an index also adds an SQL-11 void condition.
3. P0 (MV-FLAG-P0): resolve `mv-aim-v1` out of `flagged_members.json` `rules_pending` (2010: removed) and re-pin the
   file; until then the clean-source plan refuses, by design.
4. Every P9 manifest / template: `gross_match: pm6-6-engine` named (C2-9, PRE-40; no default exists); `exes.eval`
   named and pinned through the re-pin (B2-10 (b), B2-11: verify refuses a v2 cell without it, the judge refuses
   another verb); P9-W names `--walk-forward-min-decisions 21` explicitly (D3-Q3) and its parent fits under
   `--engine-fit` (D3-Q1, else P9-W lapses at 0); the D3-Q2 (`fields.output` in `constants.set`) and D3-S1 (`nav.rule`
   outside a kind `rule` wave's row) greps over `scripts/specs/p9/`.
5. At the freeze: set `P9_FREEZE = True` in `test_no_python_mirror.py` (0 P9-lane rows left, the test passes with
   it); the freeze report's P10 list takes S-1 (with D3WPB-Q1's 8 rows), S-2, S-3, S-5, S-7, and S-4's text; the
   scoreboard step prints S-6's two N.
