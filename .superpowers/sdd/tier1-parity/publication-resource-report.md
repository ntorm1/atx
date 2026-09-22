# PR1 publication resource implementation report

Status: source stable, uncommitted; fresh static review and root-owned runtime
validation pending. Implemented by the fresh Codex implementer on 2026-09-20.

## Changed paths

- `atx-db/src/atx_db/cli.py`: `publish-release` accepts `--memory-limit`
  (default `1GB`) and `--threads` (integer, default `1`). Its handler calls the
  existing `_configure_analytical_session` immediately before `publish_release`.
  The existing pending-migration refusal still precedes writable store opening.
- `atx-db/tests/test_publication_resources.py`: two parametrized cases exercise
  the actual CLI parser/handler, real migration preflight and temporary warehouse
  connection. A publication-boundary spy observes effective memory, threads and
  insertion-order settings, verifies the recorded analytical configuration,
  closes/reopens the real connection, and checks replayed effective settings.
- `.superpowers/sdd/tier1-parity/publication-resource-report.md`: this report.

The implementation adds only three lines to production source. The generic
store lifecycle, other commands' defaults, migration governance, release queries,
manifest/export contracts, clock argument, output-directory refusal and release
ledgers are unchanged.

## Exact focused selectors

Run from `atx-db/`, using the root's guarded, serial test execution after the
active archive4 writer has reached terminal state:

- `tests/test_publication_resources.py::test_cli_configures_publication_resources_and_replays_on_reopen[defaults]`
- `tests/test_publication_resources.py::test_cli_configures_publication_resources_and_replays_on_reopen[override]`
- `tests/test_publication.py::test_cli_refuses_pending_migrations_before_opening_writable_store`
- `tests/test_publication.py::test_cli_read_only_preflight_preserves_missing_or_legacy_database`

The default case requires `1GB` / one thread; the explicit override requires
`512MB` / two threads. Both assert live DuckDB settings before and after reopen.
Memory assertions use DuckDB's normalized MiB representation, consistent with the
existing production-activation test's `1GB` assertion. Existing migration-refusal
cases are reused without modification; the last selector includes both missing
and legacy warehouse variants. Existing publication tests retain artifact-level
coverage; the full suite remains at the release gate.

## Validation and limits

- Static source inspection and the scoped CLI diff confirm the narrow placement
  and unchanged migration-refusal ordering.
- No imports, tests, collection, lint/type checks, database commands or probes,
  installs, downloads, production commands or commits were run by this agent.
- No runtime result is claimed. Root owns all focused validation after the writer
  finishes; the fresh static review is also pending at handoff.
- The new tests replace only `publication.publish_release` with a boundary spy;
  they exercise CLI resource plumbing and replay, not artifact generation.
- The existing analytical helper applies settings after store opening. This task
  does not change the store lifecycle; the outer process guard remains required.
