# P9 A0 arm-comparison scoped independent review v2

Spec verdict: PASS (scoped static review).

Quality verdict: PASS (scoped static review).

Scope: Important finding 1 from review-p9-a0-arm-comparison-v1.md and new breakage in its correction only. The v1 findings remain preserved. No test, source, hash, data, index or implementation job was performed.

## Finding disposition

1. **Important 1 — ADDRESSED.** `atx-db/src/atx_db/alpha_panel/private_identity_arm_comparison.py:127` now uses exact `BETWEEN p.valid_from AND p.valid_to`; it no longer invents a permanent endpoint. Native panel input validation at line204 explicitly rejects NULL `valid_to` before comparison. The legitimate PIT bridge `end_incl=NULL` convention remains unchanged. `atx-db/tests/test_private_identity_arm_comparison.py::test_null_native_panel_end_refuses_permanent_coverage` changes the physical panel endpoint to NULL, updates the independently expected physical pin, requires the native-panel refusal and asserts no successful counts receipt. This directly covers the previously omitted distinction.

No new Important or Minor finding was identified in this correction. Concrete inclusive endpoints, native panel clocks, PIT open ends, full-row multiplicity, ambiguity interpretation and conditional result claims are unchanged from the checked v1 contracts.

## Evidence and limits

All four authored physical test nodes remain NOT RUN under the original disk-floor hold. This verdict records static correctness of the scoped fix, not an inferred test result or actual source/arm admission. The v1 report's external same-code, original-source/document/receipt, complete product inventory, D0, sanity/row11 and consumer prerequisites remain binding; no gate or acceptance is promoted. My capsule implementation is outside this review and receives no approval here.
