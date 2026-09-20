# Annual-filer metric-path check

**Result: concrete core completeness gap.**  An annual statement is admitted
and standardized, and a complete quarterly path can use an annual statement to
derive Q4 from a same-year nine-month value.  But an annual-only flow statement,
or an incomplete quarterly history without that exact Q4 stitch, cannot reach
the active P1 derived engine as an annual/TTM fallback.  Therefore it cannot
produce the catalog's flow-based TTM ratios, per-share measures, annual/TTM
growth, or their market descendants.  This is a static path finding only; it
makes no live coverage claim.

## Evidence and active path

1. The set-based standardizer deliberately accepts annual duration facts:
   `_standardization_set_based.py:145-203` classifies 330--380-day statement
   and XBRL durations as `annual`; it also accepts instantaneous facts.  The
   seed has annual, quarterly, and TTM rules for core items (for example,
   `seeds/standardization_rules.csv:20-25`, 41-43).  Thus valid annual flows
   and year-end balance-sheet instants are not excluded upstream.
2. The historical upstream rollup is **not** an annual fallback.
   `refresh_fundamental_ttm_points` first admits reported 70--115-day quarters
   (`fundamental_statements.py:1584-1614`), optionally derives a discrete Q4
   as FY less an earlier same-start YTD fact (`:1628-1683`), and publishes only
   windows with exactly four quarter-like inputs and 330--380 days
   (`:1686-1794`).  An annual-only flow has zero such quarters.
3. This supported partial-history case is intentionally tested:
   `tests/test_calendarization.py:608-649` obtains 2024 TTM 100 from Q1=10,
   6M=30, 9M=60, FY=100, with the annual value used only to make Q4=40.
   `:704-736` shows that Q1 + 6M + FY, without 9M, emits zero TTM rows.  That
   is a sound non-invention boundary, not a general FY-to-TTM fallback.
4. `fundamental_ttm_points` can itself be standardized at basis `ttm`
   (`_standardization_set_based.py:110-160`), but the active derived engine
   does not consume it.  `derived_metrics.py:120-124, 269-270` and
   `_derived_pit.py:51-64` load only `basis IN ('quarterly', 'instant')`.
   There is no alternate annual input relation or source-precedence branch.
5. On that quarterly grid, `ttm(x)` is hard-coded to `count(x)=4` and
   `sum(x)` over the four-row frame; `yoy(x)` is `lag(x,4)`
   (`derived_dsl.py:473-500`).  The P1 repair correctly preserves PIT
   event-state behavior, but it cannot manufacture a valid four-quarter or
   annual sequence from an annual standardized row.

This diverges from the Tier-1 design contract, which says every flow's `_ttm`
has a "fiscal-year fallback" (`docs/superpowers/specs/2026-09-19-tier1-parity-design.md:178-184`).
The catalog materially depends on that route: `revenue_ttm`, `eps_ttm`,
`sales_per_share`, `cfo_per_share`, `fcf_per_share`, `gross_margin`,
`net_margin`, `revenue_growth_yoy`, `pe_ttm`, `ps_ttm`, `pcf_ttm`, and
`fcf_yield` are declared in `seeds/derived_metric_definitions.csv:12,48-58,74,175-181`.

## What still works

An issuer that has only an annual filing can still supply a ratio made solely
from standardized year-end instant values, because `instant` is admitted to
the P1 input relation.  Examples include `current_ratio` and direct
balance-sheet leverage/liquidity expressions, subject to the required instant
items and their normal PIT availability.  This does not repair flow-based
metrics: an annual revenue or net-income value is basis `annual`, so it is
invisible to the current engine.

## Hand-worked annual-only example

Assume a calendar-year issuer files its FY2024 10-K on 2025-02-20, with no
usable 10-Q or YTD duration facts.  Its standardized annual flows are revenue
$1,000, gross profit $400, net income common $100, CFO $160, and capex $60.
At the same filing its standardized instants are common equity $300, current
assets $250, current liabilities $125, and period-end shares 50.  At the next
eligible market close, assume market cap is $1,000 (50 shares at $20).

Economically, the FY values are the appropriate TTM-equivalent flows at that
filing, without splitting them into invented quarters:

| Measure | Correct annual/TTM computation | Value |
| --- | --- | ---: |
| gross margin | 400 / 1,000 | 40% |
| net margin | 100 / 1,000 | 10% |
| sales per share | 1,000 / 50 | $20.00 |
| CFO per share | 160 / 50 | $3.20 |
| FCF per share | (160 - 60) / 50 | $2.00 |
| P/E | 1,000 / 100 | 10.0x |
| P/S | 1,000 / 1,000 | 1.0x |
| P/CF | 1,000 / 160 | 6.25x |
| FCF yield | (160 - 60) / 1,000 | 10% |
| current ratio | 250 / 125 | 2.0x |

Today only the last row has an input path.  The flow-derived rows remain
missing because `revenue_ttm` cannot satisfy the four-quarter frame; downstream
metrics consequently remain missing too.

If a comparable FY2023 annual revenue was $900 and it was available before the
FY2024 event, annual/TTM revenue YoY is `(1,000 - 900) / 900 = 11.11%` and is
derivable on an annual grid.  Quarterly QoQ and quarterly YoY are legitimately
unavailable: there are no observed fiscal-quarter values to compare.  Nor may
the implementation represent the FY value as four $250 quarters merely to
activate the current DSL.

## Small safe follow-on brief

Add a bounded **annual-flow fallback path** beside, rather than inside, the
existing quarterly PIT frame.

* Scope it to fiscal-year duration facts (330--380 days) and matching
  year-end instants.  First cover flow rollups, flow/instant ratios,
  per-share metrics, annual YoY, and market descendants.  Leave quarterly
  QoQ/YoY, quarterly volatility, and any other quarter-count semantics absent
  when their evidence is absent.
* At each annual filing event, prefer a valid four-discrete-quarter TTM,
  including the existing FY-minus-9M Q4 stitch, over a direct annual fallback.
  Use the direct standardized `annual` flow only when no such four-quarter
  TTM is visible.  Keep an explicit origin/precedence label so a consumer never
  sees two indistinguishable values for one fiscal endpoint.
* Evaluate annual YoY against the immediately preceding comparable fiscal-year
  endpoint, not `lag(..., 4)` on the quarterly grid.  Require both annual
  values to be positive where the existing metric requires it; retain normal
  denominator/domain rules.
* Preserve P1's PIT discipline: an annual-fallback value is first available at
  the maximum `available_at` of precisely the annual flows, year-end instants,
  prior annual values, and market inputs used.  Revisions create a later event
  state; never use load time, later 10-Qs, or a later annual filing to revise
  an earlier as-of result.
* Do not synthesize fractional quarters or relax the present four-quarter
  engine.  Implement the annual relation, its publication identity, and
  explicit consumer precedence as a focused follow-on, with fixtures for (a)
  annual-only FY, (b) valid Q4 stitch, (c) missing 9M no-stitch, (d) annual
  restatement, and (e) annual YoY availability.

No code, formulas, registry entries, database state, or tests were changed for
this check.
