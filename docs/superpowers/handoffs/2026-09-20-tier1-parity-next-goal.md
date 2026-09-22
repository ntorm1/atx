# Goal prompt for the next parent agent (2026-09-20)

Paste as `/goal` (or the first message) in a fresh Claude Code session at `C:\atx`:

```
Continue the atx-db Tier-1 parity program on branch feat/tier1-parity. Read
docs/superpowers/handoffs/2026-09-20-tier1-parity-handoff.md first, then
.superpowers/sdd/tier1-parity/program.md (rulings) and the S3/S4 sprint ledgers it names.

Objective unchanged: Tier-1 parity (Compustat / Capital IQ / FactSet / Worldscope) for US
equity data as a quant systematic shop needs it — a full production DuckDB warehouse over the
entire listed US universe, point-in-time accurate, with standardized statement items, all core
ratios, growth/rate-of-change, quality/accrual/leverage/payout families, the daily market join
(market cap, EV, multiples, returns, momentum, vol), survivorship-safe universe and delistings,
deterministic code, published datasets. No LLM API spend; bulk archives over single requests.

Priorities in order:
1. Reconcile in-flight work (handoff section 3): commit or finish S3 T7, S4 T7, S4 T8,
   S4 T3 fix1, S4 T4 fix1 with pathspec-only commits; verify `import atx_db`, the module
   boundary test and the schema contract test pass at HEAD.
2. S3 T8 (retire the 18 covered modules on parity evidence), S4 T10 (retirement wave 2),
   S4 T11 (docs/CI truth pass; regenerate DATA_DICTIONARY.md).
3. Let activation run4 finish, then run the ladder from statement_points with --force
   (activation-run5) so every new stage runs over the full universe; watch
   activation_stage_runs and the logs; fix stage failures as tasks.
4. Measure item coverage, provider coverage SLOs and all quality checks on the live warehouse;
   publish the numbers in docs; flip schema conditions only on measured thresholds; publish a
   first full-universe release with publish_release.
5. Sprint gates: full non-slow suite once, opus whole-branch review, then ask the user before
   merging to main. Also remind the user about git stash@{0} (their other session's work).

Process: subagent-driven development — fresh sonnet implementer per task from the plan briefs
(S4 briefs are plan line ranges), one sonnet review pass, re-review only for Critical, Important
fixed and accepted on the implementer's report, focused tests only (full suite at gates), commit
per task with the trailer "Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>", keep your
own context (delegate reads; reports in files), parallel implementers only on disjoint files,
registry.py / jobs.py / activation.py edits serialized. Shared working tree: agents never run
git stash / checkout -- / reset / restore / clean; pathspec-only commits; never commit a
registry line for a module you did not write. Never send the user's email to an external
service (the SEC User-Agent uses the dummy contact atx-research@example.com). Ask the user
before merging to main or any other ask-first action; otherwise do not pause.
```
