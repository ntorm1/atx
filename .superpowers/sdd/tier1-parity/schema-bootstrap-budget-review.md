# TB1 schema bootstrap budget: static review

**Finding: clean.** No Critical, Important, or Minor issue found in the working change to `atx-db/tests/conftest.py`.

The template connection receives `memory_limit=1GB` and `threads=1` in `duckdb.connect(config=...)` before `_configure_session()` and the real `DuckDBStore.initialize()` call. `initialize()` still performs its current-schema check, full schema creation, migrations, checksum verification, and seeds through the existing production path. Fixture copies use the same configured opener. Recording both analytical settings lets the existing `reopen()` path restore them. `_configure_session()` still applies UTC and the absolute spill directory, and the opener and template builder close their connections on exceptions.

The fingerprint inputs, lock, ready marker, and copy flow were not changed. Since this change only limits connection resources and does not change schema content, retaining the existing cache key is appropriate. Existing tests elsewhere in this repository also pass integer thread values to DuckDB connection configuration, so this usage is consistent with local practice.

This was a static review only. No Python, test, DuckDB, network, or live workload command was run. The parent's guarded real schema and numeric checks remain the runtime validation for this change.
