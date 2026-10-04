# Existing Task1 repair2/5: P9 indexes point-in-time correction

Read Part A of p9-al-data-request-v1.md first: it is the binding requirement,
including all exact synthetic dates, both families, receipt keys and R1-R8.
Read task-27-p9-al-data-goal-addition.md and the prior Task1 report/fix1 context.
This is existing Task1 whole-task repair2/5 (prior1/5), not a new task/counter.
The old task's historical PASS claims do not certify the corrected current stage.

Own only these currently clean paths after controller dispatch:
- atx-db/src/atx_db/alpha_panel/indexes.py
- atx-db/tests/test_alpha_panel_indexes.py
- atx-db/docs/datasets/indexes.md
- atx-db/docs/datasets/indexes.registry.json
- New narrowly private index PIT/source/release helper modules and direct tests,
  named with private_index_ or test_private_index_ prefix. Declare exact paths
  before edits so other lanes can preserve them.
- .superpowers/sdd/tier1-v4/task1-p9-r2-report.md

No subagents. Do not edit foreign panel.py, security_master.py, common,
market_common, liquidity/market_index, FINRA, identity/universe/fundamental
producers, R23 runtime, consumer code/seals, Task17 golden/catalog paths or P9's
worktree. Source readers are read-only; preserve concurrent changes and bind
actual identities rather than assuming old cited lines still match.

Implement all four channels, not just intervals():
1. Complete rank selections independent of future exits, with immutable vintages.
2. Effective bounds from independent schedule/exchange calendar, no vendor-end
   dependence, including the last built reconstitution. Respect permitted open
   vintage alternative if used consistently and documented.
3. Separate source-qualified exits with the exact requested trigger/house clock;
   alive last_session is not an exit. Read latest eligible versions correctly.
4. Summaries and daily membership use only eligible vintage facts and exact row
   clocks. Register recon_summary.available_at instead of an unclocked view.
5. R7(a) dated eligibility: ignore static directory-derived decisions entirely,
   not merely mask a label while using a contaminated downstream flag. Rebuild
   decisive eligibility from qualified dated evidence; preserve unknowns and
   their effects on company ME/breakpoints, full populations and original gates.
6. Publish only v2 rule families, complete pinned available_at-year files and
   exact rank_vintage_rows_on_recon/counts_on_recon, exit_rows and
   directory_decided_on_rank receipts. No2024+parts opened for the P9 handoff.

Keep rules/GC9 bounds and full original acceptance; no future-trained choice,
automatic type assumption, invented exit proceeds or changed denominator.
The original pre2023 return evaluation/complete-label cutoff remains. No actual
return statistic is needed for these synthetic PIT corrections. P9 consumer
re-pin/bar decisions are outside this writer's scope. Preserve published legacy
outputs while preparing a versioned candidate and safe atomic activation path;
do not run a competing builder or silently rewrite source snapshots.

Reproduce the existing leaks with the exact A.4 fixture before correcting them.
Implement test_intervals_are_point_in_time_at_their_stamp with every specified
cut, planted X/W/Z/V/Y case, final scheduled end, both vendor extents and the
SP500P analogue. Add source-shaped dated-versus-directory eligibility checks,
receipt reconciliation and partition/manifest/clock validation. A helper that
always returns empty/unqualified data is not complete reachable engineering.
Missing actual originals can remain explicit prerequisites while supported
source-shaped paths are tested. Tests must distinguish code correctness from
actual coverage/source quality.

Author may run only controller-serialized focused offline synthetic fixtures,
<60s/invocation, file-backed128MB/one thread and ordinary external guard<=1GiB.
Root owns all real source/hash/build/data/HTTP/consumer runs. No new SEC request
budget is granted. Avoid unchanged suite reruns. Current financial work continues
independently, so do not modify its dependencies or kill foreign processes.

Report exact paths/contracts/source-clock derivation, before/after fixture
observations, retained failures and exclusions, commands/resources, source
requirements, data publication/gate/consumer statuses separately. Freeze exact
owned-path commits with the repository trailer after root clears index ownership.
Independent spec/quality review follows; no self-approval or Task17 counter change.
