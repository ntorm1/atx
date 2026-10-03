# Owner coverage steering — 2026-10-03

Binding addition to Tasks 5/7/11/16/17: complete 13F coverage, including retained bronze sources, and high-quality gold features with measured cross-sectional predictive evidence. Complete profitability/growth primitives and gold-panel families. This extends the existing goal; PIT, holdout, memory and foreign-file constraints remain binding.

## Task 11: 13F source completeness and features

The old brief covers filer typing, not complete 13F source certification. Audit every existing bronze/cache location before acquisition. The old inventory's tiny raw/sec_13f directory is a clue, not proof of missing holdings elsewhere.

1. Establish authoritative expected accession inventories by filing quarter and holdings report quarter from public SEC indexes/submissions/bulk manifests, from 2020 onward plus change-feature warmup. Separate completed cohorts from the current incomplete cohort. Classify HR, amendments, notices/combination reports, confidential/withheld positions and legitimate no-holdings cases.
2. Reconcile expected accessions -> retained as-served bronze members -> parsed filings/holdings -> dated issuer/security links -> PIT aggregates -> gold cells. Publish exact counts, missing accession keys, bytes/hashes/URLs/retrieval clocks, parse failures, amendment chains, mapping misses and value/share reconciliation by quarter/year. Receipts alone are not retained bronze. Restore missing public source members with resumable rate-limited acquisition and parser provenance.
3. Gate: every expected publicly available filing/source member in completed cohorts accounted for; zero unexplained missing bronze or silently dropped parsed accessions. Holdings/cover totals reconcile or remain explicit keyed FAIL exceptions. Distinguish unavailable confidentiality, not-yet-due cohorts and real gaps. Do not equate high feature coverage with complete bronze. Verify source unit regimes and SH/PRN/put/call classes; shared/included managers and amendments must not double count.
4. Availability is public acceptance, never quarter end or an assumed 45-day lag where acceptance exists. Amendments create vintages. Missing clocks use documented conservative floors. Mapping has dated CUSIP/share-class/issuer links, quality tiers and unmatched-value counts.
5. Use clean standalone modules (thirteenf_quality.py/thirteenf_features.py, plus a bounded source module if necessary), tests/docs/stubs. Foreign thirteenf_archive.py and scripts/download_13f.py stay read-only; do not run their modified ingestion chain.
6. Feature families: ownership share/change with consistent denominator, holder breadth/change, new/closed positions, split-adjusted held-share changes/net buying, concentration/crowding, active/passive and filer-type breakdown, persistence, and supported flow-pressure proxies. Price-driven value changes are not economic flows; unknown manager types stay unknown. No managers selected using future returns.
7. Measure level/change coverage by year on member_equity, univ_all_common and univ_r3000p. Separate known zero from missing source/linkage. Source hand checks span amendments, shared managers, unit regimes, stock splits and delisted lines. Publish all failures and limits.

## Tasks 5/7/16: profitability and growth requirements

Create a machine-readable primitive -> formula -> gold-column requirements matrix. Cover sales/revenue, COGS/gross profit, operating income/EBIT/EBITDA, NI/IB, operating/free cash flow, assets/equity/invested capital, working capital/inventory/receivables, debt/interest/tax, capex/R&D, dividends/equity issuance/buybacks, basic/diluted shares and EPS. Include sector-specific structural applicability for banks/insurers/REITs.

Gold families: gross/operating/net/cash margins; GP/assets, ROA/ROE/ROIC/ROCE; cash conversion/accrual quality; quarterly YoY and annual/TTM sales/earnings/cash-flow/asset/equity/capex/R&D growth; margin/profitability changes; investment/working-capital growth; per-share growth/dilution. Define negative/zero bases, denominator averages, seasonality, fiscal-calendar changes, currency and restatement vintages. Missing prior periods must not become zero.

Every metric needs formula, source, clock, staleness, history sufficiency, structural-null reasons and yearly coverage in all three universes. Independently verify source ratios, source-backed balances and quarter/annual/TTM consistency. No balancing plugs or unsupported zero-fill. Existing stage gates remain binding; missing primitives/unsupported sector metrics are explicit Task 5 gaps, with gold quality flags/nulls rather than invented values.

## Tasks 16/17: gold delivery and evidence

Join ownership and profitability/growth through actual availability; stratify the PIT audit across filing/amendment events and fundamental restatements. Keep labels separate. Catalog bronze-to-gold lineage and exceptions, with a source-to-gold completeness table.

Before the first predictive run, extend the executable predeclared configuration with ownership breadth change (+), split-adjusted institutional ownership change (+), operating profitability (+), and annual asset growth (-). Resolve registry names and deduplicate economically identical primary candidates. All 5/21/63-session IC/quantile-return evidence stays wholly pre-2023, overlap-aware and multiplicity-adjusted. Report every outcome/unsupported candidate; completeness alone does not establish significance.

All real-data audits/fetches/builds <=1GiB guarded; three lanes maximum. Existing SEC clients/rate budgets apply. Check C: free >=40GB before >1GB landing. Task 17 cannot call the warehouse complete while public-filing/bronze or required primitive/feature gaps remain unexplained.
