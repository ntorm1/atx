# VR1 root integration and verification

Baseline: committed `216fd7dd`, with production source last changed in
`cd841f26`. Integration occurred only after archive7 was terminal, its owned
processes absent, and both interrupted ledgers recovered from measured rows.

Applied the reviewed `integration-final.patch` SHA256
`0D268C57499B9738DA4FD85AC4B0ABD2DB011479C2BF3185065CE7E872FB4E34`.
The live production source matches the frozen reviewed draft when CRLF/LF
differences are ignored. No production semantics changed after that review.

## Change

Raw replacements, including empty cleanup, still recycle after ten committed
issuers. Verified candidate-only transactions have a separate bounded counter
of100. Either recycle resets both counters. Post-proof and final partial
recycling remain; hashes, lineage, source receipts, ownership, atomicity,
configured-session eligibility and temp-state protections are unchanged.

## Focused runtime evidence

The six prescribed selectors ran once under a2.5GiB process-tree guard:
five passed; the primary mixed-resume case failed at a stale expected row
count after its four actual close-state assertions passed. Native peak was
0.698471GiB. Evidence: `vr1-focused.log`, `.err`, and `-memory.json`.

The original implementer corrected only fixture oracles: ten deliberately
replayed issuers plus one new issuer contribute11 replacement fact rows,
190 members remain verified, and the empty target brings total replayed
members to12. See `verified-resume-recycling-runtime-fix-report.md`.
No ownership, cadence, configuration, candidate, or rollback assertion was
removed or weakened. This Important fixture correction was accepted on report.

Only the affected primary selector was rerun. It passed in full under the
same2.5GiB guard, native peak0.695942GiB. Evidence: `vr1-affected.log`, `.err`,
and `-memory.json`. The other five unchanged cases were not repeated.

C1 was already closed by the scoped Critical re-review; the runtime also
exercised the real configured reopen path and all four states. No further
review pass was needed for the fixture counting correction.

## Static checks and limits

- `git diff --check` passes for both changed implementation/test files.
- Scoped Ruff passes for `fundamentals.py` and its changed test file.
- `mypy --strict --follow-imports=silent src/atx_db/fundamentals.py` reports
  four errors. The same command against an isolated `git archive` of
  `216fd7dd` reports the identical four diagnostics at the corresponding
  unchanged statements: unannotated `verified_members`; None assigned to an
  inferred string; optional archive payload indexing; optional archive path
  passed to the identity helper. Baseline lines1109/1275/1277/1316 become
  current1113/1284/1286/1325. No new diagnostic was introduced. The baseline
  guarded run peaked0.338882GiB; its log and guard receipt are preserved.
- No full suite was run. No production speedup or capacity improvement has
  yet been measured. A necessary subsequent resume will supply that evidence.

Only `fundamentals.py` and `test_companyfacts_archive_repair.py` are production
code/test changes. No registry, activation, jobs, migrations, raw schema, or
source-data ownership changes belong to VR1.
