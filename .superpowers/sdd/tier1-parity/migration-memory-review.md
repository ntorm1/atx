# MM1 independent review — 2026-09-24

Root's one independent review found no production-code blocker. One private
configuration helper bounds all three existing governed/restore connection
sites before opening. The512MB buffer limit reserves room within the unchanged
1.5GiB job cap for Python and checkpoint allocations. Lock, backup/hash,
schema/checksum verification, restoration and retention order are unchanged.
No extra close/reopen is added: a dirty close can checkpoint itself.

Important fixture repair: the initial test contract used an invalid declared_in
enum and selected internal/view columns from duckdb_columns. Four cases failed
in2.41seconds before exercising migration. Implementer must use an accepted
provenance enum and restrict the fixture contract to its actual physical tables.
Accept repair on implementer report and focused evidence; no Critical finding
or repeat review. Actual governed success/restore checks remain required.

Scoped Ruff reported four pre-existing migration_admin issues (UP035, two
UP017, SIM105); none comes from MM1's changed lines. Keep unrelated cleanup out
of this task. New test code has no Ruff findings. Live migration capacity is
unproven until the fresh governed retry completes under the same process cap.

Important repair accepted from implementer report plus focused runtime:
migration-memory-focused2 passed4cases in2.53seconds, peak0.602596283GiB
under1GiB. Production code unchanged after review. Root also linted the prior
HEAD via stdin and confirmed the same four pre-existing Ruff findings. No
repeat review or broad test rerun is needed before the live bounded retry.
