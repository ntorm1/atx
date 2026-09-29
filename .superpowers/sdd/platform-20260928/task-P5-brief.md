# Task P5: PM pitch iteration 3 -- new sections in mega_report (code + tests; root regenerates)

**Pool:** C:/atx-wt/pool-11. `git status` clean, then `git checkout -B feat/platform-v7-p5-pitch3-20260929 a5e4a10d`.
**Rules (binding):** never build C++; never run real data (IC / NAV / fit / risk / decide); never spawn subagents; never
touch C:/atx; never read validation / VAL / 2023+ statistics beyond what the existing report config already prints; no
pushes. You MAY read (not write) the JSON / CSV outputs under C:/atx-wt/pool-2/build-equity listed below to learn their
schemas and to smoke-test rendering into your own pool (output under C:/atx-wt/pool-11/build-equity-p5/, gitignored or
deleted before commit). Python "C:/Program Files/Python312/python.exe". Trailer
`Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`. Write files with the Write tool (heredoc apostrophes break).

**Context:** atx-impl/tools/mega_report/{report.py, pitch.py, ...}; configs docs/plans/mega-alpha-v6-report.config.json and
docs/plans/mega-alpha-v6-pitch.config.json; current outputs docs/plans/2026-09-28-mega-alpha-v6-{report,pitch}.html. The
pitch reader is a portfolio manager / CIO: what the book is, what it earns after costs, how much capital it takes, what
can go wrong, how it is run day to day, and how honest the statistics are. Read task-L4-report.md, task-F2-report.md,
task-W3-report.md, task-W4-report.md, task-L2-report.md in C:/atx-wt/pool-2/.superpowers/sdd/platform-20260928/.

**Deliverable: seven new pitch sections, each driven by config (path + sha256 pin where the file is large), each
degrading to a visible "not available" note when its input is absent (no crash):**
| section | input (under C:/atx-wt/pool-2/build-equity) | content |
|---|---|---|
| Capacity curve | v7-l4-nav-stress/v7_extras.json | net SR and cost bps/$ at NAV .5/1/2/4/8x $1bn; chart + table; S2 primary, S2-KO / S2-FIM beside |
| Risk model bias | v7-f2-risk/bias_summary.json | bias statistic b by family (factor, random, book when present), structural-factor count, dropped counters |
| Alpha report cards | v7-w3-cards (and the v70 cards dir when configured) | one compact row per member: theme, tier, IC h5/21/63, turnover, decay, runner check; link/expand for detail |
| Monitor baseline | v7-w3-monitor-baseline | M1-M4 status table, alarms / warns / ok counts, the alarming sleeve named |
| Decide / orders / reconcile | v7-w4-* dirs | the daily operating loop: decide parity, wall time, share orders (lots, min notional), reconcile self-breaks and planted-break catches |
| Integrity statistics | nav_summ JSON (n33 / n34), PBO JSON | cell-count DSR vs effective-N DSR (never substituted), PSR(0), MinTRL, Lo, CSCV PBO, freeze-gate status |
| Trial ledger | trials.jsonl | every construction cell with verdict (accepted / rejected / defect), N over time, validation trials spent 2 of the budget, 2025+ reserved |
Also: the cell table must accept new cells from config (v7.0, spo-v2, v7.1, universe trial) with verdict badges, without
code edits per cell.

**Quality bar:** follow the existing pitch's visual language; charts readable in light and dark; numbers formatted
as in the current pitch; every number traceable to a file path shown in a footnote. Self-contained HTML (no external
scripts besides what the pitch already uses).

**Tests:** pytest with small synthetic fixtures per section (present / absent / malformed input); the existing
mega_report tests stay green.

**Report:** C:/atx-wt/pool-2/.superpowers/sdd/platform-20260928/task-P5-report.md (<= 40 lines): files, config keys added
(with an example block root can paste), schemas assumed per input, test counts, the exact root command lines to
regenerate report + pitch, anything untested. Commit in pool-11; reply to the parent in < 15 lines with the sha.
