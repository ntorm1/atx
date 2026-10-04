# SEC baseline scope and producer accounting — code/document analysis

2026-10-04. Read-only recovery analysis; Task15 code remains frozen. No HTTP,
tests, warehouse/source-ledger reads, hashes or budget grant/reset. This report
alone is owned. No staging while Task17 owns the index.

## Baseline: origin established, interval and constituent total unresolved

The earliest exact budget assertion found in retained Markdown is
`gap-analysis.md:122`, whose header dates the read-only pass **2026-10-02** and
names branch `feat/tier1-v3-warehouse`, last atx-db commit `ca803eed`.
It says **25,762/60,000 used** in the announcement-clock earnings item, but
provides no constituent ledger list, start/end clocks, arithmetic or authenticated
attempt identity. A path-specific `git log` returned no history for this ignored
plan; its stated date is documentary evidence, not a recovered measurement time.
`task-9-report.md:14-18` repeats the figure as a cross-lane spec assertion on
2026-10-02; its recovery checkpoint (`:102-109`) says the original60,000limit
was events-ledger scoped, not all historical SEC activity. These statements
cannot establish a complete all-attempt baseline for the later shared ceiling.

Original EVT checkpoint `tier1-v3/task-EVT-report.md:122-124` records17,103
lane requests. Original TXT checkpoint `task-TXT-report.md:3,37` records3,303
at2026-09-30~01:35Z, including a stated200request allowance for one killed batch.
Neither report closes25,762. The v3 progress log records duplicate text loops
still running at00:50Z,01:00Z and01:28Z; stopped wrapper/report is not stopped
producer (`tier1-v3/progress.md:23-29`). Do not infer that checkpoint capped
subsequent requests.

Latest root audit (`sec-budget-chronology-audit-v2.json`, summarized in
`sec-budget-reconciliation-v2.md:11-34`) independently reports38,433text
receipts declaring38,678requests onSep29–30,17,103events outcomes onSep29–30,
and108,494earnings outcomes onSep26–27. It verifies only named stores and
declarations, not hidden/interrupted attempts or baseline interval. The
30,352arithmetic in `task-9-report.md:102-109` inherits the unclosed25,762;
it is not a current balance. Historical earnings are not automatically new
goal charges. No remaining capacity follows from either subtraction.

## What current code actually counts

| Producer/path | Count semantics and failure window |
| --- | --- |
| Text `filing_text.py:633-674,678-700,817-823,838-877,896-913,1019-1028` | CountingLimiter increments per adapter acquisition in process memory, including retries/redirects. Per-thread differences become receipt `requests`; prior usage sums retained receipts. Budget is per process based on that sum, not durable shared allocation. Kill after acquire/HTTP and before worker returns/receipt fsync loses counts; EX13 accumulates index and exhibit receipts until its worker returns. Concurrent duplicate loops can independently reserve the same remaining receipt-based budget. Comment at855 claiming killed batches lose no counts covers completed results only. |
| Historical events `events_sources.py:264-299,387-437` | Budget counts ledger lines plus logical fetch calls, not adapter attempts. `ensure` takes one unit then calls FetchLedgerStore; retries/redirects remain hidden. Harvest still uses this path and default limiter; current Task9 durable fetch protection does not cover harvest. |
| Current Task9 fetch/pilot `events_sources.py:123-161,172-255,782-786` | AttemptLimiter writes atomic counter and fsynced attempt reservation before inner limiter/HTTP. Single fetching process with shared-thread mutex; interrupted reservations conservatively overcount. Every adapter hop counted. Requires independently reconciled global_prior/reserved; arithmetic validation alone cannot authenticate them. Pilot max_attempts1 still invokes limiter for redirect hops. Not retroactive. |
| Earnings `scripts/fetch_sec_earnings_releases.py:1-25,62-85`; `press_release.py:1474-1518` | Uses default host limiter and FetchLedgerStore.ensure for original index/exhibit requests; no durable task attempt budget in this worker. Outcome ledger is not all attempts; shard/process invocations are allowed. Recheck repair may fetch again. |
| Shared adapter/store `sec_http.py:565-596,1001-1037` | Adapter calls limiter.acquire before each first try/retry and requests redirect send; max6attempts per hop. Store appends one final outcome after bounded body read and object write. Kill/exception before append can omit successful or failed HTTP, and retries/hops are collapsed into one outcome. |

Host limiter (`sec_http.py:259-297`) persists next_slot, not a lifetime budget
counter. Optional `ATX_SEC_RATE_LOG` (`:488-516`) is diagnostic, disabled absent
env; bounded write failures are nonfatal. Therefore neither current state nor
an incomplete grant log proves historical attempts. GC7 requires host ceiling
5/s; that rate requirement grants no new budget.

## Conservative bound supported by retained evidence

The retained `t4-r2-http-budget.json` and `task-4-report.md:477` support only
the named seven guarded handcheck executions: **1,127** attempts upper bound,
sum `ceil(5*(child_run_seconds+1))+1` per job. It includes retries, redirects,
headers and index work; completeness of that named invocation inventory is
explicitly conditional. This is already accounted in the older30,352arithmetic;
do not add it twice or generalize it to unrelated producers.

No finite objective-wide bound is proved by the inspected docs. First/last
receipt timestamps cannot bound unreceipted work before/after them. A200request
killed-batch allowance cannot cover subsequently live duplicate loops without
complete named commands, process exit intervals and configurations. Multiplying
receipt outcomes by6 would also miss redirect chains and entirely unreceipted
operations. Do not invent a total from those proxies.

## Smallest actionable local step

Root should first inventory **small retained guard/command receipts only** for
the explicitly named Sep29–30 text loops, events harvest/fetch and earnings
shards, with command, actual child execution interval, final exit, retry/redirect
configuration, rate-lock identity and any durable attempt-counter coverage.
Do this under the existing guarded metadata slot; exclude source bodies/large
ledgers. Reconcile duplicates/parent wrappers before summing per-child rate-time
bounds. Ask the plan owner for the25,762measurement's constituent stores and
interval if retained metadata cannot supply them. Missing execution inventory
must remain unknown; HTTP hold and existing cap persist. A future durable
all-attempt adapter wrapper for text/harvest/earnings is prospective prevention,
not historical recovery or a new Task15 gate.

Commands used: targeted PowerShell Get-Content/Select-Object on named code and
reports; rg exact baseline literals restricted to Markdown after discovery;
path-specific git log; no code changes. Graph discovery tools were unavailable.
One initial broad rg of the SDD directory also matched retained JSONL disclosure
snippets/review diffs; it was immediately narrowed to Markdown. This was an
avoidable scope deviation, not a guarded source audit; no data/raw ledger or
warehouse was opened, and no source qualification derives from those matches.
