# Cross-Vendor Field Map — canonical concept → vendor field

**Status:** Research, v0.1 (synthesis layer)
**Audience:** ats-eqt analysts, engineering, product. Answer to "I know the Compustat / us-gaap field for X, what is the equivalent in FactSet, Bloomberg, Worldscope, CIQ, Sharadar, SimFin, LSEG?"
**Scope:** A single lookup-table aggregation across wave-1 (`vendors/factset.md`, `vendors/sp_global.md`, `vendors/refinitiv_bloomberg.md`, `vendors/supply_chain_specialists.md`, `schemas/data_models_and_methodology.md`, `sources/public_data_sources.md`, `datasets/13f_holdings.md`, `datasets/edgar_loader.md`) and wave-2 (`datasets/fundamentals_us_equities.md`, `datasets/estimates.md`, `datasets/corporate_actions.md`, `datasets/pricing_market_data.md`, `datasets/esg_sustainability.md`) docs in this research database. **No new field names are invented here** — every cell is sourced from the underlying docs, and `[unverified]` provenance carries through.
**Last updated:** 2026-05-14

---

## 0. Executive summary

This document is a **synthesis** of the wave-1 + wave-2 ats-eqt vendor and dataset research. It collapses ~14,000 lines of source material into a single per-row lookup so an analyst, ingest engineer, or quant researcher can move between vendor vocabularies without re-reading every dataset doc.

The canonical schema that drives the rows is in
[`schemas/data_models_and_methodology.md`](data_models_and_methodology.md) **Part G**
("Recommended ats-eqt internal schema"). Section G.1 sets the entity / security
model; G.2 the long-format `fund_fact` table; G.3 estimates; G.5 corporate
actions. Item-level field detail for fundamentals (the largest section here) is
sourced almost entirely from
[`datasets/fundamentals_us_equities.md`](../datasets/fundamentals_us_equities.md)
sections 2-9, where 147 us-gaap concepts are catalogued and a ~50-row authoritative
cross-vendor table appears as Section 9. Estimates rows are sourced from
[`datasets/estimates.md`](../datasets/estimates.md) §2-§6; corporate actions
from [`datasets/corporate_actions.md`](../datasets/corporate_actions.md) §3-§11;
pricing from [`datasets/pricing_market_data.md`](../datasets/pricing_market_data.md)
§3; ESG from [`datasets/esg_sustainability.md`](../datasets/esg_sustainability.md)
§3-§12; identifiers from [`datasets/13f_holdings.md`](../datasets/13f_holdings.md)
Part A + [`sources/public_data_sources.md`](../sources/public_data_sources.md).

Five non-obvious findings that emerged in the synthesis pass:

1. **No single vendor covers all eight datasets.** FactSet, S&P, Bloomberg, LSEG each have ≥1 dataset where another vendor is materially better. CRSP has no fundamentals; Compustat has no estimates detail; FactSet has no academic IBES history; Bloomberg has no FFO/AFFO REIT taxonomy in core fields.
2. **Sign-convention is the silent integration bug across all categories.** Bloomberg's `BEST_ANALYST_RATING` is the *inverse* of IBES `ireccd` (Bloomberg 5 = Strong Buy; IBES 1 = Strong Buy). Sustainalytics ESG Risk Rating is the *inverse* of MSCI ESG Rating (lower is better vs higher is better). Compustat dividend cash-flow `dvc` is positive in CRSP convention vs negative in some retail clones.
3. **Period-indicator (FPI/FPERIOD/estimatePeriodType) is the second silent bug.** Every vendor uses a different encoding for "next fiscal year" — IBES `fpi='2'`, FactSet `FY2`, Bloomberg `1FY` (their `1FY`=current FY!), CIQ `estimatePeriodTypeId=1` with `IQ_FY1`. Direct equality joins between vendors on FPI almost always misalign.
4. **CUSIP licensing makes FIGI the only safe public spine.** SEC's June 2022 13F amendments explicitly permit FIGI as the alternative identifier. Three of the four major vendors (FactSet, S&P, LSEG) require CUSIP redistribution licences; only Bloomberg-stewarded FIGI is MIT-licensed. ats-eqt's public API surface should never expose CUSIP.
5. **The "value" units discontinuity.** 13F `<value>` switched from $thousands to $actual on 2023-01-03 (SEC final rule 34-95148). Compustat is in $millions. FactSet `FF_*` is in `ff_curr_cd` raw units. Sharadar SF1 is in $actual. Naive cross-vendor SUMs are wrong by 10^3 or 10^6 about 30% of the time without a careful unit-multiplier layer.

The full cross-vendor field map follows.

---

## 1. How to use this document

### 1.1 Reading a row

Every row has a single **Canonical concept** (left column) and one or more vendor columns showing the exact vendor-side field name to query that concept. The `ats-eqt item_id` column is a proposed sequential integer that will populate the `fund_item.item_id` dictionary in
[`schemas/data_models_and_methodology.md`](data_models_and_methodology.md) §G.2.
Item IDs in this doc are *proposals*, not yet committed; the formal allocation
will happen during the Phase-0 ingestion sprint.

### 1.2 The `?` and `[unverified]` conventions

- A bare vendor field name (e.g. `revt`) means the wave-2 source doc had it as a *verified* mapping with a confirmed authoritative source URL.
- `[unverified]` after a vendor cell means the wave-2 source doc itself flagged that name as not confirmed against a primary vendor source. The provenance carries through — do not silently drop the flag if you re-emit this table.
- `?` (a literal question-mark cell) means the wave-2 doc did not catalogue this field for that vendor, and we did not find it elsewhere in the input corpus. Resolve in a wave-3 pass against subscriber docs.
- `—` (em-dash) means the concept does not exist in that vendor's product (e.g. CRSP has no fundamentals; us-gaap has no market-cap concept; IFRS has no `MinorityInterest` because IFRS calls it `NoncontrollingInterests` and we map it on the IFRS column).

### 1.3 Disambiguation rule for many-to-one mappings

When **multiple** vendor fields map to one canonical concept (most common case: cash + short-term-investments combined, or the post-ASC 606 revenue family in us-gaap), the row shows the **preferred** field first followed by the alternatives separated by `/` and a `Notes` column entry explaining the COALESCE order. This matches the recipe pattern in [`datasets/fundamentals_us_equities.md`](../datasets/fundamentals_us_equities.md) §8.3 "Problem 1 — Multiple concepts for the same economic line".

When **one** vendor field maps to **many** canonical concepts (rare; mostly happens with Bloomberg `SHORT_AND_LONG_TERM_DEBT` covering both ST and LT debt), we emit one row per canonical concept and Notes flags the splitter rule.

---

## 2. Master cross-vendor table — Fundamentals (US-GAAP / IFRS)

The largest section. Organized by financial-statement section. Source for all rows is [`datasets/fundamentals_us_equities.md`](../datasets/fundamentals_us_equities.md) Section 9 (the authoritative cross-vendor table) plus the §2 Compustat, §3 FactSet, §4 Worldscope, §5 Bloomberg, §6 CIQ, §7 Sharadar/SimFin enumerations.

### 2.1 Income statement

| Canonical concept | item_id | us-gaap | IFRS | Compustat (A / Q) | FactSet | Worldscope | Bloomberg | CIQ | Sharadar SF1 | Notes |
|---|---|---|---|---|---|---|---|---|---|---|
| Total Revenue | 1001 | `RevenueFromContractWithCustomerExcludingAssessedTax` / `Revenues` / `SalesRevenueNet` | `ifrs-full:Revenue` | `revt` / `revtq` | `FF_SALES` | `01001` / `WS.NetSales` | `SALES_REV_TURN` / `IS_TOTAL_REVENUE` | `IQ_TOTAL_REV` (dataItemId=100174) | `revenue` | COALESCE order: ASC-606 (post-2018) → legacy `Revenues` → bank fallback (`InterestAndDividendIncomeOperating + NoninterestIncome`) → insurance (`PremiumsEarnedNet + NetInvestmentIncome`). |
| Sales (legacy) | 1002 | (alias) | — | `sale` / `saleq` | `FF_SALES` | `01001` | `IS_TOTAL_REVENUE` | `IQ_TOTAL_REV` | `revenue` | Numerically identical to Total Revenue for industrials; differs for finance subs. |
| Cost of Revenue / COGS | 1003 | `CostOfGoodsAndServicesSold` / `CostOfRevenue` / `CostOfGoodsSold` | `ifrs-full:CostOfSales` | `cogs` / `cogsq` | `FF_COGS` | `01051` / `WS.CostofGoodsSold` | `IS_COGS` / `IS_COG_AND_SERVICES_SOLD` | `IQ_COGS` | `cor` | Post-2018 default is `CostOfGoodsAndServicesSold`. |
| Gross Profit | 1004 | `GrossProfit` | `ifrs-full:GrossProfit` | `revt - cogs` (derived) | `FF_GROSS_INC` | `01100` | `GROSS_PROFIT` | `IQ_GROSS_PROFIT` (dataItemId=112) | `gp` | Derived in Compustat; explicit in FactSet/Bloomberg/CIQ. |
| SG&A | 1005 | `SellingGeneralAndAdministrativeExpense` | `ifrs-full:SellingGeneralAndAdministrativeExpense` | `xsga` / `xsgaq` | `FF_SGA` | `01101` | `SG_AND_A_EXPENSE` | `IQ_SGA` | `sgna` | Some filers break Selling and G&A separately; sum. |
| Selling expense (only) | 1006 | `SellingAndMarketingExpense` | — | — | `FF_SGA_SELL` | ? | `IS_SELLING_EXP` | ? | — | |
| G&A expense (only) | 1007 | `GeneralAndAdministrativeExpense` | — | — | `FF_SGA_GEN_ADMIN` | ? | `IS_GEN_ADMIN_EXP` | ? | — | |
| R&D Expense | 1008 | `ResearchAndDevelopmentExpense` | `ifrs-full:ResearchAndDevelopmentExpense` | `xrd` / `xrdq` | `FF_RD_EXP` | `01101A` `[unverified]` | `IS_RD_EXPEND` / `IS_OPER_RD` | `IQ_RD_EXP` | `rnd` | ASC 730 mandatory disclosure → >95% reconstructable. |
| Advertising | 1009 | `AdvertisingExpense` `[unverified]` | — | `xad` | `FF_SGA_MKT` `[unverified]` | ? | ? | ? | — | Advertising is below mandatory threshold in most filings. |
| Operating Expenses (total) | 1010 | `OperatingExpenses` | — | (computed: `cogs+xsga+xrd+dp`) | `FF_OPER_EXP` `[unverified]` | ? | `IS_OPER_EXPN` | `IQ_OPER_EXP` `[unverified]` | `opex` | |
| D&A (Income statement) | 1011 | `DepreciationAndAmortization` | `ifrs-full:DepreciationAndAmortisationExpense` | `dp` / `dpq` | `FF_DEP_AMORT_EXP` | `01201` | `IS_DEPRECIATION_EXP` + `IS_AMORT_EXP` | `IQ_DA_IS` | `depamor` | When on income statement (vs CF). |
| Depreciation only | 1012 | `Depreciation` | — | — | — | ? | `IS_DEPRECIATION_EXP` | ? | — | |
| Amortization of intangibles | 1013 | `AmortizationOfIntangibleAssets` | — | `am` | `FF_AMORT_EXP` | ? | `IS_AMORT_EXP` | ? | — | |
| Operating Income | 1014 | `OperatingIncomeLoss` | `ifrs-full:ProfitLossFromOperatingActivities` | `oiadp` / `oiadpq` | `FF_OPER_INC` | `01250` | `IS_OPER_INC` | `IQ_OPER_INC` | `opinc` | Compustat `oiadp` = After D&A. |
| Operating Income before D&A (EBITDA-ish) | 1015 | derived | — | `oibdp` / `oibdpq` | `FF_EBITDA_OPER` | `01266` | `EBITDA` (standardised) | `IQ_EBITDA_OPER` `[unverified]` | `ebitda` | Compustat `oibdp` ~= EBITDA for non-bank filers. |
| EBITDA (standardised) | 1016 | derived | derived | `ebitda` (when populated) / derived | `FF_EBITDA` | `01266` | `EBITDA` | `IQ_EBITDA` (dataItemId=164) | `ebitda` | Standardised by each vendor; not a us-gaap tag. |
| EBIT | 1017 | derived | derived | `ebit` | `FF_EBIT` | `01254` | `EBIT` | `IQ_EBIT` (dataItemId=21) | `ebit` | Not a us-gaap tag. |
| Interest Expense (total) | 1018 | `InterestExpense` | `ifrs-full:FinanceCosts` | `xint` / `xintq` | `FF_INT_EXP_TOT` | `01301` | `IS_INT_EXPENSE` | `IQ_INT_EXP` | `intexp` | |
| Interest Expense (debt only) | 1019 | `InterestExpenseDebt` | — | — | `FF_INT_EXP_DEBT` | ? | ? | ? | — | |
| Interest Income | 1020 | `InterestIncome` / `InvestmentIncomeInterest` | — | — | `FF_INT_INC` | ? | `IS_INT_INC` | `IQ_INT_INC` `[unverified]` | — | |
| Non-operating Income (Expense) | 1021 | `NonoperatingIncomeExpense` | — | `nopi` | `FF_NON_OPER_INC` | ? | ? | ? | — | |
| Special Items | 1022 | `RestructuringCharges` / `AssetImpairmentCharges` | — | `spi` | `FF_SPECIAL_ITEMS` | ? | ? | ? | — | |
| Pretax Income | 1023 | `IncomeLossFromContinuingOperationsBeforeIncomeTaxesExtraordinaryItemsNoncontrollingInterest` | `ifrs-full:ProfitLossBeforeTax` | `pi` / `piq` | `FF_PRETAX_INC` | `01401` | `IS_PRETAX_INC` | `IQ_PRETAX_INC` | `ebt` | Long us-gaap name. |
| Income Tax (total) | 1024 | `IncomeTaxExpenseBenefit` | `ifrs-full:IncomeTaxExpenseContinuingOperations` | `txt` / `txtq` | `FF_INC_TAX` | `01451` | `IS_INC_TAX_EXP` | `IQ_INCOME_TAX` | `taxexp` | |
| Current tax | 1025 | `CurrentIncomeTaxExpenseBenefit` | — | `txc` | `FF_INC_TAX_CURR` | ? | `IS_CURRENT_TAX_EXPENSE` | ? | — | |
| Deferred tax | 1026 | `DeferredIncomeTaxExpenseBenefit` | — | `txdi` | `FF_INC_TAX_DEFER` | ? | `IS_DEFERRED_TAX_EXPENSE` | ? | — | |
| Minority Interest (P&L) | 1027 | `NetIncomeLossAttributableToNoncontrollingInterest` / `MinorityInterestInNetIncomeLoss` | `ifrs-full:ProfitLossAttributableToNoncontrollingInterests` | `mii` | `FF_MIN_INT_INC` | ? | `IS_MIN_NONCONTROL_INTEREST_CREDIT` | `IQ_MINORITY_NI` `[unverified]` | — | |
| Equity in Affiliates | 1028 | `IncomeLossFromEquityMethodInvestments` | — | — | `FF_EQ_AFF_INC` | ? | ? | ? | — | |
| Income Before Extraordinary | 1029 | `IncomeLossFromContinuingOperations` | `ifrs-full:ProfitLossFromContinuingOperations` | `ib` / `ibq` | `FF_NET_INC` | `01601` | `IS_INC_BEF_XO_ITEM` | `IQ_NI_CONT_OPS` | — | |
| Discontinued Ops NI | 1030 | `IncomeLossFromDiscontinuedOperationsNetOfTax` | — | (in `xido`) | `FF_NET_INC_DISC` | ? | `IS_EARN_FROM_DISC_OPS` | ? | `netincdis` | |
| Net Income (Total) | 1031 | `NetIncomeLoss` | `ifrs-full:ProfitLoss` | `ni` / `niq` | `FF_NET_INC_TOT` | `01751` | `NET_INCOME` | `IQ_NI` (dataItemId=142) | `netinc` | The most universally tagged concept. >99% reconstructable. |
| Net Income to Common | 1032 | `NetIncomeLossAvailableToCommonStockholdersBasic` | — | `ibcom` / `ibcomq` | `FF_NET_INC_AVAIL` | `01706` | `NET_INCOME_TO_COMMON` | `IQ_NI_COMMON` | `netinccmn` | |
| Preferred Dividends | 1033 | `PreferredStockDividends` | — | `dvp` / `dvpy` | `FF_DIV_PFD` | ? | `CF_PFD_DVDS_PAID` | `IQ_PREF_DIV_PAID` (dataItemId=1281) | `prefdivis` | |
| EPS — Basic | 1034 | `EarningsPerShareBasic` | `ifrs-full:BasicEarningsLossPerShare` | `epspx` / `epspxq` | `FF_EPS_BASIC` | `05001` | `IS_BASIC_EPS` | `IQ_BASIC_EPS_EXCL` (dataItemId=1001) | `eps` | |
| EPS — Diluted | 1035 | `EarningsPerShareDiluted` | `ifrs-full:DilutedEarningsLossPerShare` | `epsfx` / `epsfxq` | `FF_EPS_DIL` | `05011` | `IS_DILUTED_EPS` | `IQ_DILUT_EPS_EXCL` (dataItemId=14) | `epsdil` | |
| EPS — Basic incl. extra | 1036 | `IncomeLossFromContinuingOperationsPerBasicShare` | — | `epspi` / `epspiq` | `FF_EPS_BASIC_EXT` | ? | `IS_EPS_CONT_OPS` | `IQ_BASIC_EPS_INCL_EXTRA` (dataItemId=1000) | — | |
| EPS — Diluted incl. extra | 1037 | `IncomeLossFromContinuingOperationsPerDilutedShare` | — | `epsfi` / `epsfiq` | `FF_EPS_DIL_EXT` | ? | `IS_DIL_EPS_CONT_OPS` | `IQ_EPS_INCL_EXTRA` (dataItemId=15) | — | |
| EPS — Diluted LTM | 1038 | derived | — | `epsf12` (Q only) | `FF_EPS_DIL` (LTM table) | — | `TRAIL_12M_DIL_EPS_CONT_OPS` | derived | — | |
| Shares Outstanding (period-end) | 1039 | `CommonStockSharesOutstanding` / `dei:EntityCommonStockSharesOutstanding` | — | `csho` / `cshoq` | `FF_SHS_OUTSTND` | `05101` | `BS_SH_OUT` / `EQY_SH_OUT` | `IQ_SHARES_OUT` | `sharesbas` | Cover-page tagged ≥99%. |
| Weighted Avg Shares — Basic | 1040 | `WeightedAverageNumberOfSharesOutstandingBasic` | `ifrs-full:WeightedAverageShares` | `cshpri` / `cshprq` | `FF_SHS_BASIC` | `05151` | `IS_AVG_NUM_SH_FOR_EPS` | `IQ_BASIC_WEIGHT_AVG_SH` | `shareswa` | |
| Weighted Avg Shares — Diluted | 1041 | `WeightedAverageNumberOfDilutedSharesOutstanding` | `ifrs-full:AdjustedWeightedAverageShares` | `cshfd` / `cshfdq` | `FF_SHS_DIL` | `05161` `[unverified]` | `IS_SH_FOR_DILUTED_EPS` | `IQ_DILUT_WEIGHT_AVG_SH` | `shareswadil` | |
| Normalised Income | 1042 | derived | — | — | — | ? | `NORMALIZED_INCOME` / `IS_NORMALIZED_INCOME` | ? | — | Non-recurring items removed; vendor-defined. |
| Common Dividends Declared per Share | 1043 | derived | — | `dvpsx_f` | `FF_DIV_PS` | ? | `DPS` | `IQ_DPS` | `dps` | |

### 2.2 Balance sheet

| Canonical concept | item_id | us-gaap | IFRS | Compustat (A / Q) | FactSet | Worldscope | Bloomberg | CIQ | Sharadar SF1 | Notes |
|---|---|---|---|---|---|---|---|---|---|---|
| Total Assets | 1101 | `Assets` | `ifrs-full:Assets` | `at` / `atq` | `FF_ASSETS` | `02999` | `BS_TOT_ASSET` | `IQ_TOTAL_ASSETS` (dataItemId=1007) | `assets` | |
| Current Assets | 1102 | `AssetsCurrent` | `ifrs-full:CurrentAssets` | `act` / `actq` | `FF_ASSETS_CURR` | `02201` | `BS_CUR_ASSET_REPORT` | `IQ_CURRENT_ASSETS` | `assetsc` | |
| Cash & ST Investments | 1103 | `CashAndCashEquivalentsAtCarryingValue + ShortTermInvestments` | `ifrs-full:CashAndCashEquivalents` | `che` / `cheq` | `FF_CASH_ST` | `02001` | `BS_CASH_NEAR_CASH_ITEM` | `IQ_CASH_ST_INVEST` (dataItemId=1003) | `cashneq` | Sum of two us-gaap concepts. |
| Cash only | 1104 | `Cash` / `CashAndCashEquivalentsAtCarryingValue` | — | `ch` / `chq` | `FF_CASH` | `02003` | `BS_CASH` | `IQ_CASH` | — | |
| Short-Term Investments | 1105 | `ShortTermInvestments` / `MarketableSecuritiesCurrent` | — | `ivst` | `FF_ST_INVEST` | ? | `BS_ST_INVEST` | `IQ_ST_INVEST` `[unverified]` | `investmentsc` | |
| Accounts Receivable | 1106 | `AccountsReceivableNetCurrent` / `ReceivablesNetCurrent` | `ifrs-full:TradeAndOtherReceivables` | `rect` / `rectq` | `FF_RECV_NET` | `02051` | `BS_ACCT_REC` | `IQ_AR` (dataItemId=1021) | `receivables` | |
| Inventory | 1107 | `InventoryNet` | `ifrs-full:Inventories` | `invt` / `invtq` | `FF_INVENT` | `02101` | `BS_INVENTORIES` | `IQ_INVENTORY` (dataItemId=1023) | `inventory` | |
| Prepaid Expense | 1108 | `PrepaidExpenseCurrent` | — | — | `FF_PREPAID` | ? | ? | ? | — | |
| Other Current Assets | 1109 | `OtherAssetsCurrent` | — | — | `FF_OTH_CURR_ASSET` | ? | `BS_OTHER_CUR_ASSETS` | ? | — | |
| PP&E — Net | 1110 | `PropertyPlantAndEquipmentNet` | `ifrs-full:PropertyPlantAndEquipment` | `ppent` / `ppentq` | `FF_PPE_NET` | `02301` | `BS_NET_FIX_ASSET` | `IQ_NET_PPE` | `ppnenet` | |
| PP&E — Gross | 1111 | `PropertyPlantAndEquipmentGross` | — | `ppegt` / `ppegtq` | `FF_PPE_GROSS` | `02351` | `BS_GROSS_FIX_ASSET` | `IQ_GROSS_PPE` | — | |
| Accumulated Depreciation | 1112 | `AccumulatedDepreciationDepletionAndAmortizationPropertyPlantAndEquipment` | — | derived | derived | ? | ? | ? | — | |
| Intangibles (total) | 1113 | `IntangibleAssetsNetExcludingGoodwill + Goodwill` | — | `intan` / `intanq` | `FF_INTANG` | ? | `BS_TOT_INTANG_ASSETS` | `IQ_INTAN` `[unverified]` | `intangibles` | |
| Goodwill | 1114 | `Goodwill` | `ifrs-full:Goodwill` | `gdwl` / `gdwlq` | `FF_INTANG_GW` | `02501` | `BS_GOODWILL` | `IQ_GW` (dataItemId=1029) | (in `intangibles`) | |
| Other Intangibles | 1115 | `IntangibleAssetsNetExcludingGoodwill` | `ifrs-full:IntangibleAssetsOtherThanGoodwill` | `intan - gdwl` | `FF_INTANG_OTH` | `02649` | `BS_OTHER_INTANG_ASSETS` | `IQ_OTHER_INTAN` | — | |
| Operating Lease ROU asset | 1116 | `OperatingLeaseRightOfUseAsset` | — | — | ? | ? | ? | ? | — | ASC 842. |
| Long-Term Investments | 1117 | `LongTermInvestments` / `EquityMethodInvestments` | — | `ivao` | `FF_INVEST_LT` | ? | `BS_LT_INVEST` | ? | `investmentsnc` | |
| Deferred Tax Assets | 1118 | `DeferredIncomeTaxAssetsNet` | — | — | ? | ? | ? | ? | `taxassets` | |
| Other LT Assets | 1119 | `OtherAssetsNoncurrent` | — | — | `FF_OTH_LT_ASSET` | ? | ? | ? | — | |
| Total Liabilities | 1201 | `Liabilities` | `ifrs-full:Liabilities` | `lt` / `ltq` | `FF_LIAB` | `03251A` | `BS_TOT_LIAB2` | `IQ_TOTAL_LIAB` (dataItemId=1008) | `liabilities` | |
| Current Liabilities | 1202 | `LiabilitiesCurrent` | `ifrs-full:CurrentLiabilities` | `lct` / `lctq` | `FF_LIAB_CURR` | `03251` | `BS_CUR_LIAB` | `IQ_CURR_LIAB` | `liabilitiesc` | |
| Accounts Payable | 1203 | `AccountsPayableCurrent` | `ifrs-full:TradeAndOtherPayables` | `ap` / `apq` | `FF_PAYABLES` | `03001` | `BS_ACCT_PAYABLE` | `IQ_AP` (dataItemId=1024) | `payables` | |
| Accrued Liabilities | 1204 | `AccruedLiabilitiesCurrent` | — | — | ? | ? | ? | ? | — | |
| Short-Term Debt | 1205 | `LongTermDebtCurrent + ShortTermBorrowings + CommercialPaper` | `ifrs-full:CurrentBorrowings` | `dlc` / `dlcq` | `FF_DEBT_ST` | `03051` | `BS_ST_BORROW + BS_CUR_PORTION_LT_DEBT` | `IQ_ST_DEBT` (dataItemId=1011) | `debtc` | Sum of three us-gaap concepts. |
| Current portion of LT Debt | 1206 | `LongTermDebtCurrent` | — | `dd1` | (sub-component of FF_DEBT_ST) | ? | `BS_CUR_PORTION_LT_DEBT` | ? | — | |
| Long-Term Debt | 1207 | `LongTermDebtNoncurrent` | `ifrs-full:NoncurrentBorrowings` | `dltt` / `dlttq` | `FF_DEBT_LT` | `03255` | `BS_LT_BORROW` / `BS_LT_DEBT` | `IQ_LT_DEBT` (dataItemId=1009) | `debtnc` | |
| Total Debt | 1208 | `LongTermDebt + ShortTermBorrowings` | derived | `dlc + dltt` | `FF_DEBT` | `03051 + 03255` | `SHORT_AND_LONG_TERM_DEBT` | `IQ_TOTAL_DEBT` (dataItemId=1005) | `debt` | Bloomberg has single field; others computed. |
| Operating Lease Liability | 1209 | `OperatingLeaseLiabilityCurrent + OperatingLeaseLiabilityNoncurrent` | — | — | `FF_OPER_LEASES_PV` `[unverified]` | ? | `BS_OPER_LEASE_LIAB` | ? | — | ASC 842. |
| Deferred Revenue | 1210 | `DeferredRevenue` | — | — | `FF_DEFERRED_REV_ST + FF_DEFERRED_REV_LT` | ? | ? | ? | `deferredrev` | |
| Deferred Tax Liabilities | 1211 | `DeferredIncomeTaxLiabilitiesNet` | — | `txditc` (combined w/ITC) | `FF_DEFERRED_TX` | ? | `BS_DEFERRED_TAX_LIAB` | ? | `taxliabilities` | Compustat bundles deferred tax + ITC. |
| Other LT Liabilities | 1212 | `OtherLiabilitiesNoncurrent` | — | — | `FF_OTH_LT_LIAB` | ? | ? | ? | `liabilitiesnc` | |
| Minority Interest (BS) | 1213 | `MinorityInterest` | `ifrs-full:NoncontrollingInterests` | `mib` / `mibq` | `FF_MIN_INT_BS` | `03401` | `BS_MIN_NONCONTROL_INTEREST` | `IQ_MINORITY` | — | |
| Preferred Stock | 1214 | `PreferredStockValue` | — | `pstk` / `pstkq` | `FF_PREF_STK` | `03451` | `BS_PFD_EQUITY` | `IQ_PREF_EQ` | (in `equity`) | |
| Common Stock at par | 1215 | `CommonStockValue` | — | — | `FF_COM_STK_PAR` | ? | ? | ? | — | |
| Additional Paid-In Capital | 1216 | `AdditionalPaidInCapital` / `AdditionalPaidInCapitalCommonStock` | — | — | (in `FF_COM_STK_PAR`) | ? | `BS_SH_CAP_AND_APIC` | ? | — | |
| Retained Earnings | 1217 | `RetainedEarningsAccumulatedDeficit` | `ifrs-full:RetainedEarnings` | `re` / `req` | `FF_RETAIN_EARN` | `03999A` | `BS_PURE_RETAINED_EARNINGS` | `IQ_RE` | `retearn` | |
| AOCI | 1218 | `AccumulatedOtherComprehensiveIncomeLossNetOfTax` | — | — | ? | ? | ? | ? | `accoci` | |
| Treasury Stock | 1219 | `TreasuryStockValue` | `ifrs-full:TreasuryShares` | `tstk` / `tstkq` | `FF_TREAS_STK` | `03999B` | `BS_TREASURY_STOCK` | `IQ_TREASURY` | (in `equity`) | |
| Common Equity | 1220 | derived | derived | `ceq` / `ceqq` | `FF_COM_EQ_TOT` | `03501` | `TOTAL_EQUITY - BS_PFD_EQUITY` | `IQ_COMMON_EQUITY` | — | |
| Stockholders' Equity | 1221 | `StockholdersEquity` | `ifrs-full:Equity` | `seq` / `seqq` | `FF_EQ_TOT` | `03999` | `TOTAL_EQUITY` | `IQ_TOTAL_EQUITY` (dataItemId=1275) | `equity` | |
| Equity incl. Non-controlling | 1222 | `StockholdersEquityIncludingPortionAttributableToNoncontrollingInterest` | `ifrs-full:Equity` (broader) | derived | ? | ? | ? | ? | — | |
| Total Liab + Equity | 1223 | `LiabilitiesAndStockholdersEquity` | — | = `at` | derived | ? | derived | derived | — | Must equal Total Assets. |

### 2.3 Cash flow statement

| Canonical concept | item_id | us-gaap | IFRS | Compustat (A / Q) | FactSet | Worldscope | Bloomberg | CIQ | Sharadar SF1 | Notes |
|---|---|---|---|---|---|---|---|---|---|---|
| Cash Flow from Operations | 1301 | `NetCashProvidedByUsedInOperatingActivities` | `ifrs-full:CashFlowsFromUsedInOperatingActivities` | `oancf` / `oancfy` | `FF_CASH_FROM_OPER` | `04001` | `CF_CASH_FROM_OPER` | `IQ_CASH_OPER` (dataItemId=1094) | `ncfo` | Quarterly Compustat values are YTD per SFAS 95. |
| CFO (Continuing Ops) | 1302 | `NetCashProvidedByUsedInOperatingActivitiesContinuingOperations` | — | — | ? | ? | ? | ? | — | |
| Cash Flow from Investing | 1303 | `NetCashProvidedByUsedInInvestingActivities` | `ifrs-full:CashFlowsFromUsedInInvestingActivities` | `ivncf` / `ivncfy` | `FF_CASH_FROM_INVEST` | `04401` | `CF_CASH_FROM_INV_ACT` | `IQ_CASH_INVEST` (dataItemId=1097) | `ncfi` | |
| Cash Flow from Financing | 1304 | `NetCashProvidedByUsedInFinancingActivities` | `ifrs-full:CashFlowsFromUsedInFinancingActivities` | `fincf` / `fincfy` | `FF_CASH_FROM_FIN` | `04801` | `CF_CASH_FROM_FNC_ACT` | `IQ_CASH_FIN` (dataItemId=1100) | `ncff` | |
| Capex | 1305 | `PaymentsToAcquirePropertyPlantAndEquipment` | `ifrs-full:PurchaseOfPropertyPlantAndEquipmentClassifiedAsInvestingActivities` | `capx` / `capxy` | `FF_CAPEX` | `04201` | `CF_CAP_EXPEND` / `CF_CAP_EXPENDITURES` | `IQ_CAPEX` (dataItemId=1093) | `capex` | |
| Capex broader (incl intangibles) | 1306 | `PaymentsToAcquireProductiveAssets` | — | — | ? | ? | ? | ? | — | |
| D&A (Cash flow) | 1307 | `DepreciationDepletionAndAmortization` | `ifrs-full:DepreciationAndAmortisationExpense` | `dpc` / `dpcy` | `FF_DEP_AMORT_CF` | `04101` `[unverified]` | `CF_DEPR_AMORT` | `IQ_DA_CF` / `IQ_DEP_AMORT` (dataItemId=1300) | `depamor` | |
| Stock-Based Compensation | 1308 | `ShareBasedCompensation` | `ifrs-full:ShareBasedPaymentsRecognised` | — (in advanced) | `FF_STOCK_COMP` | ? | `CF_STOCK_BASED_COMP` | `IQ_STOCK_COMP` | `sbcomp` | Compustat doesn't carry; FactSet Advanced does. |
| Acquisitions | 1309 | `PaymentsToAcquireBusinessesNetOfCashAcquired` | — | `aqc` / `aqcy` | `FF_ACQUIS` | ? | `CF_ACQUIS_OF_BUSINESS` | `IQ_ACQUISITIONS` | `ncfbus` | |
| Divestitures | 1310 | `ProceedsFromDivestitureOfBusinesses` | — | `sppe` | `FF_DIVEST` | ? | `CF_DISP_OF_BUSINESS` | ? | — | |
| Stock Issuance | 1311 | `ProceedsFromIssuanceOfCommonStock` | — | `sstk` / `sstky` | `FF_STOCK_ISSUE` | `04601` | `CF_ISSUE_OF_STOCK` | `IQ_STOCK_ISSUED` | (in `ncfcommon`) | |
| Stock Repurchases (buybacks) | 1312 | `PaymentsForRepurchaseOfCommonStock` | — | `prstkc` / `prstkcy` | `FF_STOCK_REPUR` | `04651` | `CF_REPURCH_OF_STOCK` | `IQ_BUYBACKS` | (in `ncfcommon`) | |
| LT Debt Issued | 1313 | `ProceedsFromIssuanceOfLongTermDebt` | — | `dltis` | `FF_DEBT_ISSUE` | `04701` | `CF_PROCEEDS_LT_DEBT` | `IQ_LT_DEBT_ISSUE` `[unverified]` | (in `ncfdebt`) | |
| LT Debt Repaid | 1314 | `RepaymentsOfLongTermDebt` | — | `dltr` | `FF_DEBT_REDUC` | `04751` | `CF_REPAY_LT_DEBT` | `IQ_LT_DEBT_REPAY` `[unverified]` | (in `ncfdebt`) | |
| Net Change in Debt | 1315 | derived | — | derived | derived | ? | `CF_NET_CHG_DEBT` | ? | `ncfdebt` | |
| Common Dividends Paid | 1316 | `PaymentsOfDividendsCommonStock` | `ifrs-full:DividendsPaidOrdinaryShares` | `dvc` / `dvcy` | `FF_DIV_COM` / `FF_DIV_CASH` | `04551` | `CF_DVD_PAID` | `IQ_COMMON_DIV_PAID` (dataItemId=1278) | (in `ncfdiv`) | |
| Preferred Dividends Paid | 1317 | `PaymentsOfDividendsPreferredStockAndPreferenceStock` | — | `dvp` / `dvpy` | `FF_DIV_PFD_CF` | ? | `CF_PFD_DVDS_PAID` | `IQ_PREF_DIV_PAID` (dataItemId=1281) | — | |
| Total Dividends Paid | 1318 | `PaymentsOfDividends` | — | `dv` (annual only) | (sum of two above) | ? | (sum) | ? | `ncfdiv` | |
| Change in AR | 1319 | `IncreaseDecreaseInAccountsReceivable` | — | `recch` | `FF_CHG_AR` | ? | ? | ? | — | Sign: increase = use of cash. |
| Change in Inventory | 1320 | `IncreaseDecreaseInInventories` | — | — | `FF_CHG_INV` | ? | ? | ? | — | |
| Change in AP | 1321 | `IncreaseDecreaseInAccountsPayable` | — | — | `FF_CHG_AP` | ? | ? | ? | — | |
| Change in Working Capital | 1322 | derived (sum of WC changes) | — | `wcapc` (legacy) | `FF_CHG_WC` | ? | ? | ? | `workingcapital` | |
| FX Effect on Cash | 1323 | `EffectOfExchangeRateOnCashCashEquivalentsRestrictedCashAndRestrictedCashEquivalents` | — | `exre` | `FF_EXCH_RATE_CF` | ? | `CF_FX_EFFECT_CASH` | ? | `ncfx` | |
| Net Change in Cash | 1324 | `CashCashEquivalentsRestrictedCashAndRestrictedCashEquivalentsPeriodIncreaseDecreaseIncludingExchangeRateEffect` | — | `chech` | `FF_NET_CHG_CASH` | ? | `CF_NET_CHNG_CASH` | ? | `ncf` | |
| Free Cash Flow | 1325 | derived (CFO − Capex) | derived | `oancf - capx` | `FF_FCF` | `04860` | `CF_FREE_CASH_FLOW` | `IQ_FCF` | `fcf` | Standardised differently across vendors. |

### 2.4 Per-share / market / ratios (derived)

These are computed, not raw filings — but they appear with vendor-specific names. Source: [`datasets/fundamentals_us_equities.md`](../datasets/fundamentals_us_equities.md) §3.5 (FF_DER), §7.1 (Sharadar ratios).

| Canonical concept | item_id | us-gaap | Compustat | FactSet (FF_DER) | Worldscope | Bloomberg | CIQ | Sharadar | Notes |
|---|---|---|---|---|---|---|---|---|---|
| Book Value per Share | 1401 | — | derived | `FF_BV_PS` | ? | `BOOK_VAL_PER_SH` `[unverified]` | ? | `bvps` | |
| Tangible Book Value per Share | 1402 | — | derived | `FF_TANG_BV_PS` | ? | ? | ? | `tbvps` | |
| FCF per Share | 1403 | — | derived | `FF_FCF_PS` | ? | ? | ? | `fcfps` | |
| Sales per Share | 1404 | — | derived | `FF_SALES_PS` | ? | ? | ? | `sps` | |
| Cash per Share | 1405 | — | derived | `FF_CASH_PS` | ? | ? | ? | — | |
| Dividend Yield | 1406 | — | — | `FF_DIV_YIELD` | ? | `EQY_DVD_YLD_IND` `[unverified]` | ? | `divyield` | |
| P/E | 1407 | — | derived | `FF_PE` | `PE` (Datastream) | `PE_RATIO` `[unverified]` | ? | `pe` | |
| P/B | 1408 | — | derived | `FF_PB` | ? | `PX_TO_BOOK_RATIO` `[unverified]` | ? | `pb` | |
| P/S | 1409 | — | derived | `FF_PS` | ? | ? | ? | `ps` | |
| Enterprise Value | 1410 | — | derived | `FF_EV` | ? | `ENTERPRISE_VALUE` `[unverified]` | ? | `ev` | |
| EV / EBITDA | 1411 | — | derived | `FF_EV_EBITDA` | ? | ? | ? | `evebitda` | |
| EV / Sales | 1412 | — | derived | `FF_EV_SALES` | ? | ? | ? | — | |
| Market Cap | 1413 | — | `mkvalt` / `mkvaltq` | `FF_MKT_CAP_TOT` | `05491` | `CUR_MKT_CAP` / `HISTORICAL_MARKET_CAP` | `IQ_MARKETCAP` | `marketcap` | |
| Gross Margin | 1414 | — | derived | `FF_GROSS_MARGIN` | ? | ? | ? | `grossmargin` | |
| Operating Margin | 1415 | — | derived | `FF_OPER_MARGIN` | ? | ? | ? | `ebitmargin` | |
| EBITDA Margin | 1416 | — | derived | `FF_EBITDA_MARGIN` | ? | ? | ? | `ebitdamargin` | |
| Net Margin | 1417 | — | derived | `FF_NET_MARGIN` | ? | ? | ? | `netmargin` | |
| ROA | 1418 | — | derived | `FF_ROA` | ? | ? | ? | `roa` | |
| ROE | 1419 | — | derived | `FF_ROE` | ? | ? | ? | `roe` | |
| ROIC | 1420 | — | derived | `FF_ROIC` | ? | ? | ? | `roic` | |
| Current Ratio | 1421 | — | derived | `FF_CURR_RATIO` | ? | ? | ? | `currentratio` | |
| Debt / Equity | 1422 | — | derived | `FF_DEBT_EQUITY` | ? | ? | ? | `de` | |
| Interest Coverage | 1423 | — | derived | `FF_INT_COVER` | ? | ? | ? | — | |
| DSO (days) | 1424 | — | — | `FF_RECV_DAYS` | ? | ? | ? | — | |
| DIO (days) | 1425 | — | — | `FF_INV_DAYS` | ? | ? | ? | — | |
| DPO (days) | 1426 | — | — | `FF_PAY_DAYS` | ? | ? | ? | — | |
| Cash Conversion Cycle | 1427 | — | — | `FF_CCC` | ? | ? | ? | — | |

### 2.5 Industry-specific — Banks (FS template)

Source: [`datasets/fundamentals_us_equities.md`](../datasets/fundamentals_us_equities.md) §2.3 (Compustat bank-template items), §3.6 (FactSet FF_BANK), §6.5 (CIQ bank items).

| Canonical concept | item_id | us-gaap | Compustat (FS template) | FactSet (FF_BANK) | Bloomberg | CIQ | Notes |
|---|---|---|---|---|---|---|---|
| Net Interest Income | 1501 | `InterestIncomeOperating - InterestExpense` (derived) | — `[unverified — bank template]` | `FF_NET_INT_INC` | ? | `IQ_NET_INT_INC` (dataItemId=1900) | |
| Net Interest Margin | 1502 | derived | `nim` | `FF_NIM` | ? | `IQ_NIM` (dataItemId=1901) | |
| Interest Income — total | 1503 | `InterestAndDividendIncomeOperating` | `intinc` | ? | ? | ? | |
| Interest Expense — bank | 1504 | `InterestExpense` | `intexp` | ? | ? | ? | Overrides industrial `xint`. |
| Provision for Loan Losses | 1505 | `ProvisionForLoanAndLeaseLosses` | `pln` | `FF_PROV_LOAN_LOSS` | ? | `IQ_PROV_LOAN_LOSSES` (dataItemId=1903) | |
| Allowance for Loan & Lease Losses | 1506 | — | `alll` | `FF_ALLL` | ? | ? | |
| Non-Performing Loans | 1507 | — | ? | `FF_NPL` | ? | ? | |
| Net Charge-Offs | 1508 | — | ? | `FF_NCO` | ? | ? | |
| Total Loans | 1509 | `LoansAndLeasesReceivableNetReportedAmount` | `tll` | `FF_LOANS_TOT` | ? | ? | |
| Total Deposits | 1510 | — | `tdsa` (savings) | `FF_DEPOSITS_TOT` | ? | ? | |
| Tier 1 Capital | 1511 | — | ? | `FF_TIER1_CAP` | ? | ? | |
| Tier 1 Capital Ratio | 1512 | — | ? | `FF_TIER1_RATIO` | ? | ? | |
| CET1 (Common Equity Tier 1) | 1513 | — | ? | `FF_CET1` | ? | ? | |
| Risk-Weighted Assets | 1514 | — | ? | `FF_RWA` | ? | ? | |
| Efficiency Ratio | 1515 | — | ? | `FF_EFFICIENCY_RATIO` | ? | ? | |

### 2.6 Industry-specific — Insurance

| Canonical concept | item_id | us-gaap | Compustat (FS template) | FactSet (FF_INS) | CIQ | Notes |
|---|---|---|---|---|---|---|
| Premiums Earned | 1601 | `PremiumsEarnedNet` | `pncia` `[unverified]` | `FF_PREM_EARNED` | `IQ_PREMIUMS_EARNED` (dataItemId=2010) | |
| Premiums Written | 1602 | — | ? | `FF_PREM_WRITTEN` | ? | |
| Loss Reserves | 1603 | — | `losres` | `FF_LOSS_RESERVE` | ? | |
| Insurance Benefits Paid | 1604 | — | `benefits` | ? | ? | |
| Unpaid Claim Liability | 1605 | — | `ucl` | ? | ? | |
| Loss Ratio | 1606 | — | ? | `FF_LOSS_RATIO` | `IQ_LOSS_RATIO` (dataItemId=2030) | |
| Expense Ratio | 1607 | — | ? | `FF_EXP_RATIO` | ? | |
| Combined Ratio | 1608 | — | ? | `FF_COMB_RATIO` | `IQ_COMBINED_RATIO` (dataItemId=2050) | |
| Investment Portfolio | 1609 | — | ? | `FF_INVEST_PORT` | ? | |
| Insurance Float | 1610 | — | ? | `FF_FLOAT` | ? | |

### 2.7 Industry-specific — REITs

REIT FFO/AFFO are Nareit definitions, not in core us-gaap. Source: [`datasets/fundamentals_us_equities.md`](../datasets/fundamentals_us_equities.md) §3.6 (FF_REIT), §8.7 gotcha #2 (REIT us-gaap coverage incomplete).

| Canonical concept | item_id | us-gaap | FactSet (FF_REIT) | CIQ | Notes |
|---|---|---|---|---|---|
| Funds From Operations (FFO) | 1701 | `nareit:FundsFromOperations` (custom extension typical) | `FF_FFO` | `IQ_FFO` (dataItemId=1366) | Nareit-defined; us-gaap incomplete. |
| FFO per Share | 1702 | derived | `FF_FFO_PS` | ? | |
| Adjusted FFO (AFFO) | 1703 | extension | `FF_AFFO` | `IQ_AFFO` (dataItemId=1367) | |
| AFFO per Share | 1704 | derived | `FF_AFFO_PS` | ? | |
| Net Operating Income (NOI) | 1705 | extension | `FF_NOI` | ? | |
| Same-Store NOI | 1706 | extension | `FF_NOI_SAME_STORE` | ? | |
| Occupancy Rate (%) | 1707 | extension | `FF_OCCUPANCY` | ? | |
| Rent per Square Foot | 1708 | extension | `FF_RENT_PSF` | ? | |
| Gross Leasable Area | 1709 | extension | `FF_GLA` | ? | |
| Net Asset Value per Share | 1710 | — | `FF_NAV` | ? | Also used in CEFs. |
| Capitalisation Rate | 1711 | — | `FF_CAP_RATE` | ? | |
| FFO Payout Ratio | 1712 | — | `FF_FFO_PAYOUT` | ? | |

### 2.8 Vintage / temporal fields

These are not line items but the dimensional axes every fundamentals fact carries. From [`datasets/fundamentals_us_equities.md`](../datasets/fundamentals_us_equities.md) §9 vintage table.

| Canonical concept | Compustat | FactSet | Bloomberg | CIQ | us-gaap / SEC | Notes |
|---|---|---|---|---|---|---|
| period_end (fiscal) | `datadate` | `ff_fp_end` | `FA_PERIOD_END_DATE` | `ciqFinPeriod.periodEndDate` | `dei:DocumentPeriodEndDate` | |
| report_date (8-K rdq) | `rdq` | `ff_eps_rpt_date` | — | filingDate where `collectionTypeId=3` | 8-K Item 2.02 acceptedDate | Market-knowable event. |
| filing_date (10-Q/K) | `fdate` | `ff_report_date` | `FA_FILING_DATE` | filingDate where `collectionTypeId=1` | 10-Q/K acceptedDate | |
| knowledge_from | `ldate` / `srcdate` | `ff_fe_date` | `FA_KNOWN_AS_OF_DATE` | `effectiveDate` | ats-eqt ingest timestamp | Bitemporal axis. |
| pdate (preliminary) | `pdate` | (subsumed in `ff_fe_date`) | — | filingDate `collectionTypeId=3` | 8-K accept | Snapshot/PIT only. |
| restated flag | `datafmt='RESTATED'` | `ff_restated` / `ff_restate_seq` | `FA_RESTATEMENT_SEQ` | `restatementTypeId` | (no us-gaap tag) | |
| as-filed vs std | `datafmt='STD'` | `ff_filing_status='OR'` | `FA_FILING_STATUS='OR'` | `collectionTypeId=1` | accession diff vs prior | |

---

## 3. Master cross-vendor table — Estimates

Source: [`datasets/estimates.md`](../datasets/estimates.md) §2 (IBES), §3 (FactSet Estimates), §4 (Bloomberg BEst), §5 (CIQ Estimates), §6 (Visible Alpha).

### 3.1 Measure-level mapping

| Canonical concept | item_id | IBES MEASURE | FactSet FE_* item | Bloomberg BEst | CIQ dataItemId / mnemonic | Visible Alpha tag | Notes |
|---|---|---|---|---|---|---|---|
| EPS — Diluted Normalized | 2001 | `EPS` (pdf='D') | `EPS` | `BEST_EPS` | 100180 / `IQ_EST_EPS_NORM` | line-item: `eps_diluted_norm` | Headline analyst forecast. |
| EPS — Diluted GAAP | 2002 | `NER` (when reported separately) | `EPS_GAAP` `[unverified]` | `BEST_EPS_GAAP` | 100181 / `IQ_EST_EPS_GAAP` | `eps_gaap` | |
| EPS — Basic | 2003 | `EPS` (pdf='P') | `EPS_BASIC` `[unverified]` | ? | ? | ? | |
| EPS — Beginning of Period | 2004 | — | — | `BEST_EPS_BEG` | ? | — | Locked at period start; useful for surprise. |
| Sales / Revenue | 2005 | `SAL` (`REV` in some files) | `SALES` | `BEST_SALES` | 100182 / `IQ_EST_REV` | `revenue` | |
| EBITDA | 2006 | `EBT` (per IBES `MEASURE` codes) | `EBITDA` | `BEST_EBITDA` | 100183 / `IQ_EST_EBITDA` | `ebitda` | |
| EBIT | 2007 | `EBI` | `EBIT` | ? | 100184 / `IQ_EST_EBIT` | `ebit` | |
| Operating Profit | 2008 | `OPR` / `OPS` | `OPR_PRO` | ? | 100186 | `op_inc` | |
| Net Income | 2009 | `NET` (`INC` in some files) | `NET_INC` | `BEST_NET_INC` | 100185 | `net_inc` | |
| Cash Flow per Share | 2010 | `CPS` | `CFPS` | ? | 100188 | — | |
| Capex | 2011 | `CPX` | `CAPEX` | `BEST_CAPEX` | 100189 | `capex` | |
| Free Cash Flow | 2012 | — | `FCF` | `BEST_FCF` `[unverified]` | 100190 | `fcf` | |
| FCF per Share | 2013 | — | `FCFPS` | ? | ? | — | |
| Dividends per Share | 2014 | `DPS` | `DPS` | `BEST_DPS` | 100191 | `dps` | |
| Book Value per Share | 2015 | `BPS` | `BPS` | `BEST_BPS` | 100192 | `bps` | |
| Gross Profit | 2016 | `GPS` (per-share) / `GRM` (margin) | ? | ? | 100187 | `gross_profit` | |
| Gross Margin | 2017 | `GRM` | `GPM` | ? | ? | `gross_margin` | |
| Operating Margin | 2018 | `OPM` | `OPM` | ? | ? | `op_margin` | |
| Effective Tax Rate | 2019 | — | ? | ? | 100193 | `tax_rate` | |
| Return on Equity | 2020 | `ROE` | `ROE` | ? | 100194 | — | |
| Return on Assets | 2021 | `ROA` | `ROA` | ? | 100195 | — | |
| Net Asset Value (REIT/CEF) | 2022 | `NAV` | `NAV` | ? | ? | — | |
| Funds From Operations (REIT) | 2023 | `FFO` | `FFO` | ? | ? | — | |
| Long-Term Growth | 2024 | `0` FPI / measure `LTG` | `LTG` | `BEST_LT_GROWTH` | `IQ_LTG` (estimatePeriodType=20) | — | 3–5y EPS growth %. |
| Target Price (12m) | 2025 | (separate `ptgdet` file) | `TARGET_PRICE` | `BEST_TARGET_PRICE` | ? | — | |
| Recommendation | 2026 | `ireccd` (1–5, in `recddet`) | `RECOMMENDATION` | `BEST_REC` / `BEST_REC_RAW` / `BEST_ANALYST_RATING` | ? | — | IBES: 1=Strong Buy, 5=Sell. Bloomberg `BEST_ANALYST_RATING`: 5=Strong Buy, 1=Strong Sell — **inverse**. |
| Recommendation count (Buys) | 2027 | derived in `recdsum` | ? | `BEST_RECS_BUYS` | ? | — | |
| Recommendation count (Holds) | 2028 | derived | ? | `BEST_RECS_HOLDS` | ? | — | |
| Recommendation count (Sells) | 2029 | derived | ? | `BEST_RECS_SELLS` | ? | — | |
| Number of Estimators | 2030 | `numest` (summary) | `num_est` | `BEST_EST_NUM` / `BEST_EPS_NUMEST` | ? | — | |
| StDev of Estimates | 2031 | `stdev` (summary) | `stdev_est` | `BEST_EPS_STDEV` | ? | — | |
| High / Low Estimate | 2032 | `highest` / `lowest` | `high_est` / `low_est` | `BEST_EPS_HIGH` / `BEST_EPS_LOW` | ? | — | |
| Surprise % | 2033 | (actual − consensus)/abs(consensus) | `surprise_pct` | `BEST_EPS_SURP_LAST_QTR` `[unverified]` | ? | — | |
| Beat/Meet/Miss flag | 2034 | derived | `beat_meet_miss` | derived | ? | — | |
| Forward P/E | 2035 | — | derived | `BEST_PE_RATIO` | ? | — | Forward P/E = price / BEST_EPS. |
| Same-Store Sales (KPI) | 2036 | `SSS` (IBES KPI feed) | `SAME_STORE_SALES` | ? | ? | `same_store_sales` | Industry KPI. |
| ARPU (Telecom KPI) | 2037 | `ARPU` (KPI feed) | `ARPU` | ? | ? | `arpu` | |
| Subscribers (Telecom/Streaming KPI) | 2038 | `SUBSC` (KPI feed) | `SUBSCRIBERS` | ? | ? | `subscribers` | |
| Load Factor (Airline KPI) | 2039 | (KPI feed) | `LOAD_FACTOR` | ? | ? | — | |
| ASK / RPK (Airline KPI) | 2040 | (KPI feed) | `ASK` / `RPK` | ? | ? | — | |
| Production — Oil (E&P KPI) | 2041 | (KPI feed) | `PROD_OIL` | ? | ? | — | |
| Production — Gold (Miner KPI) | 2042 | (KPI feed) | `PROD_GOLD` | ? | ? | — | |
| AUM (Asset Manager KPI) | 2043 | (KPI feed) | `AUM` | ? | ? | — | |
| NOI (REIT KPI) | 2044 | (KPI feed) | `NOI` | ? | ? | — | |

### 3.2 Forecast Period Indicator (FPI) / Period type — cross-vendor

Source: [`datasets/estimates.md`](../datasets/estimates.md) §2.4 (IBES FPI), §3.3 (FactSet period codes), §4.3 (Bloomberg BEST_FPERIOD_OVERRIDE), §5.4 (CIQ estimatePeriodType).

| Canonical period | IBES `fpi` | FactSet `fe_per_rel` | Bloomberg `BEST_FPERIOD_OVERRIDE` | CIQ `estimatePeriodTypeId` / mnemonic | Notes |
|---|---|---|---|---|---|
| Current fiscal year (FY0) | `1` | `FY1` | `1FY` (current) | 1 / `IQ_FY1` | FactSet/Bloomberg `FY1`/`1FY` mean the **current** FY (next reporting); IBES `fpi='1'` likewise = current. |
| Next fiscal year (FY+1) | `2` | `FY2` | `2FY` | 2 / `IQ_FY2` | |
| FY+2 | `3` | `FY3` | `3FY` | 3 / `IQ_FY3` | |
| FY+3 | `4` | `FY4` | `4FY` | 4 / `IQ_FY4` | |
| FY+4 | `5` | `FY5` | `5FY` | 5 / `IQ_FY5` | |
| Current fiscal quarter (FQ0) | `6` | `FQ1` | `1FQ` | 10 / `IQ_FQ1` | |
| Next fiscal quarter (FQ+1) | `7` | `FQ2` | `2FQ` | 11 / `IQ_FQ2` | |
| FQ+2 | `8` | `FQ3` | `3FQ` | ? / `IQ_FQ3` | |
| FQ+3 | `9` | `FQ4` | `4FQ` | ? / `IQ_FQ4` | |
| Current semi-annual | `A` | `FS1` | `1FS` `[unverified]` | 5 / `IQ_FS1` `[unverified]` | |
| Next twelve months (NTM) | `T` (summary only) | `NTM` | `1NTM` `[unverified]` | 30 / `IQ_NTM` | |
| Last twelve months (LTM) | — | `LTM` | `1TY` (trailing year) | 9 (in CIQ Fin) `[unverified]` | LTM is computed; not a forecast. |
| Current calendar year | — | `GY1` | `1GY` | 6 / `IQ_CY1` | |
| Long-Term Growth (3–5y EPS) | `0` | `LTG` | `BEST_LT_GROWTH` (field) | 20 / `IQ_LTG` | |
| Calendar year (vs fiscal) | — | `GY1` | `##BC` blended | 11 / `IQ_CY` | Used to normalise across companies with different fiscal year-ends. |

### 3.3 Date / vintage fields for estimates

| Canonical concept | IBES | FactSet | Bloomberg | CIQ | Notes |
|---|---|---|---|---|---|
| Announcement date (broker published) | `anndats` | `estimate_date` | (intraday on Terminal) | `effectiveDate` (for detail) | The earliest moment the market could know. |
| Activation date (vendor ingested) | `actdats` | (same as estimate_date) | — | `effectiveDate` | |
| Revision date (last confirmed unchanged) | `revdats` | `revision_flag` history | — | `toDate` (closing of prior row) | The Tilburg revdats-bump gotcha. |
| Period end being forecast | `fpedats` | `fe_per_end_date` | implicit in FPERIOD | `periodEndDate` | |
| Actual reported value (when known) | `actual` in `actu_*` | `actual_value` | (separate ERN field) | `actualValue` in CIQ Actuals | |
| Actual announcement date | `anndats_act` | `actual_release_date` | — | filingDate of corresponding fundamentals row | |
| Stop flag | `stop` file | `estimate_status='S'` | — | `estimateStatus` `[unverified]` | When analyst drops coverage. |
| Currency of estimate | `currency` / `estcur` | `estimate_currency` | `EQY_FUND_CRNCY` | `currencyId` | |

---

## 4. Master cross-vendor table — Corporate Actions

Source: [`datasets/corporate_actions.md`](../datasets/corporate_actions.md) §3 (CRSP DISTCD / DLSTCD), §4 (FactSet Adjustments), §5 (Bloomberg DVD/EQY_SPLIT), §11 (DTCC CAEV codes), §14 (Compustat fields).

| Action type | CRSP `DISTCD` | CRSP `DLSTCD` | Bloomberg field | FactSet field | LSEG/Refinitiv field | DTCC CAEV code | us-gaap / SEC trigger |
|---|---|---|---|---|---|---|---|
| Regular quarterly cash dividend | `1212` | — | `DVD_HIST_ALL` (Frequency='Regular Cash') | `FSYM_CA` action_type='DVCA' `[unverified]` | Datastream `AF` + WS-event | `DVCA` | 8-K (Item 8.01 sometimes); not strictly required for normal cash div |
| Semi-annual cash dividend | `1222` | — | `DVD_HIST_ALL` (Frequency='Semi') | — | — | `DVCA` | — |
| Annual cash dividend | `1232` | — | `DVD_HIST_ALL` (Frequency='Annual') | — | — | `DVCA` | — |
| Monthly cash dividend | `1242` | — | `DVD_HIST_ALL` (Frequency='Monthly') | — | — | `DVCA` | — |
| Special / non-recurring cash dividend | `1262` | — | `DVD_HIST_ALL` (Frequency='Special Cash') | — | — | `DVCA` | 8-K Item 8.01 (issuer typical practice) |
| Final cash dividend (terminal) | `1272` | — | — | — | — | `DVCA` (final) | 8-K typically; precedes delisting |
| Return of capital cash distribution | `1282` | — | `DVD_HIST_ALL` (Type='Return of Capital') | — | — | `DVCA` (sub-type ROC) | Schedule M-3 / Form 1099-DIV |
| Stock dividend — same class | `3232` | — | `DVD_HIST_ALL` (Type='Stock Dividend') | — | — | `DVSE` | 8-K Item 8.01 |
| Stock dividend — different class | `3245` | — | — | — | — | `DVSE` | 8-K |
| Forward stock split | `5523` / `3522` | — | `EQY_SPLIT_HIST`, `SPLIT_FACTOR` >1 | `FSYM_CA` action_type='SPLF' `[unverified]` | Datastream `AF` step change | `SPLF` | 8-K Item 5.03 (amend articles) |
| Reverse stock split | `5531` / `3565` | — | `EQY_SPLIT_HIST`, `SPLIT_FACTOR` <1 | — | — | `SPLR` `[unverified]` | 8-K |
| Spinoff (same parent class) | `7525` | — | `DVD_HIST_ALL` (Type='Spinoff') | `FSYM_CA` action_type='SOFF' | — | `SOFF` | 8-K Item 2.01 / Form 10 / Form 8937 |
| Spinoff (new shares of new entity) | `7232` | — | `DVD_HIST_ALL` (Type='Spinoff') | — | — | `SOFF` | 8-K + Form 10 + Form 8937 |
| Merger — cash | `8225` | `202` / `220` (tender) | `M_A_STATUS`, `M_A_TRANS_VALUE` | `FSYM_CA` action_type='MRGR' | — | `MRGR` / `TEND` | Form S-4, 14D-9, 8-K |
| Merger — stock | `8232` | `201` / `231` | `M_A_STATUS` | — | — | `MRGR` | Form S-4, 8-K |
| Merger — mixed cash + stock | `8245` | `203` | `M_A_STATUS` | — | — | `MRGR` | — |
| Tender offer | — | `220` / `231` / `241` | `TENDER_FLAG`, `ACQ_ANNCMT_DT` | — | — | `TEND` | Schedule TO, 14D-9 |
| Conversion (pfd→common) | `8525` | `300` / `301` / `331` | — | — | — | — | 8-K Item 3.02 |
| Name change | — | (kept; new row in `dsenames`) | (no event field; reflected in name history) | `FSYM-S` lineage | Datastream `NAME` | `CHAN` | 8-K Item 5.03(b) |
| Ticker change | — | (kept; new row in `dsenames`) | — | `FSYM-L` lineage (new listing ID) | — | `CHAN` | 8-K Item 5.03 |
| IPO | — | — | `EQY_INIT_PO_DT` / `IPO_HIST` `[unverified]` | — | — | — | S-1 / S-11 |
| Secondary offering | — | — | — | — | — | `RHTS` / `BIDS` | S-3 / 424B |
| Stock repurchase / buyback (Dutch auction) | — | — | (separate `EQY_SH_OUT` decline) | — | — | `BIDS` | 8-K (when material), 10-Q |
| Delisting — voluntary | — | `510` / `513` | — | — | — | — | 25-NSE (notice of voluntary delisting) |
| Delisting — bankruptcy Chapter 11 | — | `560` / `561` / `574` | — | — | — | `LIQU` / `WRTH` | Form 15 (deregistration) |
| Delisting — bankruptcy Chapter 7 / dissolution | — | `562` | — | — | — | `LIQU` / `WRTH` | Form 15 |
| Delisting — financial-standards breach | — | `501` / `502` / `503` / `504` / `505` | — | — | — | — | NYSE/Nasdaq Rule 802 |
| Delisting — non-filer | — | `570` / `580` / `584` | — | — | — | — | Form 15-12B / 12G |
| Liquidation | — | `400` / `410` / `420` | — | — | — | `LIQU` | Form 15 |
| Exchanged for another security | — | `300` | — | — | — | — | Form S-4 / Schedule 14D-9 |
| Acquired by foreign parent | — | `210` | — | — | — | `MRGR` | F-4 / Schedule 14D-9 |
| Privately-negotiated acquisition | — | `261` | — | — | — | `MRGR` (sub-type) | 8-K |

**Caveats** (from [`datasets/corporate_actions.md`](../datasets/corporate_actions.md) §10):
- FactSet `FSYM_CA` schema is gated behind subscriber login; entries above marked `[unverified]` are inferred.
- DTCC CAEV codes are the ISO 20022 CAEV enumeration (4-char); see §11 of the source doc for the full ~80-code list.
- Compustat doesn't carry a granular event table; price/share adjustment factors `AJEXM` / `TRT1` etc. roll up the events fiscal-period at a time.

---

## 5. Master cross-vendor table — Pricing

Source: [`datasets/pricing_market_data.md`](../datasets/pricing_market_data.md) §3.1 (CRSP), §3.2 (Compustat CCM), §3.3 (FactSet), §3.4 (Bloomberg), §3.5 (LSEG/Refinitiv), §3.7 (Polygon), §3.8 (Tiingo), §3.10 (Yahoo).

| Concept | CRSP DSF | Compustat CO_SECD / SEC_DPRC | FactSet `/factset-prices` | Bloomberg `PX_*` | LSEG Datastream | Polygon `/v2/aggs` | Tiingo | Yahoo Finance |
|---|---|---|---|---|---|---|---|---|
| Open | `OPENPRC` | `PRCOD` | `p_price_open` | `PX_OPEN` | `PO` | `o` | `open` | `Open` |
| High | `ASKHI` (special semantic when no trade) | `PRCHD` | `p_price_high` | `PX_HIGH` | `PH` | `h` | `high` | `High` |
| Low | `BIDLO` (special semantic when no trade) | `PRCLD` | `p_price_low` | `PX_LOW` | `PL` | `l` | `low` | `Low` |
| Close (raw) | `PRC` (sign-coded: neg = midquote) | `PRCCD` | `p_price` | `PX_LAST` | `UP` (unadjusted) | `c` | `close` | `Close` |
| Adjusted Close (split + div) | `PRC / CFACPR` (computed) | `PRCCD / AJEXDI × TRFD` | `p_price_adj` | `(via DPDF toggle)` | `P` (split-adj) / `RI` (div+split) | (`adjusted=true` flag, split only) | `adjClose` | `Adj Close` (known broken for spinoffs) |
| Volume | `VOL` (shares) | `CSHTRD` | `p_volume` | `PX_VOLUME` | `VO` | `v` | `volume` | `Volume` |
| VWAP | — | — | — | (derived) | — | `vw` | — | — |
| Bid (closing) | `BID` | — | — | `PX_BID` | — | — | — | — |
| Ask (closing) | `ASK` | — | — | `PX_ASK` | — | — | — | — |
| Bid Size | — (in TAQ only) | — | — | `PX_BID_SIZE` `[unverified]` | — | — | — | — |
| Ask Size | — (in TAQ only) | — | — | `PX_ASK_SIZE` `[unverified]` | — | — | — | — |
| Market Cap | `PRC × SHROUT` (computed) | `MKVALT` / `PRCCD × CSHOC` | `FF_MKT_CAP_TOT` | `CUR_MKT_CAP` / `HISTORICAL_MARKET_CAP` | `MV` | — | — | (derived) |
| Shares Outstanding | `SHROUT` (thousands) | `CSHOC` (filing-driven; sticky) | (separate `/shares` endpoint) | `EQY_SH_OUT` / `EQY_SH_OUT_REAL` | `NOSH` (millions) | — | (in `meta`) | — |
| Float | — | — | — | `EQY_FLOAT` `[unverified]` | — | — | — | — |
| Total Return (daily) | `RET` | `(TRI_t / TRI_{t-1}) - 1` via `TRFD` | `p_total_return` | `DAY_TO_DAY_TOT_RETURN_GROSS_DVDS` | `(RI_t/RI_{t-1})-1` | — | — | — |
| Total Return Index level | — (computed) | `TRI = (PRCCD/AJEXDI)×TRFD` | — | `TOT_RETURN_INDEX_GROSS_DVDS` | `RI` | — | — | — |
| Daily ex-dividend return | `RETX` | — | — | — | — | — | — | — |
| Cum Adj Factor — price | `CFACPR` | `AJEXDI` (per-day) | `p_split_factor × p_div_factor` | `(via override)` | implicit (P series) | — | (baked in) | (baked in) |
| Cum Adj Factor — shares | `CFACSHR` | (computed) | `s_split_factor` | — | — | — | — | — |
| Per-day dividend amount | (in `dseall`/`dsedist`) | `DVI` | (separate `/dividends` endpoint) | `DVD_HIST_ALL` table | (via `AF` step change) | — | `divCash` (in `meta`) | `Dividend` column |
| Per-day split ratio | (in `dseall`/`dsedist`) | (in `sec_dprc`) | (separate `/splits` endpoint) | `EQY_SPLIT_HIST` table | (via `AF` step change) | — | `splitFactor` (in `meta`) | (in adj close) |
| Delisting return | `DLRET` (in `dsedelist`) | (no field — fundamentals only stop) | — | — | — | — | — | — |
| Delisting code | `DLSTCD` | (terminal NULL row) | — | — | — | — | — | — |
| Number of trades | `NUMTRD` (NASDAQ historic) | — | — | — | — | `n` | — | — |
| Exchange code | `HEXCD` / `EXCHCD` | `EXCHG` | (in symbology) | (in yellow-key suffix) | (in RIC suffix) | — | — | — |

**Adjustment-quality caveats** (from [`datasets/pricing_market_data.md`](../datasets/pricing_market_data.md) §0.2):
- CRSP separates `CFACPR` (price) from `CFACSHR` (shares); they diverge only for stock dividends in different-class shares.
- Tiingo/Yahoo bake split + div into a single `adjClose`; Polygon's `adjusted=true` applies splits only.
- LSEG Datastream's `P` is **already** split-adjusted; the unadjusted is `UP`. Opposite convention from CRSP. Naive cross-vendor joins on "price" silently misalign on every split.
- Yahoo Finance's spinoff handling is documented broken (see [`datasets/corporate_actions.md`](../datasets/corporate_actions.md) §0 finding).

---

## 6. Master cross-vendor table — Identifiers

Source: [`schemas/data_models_and_methodology.md`](data_models_and_methodology.md) §D.2 (major identifier systems table), [`sources/public_data_sources.md`](../sources/public_data_sources.md) (GLEIF, OpenFIGI, PermID, CIK cards), [`vendors/factset.md`](../vendors/factset.md) §4.5 (FSYM hierarchy), [`vendors/refinitiv_bloomberg.md`](../vendors/refinitiv_bloomberg.md) §2.1 (PermID), §7.2 (FIGI levels), [`datasets/13f_holdings.md`](../datasets/13f_holdings.md) Part B (CUSIP licensing).

| Identifier | Format | Vendor of record | Open / Licensed | Persistent across (action) | Mapping availability |
|---|---|---|---|---|---|
| **CIK** | 10-digit integer | SEC EDGAR | **Open** (public domain) | name change, ticker change, merger (sometimes reassigned); occasional CIK reassignment on absorption | Direct from `https://www.sec.gov/files/company_tickers.json`; EDGAR Submissions API |
| **LEI** | 20-char ISO 17442 | GLEIF (free) | **Open** (LEI Data Terms; redistribution OK) | name change, ownership change (Level-2 self-reported) | GLEIF API `https://api.gleif.org/api/v1/lei-records`; daily concatenated CDF zip |
| **FIGI** | 12-char alphanumeric | Bloomberg (OMG-registered) | **Open MIT-licensed** | ticker change, exchange relisting | OpenFIGI API `https://api.openfigi.com/v3/mapping`; free, 25 req/6s anon, 1000 req/6s auth |
| **Composite FIGI** | 12-char (BBG…) | Bloomberg | Open MIT | country-level rollup of venue FIGIs | OpenFIGI |
| **Share Class FIGI** | 12-char | Bloomberg | Open MIT | global rollup across composite FIGIs | OpenFIGI |
| **ISIN** | 12-char (country code + 9 nums) | National numbering agencies | Semi-open (varies by NNA) | ticker change; **not** persistent across share-class reclassification | OpenFIGI returns ISIN as alias; LEI Mapping (ISIN-LEI free certified file at GLEIF) |
| **CUSIP** | 9-char | CUSIP Global Services (FactSet/ABA) | **Proprietary**; redistribution license required ($25k–$500k/yr `[unverified]`) | not persistent across share-class reclassification | OpenFIGI accepts as input |
| **SEDOL** | 7-char | LSE | Proprietary | UK-centric; not persistent across class changes | OpenFIGI |
| **Bloomberg Global ID** | (now = FIGI) | Bloomberg | Open MIT | renamed to FIGI in 2014 | OpenFIGI |
| **Bloomberg Ticker** | e.g. `AAPL US Equity` | Bloomberg | Proprietary; license under Terminal/DL+ | not persistent across ticker change | Internal Bloomberg |
| **PermID — Organization** | URI `https://permid.org/1-…` | LSEG/Refinitiv | **Open** (free tier, redistribution conditional; LSEG can change terms) | name change, ownership change | PermID API; ~13M orgs |
| **PermID — Instrument** | URI | LSEG | Open (free tier) | ticker change | PermID API; ~550K equity instruments |
| **PermID — Quote** | URI | LSEG | Open (free tier) | listing change | ~3M equity quotes |
| **PermID — Person** | URI | LSEG | Open | — | PermID API |
| **RIC** | e.g. `IBM.N`, `MSFT.O` | LSEG | Proprietary | not persistent across exchange transfer | LSEG Symbology API |
| **GVKEY** | 6-char zero-padded | Compustat (S&P) | Proprietary | persistent across ticker / name change | WRDS CCM link table `ccmxpf_lnkhist` |
| **PERMNO** | int (security-level) | CRSP | Proprietary | persistent across ticker / name change / exchange transfer; reused **never** | CCM link `ccmxpf_lnkhist` |
| **PERMCO** | int (company-level) | CRSP | Proprietary | persistent | CCM link |
| **FSYM-E (Entity)** | 8-char alphanum | FactSet | Proprietary | persistent across M&A (survivor inherits target) | FactSet Symbology API |
| **FSYM-S (Security)** | 8-char `-S` | FactSet | Proprietary | persistent across exchange relisting | FactSet Symbology |
| **FSYM-R (Regional)** | 8-char `-R` | FactSet | Proprietary | one FSYM-S can have multiple FSYM-R (ADR + local + dual-listing) | FactSet Symbology |
| **FSYM-L (Listing)** | 8-char `-L` | FactSet | Proprietary | new FSYM-L on ticker change; FSYM-S/-R stable | FactSet Symbology |
| **FactSet Entity ID** | (= FSYM-E or `factset_entity_id`) | FactSet | Proprietary | persistent | FactSet Entity API |
| **CIQ Company ID** | int | S&P Capital IQ | Proprietary | persistent | `ciqCompany` table; WRDS Capital IQ overview |
| **CIQ Security ID** | int | S&P CIQ | Proprietary | persistent | `ciqSecurity` |
| **CIQ Trading ID** | int | S&P CIQ | Proprietary | persistent within exchange listing | `ciqTradingItem` |
| **CIQ Estimate Analyst ID** | int | S&P CIQ | Proprietary | persistent within subscription | `ciqEstimateAnalyst` |
| **OpenCorporates ID** | URI `https://opencorporates.com/companies/…/<num>` | OpenCorporates | Open (CC-BY-SA where data permits; some jurisdictions restricted) | persistent within registry | OpenCorporates API |
| **Wikidata Q-ID** | `QNNNNN` | Wikidata Foundation | **Open** (CC0) | persistent; user-edited | Wikidata SPARQL / API |
| **IBES Ticker** | 6-char alphanum | LSEG (IBES) | Proprietary | NOT exchange ticker | WRDS ICLINK macro (cusip-first, then ticker-name fuzzy) |
| **NPORT Series ID** | `S…` 9-char | SEC | Open | fund series-level | EDGAR |

**Stability ordering** (from [`schemas/data_models_and_methodology.md`](data_models_and_methodology.md) §D.5 + [`datasets/13f_holdings.md`](../datasets/13f_holdings.md) §B.3):

```
MOST STABLE    LEI ≈ FIGI (share-class) > FIGI (composite) > CIK > GVKEY ≈ PERMNO > ISIN > CUSIP > SEDOL > Ticker
                                                                                                              LEAST STABLE
```

ats-eqt's hub-and-spoke spine: **CIK (issuer for US-filings) + LEI (legal-entity globally) + FIGI (security globally)** as the public spine; CUSIP, SEDOL, RIC, FSYM, GVKEY, PERMNO as time-bounded internal aliases never exposed on the public API.

---

## 7. Master cross-vendor table — Industry Classifications

Source: [`vendors/refinitiv_bloomberg.md`](../vendors/refinitiv_bloomberg.md) §2.3 (TRBC), §4.5 (BICS), [`vendors/factset.md`](../vendors/factset.md) §4.5 (RBICS), [`sources/public_data_sources.md`](../sources/public_data_sources.md) (NAICS, SIC), [`datasets/fundamentals_us_equities.md`](../datasets/fundamentals_us_equities.md) §2.1.1 (Compustat industry columns).

| Taxonomy | Depth (levels) | Leaf count | Vendor of record | Open / Licensed | Compustat field | FactSet field | Bloomberg field | LSEG/Refinitiv field |
|---|---|---|---|---|---|---|---|---|
| **GICS** | 4 (Sector → Industry Group → Industry → Sub-Industry) | ~158 sub-industries (2023 revision) | MSCI + S&P | **Proprietary** ($) | `gsector` (2-digit), `ggroup` (4), `gind` (6), `gsubind` (8) | `factset_gics_*` `[unverified]` | `EQY_FUND_IND_GICS` / `GICS_INDUSTRY_NAME` | (available via Workspace as licensed overlay) |
| **ICB** | 4 (Industry → Supersector → Sector → Subsector) | ~173 subsectors | FTSE Russell | Proprietary | — | — | `ICB_INDUSTRY` `[unverified]` | (Datastream `INDUSTRY` series in some files) |
| **TRBC** | 5 (Economic Sector → Business Sector → Industry Group → Industry → Activity) | ~157 activities (TRBC 2023) | LSEG | **Open via PermID** (free tier) — meaningful for open-data competitors | — | — | — | PermID API + Datastream `TRBC` mnemonic |
| **BICS** | 7 (Sector → Group → Industry → Sub-Industry … → Level 7) | ~2,000+ Level-7 nodes | Bloomberg | Proprietary | — | — | `BICS_LEVEL_*_SECTOR_NAME` | — |
| **RBICS** | 6 (Economy → Sector → Sub-Sector → Industry Group → Industry → Sub-Industry) | ~1,400 sub-industries (FactSet 14×6 matrix) | FactSet (Revere) | Proprietary | — | `factset_rbics_l*_id` / `_l*_name` (1..6) | — | — |
| **NAICS** | 6 (digit-depth) | ~1,070 6-digit codes (NAICS 2022) | US/CA/MX statistical agencies | **Open** (public domain) | `naics` (6-digit) | (in `factset_company`) | — | (in Datastream identifier set) |
| **SIC** | 4 (digit-depth) | ~1,000 4-digit codes | US BLS / SEC | **Open** | `sic` (4-digit) / `spcindcd` (S&P industry) / `spcseccd` (S&P sector) | — | — | — |
| **ISIC Rev 4** | 4 (digit-depth) | ~419 4-digit classes | UN Statistics Division | **Open** | — | — | — | — |
| **NACE Rev 2** | 4 (digit-depth) | ~615 4-digit classes | Eurostat | **Open** | — | — | — | — |
| **SASB Standards** | 11 sectors × 77 industries | 77 industries; 26 General Issue Categories | IFRS Foundation (post-2022 transition) | **Open** (SASB Standards freely downloadable) | — | — | — | — |
| **GHG Protocol Scope 3 categories** | 15 categories | 15 | WRI / WBCSD | **Open** | — | — | (per-category fields) | (per-category) |

**Coverage gap notes:**
- GICS depths and code lengths: 2 / 4 / 6 / 8 digit hierarchy.
- ICB recently updated (2023 revision); Compustat does not natively carry ICB.
- TRBC's openness via PermID is the only major free industry-classification fully tagged at the security level globally.

---

## 8. Master cross-vendor table — ESG

Source: [`datasets/esg_sustainability.md`](../datasets/esg_sustainability.md) §3 (MSCI), §6.1 (Bloomberg ESG fields), §7.5 (Refinitiv ESG Datastream mnemonics), §9 (Truvalue), §10 (CDP), §12.5 (ESRS datapoints), §12.6 (SFDR PAI), §12.14 (GHG Protocol).

Convention: ESG ratings disagree across vendors (Berg-Kölbel-Rigobon: 0.38–0.71 pairwise correlation). The table maps **raw underlying disclosed metrics** where possible; vendor opinion scores are listed at the bottom of the section.

| Canonical metric | item_id | ESRS datapoint family | SASB tag | TCFD pillar | MSCI Key Issue | Sustainalytics MEI | Bloomberg ESG | LSEG ESG mnemonic | Truvalue SASB cat | CDP question family |
|---|---|---|---|---|---|---|---|---|---|---|
| Scope 1 emissions (tCO2e) | 3001 | ESRS E1 | GHG Emissions | Metrics & Targets | Carbon Emissions | Carbon — Own Operations | `GHG_SCOPE_1` | (in `ENERP` Emissions category) | GHG Emissions | C6.1 |
| Scope 2 — location-based (tCO2e) | 3002 | ESRS E1 | GHG Emissions | Metrics & Targets | Carbon Emissions | Carbon — Own Operations | `GHG_SCOPE_2_LOCATION_BASED` | — | GHG Emissions | C6.3 (loc) |
| Scope 2 — market-based (tCO2e) | 3003 | ESRS E1 | GHG Emissions | Metrics & Targets | Carbon Emissions | Carbon — Own Operations | `GHG_SCOPE_2_MARKET_BASED` | — | GHG Emissions | C6.3 (mkt) |
| Scope 3 — total (tCO2e) | 3004 | ESRS E1 | GHG Emissions | Metrics & Targets | Carbon Emissions | Carbon — Products & Services | `GHG_SCOPE_3_TOTAL` | — | GHG Emissions | C6.5 |
| Scope 3 cat 1 — Purchased Goods & Services | 3005 | ESRS E1 | GHG Emissions | — | — | — | `GHG_SCOPE_3_PURCHASED_GOODS` `[unverified]` | — | — | C6.5 (cat 1) |
| Scope 3 cat 2 — Capital Goods | 3006 | ESRS E1 | GHG Emissions | — | — | — | `GHG_SCOPE_3_CAPITAL_GOODS` `[unverified]` | — | — | C6.5 (cat 2) |
| Scope 3 cat 3 — Fuel- & Energy-Related Activities | 3007 | ESRS E1 | GHG Emissions | — | — | — | `GHG_SCOPE_3_FUEL_ENERGY` `[unverified]` | — | — | C6.5 (cat 3) |
| Scope 3 cat 4 — Upstream Transportation | 3008 | ESRS E1 | GHG Emissions | — | — | — | `GHG_SCOPE_3_UPSTREAM_TRANS` `[unverified]` | — | — | C6.5 (cat 4) |
| Scope 3 cat 5 — Waste in Operations | 3009 | ESRS E1 | GHG Emissions | — | — | — | `GHG_SCOPE_3_WASTE_OPS` `[unverified]` | — | — | C6.5 (cat 5) |
| Scope 3 cat 6 — Business Travel | 3010 | ESRS E1 | GHG Emissions | — | — | — | `GHG_SCOPE_3_BIZ_TRAVEL` `[unverified]` | — | — | C6.5 (cat 6) |
| Scope 3 cat 7 — Employee Commuting | 3011 | ESRS E1 | GHG Emissions | — | — | — | `GHG_SCOPE_3_COMMUTING` `[unverified]` | — | — | C6.5 (cat 7) |
| Scope 3 cat 8 — Upstream Leased Assets | 3012 | ESRS E1 | GHG Emissions | — | — | — | `GHG_SCOPE_3_UPSTREAM_LEASED` `[unverified]` | — | — | C6.5 (cat 8) |
| Scope 3 cat 9 — Downstream Transportation | 3013 | ESRS E1 | GHG Emissions | — | — | — | `GHG_SCOPE_3_DOWN_TRANS` `[unverified]` | — | — | C6.5 (cat 9) |
| Scope 3 cat 10 — Processing of Sold Products | 3014 | ESRS E1 | GHG Emissions | — | — | — | `GHG_SCOPE_3_PROCESSING` `[unverified]` | — | — | C6.5 (cat 10) |
| Scope 3 cat 11 — Use of Sold Products | 3015 | ESRS E1 | GHG Emissions | — | — | — | `GHG_SCOPE_3_USE` `[unverified]` | — | — | C6.5 (cat 11) |
| Scope 3 cat 12 — End-of-Life | 3016 | ESRS E1 | GHG Emissions | — | — | — | `GHG_SCOPE_3_EOL` `[unverified]` | — | — | C6.5 (cat 12) |
| Scope 3 cat 13 — Downstream Leased Assets | 3017 | ESRS E1 | GHG Emissions | — | — | — | `GHG_SCOPE_3_DOWN_LEASED` `[unverified]` | — | — | C6.5 (cat 13) |
| Scope 3 cat 14 — Franchises | 3018 | ESRS E1 | GHG Emissions | — | — | — | `GHG_SCOPE_3_FRANCHISES` `[unverified]` | — | — | C6.5 (cat 14) |
| Scope 3 cat 15 — Investments | 3019 | ESRS E1 | GHG Emissions | — | Financing Environmental Impact | — | `GHG_SCOPE_3_INVESTMENTS` `[unverified]` | — | — | C6.5 (cat 15) |
| GHG intensity per revenue | 3020 | ESRS E1 | — | — | — | — | `GHG_INTENSITY_PER_SALES` | — | — | C6.10 |
| Energy consumption — total (MWh) | 3021 | ESRS E1 | Energy Management | — | — | — | `ENERGY_CONSUMPTION_TOTAL` | (in `ENRRP`) | Energy Management | C8.2 |
| % Renewable energy | 3022 | ESRS E1 | Energy Management | — | Opportunities in Renewable Energy | — | `RENEWABLE_ENERGY_CONSUMPTION_PCT` | — | — | C8.2d |
| Energy intensity per revenue | 3023 | ESRS E1 | — | — | — | — | `ENERGY_INTENSITY_PER_SALES` | — | — | — |
| Water withdrawal (m³) | 3024 | ESRS E3 | Water & Wastewater Mgmt | — | Water Stress | — | `WATER_WITHDRAWAL_TOTAL` | (in `ENRRP`) | Water Mgmt | W1.2 |
| Water discharge (m³) | 3025 | ESRS E3 | Water & Wastewater Mgmt | — | — | — | `WATER_DISCHARGE_TOTAL` | — | — | W1.2b |
| Water consumption (m³) | 3026 | ESRS E3 | — | — | — | — | `WATER_CONSUMPTION_TOTAL` | — | — | W1.2 |
| Water recycled (%) | 3027 | ESRS E3 | — | — | — | — | `WATER_RECYCLED_PCT` | — | — | — |
| Waste generated (tonnes) | 3028 | ESRS E5 | Waste & Hazardous Materials | — | Toxic Emissions & Waste | — | `WASTE_GENERATED_TOTAL` | — | Waste Mgmt | — |
| Hazardous waste (tonnes) | 3029 | ESRS E5 / E2 | Waste & Hazardous Materials | — | Toxic Emissions & Waste | — | `HAZARDOUS_WASTE_TOTAL` | — | — | — |
| Waste recycled (%) | 3030 | ESRS E5 | — | — | — | — | `WASTE_RECYCLED_PCT` | — | — | — |
| Total employees (headcount) | 3031 | ESRS S1 | Labor Practices | — | Labor Management | Own Workforce | `EMPLOYEES_TOTAL` | (in `SOWO`) | Labor | — |
| Employee turnover (%) | 3032 | ESRS S1 | Labor Practices | — | Labor Management | Own Workforce | `EMPLOYEE_TURNOVER_PCT` | — | — | — |
| % Female employees | 3033 | ESRS S1 | Employee Engagement & D&I | — | Labor Management | Own Workforce | `EMPLOYEES_FEMALE_PCT` | — | — | — |
| % Women in management | 3034 | ESRS S1 | Employee Engagement & D&I | — | Labor Management | Own Workforce | `WOMEN_IN_MGMT_PCT` | — | — | — |
| Training hours per FTE | 3035 | ESRS S1 | — | — | Human Capital Development | Own Workforce | `TRAINING_HOURS_PER_EMPLOYEE` | — | — | — |
| Lost-Time Injury Rate (LTIFR) | 3036 | ESRS S1 | Employee Health & Safety | — | Health & Safety | Own Workforce | `LOST_TIME_INJURY_RATE` | — | — | — |
| Fatalities (total) | 3037 | ESRS S1 | Employee Health & Safety | — | Health & Safety | Own Workforce | `FATALITIES_TOTAL` | — | — | — |
| Unadjusted gender pay gap (%) | 3038 | ESRS S1 (SFDR PAI 12) | — | — | — | — | (custom field) | — | — | — |
| % Board independent | 3039 | ESRS G1 | — | Governance | Board | Corporate Governance | `PCT_BOD_INDEPENDENT` | (in `CGVS`) | — | — |
| % Board female | 3040 | ESRS G1 (SFDR PAI 13) | — | Governance | Board | Corporate Governance | `PCT_BOD_FEMALE` | — | — | — |
| Board size | 3041 | ESRS G1 | — | Governance | Board | Corporate Governance | `BOARD_SIZE` | — | — | — |
| Chair / CEO same person | 3042 | ESRS G1 | — | Governance | Board | Corporate Governance | `CHAIRMAN_CEO_SAME_PERSON` | — | — | — |
| CEO Pay Ratio | 3043 | ESRS G1 (via DEF 14A) | — | — | Pay | Corporate Governance | `CEO_PAY_RATIO` | — | — | — |
| Say-on-Pay vote % support | 3044 | — | — | — | Pay | Corporate Governance | `SAY_ON_PAY_VOTE_PCT_FOR` | — | — | — |
| Audit Committee independence (%) | 3045 | ESRS G1 | — | Governance | Accounting | Corporate Governance | `AUDIT_COMMITTEE_INDEPENDENT_PCT` | — | — | — |
| Tax paid by jurisdiction | 3046 | ESRS G1 | Business Ethics | — | Tax Transparency | — | (custom field) | — | Business Ethics | — |
| Anti-corruption training (%) | 3047 | ESRS G1 | Business Ethics | — | Business Ethics | — | — | — | Business Ethics | — |
| Violations of UN Global Compact / OECD (SFDR PAI 10) | 3048 | ESRS S2/G1 | — | — | — | Norm-Based | — | — | — | — |
| Exposure to controversial weapons (%) | 3049 | — (SFDR PAI 14) | — | — | (Business Involvement) | — | — | — | — | — |
| Exposure to fossil fuel sector (%) | 3050 | ESRS E1 (SFDR PAI 4) | — | — | — | — | — | — | — | — |

**Vendor opinion-score fields (orthogonal to the raw metrics above):**

| Score | MSCI | Sustainalytics | Bloomberg | LSEG / Refinitiv | Notes |
|---|---|---|---|---|---|
| Headline rating / score | `IVA_COMPANY_RATING` (AAA-CCC) + `INDUSTRY_ADJUSTED_SCORE` (0-10) | `esg_risk_score` (0-100+) + `esg_risk_category` | `ES Score` (0-100) + `Disclosure Score` | `TRESGS` (0-100) | Sustainalytics lower = better; others higher = better |
| Environmental pillar | `ENVIRONMENTAL_PILLAR_SCORE` | (decomposed by Material Issue) | E sub-issue score | `ENSCORE` | |
| Social pillar | `SOCIAL_PILLAR_SCORE` | (decomposed) | S sub-issue score | `SOSCORE` | |
| Governance pillar | `GOVERNANCE_PILLAR_SCORE` | `governance_score` (since v3.1) | G score (separate) | `CGSCORE` | |
| Controversies | `CONTROVERSY_FLAG` (Red/Orange/Yellow/Green) + `CONTROVERSY_CASES_NUMBER` | `controversy_rating` 1-5 | (via field-level events) | `TRESGCCS` | |
| Combined ESG + Controversies | (handled within IVA) | (built into Unmanaged Risk) | — | `TRESGCS` / `ESGC` | LSEG asymmetric combination. |
| NLP-derived score | — | — | — | — | FactSet Truvalue: `truvalue_insight`, `truvalue_pulse`, `truvalue_momentum`, `truvalue_volume` per (entity × SASB category × date) |

---

## 9. Master cross-vendor table — Ownership

Source: [`datasets/13f_holdings.md`](../datasets/13f_holdings.md) Part A (EDGAR 13F XML), Part C (commercial vendors), and the supply-chain doc's ownership cross-reference.

| Concept | Form 13F (EDGAR) | Form N-PORT (EDGAR) | Form 4 (insider) | Schedule 13D/13G | FactSet Ownership | CIQ ciqOwnership* | Bloomberg HDS/PHDC | Refinitiv eMAXX / Lipper |
|---|---|---|---|---|---|---|---|---|
| Holder name | `coverPage/filingManager/name` | `<filerInfo>/<filer>/<credentials>/<cik>` → manager name lookup | `Reporting Owner Name` | `Item 1` of schedule | `holder_name` / `manager_name` | `ciqInstitution.institutionName` | (via HDS lookup, manager from BB Manager ID) | (eMAXX `institution_name`) |
| Holder CIK | `coverPage/filingManager/CIK` | `coverPage/filerInfo/filer/CIK` | `Reporting Owner CIK` | `Filer CIK` | (in master) | (in CIQ entity master) | (mapped) | (mapped) |
| Holder LEI | (optional cover page) | (in `<headerData>`) | — | — | `holder_lei` `[unverified]` | (in ciqEntity) `[unverified]` | (mapped) | — |
| Holder type (institution / fund / insider) | (manager type via SEC) | `<filerInfo>/regCategory` (RIC/RIA/etc.) | derived | derived | `holder_type` | `ciqInstitution.institutionType` | (in HDS) | (in eMAXX `holder_type`) |
| Security held — name | `infoTable/nameOfIssuer` | `<security>/<name>` | (security label) | (security label) | (joined via FSYM) | (joined via tradingItemId) | (FIGI-keyed) | — |
| Security held — CUSIP | `infoTable/cusip` (9-char) | `<cusip>` | `Security` | `Security` | (internal alias) | (internal alias) | (FIGI primary; CUSIP alias) | — |
| Security held — FIGI | `infoTable/figi` (optional since 2022) | (not yet adopted) | — | — | — | — | **primary key** | — |
| Shares held | `infoTable/shrsOrPrnAmt/sshPrnamt` with type=SH | `<balance>` + `<units>` | `Transactions[]` net | (item 3 share count) | `shares_held` | (numericValue) | `OPT_TOT_VAL_OUTSTAND_SHRS` `[unverified]` | — |
| Market value (USD) | `infoTable/value` (pre-2023: $K; post: $) | `<valUSD>` | (computed) | (Item 4 value) | `position_value_usd` | (computed) | — | — |
| % of outstanding shares | (derived) | `<pctVal>` | — | (Item 4 %) | `pct_outstanding` | (derived) | (derived) | — |
| Sole voting authority | `votingAuthority/Sole` | (not asked) | — | — | — | — | — | — |
| Shared voting authority | `votingAuthority/Shared` | (not asked) | — | — | — | — | — | — |
| No voting authority | `votingAuthority/None` | (not asked) | — | — | — | — | — | — |
| Sole investment discretion | `investmentDiscretion='SOLE'` | — | — | — | — | — | — | — |
| Other manager (co-filer) | `otherManager` repeating | — | — | — | (resolved into manager rollups) | — | — | — |
| Put / Call indicator | `putCall='Put'|'Call'` | — | (separate filings) | — | — | — | — | — |
| Convertible principal amount | `sshPrnamt` with type=PRN | — | — | — | — | — | — | — |
| Change from prior period | derived | derived | derived | derived | `pct_change_qoq` | `chgFromPrior` `[unverified]` | derived | derived |
| Manager-vs-fund attribution | manager-level only | fund-level only | reporting-owner-level | filer-level | manager rollup + sub-advisor logic | manager + fund | both | both |
| Report period end | `coverPage/reportCalendarOrQuarter` | `<reportDate>` (monthly) | `Period of Report` | `Date of Event` | `report_date` | `periodEnd` | (in HDS) | — |
| Filing date | (SEC accept timestamp) | (SEC accept) | (SEC accept) | (SEC accept) | `filing_date` | `filingDate` | — | — |
| Transfer / activation date | — | — | — | — | `transfer_date` (when FactSet processed) | `effectiveDate` | — | — |

**Caveats:**
- 13F's `<value>` field switched from $K to $actual on 2023-01-03 (SEC final rule 34-95148); ingestion must branch on `periodOfReport`.
- N-PORT is fund-level; 13F is manager-level. They are complementary, not redundant.
- CUSIP licensing means ats-eqt's public surface should expose FIGI-keyed positions only; CUSIP behind internal ACL.

---

## 10. Canonical `ats-eqt` item dictionary (headline 80–120 items)

The "if you only build one table" reference. These are the items most analysts touch. Full ~10,000+ leaf items in the long-format `fund_fact` table will be allocated during Phase-0 ingestion. The format mirrors what should populate the `fund_item` dictionary (see [`schemas/data_models_and_methodology.md`](data_models_and_methodology.md) §G.2 + [`datasets/fundamentals_us_equities.md`](../datasets/fundamentals_us_equities.md) §10.5 `fund_item`).

| item_id | item_code | description | default_unit | default_period_types | source_priority_order | restatement_aware |
|---|---|---|---|---|---|---|
| 1001 | `revenue_total` | Total revenue (post-ASC 606) | USD | A, Q, S, LTM, YTD | compustat → us-gaap → factset → bloomberg → ciq → sharadar | Y |
| 1003 | `cogs` | Cost of goods/services sold | USD | A, Q | compustat → us-gaap → factset → bloomberg | Y |
| 1004 | `gross_profit` | Gross profit | USD | A, Q | compustat (derived) → us-gaap → factset | Y |
| 1005 | `sga` | SG&A expense | USD | A, Q | compustat → us-gaap → factset | Y |
| 1008 | `rd_expense` | R&D expense | USD | A, Q | compustat → us-gaap → factset | Y |
| 1011 | `da_is` | D&A on income statement | USD | A, Q | compustat → us-gaap | Y |
| 1014 | `operating_income` | Operating income (after D&A) | USD | A, Q | compustat → us-gaap → factset | Y |
| 1015 | `ebitda_oibdp` | Operating income before D&A | USD | A, Q | compustat → factset | Y |
| 1016 | `ebitda` | Standardised EBITDA | USD | A, Q, LTM | factset → bloomberg → ciq → derived | Y |
| 1017 | `ebit` | Standardised EBIT | USD | A | factset → bloomberg → ciq | Y |
| 1018 | `interest_expense` | Interest expense total | USD | A, Q | compustat → us-gaap | Y |
| 1023 | `pretax_income` | Pretax income | USD | A, Q | compustat → us-gaap → factset | Y |
| 1024 | `income_tax` | Income tax expense | USD | A, Q | compustat → us-gaap → factset | Y |
| 1027 | `minority_int_pl` | Minority interest (P&L) | USD | A, Q | compustat → us-gaap | Y |
| 1029 | `ni_continuing` | Net income — continuing ops | USD | A, Q | compustat → us-gaap | Y |
| 1031 | `ni` | Net income (total) | USD | A, Q | compustat → us-gaap → factset → bloomberg | Y |
| 1032 | `ni_to_common` | Net income to common | USD | A, Q | compustat → us-gaap | Y |
| 1033 | `pref_dividends` | Preferred dividends declared | USD | A, Q | compustat → us-gaap | Y |
| 1034 | `eps_basic` | Basic EPS | USD/share | A, Q, LTM | compustat → us-gaap → factset | Y |
| 1035 | `eps_diluted` | Diluted EPS | USD/share | A, Q, LTM | compustat → us-gaap → factset | Y |
| 1039 | `shares_out` | Common shares outstanding (period-end) | shares | A, Q, M | compustat → us-gaap (cover-page) → factset | Y |
| 1040 | `shares_basic_avg` | Weighted avg shares — basic | shares | A, Q | compustat → us-gaap | Y |
| 1041 | `shares_diluted_avg` | Weighted avg shares — diluted | shares | A, Q | compustat → us-gaap | Y |
| 1043 | `dps_declared` | Dividends per share declared | USD/share | A, Q | compustat → factset | Y |
| 1101 | `total_assets` | Total assets | USD | A, Q | compustat → us-gaap → factset | Y |
| 1102 | `current_assets` | Current assets | USD | A, Q | compustat → us-gaap | Y |
| 1103 | `cash_st_inv` | Cash + ST investments | USD | A, Q | compustat → us-gaap (sum) → factset | Y |
| 1104 | `cash` | Cash only | USD | A, Q | compustat → us-gaap | Y |
| 1105 | `st_investments` | Short-term investments | USD | A, Q | compustat → us-gaap | Y |
| 1106 | `ar` | Accounts receivable | USD | A, Q | compustat → us-gaap → factset | Y |
| 1107 | `inventory` | Inventories | USD | A, Q | compustat → us-gaap | Y |
| 1110 | `ppe_net` | PP&E — net | USD | A, Q | compustat → us-gaap | Y |
| 1111 | `ppe_gross` | PP&E — gross | USD | A, Q | compustat → us-gaap | Y |
| 1114 | `goodwill` | Goodwill | USD | A, Q | compustat → us-gaap | Y |
| 1115 | `intangibles_other` | Intangibles excluding goodwill | USD | A, Q | compustat (derived) → us-gaap | Y |
| 1201 | `total_liabilities` | Total liabilities | USD | A, Q | compustat → us-gaap | Y |
| 1202 | `current_liabilities` | Current liabilities | USD | A, Q | compustat → us-gaap | Y |
| 1203 | `ap` | Accounts payable | USD | A, Q | compustat → us-gaap | Y |
| 1205 | `st_debt` | Short-term debt | USD | A, Q | compustat → us-gaap (sum) → factset | Y |
| 1207 | `lt_debt` | Long-term debt | USD | A, Q | compustat → us-gaap → factset | Y |
| 1208 | `total_debt` | Total debt (ST + LT) | USD | A, Q | factset → derived | Y |
| 1213 | `minority_int_bs` | Minority interest (BS) | USD | A, Q | compustat → us-gaap | Y |
| 1214 | `pref_stock` | Preferred stock | USD | A, Q | compustat → us-gaap | Y |
| 1217 | `retained_earnings` | Retained earnings | USD | A, Q | compustat → us-gaap | Y |
| 1219 | `treasury_stock` | Treasury stock | USD | A, Q | compustat → us-gaap | Y |
| 1220 | `common_equity` | Common equity | USD | A, Q | compustat → us-gaap (derived) | Y |
| 1221 | `total_equity` | Total stockholders' equity | USD | A, Q | compustat → us-gaap → factset | Y |
| 1301 | `cfo` | Cash flow from operations | USD | A, Q (YTD) | compustat → us-gaap | Y |
| 1303 | `cfi` | Cash flow from investing | USD | A, Q (YTD) | compustat → us-gaap | Y |
| 1304 | `cff` | Cash flow from financing | USD | A, Q (YTD) | compustat → us-gaap | Y |
| 1305 | `capex` | Capital expenditures | USD | A, Q (YTD) | compustat → us-gaap → factset | Y |
| 1307 | `da_cf` | D&A on cash flow | USD | A, Q (YTD) | compustat → us-gaap | Y |
| 1308 | `stock_based_comp` | Stock-based compensation | USD | A, Q | factset → us-gaap (no compustat headline) | Y |
| 1309 | `acquisitions` | Acquisitions of businesses | USD | A, Q (YTD) | compustat → us-gaap | Y |
| 1311 | `stock_issuance` | Common/preferred stock issuance | USD | A, Q (YTD) | compustat → us-gaap | Y |
| 1312 | `stock_buybacks` | Stock repurchases | USD | A, Q (YTD) | compustat → us-gaap | Y |
| 1313 | `lt_debt_issued` | LT debt issuance | USD | A | compustat → us-gaap | Y |
| 1314 | `lt_debt_repaid` | LT debt repayment | USD | A | compustat → us-gaap | Y |
| 1316 | `common_div_paid` | Common dividends paid (cash) | USD | A, Q (YTD) | compustat → us-gaap | Y |
| 1317 | `pref_div_paid` | Preferred dividends paid (cash) | USD | A, Q (YTD) | compustat → us-gaap | Y |
| 1322 | `change_in_wc` | Change in working capital | USD | A, Q (YTD) | factset → derived | Y |
| 1323 | `fx_effect_cash` | FX effect on cash | USD | A, Q (YTD) | compustat → us-gaap | Y |
| 1324 | `net_change_cash` | Net change in cash | USD | A, Q (YTD) | compustat → us-gaap | Y |
| 1325 | `fcf` | Free cash flow (CFO − Capex) | USD | A, Q, LTM | derived | Y |
| 1401 | `bvps` | Book value per share | USD/share | A, Q | factset → sharadar → derived | N |
| 1413 | `mkt_cap` | Market cap | USD | D, M, Q, A | crsp (price×shrout) → compustat → bloomberg | N |
| 1501 | `net_interest_income` | Net interest income (banks) | USD | A, Q | factset → ciq | Y |
| 1502 | `nim` | Net interest margin (banks) | ratio | A, Q | factset → ciq | Y |
| 1505 | `provision_loan_losses` | Provision for loan losses | USD | A, Q | compustat → factset → ciq | Y |
| 1601 | `premiums_earned` | Premiums earned (insurance) | USD | A, Q | factset → ciq | Y |
| 1606 | `loss_ratio` | Loss ratio (insurance) | ratio | A, Q | factset → ciq | Y |
| 1608 | `combined_ratio` | Combined ratio (insurance) | ratio | A, Q | factset → ciq | Y |
| 1701 | `ffo` | Funds from operations (REIT) | USD | A, Q | factset → ciq → nareit extension | Y |
| 1703 | `affo` | Adjusted FFO (REIT) | USD | A, Q | factset → ciq | Y |
| 1705 | `noi` | Net operating income (REIT) | USD | A, Q | factset | Y |
| 1710 | `nav_per_share` | Net asset value per share (REIT/CEF) | USD/share | A, Q | factset → ciq | Y |
| 2001 | `est_eps_diluted_norm` | Consensus diluted normalized EPS | USD/share | FY1..FY5, FQ1..FQ4, LTG, NTM | ibes → factset → bloomberg → ciq | (estimates are inherently versioned) |
| 2005 | `est_sales` | Consensus sales/revenue | USD | FY1..FY5, FQ1..FQ4 | ibes → factset → bloomberg → ciq | (versioned) |
| 2006 | `est_ebitda` | Consensus EBITDA | USD | FY1..FY5 | ibes → factset → bloomberg → ciq | (versioned) |
| 2009 | `est_ni` | Consensus net income | USD | FY1..FY5 | ibes → factset | (versioned) |
| 2024 | `est_ltg` | Long-term growth (3-5y EPS %) | % | LTG | ibes → factset → bloomberg | (versioned) |
| 2025 | `est_target_price` | Consensus 12m price target | USD | 12m | ibes (`ptg*`) → factset → bloomberg | (versioned) |
| 2026 | `est_recommendation` | Consensus recommendation (1=Buy↔5=Sell) | rec | (current) | ibes → factset → ciq | (versioned) |
| 3001 | `ghg_scope_1` | Scope 1 emissions | tCO2e | A | esrs → cdp → bloomberg → msci → lseg → sustainalytics | Y (vendors silently restate) |
| 3002 | `ghg_scope_2_loc` | Scope 2 emissions — location-based | tCO2e | A | esrs → cdp → bloomberg | Y |
| 3003 | `ghg_scope_2_mkt` | Scope 2 emissions — market-based | tCO2e | A | esrs → cdp → bloomberg | Y |
| 3004 | `ghg_scope_3_total` | Scope 3 emissions — total | tCO2e | A | esrs → cdp → bloomberg | Y |
| 3021 | `energy_consumption` | Total energy consumption | MWh | A | esrs → cdp → bloomberg | Y |
| 3022 | `pct_renewable_energy` | Renewable energy % | % | A | esrs → cdp → bloomberg | Y |
| 3024 | `water_withdrawal` | Water withdrawal | m³ | A | esrs → cdp → bloomberg | Y |
| 3028 | `waste_generated` | Waste generated (total) | tonnes | A | esrs → bloomberg | Y |
| 3031 | `employees_total` | Total employees | headcount | A | esrs → 10-K Item 1 → bloomberg | Y |
| 3032 | `turnover_pct` | Employee turnover | % | A | esrs → bloomberg | Y |
| 3033 | `pct_female_employees` | % Female employees | % | A | esrs → bloomberg | Y |
| 3036 | `ltifr` | Lost-Time Injury Rate | rate | A | esrs → bloomberg | Y |
| 3038 | `gender_pay_gap` | Unadjusted gender pay gap | % | A | esrs (PAI 12) → UK Companies House | Y |
| 3039 | `pct_board_independent` | % Independent directors | % | A | DEF 14A → bloomberg → iss | Y |
| 3040 | `pct_board_female` | % Female directors | % | A | DEF 14A → bloomberg | Y |
| 3043 | `ceo_pay_ratio` | CEO-to-median pay ratio | ratio | A | DEF 14A → bloomberg | Y |

---

## 11. Identifier mapping rules

### 11.1 Hub-and-spoke spine

```
                           CIK (US issuer)
                              |
                              +-- 1:1 ----- entity_id (ats-eqt internal)
                              |                  |
                              |                  +-- 1:N --- LEI (legal entities under the issuer; subsidiaries each have their own LEI)
                              |                  |
                              |                  +-- 1:N --- security_id (common, preferred, debt, ADR, …)
                              |                                    |
                              |                                    +-- 1:1 --- Share Class FIGI
                              |                                    +-- 1:N --- Composite FIGI (per country)
                              |                                                       +-- 1:N --- Venue FIGI (per exchange)
                              |
                              +-- 0:N --- subsidiary CIK (Form 10-K Exhibit 21)
```

### 11.2 Mapping degradation order

```
LEI (entity)      ≈  most stable; regulator-mandated for derivatives counterparties
FIGI (share-class)  ≈  permanent for the equity class
FIGI (composite)  ≈  permanent for country-level instrument
CIK               =  stable for US filer (occasional reassignment on absorption)
GVKEY / PERMNO    =  stable proprietary
ISIN              ≈  stable per share class; ties to CUSIP
CUSIP             =  changes on share-class reclassification or material restructuring
SEDOL             =  UK-centric, changes on listing change
Ticker + Exchange =  LEAST STABLE; reused after delisting
```

### 11.3 GLEIF — LEI resolution

- **API base:** `https://api.gleif.org/api/v1/`
- **Single LEI:** `GET /api/v1/lei-records/<LEI>`
- **Search:** `GET /api/v1/lei-records?filter[entity.legalName]=Apple`
- **Daily concatenated CDF zip:** `https://goldencopy.gleif.org/api/v2/golden-copies/publishes` (latest file zipped XML)
- **License:** free under LEI Data Terms of Use; CHF 100,000 liquidated-damages clause for breach (see [`sources/public_data_sources.md`](../sources/public_data_sources.md)).

### 11.4 OpenFIGI — FIGI resolution

- **API base:** `https://api.openfigi.com/v3/mapping`
- **Method:** `POST` with JSON body `[{"idType":"ID_ISIN","idValue":"US0378331005"}, ...]`
- **Rate limits:** 25 req / 6s anonymous; 1000 req / 6s with API key (free).
- **License:** **MIT** — FIGI itself is in the public domain.
- Resolves CUSIP, ISIN, SEDOL, ticker, BBG composite ticker → FIGI(s).

### 11.5 PermID — open ID resolution

- **API base:** `https://api.thomsonreuters.com/permid/` and `https://permid.org/`
- **Entity search:** `https://permid.org/api/mdaas/getEntityByLei/<LEI>`
- **Record matching:** Bulk API accepts file upload (CSV); returns PermID + crosswalk fields.
- **License:** free for use; redistribution conditions apply (LSEG retains right to change terms).

---

## 12. Open questions / next-wave gaps

Synthesis pass surfaced these unresolved or partially-resolved many-to-many mappings:

1. **Worldscope item-code coverage for cash-flow + bank/insurance templates.** The fundamentals doc (§4 in source; not transcribed here) cites Worldscope `01001` through `05491` for industrial template but the bank (`08xxx`) and insurance (`09xxx`) template codes are partially `[unverified]`. Tilburg's public WS Datatype Definitions Guide PDF covers industrial only.
2. **CIQ `dataItemId` integer stability.** All ~30 CIQ integer IDs in the fundamentals table are `[unverified]` — CIQ's `IQ_*` mnemonics are stable but the integer keys revise across CIQ releases. WRDS `ciq.ciqfinancialitem` should be queried directly to lock current bindings.
3. **FactSet `FF_BANK` / `FF_INS` / `FF_REIT` industry-template full field lists.** Subscriber-only Data Item Definitions PDF not parsed. Mnemonics above are partially inferred from public methodology briefs.
4. **CRSP `DISTCD` ↔ DTCC `CAEV` authoritative crosswalk.** Not published publicly. Compute empirically from a sample period or contact WRDS.
5. **Bloomberg Scope 3 by-category field names.** All `GHG_SCOPE_3_*` per-category mnemonics in §8 are `[unverified]` — Bloomberg's ESG taxonomy PDF lists the 15 categories conceptually but the precise field name per category is not in the methodology PDF.
6. **MSCI ESG Manager exact field names** (`IVA_COMPANY_RATING`, etc.) — sourced from MSCI's published methodology PDF and third-party scorecards; not from a vendor data dictionary. `[unverified]` flag should ride.
7. **N-PORT field coverage.** N-PORT's full schema (~150 columns) is not yet enumerated in the wave-2 docs; only the cross-mapping subset in §9 is covered. A wave-3 N-PORT field-level pass is appropriate.
8. **Visible Alpha line-item dictionary.** The 156 average line-items per company × 1M total line items in Visible Alpha is opaque — no public schema. Subscriber-only data dictionary required.
9. **IBES `MEASURE` vs FactSet `FE_*` for KPI feed measures.** KPI feeds (Same-Store Sales, ARPU, etc.) use industry-specific codes that don't always map 1:1 across vendors. Wave-3 should produce an industry-KPI sub-map.
10. **Refinitiv/LSEG Worldscope numeric ↔ WS.* mnemonic mapping.** Worldscope ships both `WS01001` and `WS.NetSales` — the WS.* mnemonics are partly documented but the full ~1,500-item correspondence isn't in our corpus.
11. **Form SHO (short interest) field schema.** Compliance extended to 2028-02-14; no concrete fields to map yet.
12. **SEC ownership form unification.** 13F vs N-PORT vs Form 4 vs 13D/G use different XSDs; ats-eqt's `ownership_fact` table needs a unified holding-event grain. Wave-3 task.

---

## 13. Sources

Synthesis-only; URLs below were the primary references in the input docs and are repeated here so this file can be read standalone. All wave-1/wave-2 docs themselves carry far longer source bibliographies — see each doc's "Sources" appendix.

### Fundamentals — Compustat / FactSet / Worldscope / Bloomberg / CIQ

- <https://w3.loibl.com/uni/xf_understanding_the_data.pdf> — Compustat Xpressfeed "Understanding the Data".
- <https://wrds-www.wharton.upenn.edu/data-dictionary/comp_na_daily_all/> — WRDS Compustat data dictionary.
- <https://ionmihai.github.io/finsets/01_wrds/compq.html> — Compustat quarterly mnemonics.
- <https://insight.factset.com/resources/at-a-glance-factset-fundamental-datafeed> — FactSet Fundamentals at-a-glance.
- <https://my1396.github.io/Econ-Study/2024/02/20/FactSet101.html> — `ff_v3.*` schema enumeration.
- <https://developer.factset.com/api-catalog/factset-fundamentals-api> — FactSet Fundamentals API.
- <https://www.lseg.com/en/data-analytics/financial-data/company-data/fundamentals-data/worldscope-fundamentals> — Worldscope product page.
- <https://www.tilburguniversity.edu/sites/default/files/download/WorldScopeDatatypeDefinitionsGuide_2.pdf> — Worldscope Datatype Definitions Guide.
- <https://bautheac.github.io/BBGsymbols/> — Bloomberg field catalogue (BBGsymbols R package).
- <https://data.bloomberglp.com/professional/sites/10/189913_CDS_REF_Fundamentals_SFCT_DIG.pdf> — Bloomberg Fundamentals data sheet.
- <https://wrds-www.wharton.upenn.edu/pages/grid-items/introduction-capital-iq/> — WRDS Capital IQ overview.
- <http://larryschrenk.com/Capital%20IQ/Excel%20Plug-in%20Manual.pdf> — CIQ Excel Plug-in Manual.
- <https://data.nasdaq.com/databases/SF1> — Sharadar Core US Fundamentals (SF1).
- <https://www.simfin.com/en/fundamental-data-download/> — SimFin.

### us-gaap / SEC XBRL

- <https://www.sec.gov/search-filings/edgar-application-programming-interfaces> — EDGAR APIs.
- <https://www.sec.gov/data-research/sec-markets-data/financial-statement-data-sets> — DERA Financial Statement Data Sets.
- <https://xbrl.us/why-normalize-data/> — XBRL US "Why Normalize Data".
- <https://www.fasb.org/page/detail?pageId=/projects/FASB-Taxonomies/2024-gaap-financial-reporting-taxonomy.html> — FASB 2024 GAAP Taxonomy.
- <https://arelle.org/> — Arelle XBRL processor.

### Estimates

- <https://www.library.kent.edu/files/IBES_GuideUS.pdf> — IBES Detail History Guide.
- <https://www.tilburguniversity.edu/sites/default/files/download/IBESonWRDS_2.pdf> — IBES on WRDS.
- <https://wrds-www.wharton.upenn.edu/pages/about/data-vendors/vendor-partner-ibes/> — IBES vendor partner page.
- <https://insight.factset.com/resources/factset-consensus-estimates-datafeed> — FactSet Consensus Estimates.
- <https://developer.factset.com/api-catalog/factset-estimates-api> — FactSet Estimates API.
- <https://www.bloomberg.com/professional/dataset/global-bloomberg-estimates-data/> — Bloomberg BEst.
- <https://studylib.net/doc/25233007/best-fperiod-override> — Bloomberg BEST_FPERIOD_OVERRIDE.
- <https://www.spglobal.com/market-intelligence/en/solutions/visible-alpha> — Visible Alpha.

### Corporate actions

- <https://www.crsp.org/products/documentation/distribution-codes> — CRSP DISTCD enumeration.
- <https://www.crsp.org/products/documentation/delisting-codes> — CRSP DLSTCD enumeration.
- <https://www.crsp.org/wp-content/uploads/guides/CRSP_US_Stock_&_Indexes_Database_Data_Descriptions_Guide.pdf> — CRSP US Stock Database guide.
- <https://www.dtcc.com/data-services/corporate-actions-and-reference-data/dtcc-ca-20022-service> — DTCC CA 20022.
- <https://www.dtcc.com/asset-services/corporate-actions-processing/iso-20022-messaging-specifications> — DTCC ISO 20022 messaging specs.
- <https://www.openfigi.com/assets/local/figi-allocation-rules.pdf> — FIGI allocation rules.
- <https://www.irs.gov/forms-pubs/about-form-8937> — IRS Form 8937 (spinoff cost basis).

### Pricing

- <https://ionmihai.github.io/finsets/01_wrds/crspd.html> — CRSP daily file fields.
- <https://www.nyse.com/publicdocs/nyse/data/Daily_TAQ_Client_Spec_v4.2.pdf> — NYSE Daily TAQ v4.2 spec.
- <https://developer.factset.com/api-catalog/factset-prices-api> — FactSet Prices API.
- <https://finm-32900.github.io/lectures/Week7/LSEG_datastream.html> — Datastream mnemonics.
- <https://polygon.io/pricing> — Polygon API pricing.

### Identifiers

- <https://www.openfigi.com/> — OpenFIGI.
- <https://www.gleif.org/en/lei-data/lei-mapping> — GLEIF LEI mapping.
- <https://permid.org/> — LSEG PermID.
- <https://www.openfigi.com/about/regulations> — FIGI as 13F alternative ID.
- <https://www.sec.gov/files/company_tickers.json> — SEC CIK ticker mapping.
- <https://assets.ctfassets.net/lmz2w5z92b9u/7INM5wpJ5u1bomIisoOoz2/beaad6e64bbbdc96f8996acc9c8a1b34/FactSet_Permanent_Security_Identifier.pdf> — FactSet FSYM hierarchy.

### Industry classifications

- <https://www.msci.com/our-solutions/indexes/gics> — MSCI GICS.
- <https://www.ftserussell.com/data/industry-classification-benchmark-icb> — FTSE Russell ICB.
- <https://permid.org/> — TRBC (via PermID).
- <https://www.factset.com/marketplace/catalog/product/factset-rbics> — FactSet RBICS.
- <https://www.census.gov/naics/> — NAICS.

### ESG

- <https://www.msci.com/documents/1296102/34424357/MSCI+ESG+Ratings+Methodology.pdf> — MSCI ESG methodology.
- <https://www.spglobal.com/sustainable1/en/csa> — S&P Global CSA.
- <https://www.sustainalytics.com/docs/knowledgehublibraries/default-document-library/sustainalytics_-esg-risk-ratings_-version-3-1_-methodology-abstract_-june-2024.pdf> — Sustainalytics v3.1.
- <https://professional.bloomberg.com/globalassets/professional/solutions/sustainable-finance/scores/bloomberg-esg-scores-methodology.pdf> — Bloomberg ESG scores methodology.
- <https://www.lseg.com/content/dam/data-analytics/en_us/documents/methodology/lseg-esg-scores-methodology.pdf> — LSEG ESG methodology.
- <https://www.cdp.net/en/data> — CDP datasets.
- <https://xbrl.efrag.org/e-esrs/esrs-set1-2023.html> — ESRS Set 1 XBRL taxonomy.
- <https://academic.oup.com/rof/article/26/6/1315/6590670> — Berg-Kölbel-Rigobon "Aggregate Confusion".

### Ownership

- <https://www.sec.gov/edgar/filer-information/specifications/form13fxmltechspec-draft> — EDGAR 13F XML technical spec.
- <https://www.factset.com/marketplace/catalog/product/factset-ownership> — FactSet Ownership.
- <https://www.spglobal.com/market-intelligence/en/solutions/products/sp-capital-iq-pro> — CIQ Pro Ownership module.
- <https://data.bloomberglp.com/professional/sites/10/Security-Ownership-fact-sheet.pdf> — Bloomberg Security Ownership.
- <https://www.lseg.com/en/data-analytics/financial-data/company-data/company-ownership-information-profiles> — LSEG / Lipper / eMAXX.
