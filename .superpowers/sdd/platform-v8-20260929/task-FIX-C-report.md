# Lane FIX-C report: Wave 1 review fixes, area C (research tooling, Python) plus Ruling E-33

Branch `feat/platform-v8-fixc-20260930` from `81abfa12`, worktree `C:/atx-wt/pool-3`. Status: DONE_WITH_CONCERNS
(the concerns are listed under "Open risks"; every finding in the brief is fixed and tested).

## Commits (one per finding, in brief order)

| finding | commit | what |
|---|---|---|
| C-1 | `337ed421` | verdict DSR from the ledger's cross-trial variance (`--dsr-ledger`); verdict refuses without it |
| C-2 | `9ddffedd` | `--protocol v8` + `--origin` on every v8 summ step; add-alpha children of a v8 parent inherit it |
| C-9, C-10 | `6aa72350` | overlap report: never identical on zero cells; NaN rules; W0-a class; relative diff |
| C-3 | `396e3e94` | dropped defect / re-run flags refused; defect event lines (`research_cycle.py ledger-defect`) |
| C-4 | `c0e90ab7` | `rerun_of` must name an earlier cell line of the same kind; window re-run on another window |
| C-5 | `c211624f` | blind / returns re-run needs a prior defect of its target; a re-run never lowers N |
| C-6 | `0aa411a9` | hash chain from the first ledger line (legacy lines folded as stored); head in verdict and copy |
| C-7 | `1e82e212` | the gate ledgers the admission trials; v8 Appendix A counts them |
| C-13 | `af9f4d76` | resume scores a done NAV only when its binding (spec or argv digest) matches the current spec |
| C-11 | `91c5dc5b` | holdout_gate appends a chained validation line citing the ruling's SHA-256; edited ruling refused |
| C-12 | `51aa3202` | cache gc compares normalised paths (case, separators, trailing slash, `..`, absolute) |
| E-33 | `448bdc85` | `mining-campaign` ledger kind: adds 0 to N, carries `registry.count` |

C-8 skipped (Ruling E-34).

## Verification of the findings marked unverified

- C-2 (review line 49, "the v8 base specs are not in this tree"): CONFIRMED. `scripts/research_cycle.py:935` at
  `81abfa12` passes only `summ.extra` (no `--protocol`, no `--origin`); the v71 cycle spec's summ extra
  (`scripts/specs/v71.json:313`) has no `--protocol`; the A2 lane drafts (`79440cfa`, base-lo1 / base-lo3) carry `--protocol v8` but no
  `--origin`, so nav_summ with `--ledger` would exit 2; add-alpha children of a v8 parent got no v8 protocol.
- C-7 (review line 108, "a writer outside my files"): CONFIRMED. `git grep admission 81abfa12 -- '*.py'` finds no
  writer of an admission ledger line: only `LEDGER_KINDS` (`backtest_integrity.py:64`), the reader
  (`backtest_integrity.py:864`) and a test fixture (`test_backtest_integrity.py:191`); the unmerged heads `79440cfa`,
  `126a5f5f`, `339c07b1` add none.
- C-14 (review line 188) is a minor finding not fixed here, so it was not verified.

## What was built (files, interfaces)

- `scripts/cycle_verdict.py` (C-1): `VerdictError`; `dsr_block(row)` reads nav_summ's `deflated_ledger` (N, cell
  count, effective N, `variance_sr`, `variance_cells`, `window_id`, legacy variance reported but gating nothing) and
  refuses a row without it; `verdict(..., ledger=)` records `{path, head, lines}` of the ledger.
- `scripts/research_cycle.py`: summ step adds `--protocol v8` and `--origin <summ.origin>` for v8 specs (C-2;
  `validate_summ_protocol`), `--dsr-ledger <ledger>` for verdict specs (C-1); gate step appends admission lines (C-7,
  `cycle_admission.py`); NAV binding written after a NAV run and checked on resume (C-13, `cycle_resume.py`); ledger
  head in the verdict and the ledger copy log (C-6); `ledger-defect` verb (C-3).
- `scripts/research_add_alpha.py` (C-2): the child spec's `summ.origin` is the highest origin class of the added
  alphas (registry), the parent's `--origin X` in extra is dropped.
- `scripts/cycle_admission.py` (new, C-7): `admission_lines(cycle, w_dir)` / `ledger_admissions(...)`: one chained
  admission line per gate candidate (kind admission, count 1, candidate, origin, window_id, pins, trial_id).
- `scripts/cycle_resume.py` (new, C-13): `cycle_binding.json` = {schema `atx.cycle-nav-binding/v1`, output,
  spec_sha256, argv_sha256}; `check_binding` refuses a mismatch naming both digests (exit 3, "HARD-STOP [nav]").
- `scripts/research_ledger.py`: `defect_main` (C-3); validation and mining-campaign lines skipped by `cells()`.
- `scripts/research_gc.py` (C-12): `path_key(root, path)` and `shown(root, path)`; candidates deduplicated.
- `atx-impl/tools/backtest_integrity.py`: `defect_line`, `check_line`, `check_rerun`, `invalid_ids` (C-3..C-5);
  `trial_counts` / `excluded_lines` per C-5; `fold_head`, `chain_head`, `_walk` (C-6); `VALIDATION` event kind (C-11);
  `MINING_CAMPAIGN`, `campaign_line`, `is_campaign`, `campaign_registry_count` (E-33).
- `atx-impl/tools/compare_window_overlap.py` (C-9, C-10): `W0A_TOLERANCE = 1e-9`, `w0a_class`, per-key `reason`,
  `max_rel_diff`, `keys_without_cells`; zero cells is never `bit_identical`; NaN vs value is a mismatch, NaN vs NaN a
  match.
- `atx-impl/tools/holdout_gate.py` (C-11): `--ledger TRIALS` (required); after every check and before any NAV series
  is opened, one chained `validation` line {count 0, book, owner_ruling {path relative to the ledger dir, sha256},
  owner, date, blocks, thresholds_sha256, deploy_manifest_sha256}; the same ruling bytes append nothing; a ruling whose
  bytes differ from the SHA-256 recorded for the same file (path normalised) is refused, exit 2.

Tests added or changed: `scripts/tests/test_cycle_scoring.py` (new), `scripts/tests/test_cycle_resume.py` (new),
`atx-impl/tools/test_trial_ledger_rules.py` (new: C-3, C-4, C-5, C-6, E-33), `test_research_cycle.py` (C-12 spelling
test, fixtures), `test_research_cycle_roles.py`, `test_research_ledger.py`, `test_nav_summ_v8.py`,
`test_compare_window_overlap.py`, `test_holdout_gate.py`.

## How root verifies

```
"C:/Program Files/Python312/python.exe" -m pytest -q -p no:cacheprovider scripts/tests
    -> 130 passed, 4 skipped
"C:/Program Files/Python312/python.exe" -m pytest -q -p no:cacheprovider .      (from atx-impl/tools)
    -> 463 passed, 2 skipped
```

Targeted: `scripts/tests/test_cycle_scoring.py` (C-1 example, C-2 argv pin), `test_cycle_resume.py` (C-13),
`atx-impl/tools/test_trial_ledger_rules.py` (C-3..C-6, E-33), `test_compare_window_overlap.py` (C-9/C-10),
`test_holdout_gate.py` (C-11), `test_research_cycle.py -k cache_gc` (C-12), `test_research_ledger.py` (C-7, defect CLI).

## Output changes (old -> new, synthetic fixtures)

- C-1 verdict DSR, fixture of the review (40 ledgered cells 20 x 1.15 / 20 x 0.85 annual + the cell at SR 1.0,
  1006 sessions, N 41): old (Lo single-cell `deflated`) SR0 1.10, DSR .42 -> new (ledger cross-trial variance, SD .15)
  SR0 .33, DSR .91 (`test_cycle_scoring.py`).
- C-7 v8 Appendix A on the gate fixture: "admission trials this sprint 0" -> "1" (`test_research_ledger.py`).
- C-9 zero compared cells: `bit_identical` true -> false with a reason.
- C-6 chain head of a ledger with legacy lines: the first chained line's `prev_sha256` is now the fold of every
  legacy line, not the SHA-256 of the last one. The root ledger `C:/atx-wt/pool-2/build-equity/trials.jsonl` has 37
  lines and 0 chained lines (format check only), so it reads unchanged and its first chained line will pin all 37.
- C-11 the gate writes one ledger line per ruling; its two bits are unchanged.
- No other number moves with consistent flags: C-5 changes attribution only (the test pins equal totals with the old
  rule on a consistent ledger); C-12 lists exactly the old keep/gc sets for the canonical spelling; E-33 leaves
  Appendix A of a ledger without a campaign line byte-identical.

## Deviations, with reasons

- C-3/C-5: a window re-run needs no defect (v8-prereg item 2 re-scores valid cells on the longer window); blind and
  returns re-runs do. A replaced cell stays counted and its blind re-run adds 0 (brief: "the replaced cell stays
  counted and the rerun adds 0"); a returns re-run is a new trial beside it.
- C-11: implemented the brief's rule (ruling SHA-256 in the citing ledger line, refusal on a different hash) plus the
  review's "every read leaves a record". Not implemented from the review's suggested fix: refusing a second thresholds
  SHA for the same book, and requiring the ruling file to be tracked at HEAD (both beyond the brief; see risks).
- C-12: paths are normalised with `normpath` + `normcase`, not `realpath`: a junction under build-equity is compared
  as spelled, so a store spelled through its junction target differs (see risks).
- E-33: the ruling text says "carries its own registry count field"; it is `registry.count` next to
  `registry.chain_head` and `registry.path` (plan: registry chain head copied to the cycle ledger). The line's `count`
  is 0. A NAV cell cannot be ledgered with this kind; a defect line cannot name a campaign line.
- Brief lists `scripts/compare_window_overlap.py`, `scripts/backtest_integrity.py`, `scripts/holdout_gate.py`; they live
  in `atx-impl/tools/`, edited there.

## Cross-lane

- No area A/B file (C++, field builders) was edited.
- A2 (`79440cfa`): its v8 base specs and cell templates must set `summ.origin` (prior | grid | mined) and must not put
  `--origin` in `summ.extra`; a verdict spec now needs `summ.ledger` (for `--dsr-ledger`). Otherwise plan refuses.
- H3 (`339c07b1`, mining verb): the campaign writer should append `backtest_integrity.campaign_line(...)` to the cycle
  ledger (chain=True); nothing writes it yet (no campaign runs in v8, OD-7).
- Any caller of `holdout_gate.py` must pass `--ledger` (none in the repository).
- mega_report reads `trial_counts` / `excluded_lines`: totals unchanged, per-line attribution changed by C-5.

## Minor findings left untouched (not inside an edited line)

C-14 (`research_cycle.py:773` screen u pass reused), C-15 and C-16 (`nav_summ.py`), C-17 (trial id on whole-file
bytes), C-18 and C-21 (`book_diagnostics.py`), C-19 (`atx-engine/tools/conftest.py`), C-20 (`mega_report/data.py`),
C-22 (`research_ledger.py` N and cell list from an unverified read: `cells()` still reads without the chain check).

## Open risks

- C-11: a new ruling file at a new path is allowed; the bisection is visible (one validation line per ruling, with its
  thresholds SHA) but not blocked. A book/block rule ("one thresholds SHA per book") needs an owner ruling.
- C-11: `holdout_gate.py` now writes to the trial ledger (one line per distinct ruling); a read that fails after the
  line is written stays recorded (intended: a read attempt).
- C-12: junction or symlink aliases of a store are not resolved.
- C-13: an output made before the binding existed is scored only if its bounded-runner receipt records `command`;
  older receipts without it are refused (re-run under a fresh `--suffix`).
- C-6: any ledger chained under ledger-chain-v1 after legacy lines (prev = SHA-256 of the last legacy line) would now
  read as broken; the root ledger has no chained line, so none exists at root. Other copies were not checked.
- The added tests run on Windows; the C-11 and C-12 case-folding checks are skipped off Windows.
