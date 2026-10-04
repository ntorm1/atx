# 13F source audit and admission

Status: implementation checkpoint; no source completeness certification or feature publication.

`python -m atx_db.alpha_panel.thirteenf_source audit-roots --data-root <root>`
requires unique `--output`, optionally unique `--file-inventory`. The latter is
streaming JSONL containing path, bytes and mtime_ns for every regular file.
Default bounds: 200,000 files, depth 16; reparse points are skipped. Bounds,
missing roots and permissions are explicit exceptions. Filesystem metadata and
receipts do not prove retained original bronze. Historical configurable source
CLI overrides remain unresolved until supported by operator evidence.

First root-run audit: 173,956 files, 65,768,364,696 bytes across `C:/atx/data`
and `C:/atx/atx-db/data`, zero traversal exceptions. This counts all datasets.
Named `raw/sec_13f` contains receipts, index_sample.jsonl and two HTML.gz files;
there were no named `sec-13f` candidates. Opaque ZIPs/index caches require the
all-file metadata inventory before content qualification. No missing-source
counts can be inferred from these named-directory observations.

`thirteenf_quality.master_inventory` reads retained SEC master.idx membership,
including HR/HR amendments and notices. Its accession key strips hyphens;
filing quarter derives from filing date. Report quarter and acceptance remain
missing until independently resolved from submission/cover originals.
Inventory URL and SHA are carried with every accession.

`reconcile` reports keyed FAIL exceptions for absent original provenance,
missing parses/clocks/report quarter, unresolved units, cover-total mismatch,
shared-manager/amendment chains and unsupported zero holdings. Confidentiality
does not exempt the publicly available cover filing. Acceptance clocks need an
explicit UTC offset. These adapters do not yet independently verify claimed
originals or produce quarter-completeness certificates.

`python -m atx_db.alpha_panel.thirteenf_quality build --expected expected.jsonl
--retained retained.jsonl --parsed parsed.jsonl` creates accession gates and
unexpected parsed accession diagnostics with a bounded 128MB, single-thread,
file-backed DuckDB store. Adapter inputs require accession keys; duplicates
fail closed. Input hashes and identities are pinned and rechecked before output.
The manifest remains UNQUALIFIED even when all normalized claims pass.
`available_at` for diagnostic rows is audit creation time, not filing time.
Keys: accession. Gate exceptions are VARCHAR lists; gate_status is VARCHAR.
File `unexpected_parsed.parquet` retains every parsed accession outside inventory.

Ownership kernels require already-qualified long SH positions on dated security
links. Units are shares; denominator is appropriately dated shares outstanding.
Known zero requires a complete filing cohort. Full cohort changes require equal
manager membership, complete prior/current quarters and an evidenced split ratio.
Incomplete-cohort observations remain diagnostics. Unknown manager types remain
unknown, ownership above 100% is flagged, and value changes are never flows.
Native vintage producers, public inventories, amendments, links, denominator
clocks, active/passive evidence and all-three-universe coverage remain unfinished.

`inspect-submissions-cache` authenticates a retained directory against its digest
receipt and archive stat identity using immutable read-only SQLite with8MBcache.
It explicitly does not authenticate ZIP contents. Controller metadata result:
985667mainJSON and5374historicalJSON entries, archive expectedSHA
702fbcd8b4335bc649e9e4eab3a202f3effc314b43421664bfecb59365767165.

`thirteenf_quality inventory-submissions` reads a bounded immutable source
partition. The retained archive is independently hashed; source/cache/member
identities and CRCs use the existing bounded SubmissionsArchive reader. A local
subclass forbids cache rebuilding. Defaults1000mainmembers,hardcap10000 and4MiB
per decompressed member. Main and referenced history documents are streamed,
missing/malformed history references retained as explicit exceptions. Required
submission array lengths must agree. Missing report/acceptance dates stay missing.
Partitions remain UNQUALIFIED until the whole CIK axis and all history references
are reconciled. Original archive retrieval clock remains unknown, never inferred
from filesystem mtime. No acquisition or network is involved.

`inspect-ledgers --file PATH` (one or two explicit ledgers) is streaming metadata-only discovery: <=100000 records/file, <=64MiB read/file, <=256KiB record, <=2000 emitted candidates. Missing/unrecognized forms remain UNKNOWN; malformed and bound stops remain explicit incomplete scans. Relative/outside candidate paths are retained as UNVALIDATED_CONFIGURED_ROOTS and no referenced object is read, hashed, or accessed over network. Ledger source identity is checked before/after; no source content digest is claimed. Candidate limits do not imply absence or original retention.
