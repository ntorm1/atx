# Task H-1 report: era shards (lane H1, pool-3)

Branch `feat/platform-v8-h1-20260929`. Status: **STOPPED by PM instruction before implementation.** No code was
written or changed. This commit holds only this report. The W0-1 copies used for reading were deleted (never
committed). Nothing was built and no data was run.

## STOPPED HERE

**Done:**
- Read the binding documents: lane rules, global constraints, the A-2, A-3, V-1, C-1 and G-0 reports, the W0-2
  runbook, platform review P-5 and the H-1 brief.
- Read the code H-1 extends: `research_cycle.py`, `research_tree.py`, `research_ledger.py`, `cycle_verdict.py`,
  `run_bounded_research.py`, `nav_summ.py`, `backtest_integrity.py`, `fit_composition_weights.py` (paths `fit`,
  `fit_prior`, `ensure_records`, `RoleManifest`, `WorkStore`, `factor_record`), the tiny_world fixture and the test
  harnesses (`test_research_cycle.py` fake tools, `test_nav_summ.write_nav`, `test_fit_composition_weights.Fixture`).
- Settled the design below and found three blockers (section "Findings").

**Remaining:** all of it. In order:
1. `atx-engine/tools/era_pool.py` and its tests.
2. The pooled path in the fitter and in nav_summ, and the ledger lines in `backtest_integrity`.
3. The `roles:` loop in research_cycle.
4. `era_data_audit.py`.
5. Tests and the two commits.

Estimate: one working session. The design below is complete enough to implement without re-reading.

## Findings (read before resuming; root should rule on 1)

1. **A frozen-weights read of E1/E2 is blocked in C++.** The OD-3 wording is "read once on the frozen book".
   - The IC runner binds a weights file to its TRAIN role: `strategy_ic_admission.cpp:491-494` requires
     `train_manifest_sha256 == --train-sha256`.
   - The runner's validation-only mode, the frozen-artifact path, refuses a validation role that starts before
     TRAIN ends: `strategy_ic_runner.cpp:602-603`, "overlapping/nonchronological roles".
   - So weights fitted on E3 cannot be applied to E1 or E2 without a C++ change. The Python-only tooling can
     deliver pooled re-admission instead: the frozen library and rules, with admission run on the pooled E1+E2
     factor returns. The fitter then writes one weights file per era role, each bound to that role. These files
     are honest, because the fit read every era.
   - Rebinding E3's weights to an era role would pass the check while defeating its purpose. I did not plan that.
   - Ruling needed: either "frozen book" means frozen rules (pooled re-admission; H-1 tooling covers it), or it
     means frozen weights, which needs a small runner change (a history-validation role, or an era binding). That
     change is outside H-1, which is Python only per P-5a.
2. **The ledger refuses era series.** `backtest_integrity.window_of` refuses any session before `TRAIN_BEGIN_NS`, so a
   2014-2019 series cannot be ledgered today. Era and pooled lines need an era window rule: every session must be
   before the TRAIN end, with the label `ERA <id>` or `POOL`. This is a cross-lane edit in V-1's file.
3. **The fitter drops era decisions today.** `fit_prior` builds `train_mask` from `FIT_BEGIN_NS` and `TRAIN_END_NS`,
   so E1 and E2 decisions would be masked out. The pooled path needs an explicit mask: every pooled decision, since
   each lies in its own era's scored window. Edit only `fit()` and `fit_prior()`, not the constants or `RoleManifest`:
   those are the lines W0-1 rewrites, so touching them would conflict at merge.

## Design (as settled; to implement)

### Shared module `atx-engine/tools/era_pool.py` (generic, pure, imported by all consumers)

Standard library at import; numpy only inside the array functions, so `research_cycle` stays light.
- `PoolError(ValueError)`.
- `check_windows(windows=[(id, begin_ns, end_ns)], train_begin_ns, train_end_ns, seal_ns)` refuses:
  - a bad or duplicate id (`[A-Za-z0-9_]+`, since the id goes into file names);
  - an empty window, or `end > seal`;
  - a window that straddles the TRAIN begin (each era is either history before TRAIN or inside TRAIN; this is how
    "no overlap with E3" holds even when E3 is not listed);
  - a TRAIN era that ends after the TRAIN end;
  - eras out of date order, or overlapping.

  A warm-up before `begin` is never checked, so it may reach into the previous era.
- `segment_starts([(id, sessions)])`: the start row of each segment. Refuses an empty era, sessions that are not
  strictly increasing, and an era whose first session is not after the previous era's last.
- `pool_rows([(id, columnar dict)], key="session_ns")` pools the daily CSV dicts in the same columns and order. It
  concatenates, and adds `SEGMENTS: [row starts]`.
- `pool_matrix([(id, sessions, K x T_e)])` returns `(sessions, K x T, starts)`.
- `weighted_mean(values, weights)` returns `values[0]` exactly for one era. The pooled tau uses it, weighting each
  era's tau by its transitions (`decisions - 1`), because each era deploys afresh.
- `pooled_sha256(parts)` returns `parts[0]` for one era (a one-era pool is the era: same trial_id). Otherwise it is
  the SHA-256 of canonical `{"schema": "atx.era-pool/v1", "parts": [...]}`.
- `pool_label(dirs) = "pool:" + ",".join(dirs)`.
- `era_weights_name(id) = "composition_weights.<id>.json"`.

### Spec schema additions (`scripts/research_roles.py`, pure; research_cycle keeps `RolesCycle`-free imports)

```json
"roles": [{"id": "E1", "dir": "build-equity/era-2014-2016-lo1", "manifest_sha256": null,
           "begin": "2014-01-01", "end": "2017-01-01", "universe": "linked-operating-v1",
           "fields_dir": "build-equity/era-2014-2016-lo1-fields-v9", "fields_manifest_sha256": null}, ...]
```
- `inputs.role` is absent when `roles` is given.
- `lock` fills the null role and fields pins.
- Window checks:
  - `check_windows` runs at load time, with TRAIN and the seal taken from `research_window.current()`. That
    function recomputes from the JSON, so the test-harness `bind` cannot move it.
  - When the manifest is readable, each role's `score_start_ns` and `score_end_ns` must lie inside `[begin, end)`.
- One role: the Cycle of the derived single-role spec. `inputs.role` comes from the entry, and `fields` is as built
  when `fields_dir` is given. The `--lines-only` plan lines and phase lines are unchanged. Every existing spec has no
  `roles` and takes today's path untouched.
- Two or more roles:
  - `ref`, `card`, `marginal`, `monitor`, `compare`, `fields.check`, `summ.cells`, an as-built
    `fields.manifest_sha256` and `--reuse-fields` are refused. These are single-role phases.
  - Per-era phases: fields, u, w and nav, in role-keyed outputs (`<name>-<id>[-suffix]`). The candidate cache and
    fit work stay shared: they are content-keyed by role SHA, per A-3 and C-1.
  - Shared phases: check (on the anchor's fields), fit (pooled), gate and summ (pooled). The anchor is the last
    role, E3 when present.
- `Cycle` changes, all identity-preserving when `role_key` is None:
  - `out(name, keyed=True)`; the cache, the work dir and the fit output use `keyed=False`;
  - `weights_file` in the w step;
  - `runner()` adds `--role-id ID`;
  - `fit_step(pool=(argv, binds))` inserts, before `--output`: `--era-id <anchor>` and one
    `--era ID ROLE ROLE_SHA ORIENT ORIENT_SHA SUMMARY SUMMARY_SHA` per other era;
  - `summ_step(pool=[(id, nav)])` appends `--pool NAV... --pool-ids E1,E2,...` and one `--weights` per era file.
    It resolves the ledger N with the pooled trial_id;
  - `Step.role` and `Step.cycle` (the owning era, used by `fields_check`);
  - `step_key = phase[:role]` for timings, messages, plan and status lines, and `cycle_verdict` rows;
  - `--stop-after` stops after the last step of that phase.
- `RolesCycle` needs no import of research_cycle. It builds its steps from the era Cycles' step lists, phase-major:
  fields, check, u, fit, gate, w, nav, summ.
- `run_bounded_research.py --role-id ID` writes `role_id` into `start.json` and `receipt.json`, only when given.

### nav_summ `--pool DIR... [--pool-ids ID,...] [--pool-reference DIR...]`

- Segment-aware `return_mask`, deployment and tau rows, and post-ramp, through `SEGMENTS`. Without the key the
  arithmetic is unchanged, and the one-segment post-ramp keeps the `[RAMP_ROWS:]` slice. The legacy blob tests pin
  byte identity.
- Eras must share one scenario and one rule. The pooled row uses the pooled daily dict, and the per-era rows are
  reported beside it in `row["pool"]["eras"]`.
- A one-era pool carries the era's summary fields: pooled == single, apart from `dir` and the `pool` block.
  Several eras recompute net and gross Sharpe with the nav_summ definition, report `hac_t` null, and report
  `held_share` null.
- With fewer than 3 sessions in common with the reference, `paired` is skipped with a note (pooled rows only).

### Ledger (backtest_integrity, cross-lane)

- `ledger_record(..., era=)`.
- `window_of(sessions, era=)` checks the upper bound only.
- `ledger_pool_records(...)`:
  - Two or more eras: one line per era, with `era_of` = the pooled trial_id and `era = {id, role_sha256}`, which
    adds 0 trials. Plus one pooled line: `series.pool[]`, `eras[] {id, role_sha256, cell, trial_id,
    series_sha256}`, window `POOL`, `s2_net_sr` = the pooled Sharpe. The pooled line adds 1 trial: "an era shard of
    a registered cell is one trial in total".
  - One era: a single line.
- `trial_counts`, `ledger_counts` (with an "era shard line(s)" note only when present), `dsr_variance` and
  `ledger_net_series` skip era lines.
- `research_ledger.cells` skips era and pooled lines (not grid dirs). `ledger_n(..., pool_dirs=)` matches the pooled
  trial_id.

### Fitter `--era ... --era-id ID` (prior screens only; `ew-theme-aim-v1` and v3/none screens refused)

- Each era gets its own `RoleManifest`, orientations, `CacheLayout`, entries and `ensure_records`, in its own
  role-keyed `WorkStore`.
- The era windows (first scored session to the day after the last) are checked with `check_windows`.
- `pool_matrix` builds `f_unsigned` and `f_theta_unsigned`. The pooled records are:
  - context digest `pooled_sha256`;
  - the refused decisions, tagged by era;
  - tau from `weighted_mean`.
- `fit_prior(pool=...)` uses:
  - an all-True `train_mask`;
  - `rules.train_window_ns` = the era windows;
  - the `admission.pool` and `provenance.pool` blocks;
  - one extra `composition_weights.<id>.json` per non-anchor era, which differs only in `train_manifest_sha256`.
- Without `--era`, the output bytes are unchanged.

### `atx-impl/tools/era_data_audit.py` (opens no return)

- Inputs: role manifests, the `present.u8` and `member.u8` masks (hash-verified), and the fields manifests'
  `coverage.per_year` counts. It computes a field payload's finite mask only when the manifest lacks coverage.
- It never opens:
  - `close.f64` or `raw_close.f64`;
  - any field whose name is a return token (e.g. `mkt_ret`);
  - the manifests' value statistics (`member_finite_mean/min/max`).

  A test must instrument file opens to prove this.
- Output:
  - coverage per field and year for 2014-2019;
  - the delisting share: `universe.delisting` terminations kept as members, and mask exits;
  - the first valid date per field and per library candidate (its DSL's fields, with lookback from K1 plan rows
    when given).

### Tests planned

- `atx-engine/tools/test_era_pool.py`: order, overlap and straddle refusals; one-era identities.
- `atx-impl/tools/test_nav_summ_pool.py`:
  - `test_pooled_summary_over_one_era_equals_single`;
  - the two-era version split at a session boundary (the single CSV carries the flat plus re-deployment rows at the
    split, so every return-row and tau statistic is equal; post-ramp is asserted per era);
  - overlap and out-of-order refusals, and refusal of a sealed era;
  - ledger lines per era plus one pooled line, N + 1.
- `atx-impl/tools/test_fit_composition_weights_pool.py`: pooled admission == `screen_v4` on the concatenated era
  factors; the per-era weights files; the refusals.
- `scripts/tests/test_research_cycle_roles.py`:
  - `test_each_era_has_receipt_and_ledger_line` (fake runner, real nav_summ);
  - one role plans the single-role lines;
  - overlap and sealed-era refusals;
  - lock fills the role pins;
  - the runner's `--role-id`.
- `test_era_data_audit.py` on tiny_world.

## Commands root would run under an owner ruling

None yet: nothing is implemented. After implementation, the report will list the E1 and E2 role and fields commands
(the W0-2 R2/R3/R7-R11 chain with `--start`/`--score-start`/`--end` for 2014-2016 and 2017-2019, each with 399
warm-up sessions), the audit, and the pooled read spec.

## Cross-lane edits

None in this commit.

## Open risks

- Finding 1 needs a ruling before any OD-3 read is planned.
- The design adds cross-lane edits to be listed on resumption: `backtest_integrity.py` (V-1),
  `research_ledger.py`, `cycle_verdict.py`, `run_bounded_research.py`, `research_cycle.py` (A-3/G-0).
