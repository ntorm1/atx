# Task REPORT report: v8 report lane (pool-10, branch feat/platform-v8-report-20260929)

Status: DONE_WITH_CONCERNS -- stopped by the PM (owner instruction) after task 1. Tasks 2-4 not started.
Synthetic data only; the renderer was not run on any config.

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
- `test_v7_quote_sources_are_read_again`: the three E-4 concern-6 sources (`.superpowers/sdd/mega-alpha-20260926/progress.md`,
  `.superpowers/sdd/platform-20260928/progress.md`, `docs/plans/2026-09-28-mega-alpha-scorecard-v6.md`) are not sealed
  (also with `\`); refused: `docs/plans/x-validation-notes.md`, `build-equity/nav-2024/x.csv`,
  `.superpowers/sdd/s/holdout/a.md`, `docs/plans/../build-equity/nav-2024/x.csv`, `build-equity/docs/plans/nav-2025/x.csv`.
- `test_quote_sources_through_the_registry`: `quote_source` returns the quoted lines of the three sources; the two refused
  paths record `refused (sealed)` with no bytes or SHA.
- `test_sealed_records_without_opening`.
- `MemmapPayloadSeal` (reuses `World` from `test_mega_report_sig_corr.py`): a date-shaped payload name behind an unsealed
  sidecar is refused on the cache-scan path and on the summary-entry path (manifest `refused (sealed)`, never listed as
  memmap); unsealed payloads are still mapped (3 memmap rows).

Run: `"C:/Program Files/Python312/python.exe" -m pytest -q -p no:cacheprovider atx-impl/tools/test_mega_report_seal.py
atx-impl/tools/test_mega_report_sig_corr.py atx-impl/tools/test_mega_report_pitch3.py atx-impl/tools/test_alpha_report_card.py`
-> 67 passed (seal file alone: 9 passed).

### How root verifies
- The tests above.
- Name-only checks run here (no file opened): all 147 path strings of `docs/plans/mega-alpha-v7-pitch.config.json` pass
  the new check (E-4 had refused 3); the 144 file names under `C:/atx-wt/pool-2/build-equity/mega-candidate-cache-v71`
  (the v7 sig_corr payloads) all pass, so the v7 sig_corr block is unchanged.
- E-4 step 3 (root): re-render the v7 pitch; the trial table quotes return; 0 unavailable blocks expected; file manifest
  statuses equal E-4's except the three quote sources (`read` again).

### Deviations
None from the ruling. The `..` guard is an addition: a path that climbs out of a document root is not a document.

### Cross-lane edits
None (`pitch.py` is mega_report, this lane's module).

### Open risks
- The exemption is by root prefix only, as ruled: a data file placed under `.superpowers/sdd/` or `docs/plans/` with a
  date-shaped name would be read. No v7 or v8 config puts data there today.

## STOPPED HERE

| task | state |
|---|---|
| 1 E-11 ruling + memmap gap | DONE, 09d14980 |
| 2 v8 report components (year table, bundle verdict, diagnostics, traded-horizon member columns, trial-accounting block, contradictions table) | NOT STARTED |
| 3 config scaffolds (v8 pitch / scorecard configs, scorecard v8 template) | NOT STARTED |
| 4 render on the fixture / synthetic JSON | NOT STARTED |

Input formats gathered for task 2 (read only, for whoever resumes):
- Year table: each `nav_summ --json` row (with `--protocol v8` or `--year-table`) carries `year_table: [{year,
  return_rows, net_return, net_sharpe, ann_vol, tau_gmv_mean, cost_bps_traded}]` (`nav_summ.year_table`, pool-2).
- Bundle verdict: `nav_summ --bundle BASE FINAL --bundle-json OUT` writes `{base, final, scenarios{base, final},
  paired{sessions, sr, sr_reference, dsr, rho, memmel_se, t, cbb_ci95, lw{dsr_population, se, ci95, p_value,
  p_one_sided, valid_draws}, draws, block, seed}, years[{year, sessions, sr_final, sr_base, dsr}], year_table{base,
  final}, verdict{dsr_positive, p_one_sided, alpha, pass, rule}, protocol, nav_summ_run}`.
- Diagnostics: lane G's `book_diagnostics.py` (uncommitted in pool-8) writes schema `atx.book-diagnostics/v1`:
  `{schema, declaration, window_id, tool{script, script_sha256}, diagnostics{G-1a..G-3d: {status ok|skipped, reason
  (skipped), question, inputs, method, result}}}`; results are nested dicts of varying shape.
- Traded-horizon columns (C-2): card `horizon.ic_theta` (index row `ic_theta`), card `marginal_ic` / index
  `marginal_ic21` (K6 row `{id, ic21, ic21_hac_t, marginal_ic21, marginal_hac_t, max_abs_rho, max_rho_member}`),
  admission rows `f_theta`, `f_theta_hac_t` (fitter `--report-f-theta`).
- Trial accounting: v8-prereg Appendix A block plus ruling R2-e's phrase "plus 8 re-screens" (ledger-pending-2.md);
  OD-1 disclosure text is v8-prereg item 1.
- Suggested shape: one new module `mega_report/v8.py` of pure `render_*(data: dict | None, src: str) -> str` components
  (None renders a `class="unavailable"` block naming the path) plus thin pitch blocks registered like pitch3; v7
  configs untouched so their render stays byte-identical.
