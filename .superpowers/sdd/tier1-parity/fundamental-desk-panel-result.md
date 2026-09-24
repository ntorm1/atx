# DS2 implementation handoff

Final scoped Ruff passed in ds2-ruff3-memory.json (exit0, 0.061695 GiB peak).
The second lint run needed one remaining import separator; the parent fixed
that blank line. Together with ds2-tests3 (8 passed, 0.654999 GiB peak), the
one independent parent review and implementer fixes are accepted. All runs
used the exact 1.5 GiB external guard and isolated 256 MB / one-thread test
connections. The first test attempt failed fixture-module collection and was
corrected before the binder fix and final passing run. No live result, schema
migration, full-suite result, or source-coverage improvement is claimed.

Implemented the FQ1 validated desk reader in:

- `atx-db/sql/research/fundamental-desk-screen-acceptance.sql`
- `atx-db/scripts/read_fundamental_desk_screen.py`
- `atx-db/tests/test_fundamental_desk_panel.py`
- `atx-db/docs/FUNDAMENTAL_DESK_SCREEN_ACCEPTANCE.md`

The runner requires an existing database, completed FQ1 build run ID, report
as-of date, and fresh output path. It opens a read-only 256 MB / one-thread
DuckDB session with 2 GB spill cap, calls the public panel validator inside
one read-only transaction, checks exact default frozen specs and 200-day
freshness, and then executes parameterized SQL. Output is 1000 rows / 2 MB
maximum and installed only after successful completion.

SQL selects the latest observed decision session recorded in the run and
prints its 22:00 UTC cutoff separately from the report as-of date. It reads
four FQ1 single-signal inputs and scores by one run and decision date,
qualifies raw inputs separately from future-entry score eligibility,
preserves raw accruals and leverage signs, joins market on the trading
security ID and same session, selects whole latest visible market states
before checking fields, and emits independent missing-field counts, bounded
rejection reasons, and a deterministic preview.

Independent parent Codex review found four Important issues and no Critical
issue. Fixed all four: final-session raw input qualification, suppression of
rejected retained raw numbers, in-database byte preflight, and a tiny actual
FQ1 build/validate/read success plus digest-tamper test. Focused fixtures
also cover raw signs, issuer owner versus trading security, latest NULL
market revision and later cutoff row, empty cohort, run isolation, preview
cap, an oversized field, absent completed panel, default spec mismatch,
fresh output refusal, and overwrite refusal.

The parent's guarded focused run `ds2-tests3` passed all eight tests at
0.655 GiB peak under the 1.5 GiB external memory guard. An earlier guarded
attempt found DuckDB requires `octet_length(encode(payload))`; the parent
fixed that binder error before the passing run. Scoped Ruff `ds2-ruff1`
found only import formatting and a nested context manager style issue;
both are fixed without changing behavior. The existing SQL and document
line endings remain CRLF. No live desk result is asserted.
