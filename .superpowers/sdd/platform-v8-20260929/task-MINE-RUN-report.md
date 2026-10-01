# Task MINE-RUN report: OD-7 registration, spec template and runbook for the first mined campaign

Lane MINE-RUN (wave AG). Worktree `C:/atx-wt/pool-3`, branch `feat/platform-v8-minerun-20261001`, from `20e7bd19`.

Lane rules held:
- Python and docs only; no C++ edited or built.
- No real data. I read only manifest metadata (the field names and units of fields v9) and one receipt's wall seconds.
  No value, coverage, IC or return was read. Nothing dated 2024-01-01 or later was opened.
- pytest ran on synthetic inputs only. No subagents, no push.

| task | commit | what |
|---|---|---|
| 1 read the verb | (no artifact) | the verb as coded at `20e7bd19`, the ledger, the campaign lines, the rulings |
| 2 pre-registration draft | `0b0f04b8` | `docs/plans/2026-10-01-v9-mine-campaign-prereg.md` |
| 3 spec template and runbook | `25057740` | `scripts/specs/v9/mine-c1.json`, `docs/plans/2026-10-01-v9-mine-campaign-runbook.md`, plumbing `scripts/research_mine.py` and the `mine` dispatch in `research_cycle.py`, tests `scripts/tests/test_research_mine.py` |
| 4 this report | (next commit) | |

The commit message of `25057740` says 33 tests; the file has 32.

## What was built

### Pre-registration (`docs/plans/2026-10-01-v9-mine-campaign-prereg.md`)

The owner's one decision: "Grant `v9-mine-c1` as registered, with every row of section 1 at its recommended value."

The draft holds:
- section 1: the decisions table, 15 rows, each with a recommended value and its reason;
- section 2: the registration block that root copies into the v9 pre-registration (14 items);
- section 2a: the void rules;
- section 3: the parameters bound to lanes MINE-MEM and MINE-STAT, with today's values;
- section 4: the preconditions;
- section 5: what a grant does not allow;
- section 6: what was read to write it.

Derived numbers. All are calendar or formula arithmetic, not data:

| quantity | value |
|---|---|
| budget | 132 = 11 templates x 12 fields, equal to the capacity |
| Bonferroni z(132) | 3.5544 |
| raw discover t at today's F 1.55 | 5.51 |
| confirm, raw t | 3.10, and p_BY <= .10 |
| label rows | about 734 discover and 228 confirm (floors 504 and 200) |
| balance of the two gates | they bind at about the same t / sqrt(rows): .203 and .205 |

Today's memory model at C1's geometry, in MiB:

| members | 1 worker | 2 workers | 4 workers |
|---|---|---|---|
| 48 | 7,306 | 7,903 | 9,098 |
| 52 | 7,568 | 8,165 | 9,360 |
| 57 | 7,895 | 8,492 | 9,687 |
| 64 | 8,353 | 8,950 | 10,145 |

These come from a scratch replica of `mine_working_bytes`, checked to give exactly the MINE-10 row of 11,651,171,382 B.

### Spec template (`scripts/specs/v9/mine-c1.json`, schema `atx.mine-campaign-spec/v1`)

The template is the registration as data:
- inputs: role, fields and pool, with pins left null until the lock;
- `pool_source`: the source cell's combined signal, weights and w-pass summary;
- the 12 fields;
- windows given as `"{train_begin}"`, `"2023-01-01"` and `"{train_end}"`, resolved from `research_window.py` and never typed;
- budget 132;
- search: stage 2 off (`stage2_generations` 0), racing `"none"`, 4 workers;
- rule: min_names 1,000, min_dates 128, max_promotions 16;
- runner: 600 s, 8,192 MiB, 512 MiB free floor;
- `max_memory_mib`: `<fill:...>`;
- the registry inside the output dir; the ledger `build-equity/trials.jsonl`.

It is not runnable as committed. Four `requires` lines are open (OD-7; MINE-MEM and MINE-STAT built with the fixture
acceptance passed; the source cell; the field rule re-checked) and four values are left to fill.

### Plumbing (`scripts/research_mine.py`; `research_cycle.py mine <verb> SPEC [--root R]`)

- `lock [--write] [--relock]`: pins every input and `pool_source` file that exists. It lists a missing or to-fill path
  and leaves it null. A changed pin is refused unless `--relock`.
- `pool`: writes `atx.mine-pool/v1` beside `inputs.pool.path`.
  - Regressor `book` is the source cell's `train_combined.f64`.
  - Members are every candidate with a positive weight in `pool_source.weights`, each the v2 cache payload that the
    w-pass `summary.json` names (`roles[].candidate_cache.entries`), in that order.
  - Each sidecar must match: schema, candidate id, role SHA, axes and payload SHA.
  - Payloads are hard-linked (copied if a link is impossible) and re-hashed.
  - It refuses: another role; weights the combined signal was not blended with (`composition_weights_sha256`); a
    weighted member without an entry; a payload hash miss; more than 64 members; an existing directory.
- `probe [--workers N ...]`: runs the verb at `--max-memory-mib 64` for 4, 2 and 1 workers and parses
  `required_bytes=N` from the verb's own refusal, which comes before any payload (`run_mine`).
- `plan`: prints the header and the probe, run and ledger lines. The header holds the pins, the capacity, the budget,
  z, the windows, the caps, the open `requires` and the values to fill. It executes nothing.
- `run [--date D]`:
  1. refuses (exit 3) any of:
     - an open `requires`;
     - a value to fill;
     - an unlocked or changed pin;
     - an existing output or receipt dir;
     - a registry/head mismatch;
     - a dirty code pathspec;
     - a verb whose `--help` lacks an option;
  2. runs the verb through `run_bounded_research.py` and reads the receipt; anything other than completed with exit 0 is
     exit 4;
  3. appends the campaign line (`research_ledger.campaign_record` + `append`) before anything else is read;
  4. checks and prints the mechanics only, and a miss is exit 4:
     - the trial identity, as the generic sum of the statuses;
     - distinct = capacity for stage 1;
     - racing-rejected 0;
     - registry new records = distinct;
     - `hurdle.t` = z(budget);
     - the recipe pins, windows and field names;
     - the `status` / `reason` counts of `trials.csv`.
  It never prints a promotion, the admitted count or an IC.
- `wave --parent P --name N --parent-spec S`: prints one add-alpha line per admitted member: theme `mined`, tier `C+`,
  origin `mined`, prior sign 1 with `(-1 * (dsl))` for a discover sign of -1, and the ledger trial cited. With no
  member it prints "no wave".

### Tests (`scripts/tests/test_research_mine.py`, 32 cases, synthetic)

- The template is the registration: fills, requires, capacity = budget = 132, the windows equal `research_window`'s
  TRAIN bounds, the caps, z(132).
- The field list equals the rule applied to the committed registry (DSL tokens) and the fields v9 list in
  `base-lo1.json`.
- Constants mirror the C++ and the runner: `kTemplateWindows`, `kMaxMinePoolMembers`, the pool schema, the runner's
  600 s and 8,192 MiB.
- 14 validation refusals.
- The verb argv equals the verb's full CLI, parsed from `strategy_mine.cpp`'s `key == "--x"` lines and its usage text.
- Pool assembly passes the verb's manifest checks, mirrored in Python; 5 pool refusals.
- The probe parses `required_bytes`.
- `plan` output.
- `run`: refused until granted, filled and locked; refused on a dirty tree; ledgers first and then prints the mechanics,
  with no statistic in the log; a second run is refused; a mechanics miss stops it after the ledger; a failed receipt
  ledgers nothing; a stale verb is refused.
- `wave` lines.
- `lock`, `--relock`, and a pin mismatch.

## How root verifies

```bash
PY="C:/Program Files/Python312/python.exe"
"$PY" -m pytest -q -p no:cacheprovider scripts/tests/test_research_mine.py scripts/tests/test_research_ledger.py \
  scripts/tests/test_research_spec.py scripts/tests/test_research_cycle.py atx-impl/tools/test_trial_ledger_rules.py \
  atx-impl/tools/test_mine_overlap_factor.py
"$PY" scripts/research_cycle.py mine plan scripts/specs/v9/mine-c1.json   # metadata only: shows UNLOCKED / TO FILL
```

Here: 169 passed, 3 skipped (the live-root skips), and `test_research_mine.py` alone 32 passed.

- Nothing to build: no C++ was changed.
- Identity: no existing path changed. `research_cycle.py main` gains one branch on `argv[0] == "mine"`, which was a
  usage error before. The v8 specs, plans and every existing suite above are unchanged and pass. The golden digest
  `0x889874a3b9b29c55` is untouched.

## Deviations from the brief (with reasons)

1. **New module `scripts/research_mine.py`, outside the brief's list of new files.** `research_cycle.py` cannot carry a
   mine cell: its PHASES are a book cycle. Putting the module's 690 lines (its docstring included) into the shared 1,800-line file would
   collide with other lanes. So `research_cycle.py` gets only the dispatch (the add-alpha and `cache gc` pattern), and
   `research_spec.py` is unchanged (its `FILL` and `fills` are reused).
2. **`mine pool` and `mine wave` go beyond "spec template and runbook".** No tool wrote `atx.mine-pool/v1` (the verb's
   only pool format), and the mined members' registry form (embedded sign, theme, tier) is design work. Without both,
   root would design after a read.
3. **The registry lives inside the output dir** (`build-equity/mine-v9-c1/registry.atxtrg`). There is one directory per
   campaign. The verb creates the dir before it opens the registry, so a void run is renamed as one unit.
4. **The field list comes from an explicit rule over metadata.** The rule uses the registry DSL tokens and the units
   column of fields v9. A test pins it, so a later registry change surfaces as a failing rule check (precondition 4).

## Cross-lane edits

- `scripts/research_cycle.py`: two docstring lines and three dispatch lines (`if argv[:1] == ["mine"]: ...`). No other
  change.

## What in the verb blocks a real campaign

1. **Memory against the bounded runner (blocker until MINE-MEM lands).**
   - `run_bounded_research.py` accepts at most 8,192 MiB RSS and 600 s.
   - Today's model at C1's geometry is 7.3 to 10.1 GiB.
   - With the 512 MiB margin, C1 fits only at 1 worker and at most 53 members.
2. **Time against the runner's 600 s.**
   - [est, from a 3-year u-pass receipt] C1 takes about 260 s at 4 workers and about 1,050 s at 1.
   - So the campaign needs 2 to 4 workers, and therefore MINE-MEM's reduction.
   - If no worker count fits both, the runner's caps need an owner ruling.
3. **PM5-9's memory term.** The rho step over the whole above-hurdle list holds up to 132 signals (about 8.4 GiB at
   65.4 MiB each) unless it is streamed. MINE-STAT and MINE-MEM must put that term in the model.
4. **No pool producer in the verb.** This is resolved by `mine pool`, for the book composite only. K6's theme composites
   as regressors (D6's alternative) need a C++ exporter that does not exist.
5. **Not blockers, noted:**
   - No metadata-only mode exists; the probe uses the verb's pre-payload refusal, and its `required_bytes=` text must
     survive MINE-MEM.
   - A defect line cannot name a campaign line, so a void complete campaign is recorded by a ruling only.
   - `--min-names` sets both the IC floor and the PM5-8 rho floor (1,000 is conservative).
   - `kMineMaxProgramSlots` 8 is above the house budget's max_slots 7. That matters only if stage 2 is ever on.

## Merge checks for root (the code here reads what MINE-STAT and MINE-MEM change)

- `research_mine.max_budget()` reads `backtest_integrity.MINED_MAX_BUDGET`. If MINE-STAT renames it, change that one
  function.
- `mechanics()` checks `campaign.json hurdle.t` == z(budget), as coded: `mined_hurdle` is the Bonferroni z, read on
  t / F. If MINE-STAT redefines `hurdle.t`, change that one check and the fake campaign in the test.
- `test_verb_argv_is_the_verbs_full_cli` parses the verb's options. A new verb option from either lane makes it fail
  until `verb_argv` passes it, which is intended.
- New trial statuses (MINE-16) are handled generically.

## Open questions for the PM and owner

- D2: stage 1 only (recommended) or stage 2, given plan section 13's "no free-form search over all operators".
- D6: the book composite alone (buildable now) or K6's theme composites (stricter; needs a C++ exporter task).
- D15: the campaign after the v9 prior wave (LIB3), with the pool equal to that cell's book. This orders the v9 waves.
- D13: if no worker count fits both caps, rule on the runner's 600 s and 8,192 MiB maxima, or wait for MINE-MEM.
- The 2023 confirm year was read twice at book level before v8 (v8 rule 1). It is disclosed in the registration. It
  does not select the mined signals, but the regressor book was partly chosen with 2023 in view.
