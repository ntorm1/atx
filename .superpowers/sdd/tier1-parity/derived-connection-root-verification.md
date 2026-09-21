# DP1 root verification

2026-09-21 UTC. One review found1Important/1Minor; fix1 snapshot enumeration
and settings-oracle changes accepted on implementer report. Focused run20passed,
2new lifecycle fixtures failed during invalid input seeding. Fix2 used existing
AF1 nonfinite-input convention, then exposed missing quarterly fiscal spans.
Fix3 supplied the actual contiguous spans and corrected one lint context shape.
Root accepted reports and reran only2affected cases: BOTH PASSED, peak0.7244GiB.
Initial full focused peak0.7956GiB. Production code/arithmetic unchanged by fixes.
Strict mypy derived_metrics.py passed. Touched source/test Ruff passed in the
combined capacity check after MP1's unrelated raw-regex spelling correction.
All runtime serial under2.5GiB guard; full production capacity still unmeasured.
No full-suite run or additional review occurred. Source retains all states,
annual fallback, stale scope cleanup, one ID snapshot and bounded page lifetime.
