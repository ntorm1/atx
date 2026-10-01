# v9 mined campaign C1 (`v9-mine-c1`): runbook for root

This runbook runs the campaign registered in `docs/plans/2026-10-01-v9-mine-campaign-prereg.md` (the registration) from
the spec `scripts/specs/v9/mine-c1.json`, through `research_cycle.py mine` (`scripts/research_mine.py`). Nothing in it
runs before the owner grants OD-7.

Root works in pool-2, at the merged head that holds MINE-MEM, MINE-STAT and MINE-RUN. Every step is serial, and no other
real-data run or build is active while step 8 runs (E-6).

```bash
PY="C:/Program Files/Python312/python.exe"
RC="$PY scripts/research_cycle.py"
SPEC=scripts/specs/v9/mine-c1.json
```

Exit codes of `$RC mine ...`: 0 done; 2 usage or spec refused; 3 pin, refusal or stale verb (nothing ran); 4 hard stop
(see step 9).

## 1. Fixture acceptance (must pass before anything touches the role)

```powershell
powershell -File scripts\research-build.ps1 -Tag v9-mine-1 -Targets "atx-impl-strategy-mine-tests,atx-engine-factory-tests,atx-equity-strategy-mine" -Preset equity
```

```bash
build-equity/bin/atx-impl-strategy-mine-tests.exe --gtest_filter='StrategyMine*:SignalFitness*:ResearchIc*:OpCatalogCfgTest.*'
build-equity/bin/atx-engine-factory-tests.exe
"$PY" -m pytest -q -p no:cacheprovider scripts/tests/test_research_mine.py scripts/tests/test_research_ledger.py \
  atx-impl/tools/test_trial_ledger_rules.py atx-impl/tools/test_mine_overlap_factor.py
```

Each of these must pass:
- every case of the first filter, including `StrategyMineCampaign.RulePinsOnTheTemplates` (C1's configuration: stage 2
  off, racing off, exact shortlist) and the MINE-MEM and MINE-STAT cases (memory model sum, rung-failed 0, PM5-8 and
  PM5-9 fixtures);
- the golden chain head `0x889874a3b9b29c55` at 1 and 4 workers (`SignalFitnessDefaults.*`,
  `NsgaSearch.ScalarRaw_ReproducesGoldenDigest`) and the whole factory suite;
- the pytest files, plus MINE-STAT's own pytest files.

A failure stops the runbook here. Record the build tag and the test counts in progress.md.

## 2. Fill the spec (metadata only)

The source cell is the last accepted cell at the campaign (prereg D15): V8-F after its freeze gate, or the v9 prior
wave's cell. Call its spec `CELL`. `$RC status CELL` names its w attempt `k`.

Edit `$SPEC`:
- `pool_source.combined.path`: `<CELL ic.w_output>-<k>/train_combined.json`.
- `pool_source.summary.path`: `<CELL ic.w_output>-<k>/summary.json`.
- `pool_source.weights.path`: `<CELL fit.output>/composition_weights.json`.
- `inputs.role.path` and `inputs.fields.path`: CELL's `inputs.role.path` and `<CELL fields.output>/manifest.json`. The
  spec shows lo1; if B0b won, point both at lo3.
- Fields rule (prereg item 4): run `"$PY" -m pytest -q -p no:cacheprovider scripts/tests/test_research_mine.py -k
  fields_are_the_rule` on the registry of record. If it fails because the book now reads one of the 12 fields, the
  assertion shows the rule's list. Set `fields` to that list and `budget` to 11 x its length, change prereg items 4
  and 5 to match in the same commit, and do this before step 3. The list can only shrink.

## 3. Lock the inputs that exist

```bash
$RC mine lock $SPEC --write
```

The command pins `inputs.role`, `inputs.fields` and the three `pool_source` files. It prints `missing (lock again once
built): inputs.pool`. Any other `missing` or `to fill` line means step 2 is not done.

## 4. Build the pool, then pin it

```bash
$RC mine pool $SPEC
$RC mine lock $SPEC --write
```

The pool step writes `build-equity/mine-pool-v9-c1/`:
- `manifest.json` (`atx.mine-pool/v1`);
- `book.f64`, the regressor;
- one `<member>.f64` per positive-weight member, at most 64.

Payloads are hard-linked; if a copy is needed, the copy costs about 69 MB per row on disk. The step refuses a combined
signal on another role, weights that the combined signal was not blended with, a weighted member without a v2 cache
entry, a payload that does not hash to its pin, or an existing pool dir. The second lock pins `inputs.pool`.

## 5. Probe the footprint (no payload is opened)

```bash
$RC mine probe $SPEC
```

The verb runs at `--max-memory-mib 64` once each for 4, 2 and 1 workers. Each run refuses before any payload and
prints `required_bytes`. Then:
- choose the largest worker count W whose required MiB is at most 7,680 (prereg D11, D12);
- set `search.workers` = W;
- set `max_memory_mib` = the required MiB at W, rounded up to a multiple of 64. `runner.max_rss_mib` stays 8,192.

If no worker count fits, STOP and report the three numbers to the PM. Do not drop members (D7), do not raise the
runner cap, and do not run.

## 6. Preconditions and commit

Delete each `requires` line of `$SPEC` only when it holds:
1. OD-7 granted (cite the ruling id);
2. step 1 passed;
3. step 2 done for the source cell;
4. the fields rule re-checked.

`$RC mine plan $SPEC` must then print no `# requires` and no `# fill` line. Commit before the run, because the runner
and `mine run` refuse a dirty code pathspec:

```bash
git add -f $SPEC docs/plans/2026-10-01-v9-mine-campaign-prereg.md && git commit -m "spec(mine): v9-mine-c1 locked; OD-7 granted (ruling <id>)"
```

## 7. Plan (executes nothing)

```bash
$RC mine plan $SPEC
```

Check the header before going on:
- every input and `pool_source` line reads `[locked, verified]`;
- `capacity 132 (templates 11 x 12, stage 2 off); budget 132`;
- `Bonferroni z 3.5544`;
- `discover [2020-01-01, 2023-01-01), confirm [2023-01-01, 2024-01-01)`;
- `--max-memory-mib` is the step-5 value, `runner 8192 MiB / 600 s`, and the registry is `new`.

The plan also prints the probe line, the bounded run line and the ledger line.

## 8. Run

Before the run, free memory must be at least `max_memory_mib` + 1,536 MiB: the runner stops the run below 512 MiB free.
Stop any idle session that holds memory.

```bash
$RC mine run $SPEC --date <YYYY-MM-DD>
```

`mine run` does the following, in order:
1. refuses (exit 3) an open `requires`, a `<fill:...>` value, an unlocked or different pin, an existing output or
   receipt dir, a dirty tree, or a built verb whose `--help` lacks an option;
2. runs the verb through `scripts/run_bounded_research.py` (receipt `build-equity/mine-v9-c1-run/`);
3. on a completed receipt, appends the campaign line to `build-equity/trials.jsonl` (`research_ledger.campaign_record`:
   the registry bytes against the chain head, the recipe against its SHA-256, the verb's `ledger_line.json` byte for
   byte);
4. prints and checks the mechanics only:
   - trial identity, distinct = 132, racing-rejected 0;
   - registry new records = distinct;
   - hurdle z = z(132);
   - recipe pins and windows = the spec, fields = the spec;
   - the counts of `trials.csv`'s `status` / `reason` columns.

It prints no promotion, no admitted count and no IC.

## 9. When `mine run` stops (exit 4)

| stop | meaning | action |
|---|---|---|
| `HARD-STOP: receipt ... outcome time-limit / rss-limit / system-memory-limit / process-error` | no `campaign.json`; no statistic was written | Do not open `stdout.log`. Rename `build-equity/mine-v9-c1` to `build-equity/mine-v9-c1.void-<k>` (its registry is inside) and `build-equity/mine-v9-c1-run` to `...-run.void-<k>`. Record a ruling in progress.md (blind re-run, prereg item 14). Remove the cause without reading anything: more workers if the probe allows, a code fix, or an owner ruling on the runner's 600 s cap. Then repeat steps 5-8. |
| `HARD-STOP: no receipt` | the runner refused at parse time | Read the runner's stderr in the message, fix it, and re-run. Nothing was written. |
| `HARD-STOP [ledger]` | the campaign completed but its line was refused | Read nothing else. The message names the cause (registry edited, recipe hash, line differs, a second line on the recipe). Report to the PM; the campaign is void if the cause is in the campaign (2a.1). |
| `HARD-STOP [mechanics]` | ledgered, but a count or constant differs from the registration | Read no promotion. Fix the check if the check is wrong; otherwise the campaign is void (2a.3). Report to the PM. |

## 10. Ledger checks (after a complete run)

```bash
"$PY" -c "import sys; sys.path.insert(0, 'atx-impl/tools'); import backtest_integrity as BI; p = 'build-equity/trials.jsonl'; r = BI.ledger_read(p); c = [x for x in r if x.get('kind') == 'mining-campaign']; print('chain head', BI.ledger_head(p)); print('campaign lines', len(c), 'registry count', BI.campaign_registry_count(r), 'last', c[-1]['campaign'], c[-1]['trial_id'], c[-1]['budget']); print('construction N', BI.ledger_n(r, True))"
$RC ledger-campaign --ledger build-equity/trials.jsonl --campaign build-equity/mine-v9-c1
```

Expected:
- one campaign line `v9-mine-c1` with budget 132 and registry count 132;
- the construction N unchanged from before the run (the line adds 0);
- the second command exits 2 with `shares trial_id`: a second line on the same campaign is refused.

Record the chain head.

## 11. Read the results, in this order

Each read prints only what it is for.

```bash
# (a) counts of the rule's steps
"$PY" -c "import json; c = json.load(open('build-equity/mine-v9-c1/campaign.json')); p = c['promotions']; print('shortlisted', len(p), 'rho pass', sum(x['rho_pass'] for x in p), 'confirm read', sum(x['confirm_read'] for x in p), 'confirm defined', sum(x['confirm_defined'] for x in p), 'admitted', c['admitted'])"
# (b) the promotion rows (f2, rho, confirm t, p, p_BY), then the admitted members
"$PY" -c "import json; print(json.dumps(json.load(open('build-equity/mine-v9-c1/campaign.json'))['promotions'], indent=1))"
"$PY" -c "import json; print(json.dumps(json.load(open('build-equity/mine-v9-c1/mined_members.json')), indent=1))"
# (c) last, diagnostic only: trials.csv numeric columns (select nothing)
```

Record in progress.md:
- the campaign line (trial id, registry count) and the ledger chain head;
- the mechanics lines;
- (a) and the admitted ids.

Add to the Appendix A block: `mined campaigns 1 (v9-mine-c1: budget 132, registry count <C>, admitted <a>)`.

## 12. The mined wave (only when at least one member was admitted)

Prereg item 11 and D14. Run it next, on the last accepted cell; no other wave is accepted in between.

1. Add the theme to `atx-impl/strategies/alphas/registry.json` (a hand edit, as R-2's E1). Append to `themes`:
   `"mined": "mined-v1 campaign members (OD-7): one theme for every mined member"`.
2. Print the add-alpha lines. `P` is the source cell's library id, e.g. `v81`.

   ```bash
   $RC mine wave $SPEC --parent P --name Pm1 --parent-spec CELL
   ```

   Each line has these fixed values:
   - `--theme mined --tier C+ --prior-sign 1 --origin mined`;
   - the citation names the ledger trial;
   - a member with sign -1 is registered as `(-1 * (<dsl>))`.
3. Run the printed lines in order. add-alpha validates each member through the exe's `--plan-only` rows (K1) and writes
   `scripts/specs/v8/lib-Pm1.json`. Lock it if add-alpha says so: `$RC lock scripts/specs/v8/lib-Pm1.json --write`.
   Register `libraries/Pm1.prereg.md` and commit.
4. Screen: `$RC run scripts/specs/v8/lib-Pm1.json --screen`. The gate requires `any`; a mined member that the gate does
   not admit with its sign leaves the wave. Each listed member is one admission trial.
5. Then run the cell: `$RC run scripts/specs/v8/lib-Pm1.json`. It is one construction cell (N + 1), accepted iff the
   paired S2 net dSR > 0 against its parent AND mechanics AND turnover not higher. A rejected wave is not retried.

If no member was admitted, `mine wave` prints `admitted no member`. Then there is no wave, no cell and no admission
trial. The campaign line stays.
