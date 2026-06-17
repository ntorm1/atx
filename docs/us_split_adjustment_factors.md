# US Security Master And Corporate Actions

Build the full open-data security master with:

```powershell
python scripts/build_us_split_adjustments.py --common-only --overwrite
```

The default output root is:

```text
data/us_split_adjustment_factors/
  factors_by_symbol/
  factors_by_date/
  split_events_by_symbol/
  dividends_by_symbol/
  shares_outstanding_by_symbol/
  sectors_by_symbol/
  security_master/security_master.parquet
  _cache/
  manifest/
```

`factors_by_symbol/` is the canonical split-adjustment factor store. A new
split changes the entire prior factor history for one ticker, so symbol
partitioning lets that ticker be rewritten without touching every historical
date partition.

`factors_by_date/` is a derived mirror for daily panel reads. It preserves the
requested row shape:

```text
symbol,date,return_factor
```

The final joined file is:

```text
security_master/security_master.parquet
```

with columns:

```text
date
symbol
cumulative_adjustment_factor
cash_dividend
dividend_currency
shares_outstanding
shares_as_of_date
shares_filed_date
sec_cik
sec_sic
sec_sic_description
gics_sector_code
gics_sector
gics_sub_industry
gics_source
```

For the default `--mode split`, `cumulative_adjustment_factor` is the
cumulative future split factor for that symbol and date. Multiplying a raw
close by this value gives the close adjusted onto the latest share basis
available in the downloaded history.

Daily rebuild pattern:

```powershell
# Full refresh: replace all canonical datasets and rebuild final outputs.
python scripts/build_us_split_adjustments.py --common-only --overwrite

# Incremental refresh: replace only the named symbol partitions, then rebuild
# the derived date mirror and final master from all canonical symbol partitions.
python scripts/build_us_split_adjustments.py --symbols AAPL,MSFT --datasets yahoo sec security-master --incremental

# Self-healing full backfill: skips symbols already complete for the requested
# start/end window and builds only missing or schema-stale symbol partitions.
python scripts/build_us_split_adjustments.py --common-only --incremental --sec-user-agent "your-app your-email@example.com"

# Refresh Yahoo corporate actions/prices and rebuild the final master.
python scripts/build_us_split_adjustments.py --common-only --datasets yahoo security-master --overwrite

# Refresh SEC reference data only, then rebuild the final master from existing factors.
python scripts/build_us_split_adjustments.py --common-only --datasets sec security-master --overwrite

# Rebuild only the final joined file from existing canonical partitions.
python scripts/build_us_split_adjustments.py --datasets security-master --overwrite
```

Source coverage:

- Yahoo Finance chart history supplies daily closes, split events, and cash
  dividend events.
- Nasdaq Trader symbol directory files (`nasdaqlisted.txt`, `otherlisted.txt`)
  define the active listed US symbol universe.
- SEC EDGAR `company_tickers.json`, CompanyFacts, and submissions supply CIK,
  common shares outstanding facts, SIC, and SIC description.
- The open `datasets/s-and-p-500-companies` CSV supplies current S&P 500 GICS
  sector and sub-industry values where available.

Limitations:

- Broad full-universe GICS is licensed, not open. `gics_*` columns are populated
  only where an open source provides them; SEC SIC is the broad open fallback.
- The default universe is active listed symbols from Nasdaq Trader. It does not
  recover all delisted US equities.
- Shares outstanding are SEC fact values forward-filled by fact `end` date, with
  `shares_filed_date` included for provenance.
- `--mode split` is split-only. Use `--mode adjclose` when you want the factor
  that maps Yahoo raw close to Yahoo adjusted close, including dividends.
- `--incremental` is the self-healing mode. It reads the previous success
  manifests and checks physical `symbol=` partitions; symbols that already
  satisfy the requested backfill are skipped. Use `--force-symbols` to override.
- `--yahoo-min-interval`, `--sec-min-interval`, `--request-delay`,
  `--sec-request-delay`, and `--backoff-max-seconds` control polite crawling.
  SEC EDGAR should stay below the published automated-access ceiling; set
  `--sec-user-agent` to a real app/contact string for production runs.
