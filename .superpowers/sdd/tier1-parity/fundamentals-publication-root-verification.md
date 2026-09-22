# AP1 root verification

2026-09-21 UTC. One fresh static review0/0/0. Initial focused set47passed,
one new fixture expected only complete calendar TTM despite the existing table
retaining partial periods. Implementer fix1 corrected the oracle to all four
ordered periods; root read/accepted the report and the failed selector PASSED.
Production arithmetic was unchanged. Initial peak0.9532GiB; rerun0.7193GiB.
No re-review needed. Strict mypy helper/sharedbulk/0318passed. Scoped Ruff
verification on11paths found0new diagnostics (2pre-existing oncalendar/schema).
All executed serially under2.5GiB guard; actual warehouse was not migrated.

Eight complete outputs retain physical constraints/atomic swaps. Optional
indices are retired by0318 only;0317 lines preserved. Root added exact private
module pin. Full-universe disk/memory/scan behavior remains to be measured.
Combined committed-HEAD import/module/schema checks follow final integration.
