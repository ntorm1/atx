# Task YINFRA report: one research wave, one command (lane YINFRA, 2026-10-02)

Worktree `C:/atx-wt/pool-15`, branch `feat/platform-v8-yinfra-20261002`, base `798d3b23`. Python only; no C++ built or
written; no real data run; nothing under `atx-db/`; no subagents. Every stage is tested on synthetic fixtures (a git
root per test, sessions 2020-2021, fake research_cycle / add-alpha / bounded runner / bundle processes).

## Commits

| item | commit | content |
|---|---|---|
| 1 wave manifest + driver | `487faf0b` | `atx-engine/tools/stage_chain.py` (+ test), `scripts/research_wave.py`, `wave_context.py`, `wave_stages.py`, `wave_steps.py`, `wave_rules.py`, `wave_manifest.py`, `wave_readers.py`, `wave_result.py`; `research_cycle.py` verb `wave`; `research_add_alpha.py --save-plan` |
| 2 candidate queue | `00722108` | `scripts/wave_queue.py`; verb `candidates`; preflight queue check; record stage sets statuses |
| 3 book scoreboard | `d610985f` | `scripts/wave_scoreboard.py`; verb `scoreboard` |
| 4 speed | `4af3ce8a` | manifest `speed` (reuse_screen_marginal, screen_first), replacing-wave pool-only marginal, `scoreboard --timings` |
| 5 plan + report | (this commit) | `docs/plans/2026-10-02-v8y-research-loop.md`, this report |

## 1. What was built (interfaces as coded)

**Generic** `atx-engine/tools/stage_chain.py`: `Stage(name, run, inputs, plan)`, `Chain(name, stages, state_dir)` with
`run(ctx, until=, log=)`, `state(ctx)`, `plan(ctx)`. Receipts `<state>/receipts/NN-<stage>.json` (written once);
`inputs` recorded and re-computed on every run: unchanged -> skipped, changed -> `ChainError` code 3 (never re-run
over a receipt); each stage's inputs include the SHA-256 of the previous ok receipt; a `StageError` writes
`NN-<stage>.failed-k.json` and the next run retries. Standard library only.

**Wave** `research_cycle.py wave {plan,run,status} MANIFEST [--root R] [--until STAGE] [--dry-run]`
(`scripts/research_wave.py`). Manifest `atx.research-wave/v1` (`scripts/wave_manifest.py`): wave, parent {spec,
library}, fields {dir, manifest_sha256}, library, candidates[] (id, dsl, dsl_sha256, theme, tier, prior_sign, citation,
origin, hypothesis, kind add|replace, replaces, rescreen, removes, fields, exception, ...) or rule_cell {template,
template_sha256, name, constants{set, flags}}, acceptance {rule, printed[]}, sign_rule, gross_match, budget {id,
admission_cap + admission_cycle_prefix, construction_cap}, ledger, expect {n_before}, out_dir, record {copy_to},
speed, b_suffix. Stages (`scripts/wave_stages.py`): preflight -> register -> screen -> spec -> run -> match -> verify
-> judge -> record (table in the plan doc, section 2). Named rules (`scripts/wave_rules.py`): sign `pm7-35`;
acceptance `pm7-34`, `v8-prereg-5`; criteria `turnover-per-gross-not-higher`, `capacity-4x-higher`, `cost-bps-lower`;
gross match `pm6-6` (tolerance .005, one correction), `none`; mechanics `v8-mech`. Readers (`scripts/wave_readers.py
mechanics|book --nav NAME=DIR --output FILE`) run under the bounded runner; `mechanics` writes construction keys only.
Result `atx.wave-result/v1` (`scripts/wave_result.py`) + `wave-log.md` (the ready-to-paste log section).

**Queue** `research_cycle.py candidates {new,validate,list,pin,emit}` (`scripts/wave_queue.py`), files
`scripts/specs/v8/candidates/<id>.json` (`atx.wave-candidate/v1`): status proposed -> pinned -> screened | admitted |
dropped | in-book, wave, history. Refuses a duplicate DSL hash (queue or alpha registry under another id), an id
registered with another DSL, a second variant of one hypothesis without `ruling`; `emit --head H --select ids --output
M [--after wave-result.json]` writes a validated manifest of pinned candidates (parent and N from the previous result).

**Scoreboard** `research_cycle.py scoreboard [--results GLOB ...] [--ledger P] [--markdown F] [--json F] [--timings]`
(`scripts/wave_scoreboard.py`): reads wave-result.json files and the ledger only; prints the accepted lineage and every
wave (S2 net SR, net and gross annual, 4x net SR, tau, max DD, N, dSR, bundle p, ledger DSR); checks each cell's ledger
line and s2_net_sr (exit 1 on a mismatch; 2 on conflicting copies of one wave).

## 2. How root verifies

- Tests: `"C:/Program Files/Python312/python.exe" -m pytest -q -p no:cacheprovider scripts/tests` and
  `... atx-engine/tools/test_stage_chain.py` (results in section 6).
- Identity (flag-absent): no existing command changed. `research_cycle.py` gains three dispatch lines per verb (`wave`,
  `candidates`, `scoreboard`) before the argparse of plan/run/status/lock (its docstring and `--help` unchanged);
  `research_add_alpha.py` gains `--save-plan`, and `test_add_alpha_save_plan_is_opt_in` asserts that without it the
  files add-alpha writes are the same set with the same bytes. Check with `git diff 798d3b23 -- scripts/research_cycle.py
  scripts/research_add_alpha.py` (additions only).
- Dry run on the real tree (reads specs only): write a manifest (example in the plan doc), then
  `python scripts/research_cycle.py wave plan <manifest>`; checked here against `x-theme-erc-gm.json` as parent: every
  stage's argv printed, nothing created.

## 3. Commands root will use

```
python scripts/research_cycle.py candidates pin --id ID [--id ID ...] --by PM --ruling R
python scripts/research_cycle.py candidates emit --head H.json --select a,b,c --output scripts/specs/v8/waves/W.json --after build-equity/waves/<prev>/wave-result.json
git add scripts/specs/v8/waves/W.json && git commit -m "wave W: manifest"
python scripts/research_cycle.py wave plan scripts/specs/v8/waves/W.json
python scripts/research_cycle.py wave run  scripts/specs/v8/waves/W.json [--until screen]
python scripts/research_cycle.py scoreboard --timings
```

## 4. Speed findings (evidence: integration-log.md)

1. Marginal IC verb twice per wave with a drop (report only): X-2 141.1 + 134.6 s (lines 5622, 5647), X-3 175.5 +
   167.0 (5723, 5741), X-4 157.7 + 143.8 (5814, 5830): 57-59% of the phase wall. **Implemented** `reuse_screen_marginal`
   (the b cell carries the screen's rows; saves 134.6-167.0 s per such wave). C++ design: `--candidates` subset.
2. NAV replays: calibration + matched (X-3 41.6 + 45.5, 5744-5745; X-5 45.1 + 45.4; X-6 47.1 + 46.7) or nav + ref (X-2
   41.5 + 43.3, 5649-5651). Nothing safe in Python beyond resume (the calibration is the cell when matched; ref skipped
   on equal fields). C++ design: in-process `--calibrate-gross` without the capacity books; NAV stays Debug (Release
   differs by one ULP, 3359-3365).
3. IC passes on the Debug build: u twice + w (X-3 16.2 + 26.6 + 34.7; X-4 18.2 + 26.3 + 41.5). **Implemented**
   `screen_first` (the b library's u pass is the screen's, without the unread blend: X-3's b u pass was 26.6 s vs the
   screen's 16.2 s). Recommendation (no code): the Release IC exe is byte-identical for u and w and 59% / 82% faster
   (3344-3358); a cell spec can name `exes.ic` on `build-equity-rel` with `exes.nav` on Debug.

## 5. Deviations, cross-lane edits, open risks

- Deviations: the brief's stage list gains `register` (split from screen so a failed screen resumes without re-running
  add-alpha) and `match` (gross matching between run and verify). The driver commits the files a stage writes (exactly
  those paths) because the bounded runner refuses a dirty code pathspec; commit messages carry no trailer (tool commits).
  The replacing-wave pool-only marginal (PM6-8 (i), PM7-32) is applied by code in register (the integrator's X-2 / X-4
  hand fix). The mechanics reader reads the NAV's accounting checks from the summary's `accounting_checks` block
  (`strategy_nav_replay.cpp` risk_json); the book reader's gross of cost is net + annualised |trade cost|, |borrow|,
  |long financing| (the integrator's cellstats arithmetic: 4.64 + .74 + .34 + .20 = 5.92 for X-2).
- Cross-lane edits: `scripts/research_cycle.py` (3 dispatch blocks), `scripts/research_add_alpha.py` (`--save-plan`).
- Risks: (a) not run on real data: the first real wave should be run stage by stage (`--until`) with root reading each
  receipt; (b) the seal scan stops on any date token at or after 2024-01-01 in a run log, so a tool that prints today's
  date would stop the wave (none did in X batch 1, per the log's scans); (c) the budget counts admission lines by cycle
  prefix, and a re-screen-only wave's lines count like additions (as the ledger records them); (d) `max_rho_member` in
  carried marginal rows may name a dropped string (labelled in the result); (e) the capacity 4x row is read as the row
  of multiple 4 (the primary book's when several): confirm on the first real `capacity_curve.csv`; (f) the readers and
  the record stage use `nav_summ` / `backtest_integrity` as they are; a change there changes what the wave reads.

## 6. Tests

New (31): `scripts/tests/test_research_wave.py` (15), `test_wave_queue.py` (4), `test_wave_scoreboard.py` (3),
`test_wave_speed.py` (4), `atx-engine/tools/test_stage_chain.py` (5).

- `scripts/tests` (whole, no `ATX_EQUITY_BIN`): **273 passed, 4 skipped, 0 failed** (424.7 s) = the base's 247 passed /
  4 skipped + the 26 new; the base run here before any change: 247 passed, 4 skipped.
- `atx-engine/tools/test_stage_chain.py` + the four new scripts files after the last edit: **31 passed**.
