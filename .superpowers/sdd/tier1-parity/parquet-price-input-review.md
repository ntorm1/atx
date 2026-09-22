# Native Parquet price-input independent review

Reviewed commit `981f1ec722b7a5d7500efced98352cf1216551d0` against its task report,
the existing AR6 source contract and program rulings. This was one Codex source
review; no tests, actual-source scans or warehouse operations were run.

Disposition: **one Important finding; no Critical findings.** Accept the bounded
fix on the implementer's issue-by-issue report and focused regression evidence;
another review pass is not required by the process ruling.

## Important P1: accepted native integral numeric identifiers become invalid tokens

`atx-db/src/atx_db/ticker_history_quality.py:92` accepts FLOAT, DOUBLE and scaled
DECIMAL columns for securityID and dn, but their projection at lines23 and28
requires the cast-to-VARCHAR representation to match an integer-only token.
Native numeric values such as DOUBLE 101.0 and DECIMAL 1.0 have a fractional text
suffix despite being exactly integral, so the projection sets vendor_id/dn to
NULL. Positive vendor breadth becomes zero for such files; original positive-key
duplicates bypass `ticker_history_source_keys`, and adjacency diagnostics stop
comparing those rows. The unnormalized vendor_security_id text also remains
different from an equivalent integer identifier in another accepted file.

The new tests establish BIGINT identifier equivalence and rejection of fractional
101.5, but they do not exercise integral FLOAT/DOUBLE/scaled DECIMAL identifiers
or dn. This is a contract defect for the types the validator explicitly accepts.
It does not establish a defect in the user's particular staged source, whose
identifier columns the controller reports are int64; this review did not inspect
that file.

Required bounded repair: either deliberately reject unsupported identifier/dn
types during schema binding, or normalize exactly integral, finite, in-range
native numeric values without rounding fractional values into valid keys. Preserve
the existing strict text-token policy for textual sources, canonical identifier
stability and duplicate quarantine. Cover native integral identifiers/dn,
fractional values and duplicate-key diagnostics with focused fixtures. Do not
relax the original row, security or latest-date breadth floors.

## Confirmed scope and limits

- Parquet reads and file paths are parameter bound; schema inspection fetches only
  the column description. Required fields, scalar types, list values and integer
  date encodings are checked before staging. Original rows stay inside DuckDB.
- The same projection, adjusted-close recipe, duplicate-key quarantine, invalid
  price filtering and publication breadth checks serve both input formats.
- Existing positional/keyword TSV callers and CLI aliases are retained, while
  ambiguous dataclass inputs fail explicitly.
- Format-specific provenance accurately limits native floating-point precision,
  historical vintage, identity and economic-adjustment claims. The offline audit
  hashes original bytes and compares file size/mtime after scanning; publication
  retains the existing original-file hash lineage.
- Source latest-date counts are correctly described as prefilter source breadth,
  not canonical US-common-equity coverage. Defaults are 1 GB/one thread. No full
  updated-source audit or production publication is claimed by the task report.

The controller should retain the existing single-heavy-workload sequencing and
process-tree memory guard for the pending audit and publication.
