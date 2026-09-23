# Pipeline ledger status

Run `atx-db pipeline-status --db-path PATH --as-of-date YYYY-MM-DD` to inspect one
activation snapshot. The command prints one JSON object and exits 0 when the
three ledgers can be read. A missing database, missing ledger table, or an
unavailable/locked database produces `status: "unavailable"`, a short `reason`,
and exit code 2. It never creates or repairs a database.

`migrations` lists registered, applied, and pending versions. A version in
`lower_version_gaps` is missing despite a higher version being applied.
`stages` follows the activation ladder order and includes an explicit
`status: "missing"` row for each stage with no attempt for the requested date.
For a stage with multiple attempts, the newest `started_at` wins, with `run_id`
as a deterministic tie breaker. A stage belongs to the date only when its
recorded `params_json.as_of_date` is a valid, exact date match. Missing or
malformed snapshot metadata cannot be assigned to a date.
`unattributed_attempts_present` flags ledger rows whose snapshot date is
missing or invalid; it does not assign those rows to the requested date.

Stage rows show the recorded status, activation label, start/end times, ledger
rows, and whether an error was recorded. Source stages also show whitelisted
scope controls. Neither parameters nor error text are returned. Ledger rows
are recorded stage outcome counts, not durable table row counts.

For `submissions_load`, `earnings_release_facts`, and `companyfacts_load`,
`source_attempt` locates a dataset run by its known dataset ID, the exact
activation-generated run label in dataset metadata, and its start time inside
the activation attempt's completed interval. `match` is `unique`, `missing`,
`ambiguous`, `invalid_uuid`, or `unknown`. A unique match exposes the actual
dataset UUID and recorded status. `resume_candidate_run_id` is populated only
for a uniquely matched failed UUID. It is a candidate for the source's normal
resume procedure; that procedure must still verify its own source proof.
An unfinished activation interval has an unknown source match.

`ledger_completion` says whether every stage's latest attempt for the date is
recorded as completed. `production_readiness` stays `unassessed` and
`process_liveness` stays `unknown` even if the ledger is complete. A recorded
`running` stage sets `operator_check_needed` because a persisted row cannot
prove that its process is alive. The report does not close or restart runs.

The command opens the existing DuckDB file read-only with a 256 MB memory
limit, one thread, and UTC configured at connection time. It reads only
`schema_migrations`, `activation_stage_runs`, and `dataset_runs`; it never
scans source fact or price tables.
