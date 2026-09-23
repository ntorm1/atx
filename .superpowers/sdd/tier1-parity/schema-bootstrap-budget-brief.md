# TB1: bound test bootstrap before initialization

Required schema/numeric verification has been interrupted by host headroom
stops. Static inspection found tests/conftest.py opens DuckDBStore via its
context manager and only caps threads after initialize() completes. Copied
test stores cap threads but have no explicit DuckDB memory limit. The process
tree guard remains mandatory and unchanged; this fix does not claim to solve
external memory pressure.

Own only atx-db/tests/conftest.py and a tiny focused test file if necessary
to prove initialization observes the settings. Set one DuckDB thread and a
1GB analytical memory limit BEFORE actual template bootstrap; use the same
settings when opening fixture copies. Use existing real initialize() path,
full migrations/seeds/constraints and exact fingerprint/cache readiness.
Do not bypass migration checks, modify historical checksums or reduce schema.
Ensure close on exceptions and preserve UTC/spill settings. Avoid global
monkeypatching duckdb.connect or changing production connection defaults.
Do not run any DB/test/Python command until parent grants runtime. No live
warehouse/network/migrations/registry/job/activation edits. Preserve unrelated
EOL-only _runner.py, bodies_0280.py, test_migrations.py and governance edits.

Fresh Codex implementer, one independent static review, Important fixed on
report and Critical rereview only. Parent will run the actual required schema,
module and affected numeric files under the existing1.5GiB process-tree guard;
no full suite or extra bootstrap just to test a mirror of implementation.
The actual schema build is the useful validation. A cheap spy test may check
that settings are active at initialize() if it provides independent ordering
coverage without creating the full schema. No test resource cap is proof of
live backfill capacity; source1GB/one thread/2GiBcap and sustained6/8GiB floor
remain unchanged. Report in schema-bootstrap-budget-result.md, no commit
until parent coordinates the clean-source verification.
