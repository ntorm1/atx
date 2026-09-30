# Report: task V-2 (hidden-block gate, tool only)

Lane EV, pool-8, branch `feat/platform-v8-ev-20260929`. Status: DONE. Built and tested on synthetic NAV files only;
never run on real data.

## What was built

`atx-impl/tools/holdout_gate.py --deploy MANIFEST --thresholds FILE --owner-ruling FILE`

- stdout, exit 0: exactly one line `{"pass_2024": <bool>, "pass_2025_onward": <bool>}`. No statistic, session count or
  file is printed or written.
- Refusal, exit 2: one reason on stderr naming inputs only. Checked first: the owner ruling (absent file or flag refuses
  with "no owner ruling"); then the ruling's pins; then thresholds and manifest; then the NAV summary (frozen-book
  weights SHA, scenario). All of this happens before the daily series is read (tests trap `load_daily_csv` to prove it).
- Failure after the series is opened, exit 3: a fixed message, "the NAV output could not be evaluated (no result)".
  Nothing from the exception is echoed.

Inputs (every key checked; an unknown key refuses):

| file | schema | keys |
|---|---|---|
| deploy manifest | `atx.holdout-deploy/v1` | `book`, `nav_dir` (a NAV replay of the frozen book over the hidden sessions; relative to the manifest), `scenario` (null = primary), `composition_weights_sha256` (must equal the NAV summary's) |
| thresholds | `atx.holdout-thresholds/v1` | `min_sessions` (int >= 2), `net_sharpe_min`; optional `max_drawdown_max` (0, 1], `net_return_min` |
| owner ruling | `atx.holdout-owner-ruling/v1` | `date`, `owner`, `text`, `deploy_manifest_sha256`, `thresholds_sha256` (SHA-256 of the two files' bytes), `blocks` = `["2024", "2025_onward"]` |

The ruling pins the bytes of the manifest and the thresholds, so neither can be changed (a threshold loosened, another
book named) after the owner signs.

Blocks: read from `research_window.json` `hidden` (W0-1): `read_twice_at_book_level` [2024-01-01, 2025-01-01) gives
`pass_2024`, `never_read` [2025-01-01, open) gives `pass_2025_onward`. No date is written in the tool. TRAIN sessions in
the file (a warm-up tail) never enter a bit.

A block passes when it has at least `min_sessions` return rows, all finite, a defined S2 net Sharpe (mean / sd ddof 1 x
sqrt 252, as nav_summ) of at least `net_sharpe_min`, and the optional drawdown and compounded-return bounds hold. Anything
else, an empty block included, is a fail (fail-closed).

The daily CSV is read with `nav_summ.load_daily_csv(..., allow_sealed=True)`; holdout_gate.py is the only caller that
passes it (V-1 made every other read refuse sealed sessions).

## Tests

`atx-impl/tools/test_holdout_gate.py` (22 tests, synthetic NAV dirs with calendar-day sessions from late 2023 into
2026): exactly two bits on stdout, empty stderr and no new file, in-process and in a fresh interpreter; blocks equal the
window's `hidden` dates and the 2024 block starts at `SEAL_NS`; refusal without a ruling (flag absent, file absent);
eight ruling defects (either SHA, blocks, schema, text, owner, date, unknown key); inputs edited after the ruling; seven
malformed thresholds or manifests; a NAV output of another book, an unknown scenario, a missing nav_dir; fail-closed
thresholds (too few sessions, absent 2025 block, drawdown, compounded return, zero volatility, NaN); an unreadable CSV
gives exit 3 with the fixed message only.

```
pytest atx-impl/tools/test_holdout_gate.py        -> 22 passed
```

## How root verifies

```bash
PY="C:/Program Files/Python312/python.exe"
# after W0-1 (lane W0E) is merged:
"$PY" -m pytest -q -p no:cacheprovider atx-impl/tools/test_holdout_gate.py
```

Do not run the tool on real data in v8 (OD-1). Identity: a new file; no existing output changes.

## Deviations from the brief

1. The three input schemas are defined here (the brief names the files, not their content). The owner signs a ruling
   that pins the SHA-256 of the manifest and the thresholds. The ruling must name both blocks.
2. The manifest points at a NAV output dir, not at a book to run: the gate evaluates, it never runs a replay.

## Cross-lane edits

None.

## Open risks

- Depends on W0-1 (`research_window.py`, `research_window.json` `hidden` block) through `backtest_integrity`. It refuses
  when the `hidden` block is absent or the blocks overlap.
- The bit names are fixed by the brief (`pass_2024`, `pass_2025_onward`); a later window change moves the block dates
  (read from the JSON) but not the names.
- The thresholds are absolute on the block's S2 net Sharpe (with optional drawdown and return bounds). A paired test
  against a baseline on the hidden blocks is not offered: it would need a second NAV run over the hidden sessions.
