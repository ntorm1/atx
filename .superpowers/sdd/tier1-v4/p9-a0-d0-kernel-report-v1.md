# A0 bounded D0 comparison kernel

Implemented `private_identity_differential.compare_d0` as a separate diagnostic
module. No shared identity producer, common module, financial pin or published
data changed. The caller supplies the two explicitly pinned files and a fixed
product kind (`r4`, `pit`, `link_table`). The kernel checks raw hashes before
and in `finally` after comparison, uses bounded disk-backed SQL, and cleans its
private temporary views on success and failure.

It compares full-row multisets using EXCEPT ALL. Only r4 excludes created_at;
PIT and link_table exclude nothing. A schema change produces explicit failed
D0 with undefined row differences, rather than comparing a common subset.
r4 duplicate and NULL TABLE_KEYS are separately counted and fail D0 even when
the two multisets agree. One-thread/UTC, <=128MiB database memory, file-backed
database and configured spill storage are enforced. Source quality remains
false regardless of the row comparison.

All three new physical Parquet fixtures passed in0.85 seconds: r4's exact
created_at exclusion, same-key/full-row changes and duplicate keys; PIT
multiplicity/schema changes and stale pin refusal; and source mutation during
comparison with failure-path view cleanup. The exact module command used
`-x -q -n0 -o addopts='' -p no:cacheprovider`; bytecode writes disabled.
Guard `logs/p9-a0-d0-kernel-v1.receipt.json` completed exit0/2 seconds at
64,602,112 peak job bytes under unchanged0.6GiB/40GiB limits, no override or cap
hit. These are synthetic implementation checks only.

Still required: independently reviewed orchestration tied to the exact full
published manifests and isolated arm outputs, same-code S1-S4 controls, complete
original source restoration/admission, actual D0 and all four-comparison tables,
PIT-a/b/c repairs and T1-T4 tests. This helper does not establish an arm result,
producer provenance or production/coverage/evaluation/consumer acceptance.
