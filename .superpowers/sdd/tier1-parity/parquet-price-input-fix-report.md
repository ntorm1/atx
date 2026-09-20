# Parquet input review repair

Review: `parquet-price-input-review.md`, Important P1, no Critical findings.

## P1: explicit identifier column contract

**Fixed by rejecting unsupported identifier column types before staging.**
`securityID` and `dn` now accept native integer columns or VARCHAR containing
the existing strict integer tokens. FLOAT, DOUBLE and DECIMAL columns are
rejected even when every value happens to be integral. This matches the supplied
archive's reported int64 identifier columns and avoids silently turning an
accepted integral value such as DOUBLE 101.0 into an invalid identifier.

Validation happens before creating the original-row staging table, so an
unsupported schema cannot silently reduce positive-vendor breadth, bypass
duplicate quarantine or disable original-dn adjacency comparisons. Price,
quantity and return fields retain their existing scalar numeric acceptance.
The source provenance explicitly records this narrower identifier type policy.
No publication floor or residual diagnostic threshold changed.

Text remains strict: fractional-looking tokens such as `101.0` and `101.5` do
not round to 101, and values outside signed BIGINT range do not become keys.
Native int64 values preserve their canonical integer string, positive-key
quarantine and original-dn adjacency. No source file has been scanned or changed,
and no live warehouse was opened.

## Focused evidence

All 16 focused identifier cases passed once with `-n 0 -q`: 12 reject native
FLOAT/DOUBLE/DECIMAL securityID/dn columns for integral and fractional values,
three preserve strict text-token/range handling, and one verifies actual int64
keys plus duplicate quarantine and adjacency diagnostics. Fixtures used
128 MB/one-thread DuckDB connections under the reviewed 2 GiB process-tree cap.
Receipt `parquet-price-p1-focused-memory.json` recorded approximately 0.60 GiB
native peak job accounting and ample physical/commit headroom. The DB slot was
released after this one run; no full schema or production source was loaded.
Ruff passed for the two modified Python files, mypy passed for the production
helper, and `git diff --check` passed.

The original implementation report remains historical evidence of its initial
32 covered cases. Its broad numeric-column acceptance is superseded specifically
for securityID/dn by this explicit contract. Per the program ruling, this
Important repair is accepted on this report without a second review pass.
