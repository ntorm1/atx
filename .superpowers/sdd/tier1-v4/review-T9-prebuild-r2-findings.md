### Spec Compliance — ❌ Scoped round-2 prebuild needs fixes; final Task9 remains pending

Reviewed commit 59b2626f9f6cb0c18f920acc19460da9d24e7ee1 through the complete 395-line code/report/helper diff, retained source-context JSON (five originals, seven tables), Task9 brief/report, REVIEWER, GC/resume contracts and preceding round-1 findings. The source-context excerpt agrees with the AAR split quarter/YTD bands and Abbott 2Q15/2Q14 plus percent-change geometry. No HTTP, real-data jobs, broad discovery, full suites, git mutations or source changes were performed. Only this assigned artifact was written.

✅ Split date/year binding and caption-qualified abbreviated years work for the supplied source-shaped fixtures. Currency remains explicit; bare-dollar originals stay NULL. Missing historical fiscal-calendar identity remains NULL. The changed partial-cache path rechecks SEC input integrity and catalog identity before admitting a receipt. No new PIT or comparative-merge behavior was introduced by this diff; prior round-1 dispositions remain intact.

❌ ea_actuals.py:127–136 does not exclude overlapping quarter/YTD duration spans. ❌ ea_actuals.py:131–136 does not exclude date-bearing percent-change columns. Both conflict with the new function contract and report's claimed conservative geometry handling.

⚠️ Final production requirements cannot be verified from this diff: actual acceptance gates remain 0/5 PASS, frozen manual precision 0/200. The earlier 100-unit cache prefix produced zero qualified values and predates revision v3; it establishes bounded-prefix memory/resume only, not representative recall, precision or revision-v3 extraction. Full-corpus and feature resource bounds, historical EPS bridge, final producer integrity, Task3 rdq refresh and acquisition accounting remain pending.

### Strengths

The change preserves source-backed missingness, limits assembly to one digit-bearing cell, validates malformed spans/unsupported rowspans and rejects overlapping or contradictory date bands. Tests deliberately distinguish synthetic USD positive controls from source-shaped bare-dollar negatives. Retained accession/hash/table context makes the concrete parser changes auditable without additional fetching.

### Issues — Critical / Important / Minor

Critical: none.

**Important I1 — Overlapping duration bands qualify an ambiguous/YTD cell** (`atx-db/src/atx_db/alpha_panel/ea_actuals.py:127`, `:132`). The implementation stores only quarter spans. YTD evidence merely sets a global boolean; it never records the YTD column intervals or checks their intersections. A year band [2,3) enclosed by quarter [1,3) and YTD [2,4) therefore qualifies. A tiny offline parse with these headers and Sales=900 at that band emits saleq=900,000,000 rather than NULL. The final overlap check considers date bands only, so cannot catch this case. Fix: retain all duration intervals, require each accepted date/year band to have unambiguous quarter-only ancestry, and reject intersecting/crossing quarter/YTD evidence. Add an overlap-duration negative fixture separate from the existing date-band-overlap test.

**Important I2 — Date-bearing percent-change columns become monetary actuals** (`atx-db/src/atx_db/alpha_panel/ea_actuals.py:131`, `:135`). There is no percent/change-column exclusion in quarter_columns. The supplied Abbott fixture passes because its percent cell contains no date or quarter token. A source-shaped variant headed `March 31, 2020 % Change`, alongside `March 31, 2019`, with Sales cells 2.2 and 100 under a `Three months ended USD in millions` caption emits saleq=2,200,000 and comparative=100,000,000. The percentage is silently treated as currency. Fix: record percentage/change header intervals across header rows and exclude any overlapping candidate date band, including date or abbreviated-year tokens underneath spanning percent headers. Add both same-cell and multilevel percent-header negatives.

Minor: retained full-corpus/feature resource evidence remains open. The actual 100-unit memory/reuse measurement partially addresses prior M2 but cannot close the broader bound. No new scoped polish finding.

### Focused verification

The three new named fixture tests passed: 3 passed in 1.76 seconds, no warnings. Command from C:/atx with PYTHONPATH=C:/atx/atx-db/src and OPENBLAS_NUM_THREADS=1:

```powershell
atx-db/.venv/Scripts/python.exe -m pytest -n0 -o addopts='' -p no:cacheprovider atx-db/tests/test_alpha_panel_ea_actuals.py::test_aar_original_shaped_split_header_quarter_ytd_and_unknown_currency atx-db/tests/test_alpha_panel_ea_actuals.py::test_abbott_original_shaped_quarter_abbreviations_exclude_percent_columns atx-db/tests/test_alpha_panel_ea_actuals.py::test_quarter_column_conflict_overlap_and_rowspan_fail_closed -q --tb=short
```

Two additional tiny stdin Python checks using the same interpreter/environment reproduced the Important findings; neither reads actual source data. Duration reproduction:

```python
blob = b'<html><p>USD in millions</p><table><tr><td></td><td colspan="2">Three Months Ended March 31,</td></tr><tr><td></td><td></td><td colspan="2">Nine Months Ended March 31,</td></tr><tr><td></td><td></td><td>2020</td><td>2020</td></tr><tr><td>Sales</td><td></td><td>900</td><td>800</td></tr></table></html>'
EA.parse_release(blob, datetime.date(2020,4,20))['saleq']  # 900000000.0
```

Percentage reproduction:

```python
blob = b'<html><p>Three months ended USD in millions</p><table><tr><td></td><td>March 31, 2020 % Change</td><td>March 31, 2019</td></tr><tr><td>Sales</td><td>2.2</td><td>100</td></tr></table></html>'
EA.parse_release(blob, datetime.date(2020,4,20))['saleq']  # 2200000.0
```

### Assessment — Task quality: Needs fixes

Scoped prebuild spec: Needs fixes. Scoped code quality: Needs fixes because explicit exclusion guarantees can admit nonquarter or percentage amounts. Final Task9 remains unaccepted at 0/5 gates and 0/200 manual checks. This review confers no full-parse, materialization, acquisition or HTTP grant; another bounded cache experiment requires the controller's authorized guarded slot after the scoped fixes are reviewed.
