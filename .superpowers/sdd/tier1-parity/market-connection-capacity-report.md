# MP1 daily market connection-capacity draft

Prepared 2026-09-21 UTC against `feat/tier1-parity`, inspected HEAD
`1c6535e8e194c222fbc646e33cf4f35be30bb972`. This is a static draft only.
Archive4 retained the sole runtime slot. No live source or tests were edited;
no Python/imports/tests/collection/Ruff/mypy/DB/probes/network/install/download,
Git mutation, or subagents were run. Graph tools were not available, so code
discovery used static reads and `rg`. The source diff and both draft whitespace
diffs were inspected. No runtime pass or production memory measurement is claimed.

## Exact files and integration

| Draft under `.superpowers/sdd/tier1-parity/` | Root integration destination |
| --- | --- |
| `market-connection-draft/src/atx_db/market_daily.py` | `atx-db/src/atx_db/market_daily.py` |
| `market-connection-draft/tests/test_market_connection_capacity.py` | `atx-db/tests/test_market_connection_capacity.py` (new) |

Both files are complete replacement/addition artifacts. A two-path live-relative
patch is also available at
`market-connection-draft/market-connection.patch` (SHA256
`B2135480B2459F969D7B4C3389E27C4762483E7AD2925A7B241EE750A55FC2F1`).
It modifies only `atx-db/src/atx_db/market_daily.py` and adds only
`atx-db/tests/test_market_connection_capacity.py`. Root should wait for the
writer's terminal receipt, confirm the source basis below, apply this patch or
copy only the two artifacts to their destinations, and inspect the exact scoped
diff. If live source has changed, merge MP1 hunks rather than overwrite other work.
No migration, registry, jobs, activation, connection, shared publication helper,
or public API snapshot changes are needed. The sole new function is private to
the existing module; imports and `__all__` are unchanged.

SHA256 byte hashes at handoff:

| File | SHA256 |
| --- | --- |
| Live source basis: `atx-db/src/atx_db/market_daily.py` | `FF351DAF903A9887E18DA645B03A691A2D8C05CEDA68FD533F768210AF9BC78C` |
| Draft source: `market-connection-draft/src/atx_db/market_daily.py` | `CB1AC21ECA36801D20B0E571B64B1C29B0AD9C5A9B4353111083B15445973D7D` |
| New draft test: `market-connection-draft/tests/test_market_connection_capacity.py` | `745745F75554CEDA5E9BCC34E6E2A215162282796C41C049ADA49E6B78A7F473` |
| Existing fixture basis: `atx-db/tests/test_market_daily.py` | `465C99598AB333229038754BF9ED305FD833D8F407A6AAADE267C8D73896F04A` |
| Existing lifecycle basis: `atx-db/src/atx_db/connection.py` | `CA82DA0FCE3F3917BDBB3201DE911F37771FC4395B1AC8DE9417B57CD4E7786D` |
| CF5 eligibility basis: `atx-db/src/atx_db/_companyfacts_resume.py` | `A226864AB6929F59EB38C5635B8818C6243ACC8DF364277D91669CF5ACED0828` |
| Schema fixture basis: `atx-db/tests/conftest.py` | `2930197A364E6A70216B392028A5D7AC780898535BE213E724796DD6FB687897` |
| Existing AF1 tests: `atx-db/tests/test_derived_annual.py` | `FBC494E482D96817BDBD846979B76E07F694D15ABFA26005703F3008034DA7CF` |
| Existing P1 tests: `atx-db/tests/test_derived_pit_revisions.py` | `DA21C08E7E132046955C2D412A2730F79827F8296B9C7C69B683CDAF4939CBD9` |

Git reported the repository's normal LF-to-CRLF conversion warning during
textual diff checks; no whitespace error was reported. Byte hashes may change
if root deliberately normalizes line endings; inspect the logical diff then.

The packaging follow-up preserved both accepted draft hashes and the unchanged
live basis above. The live source has 552 CRLF lines; the accepted source draft
has 540 CRLF and 42 LF lines, and the new test has 307 LF lines. The patch was
generated using `core.autocrlf=true`, the repository's current setting, and is
UTF-8 without BOM with LF-normalized narrow source hunks. It intentionally
omits EOL-only changes and does not require `core.autocrlf=false`. An applied
working file may therefore have different byte endings from its accepted draft,
while preserving identical content after CRLF-to-LF normalization.

Root owns the check-only step from `C:/atx`:

```text
git -c core.autocrlf=true apply --check -- .superpowers/sdd/tier1-parity/market-connection-draft/market-connection.patch
```

After the writer is terminal and root is ready to integrate, the corresponding
application command is the same command without `--check`. The implementer did
not apply the patch or run its check-only command. Packaging changed only the
new patch and this report; source and test artifacts remain untouched.

## Behavior and preserved contracts

`_market_connection_recycling` applies the same eligibility as CF5: the path
does not start with `:memory:`, is an existing file, and both
`analytical_memory_limit` and `analytical_threads` are recorded. It does not
invent settings or infer them from SQL. Eligible callers are checked for
non-internal temporary tables/views before initialization or mutation, and
refused with a specific `RuntimeError` if present (or the catalog query yields
no result). Unconfigured/partially configured and in-memory callers retain
their existing connection and session state.

The preflight runs at the start of `refresh_market_daily_metrics` and in
`MarketDailyDataset.ensure_schema`. The latter matters because `Dataset.run`
otherwise initializes and inserts its running ledger row before `load` calls
the refresh. A caller-temp refusal therefore precedes both paths' mutation.

After each existing successful security-batch transaction, the refresh adds
the already-fetched INSERT count to its total, calls `store.close()` (which
checkpoints), and calls `store.reopen()` (which replays the existing recorded
budget and base session configuration). An empty successful batch also recycles.
There is no extra final reopen, retry, failure-path reopen, or cadence increase.
The final successful call leaves the store open for downstream consumers.

The identifier list was already detached in the live implementation. The draft
eliminates the lingering local INSERT cursor alias, fetching only its scalar
count before transaction exit; no live cursor, transaction, or task-owned temp
relation crosses reopen. Existing failure behavior remains per-batch atomic:
DELETE and INSERT roll back together on an INSERT failure, while earlier
completed batches remain committed. A close/reopen failure propagates after
the successful batch commit rather than pretending that batch rolled back.

All calculation SQL is unchanged: daily DSL expressions, P1 state/NULL ranking,
AF1 derived state selection, PIT and availability clocks, source predicates,
date scope, raw/adjusted input handling, shares, hashes, schema, 300-row
lookback, full history/universe selection, and the default 200-security batch
size. No RAM, thread, query, history, metric, or issuer budget is increased.

## Focused validation prepared, not executed

The new file has six test functions / twelve parametrized cases:

- Real file stores, batches of one and two across three securities, complete
  logical-row comparison with the retained-connection baseline (only
  `source_loaded_at` excluded), non-null return/valuation values, exact row
  totals, and actual SQL settings captured before any first reopen. Every
  close/reopen checks those original memory/thread/order/timezone/spill settings
  and the absence of caller/task temp tables or views.
- Scoped dates and explicit IDs, a foreign security and foreign output source,
  later alternate bar/derived sources that must be excluded, repaired stale
  scoped rows, physical preservation of every out-of-scope row, and an empty
  final batch. Exact inserted total is 15 and all four batches recycle.
- A duplicate-PK INSERT injected into batch two, after its scoped DELETE,
  retaining the first batch's new rows and preserving failing/unvisited old
  rows including physical load timestamps. Only the first batch reopens.
- Caller temp table/view rejection through direct refresh and `Dataset.run`,
  before initialize, panel mutation, ledger mutation, close, or reopen.
- Persistent callers with neither/only one recorded analytical field preserve
  the same connection, real SQL settings, and caller temp table/view.
- A real in-memory connection with both recorded fields still preserves its
  database, output rows, actual settings, and caller temp table/view. It reuses
  the current five input/output table DDLs, avoiding another schema bootstrap.

The fixtures use 512MB/one thread and at most 100 baseline panel rows. They
reuse the existing market fixture helpers and floating-row comparator. Existing
P1/AF1/arithmetic matrices are reused through the selectors below, not copied.

After archive4 is terminal and the two files are integrated, root owns one
serial focused run from `C:/atx/atx-db`, inside the existing process-tree guard
and sole runtime allocation (no guard or memory-budget increase):

```text
python -m pytest -n 0 tests/test_market_connection_capacity.py tests/test_market_daily.py tests/test_derived_pit_revisions.py::test_exact_amendment_dependency_export_daily_and_projection tests/test_derived_pit_revisions.py::test_daily_raw_and_dei_history_preserves_pre_amendment_state tests/test_derived_annual.py::test_annual_only_values_growth_api_and_daily tests/test_derived_annual.py::test_precedence_switches_and_null_never_resurrects
```

Use the project's existing Python executable/guard command rather than a new
environment. The complete new-file selector expands to these exact functions:

```text
tests/test_market_connection_capacity.py::test_recycles_each_batch_with_budget_replay_and_identical_output
tests/test_market_connection_capacity.py::test_scoped_recycle_preserves_dates_sources_foreign_rows_and_empty_batch
tests/test_market_connection_capacity.py::test_failing_batch_rolls_back_after_an_earlier_committed_reopen
tests/test_market_connection_capacity.py::test_caller_temp_is_refused_before_initialization_or_mutation
tests/test_market_connection_capacity.py::test_unconfigured_file_keeps_connection_settings_and_temp_state
tests/test_market_connection_capacity.py::test_configured_memory_keeps_database_settings_and_temp_state
```

Then run the normal focused Ruff check on the two integrated paths and strict
mypy for `src/atx_db/market_daily.py`, also under root's sole runtime guard:

```text
python -m ruff check src/atx_db/market_daily.py tests/test_market_connection_capacity.py
python -m mypy src/atx_db/market_daily.py
```

No checks above were run or collected by the implementer. The one fresh
independent static review is complete in `market-connection-capacity-review.md`:
0 Critical / 0 Important / 0 Minor. Root accepted the source for later
integration; runtime remains pending. No additional review is needed for this
packaging-only follow-up. Any later Important findings are handled by
implementer fix/report and root focused validation; re-review only Critical.

## Limits

This closes a source-level retained-connection/index-state lifetime gap. It is
not evidence of an observed market-stage failure, production memory capacity,
full-universe readiness, or measured speed. A single existing security batch
can still fail within the configured SQL/process budget; this task intentionally
preserves its existing batch size and transaction atomicity. Checkpoint/reopen
adds I/O and connection setup cost that these tiny fixtures do not measure.

The production lifetime guarantee applies only to configured persistent stores.
Unconfigured and in-memory callers intentionally retain their prior lifecycle.
The existing replay restores recorded analytical/base settings; this draft adds
no global session-state replay, migration, new resource default, or shared
lifecycle policy. Root owns all integration, runtime receipts, production work,
and the later whole-branch gate.
