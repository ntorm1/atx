# AF1 annual-filer fallback: isolated implementation draft

Date: 2026-09-20. Branch observed: `feat/tier1-parity`.

Refreshed base: `0c52fdbd3f9e2031fdb9e15ca1fe7cfc2f458d1e` (Core), following
P1 commit `5519d1ac`. Root reports the P1/Core focused checks passed; those checks
do not validate this unapplied AF1 draft.

**Result: concrete static draft prepared; not applied or runtime verified.**
All source, test, migration and patch writes are under
`.superpowers/sdd/tier1-parity/annual-fallback-draft/`. This report is the only
write beside that directory. No actual `atx-db/` file was modified. No Python
process/import, test, database connection, production command, source fetch,
commit, stash, checkout, reset, restore, clean, subagent or external LLM/API was
used. The existing reviewed P1/Core work remains untouched. Runtime correctness,
memory behavior and production coverage remain unmeasured. Root must apply,
verify, independently review and commit before treating AF1 as complete.

## Implementation

The new private `_derived_annual.py` helper supplies exact-span annual input
states to the existing `_derived_pit.py` event evaluator. There is one canonical
source/security/metric/window/bucket/definition event stream; no public annual
metric catalog or alternate consumer table was introduced.

`prepare_security` admits annual standardized rows with nonnull availability
and actual 330--380-day inclusive durations. Annual states are separate from
quarterly/instant `_pit_items`. Whole structs are selected by availability and
stable source/rule/ID ties within an exact start/end pair, preserving explicit
NULLs. A visible annual span is selected per bucket with the same newest-period
then event/stable-source precedence. All annual operands must match that exact
span and the event-visible output endpoint. A later span selection is an event;
incompatible concepts become unavailable instead of combining different spans.

For a catalog `ttm(scalar)` definition, the helper expands scalar metric
dependencies using their existing registry ASTs. It then lowers that scalar
against annual inputs using the unchanged arithmetic DSL. Thus gross profit,
EBITDA, capex and FCF reuse their real formulas rather than gaining separate
hand-maintained arithmetic. Nothing publishes those annual operands under
`gross_profit_q`, `ebitda_q`, `capex_q`, `fcf_q` or raw quarterly item codes.

At each candidate event the evaluator prefers the valid four-quarter result
when comparable with the annual endpoint/span. Comparison verifies four actual
70--120-day, contiguous quarter spans covering 330--380 days, with the same
annual start/end. If the quarterly candidate is unavailable, nonfinite or
incomparable, a finite direct annual alternative may supply that endpoint.
Without annual evidence, existing quarterly-only numeric/status semantics stay
unchanged, including nonfinite results and legacy fixtures without start dates.

Annual source changes, span selection changes and quarterly/dependency changes
all enter the existing event-key relation. Later quarterly completion can
replace annual fallback, and later quarterly invalidation can restore the
currently visible annual alternative. A later annual NULL replaces its older
numeric state; there is no latest-valid search. A reported zero is a valid
annual total. Precedence changes retain event time as `available_at`, while
the selected arithmetic clock can be an earlier annual filing time.

Selected operand metadata follows the existing expression's coalesce branches
and window offsets. When annual evidence participates, scalar operands must
have matching current spans/endpoints; annual comparisons must have comparable
year-separated dates. The existing dense bucket offsets remain: four for YoY,
four times N for N-year CAGR, and four for opening/closing balance averages.
Missing target years and quarters stay missing. No fractional quarterly values
or general relaxation of `ttm`/quarter-window lowering was made.

Annual weighted-average basic/diluted share inputs are permitted only in TTM
denominator contexts. The current catalog's applicable denominator consumer is
`eps_ttm`, using diluted weighted shares. With an annual-backed numerator it
selects the matching annual denominator; otherwise annual shares can fill a
missing quarterly denominator at that exact annual endpoint. Annual shares are
never exposed as a reported quarterly item. The existing `eps_basic_ttm` and
`eps_diluted_ttm` also accept actual annual reported EPS through the direct
rollup route; no new basic weighted-share metric was invented.

## Canonical metadata and consumers

Draft migration **0316**, reserved only in the draft, adds:

* `value_origin`: `annual_fallback`, `annual_dependency`, `quarterly`, `instant`,
  `scalar`, `incomparable`, `unavailable`, or `legacy_unspecified`.
* `fiscal_period_start`: selected current duration start, or NULL for pure
  balance/scalar inputs or missing span evidence.
* `fiscal_period_end`: selected current operand endpoint. Earlier comparison
  operands retain their own source/dependency lineage.

These fields are part of canonical publication and state compression. The
public derived schema becomes 2.1.0 and exposes them directly; daily input
lineage also includes them. Existing API/daily whole-state selection consumes
the one canonical stream, including its NULL transitions, without another
precedence rule. Old rows receive `legacy_unspecified`; migration does not
invent annual origin, revision history or historical delivery evidence.

The engine identity contract is `quarter-pit-v3-annual`. Definition fingerprints
include the expanded annual AST and weighted-share policy as well as the
existing definition fields. Input hashes include annual source state IDs,
values, clocks, exact spans and span selection. Event IDs remain independent
of run/load time. As in P1, conservative frame lineage can preserve additional
same-value transitions when an unselected alternative changes.

## Supported catalog semantics

All 26 current top-level TTM rollups receive direct annual alternatives:

`revenue_ttm`, `cost_of_revenue_ttm`, `gross_profit_ttm`, `sga_expense_ttm`,
`rd_expense_ttm`, `depreciation_ttm`, `operating_income_ttm`, `ebitda_ttm`,
`interest_expense_ttm`, `pretax_income_ttm`, `income_tax_ttm`, `net_income_ttm`,
`net_income_common_ttm`, `eps_diluted_ttm`, `eps_basic_ttm`, `cfo_ttm`,
`capex_ttm`, `fcf_ttm`, `dividends_paid_ttm`, `common_dividends_ttm`,
`share_repurchase_ttm`, `share_issuance_ttm`, `debt_issuance_ttm`,
`debt_reduction_ttm`, `stock_compensation_ttm`, and `acquisitions_ttm`.

`supported-rollups.json` records every code's existing expression and inputs.
Twenty-two use a direct annual item. The four composed cases use the existing
gross-profit coalesce/revenue-minus-COGS formula; EBITDA's standardized/operating
income plus D&A alternatives; positive `abs(capex)`; and CFO minus positive
capex. All sign, safe-division, negative-base YoY and positive-endpoint CAGR
rules remain the existing DSL rules.

Their existing descendants receive annual-backed values when dependencies are
present: `total_payout_ttm`; TTM per-share measures; margins and tax/NOPAT;
ROA/ROE/ROIC and average-balance measures; annual TTM growth/CAGR and changes;
turnover/days/cash conversion; cash-flow and profitability measures; accruals,
payout and flow-based leverage/coverage; quality composites with the required
inputs; and the existing daily valuation/yield descendants. Annual year-end
instant-only liquidity, leverage and book measures continue using their
existing input path.

The prepared annual-only fixture supplies FY2024 revenue1000, GP400, income100,
CFO160, capex-60, weighted diluted shares50, reported EPS2, equity300, current
assets250/liabilities125, and end shares50. It expects margins40%/10%, sales
per share20, CFO/share3.2, FCF/share2, EPS2, current ratio2, and at price20:
market cap1000, P/E10, P/S1, P/CF6.25 and FCF yield10%. It supplies both
`net_income_total` and `net_income_to_common` at100 because the unchanged
`net_margin` formula requires the former; common income alone does not satisfy
that catalog definition. FY2023 revenue900 supplies 11.11% YoY. Prior assets800
and equity250 demonstrate annual balance averages, ROA, ROE and accruals.

Material limits are explicit:

* Quarterly reported-flow outputs and their QoQ/YoY remain unavailable without
  quarterly facts. Annual-backed TTM QoQ and quarter-count volatility are also
  withheld; annual endpoints do not establish quarter-count observations.
* Annual comparisons retain the existing calendar-quarter bucket positions.
  Fiscal-year-end changes across buckets do not receive a new fiscal calendar
  alignment; incomparable dates or missing years remain unavailable.
* Exact span selection is deliberately conservative when different concepts
  report different full-year starts at the same endpoint. It can withhold a
  metric rather than combine those spans. A fiscal total is never moved to a
  later quarter's endpoint.
* Quarterly-only states with absent or conflicting source span evidence retain
  existing numeric behavior and are marked `incomparable` where appropriate.
  They cannot establish comparability against an actual annual span.
* Period-end per-share formulas require real instant share counts. The helper
  does not replace them with weighted shares or create absent catalog variants.
* Omitted upstream concepts are still not withdrawals. Modeled filing events
  do not establish source delivery or local-observation vintage certification.

## Bounds and publication

The 250,000-input-row per-security guard now includes annual rows, even rejected
stub annual candidates at the pre-materialization count. Each annual temporary
state/span relation is bounded by that input scope. For each metric, candidate
work is guarded by `E * W + T + A`, where E is normal input/dependency events,
W is the existing local frame width, T is target metadata events and A is
relevant annual input/span events. Annual events affect their own endpoint;
the existing DAG propagates later growth/dependency effects.

The 250,000 candidate, 8,192 frame-row, 1,024 key-chunk and 100,000 publication
scope defaults remain. The new ASOF joins select one exact-span annual state
per code/frame row. No Python fact panel, universe/event product, unbounded
fallback, raised memory limit, extra thread or new index is introduced.
SQL scalar provenance expressions add work; their runtime/byte cost is
unmeasured. Existing 1GB/one-thread settings and root's process guard remain
necessary. Scope closure and the atomic per-security DELETE/INSERT transaction
are unchanged; staging/limit failures happen before publication and PK failures
still roll back the complete security scope.

## Paths and integration

All paths below are relative to `annual-fallback-draft/`:

| Path | Kind |
| --- | --- |
| `atx-db/src/atx_db/_derived_annual.py` | New helper |
| `atx-db/src/atx_db/_derived_pit.py` | Existing event integration |
| `atx-db/src/atx_db/derived_metrics.py` | Annual plan/input/candidate integration |
| `atx-db/src/atx_db/migrations/bodies_0316.py` | New forward migration |
| `atx-db/src/atx_db/migrations/registry.py` | Draft0316 registration |
| `atx-db/src/atx_db/migrations/__init__.py` | Draft0316 facade cleanup |
| `atx-db/src/atx_db/api/catalog.py` | Public origin/span fields, version2.1 |
| `atx-db/src/atx_db/market_daily.py` | Selected canonical origin/span lineage |
| `atx-db/tests/test_derived_annual.py` | Eleven functions / thirteen parameterized cases, unrun |
| `atx-db/tests/test_derived_pit_revisions.py` | Upgrade now includes0316; legacy origin assertions |
| `atx-db/tests/test_panel_export.py` | Public derived version expectation2.1 |

For the eight changed existing paths, `baseline/<same-relative-path>` contains
the pristine bytes matching committed HEAD `0c52fdbd3f9e2031fdb9e15ca1fe7cfc2f458d1e`.
`baseline-sha256.json` has their exact SHA-256s. `path-inventory.json` lists all
eleven delivered source/test paths, their new/modified status and draft hashes.
`source-evidence-sha256.json` pins the read-only DSL, registry, catalog CSV and
standardizer used to interpret formulas. `integration.patch` includes all
eleven paths, including complete additions for the three new files.
`base-commit.txt` records the full integration-base commit.
`refresh-receipt.json` records the mechanical refresh and artifact hashes. The
refreshed 61,704-byte patch SHA-256 is
`f3711931e29b5d95c50c8cc85d24f8ab34a803b77c0c826d32d8418dde97319d`.

At the final static inventory check all eight live paths still matched their
pristine draft baselines. This is not authorization to overwrite future fixes.
P1 and Core are now committed. Compare these baseline and source evidence hashes
against the actual committed/live files immediately before application. If any differs,
regenerate/rebase the isolated draft against the final committed source and
review that delta; do not copy draft files wholesale over newer work.

The mechanical refresh preserved exactly the two post-baseline P1 edits:
removal of the extra blank line after `_derived_pit.py` imports and the raw
duplicate/primary/constraint regex in `test_derived_pit_revisions.py`. Both
pristine baselines and AF1 draft copies contain them. All eight baseline paths
and all four read-only source-evidence paths were checked against HEAD with a
read-only Git diff; none differs. The source-evidence hashes are unchanged.
AF1 semantics, formulas, new files and the unrelated Core fixture were unchanged.
Baseline, evidence, draft inventory and patch hashes were regenerated.

From `C:/atx`, after the hash comparison and root's committed-HEAD verification:

```powershell
git apply --check .superpowers/sdd/tier1-parity/annual-fallback-draft/integration.patch
git apply .superpowers/sdd/tier1-parity/annual-fallback-draft/integration.patch
```

These apply commands have not been run. Migration0316 exists only in this
draft; no live registry or historical migration body was edited. Root should
regenerate public schema/data-dictionary artifacts under the existing integration
gate after applying the final code, and rebuild canonical/daily consumers only
through root's authorized production workflow.

## Prepared verification, not executed

The new file covers annual-only values and actual standardizer FY-minus-9M
versus missing-9M behavior; annual restatement; quarterly completion and
invalidation switches; annual NULL/zero and weighted-share zero/NULL; exact
endpoint/span and stub rejection; missing-year YoY/CAGR/domain; direct API
latest/first-reported and daily valid/invalid consumption; deterministic rerun
with different chunk sizes; annual candidate/input bounds; and required-PK
rollback preserving both failing and unrelated securities.

After source ownership is stable, root may run this single focused invocation
from `C:/atx/atx-db`, using the pinned interpreter and fresh guard receipt paths:

```powershell
$annualRun = 'annual-fallback-focused-' + [guid]::NewGuid().ToString('N')
& 'C:/atx/atx-db/.venv/Scripts/python.exe' 'C:/atx/.superpowers/sdd/tier1-parity/run_memory_guarded.py' --job-gb 2.5 --receipt "C:/atx/.superpowers/sdd/tier1-parity/$annualRun-memory.json" --stdout "C:/atx/.superpowers/sdd/tier1-parity/$annualRun-stdout.log" --stderr "C:/atx/.superpowers/sdd/tier1-parity/$annualRun-stderr.log" -- 'C:/atx/atx-db/.venv/Scripts/python.exe' -m pytest -n0 -q tests/test_derived_annual.py tests/test_derived_pit_revisions.py tests/test_derived_metrics.py tests/test_market_daily.py tests/test_panel_export.py
```

Touched-file Ruff and migration/schema-contract checks remain pending in the
same root-owned slot. No lint, parse/import check, test, migration bootstrap,
patch-apply check, benchmark or production coverage measurement was performed
by this implementer. A fresh independent Codex implementation review is still
required after root application/verification; rereview is needed only for a
Critical fix, per the AF1 brief.

No unresolved design conflict was found. The remaining gate is runtime/static
tool verification of the prepared SQL/Python and migration under the resource
guard, followed by the requested independent review and root integration.
