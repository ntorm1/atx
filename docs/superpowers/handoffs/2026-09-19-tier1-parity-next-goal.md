# Goal prompt for the next parent agent

Paste as `/goal` (or the first message) in a fresh Claude Code session at
`C:\atx`:

```
Continue the atx-db Tier-1 parity program. Read docs/superpowers/handoffs/2026-09-19-tier1-parity-handoff.md first, then .superpowers/sdd/tier1-parity/program.md (rulings) and the sprint ledgers under .superpowers/sdd/2026-09-19-tier1-*/progress.md. Work on branch feat/tier1-parity.

Objective unchanged: reach Tier-1 parity (Compustat / Capital IQ, FactSet, Worldscope) for US equity data as a quant systematic shop needs it — a full production warehouse over the entire listed US universe, point-in-time accurate, with standardized statement items, all core ratios, growth/rate-of-change, quality/accrual/leverage/payout families, daily market join (market cap, EV, multiples, returns, momentum, vol) from the ticker-history archive, survivorship-safe universe and delistings, deterministic code, published datasets. No LLM API spend; ML only for deterministic extraction assists. Prioritize bulk backfills from archives rather than single api requests.

Priorities in order:
1. Reconcile any in-flight/uncommitted work from the previous session (git status; handoff §5), commit or stash.
2. Unblock SEC data: use a dummy email for ATX_SEC_USER_AGENT. Then run scripts/warehouse_activate.py end to end on data/warehouse.duckdb (prices already loaded) and keep it running in the background while coding.
3. Finish Sprint 2 core tasks only (T6, T7, T9 — skip T8 industry templates and T10 fixtures), then rerun the ladder from companyfacts_load with --force.
4. Execute Sprint 3 fully (derived engine, market daily join, panel exports, parity + retirement, activation wiring; add api_schema_coverage_slo rows for the new schemas).
5. Execute Sprint 4 core tasks (universe, delistings with Shumway default on, quality gates, publish_release, data dictionary, docs/CI; skip LEI/FIGI).
6. Measure coverage on the live warehouse after each stage rerun; publish numbers in docs; flip schema conditions only on measured thresholds.

Process: subagent-driven development, fresh implementer per task from the plan briefs, one review pass per task, re-review only for Critical findings, focused tests only (no full-suite runs except at sprint gates), commit per task with the trailer "Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>", preserve your own context (delegate reads/audits to subagents; keep reports in files), run parallel implementers only on disjoint files. Ask the user before merging to main or any other ask-first action; otherwise do not pause.
```
