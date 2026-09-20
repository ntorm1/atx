# SEC companyfacts exact empty archive members

## Result

Exact two-byte `b"{}"` members in a local companyfacts ZIP now produce an explicit
unavailable source disposition. They cannot replace or clear retained facts,
fundamental points, or identifier-resolution candidates. Usable archive members
continue loading. Real source failures continue to fail activation, while
unavailable placeholders alone do not make execution fail or claim coverage.

## Source evidence

The first inspected member, `CIK0000003521.json`, contained exactly `{}` and no
CIK, name, or facts. Root's later inventory found 62 exact two-byte placeholders
among 20,390 main members and no other members of two bytes or less. At inventory
time those 62 CIKs had zero retained fact rows; the implementation and tests still
require preservation when history exists. The inventory is not evidence of
issuer eligibility or financial coverage.

Evidence remains in `companyfacts-empty-member-inspection.json`,
`companyfacts-archive2-stop-inspection.json`, and
`companyfacts-placeholder-inventory.json` in this directory.

## Implementation and accounting

`atx-db/src/atx_db/fundamentals.py` returns a private placeholder marker only when
the ZIP member name satisfies the existing exact CIK member pattern and its raw
bytes equal `b"{}"`. Network JSON objects cannot enter this path. Whitespace
variants, nonempty objects missing facts, null/non-object facts, mismatched CIKs,
non-object payloads, and malformed JSON keep their existing failure semantics.

The loader records a source receipt with status `unavailable`, zero observed rows,
the reason `empty_archive_placeholder`, member name and byte size, CIK, run ID,
and archive/allowlist hashes. The receipt write is outside the catch that skips
source failures, so a database write failure remains fatal. Processing then
continues before identifier resolution, the throughput CIK inventory, financial
replacement, or candidate collection.

Unavailable targets have separate counts, reason counts, and a bounded sample.
They increment neither loaded nor completed targets and never count retained
facts as newly loaded. The outcomes `source_unavailable` and
`loaded_with_unavailable` distinguish empty-placeholder-only and mixed usable
loads. Actual failures still take outcome precedence. Existing valid empty
payloads retain their authoritative empty-replacement semantics.

`coverage_warnings` reports preserved prior data for empty archive placeholders.
A separate `source_availability` warning records the unavailable count; the
existing `rows_loaded` threshold is unchanged. Listed-security coverage stays
unverified. No activation code, quality threshold, universe certificate, archive
discovery rule, unknown-fact filtering, or previously committed throughput
inventory/issuer cleanup was changed.

## Validation

Root ran the following focused command under its 2.5 GiB memory guard:

```text
C:/atx/atx-db/.venv/Scripts/python.exe -m pytest -n 0 -q tests/test_companyfacts_empty_members.py tests/test_companyfacts_zip.py tests/test_companyfacts_resilience.py tests/test_companyfacts_cik_spellings.py
```

All 56 cases passed with exit code 0. The native peak job memory was
0.6872520446777344 GiB. The preserved receipts are
`companyfacts-empty-members-tests.log`,
`companyfacts-empty-members-tests-memory.json`, and
`companyfacts-empty-members-tests.err` (empty stderr).

The new file contributes 19 cases: retained canonical and legacy identity
preservation, exact source metadata and warning, no financial-path calls,
source-receipt database failure, mixed usable facts, eight excluded invalid
representations, both ordinary network failure modes, existing valid-empty
replacement, and three activation outcomes. The existing test fixtures and
assertions required no relaxation.

Static `git diff --check` found no whitespace errors. Authorized touched-file
Ruff passed for the implementation and new tests after removing one extra blank
line after the test imports; this was the only post-test adjustment. Independent
fresh review is CLEAN with no Critical or Important findings; its receipt is
`companyfacts-empty-members-review.md`. This agent performed source edits and
static inspection only; root owned all imports, tests, database access, probes,
and network activity.

## Limits

Completed execution does not establish source or listed-security coverage.
Retained financial data for an unavailable CIK remains historical data, and the
unavailable receipt describes only the current source observation. This change
does not authorize clearing that history or treating other representations as
placeholders. The production archive retry remains root-owned and has not been
claimed as complete by this report.
