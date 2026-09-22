# Verified CompanyFacts resume: prefix-cost static audit

**Scope.** Static source review on `feat/tier1-parity`; no database, archive,
runtime, profiler, import, or test was run. This report uses the observed
archive6 facts supplied for this audit: proof completed at 2026-09-21
22:00:42.881 UTC (6,570 loaded receipts / 31,008,510 retained rows), and by
22:15:54.149 UTC the loop had visited 7,700 archive entries (6,554 verified
loaded, 1,107 empty, 39 unavailable; no newly attempted rows). The elapsed
time and CPU increase establish that the prefix is costly; they do **not**
attribute a percentage of that cost to any individual operation below.

## What the source necessarily does

`SecCompanyFactsDataset.load` performs archive hashing and inventory before it
calls the verifier (`atx-db/src/atx_db/fundamentals.py:1115-1146`).
`verify_companyfacts_resume` then reads compatible receipts, scans the full
`sec_company_facts` CIK counts, computes grouped SHA-256 multiset sums for all
SEC facts and all SEC points, and recovers unresolved summaries
(`atx-db/src/atx_db/_companyfacts_resume.py:150-275`). Those are deliberate
full-retained-state proof operations. They had completed before the observed
prefix, so they cannot explain the subsequent 15-minute interval, although
they remain a material up-front resume cost.

After proof, the loop has three relevant paths:

1. **Verified loaded member.** The `cik in verified_members` branch does not
   parse that ZIP member or replace facts or points. It
   still recreates an unresolved-candidate frame when required, then opens a
   transaction and calls `_replace_companyfacts_candidates`; the ordinary
   case opens the same transaction with an empty DataFrame
   (`fundamentals.py:1160-1181`). That helper deletes the CIK's existing
   candidate rows using a regex/cast predicate and then invokes `insert_frame`
   (`fundamentals.py:621-631`). This cleanup is semantically required: CF5's
   regression test explicitly models missing candidate output after committed
   facts and requires verified resume to restore it
   (`tests/test_companyfacts_archive_repair.py:75-133`).

2. **Empty member.** Empty receipts are intentionally not completion evidence
   and are replayed (`_companyfacts_resume.py:162-164`). A non-placeholder ZIP
   member is read in full and JSON-decoded (`fundamentals.py:970-986`),
   normalized (`:1210-1218`), identity-resolved (`:1250-1264`), classified,
   and sent through `_replace_facts` (`:1276-1315`). Even with no new rows,
   `_replace_facts` creates three temporary relations, deletes matching prior
   points and facts, reconciles candidates, writes the receipt, and commits
   (`:1412-1479`). This is the required zero-row replacement cleanup; it must
   remain in production.

3. **Unavailable `{}` placeholder.** The fetcher recognizes the exact
   two-byte placeholder (`fundamentals.py:977-985`); the loop records an
   `unavailable` receipt and preserves prior issuer data without a replacement
   transaction (`:1232-1249`). These 39 visits still read the ZIP entry, but
   do not take the fact/point cleanup path or increment the reopen counter.

## The avoidable-looking source pattern

The memory-bounding counter is incremented for **every verified loaded
member** after its candidate-only transaction (`fundamentals.py:1173-1177`),
as well as after every replayed completed member (`:1304-1313`). On the next
iteration after ten counted members, the loader calls
`reopen_companyfacts_store` (`:1152-1155`). That helper calls
`store.close()` then `store.reopen()` (`_companyfacts_resume.py:278-290`), and
`DuckDBStore.close()` issues `CHECKPOINT` before closing
(`connection.py:105-116`).

Therefore the observed 6,554 verified members necessarily incurred 6,554
small transactions/candidate reconciliations and drove roughly 655
checkpoint/close/reopen cycles before counting any replayed empty members.
Including the stated 1,107 empty members gives 7,661 counted completions,
which implies roughly 766 interval cycles already encountered in this prefix,
plus the mandatory one recycle immediately after proof (`fundamentals.py:1142-1144`).
The exact live count depends on the loop's current boundary and is not inferred
from source alone.

The ten-target cadence is documented as protection against retained raw
fact/index state after issuer replacements (`fundamentals.py:66-68`) and is
covered for raw writes by CF5 (`tests/test_companyfacts_archive_repair.py:368-393`).
A verified member deliberately leaves raw fact rows, point rows, receipt
ownership, and archive identity untouched. Its only mutation is candidate
reconciliation. Source therefore supports the distinction; it does not prove
that candidate-table mutations cannot themselves need periodic recycling.

## Narrow, correctness-preserving follow-up worth considering

Create a bounded task to split the existing counter into a **raw
fact/point-replacement recycle counter** and verified-member candidate cleanup.
Keep each verified member's candidate delete/insert in its own transaction,
keep every empty replay on the raw replacement counter, keep the post-proof
reopen, and keep the final post-raw-work reopen. Do not change receipt
admission, archive identity checks, multiset proof, source scope, empty
cleanup, atomicity, or clocks.

The minimal implementation hypothesis is that the verified branch at
`fundamentals.py:1176` no longer advances the raw replacement counter; the
existing increment after `_replace_facts` remains. The benefit to measure is
elimination of checkpoint/reopen cycles induced solely by already-proven raw
members. Candidate reconciliation remains intact, so this does not turn a
verified resume into a no-op.

Risks: the original cap may also have been shielding candidate-table delete
state, and moving the counter changes when a process is checkpointed. The task
must retain per-member transactions and fail loudly on any reconciliation
error; batching cleanup across CIKs or removing it is not justified by this
audit. Do not claim a wall-time win until a guarded comparable resume measures
proof time, verified-loop time, empty-loop time, reopen count, DuckDB memory,
and final ledger/fingerprint equality.

Focused verification for that task:

- extend the CF5 archive-resume fixture with more than ten verified members
  plus a replayed empty member and a failed/unavailable tail;
- assert candidate rows are recovered/cleared exactly as today and verified
  fact/point rows and original source receipts remain byte-for-byte unchanged;
- instrument `close`/`reopen` to prove the post-proof recycle remains, verified
  members alone do not consume the raw-replacement cadence, and an empty
  replacement still does;
- retain the existing cap/session/temp-relation checks and add an interruption
  case around candidate reconciliation so its per-issuer atomic behavior is
  explicit.

No other optimization is recommended from static evidence. In particular, the
full proof scans precede the observed interval, and ZIP/normalization plus
zero-row replacement for the 1,107 empty members are required production work
rather than safe targets for removal.
