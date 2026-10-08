# Native retained-original and position diagnostics

Stage/schema: `atx.thirteenf-native-originals/v2`. This is the first native reader
piece; source/economic admission, dated security assembly and Task11 acceptance
remain open. Every emitted position is observed raw evidence with NULL USD value,
NULL public clock and false source qualification. No official documentary rule
IDs have yet been independently established. Retained rule-document identities
and proposed IDs are lineage, never instructions or approval flags.

Build with `python -m atx_db.alpha_panel.thirteenf_native --plan plan.json`.
Validate a committed generation with `--validate ABSOLUTE_GENERATION`.
Actual jobs require controller scheduling and the <=1GiB guard; neither command
is currently an approved real-data build.

The plan schema is `atx.thirteenf-native-plan/v1`, with these exact reader inputs:

- `roots`: explicit absolute source directories; paths outside them, relative
  paths and reparse traversal reject.
- `products`: exactly one each of `expected_inventory`, `retained_receipts` and
  `rule_bundle`, each containing absolute `path`, `bytes` and `sha256`.
- `originals`: explicit reader records with accession, absolute path, bytes,
  SHA256, `receipt_line_sha256` and `reader_type`. Supported readers are
  `sec_submission_text` and `gzip_sec_submission_text`. ZIP/member and undeclared
  reader fields remain keyed unsupported diagnostics.
- `generation`: a new absolute directory, never reused or overwritten.
- `max_originals`: integer 1..10000. Selection preserves the entire expected
  accession denominator; unprocessed expected rows have explicit reasons and
  the publication has partial status.

Inventory JSONL records preserve SEC inventory URL/digest, accession/form/filer
and report metadata. Duplicate accessions fail rather than silently select a
source vintage. Receipts are accession-keyed JSONL; the reader binds the exact
raw line digest, original path/object digest, URL and retrieval clock. The rule
bundle schema is `atx.thirteenf-documentary-proof/v1` with at most32 explicitly
pinned documents, their official URL, evidenced publication clock and proposed
rule identity. Documentary identities alone do not establish rule semantics or
historical availability. Missing inventory provenance stays a named exception.

Input plan and each original/document are bounded at8MiB; metadata products at
64MiB, each JSONL record at256KiB and each metadata stream at100000 records. Gzip
decompression has the same8MiB byte bound. Input/code identities are bound before
computation and rechecked afterward. DuckDB uses128MB, one thread and a private
generation database/spill directory. Partial/interrupted attempts remain forensic
evidence and cannot be resumed as admitted units.

`admissions.parquet` is keyed by accession, retaining original/object digests,
source URL/retrieval clock, actual filer header, report date, raw acceptance
header, observed complete-container/cover flags, exceptions and full evidence.
`positions.parquet` is keyed by accession/position_index: CUSIP, observed SH or
PRN amount, raw value, put/call, discretion and other-manager fields. Unsupported
or missing originals do not become zero holdings. `rejects.parquet` includes
every admission exception and unexpected receipt/original key.

Outputs install by atomic no-clobber hard links. The final `manifest.json` is the
commit marker; collision/interruption preserves partials and never overwrites or
rolls back concurrent evidence. Version2 persists the full reproducible plan,
its absolute current file identity, exact reader module inventory, documentary
pins and consumed-original pins. Missing, abbreviated or stale inventories reject.
Validation authenticates these identities before and after a bounded replay in
an isolated temporary128MB/one-thread DuckDB; it never publishes replay products
or modifies the original generation, attempt database, spill or source files.
The replay independently reads authenticated expected/receipt/document/original
inputs and derives expected keys, selected/bounded/missing states, observations,
cover flags, full position values, exception/reject lineage and processing status.
All published rows are compared as multisets and every column/order/type must
match the replay schema; writer-provided hashes/counts alone are insufficient.
Processing `complete` means the entire declared expected inventory was visited;
it does not assert complete retained originals or qualified population. Bounded
or empty inventories remain `partial`, and source completeness stays UNMEASURED.
Validation also rejects unsupported qualification, computed USD or public clock.
Structural PASS does not qualify economics, bronze completeness or coverage.

Next native pieces must independently establish clock/unit/shared-manager/
confidentiality/omission/amendment evidence, authenticated archive member reads,
dated mapping/shares/splits, R16 event cohorts and feature assembly. Final readers
must bind independently admitted R15 `universe_release_v1` and R17 outer
`fundamentals_release_v1` generation plus exact `fundamentals_v11` leaf products.
This first reader consumes neither legacy defaults nor those still-unpublished
release outputs. Retrospective inventory never defines historical populations.
