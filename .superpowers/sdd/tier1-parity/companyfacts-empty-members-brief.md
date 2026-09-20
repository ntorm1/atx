# Companyfacts archive empty-member brief

## Observed evidence and scope

The inspected SEC archive member `CIK0000003521.json` is exactly two bytes,
`{}`. It supplies no CIK, entity name, facts, or authority to remove retained
financial data. The recorded attempt classified that member as a `ValueError`
(`companyfacts payload requires a facts object`). The accompanying inspection
records 987,977 retained attempt rows from 146 loaded CIKs, plus 19 existing
`empty` source outcomes and one error. These observations establish one exact
archive placeholder; they do not establish that other empty or malformed
members are valid placeholders.

Root's subsequent `companyfacts-placeholder-inventory.json` reports 62 exact
`b"{}"` members among 20,390 main members, no other members of two bytes or less,
and zero retained fact rows/CIKs for those 62 members at inspection time. This
inventory establishes byte-level source absence only, not issuer eligibility or
financial coverage. Preservation remains a required behavior for future runs.

## Smallest implementation plan

1. Recognize only an archive member whose uncompressed content is exactly
   `b"{}"`, after resolving and validating its member filename against the
   requested CIK using the existing archive rules. Give that member a distinct
   unavailable/empty-placeholder disposition before normal payload parsing or
   replacement. Do not accept general missing-facts objects as placeholders.
2. Record the archive source URL/member identity, existing checksum metadata,
   explicit placeholder outcome, and zero rows for that observation. Preserve
   all preexisting facts, point rows, and fundamental candidates for its CIK;
   skip every fact write, delete, candidate replacement, and loaded-CIK count.
3. Count placeholders separately from loaded, valid-empty, missing-member, and
   failed outcomes. A placeholder is observed source unavailability and is not
   a failure of execution. Keep invalid payloads as failures. Surface a coverage
   warning and placeholder counts without changing quality thresholds or
   treating completed execution as passed coverage.
4. Keep ordinary network `{}` responses, nonempty invalid payloads, mismatched
   CIKs, malformed JSON, and non-object payloads on the existing failure path.
   Preserve existing valid-empty payload semantics.

## Focused test scope

- An archive containing one exact `{}` member and one valid member retains old
  facts, points, and candidates for the placeholder while loading usable data.
- Placeholder-only loading performs no financial writes, reports zero loaded
  rows/CIKs, and records an explicit unavailable outcome and coverage warning.
- Source metadata identifies the archive member and checksum truthfully.
- Network `{}`, archive nonempty/malformed/mismatched objects, and invalid member
  identities remain failures and preserve old data.
- Existing valid-empty payload handling remains distinct and unchanged.
- Activation may complete execution with placeholders but must not manufacture
  coverage passage; only add activation tests if its existing aggregation needs
  a change.

## Semantic tradeoffs and boundaries

The two-byte check intentionally excludes whitespace variants and any other
unobserved representation. It limits this exception to the source condition
actually inspected. Preserved records remain historical data rather than
evidence that the current archive supplied facts for that CIK. A separately
reported unavailable outcome avoids conflating this condition with authoritative
valid-empty results or claiming fact coverage. The scope does not relax archive
validation, silently discard unknown facts, reinterpret missing facts generally,
or certify full-universe readiness.

## Execution ownership

Root owns runtime, imports, tests, database access, probes, and network activity.
At this stage this agent may prepare only this report and a new
`tests/test_companyfacts_empty_members.py`. `fundamentals.py` remains locked by
the throughput task; `activation.py` requires a separately serialized root
lock. Implementation, independent fresh review, and root-run focused cases must
precede an exact-path commit with the required co-author trailer.

## Prepared implementation

After root granted the `fundamentals.py` lock following throughput commit
`4ff0e9c2`, the implementation added a private archive-placeholder marker emitted
only for exact `b"{}"` bytes under an existing exact CIK member name. The loader
records the unavailable source receipt outside its skippable source-error catch
and continues before normalization, identifier resolution, CIK inventory, fact
replacement, or unresolved-candidate collection. Database receipt failures
remain fatal.

The output contract now supplies `unavailable_target_count`, reason counts, a
bounded target sample, and `coverage_warnings`. Source metadata identifies the
member, its two-byte size, archive/allowlist hashes, and zero observed rows.
`completed_targets` keeps its existing successful-payload meaning, while loaded,
empty, failed, and unavailable counts stay distinct. Outcomes are
`source_unavailable` or `loaded_with_unavailable`, with real failures retaining
precedence. A separate `source_availability` quality warning leaves the existing
`rows_loaded` threshold unchanged. Listed-security coverage remains unverified.
Activation's existing execution rule requires no change.

The new focused file contains 19 parameterized cases, including canonical and
legacy retained identities, source-receipt database failure, usable mixed data,
eight non-placeholder invalid representations, both network failure modes,
authoritative valid-empty replacement, and three activation outcomes. Existing
ZIP tests need no edits: their `{}` invalid fixture is a nested selected concept,
not an exact top-level archive placeholder.

## Current validation status

Static diff inspection and `git diff --check` completed without whitespace
errors. Root-run focused tests passed all 56 cases with exit code 0 and native
peak job memory 0.6872520446777344 GiB; see the implementation report and
`companyfacts-empty-members-tests*` receipts. Independent fresh review is CLEAN
with no Critical or Important findings. This agent has performed no import, test,
database operation, probe, or network request. The runtime ownership rule remains
in force.
