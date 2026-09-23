# TB1 schema bootstrap budget result

Implemented in `atx-db/tests/conftest.py` on `feat/tier1-parity`.

- Template bootstrap now opens DuckDB with `memory_limit=1GB` and `threads=1` in the connection config before calling the real `DuckDBStore.initialize()` path. It still runs the full schema, migration, checksum, and seed work.
- Fixture copies use the same connection config. The store records the analytical limits so `reopen()` retains them.
- Existing `_configure_session()` still applies UTC and the absolute spill directory. Open failures close the connection; bootstrap closes it on success or exception.
- The template fingerprint, file lock, ready marker, and copy behavior are unchanged.

Implementer performed static inspection only. Independent static review is clean. Parent scoped Ruff passed under the exact1.5GiB process-tree guard (tb1-ruff1-memory.json, peak0.057GiB). Parent commits this reviewed fixture before the isolated HEAD export so the real required schema/numeric validation targets committed content. That runtime proof remains pending; no live database, source capacity or production claim follows from this fixture change.
