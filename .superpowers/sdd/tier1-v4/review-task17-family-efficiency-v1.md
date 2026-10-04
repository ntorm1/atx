# Independent scoped family text-consumption review

Reviewed frozen commit 7b6e9aacd5bd5e050c4618c1eafbccb6dd07d9be.
Spec verdict: PASS. Quality verdict: PASS. No corrections required for this
scoped efficiency change. This is not approval of actual source qualification,
the complete Task17 integration, or any goal/coverage/statistical gate.

Scope is exactly the changes to private_tier1_family_sources.py,
test_private_tier1_family_sources.py and task17-family-sources-helper-report.md.
The reviewer owns none of those changes. The unchanged Task15 native reader was
read to compare the original iterator invariants; this review does not reopen or
self-approve the reviewer's original Task15 implementation. No product edits,
fixtures, source scans/hashes, HTTP calls, data jobs or index writes were made.

## Findings

None in scope. The full `_text` admission still calls unchanged
`T.read_native_generation`, whose original-byte replay, closed publication
inventory, units/checkpoint, complete metadata/product pins and source/code/runtime
checks are unchanged. The wrapper still requires exact complete dependency and
validation bindings before returning an admission (`admit`, lines 345–384).

The changed iterator (`iter_rows`, lines 388–410) deep-copies the admitted binding
before suspension. Complete helper checks and the connection/disk guard run before
reading. `_text_consumption_check` (lines 413–433) preserves the original native
iterator's manifest SHA, physical source-plan pin, files/code/runtime equality,
unchanged `N.check_plan` and selected product identity checks. It also checks that
the selected native product path remains the generation's fixed product path.
These checks now run before and after one consumption rather than for every batch.
The Arrow Parquet batches and typed `to_pylist` rows are the same native read.

Nested finally blocks execute the native, complete helper and resource checks on
normal exhaustion, downstream exception, Parquet error and explicit generator
close. A failing native final check cannot skip the other final checks. Interrupted
consumption does not return any successful complete-consumption certificate.
An integrity failure before the first read refuses consumption. Constant check
count replaces the repeated whole-source work; no source/code/product dependency
is waived, and full native derivation is retained at admission.

## Evidence checked

Read `git show 7b6e9aac` for the exact three-path diff, the unchanged native
`read_native_generation`/`iter_native_rows`, wrapper `_text`/`admit` and complete
dependency/review checks. `git diff 7b6e9aac --` the three reviewed paths was empty
at review time. No test was rerun during the root host-observation window.

The added tests use the original six-filing SEC-shaped native fixture, full
publication, full public native admission and file-backed 128MB/one-thread/UTC
consumer. Many-batch coverage compares all six typed rows across six batches and
counts exactly two calls to the unchanged native check. Its counting wrapper
delegates to the original function; it does not bypass admission. Mutating the
caller's source/product dictionaries after the first yield cannot redirect the
copied iterator. The early-close case changes an original filing body and requires
close to fail after its second original-source check.

Retained direct guard receipts 20261004T183003-12452.json and
20261004T183027-5136.json were read as small controller evidence. Their exact
commands select the two new singleton nodes with `-n 0`, empty addopts and no cache
provider. Both report completed/exit 0/no cap or hits: 11s/219,705,344B and
9s/219,930,624B respectively. The author report distinguishes pytest 9.89s/7.99s
from those guard durations and retains both earlier pre-child low-disk refusals
as NOTRUN. These offline fixture results support engineering behavior only.

Actual original sources/populations/clocks/economics, 95%/90% Task15 gates, complete
consumer wiring and all other original release/manual/statistical gates remain
open as previously recorded. This scoped PASS does not promote them.
