# LR2 — literal-only archive18 ledger recovery

Status: implementation and one authorized scratch acceptance complete; code
frozen for root's independent review. Live recovery remains pending. No
warehouse or source archive was opened by the implementer.

## Change and preserved scope

`close_companyfacts_archive18_headroom_stop.py` no longer binds Python scalar
parameters. Its four affected calls are the run-owned fact count, source receipt
outcomes, activation-ledger UPDATE, and dataset-ledger UPDATE. LR1 measured that
the first such binding on DuckDB 1.5.5 imports pandas/NumPy and adds approximately
0.510460 GiB of process private memory to this otherwise small operator script.

The adaptation validates canonical UUID strings, builtin nonnegative integer
counts, aware datetime inputs, and string inputs without NUL. Every SQL string
is escaped by doubling single quotes. Recovery time is an explicit UTC
TIMESTAMPTZ literal generated from the same aware `now`, retaining microseconds
and the existing UTC session. No user SQL, dynamic SQL identifiers, or global
library import suppression was introduced.

The existing ledger transaction was isolated into `_close_ledgers`; a main
guard permits scratch import of that exact function and the four literal
helpers without opening production. All other behavior remains:

- Refuse an existing recovery receipt; require the exact archive18 run,
  terminal session code, stopped-headroom guard, zero matching/original PIDs,
  and process evidence younger than five minutes.
- Require exactly one running activation row and exactly one running source
  dataset row with NULL finished times, using the original metadata predicates.
- Retain the full run-owned fact count, distinct/min/max CIKs, complete fact,
  point, price and custom-feature counts, latest migration version, and all
  run-owned source receipt outcome counts/sums. No scan or count was removed.
- Update only the original activation/stage and dataset/run scopes while they
  remain running; require one changed row in each ledger and roll back both
  if either fails. Preserve the reason text, returned fields, complete receipt,
  and final checkpoint.
- Use the existing 256 MB/one-thread live connection and 2 GB temporary-space
  cap. No params/email fields are selected or printed.

## Actual bounded acceptance

`recovery-literal-memory-check.py` imports the actual adapted helpers and runs
only a tiny in-memory DuckDB at 64 MB/one thread. The authorized guarded run
completed with exit 0 and **0.041278839 GiB native process-tree peak under a
0.5 GiB cap**. Receipt:
`recovery-literal-memory-check1-memory.json`; semantic results are in
`recovery-literal-memory-check1.json`, with `.log` and empty `.err` retained.

The actual SQL checked:

- Exact string/UUID/integer/aware UTC datetime values, including apostrophes,
  SQL-shaped reason text, and six-digit microsecond precision.
- Rejection of NUL text, noncanonical/malformed UUIDs, negative/boolean/string
  counts, and a naive timestamp before any ledger mutation.
- Wrong dataset scope rolls back the already-updated activation row as well.
- Exactly one eligible row changes in each ledger, while another archive,
  another stage and another dataset remain unchanged.
- TIMESTAMPTZ literals store the expected naive UTC TIMESTAMP values in the
  actual column types used by the ledgers.
- Repeat recovery is refused and preserves the completed rows.
- AST inspection finds no remaining bound `execute` call, and pandas and NumPy
  remain absent from `sys.modules` after the complete scratch operation.

The memory result applies to the scratch contract check. A live recovery still
performs all original production scans and requires its own guarded peak and
terminal receipt. Root will refresh process evidence immediately before that
run and obtain the actual archive18 dataset UUID from its output for the next
full archive resume. No source completeness or release readiness is inferred.
