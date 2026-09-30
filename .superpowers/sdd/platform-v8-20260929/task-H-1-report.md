# Task H-1 report: era shards (lane H1, pool-3)

Branch `feat/platform-v8-h1-20260929`. Status: **DONE (tooling; no era read, per Ruling E-2 / E-17).** Python only.
Nothing was built and no real data was run: every test uses synthetic data or the tiny_world fixture.

| commit | content |
|---|---|
| `1e66a7e1` | merge of root integration 3 (`feat/platform-v8-20260929` 41ac94fd), no conflicts |
| `1f3ded97` | part 1: `era_pool.py`, the pooled fitter, nav_summ `--pool`, the era/pool ledger lines |
| `fc802825` | part 2: the `roles:` loop, `--role-id`, `era_data_audit.py`, their tests |

Rulings applied: E-17 (finding 1: "frozen book" = frozen rules with pooled re-admission; no C++ change), approved
cross-lane edits for finding 2 (era window rule in `backtest_integrity`) and finding 3 (explicit pooled mask in the
fitter's `fit()` / `fit_prior()`).

## What was built (interfaces as coded)

### `atx-engine/tools/era_pool.py` (generic, standard library at import)
- `PoolError(ValueError)`; `SCHEMA = "atx.era-pool/v1"`; `SEGMENTS = "SEGMENTS"`.
- `check_windows([(id, begin_ns, end_ns)], train_begin_ns, train_end_ns, seal_ns)` refuses: a bad or duplicate id
  (`[A-Za-z0-9_]+`), an empty window, `end > seal` ("sealed"), a window straddling the TRAIN begin, a TRAIN era ending
  after the TRAIN end, eras out of date order, overlapping eras. A warm-up before `begin` is never checked.
- `segment_starts([(id, sessions)])`, `pool_rows([(id, columnar dict)], key="session_ns")` (adds `SEGMENTS`),
  `pool_matrix([(id, sessions, K x T_e)]) -> (sessions, K x T, starts)`, `weighted_mean(values, weights)` (one era:
  `values[0]`, no arithmetic), `pooled_sha256(parts)` (one era: `parts[0]`), `pool_label(dirs)`,
  `era_weights_name(id) = "composition_weights.<id>.json"`.

### Fitter `atx-impl/tools/fit_composition_weights.py`
- `--era ID ROLE ROLE_SHA ORIENT ORIENT_SHA SUMMARY SUMMARY_SHA` (repeatable, date order) and `--era-id ID` (the
  `--train` role = the anchor = the last era). `--era-id` alone is a one-era pool (a history role scored with the
  explicit mask). Refused: any screen but v4-prior-v1/v2, `ew-theme-aim-v1`, `--era` without `--era-id`, a bad or
  duplicate id, windows `check_windows` refuses, and every per-era pin (the era's orientations must bind its role).
- New functions: `pooled(args)`, `era_state`, `era_entry`, `era_window(role)` (first scored decision to one day after
  the last), `pool_eras(eras)` (pool_matrix of `f_unsigned` and `f_theta_unsigned`; tau = transitions-weighted mean;
  context digest = `pooled_sha256`; refused decisions tagged `era`; used rows min/max over eras; the `pool` block).
- `fit_prior(..., pool=None)`: all-True `train_mask`; `rules.train_window_ns` = the era windows; `admission.pool` and
  `provenance.pool`; `report_only.f_theta.window_ns` = the era windows; one `composition_weights.<id>.json` per
  non-anchor era, equal to `composition_weights.json` except `train_manifest_sha256`; summary `pool {anchor, eras,
  decisions}`. Each era keeps its own RoleManifest, CacheLayout, entries and role-keyed WorkStore under `--work-dir`.
- `fit()` edits: the refusals above and one branch before `if prior:`; the constants and `RoleManifest` are untouched.

### nav_summ `--pool DIR... [--pool-ids ID,...] [--pool-reference DIR...]`
- Segment-aware helpers: `segments`, `return_mask` (each era's first row), `deployment_rows` / `turnover_rows` (each
  era's deployment), `post_ramp_rows` (63 rows per era; a single CSV keeps the `[RAMP_ROWS:]` slice).
- `pooled_daily` (one scenario and one rule; `check_windows` on the NAV sessions), `analyse_pool`, `pool_weights`,
  `paired_or_note` (below 3 common sessions: `paired_note`, pooled rows only), `pool_ids`, `print_pool`, `load_era`.
- The pooled row is the last result (it joins V[SR_n] and the DSR as one trial). One era: the era's own `analyse` row
  plus `dir` and `pool`. Several: net and gross Sharpe recomputed over the pooled return rows, `hac_t` and
  `held_share` null, `pool.eras[]` = the eras' own rows with `id` and `role_sha256` (the NAV summary's).
- `--ledger` with `--pool`: `BI.ledger_pool_records`; `--psr`, `--effective-n dirs`, `--dsr-ledger` include the row.

### Ledger (`atx-impl/tools/backtest_integrity.py`, `scripts/research_ledger.py`)
- `window_of(sessions, era=None, pool=False)`: an era (`ERA <id>`) or pooled (`POOL`) series may begin before TRAIN;
  every session must still be before the TRAIN end. Without `era`/`pool`: unchanged.
- `ledger_record(..., era=None, era_of=None)`; `check_record_fields`, `ledger_record_fields` (shared by both line
  builders); `is_era_line`, `is_pool_line`, `pooled_trial_id(kind, shas)`; `era_pool()` loader (engine_tools naming).
- `ledger_pool_records(kind, eras, pooled_nets, pooled_sr, ...)`: one era = the era's own line (ERA window and `era`
  block only for a history series); two or more = one line per era (`era`, `era_of` = pooled trial_id, origin and
  window id only; adds 0) + one pooled line (`series.pool[]`, `series.sha256`, `eras[] {id, role_sha256, cell,
  trial_id, series_sha256}`, window POOL, `s2_net_sr` = pooled Sharpe, every v8 field incl. defect/re-run; adds 1).
- `trial_counts` (era lines 0), `ledger_counts` / `appendix_a` (era lines skipped; an "era shard line(s)" line only
  when present), `dsr_variance` (era and pooled lines out of V[SR], see deviations), `ledger_net_series` (skipped).
- `research_ledger.cells` skips era and pooled lines; `ledger_n(path, cell, nav_dir=None, pool_dirs=None)` matches the
  pooled trial_id; `pool_label(dirs)`, `pooled_trial_id(nav_dirs)`.

### The `roles:` loop (`scripts/research_roles.py`, new; `research_cycle.py`, `cycle_verdict.py`)
- Spec: `roles: [{id, dir, begin, end, manifest_sha256, universe?, fields_dir?, fields_manifest_sha256?}]` in date
  order, `inputs.role` absent. Windows checked at load with `research_window.current()`; a readable manifest's
  `[score_start_ns, score_end_ns]` must lie inside `[begin, end]`. `lock` fills role and fields pins.
- `research_roles.roles_cycle(spec, res, **kw)`: one role = the plain `Cycle` of the derived spec (a history role adds
  the fitter's `--era-id` and nav_summ's `--pool`); two or more = `RolesCycle` (duck-typed Cycle; unknown attributes
  are the anchor's): per-era fields/u/w/nav (outputs `<name>-<id>`, receipts `--role-id`), anchor check, pooled fit,
  gate and pooled summ; steps phase-major (fields, check, u, fit, gate, w, nav, summ). Refused with two or more roles:
  ref, card, marginal, monitor, compare, fields.check, summ.cells, an as-built base fields.manifest_sha256,
  inputs.reuse_fields, `--reuse-fields`, `--keep-fields`, and ic without fit.
- `research_cycle.py` hooks (all identity-preserving when `role_key` is None): `Step.role` / `Step.cycle`;
  `Cycle(role_key=None)`, `out(name, keyed=True)` (cache, work dir and fit output `keyed=False`), `weights_name`,
  `fit_pool` (inserted before `--output`), `summ_pool` (extra `--weights`, `--pool ... --pool-ids`, ledger N by the
  pooled trial_id), `ERA_PHASES` (`--role-id` only on fields/u/w/nav runs), `step_key` in plan/status/run logs and
  timings, `fields_check(st.cycle or cycle)`, `--stop-after` after the last step of the phase, `lock_pin` (shared by
  both locks), `make_cycle(res, spec_path=..., **kw)`, and `sys.modules` aliasing under `__main__` (so the lazily
  imported research_roles raises main()'s CycleError). `marginal_step` is untouched.
- `cycle_verdict.step_key`; phase rows keyed by it; scoring blocks from the last NAV step.
- `scripts/run_bounded_research.py --role-id ID` (`[A-Za-z0-9_]+`): `role_id` in start.json and receipt.json, only
  when given.

### `atx-impl/tools/era_data_audit.py` (opens no return)
`--era ID ROLE_MANIFEST ROLE_SHA FIELDS_MANIFEST FIELDS_SHA` (repeatable) `[--library L --library-sha256 S] [--plan
K1.json] [--json OUT]`. Per era: coverage by field and year (manifest `coverage.per_year`, else counted), delisting
(terminations, kept as members and share, by cause; mask exits and the delisted share), first valid date per field and
per candidate (K1 `extra_fields` + `required_lookback` when given). Never opens close.f64 / raw_close.f64 or a field
with a return token; drops `member_finite_min/max/mean` and `delist_return(_if_performance)` / `with_return` on load;
refuses a role reaching the seal on its manifest before any payload is opened; fields must be bound to their role.

## Tests (pytest, synthetic / tiny_world only)

New (83): `atx-engine/tools/test_era_pool.py` 17, `atx-impl/tools/test_nav_summ_pool.py` 8,
`test_fit_composition_weights_pool.py` 16, `test_era_data_audit.py` 18, `scripts/tests/test_research_cycle_roles.py`
24 (includes `test_pooled_summary_over_one_era_equals_single`, `test_each_era_has_receipt_and_ledger_line`,
`test_e3_alone_is_the_single_role_cycle_byte_for_byte`, the refusals, lock, `--role-id`, the script path).
Full touched + dependent run at `fc802825` (the new files plus test_fit_composition_weights{,_store}, test_nav_summ{,_v8},
test_backtest_integrity, test_holdout_gate, test_book_monitor, test_alpha_report_card{,_store}, test_horizon_stats,
test_exposures_export, test_mega_report_pitch3, test_research_window, atx-impl/strategies, test_research_cycle,
test_cycle_e2e, test_research_ledger): **584 passed, 6 skipped** (the existing ATX_EQUITY_ROOT / ATX_EQUITY_BIN /
live-root gates), 221 s.

## How root verifies

```bash
PY="C:/Program Files/Python312/python.exe"
"$PY" -m pytest -q -p no:cacheprovider atx-engine/tools/test_era_pool.py atx-impl/tools/test_nav_summ_pool.py \
  atx-impl/tools/test_fit_composition_weights_pool.py atx-impl/tools/test_era_data_audit.py \
  scripts/tests/test_research_cycle_roles.py
# identity of the flag-off paths (unchanged suites):
"$PY" -m pytest -q -p no:cacheprovider atx-impl/tools/test_fit_composition_weights.py atx-impl/tools/test_nav_summ.py \
  atx-impl/tools/test_nav_summ_v8.py atx-impl/tools/test_backtest_integrity.py scripts/tests/test_research_cycle.py \
  scripts/tests/test_cycle_e2e.py scripts/tests/test_research_ledger.py
```
- Single-role plans: `test_plan_equals_v61_train_sh_dry_output_pin_for_pin` (v6.1 DRY fixture) passes unchanged.
  Lane check (scratch, not committed): `plan_lines`, `plan_lines(lines_only=True)` and `status_lines` of every
  committed spec (`scripts/specs/*.json`) x {default, `--suffix r9`, `--screen`} in hash-only mode (the v6.1 + v7.1 pin
  fixtures) with `research_cycle.py` / `research_ledger.py` / `cycle_verdict.py` of the merge commit `1e66a7e1` and of
  HEAD: 21 cases (9 plan, 12 refuse), output byte-identical including the refusal texts.
- E3 alone: `test_e3_alone_is_the_single_role_cycle_byte_for_byte` (plan lines, pin/phase lines, status lines, the run's
  calls and every output byte incl. the ledger). One-era pool: `test_pooled_summary_over_one_era_equals_single` (JSON
  row equal apart from `dir` and `pool`; ledger file byte-equal).
- Fitter without `--era`: the 95 existing tests pass; output bytes change only in `script_sha256` (as for any edit of
  the script; the tiny_world admission digest excludes `inputs`, so the e2e goldens stay valid).
- nav_summ without `--pool`: the pre-T41 / pre-v6 blob tests pass (text and JSON byte-identical).
- With exes (root): `ATX_EQUITY_BIN=... pytest scripts/tests/test_cycle_e2e.py` must still match the goldens.

## Commands root would run under an owner ruling OD-3 (not run; Ruling E-2 stands)

Chain = W0-2 runbook R1-R3, R7, R10 with era dates; `$PY`, `sha`, `$TH`, `$R4`, `$FE`, `$FEV`, `$F63` and the stage
pins as in `w0-2-runbook.md` section 2 (re-hash the stages first). Each era gets the 399-session warm-up of today's
geometry (E1 and E2: 756 scored + 399 = 1,155 dates, the 3-year role's shape). Seal: every end is 2020-01-01 or
earlier; `check_windows` refuses any era past the seal.

```bash
# R1' projection covering both eras and their warm-ups (TickerHistory3 must reach back to 2012-06; check first)
"$PY" scripts/run_bounded_research.py --seconds 300 --max-rss-mib 1100 --min-free-mib 512 \
  --output build-equity/era-projection-2012-2019-run --bind atx-engine/tools/prepare_recent_research.py -- \
  "$PY" atx-engine/tools/prepare_recent_research.py project --source $TH \
  --out C:/atx-wt/pool-2/build-equity/era-projection-2012-2019 --start 2012-06-01 --end 2020-01-01 \
  --memory-mib 768 --disk-mib 12288 --max-seconds 270
PROJE=$(sha build-equity/era-projection-2012-2019/manifest.json)
# R2' base roles (then check score_begin == 399; move --start by the difference if not)
for E in "E1 2012-06-01 2014-01-01 2017-01-01 2014-2016" "E2 2015-06-01 2017-01-01 2020-01-01 2017-2019"; do
  set -- $E
  "$PY" scripts/run_bounded_research.py --seconds 180 --max-rss-mib 1024 --min-free-mib 768 \
    --output build-equity/era-$5-base-v1-run --bind atx-engine/tools/prepare_recent_research.py \
    --bind build-equity/era-projection-2012-2019/manifest.json -- \
    "$PY" -B atx-engine/tools/prepare_recent_research.py role --cache build-equity/era-projection-2012-2019 \
    --out build-equity/era-$5-base-v1 --start $2 --score-start $3 --end $4 \
    --top-n 3000 --max-union 8000 --memory-mib 768 --disk-mib 1024 --max-output-mib 512 --max-seconds 120
done
# R3' factor-break scan, then repair with the sessions the scan lists (root reviews them; no expected set for 2014-2019)
"$PY" scripts/run_bounded_research.py --seconds 120 --max-rss-mib 1536 --min-free-mib 512 \
  --output build-equity/era-2014-2016-scan-run --bind atx-impl/tools/repair_role_factor_breaks.py -- \
  "$PY" -B atx-impl/tools/repair_role_factor_breaks.py --scan-only --role build-equity/era-2014-2016-base-v1 \
  --role-sha256 $(sha build-equity/era-2014-2016-base-v1/manifest.json)
#   ... repair --out build-equity/era-2014-2016-base --expect-sessions <scan list>; same for 2017-2019
# R7' lo1 roles (R6' grp cross-check optional, as in W0-2)
"$PY" scripts/run_bounded_research.py --seconds 180 --max-rss-mib 1536 --min-free-mib 512 \
  --output build-equity/era-2014-2016-lo1-run -- \
  "$PY" atx-engine/tools/prepare_recent_research.py role --universe linked-operating-v1 \
  --base-role build-equity/era-2014-2016-base --base-role-sha256 $(sha build-equity/era-2014-2016-base/manifest.json) \
  --identity-bridge build-equity/identity-bridge-r4-v2 --identity-bridge-sha256 $R4 \
  --sic-events $FE --sic-events-sha256 $FEV --out build-equity/era-2014-2016-lo1 --memory-mib 1024 --max-seconds 170
#   same for build-equity/era-2017-2019-lo1
# R10' fields v9 on each era role: the R10 argv with --role build-equity/era-2014-2016-lo1 --role-sha256 <its sha>
#   --output build-equity/era-2014-2016-lo1-fields-v9 (and the 2017-2019 pair); 600 s / 2,560 MiB as R10
# audit before any read (opens no return):
"$PY" atx-impl/tools/era_data_audit.py \
  --era E1 build-equity/era-2014-2016-lo1/manifest.json <sha> build-equity/era-2014-2016-lo1-fields-v9/manifest.json <sha> \
  --era E2 build-equity/era-2017-2019-lo1/manifest.json <sha> build-equity/era-2017-2019-lo1-fields-v9/manifest.json <sha> \
  --library atx-impl/strategies/<V8-F library>.json --library-sha256 <sha> --plan <K1 plan-only JSON> \
  --json build-equity/era-audit-v1.json
```

The pooled read spec (one trial): the V8-F spec with `inputs.role` replaced by
`"roles": [{"id": "E1", "dir": "build-equity/era-2014-2016-lo1", "begin": "2014-01-01", "end": "2017-01-01",
"universe": "linked-operating-v1", "fields_dir": "build-equity/era-2014-2016-lo1-fields-v9", ...}, {"id": "E2", ...
"2017-01-01" .. "2020-01-01"}]`, the fields section as built (fields_dir), no ref/card/marginal/monitor/compare/
cells, `summ.extra` += `--pool-reference <B0c N-E1> <B0c N-E2>`, then `research_cycle.py lock --write` and `run`. B0c's
era NAV dirs come from the same roles spec with B0c's construction, run without `--ledger` (it is the reference), so
the read ledgers one pooled line.

## Deviations from the design (with reasons)

- `dsr_variance` also leaves the pooled line out of V[SR] (the design named era lines only): it is scored on history
  eras, not on a research window's TRAIN; it still counts 1 in N.
- The era lines carry only `origin` and `window_id`; a defect or re-run names the pooled line (the trial) only.
- `--role-id` goes only on the per-era runs (fields, u, w, nav): the pooled fit and summ belong to no era.
- The audit streams each non-return field payload once (hash-verified) for the first valid date; coverage comes from
  the manifest when present and is counted in that same pass otherwise.
- Non-anchor eras' derived specs drop static_check, gate and summ (they run on the anchor), so no era computes a
  ledger N of its own.

## Cross-lane edits

- `atx-impl/tools/backtest_integrity.py` (V-1): era window rule, era/pool lines and skips (approved, E-17).
- `atx-impl/tools/fit_composition_weights.py` (C-1 / A): `fit()` refusals + pooled branch, `fit_prior(pool=)` (approved).
- `atx-impl/tools/nav_summ.py` (V-1): `--pool` (brief file).
- `scripts/research_cycle.py` (A-3 / G-0; A2 is editing `marginal_step`): the hooks listed above; `marginal_step` and
  the A-1 plan-rows test untouched.
- `scripts/research_ledger.py` (A-3), `scripts/cycle_verdict.py` (A-2), `scripts/run_bounded_research.py` (A-3).
- The research-window lines of the merge were taken from root (W0-1 form); no conflict arose.

## Open risks

- V8-F's composition must be a prior screen with ew-theme-v1 or ew-theme-v6 for the pooled fit (`ew-theme-aim-v1`
  is refused: its aim record reads TRAIN signal ranks with the TRAIN mask).
- A pooled fit stores each non-anchor era's records under `<fit-work>/<anchor role16>-<window>/<era role16>-<window>/`
  (the derived work root is keyed by the anchor); a later single-role fit of that era recomputes them (cache only).
- nav_summ needs the eras' NAV CSV headers in one column order (the NAV replay writes one layout).
- An era line's trial_id is its series' single-cell trial_id: if that exact series is ledgered later as a single
  cell, `ledger_append` skips it (deduplicated), so it would add no trial.
- TickerHistory3 coverage before 2014 (the E1 warm-up from mid-2012) is unverified; R1' must be checked before R2'.
