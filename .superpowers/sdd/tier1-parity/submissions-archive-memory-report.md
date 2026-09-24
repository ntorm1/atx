# SA1 — implementation report

Root repair acceptance: `archive-disk-focused3` passed six affected archive
integrity cases (fresh/reused same-stat mutation and all four corrupt-cache
rebuild cases), alongside 14 DG1 checks, in 2.29seconds. Native peak was
0.584323883GiB under 1GiB. Earlier 19 passing archive checks stand. One clean
independent review plus the implementer's Important repair report is accepted;
no second review. Final scoped Ruff passed for all five source/test files.
Loader integration and the retained full-directory probe remain pending.

Loader integration is now accepted: integration1 passed 45 checks and exposed
two Important test integration mismatches. The shared fixture now records
bounded session settings, so the unconfigured case explicitly clears only that
recorded configuration; actual DuckDB limits stay bounded. The module snapshot
adds exactly the new imported private module. Those two repaired checks pass
in integration2 (2.89seconds, peak 0.654464722GiB/cap 1GiB). The 45 unchanged
passes stand (34.49seconds, peak 0.677158356GiB/cap 1.5GiB). Retained-directory
probe and actual full submissions ingestion remain separate pending outcomes.

Retained-directory acceptance is now complete: `submissions-index-probe1`
passed at 0.591762543GiB native peak under 1GiB. Source SHA256 is
702fbcd8b4335bc649e9e4eab3a202f3effc314b43421664bfecb59365767165. The reader
indexed all 991,042 entries, streamed all 985,667 main names, and retained all
5,374 history entries plus one other entry. Counts and recorded byte totals
match the earlier inventory; that earlier artifact lacks SHA, so these matches
alone cannot prove historical byte identity. Source stat remained unchanged.

The 202,481,664byte directory index was built in 51.141seconds; build and reuse
agree on source hash, all denominators, ordered-name digest and all four bounded
selected payloads (first/last issuer, CVX and one history member). Every selected
read passed stdlib ZIP integrity checks, and reuse retained the same sidecar.
The source was hashed independently on both opens. This verifies the actual
directory memory improvement, not all payloads, normalization memory, dataset
completion or historical-vintage eligibility. Full batch50 submissions remains
pending the completed CompanyFacts source prerequisite.

Status: the single independent static review is clean. Root's focused Important
repair recheck passed all six cases. Root's integration check then produced 45
passes and two fixture-contract failures; the exact two repairs below await its
focused recheck. No Python, tests, warehouse or archive opens, source requests
or heavy workloads were run by this implementer. Root owns the runtime slot.
Scoped native Ruff and `git diff --check` pass after the repairs.

`SubmissionsArchive` hashes the retained file with bounded reads, streams one
central record at a time, and stores the directory in SQLite with an 8 MiB cache,
disabled mmap and disk temporary storage. The complete directory never enters
ordinary `ZipFile`; a synthetic one-entry directory delegates interpretation to
the installed Python stdlib. Standard `ZipFile.read(ZipInfo)` then checks the
actual local header, compression stream and CRC. The wrapper additionally checks
expanded length and source stat identity and compares the selected cached
central record with actual source bytes before every read. ZIP64, Unicode extras,
UTF-8 and CP437 interpretation remain with stdlib. Unsupported multi-disk layouts
and inconsistent/truncated directory framing fail closed.

SQLite stores one last-wins member row per normalized stdlib filename, plus all
physical entry offsets (including overwritten entries) for the same overlap
boundaries as the standard reader. An indexed cursor traverses every sorted main
member; SQL membership checks replace the million-name set. Prefix/remainder
cursors preserve empty issuers, recent-before-history accession precedence,
all-forms/all-history scope and the existing batch boundary. Resume uses the
reader's freshly computed source SHA in its unchanged pre-attempt receipt proof.

The completed index is committed and atomically replaced, followed by its own
atomically replaced full-file SHA receipt. Reuse hashes the entire index and
checks source SHA/stat and parser version. Missing/renamed index rows therefore
cause rebuild even if SQLite still opens and cached counts look plausible. A
crash between publications cannot authorize an unverified replacement. Partial
build files from another interrupted process are ignored, never trusted or
removed. Normal exceptions clean up only the current build's temporary files.

Trust limit: these checks address interrupted writes, accidental corruption and
ordinary stale local caches. They do not authenticate a malicious coordinated
replacement of both the cache and its digest receipt. The source SHA is computed
independently on every open; selected central/local identities and CRCs are still
validated against the source. This is not a historical-vintage provenance claim.

Focused tests cover member equality with stdlib across four compression formats,
duplicate last-wins, UTF-8/CP437, concatenated archives, real forced ZIP64 records,
CRC failure, same-offset overlap, invalid directory lengths/counts/multi-disk,
interrupted publication/restart, same-stat source replacement, mutated source
central records and corrupt cache rows. Bulk/resume additions assert duplicate
main economic content and full-scope recovery after both prefix and remaining
issuer names are deleted from the index. Existing resume integrity tests retain
their assertions.

Commands for root's guarded slot (from `atx-db`, no parallel workers):

```powershell
.venv\Scripts\python.exe -m pytest tests/test_submissions_archive.py -n 0 -o addopts= -q
.venv\Scripts\python.exe -m pytest tests/test_sec_submissions_bulk.py tests/test_sec_submissions_resume.py -n 0 -o addopts= -q
.venv\Scripts\python.exe -m ruff check src/atx_db/_submissions_archive.py src/atx_db/sec_submissions.py tests/test_submissions_archive.py tests/test_sec_submissions_bulk.py tests/test_sec_submissions_resume.py
```

Root must record retained-archive denominator/digest/native-peak evidence and the
actual full-scope dataset outcome before qualifying the lower-memory profile.
Per-member JSON/pandas and each batch50 filing buffer remain unchanged and may
still dominate memory. No production-readiness or memory-saving result is claimed
from static inspection or fixtures. No commit yet; stash@{0} is untouched.

## Focused check and Important repairs

Root's `submissions-archive-focused1` completed with 19 passed / 3 failed in
4.92 seconds, native peak 0.673927 GiB under a 1 GiB process cap. This is fixture
evidence only. The two cache-corruption failures were Windows replacement locks
held by the test's own SQLite handle: SQLite's connection context manager ends
the transaction but does not close the connection. The corruption fixtures now
commit and close explicitly, including the analogous resume fixture. Product
index replacement and full-file cache-digest checks are unchanged.

The third failure exposed an Important per-read integrity gap, not a wrong
fixture offset. `CIK0000000003.json` is the first central entry; byte 16 is its
CRC field. The source `BufferedReader` retained old directory bytes, and an
in-buffer seek could return them after another handle changed the central CRC
and restored mtime. Fresh-open source hashing, ordinary stat changes and payload
CRC checks were still active, but the promised current central-byte comparison
was incomplete for this same-stat edit.

The persistent source handle is now unbuffered, so selected central records and
payload reads cannot reuse Python read-ahead. Initial directory indexing keeps a
separate short-lived buffered handle, verifies its device/inode/size/mtime against
the hashed source handle, and rechecks source identity after traversal. This
preserves bounded sequential index construction without sacrificing freshness on
subsequent reads. The unchanged integrity assertion now covers both newly built
and reused indexes, with an initial selected read to prime the former failure.
Neither classification requires a second review under the user's Important-fix
policy; root must accept the report plus focused repair evidence. No runtime
claim is made until those rechecks finish.

## Integration check and fixture-contract repairs

Root's `submissions-archive-integration1` completed with 45 passed / 2 failed in
34.49 seconds, native peak 0.677158 GiB under a 1.5 GiB process cap. The 45 passing
checks remain accepted. Both failures are Important integration repairs and
change no production loader behavior.

The `unconfigured` caller-session test inherited recorded analytical settings
from the now-bounded shared fixture. It therefore exercised a configured caller
instead of the intended unconfigured contract. The test now explicitly clears
only the two recorded opt-in attributes with `monkeypatch`, retaining the real
DuckDB memory/thread limits and all existing connection/row/settings assertions.
Production recycling remains unchanged.

`public_api_snapshot()` includes all names except double-underscore names and
explicit exclusions, so an imported single-underscore helper module enters the
pinned `atx_db` surface. The exact one-entry snapshot addition records
`_submissions_archive` in sorted order; no other symbols, excludes, exports or
module-boundary policy changed. No whole-snapshot regeneration was performed.

Root's exact two recheck selectors:

```text
tests/test_sec_submissions_bulk.py::test_bulk_preserves_caller_session_state[unconfigured]
tests/test_module_boundaries.py::test_public_api_snapshot_matches_pinned_fixture
```

The operator measurement script has root review and scoped native Ruff acceptance.
Retained-archive measurement and full-scope production submissions are still
pending; passing integration fixtures do not qualify either outcome.
