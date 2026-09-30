# Task E-4 report: report seal check (OD-5)

Lane W0E, pool-3. OD-5 approved. Synthetic data only.

## What was built
`atx-impl/tools/mega_report/data.py`:
- `FORBIDDEN` (a bare `2023|2024|2025` substring match) is replaced by the brief's `_HEX`, `_NAMED`, `_YEAR` and
  `path_is_sealed(rel_path, first_sealed_year=FIRST_SEALED_YEAR) -> bool`, verbatim except the default:
  `FIRST_SEALED_YEAR = research_window.FIRST_SEALED_YEAR` (2024), read through `engine_tools.py` (W0-1), not a literal.
- `Registry.read_bytes` checks `path_is_sealed(key)` on the key relative to the registry root (unchanged
  `Registry.rel`), before the file is opened; a refusal records status `refused (sealed)` (was
  `refused (forbidden name)`), no bytes, no SHA.
- Module docstring updated.

`atx-impl/tools/test_mega_report_seal.py`:
- `test_seal_regex_hex_vs_date`: the brief's cases (`fp_2e2025f0aa11bb22/droe.f64` and `train-2020-2023-lo1/daily.csv`
  read; `nav-2023-2024/x.csv`, `x_20250131.csv`, `nav-2026/y.csv`, `VAL/z.csv` refused), plus an `ic1_` hex folder,
  a 64-hex cache folder, the 2023-2024 validation role and `holdout/`, the year threshold and `\` separators.
- `test_seal_paths_are_relative_to_out_root`: a registry rooted under `.../platform-v8-20260929/research-out`
  reads the allowed files (status `read`) although the absolute root is date-shaped, and refuses the sealed ones
  without opening them.
- `test_first_sealed_year_is_the_research_windows`.

## How root verifies
- `pytest atx-impl/tools/test_mega_report_seal.py -q` (3 passed here); with the other mega_report and card tests:
  `pytest atx-impl/tools/test_mega_report_seal.py atx-impl/tools/test_mega_report_sig_corr.py
  atx-impl/tools/test_mega_report_pitch3.py atx-impl/tools/test_alpha_report_card.py -q` -> 61 passed.
- Step 3 (root): re-render the v7 pitch and count `class="unavailable"` blocks; compare the file manifest statuses.

## Finding for step 3 (read before re-rendering)
I ran both checks over the 156 path strings of `docs/plans/mega-alpha-v7-pitch.config.json` (root
`C:/atx-wt/pool-2`; no file opened). Only these change, all from read to refused, all trial-accounting quote sources:
- `.superpowers/sdd/mega-alpha-20260926/progress.md` (part `20260926`)
- `.superpowers/sdd/platform-20260928/progress.md` (part `20260928`)
- `docs/plans/2026-09-28-mega-alpha-scorecard-v6.md` (part `2026`)
The sprint folders sit inside the report root, so the relative path still carries their run-date stamp. The trial
table still renders (quotes go blank; `quote_source` returns None), so this is not an `unavailable` block, but the
quoted source lines disappear. No data input changes status. Options for root: accept blank quotes; move the report
root; or exempt tracked documentation (`.superpowers/`, `docs/`) from the year rule only (a change to the approved
regex, so not made here).

## Deviations
- Default `first_sealed_year` comes from the research window instead of the literal 2024 (brief step 2).
- Refusal status text `refused (sealed)`.

## Cross-lane edits
None (lane EV's V-1 moves `nav_summ.py`, not `mega_report`).

## Open risks
- `pitch.py` registers memmapped cache payloads directly in `reg.files` (no `read_bytes`), so those paths are not
  seal-checked; their metadata JSON is read through the registry first. Unchanged here.
