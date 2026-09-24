# UM1 independent review — 2026-09-24

Root's one static review is clean. The two phases still create the real0314
schema, seed and preserve two legacy rows, and apply every actual pending
migration. All prior constraint, lineage/vintage, column and schema-contract
assertions remain. Explicit checkpoint and close release bootstrap resources;
the second connection receives256MB/one thread before opening. Store settings
match that budget for any lifecycle reuse.

User explicitly authorizes lower launch requirements when efficiency and
incrementality improve. This supports a bounded experiment for this changed
test only: sustained4GiB physical/6GiB commit for120seconds, lower1GiB process
cap and unchanged emergency1.5GiB physical/3GiB commit stops. It does not prove
the production source loader fits these settings. Runtime acceptance passed:
upgrade-lowmemory-test1 records1pass154.60seconds,0.858974457GiB peak. Scoped
Ruff passed; UM1 is committed4f9bfb90. No repeat review was needed.
