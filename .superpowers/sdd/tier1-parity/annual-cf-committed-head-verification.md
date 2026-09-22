# Committed source verification before archive4

Root used isolated `git archive` exports and the project Python, each under a
2.5 GiB job guard. No live warehouse connection was opened by these checks.

The first export used AF1 source commit `244af575`. Package import resolved
inside the export. All schema-contract cases passed except the pre-existing
slow-test skip; six of seven module checks passed. The sole failure was the
exact absent `_companyfacts_resume` / `_derived_annual` snapshot names.
Peak job memory was 0.9624214172363281 GiB.

The root documentation/snapshot commit initially encountered ignored-path
staging, so the first export started before that commit succeeded. Root then
force-added only the exact intended evidence paths and committed `c33b5ccd`.
No production source or schema-test content changed from 244af575 to c33b5ccd.
Root verified this with an empty path-limited git diff.

An isolated c33b5ccd export then passed package import and all seven module
checks, peak 0.6143646240234375 GiB. This checks the snapshot correction without
repeating the unaffected schema fixture. The regenerated dictionary includes
201 definitions and API 2.1 metadata; its prose now distinguishes event clocks
from arithmetic input clocks.

Evidence is retained in annual-cf-head.log / -memory.json and
annual-cf-head-module-final.log / -memory.json. This is focused committed-source
verification, not the full sprint test gate or production coverage evidence.

With no other matching runtime process active and 114,996,756,480 free disk
bytes observed, root launched `activation-companyfacts-archive4` at
2026-09-20 23:03:23 UTC under the 3 GiB guard, DuckDB 1 GB/one thread. It uses
the exact failed UUID and local archive specified in production-resume-after-cf5.md.
The guard command uses only the dummy SEC contact. Its live result is pending;
the launch does not establish migration, proof or ingestion completion.
