# DP1 independent static review

Reviewed 2026-09-20 against the current live `derived_metrics.py` and the complete
draft source/test, after reading the DP1 brief, implementer report, and focused
activation memory audit. **0 Critical / 1 Important / 1 Minor.** The connection
lifecycle change is coherent, but the identifier enumeration needs the Important
fix below before integration is considered ready.

This is one independent static pass on a stable, unintegrated draft. Archive4
retained the sole runtime slot. No Python, imports, test collection/execution,
lint, mypy, database/probe, network/install, live source edit, patch integration,
commit, destructive Git operation, or subagent was used. Only this review file
was written. Graph tools were not exposed, so discovery used static reads and
`rg`. The live-versus-draft textual diff and supporting store/PIT/test-fixture
source were inspected. No new runtime pass or production-capacity result is
claimed; existing live-source test results cannot validate this draft.

Verified SHA256 values match the implementer report:

- Live `atx-db/src/atx_db/derived_metrics.py`:
  `F953220EA651DD4F60BB1E205046CEE2966F89F1AAD9582412BD805E20978729`.
- Draft `derived-connection-draft/src/atx_db/derived_metrics.py`:
  `8C449A3B8DDA462DC366137CB13CC8B40701631707AF82A3D1833C1AAF6F6AC0`.
- Draft `derived-connection-draft/tests/test_derived_connection_capacity.py`:
  `D166FA37AC23AD4CEF08046DE98F12A339CC4BBFBC0CF93ECF31D97D63865042`.

References to `draft source` and `draft test` below mean those two exact draft
paths. Supporting live paths are relative to `atx-db/`.

**I1 — Important: each identifier page repeats enumeration over fact/output
tables.** Draft source lines 126–138 execute the complete remaining-security
`UNION` against `fundamental_standardized` and `derived_metric_values` on every
iteration. The default page size remains one (line 56), and the real activation
caller supplies only `run_id` (`src/atx_db/activation.py:793–799`), so production
issues this fact-level enumeration once per security plus the terminal query.
The prior live implementation executed the union once and fetched its result.

The new `LIMIT` bounds the returned identifier list; it does not materialize or
reuse the deduplicated universe. A page still asks SQL to discover the next
ordered distinct ID from the remaining fact/output rows. The required derived
key is `derived_value_id`, not `security_id`
(`src/atx_db/migrations/bodies_0302.py:117–120`), and migration 0315 explicitly
removes the optional lookup index
(`src/atx_db/migrations/bodies_0315.py:12–15`). There is no maintained ordered
identifier relation in this draft. Consequently the change introduces repeated
fact-level selection/deduplication work that grows with the page count; eligible
rows for later securities can be revisited across preceding pages. SQL pruning
may reduce individual scans, but the source provides no bound that makes each
source row participate only once in identifier enumeration. This is a concrete
scaling regression in the DP1 capacity path, independent of the AP1 index
changes. No execution plan, production elapsed time, failure threshold, or peak
memory was measured in this review.

Minimal fix: enumerate the exact existing union once into an invocation-owned,
persistent **ID-only** staging relation, then fetch bounded ordered keyset pages
from that relation. Persistent storage allows it to survive the ten-security
reopen without keeping a cursor or temporary object alive. Preserve the current
standardized bases, selected-source stale-derived IDs, ordering/deduplication,
explicit caller scope behavior, and metric dependency closure. Use a collision-
safe owned name and explicit cleanup on normal completion and failure; if the
generator owns the relation, ensure refresh explicitly closes it when a security
fails rather than relying on garbage collection. Do not transfer the complete
ID universe or fact/output frames to Python, increase budgets, or restore
secondary fact-table indexes as a workaround.

Extend the focused lifecycle check to establish that the fact-level union is
built once while multiple pages and reopens consume the ID relation. Keep the
early/later stale-only IDs and assert owned persistent staging is removed on
success and injected publication failure. The current seven-security fixtures
prove traversal behavior if they pass; they do not detect repeated enumeration
cost. Accept the Important repair through the implementer report and root's
focused checks; this review does not request another non-Critical review pass.

**M1 — Minor: the settings expectation is captured after a baseline reopen.**
Draft test lines 83–84 set cadence 100 and perform the baseline refresh over four
securities. Draft source lines 310–311 therefore perform a final-partial reopen
before the test records `expected_settings` at draft test line 100. A replay
regression on that first reopen could become the expected baseline for all later
reopens, allowing lines 120–125 to assert consistency with already-lost settings.
The real configured fixture is useful, but this order weakens its budget-replay
claim. Minimal fix: capture `SETTINGS_SQL` immediately after the configured
fixture is obtained and before the first refresh, assert the settings still match
after that baseline refresh, and retain the per-reopen comparisons. No production
source change is needed for this finding.

The rest of the requested contract has the following static support:

| Contract | Assessment and evidence |
| --- | --- |
| Full keyset coverage and stale cleanup | Draft source 131–137 retains all quarterly/instant/annual standardized IDs plus selected-source derived IDs. `UNION` deduplicates, `ORDER BY` is deterministic, and the boundary is applied to both arms. Deleting an earlier stale-only ID does not shift the boundary. Draft test 67–136 covers quarterly/annual/instant issuers, overlap, early stale-only A0/B0, later stale-only Z9, and an excluded foreign-source-only ID. This is logically sound for the unchanged single-writer input universe, subject to I1's enumeration fix. |
| Explicit scopes and dependency closure | Draft source 117–120 preserves sorted unique caller IDs and the 1–500 clamp; the metric closure code and scoped DELETE predicate are unchanged. Draft test 139–182 checks duplicate/unsorted scopes, preserved S2, the dependent growth rebuild, scoped return count, and an independent metric family retained in stale Z9. |
| Cursor, transaction, and owned PIT lifetime | The page uses `fetchall()` after SQL `LIMIT` (draft source 129–142), leaving only bounded Python rows while yielded; no separate result cursor is retained. Publication COMMIT/ROLLBACK remains at 291–302 and PIT cleanup at 304–305 precedes reopen at 308. Live `_derived_pit.py:356–359` drops the owned PIT tables, including annual state. The test's close hook checks no temporary tables/views and that BEGIN/ROLLBACK succeeds (112–118). |
| Per-security atomicity and failure cleanup | The live publication block is unchanged. `inserted` advances after COMMIT, and the cadence counter advances only after successful cleanup (draft source 291–309). A staging/publication failure does not count as a completed scope. Draft test 206–235 injects duplicate staging rows after an earlier security has committed/reopened, then checks earlier success, preserved old failing scope, and PIT cleanup. This is an actual PK-failure fixture, pending execution. |
| Cadence and final partial group | The constant is ten (draft source 41). Successful complete scopes, including zero-row cleanup, count at 306–309; a remaining partial group reopens at 310–311. An exact full group does not get an extra final reopen. Tests force cadence two across scopes 2/4/6/7, including a reopen inside a fetched page, and cadence one in the failure case. |
| Eligibility and configuration | Draft source 145–151 matches the existing CF5 predicate in `_companyfacts_resume.py:278–282`: persistent existing file and both recorded analytical settings. Reopen uses the established `connection.py:105–134` checkpoint/close/base-session/replay mechanism; the memory/thread/insertion-order/UTC/spill behavior is unchanged. The configured test uses real closes/reopens and SQL settings reads, with the M1 baseline caveat. |
| Caller temporary state before mutation | The eligible-session check runs at draft source 209–214, before initialize or PIT preparation, and again immediately before close (154–167). Its noninternal table/view scope matches CF5. Draft test 185–203 checks a caller table, caller view, and reserved `_pit_stage` table, asserts initialize was not reached, and verifies both caller data and canonical rows remain. No expanded contract for arbitrary unrecorded session configuration or caller transactions is assumed. |
| In-memory and unconfigured callers | These remain outside recycling and its temporary-state refusal. Draft tests 238–279 verify connection identity, caller temp retention, real output, and unrecorded settings/in-memory database preservation. The file-backed fixture configures the base session in `tests/conftest.py:172–183`; its UTC/spill expectations align with the store reopen path. |

P1/AF1 expressions, historical state construction, limits, annual fallback,
publication schema, return-count rules, and dataset diagnostics are unchanged in
the inspected live-to-draft diff. This review does not reopen their broader
semantics. The new tests reuse their existing fixture helpers and focus on DP1
lifecycle boundaries; the selectors in the implementer report remain suitable
for root's post-integration focused verification, together with the I1/M1 test
adjustments. Full-universe completion and production capacity remain unmeasured.
