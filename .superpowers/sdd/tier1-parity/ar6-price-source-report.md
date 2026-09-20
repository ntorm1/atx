# AR6 price-source correction

Implemented in the two ticker-history adapters, a focused offline source-quality
helper/script, covering tests, and authorized price documentation. No live
warehouse was opened, no source bytes were changed, and no jobs, activation,
market_daily formulas, compatibility formulas, registry or migrations were edited.

## Semantics and rejection policy

- Current adjusted close is same-row raw `close * cumulReturnFactor`, requiring
  finite positive operands and product. Missing/invalid factors yield NULL; no
  prior-close or raw-close fallback. Raw OHLC, shares and volume are not adjusted.
- Canonical `split_factor` is NULL. Vendor `returnFactor` includes distributions;
  it remains in the full raw-loader table and original archive/TSV. Bulk staging
  retains the diagnostic projection; the original source file is the durable raw
  lineage, not the filtered canonical table.
- Canonical symbol prefers historical `ticker_tk`. Positive vendor IDs are grouped
  across changing display symbols. Existing current-symbol/CIK resolution remains
  explicitly unverified; this change does not establish historical issuer identity.
- All repeated positive vendor-ID/date keys are quarantined, including any exact
  repeats. This conservative policy does not choose volume/order winners or infer
  share classes from contradictory rows. The plain loader retains complete source
  date groups across chunk boundaries; unsorted sources fail explicitly. Partial
  archive/resume options still describe only their selected source extent.
- Bulk QA measures original rows before exclusions. It requires unique original
  positive-ID keys, increasing dates and consecutive original `dn` values for
  residual comparisons. Duplicates and missing ordinals block adjacency rather
  than letting a previous accepted row masquerade as the prior original session.
- `available_at = tradingDate + 22h` remains a modeled backfill convention. It is
  not vendor delivery evidence. Source metadata and operator output explicitly
  mark original historical vintage and economic adjustments as unknown/unverified.
  New bulk current-identity metadata is available at load time; plain-loader
  identity metadata has unknown availability rather than an invented old timestamp.
  Historical validity of current-symbol listings/CIK links remains a separate
  prerequisite; neither path certifies those links for historical replay.

The authoritative field definitions and current 05:00 America/Chicago T+1
delivery schedule are in the
[SpiderRock dictionary](https://docs.spiderrockconnect.com/docs/next/HistoricalData/Data%20Dictionaries/TickerHistory3/).
The page does not authenticate any historical vintage of this archive.

## Newly measured original-source evidence

Receipt: `ar6-source-audit.json` alongside this report. The audit read the exact
original extracted TSV, not the other session's accepted subset or its reports.

| Measurement | Result |
| --- | ---: |
| Source bytes | 11,084,562,320 |
| Full SHA256 before scan | `96cb7fbde52e03c7f6559bc1ccdf2dd97e8cec225d93128200afeb8d60ab0629` |
| Extraction sidecar comparison | Exact match |
| Original rows | 31,598,499 |
| Original date extent | 2012-03-26 through 2026-06-15 |
| Repeated positive vendor-ID/date keys | 489 |
| Rows belonging to those keys | 978 |
| Zero-ID rows | 351,651 |
| Missing/invalid or negative IDs under measured numeric projection | 0 |
| Missing date/display-symbol keys | 892 |
| Structurally invalid OHLCV rows | 419,228 |
| Invalid current close/cumulative factor/adjusted product rows | 0 / 0 / 0 |
| Comparable adjacent unique original pairs | 30,679,047 |
| Prior-close relative disagreement greater than 1e-6 | 0 |
| Cumulative recurrence disagreement greater than 1e-6 | 0 |
| Daily-factor disagreement greater than 1e-3 | 79,416 |
| Adjusted-price-return vs reported-return disagreement greater than 1e-3 | 79,365 |
| Audit duration (hash + projection + diagnostics) | 299.084 seconds |

The scan used one DuckDB thread and a 1 GB memory cap, with local temporary spill.
File size and modification time matched before and after. SHA256 was measured
before the scan, not again afterward; an extracted TSV has no ZIP CRC check.
The earlier 31,464,423-row ledger figure is superseded by this direct measurement
of the hash-bound source. Overlapping QA counts are not additive. The receipt
contains separate comparable/not-comparable counts and residual bins at 1e-8,
1e-6, 1e-4 and 1e-3. These are diagnostic bins, not economic acceptance thresholds.
No source row was economically repaired or certified. The final parser also
rejects nonintegral ID/dn strings rather than allowing an integer cast to round
them; that defensive parsing tightening postdated the measured audit.

Full current-symbol/CIK matched/unmatched row counts require the eventual
publication against its actual identity metadata. The bulk publisher records
those two explicitly unverified counts in its result and final quality details;
this offline source audit did not open the live database to manufacture them.

## Validation

- One focused run of `tests/test_ticker_history.py`,
  `tests/test_ticker_history_bulk.py`, and
  `tests/test_ticker_history_price_source.py`, `-n 0 -q`: 25 passed, one floating
  parsing assertion failed. Only that seven-for-one fixture was rerun after
  replacing bit equality with numerical comparison; it passed (26 covered cases).
- Independent expected adjusted returns: ordinary 100→110 = +10%; seven-for-one
  700→110 with 1/7→1 cumulative factor = +10%; two-for-one 100→55 with .5→1
  factor = +10%; cash distribution 100→99 with .98→1 factor = +1/98.
- Invalid/missing/zero/negative/nonfinite/overflowing factors, preserved raw
  factors/volume/shares, historical rename, both-adapter numerical parity,
  cross-chunk duplicate quarantine, original-dn gaps and duplicate predecessors
  are covered. Invalid factors never produce a fallback adjusted close.
- Ruff passed on all touched Python files. Strict mypy passed for the new helper
  and standalone script. `git diff --check` passed.
- Regenerated `docs/DATA_DICTIONARY.md` under the controller's explicit expanded
  ownership. Generator `--check` and the three deferred dictionary tests passed:
  `test_the_committed_dictionary_is_current`,
  `test_check_mode_passes_on_a_current_file`,
  `test_the_output_uses_lf_newlines_only`.
- No whole suite, boundary suite, production publication, or production derived
  rebuild was run. `ticker_history_quality` snapshot entry landed with S3's
  authorized snapshot-line bleed in `72d38a93`.

## Exact offline operator handoff

After run4 releases the writer, controller-governed migration/checkpoint work
must precede publication. From `C:/atx/atx-db`, use the same extracted file:

```powershell
.venv/Scripts/python.exe scripts/publish_broad_daily_bars.py --db-path C:/atx/atx-db/data/warehouse.duckdb --tsv-path C:/atx/atx-db/data/staging/broad-bars/tbltickerhistory3_10y.txt --memory-limit 6GB --threads 8 --run-id activation-ar6-price-republish-v2
```

Source remains `tbltickerhistory3_10y`; publication now records the source file
hash, preprojection diagnostics, separate canonical gate results and provenance.
Retain the original TSV, ZIP and hash sidecar. Do not infer raw-source cleanliness
from zero postpublication duplicate keys. The command replaces this source's
canonical price surface transactionally, preserving other sources. Existing
market_daily, derived/price-metric and return-dependent artifacts must be rebuilt
under the controller's run5 sequencing; the dependent issues below must be
resolved or explicitly excluded before certifying their outputs.

For a fresh read-only audit, choose a new report filename (the tool refuses to
overwrite reports):

```powershell
.venv/Scripts/python.exe scripts/audit_ticker_history_source.py --tsv-path C:/atx/atx-db/data/staging/broad-bars/tbltickerhistory3_10y.txt --output C:/atx/.superpowers/sdd/tier1-parity/ar6-source-audit-recheck.json --memory-limit 1GB
```

## Concrete downstream dependency handoff (not edited)

These references are to the post-S3-retirement tree; deleted `net_issuance.py`
and `net_payout.py` must not be restored. NULL split factors are not evidence that
no splits occurred. Many existing consumers currently substitute neutral 1.

| Surviving reader | Required follow-on |
| --- | --- |
| `equity_price_metrics.py:92` `_back_adjusted_close`, `:105` `_derive_one_security`, `:261` price input SQL | Return/momentum/volatility should consume corrected canonical adjusted_close directly. For adjusted open/gap returns, use the same-row adjusted_close/raw_close scale where valid. Keep raw price × raw volume for dollar volume; never compound the factor twice. |
| `signal_eval.py:1947` `_derive_forward_returns_from_prices`, reconstruction at `:2012` | Forward returns should use corrected adjusted-close endpoint ratios with invalid observations excluded explicitly; update the obsolete split-factor reconstruction contract. |
| `filing_reaction.py:51` `load_filing_reaction_inputs`, factor at `:74` | Daily reaction return should use corrected adjusted closes, not raw close divided by a dividend-inclusive field mislabeled split. |
| `fundamental_momentum.py:74` `load_fundamental_momentum_inputs`, factor at `:131` | Its 12-1 price-return control should use corrected adjusted endpoint prices; avoid applying cumulative split factors again. |
| `twin_momentum.py:74` `load_twin_momentum_inputs`, factor at `:131` | Same return-input correction as fundamental momentum. |
| `derived_compatibility.py:175` `_PRICE_RELATION`, `:192` `shares` | Split-adjusted shares/issuance require independent actual split-only evidence. Corrected total-return prices cannot substitute for share adjustment. This is the surviving replacement for retired net issuance/payout logic. |
| `earnings_surprise.py:54`, `revenue_surprise.py:66`, `earnings_acceleration.py:59` input loaders; split reads at `:152`, `:169`, `:106` respectively | Per-share histories require actual split evidence, not a magnitude heuristic on distribution-inclusive factors or adjusted-price ratios. Unknown split history must remain explicit. |
| `earnings_seasonality.py:68` `load_earnings_seasonality_inputs`, factor `:121`, EPS adjustment `:205` | Its EPS normalization likewise requires actual split evidence. |
| `corporate_actions.py:57` `_build_actions`, inference `:94`; `adjustment_factors.py:163` `refresh_adjustment_factor_history` | The legacy inferred cash-distribution path can misclassify splits and has further ratio heuristics. Raw vendor factors are preserved for evidence, not permission to manufacture economic events or add distributions again to total-return-adjusted prices. |

Core `market_daily.py:249` already consumes canonical adjusted_close and needs a
rebuild, not a formula edit. These dependencies, historical identity, source
availability/vintage, historical shares revisions, and economic source
exceptions remain distinct unresolved prerequisites.

Independent review: requested from controller after the implementation commit;
not yet completed at report creation.
