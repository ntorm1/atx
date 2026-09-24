# Whole-branch static review: candidate 727e6b90

Review the committed branch candidate727e6b90 against its merge base
938ff7f178f21c2150ec83ca50cfee1e6542ed9a. Current main9562d315 contains the
earlier parity snapshot plus equity checkpoint22; no new merge is authorized.
There are38 branch commits and50 changed atx-db paths. Include the operational
controller/report changes for safety and truth, not only the last two tasks.
Use git show/diff for committed source; four unrelated working-tree EOL
modifications are not part of this candidate and must remain untouched.

This is the user's one whole-branch code review while memory blocks long
runtime. It can establish a static review result only. Full schema/numeric
verification, full non-slow suite, complete source/downstream build, measured
gates, first release and explicit permission before main merge remain open.
Do not approve those from static inspection or tiny test reports. If later
stage repairs change code, retain a precise reviewed-path/SHA record so their
own review and any genuinely necessary Critical follow-up are explicit.

Review correctness, point-in-time behavior, reproducibility, resource bounds,
and the practical quant workflow: raw source -> selected derived operands ->
issuer reader or dated market identity -> daily signal inputs -> future labels
and decile inference -> API/desk/release consumer. Look for concrete leaks,
unqualified ownership, stale-state revival, incompatible schema/definition
contracts, mixed runs or vintages, inference mistakes, unbounded memory, and
false completion/reporting. Existing source/lineage limitations are documented
open requirements; do not repackage them as newly introduced code bugs.

One fresh Codex review lead may delegate at most two static sections to Codex
children while covering the rest locally. Suggested disjoint review scopes:
1. DL1 publishers/DSL/lineage + IQ2 API/as-of and migrations0323.
2. FQ1/FQ2 research, labels, decile math, migrations0324/0325 and reader SQL.
3. Lead: dataset failure recovery, pipeline status/CLI, migration integration,
   test-bootstrap resources, dictionary/readiness/docs/controllers and synthesis.
Coordinate exact paths and cross-cutting interface findings. Every changed
production path must be accounted for in the final coverage matrix.

No Python, tests, database queries, network, source edits, commits or mutations
of shared files. Small static reads only. Root owns the sole runtime slot.
Never stash/reset/restore/clean/checkout--, and never print raw dataset params,
contacts or full private readiness diagnostics. Graph tools are unavailable;
use targeted rg/git source reads. Read current program rulings and existing
task review/result files to distinguish intentional limits from regressions.

Write severity-ranked findings with exact file/line, concrete trigger and
effect, and narrow repair guidance to whole-branch-review-2026-09-24.md.
Include reviewed SHAs, path coverage, verification limits and open runtime
gates. No source changes; findings will become fresh implementation tasks.
One review pass; Important fixes accepted on implementer report plus focused
evidence, Critical fixes re-reviewed. This process supersedes the earlier
Claude/Opus model request: Codex only, no external LLM API spend.
