# Task G-0 report: one trial count N, one ledger append (lane G, task 1)

Lane G, pool-8, branch `feat/platform-v8-g-20260929`. Status: DONE. Python only; nothing built, no real data.

PM ruling (dispatch): the N that gates is the validation kit's defect-rule count (`backtest_integrity.trial_counts`;
invalid cells, window re-runs and protocol lines add 0; v8-prereg Appendix A rules 2 and 7). Both tools now read it
from one function, and the protocol line is written through nav_summ's chained append.

## What was built

| file | change |
|---|---|
| `atx-impl/tools/backtest_integrity.py` | new `ledger_n(records, scored_in_ledger, kind="construction") -> int`: sum of `trial_counts` over the `kind` lines + 1 when the scored cell has no line yet. A line without a `kind` (a layout `ledger_record` never writes; lane A's test fixtures use it) is read as a `kind` line. |
| `atx-impl/tools/nav_summ.py:550` | `ledger_dsr`: `n = BI.ledger_n(records, in_ledger)` (was `v["n"] + (0 if in_ledger else 1)`; same value on every valid ledger, since every line written by `ledger_record` has a kind). |
| `scripts/research_ledger.py` | `backtest_integrity()`: imports `atx-impl/tools/backtest_integrity.py` as the nav_summ shim imports the moved tools (that directory on `sys.path`, module by name), on first use only, so `research_cycle.py`'s import surface is unchanged (numpy loads only when a ledger is read). `scored_trial_id(nav_dir)`: nav_summ's identity of a cell, `trial_id("construction", sha256(daily_<primary>.csv))`, or None while the NAV output does not exist. `ledger_n(path, cell, nav_dir)`: `BI.ledger_n` over every line read leniently; the scored cell is matched by trial_id once its NAV output exists (nav_summ's rule), by cell name before that (plan time). `append(path, rec)` now calls `BI.ledger_append(path, [rec], chain=True)` (the chain is verified first; the line carries `prev_sha256`) and returns the line as written (None when present). `main` catches the chain / schema refusal (exit 2). |
| `scripts/research_cycle.py` | minimal, listed below. |
| `scripts/tests/test_research_ledger.py` (new) | the two tests. |

`research_cycle.py` edits (line numbers after the change):

- 27-29: module docstring, the `summ.dsr_n` paragraph names the new N.
- 818: `prior, ledger_n = self.ledger_cells(n_out) if from_ledger else (None, None)`.
- 822: `--dsr-n` takes `self.dsr_n(ledger_n)`.
- 834-838: `dsr_n(self, ledger_n)` returns the ledger N for `"ledger+1"` (was `len(prior) + 1`).
- 884-887: `ledger_cells` returns `(prior, n)`; docstring.
- 895: `n = research_ledger.ledger_n(p, n_out, self.res.path(n_out))`.
- 901-904: an integer `dsr_n` must equal `n` (message wording unchanged: "lists K prior cells, so cross-cell N = n
  ... set summ.dsr_n to n"); returns `(prior, n)`.

The grid (`summ.cells_from_ledger`) is unchanged: every trial line's cell, protocol lines skipped.

## How root verifies

```bash
PY="C:/Program Files/Python312/python.exe"
"$PY" -m pytest -q -p no:cacheprovider scripts/tests/test_research_ledger.py scripts/tests/test_research_cycle.py \
    scripts/tests/test_cycle_e2e.py atx-impl/tools/test_nav_summ.py atx-impl/tools/test_backtest_integrity.py \
    atx-impl/tools/test_nav_summ_v8.py atx-impl/tools/test_holdout_gate.py
```

Here (W0-1's `research_window.py`, `research_window.json` and `engine_tools.py` copied in untracked from `880faac7`
and removed before commit, as lane EV did): 152 passed, 5 skipped (the live / n37 tests).

- `test_dsr_n_equals_trial_counts_with_defect_and_rerun_lines`: a chained ledger of six synthetic cells (plain,
  invalid, window re-run, blind re-run of the invalid one, a cell and its returns-based re-run) plus a protocol line;
  `trial_counts` = [1, 0, 0, 1, 1, 1, 0], N = 5 (the old line count gave 7). research_cycle's `--dsr-n`, nav_summ's
  `ledger_dsr` N (with nav_summ's own in-ledger rule) and `BI.ledger_n` agree at plan time, after the NAV output
  exists, after an identity run of the same series under another cell name is ledgered first (counted once by both;
  the old cell-name rule gave N + 1 here), and after nav_summ's own (no-op) append; an integer `dsr_n` of N + 1 stops
  with "set summ.dsr_n to 5".
- `test_protocol_line_is_chained_when_written`: on an unchained (v7) ledger the protocol line carries
  `prev_sha256` = SHA-256 of the line before it, in nav_summ's encoding; `ledger_read` verifies it; a second call adds
  nothing; a later nav_summ append (chain off) continues the chain; editing the line before the protocol line breaks
  the chain at the protocol line; on a broken chain the verb refuses (exit 2) and writes nothing.

Identity: every existing test passes unmodified. On a ledger whose trial lines are all kind `construction` without
defect or re-run fields (the v7 `build-equity/trials.jsonl` layout; root can check with
`"$PY" -c "import json,collections;print(collections.Counter(json.loads(l).get('kind') for l in open('build-equity/trials.jsonl') if l.strip()))"`),
the resolved N equals the old `len(prior) + 1`, so the v7.1 plan lines are unchanged (`test_v71_*` pass). Lines of
another kind (admission, composition) no longer add to this N, as they never did to nav_summ's `--dsr-ledger` N. The ledger files gain one field (`prev_sha256`) on protocol lines
written from now on; no existing line is rewritten.

## Deviations

- Tests live in a new file `scripts/tests/test_research_ledger.py` (imports `make_root` / `cycle_of` from
  `test_research_cycle.py` and `write_nav` from `atx-impl/tools/test_nav_summ.py`), so lane A's test file is untouched.
- The scored cell is matched as nav_summ matches it (series trial_id) once its NAV output exists. Without that, an
  identity run under a new name (`--suffix`) gives research_cycle N + 1 and nav_summ N.

## Cross-lane edits

- `atx-impl/tools/nav_summ.py` line 550 (lane EV / V-1): one line, value-preserving.
- `atx-impl/tools/backtest_integrity.py` (lane EV / V-1): the new function `ledger_n` only.
- `scripts/research_cycle.py`, `scripts/research_ledger.py` (lane A / A-3): as listed.

## Open risks

- Timing: N is resolved from the ledger as it stands when the summ step is built. A spec whose summ appends its own
  line as a window re-run or a defect (`--rerun-basis window`, `--ledger-defect` in `summ.extra`) gets N = trials + 1
  from research_cycle, while nav_summ's `--dsr-ledger` (read after its own append) prints N = trials. The same
  "+1 for a dir not in the ledger" rule applies in both; only the order of append and read differs.
- A ledger with an already broken chain now refuses `ledger-protocol` (it refused nav_summ appends before).
