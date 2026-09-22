# Reported EPS core bridge — durable task state

Updated 2026-09-22 while the archive9 loader is active.

## State

- Static isolated draft is complete.  No production-source file, database,
  import, runtime, test, registry, job, or activation hook was changed.
- Ready artifact: `integration-final.patch`; `git apply --check` passed from
  repository root `C:/atx` without applying it.  SHA-256 is
  `9D4BC4F7A147FAA1D73853008D36EAB8685A7CA1BAAA548169BB230E94F1012B`.
- Migration body `0321` is included but deliberately unregistered.  The
  source-0320 owner retains registry/jobs/activation serialization; root must
  register 0321 only after 0320 is finalized.
- The draft includes copied `item_coverage.py` and `provider_coverage.py` only
  for the narrow latest-state-before-usability changes.  `api/catalog.py` is
  not included because the issuer-query owner updated the matching public type
  contract in its owned draft.

## Required next actions

1. Fresh static review of `integration-final.patch`, focusing on the SQL CTE
   column contract, nullable publish shadow behavior, equal-clock ordering,
   source lineage, and exact period duration conditions.
2. After the loader is terminal and source-0320 is integrated/registered,
   register 0321 and run the focused tests in the implementation report.
3. Do not apply either temporary `eps-patch-old/` or `eps-patch-new/` folders;
   they were only used to generate the patch and are outside its scope.

## Repair state, 2026-09-22

The prior SHA and “ready” statement describe the reviewed, superseded patch.
The current patch includes the Critical SQL repairs, accepted-receipt
qualification, migrated-schema integration tests, and public-module snapshot.
`git apply --check` is the only executable validation performed during the
loader run. No production source, Python import, tests, database, or runtime
were touched. Root should obtain one Critical-only static rereview of this
patch, then integrate source 0320 before registering 0321 and run the focused
test under its guarded runtime gate. The 2 GiB process-tree peak remains
unmeasured.
Current patch SHA-256:
`F6E6712CB81FC9F25FEFBD80372B3CE9313B8B5D0CD4007254D355671D3D777A`.
