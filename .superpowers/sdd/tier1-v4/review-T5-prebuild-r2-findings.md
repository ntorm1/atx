### Spec Compliance — ✅ Approved for the scoped prebuild code checkpoint (fix round 2/5)

Reviewed commit 5ca0e6db0a46c63ac8f2d3bf58b864474ee46b60 through the supplied 1,078-line review-T5-prebuild-r2.diff, the newest Task5 report section and the two remaining Important findings in review-T5-prebuild-r1-findings.md. Scope is retained I4/R1 metric histories and R2 exact extract completeness. Earlier component, TEQ, wrapper and IB closures remain retained; this package establishes no specific fix-induced risk that reopens them. No foreign/shared-registry path appears in the package. No real-data jobs, full suites, source acquisition, producer writes, commits or children were run.

| Remaining finding | Disposition | Exact evidence |
|---|---|---|
| R1, retained I4: average/change contracts omit exact history | ADDRESSED | t5-requirements.py:88 selects exact TTM metric inputs; :105–115 binds prior AT for ROA/accrual quality and prior SEQ/TXDITC for ROE at the same fiscal quarter four quarters earlier. :116–129 binds each of seven changes to its exact base formula and current/prior endpoint inputs. ROA/ROE changes explicitly require stock history four quarters before each endpoint, including eight quarters before the current endpoint. Generated JSON agrees with these bindings. ROIC/ROCE prerequisite names are explicitly UNPROVIDED/UNSUPPORTED, with prior capital dependencies, rather than claimed published equivalents. |
| R2: duplicate declared batch can omit approved data and pinned plan | ADDRESSED | fund_depth.py:112–117 checks actual data member names equal the approved set and requires the approved plan/hash. :125–134 checks member shape, receipt/data stem equality, uniqueness, each data hash/receipt size and exact declared-set equality. Destination creation and links begin only at :136 after all source validation. Metadata remains copied; data alone is hardlinked. Four malformed-snapshot regressions assert rejection before destination creation. |

⚠️ This verdict concerns code readiness for the coordinated prebuild checkpoint. Actual fundamentals_v11/compustat_std_v11/ratios_v11 publication and output SHA/size integrity, independent retained-source checks, cutoff/source parity, coverage deltas, 105/117 coverage, ≥98.5% balance identity and mega-alpha gates remain unmeasured. Task4 remains OPEN with 0/6 acceptance gates PASS. Neither this review nor fixture success establishes warehouse spec completion or final acceptance. Final producer and coordinated universe snapshots and the required source/model/comparison qualification remain mandatory.

### Strengths

Source snapshot validation now fails before reuse writes for a duplicate reference, missing approved batch, missing pinned plan or changed plan. The contract fixes specify actual economic inputs and the nested history needed by profitability changes without replacing missing prior values. Unsupported ROIC/ROCE economic prerequisites are explicit, preserving the distinction between contract completeness and recovered source availability.

### Issues — Critical / Important / Minor

None remaining in this scoped round. Both Important findings from round 1 are closed; broader actual-data requirements remain pending evidence rather than silently waived.

### Focused verification

Six named offline cases passed in 0.90 seconds; captured output contained no warnings. These cover valid source reuse/hash rejection, four parameterized incomplete snapshots and exact prior-dependent average/change contracts. Command from C:/atx, with PYTHONPATH=C:/atx/atx-db/src and OPENBLAS_NUM_THREADS=1:

```powershell
atx-db/.venv/Scripts/python.exe -m pytest -n0 -o addopts='' -p no:cacheprovider atx-db/tests/test_alpha_panel_fund_depth.py::test_extract_reuse_verifies_receipt_and_excludes_mutable_parts atx-db/tests/test_alpha_panel_fund_depth.py::test_extract_reuse_fails_closed_on_incomplete_approved_snapshot atx-db/tests/test_alpha_panel_fund_depth_publish.py::test_prior_dependent_average_and_change_contracts_bind_actual_history -q --tb=short
```

The implementer reports 23 PASS in 6.58 seconds, 16 PASS in 2.21 seconds and 5 PASS in 1.24 seconds; those broader counts were not rerun. No generator or source-data build was executed for review.

### Assessment — Task quality: Approved for the scoped prebuild code checkpoint

Spec checkpoint: Approved. Quality checkpoint: Approved. The two remaining code blockers are repaired with direct regression evidence; proceed to the coordinated guarded publication workflow, while treating every real acceptance gate as pending until actual outputs and source/value/coverage evidence are reviewed.
