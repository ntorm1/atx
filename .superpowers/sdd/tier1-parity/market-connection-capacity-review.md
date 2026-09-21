MP1 independent static review, 2026-09-21 UTC.

No concrete Critical, Important, or Minor findings were identified in this one
static pass. No draft correction is required by this review. Integration remains
subject to root's already planned focused checks after archive4 is terminal;
this review supplies no runtime, production memory capacity, or readiness claim.

The review covered the MP1 brief/report, both stable draft files, the exact draft
source diff against live `market_daily.py`, and the supporting store, dataset,
configuration, schema, and existing fixture/semantic-test paths. MCP graph tools
were unavailable, so discovery used static file reads and `rg`. The only file
written by this reviewer is this review. No Python, imports, tests, collection,
lint, mypy, database commands, probes, network, downloads, installs, live edits,
Git mutation, or subagents were used.

The following SHA256 hashes were checked against the implementer report and
matched at review time:

| Reviewed file | SHA256 |
| --- | --- |
| Live `atx-db/src/atx_db/market_daily.py` | `FF351DAF903A9887E18DA645B03A691A2D8C05CEDA68FD533F768210AF9BC78C` |
| Draft `market-connection-draft/src/atx_db/market_daily.py` | `CB1AC21ECA36801D20B0E571B64B1C29B0AD9C5A9B4353111083B15445973D7D` |
| Draft `market-connection-draft/tests/test_market_connection_capacity.py` | `745745F75554CEDA5E9BCC34E6E2A215162282796C41C049ADA49E6B78A7F473` |

Supporting hashes for live `connection.py`, `_companyfacts_resume.py`,
`tests/conftest.py`, and `tests/test_market_daily.py` also matched the report.

Static conclusions and their basis:

- Draft `market_daily.py:401-419` uses the CF5 eligibility rule: an existing
  persistent file and both recorded analytical fields. It checks non-internal
  temporary tables and views before initialization on the direct refresh path
  (`:427-428`). `MarketDailyDataset.ensure_schema` performs the same preflight
  (`:569-572`), which live `Dataset.run` invokes before creating its attempt ID
  or inserting its running ledger row. In-memory and partially/unconfigured
  callers skip the catalog refusal and connection replacement.
- Draft `market_daily.py:492-513` retains DELETE and INSERT in the existing
  per-batch transaction. INSERT results are exhausted into a detached list
  before transaction exit; total accumulation and close/reopen occur after the
  transaction context's successful COMMIT. An INSERT or COMMIT exception cannot
  reach that batch's recycle. The identifier list was already detached, and
  the daily implementation creates no task-owned temporary relation. There is
  no retained local cursor alias or active batch transaction crossing reopen.
- Live `DuckDBStore.close` checkpoints before closing, and `reopen` replays the
  existing base setup and recorded analytical settings. MP1 calls these methods
  without adding a resource default, increasing the budget, or changing the
  existing 200-security default. A close/reopen failure remains a post-commit
  failure and is allowed to propagate; no batch retry or rollback claim is made.
- The exact source diff leaves the calculation SQL, bind construction, source
  predicates, scoped DELETE, date limits, 300-row lookback, full-history and
  security selection, schema, hashes, P1 state ranking, and AF1 selection
  unchanged. The only changes are lifecycle eligibility/preflight, exhausted
  INSERT-result handling, post-commit recycling, and dataset preflight.
- Draft tests `:97-124` exercise actual persistent stores across batches of one
  and two securities, compare every logical output column against the retained
  connection baseline, assert exact counts, and require non-null trailing and
  valuation output. `expected_settings` is captured at `:100`, before even the
  baseline refresh, and `_observe_recycling` checks it before every close and
  after every real reopen. The spy delegates to the real lifecycle methods and
  also checks temporary relations and intermediate persisted row counts.
- Draft tests `:128-176` check explicit security/date scope, excluded alternate
  bar and derived sources, foreign securities/output ownership, exact totals,
  restoration of stale scoped rows, physical preservation outside scope, and
  a successful empty batch. The scoped rebuild compares against the full-range
  baseline, so truncated short trailing windows would be observable.
- Draft tests `:180-220` inject a duplicate primary key into the second batch's
  actual INSERT after its DELETE. Their expectations retain new first-batch
  rows and the physical old rows of failing/unvisited batches, with only one
  reopen. This directly targets per-batch rollback after prior committed work.
- Draft tests `:223-248` cover caller temporary tables and views through both
  direct refresh and `Dataset.run`, with initialization/close/reopen forbidden
  and both panel and ledger snapshots checked. Tests `:251-270` cover neither or
  only one recorded setting on persistent stores. Test `:273-307` uses a real
  in-memory connection with both recorded fields and checks preserved rows,
  settings, connection identity, and caller temporary state. Fixture teardown
  uses raw connection closure and does not call the patched lifecycle methods.
- The report's four P1/AF1 selectors exist and exercise the existing amendment,
  raw/DEI history, annual fallback, and explicit NULL-state daily behavior.
  Reusing them and the existing market tests is appropriate for this lifecycle
  change; this review does not request a broader branch test repetition.

The source/test reasoning above is static evidence, not a test receipt. Root
owns integration and the focused serial validation already listed in the MP1
report, under the existing runtime allocation. No repeat independent review is
requested; any later Important finding can be resolved by implementer report
and root checks, with a new review reserved for Critical findings.
