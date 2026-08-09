# Sprint PF-S6 — Modern Pricing Overlap + Valuation Multiples

**Goal:** ingest 2015+ daily bars to intersect the fundamentals window; compute market cap +
valuation multiples PIT-safely joining price × shares × fundamentals via the identifier spine.
Reserved migrations 0084–0087.

**Mandate / Owns:** NEW `db/pricing_bulk.py` (injectable 2015+ bars), `db/ticker_history.py`
extension, NEW `db/valuation_multiples.py`, `db/tests/test_valuation_multiples.py`.

**Must NOT touch:** the `formula_registry` internals (PF-S4 owns; consume it), identifier loaders
(PF-S5 owns `identifiers_figi.py` / `identifiers_lei.py` / `security_master.py` resolution). Do not
edit a landed migration or another sprint's `fundamental_ratios.py` region (PF-S4 owns the formula
consumption region; PF-S8 owns the lineage-column region — this sprint adds valuation rows through a
new module, never by re-editing the ratio engine's core).

**Depends on:** PF-S4 (formula registry — valuation families live here as declarative rows) and
PF-S5 (identifier join — the stable `security_id` that lets a price line meet a fundamental line).

---

## Baseline / where the cycles go

The single most-cited fundamentals-parity gap is that the warehouse emits **zero** price-based
valuation multiples. The cause is not missing math — it is a window mismatch that makes the join
empty:

| Surface | Window (measured) | Consequence |
|---|---|---|
| `equity_daily_bars` (source `tbltickerhistory_10y`) | **2012–2014** sample | the only daily price we hold predates every fundamental |
| `sec_company_facts` → `fundamental_statement_points` / `fundamental_ttm_points` | **2017–2026** | every EPS / equity / sales fact is *after* the last bar |
| overlap | **∅ (none)** | any `price × fundamental` join on a shared date returns 0 rows |

Because the two surfaces do not overlap on any `(security_id, date)`, `market_cap = close × shares`
has no close to pair with a share count on a fundamental period, and every P/E, P/B, P/S,
EV/EBITDA row would compute to `NULL` / be filtered — hence the current ~0-row reality. There is no
`market_cap` table at all. `shares_outstanding_history` **does** hold PIT XBRL share counts
(`security_id`, `effective_date`/`period_end`, `as_of_date`, `available_at`, `share_count`,
`share_count_type ∈ {shares_outstanding, shares_basic_avg, shares_diluted_avg}`,
`is_latest_revision`) over the 2017–2026 fundamentals window, so the *share* leg of market cap is
already available and PIT-stamped — only the *price* leg is missing over that window. This sprint
adds the missing price leg and the multiples that then become computable.

**Already good — do not regress:**

- **S39 recycled-ticker / share-class collision repair.** `disambiguate_vendor_collisions`
  (`ticker_history.py`) splits a `security_id` that collapses multiple distinct tradeable lines
  (recycled ticker across issuers; LMCA/LMCAV-style share classes) onto per-line synthetic ids,
  materializing first-class `securities` / `security_identifier_history` / `exchange_listings`
  rows, and collapses residual exact `(security_id, trade_date)` duplicates. Global and idempotent.
  The new 2015+ loader MUST route through the same repair, not reinvent it.
- **Split/dividend back-adjusted close.** `equity_daily_bars.adjusted_close` and
  `split_factor` are the corp-action-consistent series; the multiples pick the correct price column
  deliberately (see S6-1) and never silently mix raw and adjusted.
- **PIT `shares_outstanding_history`.** Its `available_at` / `as_of_date` / `is_latest_revision`
  discipline is correct and is the share leg's system of record — consume it as-is.

---

## PIT / determinism contract

This sprint invokes ROADMAP clauses **(A) bitemporal correctness**, **(B) append-only catalogued
migrations**, and **(C) offline / no-network tests**, plus **(D) determinism + provenance**.

The 2015+ bar loader reads an **operator-supplied offline archive** (a Nasdaq HistoricalData ZIP or
a bulk daily-bar CSV the operator has already downloaded) behind an injectable file option
(`--bulk-bars-file` / `--bulk-bars-zip`, mirroring `TickerHistoryOptions.zip_path`). There is **no
network call in the test path** — every test runs against in-memory DuckDB with a small fixture
price panel and a small fixture fundamental panel.

**The PIT rule for a multiple (clause A, made concrete):**

```
multiple.available_at = max(price.available_at, fundamental.available_at)
```

and, symmetrically, an as-of query at `as_of_ts` may combine a price and a fundamental **only when
both `available_at ≤ as_of_ts`**. Never value a fundamental against a price stamped after the
fundamental's own `available_at` window in a way that leaks future price into a historical
knowledge cut — the multiple is knowable only once *both* legs were knowable. `as_of_date` of the
multiple is the price `trade_date` (the observation date the market cap is struck on); the
fundamental supplies the denominator vintage, carried via `input_codes_json`. `equity_daily_bars`
already stamps `available_at = trade_date + 22h` (end-of-session knowledge time), and
`shares_outstanding_history` / the ratio inputs carry their own `available_at` — the loader and the
multiples module must not overwrite either.

---

## Tasks

### S6-0 — Injectable 2015+ bar loader (`pricing_bulk.py`)

**Root cause:** `TickerHistoryDataset` (`ticker_history.py`) is bound to one archive shape
(`tbltickerhistory3_10y.zip`, tab-separated, ~70 vendor columns) whose data is a 2012–2014 sample.
It cannot ingest a differently-shaped modern bar archive, and even if pointed at one, its column
map is the vendor's, not a generic OHLCV shape. So there is no path to land bars over 2015–2026.

**Fix:** NEW `db/pricing_bulk.py` with a `BulkBarsDataset` / `BulkBarsOptions` pair that reads an
operator-supplied offline archive (ZIP-of-CSV or bare CSV) of daily OHLCV into the **existing**
`equity_daily_bars` table over the fundamentals window (default filter `trade_date ≥ 2015-01-01`).
Normalize the generic archive columns (`symbol, date, open, high, low, close, adjusted_close,
volume`) into the canonical bar frame, resolve `security_id` through `security_ids_for_symbols`
(same resolver `ticker_history.py` uses), stamp `available_at = trade_date + 22h` and a distinct
`source` (e.g. `bulk_bars_2015plus`) so the modern bars are lineage-separable from the legacy
sample. After all chunks land, **reuse** `disambiguate_vendor_collisions(store, source)` — do not
copy its body — so recycled-ticker / share-class collisions in the modern universe are repaired
identically.

**PIT:** bars carry `available_at = trade_date + 22h`; the loader never fabricates a share count or
a fundamental — it only lands price. The distinct `source` value keeps the overlap analysis honest
(we can count modern-source rows separately).

**Migration:** the loader targets the existing `equity_daily_bars` schema, so **no schema change is
required** — reserve `0084` and document "intentionally unused (no schema delta)" in the migration
catalog note, OR, if a `bulk_bar_source_file` provenance table is added, land it as `0084` with
`table_catalog` + `field_catalog` seeded in the same migration (clause B).

**Accept:**
- (a) A fixture ZIP/CSV of ~2016–2020 bars for two symbols lands N>0 rows into `equity_daily_bars`
  with `source = bulk_bars_2015plus` and `trade_date ≥ 2015-01-01`.
- (b) `security_id` resolves through the shared resolver; a fixture recycled-ticker collision is
  split by the reused `disambiguate_vendor_collisions` (no duplicate `(security_id, trade_date)`).
- (c) Re-running the loader on the same fixture is idempotent (upsert on `(source, security_id,
  trade_date)`; row count stable).

---

### S6-1 — Market cap: daily close × PIT shares

**Root cause:** no `market_cap` exists because there was no price over the fundamentals window and
no join wiring between `equity_daily_bars` and `shares_outstanding_history`.

**Fix:** in `valuation_multiples.py`, for each `(security_id, trade_date)` over the overlap window,
join the daily price to the **PIT-correct** share count: pick the `shares_outstanding_history` row
for that `security_id` with `share_count_type = 'shares_outstanding'` (fall back to
`shares_diluted_avg` only when the instant count is absent, recording which was used), whose
`available_at ≤ price.available_at` and whose `effective_date` is the latest on or before
`trade_date` among `is_latest_revision = TRUE` rows. Then:

```
market_cap = close × share_count
market_cap.available_at = max(price.available_at, shares.available_at)
```

Use the price column deliberately: market cap is a *level as struck on the day*, so use raw `close`
(the actual traded price and the actual then-outstanding share count), not `adjusted_close`; note
the choice in the module docstring. Materialize into a new `market_cap` table (migration `0085`,
catalogued) keyed `(source, security_id, trade_date)` with `close`, `share_count`,
`share_count_type`, `market_cap`, `as_of_date = trade_date`, `available_at`, `run_id`.

**PIT:** the share count chosen is the latest revision **knowable at the price's `available_at`** —
never a share count filed after the price date. `market_cap.available_at` is the max of the two
legs (clause A).

**Accept:**
- (a) On a fixture where a security has a known close and a known PIT share count, `market_cap`
  equals `close × share_count` to floating tolerance.
- (b) A share revision filed *after* the price date is NOT selected; the earlier knowable revision
  is used (PIT test).
- (c) `market_cap.available_at == max(price.available_at, shares.available_at)`.

---

### S6-2 — Valuation multiples as `formula_registry` families

**Root cause:** the derived layer (`fundamental_ratios.py` `RATIO_DEFS`) has statement ratios but
**no price-based family at all** — there was no market cap and no price to feed them. PF-S4 turns
formulas into declarative `formula_registry` rows; this sprint *consumes* that registry to add the
valuation family, rather than hard-coding new lambdas.

**Fix:** register (as PF-S4 `formula_registry` rows, `kind = ratio`, with a `price` / `market_cap`
input tag) and compute in `valuation_multiples.py` the standard valuation family, each joining
`market_cap` (S6-1) or `close` to a fundamental denominator from the existing PIT inputs
(`fundamental_ttm_points` for TTM flows, `shares_outstanding_history` / statement points for
balances, `fundamental_xbrl_metric` for debt/cash):

| Code | Definition | Denominator source |
|---|---|---|
| `price_to_earnings` | `close / EPS_TTM` (EPS = net_income_TTM / diluted shares) | TTM net income + diluted shares |
| `price_to_book` | `close / BVPS` (BVPS = stockholders_equity / shares) | statement equity + shares |
| `price_to_sales` | `market_cap / revenue_TTM` | TTM revenue |
| `enterprise_value` | `market_cap + total_debt − cash_and_equivalents` (level, currency) | XBRL long-term debt + cash |
| `ev_to_ebitda` | `enterprise_value / EBITDA_TTM` | derived EBITDA (oi + D&A) |
| `ev_to_sales` | `enterprise_value / revenue_TTM` | TTM revenue |
| `fcf_yield` | `free_cash_flow_TTM / market_cap` | TTM (ocf + capex) |
| `earnings_yield` | `net_income_TTM / market_cap` | TTM net income |
| `dividend_yield` | `abs(dividends_paid_TTM) / market_cap` | TTM dividends |

Enterprise value uses **item-dimension inputs** (long-term debt and cash from
`fundamental_xbrl_metric`, resolved through PF-S1's item registry — consume it, don't re-key on raw
strings). Every emitted row records numerator/denominator codes + values,
`is_meaningful`, `input_codes_json`, and the PF-S4 formula id, and enforces the PIT rule:

```
multiple.available_at = max(price.available_at, fundamental.available_at)
```

Emit multiples into `fundamental_ratios` (same long schema, new `ratio_category = 'valuation'`) OR
a sibling `valuation_multiples` table — pick the sibling table (migration `0086`, catalogued) to
avoid touching the `fundamental_ratios.py` core owned by PF-S4/PF-S8; it mirrors the ratio columns
plus `price`, `market_cap`, `enterprise_value`.

**PIT:** each multiple's `available_at` is the max over its price leg AND every fundamental input it
consumes (the fundamental leg already carries its own max-of-inputs `available_at` from the ratio
engine). `as_of_date` is the price `trade_date`.

**Accept:**
- (a) On a fixture panel (2 securities, a handful of overlapping dates, known EPS/BVPS/sales/debt/
  cash), each family member computes to the expected value.
- (b) EV = `market_cap + debt − cash` on the fixture; `ev_to_ebitda` and `ev_to_sales` derive from
  it.
- (c) `is_meaningful = FALSE` when the denominator is non-positive (negative EPS → P/E flagged, not
  dropped silently); the row is still emitted with the flag (matching `compute_ratio_rows`
  semantics).
- (d) `available_at == max(price_av, fundamental_av)` for every emitted multiple.

---

### S6-3 — Coverage / quality over the overlap window

**Root cause:** with no overlap today, any coverage report would be vacuous; once S6-0 lands bars,
overlap is *thin at the edges* (2015–2016 fundamentals are sparse; some securities never overlap)
and we must not overstate it.

**Fix:** compute multiples **only** over the true `price × fundamental` overlap window; add a
`quality_check` that reports overlap coverage honestly — `(securities with ≥1 valuation multiple) /
(securities with ≥1 fundamental over the window)` and the overlapping date span. Set
`is_meaningful = FALSE` on non-positive denominators (EPS ≤ 0, equity ≤ 0, EBITDA ≤ 0,
market_cap ≤ 0), never emitting a garbage ratio as if usable. Add a **stale-price-vs-fundamental**
check: flag (do not drop) a multiple where the price `trade_date` is more than a configurable gap
(e.g. > 5 trading days, or a fundamental with no price within the window) so a fundamental valued
against a far-away price is visible rather than silently trusted.

**PIT:** the coverage query itself respects `available_at` (it counts only rows that would be
visible), so the reported coverage is the *knowable* coverage, not a look-ahead-inflated one.

**Accept:**
- (a) Coverage `quality_check` row written with the overlap security count, fundamental security
  count, ratio, and date span; state exact counts in the ledger.
- (b) A fixture with a negative-EPS security emits `price_to_earnings` with `is_meaningful = FALSE`,
  not a dropped/garbage row.
- (c) The stale-price flag fires on a fixture fundamental whose nearest price is beyond the gap
  threshold.

---

## Sequencing & expected compounding

**S6-0 (load bars) first — everything else is blocked until overlap exists.** With no 2015+ bars
there is no price leg, so market cap, every multiple, and coverage are all empty. Then **S6-1**
(market cap = close × PIT shares) unlocks the level every multiple divides into or by; then **S6-2**
(the nine-member valuation family) is the visible parity surface; then **S6-3** (coverage/quality)
makes the honest claim. Each step strictly enables the next: bars → market cap → multiples →
measured coverage. Completing the chain closes the single most-cited fundamentals-parity gap versus
Compustat/FactSet — price-based valuation multiples — from ~0 rows to a real, PIT-safe surface over
the overlap.

---

## Risks / guardrails

- **Look-ahead by stale-price valuation (primary risk).** Valuing a fundamental against a price
  stamped before that price's `available_at`, or selecting a share revision filed after the price
  date, leaks future information. **Mitigate:** the strict `available_at = max(...)` rule enforced
  in both S6-1 and S6-2, plus a dedicated PIT test that a later-filed share revision / a
  future-dated price is never selected. This is the non-negotiable analogue of the warehouse's
  bitemporal system of record.
- **Thin overlap → few rows.** The 2015–2016 fundamental edge is sparse and some securities never
  overlap. **Mitigate:** S6-3 reports coverage honestly (numerator/denominator + date span); the
  ledger states real counts. Never claim full coverage; a small honest number is correct.
- **Loader offline / injectable only.** No SEC / Nasdaq / vendor network call in the test path
  (clause C). The archive is operator-supplied behind a file/zip option; the live fetch is an
  operator-run smoke recorded in the ledger, never in pytest.
- **Raw vs adjusted price mix-up.** Market cap uses raw `close` × then-outstanding shares
  deliberately; any long-horizon return/series work uses `adjusted_close`. Do not mix the two in a
  single multiple; document the chosen column.
- **Do not touch owned neighbours.** `formula_registry` internals (PF-S4), identifier loaders and
  `security_master` resolution (PF-S5), and the `fundamental_ratios.py` core regions owned by
  PF-S4/PF-S8. This sprint adds through NEW modules and a NEW sibling table, not by re-editing them.

---

## Bench / acceptance

- Valuation multiples emitted over the overlap window with correct PIT `available_at`
  (`= max(price_av, fundamental_av)`).
- Market cap correct on a fixture (`close × PIT share_count`), with the PIT share-revision selection
  test green.
- The full nine-member valuation family (`price_to_earnings`, `price_to_book`, `price_to_sales`,
  `enterprise_value`, `ev_to_ebitda`, `ev_to_sales`, `fcf_yield`, `earnings_yield`,
  `dividend_yield`) computes to expected values on the fixture panel; `is_meaningful = FALSE` on
  non-positive denominators.
- State row counts over the overlap window (bars landed, market_cap rows, multiple rows per family)
  in the ledger, with the honest coverage ratio and date span from S6-3.
- `python -m pytest atx-impl\db\tests\test_valuation_multiples.py -q` green **offline** (small
  price + fundamental fixture panel; in-memory DuckDB; no network).
- `python -m pytest atx-impl\db\tests -q` green before commit.

**Process:** never `git add -A` (stage explicit paths — the tree carries many unrelated dirty /
untracked files); never push unless asked. Update `PARITY_GAP.md` status and append a row to
`WAREHOUSE_PARITY_TRANCHES.md` (start/end SHA, domains, verification commands, live-DB smoke with
exact counts + `run_id`, caveats/next). Commit trailer EXACTLY
`Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>`.

## Out of scope

Intraday / minute bars; options-implied or forward multiples; PEG and analyst-estimate-based
multiples (estimates schema is parked); the `fundamental_ratios.py` core refactor (PF-S4) and the
restatement-lineage columns (PF-S8); non-US listings and non-USD market cap. This sprint closes the
price × fundamental overlap and the standard trailing valuation family — nothing more.
