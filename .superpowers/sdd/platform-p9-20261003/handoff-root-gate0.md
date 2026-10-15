# Handoff: root GATE-0 (RESUME 11: the gate's step-0 slips, pulled forward while SQL4's review runs), 2026-10-15

Root `C:/atx-wt/pool-2`, branch `feat/platform-p9-20261003`. Started from `handoff-root-m6.md` (code head `d51d011e`,
docs head `475f5d8f`, next free tag `p9-3e`, known-red set EMPTY). Rows: `integration-log.md` "GATE-0 (RESUME 11)";
evidence and tools `gate0-logs/` (tools in `gate0-logs/tools/`).

- **Status: DONE.** Step 0 docs committed; STORE-M1 slip and STORE-S S-1 landed in one code commit; IC-S: no edit
  (the mechanism is not a byte freeze; G-P5 PARTIAL); PY-S S-6 paths and command lines below (no run).
- **Code head: `7320e1b9`.** Commits of the group, in order:
  - `22f99018` docs: the five `wave-review-<area>.md` files and `progress.md` as the PM left it (`git add -f`).
  - `7320e1b9` fix(store): STORE-M1 slip + STORE-S S-1 test.
- **Docs head:** the records commit that adds this file (`git log -1 --format=%h -- <this file>`). It carries this
  file, the GATE-0 rows of `integration-log.md`, `gate0-logs/`, and `progress.md` as it stands (the PM's four lines
  added after `22f99018`: SQL4 APPROVE `44a76937`, SQL4-S, SQL4-ADD, the M6b dispatch; no ledger line edited by root).
- **No build** (Python only). **Next free root tag: `p9-3e`** (unchanged).
- **Trial ledger** `build-equity/trials.jsonl`: 133 lines, sha256 prefix `27e40f9f` at start and end (0 trials).
- **SQL3-2** holds: `build-equity/research-store/` absent at start, before and after every store-suite, hook and
  fixture-chain run, and at the end; every test store, catalog and basetemp sat under the session scratchpad
  `root-gate0/` (TMP / TEMP pointed there too). No `index.sqlite` under `build-equity/` (depth 4).
- **Exe used:** `build-equity/bin/atx-research-store.exe` p9-2r, SHA-256
  `d8a500d84dc2a25130c69b226d8f5deec47b8dabb6256522c83c00764d3d9d65` = `mega-p9-2r-receipt.json`, at start and end
  (`gate0-logs/exe-sha-start.txt`, `exe-sha-end.txt`). No other exe run.
- **Known-red set: EMPTY.** No new red.

## What changed (file:line, at `7320e1b9`)

| file:line | change | ruling |
|---|---|---|
| `atx-engine/tools/test_research_store_classes.py:26` | `PY_DIRS = ("scripts", "atx-engine/tools", "atx-impl/tools", "atx-impl/strategies")` | STORE-M1 |
| `atx-engine/tools/test_research_store_classes.py:3-7` | docstring reflowed to name `atx-impl/strategies/` (no other text change) | STORE-M1 |
| `atx-engine/schemas/research_store/classes.json:7` | new class row `library-spec-receipt`, right after `run-start` (below) | STORE-M1 (SQL2-CLS form) |
| `scripts/tests/test_research_store_hook.py:467-480` | `test_an_unexpected_error_is_one_warning` (14 lines; header lines :22-23) | STORE-S S-1 |

Path note: the dispatch named the guard `scripts/tests/test_research_store_classes.py:26`; the guard lives at
`atx-engine/tools/test_research_store_classes.py:26` (as `wave-review-store.md` M-1 says). Same line, same edit.

## The classes guard: before / after (each its own invocation, seed 0)

| state | `PY_DIRS` | pytest | scan (files / distinct literals / unregistered) | log |
|---|---|---|---|---|
| baseline (as `d51d011e`) | 3 dirs | **5 passed**, 97 subtests | 930 / 132 / 0 | `pyt-classes-guard-baseline-q-s0.log`, `guard-scan-before-current-dirs.txt` |
| before (dirs extended, no row) | 4 dirs | **1 failed**, 4 passed: only `atx.library-spec-receipt/v1` in `atx-impl/strategies/generate_from_spec.py` | 932 / 135 / 1 | `pyt-classes-guard-before-s0.log`, `guard-scan-before-extended-dirs.txt` |
| after (row added) | 4 dirs | **5 passed**, 98 subtests (one more class) | 932 / 135 / 0 | `pyt-classes-guard-after-s0.log`, `guard-scan-after.txt` |

The scan counts come from `gate0-logs/tools/guard_scan.py` (the guard module's own `scanned_files` / `scan` /
`unregistered`, imported read-only).

## Class rows added (literal -> class; precedent)

| literal | class | precedent stated |
|---|---|---|
| `atx.library-spec-receipt/v1` (writer `atx-impl/strategies/generate_from_spec.py:39`, `<out>/receipt.json` at `:174`) | **`library-spec-receipt`** (new class; label `I5`; globs `["**/receipt.json"]`; format json; ingest `artifact-only`; render null; stage 1; writer / pinned_by text name the generator and the pins the receipt holds) | the **run-receipt** row (a receipt: glob on the literal file name `**/receipt.json`, json, the schema keys the class, stage 1). Ingest `artifact-only` / render null because no ingester or render rule exists for this document: the precedent of every json output class without one (e.g. `fields-build`, which holds the receipt literal `atx.research-fields-receipt/v1`). Label `I5`: the sql-design inventory row whose writer is `generate_from_spec.py`. Placed after `run-start` (the store review's suggestion). The literal sits in one class only, so `shared_literal_problems` is unaffected. |

Not added: the three JSON-input literals the store review names (`atx.curated-corporate-action-evidence/v1`,
`atx.selected-stock-transition-basis/v1`, `atx.dsl-strategy-experiment/v1`). The extended guard does not flag them:
it scans `.py` and C++ files only, and these occur only in git-tracked JSON inputs under `atx-impl/strategies/`. They
still class as `other` (P10, the store review's open question 2).

Proof the row works in C++ (`gate0-logs/class-probe.txt`, `tools/class_probe.py`, a synthetic two-file tree in the
scratchpad): the p9-2r store exe classes a `receipt.json` of schema `atx.library-spec-receipt/v1` as
`library-spec-receipt` and one of `atx.bounded-research-run/v1` as `run-receipt`. Its pins are not extracted
(artifact-only), so G-P10 (a) still misses the library-spec receipts' pins (P10 with S-3 / S-4).

`classes.json` at `7320e1b9`: git blob `5a0ea524a431d3bd606261956981e4412a2b47dc`; working bytes (LF) SHA-256
`5bc27ba489f24a0d1c8c42c8c29927c8d1bde6bf63cd5993f285a5b084086557` (the gate's catalog record states the SHA by hand,
STORE-S S-4; re-hash at the gate head).

## Tests (seed as stated; `ATX_EQUITY_ROOT=C:/atx-wt/pool-2/build-equity`, `ATX_EQUITY_BIN=C:/atx-wt/pool-2/build-equity/bin`)

| run | result | log |
|---|---|---|
| store suite `atx-engine/tools/test_research_store{,_blind,_classes,_fixtures,_identity}.py`, seed 0 | **43 passed**, 137 subtests (`FixtureChain` ran the real exe) | `pyt-store-suite-s0.log` |
| the same, seed 1 | **43 passed**, 137 subtests | `pyt-store-suite-s1.log` |
| `scripts/tests/test_research_store_hook.py`, before the S-1 case | **13 passed** (real-exe case ran) | `pyt-hook-before-s1-s0.log` |
| the same, after the S-1 case | **14 passed** (real-exe case ran) | `pyt-hook-after-s1-s0.log` |
| SQL2-CLS fixture chain (`tools/fixture_chain.sh`, rel and abs root) | `catalog_digest d32f7655...4e69` both (unchanged), `files_seen 28 verified 25 declared 0 skipped 4`, `verify --pins` exit 0, identity 12 / 0 | `fixture-chain-storem1.txt` |
| teeth: the hook's catch-all re-raises (`tools/teeth_hook_catchall.py`, TEETH-RESTORE / TEETH-CRLF) | **caught**: 1 failed (`test_an_unexpected_error_is_one_warning` only), 13 passed; restore: raw SHA-256 `4daa7f42...` = the kept copy, `git hash-object` = HEAD blob `ffc532a7...` | `teeth-hook-catchall.txt`, `teeth-hook-catchall-pytest.log` |
| `scripts/tests/test_no_python_mirror.py` (own invocation; state check, no edit) | **7 passed**; "G-P5: 0 mirrored rule function(s) in 0 row(s) left for P9 lanes" | `pyt-mirror-guard-s0.log` |

Every pytest line (head, dirty paths, exit, ENV-1 count) is in `gate0-logs/pyt-exits.txt`. ENV-1: 0 hits.

## STORE-S S-1: the case

`test_an_unexpected_error_is_one_warning` plants the hook's on-switch with `catalog_on` (an empty catalog file),
lets the fake ingest exit 0, then lets the hook's REAL `check` open that empty file: `research_store.Store.open`
raises `StoreError` ("no store_info table"), which only the catch-all at `scripts/research_store_hook.py:158-159`
turns into the one warning. Asserts: `wave_queue.write` returns its path, the file's bytes equal a hook-off write,
exactly one `store: WARNING wave-candidate <rel>: StoreError: ...` line ending in the repair text. 14 lines < 20.

## IC-S: FROZEN_PAIRS outcome: NO EDIT

How `FROZEN_PAIRS` works (`scripts/tests/test_no_python_mirror.py:77-98`): a frozenset of (module, function name)
pairs. It pins **no bytes and no hash**. Its names form `RULE_SYMBOLS`, which the scan looks for in every non-test
module of `SCOPE` (methods included, `ast.walk`); every definition found must be an `ALLOWLIST` row (module, symbols,
rule, the deciding C++ path that must exist, and a retiring lane in `P9_LANES + ("P10",)`); the ALLOWLIST may only
shrink inside `FROZEN_PAIRS`. So adding a pair is a name-based mirror registration with a retirement lane, not a
freeze of the two sides' bytes. The condition of IC-S ("only pins their current bytes") does not hold.

In-memory probe (`gate0-logs/frozen-pairs-probe.txt`, `tools/frozen_pairs_probe.py`; module imported, no file edited):
- the pairs added to `FROZEN_PAIRS` alone: `test_no_new_python_mirror` FAILS (every candidate definition is unlisted);
- with matching `P10` ALLOWLIST rows too (the E2 `fed4d4cb` form): still FAILS on a name collision,
  `scripts/research_ledger.py: def ledger_n` (the cycle's wrapper of the same name); the other five tests pass.

**For the gate report (G-P5): "G-P5 PARTIAL: four pairs unlisted, P10 retirement".** The four Python / C++ pairs the
guard cannot see (`wave-review-ic.md` S-3; `wave-review-python.md` S-1), for the freeze report's P10 list beside the
guard's 8 P10 rows:

| pair | Python (module: functions) | C++ |
|---|---|---|
| ledger counting / chain | `atx-impl/tools/backtest_integrity.py`: `fold_head` :622, `chain_head` :628, `trial_counts` :781, `ledger_n` :810, `dsr_variance` :907 (callers `scripts/research_ledger.py:161` `ledger_n`, `scripts/research_common.py:76-81`, `scripts/cycle/phases.py:747-752`, `:868-870`) | `atx-engine/include/atx/engine/research/ledger/trial_counts.hpp`, `chain.hpp` (`src/research/ledger/trial_counts.cpp`, `chain.cpp`) |
| theme-tsmom-v1 schedule | `atx-impl/tools/composition_theme_tsmom.py`: `masses` :88, `theme_masses` :107, `sleeves` :119, `trailing` :141, `schedule` :160 | `atx-engine/src/research/composition/fit.cpp:142-246` |
| theme-resid-v1 order | `atx-impl/tools/composition_resid.py`: `registered_order` :122, `theme_order` :135 | `atx-engine/src/research/composition/fit.cpp` |
| mom-volman-v1 sleeve | `atx-impl/tools/composition_mom_volman.py`: `schedule` :77 (the sleeve via `composition_theme_tsmom.sleeves`, :93) | `atx-engine/src/research/composition/fit.cpp:247` |

Also P10 text: the stale CMake comment `atx-engine/CMakeLists.txt:498` ("chain_head, the one copy of the rule").

## PY-S S-6: the two N producers and the hand comparison (for the gate's scoreboard step; NOT run here)

- **Python `ledger_n`:** `atx-impl/tools/backtest_integrity.py:810` (`ledger_n(records, scored_in_ledger,
  kind="construction")`, over `trial_counts` :781). The wave preflight's and the scoreboard's N:
  `scripts/research_common.py:76-81` `ledger_view` (`n = ledger_n(records, True)`), called by
  `scripts/wave_stage_preflight.py:115` and `scripts/wave_scoreboard.py:58-64`, `:126` (scoreboard JSON `$.ledger.n`).
  The cycle's `summ.dsr_n "ledger+1"`: `scripts/research_ledger.py:161-175` (wrapper) via `scripts/cycle/phases.py:747-752`,
  `:787-791`, `:868-870`.
- **Eval verb N:** `atx-engine/src/research/eval/eval_report.cpp:130-141` (`house_v1_json`: `lg::ledger_n(records,
  in_ledger || history)` -> `$.cells[k].dsr.house_v1.n`, with `n_rule`), `:177-198` (`ledger_rows_json`:
  `$.cells[k].dsr.tot.v8.n` = `ledger_n(records, in_ledger)`), text line `:759-767` ("house_v1 (N=...)"); the C++ rule
  `atx-engine/src/research/ledger/trial_counts.cpp:210-219` (header `trial_counts.hpp:72`). `in_ledger` = `--trial
  LABEL=TID` with TID in `construction_ids` (`trial_counts.cpp:339-348`: lines whose `kind` is "construction";
  `eval_report.cpp:217`, `:578-584`). `nav_summ.py --dsr-ledger` stores the same row as `deflated_ledger`
  (`atx-impl/tools/nav_summ.py:1015-1031`).
- **Hand comparison** (Git Bash, cwd pool-2; reads N only, never a dsr / sr0 / variance field; the net series is
  synthetic, so no data is opened and the verb's output carries no research statistic):

```
cd C:/atx-wt/pool-2; PY="C:/Program Files/Python312/python.exe"; S=<gate scratch>/s6; mkdir -p "$S"
EVAL=<the eval exe of record: the gate's Release atx-research-eval.exe, named by its receipt (EXE-CUR)>
# 1. Python: the preflight / scoreboard N (scored), the unscored N, the window id, one ledgered construction trial id
PYTHONDONTWRITEBYTECODE=1 "$PY" -c "import sys; from pathlib import Path; sys.path[:0] = ['scripts', 'atx-impl/tools']; import research_common as rc, backtest_integrity as BI; p = Path('build-equity/trials.jsonl'); r, v = rc.ledger_view(p, p.as_posix()); tid = [x['trial_id'] for x in r if x.get('kind') == 'construction' and isinstance(x.get('trial_id'), str)][-1]; print('py n_scored', v['n'], 'n_unscored', BI.ledger_n(r, False), 'lines', v['lines'], 'head', v['head'][:16]); print('WIN', BI.window_id(), 'TID', tid)"
# 2. a synthetic net series (placeholder dates and values; N does not read them)
"$PY" -c "import sys; open(sys.argv[1], 'w').write('session_ns,net_return\n' + ''.join(f'{1262563200000000000 + k * 86400000000000},{0.0001 * (k % 7 - 3)!r}\n' for k in range(300)))" "$S/n-probe.csv"
# 3. the verb, unscored then scored (WIN and TID from step 1); stdout discarded
"$EVAL" --daily c0="$S/n-probe.csv" --ledger build-equity/trials.jsonl --ledger-missing refuse --window-id "$WIN" --out "$S/n-unscored.json" > /dev/null
"$EVAL" --daily c0="$S/n-probe.csv" --ledger build-equity/trials.jsonl --ledger-missing refuse --window-id "$WIN" --trial c0="$TID" --out "$S/n-scored.json" > /dev/null
# 4. N only, by JSON path
"$PY" -c "import json, sys; [print(p.rsplit('/', 1)[-1], 'house_v1.n', d['house_v1']['n'], 'tot.v8.n', d['tot']['v8']['n'], '|', d['house_v1']['n_rule']) for p in sys.argv[1:] for d in [json.load(open(p))['cells'][0]['dsr']]]" "$S/n-unscored.json" "$S/n-scored.json"
```

  "Equal" iff `n-unscored`: `house_v1.n` = `tot.v8.n` = py `n_unscored`, and `n-scored`: both = py `n_scored` (its
  `n_rule` must not say "+ 1"). Record "equal" or the difference (PY-S; a difference before the adoption print is a
  finding). Alternative source for the verb's N: any P9-B0 `summ.json` row's `deflated_ledger.n` (read by JSON path).

## What the gate (or M6b) must know

- **G-P10 (e): "landed"** for the merged tree: the extended guard is green over seven directories (STORE-M1). (a)
  stays PARTIAL (the library-spec receipts' pins are not extracted; S-3 / S-4); (d) PARTIAL as M6 states.
- **G-P5: PARTIAL** "four pairs unlisted, P10 retirement" (above); the guard itself is green (7, 0 P9 rows).
- **Step 7 catalog:** pass `--classes <root>/atx-engine/schemas/research_store/classes.json` and state its SHA-256 by
  hand (STORE-S S-4); the value above is at `7320e1b9`.
- **The whole `scripts/tests` at the gate** now has 14 hook cases (one more than M6's 13).
- **M6b (SQL4 `44a76937`, ledger "Task SQL4: APPROVE"; dispatched to this agent continued, after this hand-back):**
  SQL2-CLS for any new literal runs against the EXTENDED guard (seven directories); a `classes.json` conflict with the
  new `library-spec-receipt` row (`:7`) is a keep-both region; the fixture-chain digest `d32f7655...4e69` above is the
  pre-SQL4 value (SQL4-S S-1: binding the ledger head changes it by design; record the new value with its cause); the
  hook file then has 14 cases.
- **Tools of this group** (`gate0-logs/tools/`): `pyt.sh` (logs dir `gate0-logs`), `fixture_chain.sh`, `guard_scan.py
  REPO [--add DIR]`, `class_probe.py REPO SCRATCH`, `frozen_pairs_probe.py REPO`, `teeth_hook_catchall.py REPO
  BASETEMP LOG`.

## Kept and deleted outputs

- Created under pool-2 only: the two code edits, `gate0-logs/`, this file, the integration-log rows. Nothing under
  `build-equity/`.
- Scratch: the session scratchpad `root-gate0/` (basetemps, the fixture-chain and probe catalogs, TMP): deleted by
  literal name after the records commit.

## Uncommitted state at the end

- `r11-dispatch.md`: the PM's working-tree edits (the WAVE REVIEW / ROOT GATE sections), present at start; not in my
  commit list (the dispatch named only the five review files and the ledger), left as found.
- Five untracked `docs/plans/*.png` (not root's), left. Any ledger lines the PM adds after the records commit (the
  records commit carries the ledger as it stood then, including the PM's SQL4 APPROVE / SQL4-S / SQL4-ADD / M6b lines).
- `gate0-logs/.mypy_cache/` appeared during the group (an editor-side type checker on the tool files, not a command
  of mine); git-ignored, never added.

## Permission refusals

- None.

## Open questions for the PM (one line each)

- STORE-M1 named the three JSON-input literals, but the extended guard does not flag them (JSON is not scanned): confirm P10 (no rows in P9).
- `r11-dispatch.md` carries uncommitted PM edits: does the gate root commit them with its step-0 / records commit?
