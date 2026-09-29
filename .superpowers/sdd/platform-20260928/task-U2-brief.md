# Task U2: role linked-operating-v3 = lo2 rule with atx-db SIC events (code + tests + prereg draft; no runs)

**Pool:** C:/atx-wt/pool-9. `git status` clean, then `git checkout -B feat/platform-v7-u2-role-sic-20260929 a5e4a10d`.
**Rules (binding):** never build C++; never run IC / NAV / fit or any returns-conditioned statistic; never spawn
subagents; C:/atx is READ-ONLY (atx-db docs and stage exports under C:/atx/atx-db/data/alpha_panel/v1; never write, lock
or kill anything there); never read validation / VAL / 2023+ return statistics (stage coverage counts are fine); no pushes.
Building the role into YOUR pool (C:/atx-wt/pool-9/build-equity/...) from the pool-2 base role is allowed: it reads
prices-derived membership and identity only, takes ~2 s, and is how W5b measured lo2. Python
"C:/Program Files/Python312/python.exe". Trailer `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`. Write files
with the Write tool (heredoc apostrophes break).

**Problem (W5b-F1):** role lo2 (linked-operating-v2) drops 53,691 score-window member cells as `no_visible_sic` (lo1: 34)
because pool-2's build-equity/fundamental-events-v2 lacks SIC rows for the CIKs newly linked by identity-bridge-v2-pit.
atx-db publishes SIC events: C:/atx/atx-db/data/alpha_panel/v1/fundamentals/{sic_events.parquet, manifest.json}; docs
C:/atx/atx-db/docs/ALPHA_PANEL_FUNDAMENTALS.md, ALPHA_PANEL_IDENTITY_SECURITY.md. Read task-W5b-report.md and the role
code in atx-engine/tools/prepare_recent_research.py (the `role --universe linked-operating-v2` path and its --sic-events
reader).

**Work**
1. Add universe rule `linked-operating-v3`: identical to v2 except the SIC source is the atx-db fundamentals stage
   (sic_events.parquet, pinned by the stage manifest sha256). Keep the PIT clock of the existing rule exactly (visible
   when accepted / available_at < date(t-1) 22:00 UTC, age <= 550 days; map the atx-db columns to it and document the
   mapping). v1 and v2 outputs must stay byte-identical (test).
2. IMPORTANT consistency: the fields builder derives grp_ff12 / grp_* and the fundamentals link from its own SIC source.
   The v1 rule guarantees finite(grp_ff12) == linked-P and visible SIC. State precisely what breaks if the role uses the
   atx-db SIC while fields still use fundamental-events-v2 (members with NaN industry -> neutralisation, risk model
   industry factor), and implement the smallest correct option: a `--sic-events` override for the fields builder's
   grp_* fields so role and fields share one SIC source (all other field payloads byte-identical on cells where both
   sources agree; report the count of cells where the two SIC sources disagree on FF12 / FF49 for lo1 members).
3. Measure in your pool: kept member cells per year and drops by reason for lo3 vs lo2 vs lo1; how many of the 53.7k
   cells are recovered; SIC disagreement counts. No returns.
4. W5b-F2 (read-only): characterise the 820k unlinked base-role member cells from the security master / identity stage
   (ETF / ADR / unit / non-filer / link gap shares) and write the question list for atx-db.
5. Draft the universe-trial pre-registration paragraph (role lo3 + fields rebuilt on lo3; what is held fixed; acceptance
   mirrors V6-U in .superpowers/sdd/mega-alpha-20260926/v4-prereg.md; N + 1) and a research_cycle spec template
   scripts/specs/<name>-lo3.json with the library left as a parameter root fills in.
6. Tests (pytest, synthetic): v1 / v2 identity, v3 SIC mapping and clock, fields override identity.

**Report:** C:/atx-wt/pool-2/.superpowers/sdd/platform-20260928/task-U2-report.md (<= 40 lines): rule text, column
mapping, measurements, files, tests, exact root command lines (role lo3, fields on lo3 with --reuse where valid, risk
model on lo3), expected wall / RSS, the prereg draft, concerns. Commit in pool-9; reply to the parent in < 15 lines.
