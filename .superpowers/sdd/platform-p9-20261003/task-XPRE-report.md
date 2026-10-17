# Task XPRE report: pre-registration of expansion X and of the mined campaign

Lane XPRE, worktree `C:/atx-wt/pool-7`, branch `feat/platform-v8-xpre-20261002`, base `3c6ae225`.

| item | commit | what |
|---|---|---|
| draft | `71a68d60` | `v8x-prereg.md` with open choices OC-1..OC-11 |
| B (P5) | `44a27d5d` | `nav_summ.py --dsr-total / --dsr-hand`, `atx-impl/tools/dsr_total.py`, `test_dsr_total.py` |
| A | `57157a05` | `v8x-prereg.md` with every open choice written in as ruled (PM7-6..PM7-14); 258 lines |
| C | `da3bb239` | this report (items A, B) |
| OD-3 draft | `186ce9c5` | `v8-od3-protocol.md` for the PM's ruling (PM7-28 outline) |
| OD-3 B (T1-T5) | `f68058e2` | history-read tooling and tests (section "OD-3" below) |
| OD-3 A | `846941d2` | `v8-od3-protocol.md` as ruled (PM7-29) plus the integrator runbook (section 5) |
| OD-3 C | this commit | this report's OD-3 section |

## A. The ruled pre-registration

`.superpowers/sdd/platform-v8-20260929/v8x-prereg.md` (258 lines). Every open choice of the draft is now ruled text
carrying its ruling number. Section 13 lists the rulings; "recommendation" no longer appears in the file.

Text that changed in substance:
- **Repairs (PM7-9).** Only defects the PM confirmed from XIMP's proof go to X-1. Any other repair moves to X-2 and
  takes one admission trial; it counts against X-2's cap of 8.
- **Leverage (PM7-11).** The gross limit is quoted verbatim from the ruling ("[.90, 1.05] x (2.0 / L_P) x the parent's
  matched gross ratio", written in the cell's plan before the run). The unlevered and levered books are reported side
  by side with maximum drawdown; the owner chooses which is deployed.
- **Campaign (PM7-12).** The campaign runs alone. If free physical memory is below the cap + 1 GiB, the integrator
  lowers the workers (never the budget) and stops if 1 worker does not fit. The runbook comment now says this.
- **Caps (PM7-13).** The roster and pool caps rise to 80 at integration 8. The IC-pass memory cap is re-probed before
  X-3 (P7).
- **X gate (PM7-8).** An X book that fails the gate is carried to v9 with its count.
- **P5** names the tool at `44a27d5d`.

## B. Total-count deflated Sharpe (precondition P5)

Interfaces as coded:
- `atx-impl/tools/dsr_total.py` is pure over ledger records and net moments; only `read_ledger` touches a file.
  - `read_ledger(path)` refuses a missing file (ValueError) and verifies the hash chain through `BI.ledger_read`.
  - `trial_breakdown(records)` returns the trials by kind, the `ledger_trials` sum and `campaign_registry`, with
    `n_tot = sum(trial_counts) + campaign_registry_count`.
  - `dsr_at(moments, n, V)` returns SR0 and DSR through `BI.expected_max_sr` and `BI.deflated_sharpe`. Each value is
    None when V is undefined, N < 2 or the moments are undefined.
  - `ledger_rows(moments, records, in_ledger, window_id)` returns the `tot` row (N_tot, + 1 when the book is not in
    the ledger), the `v8` row (`BI.ledger_n`, which equals the `--dsr-ledger` value) and V from `BI.dsr_variance`
    (ddof 1, number of cells in V).
  - `hand_row(moments, ledger, trial_id, window_id)` computes the same rows on the ledger prefix that ends at the
    book's own construction line, with that prefix's chain head. It refuses a book that is not ledgered.
- `nav_summ.py <dirs> --dsr-total LEDGER [--dsr-hand DIR ...]`:
  - For each listed dir it prints a `== total-count DSR` block (lines `DSR_tot` and `DSR_v8`, with N's parts, V, the
    cells in V, SR0, the moments and the chain head).
  - For each `--dsr-hand` dir it prints a `== DSR_hand` block.
  - The JSON gets the key `deflated_total` on each listed row; the `--dsr-hand` rows sit under `deflated_total.hand`.
  - Usage errors: `--dsr-hand` without `--dsr-total`; `--dsr-total` without dirs or with `--pool`.
  - Refused: a missing or broken ledger, and a `--dsr-hand` dir without `summary.json`, without its daily CSV or not
    ledgered.

Tests (`atx-impl/tools/test_dsr_total.py`, 8 cases, synthetic). The expected values come from an independent oracle:
numpy moments and `statistics.NormalDist` for Phi and PhiInv. The synthetic ledger holds legacy lines, a protocol line,
a window re-run, an invalid cell, admission lines, a campaign line with registry count 132 and mined admissions. The
cases check:
- N_tot 149 = admission 10 + construction 7 + campaign registry 132, with + 1 for an unledgered book;
- the v8 count equals the `--dsr-ledger` value;
- a campaign line moves N_tot by its registry count and nothing else;
- V uses ddof 1 (not ddof 0) and only cells on the window;
- DSR_hand on the 16-line prefix: N_tot 13, chain head equal to that of a truncated ledger;
- every refusal above;
- the flags add lines and one JSON key and change nothing else;
- the flag-absent identity against the blob, described next.

### How root verifies identity (at integration 8)

1. Tests: `"$PY" -m pytest -q -p no:cacheprovider atx-impl/tools/test_dsr_total.py atx-impl/tools/test_nav_summ.py
   atx-impl/tools/test_nav_summ_v8.py atx-impl/tools/test_nav_summ_pool.py atx-impl/tools/test_backtest_integrity.py
   atx-impl/tools/test_trial_ledger_rules.py atx-impl/tools/test_holdout_gate.py`. Here: 96 passed, 1 skipped (the
   skipped test is `test_legacy_n37_numbers_reproduced`, which runs only with `ATX_EQUITY_ROOT`).
2. `test_flag_absent_is_byte_identical_to_the_pre_x_nav_summ` loads `nav_summ.py` from git blob `ef8cbbec`. That is
   the file at `3c6ae225`, and also at root head `e770fc36`, unchanged since `0b855971`. For four argv sets
   (reference + paired; v8 + `--dsr-ledger` + `--ledger-n` + PSR + effective-N + PBO; `--effective-n LEDGER`;
   `--bundle` + `--bundle-json`), it requires byte-equal stdout and stderr, and every JSON field equal except
   `nav_summ_run`.
3. With `ATX_EQUITY_ROOT=<root>/build-equity`, `test_legacy_n37_numbers_reproduced` must reproduce
   `mega-nav-v71-summ-n37.json` byte for byte (except `nav_summ_run`) on the v7.1 cells.
4. Optional, on real ledgered cells already read. It adds 0 trials and prints only numbers already ledgered:
   - `git show ef8cbbec97ec0e5d7590b1b7e3c0d327c1b1fab3 > $TMP/nav_summ_old.py`;
   - run `PYTHONPATH=atx-impl/tools "$PY" $TMP/nav_summ_old.py <B0c nav> <last accepted nav> --protocol v8
     --dsr-ledger build-equity/trials.jsonl --json $TMP/old.json > $TMP/old.txt`;
   - run the same argv with `atx-impl/tools/nav_summ.py`, writing `new.json` and `new.txt`;
   - `old.txt` and `new.txt` must be byte-equal, and the JSONs must be equal after dropping `nav_summ_run`.
5. The only expected moves: `nav_summ_run.script_sha256` and `git_head` in every JSON that nav_summ writes after the
   merge. They are the file's own provenance.

Merge facts:
- `nav_summ.py` and the new `dsr_total.py` are tool scripts covered by PM5-21's freeze list, so they merge at
  integration 8, after V8-F.
- `backtest_integrity.py` is untouched. The mining branch's one-line change there (`MINED_MAX_BUDGET`) cannot
  conflict. The `build-equity` ledger paths are unchanged.

Other suites:
- `atx-impl/tools` whole: 592 passed, 2 skipped.
- `scripts/tests`: 17 failed, 173 passed, 4 skipped. The same 17 fail on the base with this change stashed
  (`test_research_spec.py`: `r3-aim-gain-gm.json` is missing from its spec list). PM7-4 fixes them in root.

## Deviations

- The draft named `backtest_integrity.total_trials`. The code is a separate module, `dsr_total.py`, so that
  `backtest_integrity.py` (which the mining branch edits) is not touched. The prereg text names the module.
- One extra flag, `--dsr-hand DIR` (repeatable). DSR_hand needs the H-F book, which is not the listed X-F0 book.
  The same rule gives V8-F's value at its own ledger state ("DSR up", section 4).
- An unledgered listed dir gets + 1 on both N_tot and N_c (ledger_n's convention). At the X gate X-F0 is ledgered, so
  no + 1 applies there.

## Cross-lane edits

None. `nav_summ.py` is not in another X lane's brief.

## Open risks

- PM7-11's "the parent's matched gross ratio" is not defined in any text I read. The prereg quotes it verbatim; the
  leverage cell's plan must define it before the run.
- N_tot counts every legacy kind in the ledger (composition, universe and data lines, if present). The printed parts
  show them.
- With the base roster near 60, X-3 depends on PM7-13 (caps 80) landing at integration 8.

## OD-3: the history-read protocol and its tooling

The protocol is `.superpowers/sdd/platform-v8-20260929/v8-od3-protocol.md`. It records the 12 PM7-29 choices as ruled
text, with no "recommendation" left. Choice 11 adds the PM's condition in its strictest reading: data builds and data
checks (B1-B8, P1-P7) may run during X, but no era u, w or NAV pass runs before the last X cell is ledgered. Each of
those passes writes an IC or return number of the history. Section 5 is the integrator's runbook. It covers:
- per-era projection, base, scan and repair, lo3 and lo3-dlret roles;
- fields v10 argv, diffed against the TRAIN v10 receipt;
- the audit;
- the od3 spec changes;
- lock and plan;
- the R0 protocol line, R1 runs to NAV, the R2 mechanics-only read, R3 summ, the R4 history-only report and the R5
  scaled-V DSR;
- the stop conditions.

A smoke plan of an od3-shaped spec on the test fixture produced the pins, the phases (fit = carry), the per-era label
pairs and the summ argv shown in the runbook.

### Tooling (`f68058e2`), every flag absent byte-identical

| gap | change | files |
|---|---|---|
| T1 | `fit.carry {weights, admission}` in a roles spec; RolesCycle verifies both pins and that W was fitted on the anchor role, and replaces the pooled fit with `carry_weights.py`, which publishes W's bytes as `composition_weights.json`, `composition_weights.<id>.json` rebound per era with `carried_from`, `admission.json` and `carry.json` (all or nothing); `lock` fills the pins; `make_cycle` refuses `fit.carry` without roles | `atx-impl/tools/carry_weights.py` (new), `scripts/research_roles.py`, `scripts/research_cycle.py` (3 lines in `make_cycle`) |
| T2 | `nav_summ --pool-one-sided`: the pooled row's paired block adds `lw.p_one_sided` (same resamples) and a `verdict` (dSR > 0 and p < `--bundle-alpha`), plus one printed line | `atx-impl/tools/nav_summ.py` |
| T3 | `nav_summ --dsr-total` now accepts `--pool`: the pooled row gets `deflated_total` by its pooled trial_id; a pool that begins before TRAIN adds no trial (`dsr_total.ledger_rows(..., history=True)`, E-41) | `atx-impl/tools/nav_summ.py`, `atx-impl/tools/dsr_total.py` |
| T4 | `roles[].label_dir` (+ `label_manifest_sha256`): that era's `inputs.label_role`; per-era pins; `lock` fills them; refused together with a spec-level `inputs.label_role`, and a spec-level `inputs.label_role` is refused with two or more roles | `scripts/research_roles.py` |
| T5 | test only: a pool E1+E2+E3 whose E3 is the ledgered TRAIN cell appends E1, E2 era lines and the pooled line, skips E3's era line, and leaves trial_counts, V, ledger_n and the cells unchanged; one history read | `atx-impl/tools/test_od3_tooling.py` |

Tests:
- New: `atx-impl/tools/test_od3_tooling.py` (17) and `scripts/tests/test_research_cycle_od3.py` (14). They cover:
  - the carry tool's outputs and 10 refusals;
  - a full fake-pipeline roles run in which the real carry tool feeds each era's w pass with its own file and SHA;
  - the carry, label and lock refusals;
  - T2 against `paired_stats(one_sided=True)`;
  - T3 against an independent NormalDist oracle;
  - T5.
- Changed: `test_dsr_total.py::test_refusals` no longer expects `--dsr-total --pool` to be refused, because T3 removes
  that refusal.
- Run:
  - `atx-impl/tools` whole: 609 passed, 2 skipped.
  - Touched suites: 78 passed.
  - `scripts/tests` cycle, e2e and spec suites: the same 17 failures as with the change stashed (pre-existing, PM7-4).

**How root verifies identity** (after the merge, in root):
```bash
PY="C:/Program Files/Python312/python.exe"
"$PY" -m pytest -q -p no:cacheprovider atx-impl/tools/test_od3_tooling.py scripts/tests/test_research_cycle_od3.py \
  atx-impl/tools/test_dsr_total.py atx-impl/tools/test_nav_summ_pool.py scripts/tests/test_research_cycle_roles.py
```
- `test_flags_absent_nav_summ_is_identical_to_its_integration_8_blob` runs nav_summ against its blob `a680ad1f`
  (root's nav_summ at integration 8) on four pooled and dir argv lists. It compares stdout, stderr and JSON, and it
  also compares a ledgered pooled read, line by line, without the run provenance.
- `test_without_the_new_keys_plans_equal_the_integration_8_module` runs research_roles against its blob `313b23b3`. It
  compares plan, status, step argv and lock.
- `test_flag_absent_is_byte_identical_to_the_pre_x_nav_summ` (blob `ef8cbbec`) still passes.
- With exes: `ATX_EQUITY_BIN=... pytest scripts/tests/test_cycle_e2e.py`, as before.

**Merge notes.**
- Root's integration 8 changed `research_cycle.py` (the mine verb), `backtest_integrity.py` and
  `fit_composition_weights.py`.
- This work touches only `research_cycle.make_cycle`, in a hunk apart from root's, and neither of the other two files.
- `research_roles.py`, `nav_summ.py` and `dsr_total.py` are byte-equal between root and this branch's base, so their
  diffs apply cleanly.

### Concerns

- An X-F whose NAV reads `--risk-model` cannot be read: the roles loop has no per-era risk store. The runbook makes
  this a stop condition. It would be a new gap, solvable in Python (a per-era `risk_dir` like `label_dir`).
- The two-or-more-roles refusal of a spec-level `inputs.label_role` (T4) changes behaviour for a spec that previously
  planned. No committed spec has `roles`.
- The runbook's mechanics read (R2) reads `gross_leverage` and `held_names` from the daily CSV. These are mechanics
  columns, not returns, but they are NAV numbers. Under choice 11 R2 therefore waits for the last X cell, as does R1.
- The DSR leg of the gate is out of reach on any plausible history (section 2 power note). The read can move the
  p-value leg only, as PM7-29 states.
