# Task L7b: finish library v7.0 (pick up the stopped L7 lane in pool-10)

**Pool:** C:/atx-wt/pool-10, branch feat/platform-v7-l7-libv70-20260928 (base 4929824d). The previous L7 agent was stopped
with 11 uncommitted files (6 modified, 5 new: generator, library JSON + recipe, generator test, scripts/specs/v70.json).
Its last activity: the gate `require` mode ("any") in scripts/research_cycle.py. No report was written.

**Rules (binding):** never build C++; never run IC / NAV / fit on real data or any returns-conditioned statistic (synthetic
fixtures and `research_cycle.py plan` dry-run are fine); never spawn subagents; never touch C:/atx; never read any
validation / VAL / 2023 / 2024 / 2025 statistic; no pushes. Python: "C:/Program Files/Python312/python.exe". Commit trailer
`Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`. Heredocs with apostrophes break under the shell hook: write
files with the Write tool.

**Read first:** task-L7-brief.md (the original brief) and v7-prereg.md sections "Library v7.0", "Correction" and "Ruling
7.0-b" in C:/atx-wt/pool-2/.superpowers/sdd/platform-20260928/. The corrections OVERRIDE the original brief:
- there are NO new fields and NO fields v8 for wave 1: library v7.0 runs on lo1-fields-v7 unchanged (deliverable 2 of the
  original brief is void; q5_eg reads six existing fields, allowed);
- qmj_safety has an 8-slot budget exception (recipe row); max() and sign() allowed via POLICY_OPS for nincr;
- q5_eg slopes = RoF 2021 Table I Panel D (-0.029 / 0.516 / 0.771).

**Work**
1. Review the uncommitted work in the tree against the brief + corrections. Finish what is incomplete. Do not redesign.
2. scripts/specs/v70.json: fields = lo1-fields-v7 as-is (pins from scripts/specs/v61.json); u pass with library v7.0 on a
   new cache seeded so the 39 v6.1 candidates hit and 5 evaluate; fit ew-theme-v1 / v4-prior-v1; gate prints the five
   wave-1 admission rows and stops before w only if NONE is admitted; w; nav = the v6.1 final construction, L 1.247,
   output `mega-nav-v70u-ew-t.05-d.1-fixed-obdelta-x.05-loc-L1.247`; summ paired vs the v6.1 cell with --dsr-n 34 and
   `--ledger build-equity/trials.jsonl --effective-n dirs --psr --pbo`; card + monitor phases as in v61-ops.json.
3. `git merge a5e4a10d` (pool-2 HEAD: W5a/W5b/F2/W2/W3 landed after your base) and resolve conflicts keeping both sides'
   behaviour; then run the python tests you touched plus
   `-m pytest -q -p no:cacheprovider scripts/tests atx-impl/tools atx-impl/strategies` and fix failures you caused.
4. `research_cycle.py plan scripts/specs/v70.json` must print every line with pins resolved (run it from a checkout where
   build-equity exists: `cd C:/atx-wt/pool-2` is NOT allowed for writes; use `--help` to find a root/base-dir option, or
   report the exact blocker).
5. Commit everything (one or two commits). Tree clean.

**Report:** C:/atx-wt/pool-2/.superpowers/sdd/platform-20260928/task-L7-report.md (<= 40 lines): the five DSLs as
implemented, slopes + sources, files, test counts, merge conflicts resolved, the exact root command lines, expected u-pass
memory, anything untested. Reply to the parent in < 15 lines with the final commit sha.
