# Public native admission and replay contract

Admission is an independently reviewed external expectation. The caller supplies both
its JSON path and expected SHA256; generating expectations from a product/parse receipt
does not constitute source certification. Byte/replay PASS never means class/clock or
actual task gate PASS. Actual source semantics remain unqualified and every actual gate FAIL/unmeasured.
Conditional original-body chains qualify individual event rates; missing chains remain NULL. The plain `fetched_at` metadata
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

## Conditional semantic reader continuation

`distributions_qualification.py` supplies concrete offline semantic primitives:
original SEC index Accepted/full-submission acceptance header with accession and
DST-aware NewYork-to-UTC conversion (ambiguous/nonexistent wall clocks reject);
original inline-DEI same-context title/symbol/issuer/context-identifier matching
(legal-entity/co-registrant contexts and ADR/preferred classes reject); exact native
declaration replay; unique dated symbol/CIK/security linkage with no future evidence;
and conditional fixed regular USD event rate using the explicit native cadence.

A positive fixture produces2.4USD/share/year from0.60regular quarterly on the proved
native class. Quote currency remains separately unqualified. Carry requires explicit
`effective through` validity parsed from that native declaration; missing validity
never invents forever carry or a120/365day expiry. No-pay/cancellation/variable policies
are native parsed tombstones. Future declarations are filtered by availability and
cannot rewrite earlier state.

Production and public full replay now use distributions_ancillary.py. Admission v2 adds
exactly prices:{path,sha256}, supplying session_date/security_id/ticker for the existing
seven-calendar-day as-of symbol rule. Independently pinned identity link_table.parquet
supplies cik/valid_from/valid_to/available_at/link_tier. Unique available strict/dated matches
qualify; quote currency remains separately unqualified. v1 preserves missing-chain NULL.

Exact class input: {schema:"atx.native-class-evidence/v1",records:[...]}; records contain
exactly declaration_accession,declaration_sha256,class_document,class_clock_document.
Clock input has schema atx.native-clock-evidence/v1 and records containing exactly
declaration_accession,declaration_sha256,clock_document. Nested documents use the original
object/receipt contract above. Duplicate keys, unexpected fields, accession conflicts,
missing claimed objects and tampering reject. Missing records or legacy empty arrays retain
NULL/reasons. Native Accepted/SGML and inline-DEI bodies determine semantics; Boolean claims,
transformed notes and filesystem dates do not. Ancillary originals/receipts and both new
reader modules are pinned before/after; public replay independently compares every column.

Complete global carry/cancellation/variable-policy inventory, native prior history, quote
currency and vendor cash denominator remain open. Source-parsed explicit validity is exposed;
conditional event qualification never certifies persistent state or any actual gate.


## Qualification contract refreeze: repair round3

Rule native-distributions-v4 retains physical schema v3. Fixed status and cadence must
attach to the dividend phrase; contingent/conditional/unsupported clauses cannot qualify.
A meeting's annual cadence is unrelated. Native declaration date must be known and cannot
follow either reconstructed acceptance or final availability.

Native clock proof now requires one authoritative original SEC accession and issuer plus
exact original document membership. Supported index layout uses secNum accession,
companyInfo CIK, a single Accepted value, and tableFile document URI. Member receipt URI
must be https://www.sec.gov/Archives/edgar/data/<CIK>/<undashed-accession>/<filename>.
The clock object's URI must be that accession's -index.html or original full-submission
.txt. Supported SGML requires unique header accession/CIK/acceptance and a unique matching
DOCUMENT FILENAME/TEXT body matching admitted member bytes. Related prose cannot supply
identity. Unsupported, conflicting, repeated or swapped identities remain NULL/reasons.

NewYork normalization reads explicit tzdata/zoneinfo/America/New_York TZif bytes through
compiled ZoneInfo.from_file. Independently expected complete code pins now include that
actual TZif, compiled _zoneinfo, zoneinfo and its _common loader, datetime/_strptime and
Python executable. Loaded clock/path/class/loader entrypoints are checked too. Every
identity is rechecked before/after producer and independent public replay. Ambient
TZPATH/cache cannot change this normalization. Changed admitted TZif or normalizer rejects;
missing proof never falls back to ambient timezone assumptions. All old admissions must
be independently refreshed for the new complete code identities.

Actual source clocks/class proofs, global prior-history/cancellation inventory, quote
currency, full vendor member cash denominator and numerical gates remain unqualified.
