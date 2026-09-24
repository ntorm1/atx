# Full submissions resume: lower-memory static audit

Read-only source/evidence inspection at HEAD `c41457fc`, 2026-09-24. No Python, tests, database opens, archive reads, network calls or production jobs were run. Existing reviews and checks were not repeated. Root's archive17 remains the sole heavy workload.

## Decision

A 512 MB DuckDB / one-thread run under the existing native 1.5 GiB process guard is a **measured trial**, not a qualified production profile. Existing bulk streaming and periodic connection recycling make that trial more useful than repeating the former unbounded-retention process. However, substantial ZIP metadata and per-batch Python allocations remain outside DuckDB's budget; static inspection cannot establish capacity or a completion-time estimate.

The strongest existing memory evidence is **0.7695121765136719 GiB merely opening/traversing the ZIP directory**, before any database, pandas or filing payload work. Exact retained source: `C:\atx\atx-db\data\cache\submissions.zip`. The receipt is `submissions-archive-inventory-memory.json`; the inventory is `submissions-archive-inventory.json`. Root may instead retain the established 1 GB / 2 GiB source profile if the post-archive sustained 6 GiB physical / 8 GiB commit window holds. Neither profile's submissions completion is established by the historical receipts.

There is one concrete startup-budget gap: activation first opens and initializes its store, then applies the requested analytical settings. A small generic repair should pass the requested settings at initial connect, with an absolute spill path, before initialization. This is distinct from the now-bounded reopen/recovery paths. No such change was made by this audit. Registry, jobs and activation edits must remain serialized with root.

## Existing contracts and memory behavior

- `activation.py:553–572` selects `SecSubmissionsBulkDataset` with `forms=None`, no CIK restriction, history enabled by its default, and batch size 50. `--as-of-date 2026-09-20` pins the activation ledger; this stage ingests the whole retained archive, without truncating its filing history to a recent window.
- `sec_submissions.py:380–415` requires the predecessor and every ancestor to be terminal failed bulk dataset attempts, with identical path, forms, CIK scope, history setting and batch size. The authorized predecessor remains `04cf947d-53bb-49b7-a276-b3c74a2a52c8`. Do not reduce batch size as an ad hoc memory workaround: the recorded batch contract and prefix boundary depend on 50.
- `sec_submissions.py:441–542` hashes the retained archive using the 1 MiB streaming hash helper in `warehouse.py:67`, verifies a matching pre-attempt receipt, checks lineage counts/ordered complete batch boundaries, and streams retained rows using `fetchmany(4096)` (`sec_submissions.py:414–418`). It holds one CIK's deduplicated accession keys, reads that CIK's recent/history members, and compares CIK/security ID/accession/form/source URL before skipping the prefix. It does not load the complete filing table into pandas. The global ordered SQL query can spill under the active DuckDB budget.
- `sec_submissions.py:625` materializes the current CIK/security mapping, a metadata mapping rather than the filing corpus. `:670–674` opens Python's ZIP directory and additionally keeps all member names in a set plus a sorted main-member list. This is whole-archive metadata materialization, not streaming directory traversal.
- `sec_submissions.py:689–734` reads one complete JSON member at a time, normalizes it into pandas, retains all historical frames for the current CIK, and accumulates frames for 50 nonempty CIKs. `:649–659` concatenates/deduplicates the batch, then transactionally deletes/reinserts by security ID and accession. There is no independent row/byte ceiling: a large CIK or set of 50 CIKs can still require substantial Python memory. The per-member row-dictionary intermediate in `_columnar_filings` (`:152–161`) adds temporary copying.
- After each eighth committed nonempty flush, `sec_submissions.py:568–598,663–668` checkpoints/closes/reopens a configured persistent connection, only after loader registrations/cursors are released. Caller temporary relations prevent recycling. The recent generic `DuckDBStore.reopen()` repair (`connection.py:119–150`) applies the recorded 512 MB/thread/spill configuration **at connection creation**. Dataset failure-ledger recovery is also bounded at open (`dataset.py:81–109`, `connection.py:153–192`).
- The initial activation open is different: `activation.py:1127–1132` enters `DuckDBStore`, whose `connection.py:75–81` calls the unconfigured `open_duckdb_connection` (`:47–56`) and initializes before `_configure_analytical_session`. `pending_migrations` also opens read-only without a DuckDB memory cap before activation. The outer native process cap still protects both paths, but claiming a 512 MB startup budget would be incorrect today. On a checkpointed current warehouse these perform primarily catalog work; that does not repair the contract for a future WAL/recovery case.
- The submissions table still has explicit indexes on `(security_id, filing_date)` and `accession_number` (`schema.py:1947–1948`). The source-index migration removed three different raw CompanyFacts/points indexes; it did not establish a lower submissions write peak. No index removal is proposed without a demonstrated submissions bottleneck and query/physical-contract review.

## Existing receipts, not new measurements

`submissions-archive-inventory.json` records 991,042 ZIP members: 985,667 main CIK members, 5,374 history members, one other member; 1,564,656,199 compressed bytes and 5,741,005,528 declared expanded bytes. Its directory-only inspector did no payload or database reads, yet `submissions-archive-inventory-memory.json` recorded **0.7695121765136719 GiB** native peak under a 1 GiB guard. The loader adds pandas, DuckDB, sets/lists and filing data to archive-directory overhead. Do not add these peaks arithmetically as a prediction; they are different processes and phases.

`submissions-bounded-connection-report.md` records the earlier pre-recycling process reaching 2,442,285,056 bytes of private memory under a 1 GB DuckDB budget and a 3 GiB process guard, then dropping. It ultimately stopped on low host headroom, not a demonstrated worker OOM. The retained failed attempt has 8,504,213 rows over 37,700 CIKs in the prior read-only receipt. These figures do not prove the current lifecycle succeeds under 1.5 GiB. Existing lifecycle and verified-resume focused checks are already complete; this audit does not reopen those reviews.

## Bounded repair choices

Fix the initial analytical connection configuration generically before claiming a startup cap, preserving initialization, UTC, spill, migration governance and close-on-error behavior. The same bounded raw read-only connection configuration can cover the pending-migration metadata probe. This is a concrete resource contract gap, not a reason to redesign the loader or narrow its scope.

If the measured full trial fails during ZIP opening/directory retention, a separate bounded task can replace the duplicate directory/name structures or build a retained on-disk member index. Preserve archive hash identity, sorted traversal, duplicate/missing-member behavior and all history references. If it fails during normalization or writing, spill normalized chunks while preserving one transaction and durable boundary for every existing batch of 50 CIKs, including recent-before-history accession precedence. Neither change is justified as a production improvement until its actual failure phase is observed. Do not merely increase the memory cap.

### Isolation of a possible disk-backed directory repair

This can be isolated to submissions. `sec_submissions.py:441` receives an archive reader plus a sorted main-member collection and name lookup; `:670–678` constructs them. Those are the replacement seams. CompanyFacts opens its own `zipfile.ZipFile` in `fundamentals.py:354,971` and does not import `sec_submissions.py`; no shared archive helper or monkeypatch is required.

Proposed exact ownership for a future bounded task: new `atx-db/src/atx_db/_submissions_archive.py`, `atx-db/src/atx_db/sec_submissions.py`, new `atx-db/tests/test_submissions_archive.py`, and focused integration additions to `atx-db/tests/test_sec_submissions_bulk.py` / `test_sec_submissions_resume.py`. A new module may require its single module-name entry in `atx-db/tests/data/public_api_snapshot.json`, using root's serialized integration slot. No `fundamentals.py`, connection, registry, jobs, activation, migration or shared `zipfile` changes are needed for the archive-reader repair itself.

A disk-backed index must be populated by bounded central-directory traversal **without first constructing ordinary `ZipFile`**, because ordinary construction retains all entries; copying its already-loaded directory to SQLite would retain the same opening peak. The reader would look up one member's offset/metadata and decompress only that member, while main-member traversal becomes an ordered index cursor and history-existence checks become index lookups. It must preserve ZIP integrity checks, Unicode names, duplicate-name selection semantics, missing-member errors, CRC/length validation and archive hash/stat identity; unsupported archive formats must fail explicitly. SQLite sidecars could live beside the existing cache under a hash/version key, with atomic completed-index publication and no source-archive mutation. This is feasible without changing source scope, but implementing a correct bounded ZIP reader is a substantive task; no such implementation or new review is authorized by this audit.

## Full-scope trial command and acceptance

Only after archive17 is terminal and inspected, no writer remains, and root has accepted a fresh sustained-headroom window, run from `C:\atx\atx-db` with fresh receipt names:

```powershell
C:\atx\atx-db\.venv\Scripts\python.exe C:\atx\.superpowers\sdd\tier1-parity\run_memory_guarded.py `
  --job-gb 1.5 `
  --receipt C:\atx\.superpowers\sdd\tier1-parity\activation-submissions-resume512-1-memory.json `
  --stdout C:\atx\.superpowers\sdd\tier1-parity\activation-submissions-resume512-1.log `
  --stderr C:\atx\.superpowers\sdd\tier1-parity\activation-submissions-resume512-1.err `
  -- C:\atx\atx-db\.venv\Scripts\python.exe scripts\warehouse_activate.py `
  --db-path data\warehouse.duckdb --as-of-date 2026-09-20 `
  --only submissions_load --submissions-batch-size 50 `
  --submissions-resume-from-run-id 04cf947d-53bb-49b7-a276-b3c74a2a52c8 `
  --memory-limit 512MB --threads 1 --backup-keep 100 --force `
  --run-id activation-submissions-resume512-1 `
  --sec-user-agent "atx-db/0.1 atx-research@example.com"
```

Keep the retained archive path unchanged and do not add `sec_bulk_download`. The stage itself guarantees all forms/history/CIKs; there is no `--all-forms` activation switch to add. Keep the process guard's physical/commit hard stops unchanged. Record native peak, headroom, prefix-verification completion, first successful new commit, recycle progress and terminal outcome. A successful probe or one successful batch is not stage completion.

Terminal acceptance requires both dataset/stage ledgers, `scope_complete=true`, processed-main count equal to the retained archive denominator, zero missing history references, and reconciliation of `verified_prior_rows + rows_loaded` to covered rows. Distinguish newly written attempt rows from net warehouse additions. The prefix proof compares identity/form/source keys, not every timestamp column. Migration 0320 left old raw acceptance strings NULL and a prefix resume does not backfill them; preserve the existing conservative availability treatment and measure raw-clock coverage before any exact-vintage claim. The full submissions stage still does not establish historical US-listed membership or release readiness.

Root accepted this audit as operational evidence and assigned fresh SA1 implementer `bounded_submissions_archive` to the isolated reader scope above, using the measured directory overhead as the concrete motivation. Implementation is underway; focused checks, independent review and runtime acceptance remain pending. The 512 MB / 1.5 GiB command remains conditional and does not define a qualified production profile.

No files beyond this audit were changed by its author, no runtime work was performed, and `stash@{0}` was untouched. Root authorized an explicit-path documentation commit after accepting the audit; no code review or test rerun was required for that documentation.
