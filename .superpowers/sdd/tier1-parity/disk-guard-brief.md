# DG1 - optional disk floor for guarded warehouse writes

Production question: can the next full-universe source attempt be stopped before
its warehouse and spill writes exhaust the existing volume, without removing
backups or narrowing source scope?

At assignment, C: had approximately 12 GiB free alongside the approximately
12 GiB warehouse and 12 preserved full backups. No existing disk guard was
identified. This task extends only the operator's existing Windows process guard,
with focused contract checks and this task's brief/report. Registry, activation,
jobs, migrations, sources, backups and other sessions' work remain outside scope.

Add paired, opt-in `--disk-path` and `--min-free-disk-gb` arguments. Existing
commands without these options retain their memory-only policy. Resolve an
existing target, measure its volume before launch and in the current periodic
guard loop, and fail closed if the measurement fails. Below-floor preflight
refuses launch; a running failure terminates only the already-owned Windows job
tree. Disk status and the path/free-space/threshold observation must be recorded.
Keep native memory caps, launch headroom and hard memory stops unchanged.

Acceptance: root's independent review and focused checks for below/equal/above
floor, unavailable measurements and argument validation. The upcoming archive18
command is intended to opt in with `--disk-path C:\atx\atx-db\data
--min-free-disk-gb 3`. This polling guard is an emergency floor, not a reservation
or proof that the full pipeline fits. Do not delete, move or truncate anything.
