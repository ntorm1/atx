# Task REPORT report: v8 report lane (pool-10, branch feat/platform-v8-report-20260929)

Status: DONE_WITH_CONCERNS -- tasks 1-4 done. Synthetic data only: the renderer never ran on `build-equity` data and
no file dated 2024-01-01 or later was opened.

| task | commit |
|---|---|
| 0 merge root (integration 3) | ec47b8ce (one conflict: `mega_report/data.py` docstring; kept E-11 and root's `atx-impl/tools/nav_summ.py` path) |
| 1 E-11 ruling + memmap gap | 09d14980 |
| 2 v8 report components and pitch blocks | 7fa1c730 |
| 3 v8 pitch config, scorecard v8 template | d0348c27 |
| 4 render tests on synthetic JSON | 93fb94c2 |

## Task 1: ruling E-11 in code, memmap gap closed (commit 09d14980)

### What was built
`atx-impl/tools/mega_report/data.py`
- `DOCUMENT_ROOTS = ('.superpowers/sdd/', 'docs/plans/')` and `is_document_path(rel_path) -> bool`: the path (either
  separator), relative to the report root, starts with a document root and has no `..` part.
- `path_is_sealed`: the named pattern (validation / holdout / VAL) is tested first and applies to every path; a document
  path then returns False (exempt from the year rule by root class); every other path keeps the E-4 year rule unchanged.
- `Registry.sealed(rel) -> bool`: the seal check on the key relative to the root, without opening or stat'ing; a refusal
  is recorded as `refused (sealed)`. `read_bytes` now calls it (same behaviour as E-4).

`atx-impl/tools/mega_report/pitch.py` (E-4 open risk)
- `_summary_signal_entries`: `not ctx.reg.sealed(payload)` before `payload.is_file()`.
- `an_sig_corr`: each payload passes `ctx.reg.sealed(p)` before its `stat()` and `np.memmap`; a sealed payload raises
  `PermissionError('cache payload ...: refused (sealed)')`, so the analysis fails and its blocks render "not available".

### Tests (`atx-impl/tools/test_mega_report_seal.py`, 6 new)
`test_v7_quote_sources_are_read_again`, `test_quote_sources_through_the_registry`, `test_sealed_records_without_opening`,
`MemmapPayloadSeal` (a date-shaped payload behind an unsealed sidecar is refused on both paths; unsealed payloads still
mapped). Name-only checks: all 147 path strings of the v7 pitch config and the 144 v7.1 cache payload names pass.

## Task 2: v8 report components (commit 7fa1c730)

### What was built
`atx-impl/tools/mega_report/v8.py` (new). Pure components, each `render_*(data, src, ...) -> str`; `data` None renders
the report's unavailable block (`<div class="unavailable" data-block="<block>">`, report._na_block's markup) naming
`src`:

| component | block type (layout) | input (config key `v8.*`) | content |
|---|---|---|---|
| `render_year_matrix`, `render_year_table` | `v8_year_table` | `summ` (owner) | net Sharpe by cell x TRAIN year + whole window; per cell (details) year, return rows, net Sharpe, compounded return, volatility, tau mean, cost bps/$ |
| `render_cell_ladder` (`ladder_rows`) | `v8_ladder` | `cells[].paired` (owner); borrows `summ` | #, cell, parent, N after, paired S2 net dSR (Memmel SE), one-sided bootstrap p, dSR > 0, mechanics, mechanical criterion (chip + detail), rule of v8-prereg item 5, ledger verdict badge |
| `render_bundle_verdict` (`an_bundle`, `freeze_gate`) | `v8_bundle` | `bundle` (owner); borrows `summ` | gate panel: S2 net >= 1.0, 4 mechanics items, cumulative paired dSR > 0, one-sided p < .10, cell-count DSR >= .95 (`deflated_ledger.dsr`), freeze MET / UNMET / n/a; paired statistics table; per-year dSR; notes (non-registered bootstrap, alpha mismatch, nav_summ verdict disagreement, unmet -> OD-3 text) |
| `render_diagnostics` (`diag_rows`) | `v8_diagnostics` | `diagnostics` (owner) | declaration, window id (flags a mismatch), G-1a..G-3d: status chip, question, headline figures (paths in `HEADLINES`) or the skip reason; "not in file" for an absent id; full results per id in details (G-1a member table, flattened leaves, method) |
| `render_member_horizon` (`member_rows`) | `v8_member_horizon` | `member_horizon.card_index`, `.admission` (owner of both) | ic_theta, f_theta (HAC t), marginal IC21 (HAC t), max abs rho; caption "report only, gates nothing" |
| `render_trial_accounting` (`an_ledger`) | `v8_trial_accounting` | `trial_ledger` (owner) | `backtest_integrity.appendix_a_v8` over the ledger with " plus 8 re-screens" inserted after the admission count (ruling R2-e; `v8.re_screens`, default 8); N and k against `v8.budget`; ledger lines, zero-trial lines, defect exclusions, hash chain |
| `render_od1` (`prereg_item`, `window_facts`) | `v8_od1` | `prereg` (owner) | callout: v8-prereg item 1 verbatim, the research window's dates from research_window.json, `v8.od1_notes` |
| `render_contradictions` (`markdown_table_after`) | `v8_contradictions` | `literature` {path, heading} (owner) | the table under "Where the new notes contradict or update v6 and v7", verdict badge contradicts / updates / confirms / other, links kept |

- One owning block per input (`v8.inputs(cfg) -> [(owner block, key, path)]`): a missing, refused, malformed or
  wrong-schema input renders exactly one unavailable block, its owner's, naming the path. A borrower (ladder and gate on
  `summ`) shows n/a plus a note naming the file.
- Every read goes through the Registry (E-4 / E-11 seal check before the open, SHA-256 into the manifest, optional pin).
  Content seal on top: a year >= the first sealed year in any year table or bundle, or a `session_ns` at or after the
  seal anywhere in the diagnostics, raises `ValueError` (research window message) and the owner renders unavailable.
- The ledger is read through the Registry, then parsed by `backtest_integrity.ledger_read` (schema per line, v8 hash
  chain) -- one parser, no copy. Registered bootstrap constants come from `nav_summ` (`DEFAULT_BLOCK`, `V8_SEED`,
  `V8_DRAWS`, `BUNDLE_ALPHA`), train years from `engine_tools.research_window` (no literal dates).
- Analyses for prose placeholders: `v8_summ`, `v8_bundle` (paired, final_row, gate, freeze), `v8_diag`, `v8_ledger`
  (`n`, `k`, `re_screens`, `block`).
- `pitch.py`: `BLOCKS.update(_V8.BLOCKS)`, `ANALYSES.update(_V8.ANALYSES)` after pitch3's (new `v8_*` names only).

### v7 identity
A config without `v8_*` blocks takes exactly the old code path. Tested two ways in `test_mega_report_v8.py`:
`test_v7_config_renders_the_pre_v8_bytes` renders pitch3's synthetic v7-shaped world and compares the SHA-256 of the
normalised HTML (generator git head and temp root replaced) with the digest recorded at ec47b8ce before v8.py existed
(`c42c5fad...`, 109,868 bytes); `test_v7_render_is_unchanged_by_the_v8_registration` renders with and without the v8
registration and compares bytes. `docs/plans/mega-alpha-v7-pitch.config.json` is untouched (digest pinned in task 4).

## Task 3: config scaffolds (commit d0348c27)

- `docs/plans/mega-alpha-v8-pitch.config.json`: schema v2, root `C:/atx-wt/pool-2`, layout 0 summary (planning callout,
  `v8_bundle`, caveats) / 1 window (`v8_od1`, `v8_trial_accounting`) / 2 cells (`v8_ladder`, `v8_year_table`) /
  3 diagnostics (`v8_diagnostics`, `v8_member_horizon`) / 4 literature (`v8_contradictions`) / 5 appendix (`t_files`).
  Cells B0a, B0b, B0c, R-1..R-7 with N after 38..47 and the criteria of plan 12.1; mechanics (R6'); the freeze gate of
  v8-prereg item 9 with the OD-3 unmet note; the planning callout "Plan on a live net Sharpe .7 to 1.0 [est] against
  the TRAIN figure {a:v8_bundle.final_row.net_sharpe|+.3f} (S2, 2020-2023)".
- `docs/plans/mega-alpha-scorecard-v8.template.md`: scorecard v6 sections (headline, freeze gate, cells, year tables,
  stresses, alpha pipeline, alpha table with the report-only columns, DSL, diagnostics, Appendix A with "plus 8
  re-screens", OD-1, contradictions) with `{{ALIAS...}}` placeholders; grammar in its header comment.
- `v8.py` (same commit): a criterion check without `metric` ({text, met}) is a manual part (capacity curve, marginal t);
  it stays n/a, and so does the item-5 rule, until the PM records `met`.

### Inputs the PM must produce (expected paths relative to `C:/atx-wt/pool-2`; `PY="C:/Program Files/Python312/python.exe"`)

| config key (owner block) | path | produced by | schema / keys read |
|---|---|---|---|
| `v8.summ` (v8_year_table) | `build-equity/mega-nav-v8-summ.json` | `$PY atx-impl/tools/nav_summ.py --protocol v8 --dsr-ledger build-equity/trials.jsonl --json build-equity/mega-nav-v8-summ.json <the 10 cell dirs>` (add `--effective-n dirs --psr` for the scorecard) | list of rows: `dir` (basename = config dir), `net_sharpe`, `mean_gross_leverage_all_rows`, `mean_net_leverage_all_rows`, `tau_gmv_mean`, `tau_gmv_p95`, `cost_bps_traded`, `deflated_ledger.dsr`, `year_table` |
| `v8.cells[K].paired` (v8_ladder), K = B0b, R-1..R-7 | `build-equity/mega-nav-v8-paired-<b0b, r1 .. r7>.json` | `$PY atx-impl/tools/nav_summ.py --protocol v8 --bundle <parent dir> <cell dir> --bundle-json <path>` | bundle doc: `final`/`base` basenames must equal the cell's / parent's config dir; `paired.dsr`, `.memmel_se`, `.lw.p_one_sided`, `.draws/.block/.seed`, `years`, `year_table` |
| `v8.bundle` (v8_bundle) | `build-equity/mega-nav-v8-bundle-b0c-v8f.json` | `$PY atx-impl/tools/nav_summ.py --protocol v8 --bundle <B0c dir> <V8-F dir> --bundle-json <path>` | as above; `verdict.alpha/pass` |
| `v8.diagnostics` (v8_diagnostics) | `build-equity/mega-diagnostics-v8-b0c/diagnostics-v8.json` | `book_diagnostics.py run --output <path> ...` on B0c (lane G, handoff step 6) | `atx.book-diagnostics/v1`, `window_id` = research-window-v2 |
| `v8.member_horizon.card_index` (v8_member_horizon) | `build-equity/mega-cards-v8-r7/index.json` | `alpha_report_card.py --ic-theta --marginal-ic <F-2 marginal_ic.json>` on the final cell | `atx.alpha-report-card-index/v1`, `candidates[].ic_theta`, `.marginal_ic21` |
| `v8.member_horizon.admission` (v8_member_horizon) | `build-equity/mega-weights-v8-r7-ew/admission.json` | `fit_composition_weights.py --report-f-theta` on the final cell | `atx.dsl-admission/v1`, `candidates[].f_theta`, `.f_theta_hac_t` |
| `v8.trial_ledger` (v8_trial_accounting) | `build-equity/trials.jsonl` | the cycle's ledger lines | `atx.trial-ledger/v1`, hash chain intact |
| `v8.prereg` (v8_od1) | `.superpowers/sdd/platform-v8-20260929/v8-prereg.md` | exists | item "1. Window." |
| `v8.literature` (v8_contradictions) | `.superpowers/sdd/platform-v8-20260929/reports/Equity long short alpha v8.md` | exists | the three-column table under the heading |

Config edits before the render: the cell dirs of B0c and R-1..R-7 (placeholders `build-equity/mega-nav-v8-<key>`);
`parent` of each R cell = the last accepted cell (the default chain assumes every cell accepted); `v8.final` = V8-F's key
(default R-7) and the `member_horizon` paths of that cell; `verdict` of each cell (default "pending run"); `met` of the
manual criterion parts (R-3 net at 2x, R-5 net at 4x and at 1x within one SE, R-7 marginal t); optional R-8 / R-9 as
extra cell entries (N 48..51). Before an input exists its owner block names the missing path, so a first render is the
to-do list.

### Scorecard template placeholders
Aliases (inputs table of the template): `SUMM`, `PAIRED[K]` (K = B0b, R-1..R-7), `BUNDLE`, `LEDGER`, `APPX` (the
`nav_summ --protocol v8 --ledger-n build-equity/trials.jsonl` line), `DIAG`, `CARDS`, `ADM`, `W`
(`build-equity/mega-weights-v8-<final>-ew/composition_weights.json`), `NAVF` (`<V8-F dir>/summary.json`), `CAP[K]`
(`<cell dir>-stress/v7_extras.json` of R-3, R-5), `LIB` / `RECIPE` (`atx-impl/strategies/alphas/libraries/<final>.json`
and its recipe), `PREREG`, `LIT`; selectors `{{SUMM[K].path}}`, `{{ALIAS.list[KEY|i|*].path}}`, `{{TEXT:...}}`; and
`{{PM: ...}}` statements (root HEAD, date, cell dir names, V8-F key, parents, verdicts, gate result, criterion readings,
ann vol, stresses of B0c / B0a, the alpha pipeline walk-through, DSL section, theme mass, trial-accounting lines, what v8
did about each contradiction).

## Task 4: render on synthetic JSON (commit 93fb94c2)

`atx-impl/tools/test_mega_report_v8_render.py` (22 tests) reads the committed v8 config, moves its root to a short
`tempfile.mkdtemp(prefix='v8p')` directory and writes a synthetic file at each of its 16 input paths
(`test_mega_report_v8.world`, driven by `v8.inputs(cfg)`):
- every input present: 0 unavailable blocks, no unresolved placeholder, every v8 table id present, each of the 16
  inputs listed `read` with its SHA-256, 10 ladder rows;
- each input removed in turn (16 parametrized cases): exactly one unavailable block, its owner's, naming the path;
- a sealed path (`...summ-2024.json`) is refused before it is opened (one unavailable block, manifest `refused (sealed)`);
- the planning value and "admission trials this sprint 7 plus 8 re-screens" are in the text;
- the tracked v8-prereg.md and literature review parse (item 1 verbatim; >= 20 rows, >= 5 contradicts);
- the v7 pitch config is untouched (LF digest `73f80583...` at 39926caa) and has no v8 block.
tiny_world was not used: it builds inputs only, and yields no NAV or nav_summ output without the built executables.

## How root verifies

```bash
PY="C:/Program Files/Python312/python.exe"
"$PY" -m pytest -q -p no:cacheprovider atx-impl/tools/test_mega_report_v8.py atx-impl/tools/test_mega_report_v8_render.py \
  atx-impl/tools/test_mega_report_seal.py atx-impl/tools/test_mega_report_sig_corr.py \
  atx-impl/tools/test_mega_report_pitch3.py atx-impl/tools/test_alpha_report_card.py      # 123 passed here
"$PY" -m pytest -q -p no:cacheprovider atx-impl/tools                                        # 344 passed, 2 skipped here
```

Render commands (root, in `C:/atx-wt/pool-2` after merging this branch; both configs name root `C:/atx-wt/pool-2`):

```bash
# v7 re-render (E-4 step 3): expect "unavailable blocks 0"; compare the file manifest statuses with E-4's (the three
# quote sources are `read` again under E-11)
"$PY" atx-impl/tools/mega_report --config docs/plans/mega-alpha-v7-pitch.config.json \
  --out build-equity/v8-rerender-v7-pitch.html --stamp "v7 re-render after platform v8 E-11"
# v8 pitch, after every input in the table above exists and the config edits are made: expect "unavailable blocks 0"
"$PY" atx-impl/tools/mega_report --config docs/plans/mega-alpha-v8-pitch.config.json \
  --out docs/plans/<date>-mega-alpha-v8-pitch.html
```

## Deviations
- The v8 pitch is v8-focused (the eight v8 sections, prose, callouts, appendix), not a copy of the whole v7 layout. The
  legacy blocks (equity curve, costs, alpha library, DSL, attribution) need the final cell's daily CSVs, library and
  u pass; one missing file takes out many blocks at once (against "each missing input -> exactly one unavailable
  block") and they cannot be rendered synthetically without a full world. The PM can append v7 sections (layout entries
  plus their config keys, with `cells` / `final` set) after the cells run.
- Without `cells` / `final` the page header shows the window label with "n/a" for the session span and the final cell
  (report.py left untouched to keep v7 identical).
- Per-cell paired tests use `nav_summ --bundle PARENT CELL` (one-sided p, cell names checked), not `--reference`
  (two-sided p only, one run per parent).
- Lane G's module was read with `git show feat/platform-v8-g-20260929:atx-impl/tools/book_diagnostics.py` (pool-8 now
  holds another branch); read-only.
- The manual-criterion extension of `v8.py` is in the task-3 commit (it exists for the config).

## Cross-lane edits
None. `pitch.py` and `v8.py` are this lane's mega_report module; `nav_summ.py`, `backtest_integrity.py` and
`research_window.py` are imported, not edited.

## Open risks
- The diagnostics headlines name lane G's result keys (`HEADLINES`); a renamed key falls back to the first four values
  of the result, never to a failure.
- mega_report now imports `nav_summ` and `backtest_integrity` at load; an import error there would also stop v7 renders.
- The v7 golden digest is a tripwire on pitch3's synthetic world and the shared components: a deliberate change there
  needs the constant re-recorded after the diff is checked.
- The default parent chain and `final` = R-7 assume every R cell is accepted; a wrong parent is caught (the bundle's base
  name must equal the parent's dir), a wrong `final` is not.
- The gate's DSR item reads `deflated_ledger.dsr`: the summ command needs `--dsr-ledger`, else the item is n/a and the
  gate undetermined.
