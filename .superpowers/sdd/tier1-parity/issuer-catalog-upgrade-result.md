# Issuer API catalog upgrade integration - 2026-09-22

Applied reviewed draft CAE918966E345766936DC00CF5719A6B26CEE3EEC1E55263308D95B6093E0AE2
and registered0322 after committed0321/ddcd49d8. One review found noCritical;
Important scope and runner-upgrade fixture changes were fixed and accepted
on the implementer's report. No additional review round was required.

Guarded focused upgrade test passed in127.76s, including schema bootstrap,
actual pending-migration application from the simulated0321 predecessor,
all six issuer schemas/fields/hashes, unchanged unrelated catalogs/pricing,
schema pin, and replay stability. Root performed scoped import cleanup with
Ruff. No live production DB migration or source stage ran during this task.

Next: commit body/registration/test together, verify import atx_db and module/
schema contracts atHEAD, then resume full archive11 with the actualarchive10
predecessor under unchanged2GiB job/1GB DuckDB/1thread/backup-keep100 policy.
