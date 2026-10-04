# P9 A0 bounded comparison kernel

Status: authored, AST parsed; three focused physical tests NOT RUN under the
original40GiB disk hold. Independent T16 review requested. No actual A/B arm,
consumer differential or source-quality result exists.

New owned paths: private_identity_arm_comparison.py and its direct test. The
existing independently reviewed D0 kernel remains unchanged. The complete
owner request is acceptedv3,59,756B/SHA8f9916d59eb7f3b6f5c7b72768c3e4246c0d189dfbd42344330b57689f1d3321.

The conditional compare_arms API accepts only the four requested pairs. Caller
supplies exact arm PIT/link-table files, full original membership inventory,
calendar and independently frozen physical hashes. It authenticates those files
before/finally consumption, enforces a file-backed128MiB/one-thread/UTC database
and isolated exclusive output directory, and drops its uniquely named SQL
objects on success or failure. No source body, price, return or label is read;
membership projects only security_id/session_date/member.

Sparse per-year interval joins resolve consumer states at22:00UTC. Missing keys
represent zero CIKs; the complete union-SID/calendar axis count is reported
without allocating its dense cross product. Primary is any P among qualifying
P/J rows; current_ticker_verified is excluded; every class must pass common or
one permitted letter. Open ends and missing text follow the actual consumer's
null handling. Linked means one resolved CIK; ambiguous states are counted and
written separately, with all their CIK identifiers retained when a row1-4 cell
is affected. No ambiguous CIK is selected arbitrarily.

Rows1-6 count consumer-state changes and global distinct identifiers. Row7
uses true native membership and the exact2017-2019/2020/2021/2022/2023 buckets.
Rows8/9 compare every native PIT column using two-way EXCEPT ALL under the exact
start/available_at pre/post2024 predicates, retaining duplicate multiplicities.
Schema mismatch refuses a subset comparison. Row10 compares complete admitted
panel CIK sets on member sessions2018-2023, gating backfill with evidence_at and
other tiers with available_at; ambiguities remain explicit. All identifier-only
Parquet writes use deterministic ordering. Completion metadata is written only
after final input authentication and SQL cleanup.

Three prepared tests use physical typed Parquet: an independent small Python
consumer oracle covering every change category, ambiguity, mixed P/J, classes,
unknown text, open ends, subsecond cutoff, post2024 rows, duplicate full rows,
unchanged-state/changed-extra-column rows, membership and original panel clocks;
source mutation/finally refusal; duplicate membership and full-schema rejection.
No pytest result is claimed while these tests remain unrun.

This is a comparison kernel, not the complete A0 orchestrator. D0 must finish
before B comparisons, full same-code/source admission and row11 sanity remain
mandatory, and the B2 attribution aid/native-r4 provenance must still be
composed. Missing367original documents still prohibit actual arms. PIT-a/b/c,
T1-T4 and fixed-build acceptance are separate unfinished producer obligations.
A zero here alone cannot clear the later-evidence residual or inputs held fixed.

Independent T16 static review found Important I1: the first draft erroneously
treated NULL native panel valid_to as open-ended, unlike original panel BETWEEN
semantics. Failed review-p9-a0-arm-comparison-v1.md is preserved. The correction
uses the exact concrete endpoint comparison and rejects NULL native panel ends;
legitimate consumer PIT end_incl NULL remains supported separately. A fourth
physical regression checks failure before completion. Four tests remain NOTRUN
under the disk hold; scoped I1 rereview requested, no gate result claimed.

Scoped static I1 rereview PASS/PASS in review-p9-a0-arm-comparison-v2.md,
no new findings. Current module/test AST parsing passed. The first AST command
used an absent root-level interpreter path; the corrected atx-db interpreter
completed. No fixture result is inferred from static checks. Four physical
tests still require the original resource admission.
