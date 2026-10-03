### Spec Compliance — ✅ Scoped round 3/5 prebuild; final Task9 pending

Reviewed commit be225920db1197a0db6f8c981df8d99c3b8293a8, its complete code/test/report diff, prior round-2 findings, newest Task9 report and REVIEWER/global constraints. Scope is the two retained parser findings; earlier unrelated dispositions are not reopened. Only this findings file was written. No real sources, jobs, scans, HTTP, git mutations or implementation edits occurred.

✅ Prior Important I1 is closed: ea_actuals.py:129–143 retains YTD intervals and rejects any quarter/YTD parent intersection, including the exact crossing [1,3)/[2,4) reproduction and the nonintersecting-child variant. Admission also rejects date bands that cross quarter boundaries. Disjoint quarter/YTD bands retain legitimate quarter-only observations.

✅ Prior Important I2 is closed: ea_actuals.py:135–149 records percentage/change/growth/decline intervals and excludes intersecting candidates through the same admission helper. Full dates, split year bands and caption-bound abbreviated quarter tokens all use it; the abbreviated path repeats the check at :171. Safe monetary bands outside percentage intervals remain eligible. Semantic version v4 prevents earlier parser cache evidence from certifying this revision.

⚠️ Final spec remains unaccepted: actual gates are 0/5 PASS and frozen manual precision is 0/200. Earlier 100-unit measurements precede v4 and support only bounded-prefix memory/resume, not current extraction, representative precision/recall or full resource bounds. Source-qualified EPS history, final producer integrity/rdq refresh and historical acquisition accounting remain pending. This checkpoint grants no acquisition, real build or materialization permission.

### Strengths

The patch shares interval admission across all existing date-binding paths and asserts the exact monetary misclassification reproductions. Safe disjoint spans and existing AAR/Abbott source-shaped controls preserve conservative positive behavior without weakening currency or fiscal-identity requirements.

### Issues — Critical / Important / Minor

Critical: none. Important: none remaining in this bounded scope. No new scoped Minor finding. Retained full-corpus/feature resource and real-data evidence gaps remain separate acceptance work.

### Focused verification

Specific doubt: exclusions must apply to both abbreviated and split-date candidates while preserving adjacent safe bands. Nine named fixture cases passed in 1.72 seconds without warnings. Exact command from C:/atx with PYTHONPATH=C:/atx/atx-db/src and OPENBLAS_NUM_THREADS=1:

```powershell
atx-db/.venv/Scripts/python.exe -m pytest -n0 -o addopts='' -p no:cacheprovider atx-db/tests/test_alpha_panel_ea_actuals.py::test_overlapping_quarter_ytd_duration_spans_reject_monetary_cells atx-db/tests/test_alpha_panel_ea_actuals.py::test_date_bearing_percent_change_headers_never_supply_monetary_actuals atx-db/tests/test_alpha_panel_ea_actuals.py::test_multilevel_percent_spans_exclude_dates_and_abbreviated_quarter_tokens atx-db/tests/test_alpha_panel_ea_actuals.py::test_aar_original_shaped_split_header_quarter_ytd_and_unknown_currency atx-db/tests/test_alpha_panel_ea_actuals.py::test_abbott_original_shaped_quarter_abbreviations_exclude_percent_columns atx-db/tests/test_alpha_panel_ea_actuals.py::test_quarter_column_conflict_overlap_and_rowspan_fail_closed -q --tb=short
```

Three additional tiny stdin assertions with the same interpreter passed: disjoint percentage [3,5) leaves 2Q15/2Q14 monetary [1,2)/[2,3) eligible; percentage [1,2) excludes split 2020 but leaves split 2019 [2,3); adjacent quarter [1,3)/YTD [3,5) admits only the two quarter year bands. These used synthetic header tuples only, with no source reads.

### Assessment

Scoped code-prebuild spec: Approved. Scoped quality: Approved. Prior round-2 I1/I2 are closed; final Task9 spec/acceptance remains pending at 0/5 gates and 0/200 manual checks. Round remains 3 of the 5-round cap, with acquisition authorization unchanged.
