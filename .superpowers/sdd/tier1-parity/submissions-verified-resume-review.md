# SEC submissions verified resume — independent static review

Reviewed the uncommitted `sec_submissions.py` and `activation.py` changes, the new
resume tests, the implementation brief, and the supplied interruption/parameter
receipts. No implementation report existed at review time. Static review only:
no imports, tests, database access, probes, or network calls were performed.
The controller's reported test results were not independently rerun.

## Critical

None found.

## Important

### I1 — Partial bulk loads publish full completion

**Location:** `atx-db/src/atx_db/sec_submissions.py:663`.

The new `scope_complete` predicate checks unrestricted CIKs, processed main
members, and missing history, but ignores `options.forms` and
`options.include_history_files`. Consequently the existing default form-filtered
load reports `scope_complete=true`. A load with `forms=()` can report true with
zero filings, and an all-form load with `include_history_files=False` reports
true while omitting referenced history entirely. This flag is also persisted in
source metadata and quality-check details, so it falsely certifies corpus
coverage outside the activation wrapper even though explicit resume itself
rejects those scopes.

Require `options.forms is None` and `options.include_history_files` when setting
the full-scope completion flag. Add focused assertions that a form filter and
disabled history each leave the flag false, while the existing successful
unrestricted resume remains true.

### I2 — Forced activation destroys the receipt required by resume

**Locations:** `atx-db/src/atx_db/sec_submissions.py:381` (pre-attempt receipt
requirement), `atx-db/src/atx_db/activation.py:489` (archive receipt write), and
`atx-db/src/atx_db/warehouse.py:94` (replacement of the stable receipt ID).

Running activation with `--force --submissions-resume-from-run-id <failed UUID>`
and the default stage order reruns `sec_bulk_download` before `submissions_load`.
Even when the cached archive is unchanged and no download occurs,
`_download_archive` calls `record_source_file`, which deletes the existing
receipt and recreates the same ID with a new `fetched_at`. The only matching
receipt is now later than the failed attempt's start, so `_verify_bulk_prefix`
rejects the resume. A later submissions-only retry also fails because the old
receipt has already been removed. This breaks a supported activation option
combination and discards the evidence needed to reuse the retained prefix.

Preserve/reuse the existing receipt when the cache path, hash, and byte count
are unchanged, or explicitly protect the receipt before a resume invocation can
rerun the download stage. Do not assign an old timestamp to newly changed bytes.
Add a regression covering an unchanged cache hit between the failed load and
its resume, and retain rejection when the archive bytes change.

## Reviewed guarantees and limits

The prefix comparison checks the ordered archive membership through the proposed
boundary, including empty main members and referenced history, against exact
retained accession/security/form/source tuples. It checks failed compatible
lineage, complete nonempty-CIK batch boundaries, and temporal ancestry before
skipping rows. Verification precedes resumed filing writes; the existing
transaction encloses batch replacement. Python retrieval is bounded to 4096
retained rows plus one CIK's source keys, with the ordered SQL query using the
caller's existing budget. New row counts remain separate from verified prior
counts. The changes do not rewrite ticker history or establish historical
point-in-time identity.

Accept Important repairs on the implementer's concrete report under the agreed
one-pass review policy; no Critical rereview is required by this review.
