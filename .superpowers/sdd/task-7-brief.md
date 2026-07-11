### Task 7: `tools/mag7_dispersion_report.py` — HTML/SVG renderer

Python renders; C++ emitted. One self-contained HTML: inline SVG chart(s), inline CSS, no JS, no external assets. Follows `tools/tearsheet.py` precedent (pandas + matplotlib Agg; meta parser `read_run` at tearsheet.py:39-50).

**Files:**
- Create: `atx-vol/tools/mag7_dispersion_report.py`
- Create: `atx-vol/tests/mag7_dispersion_report_test.py`
- Modify: `atx-vol/tests/CMakeLists.txt` (register the python test with `add_test` + `LABELS atx_vol`, mirroring the existing Python3-guarded block at ~lines 117-134)

**Interfaces:**
- Consumes: the T6 output-dir contract (five CSV files, `# key=value` meta + pinned columns).
- Produces: `mag7_dispersion_report.html` (default: `<run-dir>/mag7_dispersion_report.html`).

**CLI:** `python mag7_dispersion_report.py <run-dir> [out.html]`

**Structure (pinned):**
- `read_meta_csv(path) -> (meta: dict, df: DataFrame)` — copy tearsheet.py's parse (`# k=v` lines, then `pd.read_csv(comment="#")`).
- SVG: matplotlib figure(s) saved to an `io.StringIO` with `format="svg"`, strip the XML declaration/DOCTYPE, embed the `<svg>...</svg>` inline. Chart 1 (required): YTD cumulative P&L / NAV equity curve with a drawdown panel beneath (shared x, `fill_between` shading). Chart 2 (welcome, cheap): cumulative attribution lines (theta/vega/gamma/unexplained cumsums) — include it.
- Tables (HTML `<table>`, built by a small helper, values formatted sensibly):
  1. **Strategy metrics** from `strategy_metrics.csv` (all rows) — includes total P&L, sharpe, max drawdown, hit rate, avg daily P&L, turnover, avg/peak open lots, attribution totals, avg net vega/theta after entry.
  2. **Backtest engine metrics** from `engine_metrics.csv` (wall clock, steps/sec, cache stats) + unpriced counts (from strategy_metrics rows `total_unpriced_*`).
  3. **Surface/db statistics**: from `db_stats.csv` meta+rows (dates covered = first/last partition key + count, partition sizes, generation) and, when `populate_stats.csv` exists, the per-symbol fit success table.
- Header block: strategy name, universe, window, and ALL pinned defaults from the shared meta (delta, tenor, theta budget, multiplier, frictions, missing policy) — "document them in the report header" is an acceptance requirement.
- Self-containment: single `<html>` string with one inline `<style>` block; assert-no-`http`/`src=` discipline.
- Dependencies: `pandas`, `matplotlib` (Agg), stdlib only. No new deps.

- [ ] **Step 1: Write the failing test.** `atx-vol/tests/mag7_dispersion_report_test.py` (pytest style consistent with the existing `tools/*_test.py` files — read one to copy conventions): a fixture function writes a minimal synthetic run dir (5 files, 3-row series, tiny metrics) into `tmp_path`; run the script via `subprocess` (or import + call `main`) → assert exit 0, output HTML exists, contains `<svg`, contains the three section headings (`Strategy metrics`, `Engine metrics`, `Surface/db statistics` — pin exact heading strings in the script), contains a pinned-default string (e.g. `theta_per_name_daily`), and contains NO `http://`/`https://`/`src=` substrings (self-containment).
- [ ] **Step 2: Run the test; verify it fails** (script missing): `& .\scripts\atx-build.ps1 -Ctest -R "Mag7DispersionReport"` (after CMake registration) or `python -m pytest atx-vol/tests/mag7_dispersion_report_test.py` directly.
- [ ] **Step 3: Implement the script.**
- [ ] **Step 4: Re-run; PASS.** Also re-run `-R "TearSheet"` C++ suite untouched-green (no C++ changes expected in this task; the check is cheap).
- [ ] **Step 5: Commit.**

```bash
git add -A
git commit -m "feat(atx-vol): mag7_dispersion_report.py - self-contained HTML/SVG report renderer"
```

---

