# CF5 implementation handoff

Implemented the bounded companyfacts archive repair in the shared tree. No tests,
Python imports, database connections, runtime probes, production commands or
commits were run by this implementer. Root owns those operations. Static
`git diff --check` passed for the modified existing Python files.

## Exact paths

- `C:/atx/atx-db/src/atx_db/fundamentals.py`
- `C:/atx/atx-db/src/atx_db/_companyfacts_resume.py` (new private helper)
- `C:/atx/atx-db/src/atx_db/activation.py`
- `C:/atx/atx-db/tests/test_companyfacts_archive_repair.py` (new)
- `C:/atx/atx-db/tests/test_companyfacts_activation_resume.py` (new)
- `C:/atx/.superpowers/sdd/tier1-parity/companyfacts-archive3-integration.patch`
- This report.

Activation was initially left untouched. After root reported P1 commit
`5519d1ac` and Core commit `0c52fdbd` and explicitly released ownership, the
prepared integration patch was applied to actual `activation.py`. The retained
patch is an audit artifact, not a pending second application. No registry, jobs,
migration, warehouse helper or required primary-key changes were made.

## Connection and transaction bounds

Configured persistent stores recycle after at most ten committed issuer
operations, plus a final partial interval. The resume proof also finishes with
a reopen before the first issuer mutation. Ten is deliberately conservative
against the measured 3,750-loaded-issuer COMMIT failure: it sharply limits index
and connection state retention without changing the 1 GB analytical allocation
or thread count. Recovered candidate transactions count toward the interval.

The existing store close/checkpoint/reopen path restores its recorded memory,
thread, insertion-order, UTC and spill settings without reinitialization or
migration. Unconfigured and in-memory stores are not destroyed. Caller-owned
temporary tables or registered views cause an explicit failure instead of
silently defeating the lifetime bound or dropping those objects. Loader temp
tables and frames are dropped/unregistered before an issuer transaction returns;
the proof's fetch loops are exhausted before the proof-phase reopen.

Issuer facts, points, unresolved candidate replacement and successful source
receipt now share the existing issuer transaction. A candidate/receipt error
rolls back the whole issuer replacement. Candidate output no longer waits in
an archive-wide DataFrame list until the end. Source/member errors remain
skippable according to existing archive behavior; database errors remain fatal.

## Verified resume

`SecCompanyFactsOptions.resume_from_run_id` and activation
`--companyfacts-resume-from-run-id` accept a terminal failed companyfacts dataset
UUID, including explicitly linked compatible failed ancestors (maximum 32).
Every ancestor must have the same local archive path, full archive selector,
replacement scope, allowlist, as-of date and universe option. Transport options
such as User-Agent are deliberately excluded from semantic identity, allowing
the contact correction. Loaded/empty receipts independently verify the active
taxonomy/allowlist fingerprint.

Before any issuer mutation, resume checks:

1. The archive SHA-256, byte count, exact canonical target/member URL and symbol,
   bulk source mode, receipt owner UUID, and receipt time inside that failed
   attempt. Duplicate archive member identities are rejected. File identity,
   size and modification time are checked around hashing, proof and ingestion.
2. Each loaded receipt's positive row count against all retained spellings and
   owners for that CIK, requiring exactly its canonical, source-URL-matched rows
   to belong to the receipt's dataset UUID.
3. Fact/point multiplicity and every shared row value (including security ID,
   accession, dates, original filing availability, fiscal fields and value).
   Both surfaces are scanned once for SHA-256 row digests, aggregated as count
   plus four 64-bit digest limbs summed into HUGEINT per run/security identity.
   Archive point symbols must remain NULL. The fact and point aggregates must
   match for every security group needed by a skipped issuer, including groups
   shared by multiple issuers.

SQL aggregate state and Python dictionaries scale with issuer/security
identities, not fact history. Fetch buffers contain at most 256 aggregate or
receipt rows; lineage SQL has at most 32 run identifiers. No full-history
fact/point join, sort, identifier list or DataFrame is created. Separate narrow
scans collect issuer counts and unresolved-issuer summaries. Progress messages
identify the inventory, fact fingerprint, point fingerprint and recovery phases.

Only verified loaded members are skipped. Empty members replay their zero-row
cleanup; unavailable placeholders are reread and preserve prior data; source
errors are retried. Missing, overwritten or unreadable receipts provide no skip
authority, so those issuers replay. This handles the old facts-committed but
receipt-missing boundary without a numeric offset. Contradictory extant proof
fails before any issuer mutation. Original receipts and fact/point run owners
are retained for skipped issuers, permitting another failed resume to reference
the explicit ancestor chain.

Completed/loaded target counters include verified skipped issuers; `rows_loaded`
and raw fact/point insertion counts include only this attempt's new replacements.
Additional details report verified rows, prior completed members, resumed loaded
members, candidate recovery and connection reopens. Empty, unavailable and source
failure counters remain distinct.

## Candidate and missing-CIK repair

Skipped issuers recover unresolved summaries from their retained facts, using
their isolated companyfacts source IDs and NULL entity IDs. Recovery never calls
current ticker resolution or changes retained fact/point identities or dates.
Each recovered candidate is persisted per issuer. Candidate details now expose
the original representative fact filing availability; candidate as-of metadata
continues using the explicitly selected as-of date. This recovers the unfinished
archive3 candidate surface even when no original candidate receipt exists.

For an absent payload `cik` key on an exact validated archive member, the loader
uses that member's CIK and records `validated_archive_member_missing_payload_cik`
and the member name in the source receipt. Explicit NULL, invalid, boolean,
floating-point or conflicting payload identities remain source errors. Real
cef-shaped fixtures model the inspected tiny closed-end fund payloads. A
supported-taxonomy fixture verifies that missing payload CIK does not allow
current ticker identity fallback or alter filing availability.

Activation explicitly forwards `sec_user_agent` (an explicit empty value when
none was configured) into companyfacts options, preventing fallback to another
module-configured contact. All new operational fixtures use only
`atx-db/0.2 atx-research@example.com`. Resume plan validation rejects a download
stage or a limited/append-missing/non-archive scope before opening/migrating the
warehouse.

## Root-only pending validation

The two new files contain 42 parameterized cases by source enumeration, not
runtime collection. They cover interrupted prefixes; repeated failed lineage;
deleted/overwritten/malformed receipts; missing or changed fact/point evidence;
source/allowlist/date/scope mismatches; candidate and receipt rollback; original
candidate availability; old missing-CIK source-error retry; real cef shapes;
explicit invalid CIKs; bounded settings-preserving reopens; a reopen between
proof and candidate recovery; and activation CLI, plan validation and dummy-UA
plumbing/persistence.

Suggested focused root command from `C:/atx/atx-db`, under root's existing sole
workload slot/process-tree memory guard and explicit dummy UA:

```powershell
$env:ATX_SEC_USER_AGENT = 'atx-db/0.2 atx-research@example.com'
& 'C:/atx/atx-db/.venv/Scripts/python.exe' -m pytest -n 0 tests/test_companyfacts_archive_repair.py tests/test_companyfacts_activation_resume.py tests/test_companyfacts_zip.py tests/test_companyfacts_empty_members.py tests/test_companyfacts_cik_spellings.py tests/test_companyfacts_issuer_cleanup.py tests/test_companyfacts_resilience.py tests/test_fundamentals_spine_link.py tests/test_activation_stages_b.py
```

One independent Codex review remains pending. No production restart has been
performed or requested by this implementer. The archive and backup artifacts
have been preserved.

Root22:29UTC update: the above child command is running inside the2.5GiB
process-tree guard, both guard and child using the explicit project .venv Python.
Tools session48505/worker18920; first19casespassed, invocationstillactive.
One independent static review is running concurrently; CF5sourceisfrozen.
This is pending verification, not a pass or authorization to restart production.

## Actual limitations

- Runtime correctness and performance have not been measured by this implementer.
  Root must run the focused checks and assess the existing-budget proof/load path.
- SHA-256 multiset evidence is a bounded cryptographic fingerprint, not a
  mathematical row-by-row equality proof. It assumes trusted source receipts
  and SHA-256 collision resistance; coordinated mutation of both retained
  surfaces is not authenticated against a reparsed source member.
- One exceptionally large issuer must still fit its existing atomic replacement
  transaction. This repair bounds accumulated connection/index state across
  issuers; it does not increase the memory budget or split an issuer transaction.
- Missing receipts replay work, and empty members replay cleanup. An overwritten
  receipt cannot recover completion authority from a maximum CIK or row count.
- A contradictory retained security group shared with an unreceipted issuer
  conservatively rejects the skip proof. It is not silently accepted as complete.
- Archive mutation checks use file identity/size/mtime around the streamed hash
  and read phases; this assumes the archive is not deliberately mutated while
  preserving all those attributes. Root's serial workload ownership remains
  necessary.
