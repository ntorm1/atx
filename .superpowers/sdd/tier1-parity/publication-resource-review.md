# PR1 publication resource static review

Reviewed once by a fresh Codex reviewer on 2026-09-20 against the stable,
uncommitted source in `C:/atx` on `feat/tier1-parity`.

## Verdict and findings

No findings: Critical 0, Important 0, Minor 0. The scoped change satisfies the
brief by static inspection. Runtime validation remains pending with root.

## Scope and evidence

- Read `publication-resource-brief.md`, `publication-resource-report.md`, the
  actual diff in `atx-db/src/atx_db/cli.py`, and the complete new
  `atx-db/tests/test_publication_resources.py`. Related helper, connection,
  publication, fixture and existing migration-refusal source was read only to
  verify integration. Unrelated changes were excluded.
- `atx-db/src/atx_db/cli.py:409` adds `--memory-limit` with default `1GB` and
  `--threads` with integer parsing and default `1`. The only handler change at
  line 763 invokes the established analytical helper before `publish_release`.
- The existing pending-migration check at `cli.py:756` still refuses before the
  writable `DuckDBStore` context at line 762. The change does not alter migration
  governance, store lifecycle, other command defaults or publication arguments.
- The existing helper at `cli.py:77` applies effective memory/thread limits and
  records both on the store. `atx-db/src/atx_db/connection.py:118` replays the
  recorded budget and insertion-order setting on replacement connections.
- The new parametrized test at
  `atx-db/tests/test_publication_resources.py:14` exercises the actual CLI with a
  private built warehouse. Its publication-boundary spy checks recorded budget
  values and live settings, closes/reopens the connection, verifies that the
  connection changed, and checks effective settings again. Default and explicit
  override cases cover `1GB`/one thread and `512MB`/two threads respectively.
  Patching `publication.publish_release` is effective because the CLI imports
  that function inside the handler. The returned `ReleaseResult` also permits
  the existing CLI serialization path to complete.
- Publication code remains unchanged. Its existing Parquet writer pins and
  restores thread/order settings, preserving deterministic export behavior when
  the helper disables insertion-order preservation. Existing artifact-level and
  migration-refusal tests remain in place without duplicate coverage.

## Root-owned pending checks

Exact focused selectors from `atx-db/`, to be executed under root's guarded
serial workflow after the active archive4 writer finishes:

- `tests/test_publication_resources.py::test_cli_configures_publication_resources_and_replays_on_reopen[defaults]`
- `tests/test_publication_resources.py::test_cli_configures_publication_resources_and_replays_on_reopen[override]`
- `tests/test_publication.py::test_cli_refuses_pending_migrations_before_opening_writable_store`
- `tests/test_publication.py::test_cli_read_only_preflight_preserves_missing_or_legacy_database`

The final selector includes both existing parametrized variants. Existing
publication tests retain artifact coverage; the full suite remains at the gate.

## Limits

This review is static only. No imports, tests, collection, lint/type checks,
database commands, probes, downloads, installs, production commands, source
edits or commits were performed. No actual warehouse was inspected. The sole
reviewer-created file is this report. The analytical budget takes effect after
store opening, consistent with the explicit scope and existing helper lifecycle;
it does not replace the mandatory outer process guard. No runtime success is
claimed. No fixes or additional review pass are requested.
