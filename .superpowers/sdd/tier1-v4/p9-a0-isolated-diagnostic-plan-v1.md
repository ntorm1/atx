# A0 isolated diagnostic implementation boundary

Status: implementation design, not executed or approved arm code. The full R28
request remains binding. Actual missing document originals are tracked by the
retained-provenance and local recovery reports; no arm starts without the full
native input guards. Code work can proceed independently of that recovery.

## Isolation

Use a new checked source copy under this goal's own diagnostic directory in
`C:/atx`. The existing resume contract forbids worktrees and branch switching.
Never edit the shared `common.py`, alpha `identity_links.py`, or the financial
candidate's source snapshot to add the diagnostic. The financial publisher
explicitly pins alpha `identity_links.py` through `code_paths`; current retained
financial units must remain admissible under their existing pins.

Freeze the source inventory before creating the copy, copy only regular Python
source files and required explicit resources, and verify each original before
and after copying. Record original and modified hashes and exact patches. No
virtual environment, source dataset, cache or `.git` copy is needed. Use the
existing interpreter. Do not import mutable shared source through a fallback
package path: every imported project dependency must belong to the checked copy.
The runtime wrapper must authenticate the complete copy before and after every
phase, in addition to all unchanged native source/code guards.

The copy must retain original source locations for companyfacts staging, FSDS
and FINRA. Bind these as explicit fixed read-only inputs; do not accidentally
resolve them beneath the copy. Pin the original root's identity-independent
prices, calendar, short-interest, panel-core and membership inputs once, then
verify their complete snapshots around consumption. Any space-saving link must
have explicit read-only use and source identity checks; do not introduce a
writable alias to original data. All writable DuckDB files, spill directories,
logs and outputs must resolve inside the named arm's directory, with reparse
paths refused. No run may target published `alpha_panel/v1`, financial stages,
or the P9 worktree.

## Same code, explicit arm configuration

One code-copy digest serves all four arms. New configuration binds the arm,
preparation/rehearsal/output roots, switch values and exact input pins. The
native preparation pin must also bind the S1 value, so an interrupted B2 cannot
reuse A preparation under identical source descriptors. This adds provenance;
it must not weaken the native source checks or change scoring semantics.

| Arm | S1: reconstruction ticker list empty | S2: backfill candidates empty | Rehearsal |
| --- | --- | --- | --- |
| A | No | No | Fresh A |
| B1 | No | Yes | The same completed A |
| B2 | Yes | No | Fresh B2 |
| B12 | Yes | Yes | The same completed B2 |

S1 changes only the `reconstruct_in_batches` ticker argument. The candidates
phase still receives the complete original nonempty directory/ticker inputs.
S2 empties the candidate relation before both backfill passes and preserves its
typed schema and ordinary downstream logic. S3 binds the selected rehearsal
and an isolated root. S4 records the complete existing expected/got/pass seal
validation and allows a mismatch to continue only under authenticated diagnostic
configuration. Every other validation and hash guard retains its normal failure
behavior. Shared production behavior remains unchanged.

Run vendor, facts and reconstruct serially in fresh processes for A and B2;
then symbols, candidates, uncertainty, links, names and export in fresh
processes for each rehearsal. The export must be attempt001. If it fails, retain
the failure and start the six phases in a new rehearsal directory. Do not rename
attempt002 or present a reused failed output as the required first attempt.
Run identity, backfill, names, combine, identity_table in the original stage
order and with the original stage arguments for every arm. The existing general
build runner resets PYTHONPATH to shared source, so the diagnostic wrapper must
explicitly execute these five stages with the authenticated copy's import path;
using that runner unmodified would defeat isolation.

## Evidence before interpretation

Tests must cover switch-off behavior, B2 affecting reconstruction only, S2
affecting both passes, mismatched resume configuration, root escape/refusal,
changed source/code pins, normal validation refusal versus diagnostic recording,
and every requested T1-T4 PIT regression. Do not use a fixture PASS as actual
source qualification. Bounded phase wrappers and differential scans need their
own operational verification, followed by independent review.

Measure D0 before reading B comparisons: full r4 rows keyed by TABLE_KEYS with
only created_at excluded, full PIT/link-table rows, and A's full Stage I
validation. Preserve nonzero D0 and continue the authorized comparison rather
than rewriting the baseline. Establish all requested non-vacuity checks before
interpreting differences. Compute the complete four-comparison table and
identifiers from the requested session/availability/class/primary conditions,
including all specified year buckets and pre/post2024 seals. Use disk-backed
bounded SQL and immutable intermediate partitions; no whole-panel dataframe.

Missing original documents, an unverified input receipt, or absent complete
source snapshots prohibit arm execution, but do not prohibit implementing and
reviewing the diagnostic/PIT code. No failed gate becomes a PASS from this plan.
