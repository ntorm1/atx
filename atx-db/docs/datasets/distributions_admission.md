# Public native admission and replay contract

Admission is an independently reviewed external expectation. The caller supplies both
its JSON path and expected SHA256; generating expectations from a product/parse receipt
does not constitute source certification. Byte/replay PASS never means class/clock or
actual task gate PASS. Current source semantics remain unqualified, every indicated
annual rate NULL, and every actual gate FAIL/unmeasured. The plain `fetched_at` metadata
must equal a retained original receipt's fetched_at/done_at; that matching clock is a
documented conservative floor, not original acceptance certification.

Exact admission JSON top-level fields:

```json
{
  "schema": "atx.native-admission/v1",
  "inputs": {
    "catalog": {"path": "native-documents.jsonl", "sha256": "externally_expected"},
    "source_catalog": {"path": "original-doc-catalog.parquet", "sha256": "externally_expected"},
    "calendar": {"path": "exchange-sessions.json", "sha256": "externally_expected"},
    "class": {"path": "class-evidence.json", "sha256": "externally_expected"},
    "clock": {"path": "clock-evidence.json", "sha256": "externally_expected"},
    "fundamentals": {"path": "fundamentals/manifest.json", "sha256": "externally_expected"},
    "identity": {"path": "identity/manifest.json", "sha256": "externally_expected"}
  },
  "code_sha256": {"every_exact_reader_or_parser_path": "externally_expected"},
  "population": {
    "catalog_rows": 2,
    "selection": "all_eligible_source_rows_hash_partition",
    "eligible_source_rows": 23958,
    "partition_index": 0,
    "partition_count": 1024
  }
}
```

Numbers above illustrate fields, not an actual admitted production partition. Paths
are explicit absolute paths in actual admissions. Exact complete code identities are
enumerated by `distributions_native.code_pins()`: distributions_native, distributions,
events_buyback, events_sources, events_common, guidance, common and imported lxml.html,
lxml.etree, pyarrow/lib/_parquet and duckdb/_duckdb reader entrypoints/binaries. Incomplete or stale code lists
reject. Every expected input is checked before computation and again afterward. The
independently admitted fundamental/identity manifests must contain exact files entries
for events.parquet/link_table.parquet with expected SHA256; these descendants are checked
before read and after replay. Hashes are identity evidence only, not economic proof.

`calendar` is a sorted unique JSON array of ISO exchange-session dates, including the
prior session and next session beyond any requested month-end. Independently pinned
class/clock evidence currently remains unqualified and cannot create rates. The source
catalog is original Parquet metadata; eligible rows are exactly forms8-K,8-K/A,10-Q,10-Q/A
and filing_date>=2020-01-01. Full eligible source count must equal the independent expected
count. Keys(accession,doc_sha256,source_doc) must be nonnull/unique. Partition assignment is
integerSHA256(JSON[key],separators=(',',':')) modulo partition_count. The native JSONL
catalog must contain every and only key in that partition. Missing objects or metadata
are not silently dropped. This is a source-population invariant, not the vendor cash
denominator gate. No keyword-hit filter defines a full denominator.

Every native JSONL record requires accession,cik,source_doc,source_path,source_url,
source_sha256(original decoded bytes),stored_sha256(storage bytes),storage_encoding
(`plain`or`gzip`),fetched_at(explicit UTC offset),link_basis(`unknown`if unproved).
Buyback additionally requires form and ISO filing_date. Exact JSON XBRL facts can use
format=xbrl_json with native fact concept/value/unit/start/end; no historical total
is annualized. Native HTML/plain text is normalized only after original-byte validation.
Each record must additionally carry an independently admitted source receipt locator:

```json
{"receipt":{"path":"original-ledger.jsonl","sha256":"external_ledger_hash","line":1,"line_sha256":"external_original_line_hash"}}
```

The original receipt line must match the original-byteSHA and either successful URI/status
or landed accession/exhibit/document_ok identity, and its fetched_at/done_at must exactly
match the conservative floor. Ledger line bytes, including newline, define lineSHA. No
HTTP or source acquisition occurs during replay. Storage GZIP and original body hashes
remain distinct; normalized UTF8SHA never replaces original as-served identity.

Bounds:≤100native docs/partition;≤2MiBstored and≤2MiBdecoded per doc;≤8MiBdecoded plus
normalized total;≤32MiBper receipt ledger, line≤64KiB and line number≤1million;
parent metadata≤8MiB/100,000rows, streamed batches256. Oversized/missing/stale inputs
reject rather than publishing partial success. GZIP decoding reads onlylimit+1bytes.
Native unknown/no-supported-declaration and unsupported PartII reasons are keyed in
product diagnostics. All output event keys are checked nonnull/unique independently.

From C:/atx/atx-db, env PYTHONPATH=C:/atx/atx-db/src,OPENBLAS_NUM_THREADS=1, use the
root-exclusive guard≤0.3GiB. Commands below require resolved, independently reviewed
actual inputs; none has run on production sources in this lane:

```text
.venv/Scripts/python.exe -m atx_db.alpha_panel.distributions --documents <native-jsonl> --input-manifest <admitted-class-file> --admission <external-admission.json> --admission-sha256 <external-hash>
.venv/Scripts/python.exe -m atx_db.alpha_panel.distributions --documents <native-jsonl> --input-manifest <admitted-class-file> --admission <external-admission.json> --admission-sha256 <external-hash> --validate-product <immutable-distributions.parquet>
.venv/Scripts/python.exe -m atx_db.alpha_panel.events_buyback --phase all --admission <external-admission.json> --admission-sha256 <external-hash>
.venv/Scripts/python.exe -m atx_db.alpha_panel.events_buyback --admission <external-admission.json> --admission-sha256 <external-hash> --validate-product <immutable-buyback.parquet>
```

Both public validators are read-only with respect to products and manifests, though
DuckDB uses its guarded spill scratch. They re-read original source objects independently
of a parse receipt, check exact schema/unique keys, and compare the full admitted native
population. Buyback also replays fundamental/identity joins and explicitly clocked future
corroboration. Manufactured parse hashes, abbreviated proofs and tampered published values
reject. Native semantic correctness still needs independent original-event handchecks.
Required actual task gates remain declaration>=90%,cash agreement within1%on>=95%, and
200distinct buybackchecks>=95%. No fixtures, metadata, source hash or Boolean match claim
closes these gates. Class/clock certification, actual full vendor/universe cash denominator,
calendar admission and rate carry policy remain blocking Task4R11 prerequisites.
