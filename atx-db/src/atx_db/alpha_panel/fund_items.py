"""Stage F item definitions and the per-issuer point-in-time fiscal arithmetic (pure Python).

``fundamentals.py`` streams fact rows per issuer into ``issuer_events``; this module owns the concept
chains, the knowledge state and every item formula. See ``docs/ALPHA_PANEL_FUNDAMENTALS.md`` for the
rules in prose.

Knowledge: for every ``(concept, start, end)`` (durations) and ``(concept, end)`` (instants) the value of
the latest-clock filing applied so far. Filings are applied in clock order, so restatements enter on their
own clock and a later comparative replaces an older value.

Tiers (rule fund-events-pit-v2). ``CHAINS`` are the v1 chains, unchanged. ``CHAINS_EXT`` adds a second
tier per item (us-gaap fallback concepts, then ``ifrs-full`` concepts). Every lookup (value at an end, a
discrete quarter, a TTM, a year-ago search) is completed on the v1 tier first; the extension tier is read
only when the v1 tier has no value for that lookup, so every v1 value is unchanged and the extension only
fills gaps. Money facts enter one knowledge per currency (``money_unit``); the event columns hold the USD
knowledge, currency-invariant items (``sue``, F-score terms) come from the knowledge in the filing's
reporting currency (``issuer_events``).
"""

from __future__ import annotations

import datetime as dt
import math
import statistics
from collections import Counter
from collections.abc import Iterable, Sequence
from typing import Any

DAY = dt.timedelta(days=1)

USGAAP = "us-gaap"
IFRS = "ifrs-full"
DEI = "dei"
CLS = "cls"          # FSDS NUM class-of-stock sums built by fundamentals.prepare (pseudo taxonomy)
POS = "pos"          # FSDS NUM ProductOrService member sums of cost of revenue (pseudo taxonomy)
X = "~x"             # suffix of the extension tier of a chain

# ---------------------------------------------------------------------------
# v1 concept chains (priority order). Source: seeds/statement_map.csv canonical metrics
# (concept_priority ascending) with the three total-over-component overrides:
# revenue: Revenues before the ASC 606 RevenueFromContractWithCustomer* concepts;
# cash: CashAndCashEquivalentsAtCarryingValue before Cash; short-term debt: DebtCurrent
# before its components. Items outside the statement map are marked "extension".
# ---------------------------------------------------------------------------

CHAINS: dict[str, tuple[str, ...]] = {
    # instants
    "at": ("Assets",),
    "lt": ("Liabilities",),
    "seq": ("StockholdersEquity",),
    "seq_nci": ("StockholdersEquityIncludingPortionAttributableToNoncontrollingInterest",),
    "mib": ("MinorityInterest",),
    "cash_sti": ("CashCashEquivalentsAndShortTermInvestments",),
    "cash": ("CashAndCashEquivalentsAtCarryingValue", "Cash", "CashAndDueFromBanks",
             "CashCashEquivalentsRestrictedCashAndRestrictedCashEquivalents"),
    "sti": ("ShortTermInvestments", "MarketableSecuritiesCurrent", "AvailableForSaleSecuritiesCurrent",
            "OtherShortTermInvestments"),
    "debt_current": ("DebtCurrent",),
    "cur_ltd": ("LongTermDebtCurrent", "LongTermDebtAndCapitalLeaseObligationsCurrent"),
    "stb": ("ShortTermBorrowings", "OtherShortTermBorrowings", "CommercialPaper", "NotesPayableCurrent",
            "LinesOfCreditCurrent", "ConvertibleNotesPayableCurrent", "ConvertibleDebtCurrent",
            "LoansPayableCurrent", "ShortTermBankLoansAndNotesPayable"),
    "ltd_nc": ("LongTermDebtNoncurrent", "LongTermDebtAndCapitalLeaseObligations", "LongTermNotesPayable",
               "SeniorNotes", "LongTermLineOfCredit", "ConvertibleDebtNoncurrent",
               "ConvertibleLongTermNotesPayable", "LongTermLoansPayable", "OtherLongTermDebtNoncurrent",
               "ConvertibleNotesPayable", "NotesPayable"),
    "ltd_total": ("LongTermDebt",),  # extension: total long-term debt incl. current maturities
    "txditc": ("DeferredIncomeTaxLiabilitiesNet", "DeferredTaxLiabilitiesNoncurrent"),
    "pstk": ("PreferredStockRedemptionAmount", "PreferredStockLiquidationPreferenceValue",
             "PreferredStockValue", "PreferredStockValueOutstanding"),
    "invt": ("InventoryNet", "InventoryGross", "InventoryNetOfAllowancesCustomerAdvancesAndProgressBillings"),
    "rect": ("AccountsReceivableNetCurrent", "ReceivablesNetCurrent", "AccountsAndNotesReceivableNet",
             "AccountsReceivableGrossCurrent"),
    "ppe": ("PropertyPlantAndEquipmentNet",
            "PropertyPlantAndEquipmentAndFinanceLeaseRightOfUseAssetAfterAccumulatedDepreciationAndAmortization"),
    "act": ("AssetsCurrent",),
    "lct": ("LiabilitiesCurrent",),
    "cso": ("CommonStockSharesOutstanding",),
    # durations
    "sale": ("Revenues", "RevenueFromContractWithCustomerExcludingAssessedTax",
             "RevenueFromContractWithCustomerIncludingAssessedTax", "SalesRevenueNet", "SalesRevenueGoodsNet",
             "SalesRevenueServicesNet", "RevenuesNetOfInterestExpense"),
    # extension: bank revenue fallback (seed BK template interest_income_bank) = interest and
    # dividend income + noninterest income, used only when the sale chain has no value
    "int_inc": ("InterestAndDividendIncomeOperating",),
    "nonint_inc": ("NoninterestIncome",),
    "cogs": ("CostOfGoodsAndServicesSold", "CostOfRevenue", "CostOfGoodsSold", "CostOfServices",
             "CostOfGoodsAndServiceExcludingDepreciationDepletionAndAmortization",
             "CostOfGoodsSoldExcludingDepreciationDepletionAndAmortization"),
    "xsga": ("SellingGeneralAndAdministrativeExpense", "OtherSellingGeneralAndAdministrativeExpense"),
    "xga": ("GeneralAndAdministrativeExpense",),
    "xsm": ("SellingAndMarketingExpense", "SellingExpense"),
    "gp": ("GrossProfit",),
    "oi": ("OperatingIncomeLoss",),
    "ni": ("NetIncomeLoss", "ProfitLoss", "NetIncomeLossAvailableToCommonStockholdersBasic"),
    "cfo": ("NetCashProvidedByUsedInOperatingActivities",
            "NetCashProvidedByUsedInOperatingActivitiesContinuingOperations"),
    "capx": ("PaymentsToAcquirePropertyPlantAndEquipment", "PaymentsForCapitalImprovements",
             "PaymentsToAcquireMachineryAndEquipment", "PaymentsToAcquireOtherPropertyPlantAndEquipment",
             "PaymentsToAcquireProductiveAssets"),
    "xrd": ("ResearchAndDevelopmentExpense", "ResearchAndDevelopmentExpenseExcludingAcquiredInProcessCost",
            "ResearchAndDevelopmentExpenseSoftwareExcludingAcquiredInProcessCost"),
    "dvc": ("PaymentsOfDividendsCommonStock", "PaymentsOfDividends"),
    "prstkc": ("PaymentsForRepurchaseOfCommonStock", "PaymentsForRepurchaseOfEquity",
               "TreasuryStockValueAcquiredCostMethod"),
    "sstk": ("ProceedsFromIssuanceOfCommonStock", "ProceedsFromIssuanceOrSaleOfEquity",
             "ProceedsFromStockOptionsExercised"),
    "dp": ("DepreciationDepletionAndAmortization", "DepreciationAmortizationAndAccretionNet",
           "DepreciationAndAmortization", "Depreciation", "DepreciationNonproduction",
           "DepreciationDepletionAndAmortizationNonproduction"),
    "txt": ("IncomeTaxExpenseBenefit", "IncomeTaxExpenseBenefitContinuingOperations"),
    "waso_basic": ("WeightedAverageNumberOfSharesOutstandingBasic",
                   "WeightedAverageNumberOfShareOutstandingBasicAndDiluted"),
    "waso_diluted": ("WeightedAverageNumberOfDilutedSharesOutstanding",),
}

# ---------------------------------------------------------------------------
# v2 extension tiers: plain names are us-gaap, ``ifrs-full:`` names IFRS. The v1 balance-sheet keys
# (at, seq, seq_nci, ...) get IFRS concepts only, so the v1 balance-sheet dates are unchanged.
# ---------------------------------------------------------------------------

def _ifrs(*names: str) -> tuple[str, ...]:
    return tuple(f"{IFRS}:{n}" for n in names)


CHAINS_EXT: dict[str, tuple[str, ...]] = {
    # instants, extension of v1 keys (IFRS)
    "at": _ifrs("Assets"),
    "lt": _ifrs("Liabilities"),
    "seq": _ifrs("EquityAttributableToOwnersOfParent"),
    "seq_nci": _ifrs("Equity"),
    "mib": _ifrs("NoncontrollingInterests"),
    "cash": _ifrs("CashAndCashEquivalents", "Cash"),
    "sti": _ifrs("ShorttermDepositsNotClassifiedAsCashEquivalents", "CurrentInvestments",
                 "CurrentFinancialAssetsAtFairValueThroughProfitOrLoss"),
    "debt_current": _ifrs("CurrentBorrowingsAndCurrentPortionOfNoncurrentBorrowings"),
    "cur_ltd": _ifrs("CurrentPortionOfLongtermBorrowings"),
    "stb": _ifrs("ShorttermBorrowings"),
    "ltd_nc": _ifrs("LongtermBorrowings"),
    "txditc": _ifrs("DeferredTaxLiabilities"),
    "invt": _ifrs("Inventories"),
    "rect": _ifrs("TradeAndOtherCurrentReceivables", "CurrentTradeReceivables", "TradeReceivables"),
    "ppe": _ifrs("PropertyPlantAndEquipment"),
    "act": _ifrs("CurrentAssets"),
    "lct": _ifrs("CurrentLiabilities"),
    "cso": _ifrs("NumberOfSharesOutstanding"),
    # instants, new items (D6)
    "ap": ("AccountsPayableCurrent", "AccountsPayableTradeCurrent", "AccountsPayableAndAccruedLiabilitiesCurrent",
           "AccountsPayableCurrentAndNoncurrent")
          + _ifrs("TradeAndOtherCurrentPayables", "TradeAndOtherCurrentPayablesToTradeSuppliers",
                  "TradeAndOtherPayables"),
    "drev": ("ContractWithCustomerLiability", "DeferredRevenue") + _ifrs("ContractLiabilities"),
    "drev_cur": ("ContractWithCustomerLiabilityCurrent", "DeferredRevenueCurrent", "DeferredRevenueAndCreditsCurrent")
                + _ifrs("CurrentContractLiabilities"),
    "drev_nc": ("ContractWithCustomerLiabilityNoncurrent", "DeferredRevenueNoncurrent",
                "DeferredRevenueAndCreditsNoncurrent") + _ifrs("NoncurrentContractLiabilities"),
    "ppegt": ("PropertyPlantAndEquipmentGross",
              "PropertyPlantAndEquipmentAndFinanceLeaseRightOfUseAssetBeforeAccumulatedDepreciationAndAmortization"),
    "gdwl": ("Goodwill",) + _ifrs("Goodwill"),
    "intan": ("IntangibleAssetsNetExcludingGoodwill",) + _ifrs("IntangibleAssetsOtherThanGoodwill"),
    "intan_fin": ("FiniteLivedIntangibleAssetsNet",),
    "intan_ind": ("IndefiniteLivedIntangibleAssetsExcludingGoodwill",),
    "intan_gw": ("IntangibleAssetsNetIncludingGoodwill",) + _ifrs("IntangibleAssetsAndGoodwill"),
    # buyback authorizations (D10): program amount authorized / remaining, point values
    "buyback_auth": ("StockRepurchaseProgramAuthorizedAmount1", "StockRepurchaseProgramAuthorizedAmount"),
    "buyback_rem": ("StockRepurchaseProgramRemainingAuthorizedRepurchaseAmount1",
                    "StockRepurchaseProgramRemainingAuthorizedRepurchaseAmount"),
    # durations, extension of v1 keys
    "sale": ("RevenuesExcludingInterestAndDividends", "RegulatedAndUnregulatedOperatingRevenue",
             "ElectricUtilityRevenue", "HealthCareOrganizationRevenue", "OperatingLeaseLeaseIncome",
             "OperatingLeasesIncomeStatementLeaseRevenue", "RealEstateRevenueNet", "OilAndGasRevenue",
             "OilAndGasSalesRevenue", "FinancialServicesRevenue", "RevenueMineralSales", "ContractsRevenue")
            + _ifrs("Revenue", "RevenueFromContractsWithCustomers", "RevenueFromSaleOfGoods",
                    "RevenueFromRenderingOfServices"),
    "cogs": ("CostOfServicesExcludingDepreciationDepletionAndAmortization", "CostOfRealEstateRevenue",
             "CostOfRealEstateSales", "DirectCostsOfLeasedAndRentedPropertyOrEquipment",
             "CostOfOtherPropertyOperatingExpense", "DirectCostsOfHotels") + _ifrs("CostOfSales") + (
        f"{POS}:CostOfGoodsAndServicesSold", f"{POS}:CostOfRevenue", f"{POS}:CostOfGoodsSold", f"{POS}:CostOfServices"),
    # total costs and expenses (Compustat-style cost of revenue = total costs - SG&A - R&D - D&A)
    "opcost": ("CostsAndExpenses", "OperatingCostsAndExpenses"),
    "xsga": _ifrs("SellingGeneralAndAdministrativeExpense"),
    "xga": _ifrs("AdministrativeExpense", "GeneralAndAdministrativeExpense"),
    "xsm": _ifrs("DistributionCosts", "SellingExpense", "SalesAndMarketingExpense"),
    "gp": _ifrs("GrossProfit"),
    "oi": _ifrs("ProfitLossFromOperatingActivities"),
    "ni": _ifrs("ProfitLossAttributableToOwnersOfParent", "ProfitLoss"),
    "cfo": _ifrs("CashFlowsFromUsedInOperatingActivities", "CashFlowsFromUsedInOperatingActivitiesContinuingOperations"),
    "capx": ("PaymentsToAcquireOilAndGasPropertyAndEquipment", "PaymentsToAcquireOilAndGasProperty",
             "PaymentsToExploreAndDevelopOilAndGasProperties", "PaymentsToAcquireMiningAssets",
             "PaymentsForConstructionInProcess", "PaymentsToDevelopRealEstateAssets",
             "PaymentsToAcquireAndDevelopRealEstate", "PaymentsToAcquireOtherProductiveAssets")
            + _ifrs("PurchaseOfPropertyPlantAndEquipmentClassifiedAsInvestingActivities",
                    "PurchaseOfPropertyPlantAndEquipment",
                    "PurchaseOfPropertyPlantAndEquipmentIntangibleAssetsOtherThanGoodwillInvestmentPropertyAndOtherNoncurrentAssets"),
    "xrd": _ifrs("ResearchAndDevelopmentExpense"),
    "dvc": _ifrs("DividendsPaidClassifiedAsFinancingActivities",
                 "DividendsPaidToEquityHoldersOfParentClassifiedAsFinancingActivities", "DividendsPaid"),
    "prstkc": _ifrs("PaymentsToAcquireOrRedeemEntitysShares", "PurchaseOfTreasuryShares"),
    "sstk": _ifrs("ProceedsFromIssuingShares", "ProceedsFromIssueOfOrdinaryShares"),
    "dp": _ifrs("DepreciationAndAmortisationExpense", "AdjustmentsForDepreciationAndAmortisationExpense"),
    "txt": _ifrs("IncomeTaxExpenseContinuingOperations"),
    "waso_basic": _ifrs("WeightedAverageShares"),
    "waso_diluted": _ifrs("AdjustedWeightedAverageShares"),
    # durations, new items
    "ebit_rep": ("IncomeLossFromContinuingOperationsBeforeInterestExpenseInterestIncomeIncomeTaxesExtraordinaryItemsNoncontrollingInterestsNet",),
    "pretax": ("IncomeLossFromContinuingOperationsBeforeIncomeTaxesExtraordinaryItemsNoncontrollingInterest",
               "IncomeLossFromContinuingOperationsBeforeIncomeTaxesMinorityInterestAndIncomeLossFromEquityMethodInvestments")
              + _ifrs("ProfitLossBeforeTax"),
    "xint": ("InterestExpense", "InterestExpenseNonoperating", "InterestAndDebtExpense", "InterestExpenseDebt",
             "InterestExpenseBorrowings", "InterestExpenseOperating") + _ifrs("InterestExpense", "FinanceCosts"),
    "dvt": ("DividendsCommonStock", "DividendsCommonStockCash", "Dividends", "DividendsCash")
           + _ifrs("DividendsRecognisedAsDistributionsToOwnersOfParent"),
}
CLS_CONCEPTS = {"cls_cso": f"{CLS}:CommonStockSharesOutstanding",
                "cls_waso_basic": f"{CLS}:WeightedAverageNumberOfSharesOutstandingBasic",
                "cls_waso_diluted": f"{CLS}:WeightedAverageNumberOfDilutedSharesOutstanding"}
CLS_INSTANT = frozenset({CLS_CONCEPTS["cls_cso"]})

DEI_SHARES = "EntityCommonStockSharesOutstanding"
SHARE_CONCEPTS = frozenset({DEI_SHARES, "CommonStockSharesOutstanding",
                            "WeightedAverageNumberOfSharesOutstandingBasic",
                            "WeightedAverageNumberOfShareOutstandingBasicAndDiluted",
                            "WeightedAverageNumberOfDilutedSharesOutstanding"})
SHARE_CONCEPTS_EXT = frozenset(_ifrs("NumberOfSharesOutstanding", "WeightedAverageShares",
                                     "AdjustedWeightedAverageShares")) | frozenset(CLS_CONCEPTS.values())
INSTANT_KEYS = ("at", "lt", "seq", "seq_nci", "mib", "cash_sti", "cash", "sti", "debt_current", "cur_ltd", "stb",
                "ltd_nc", "ltd_total", "txditc", "pstk", "invt", "rect", "ppe", "act", "lct", "cso")
INSTANT_KEYS_EXT = INSTANT_KEYS + ("ap", "drev", "drev_cur", "drev_nc", "ppegt", "gdwl", "intan", "intan_fin",
                                   "intan_ind", "intan_gw", "buyback_auth", "buyback_rem")
# Concepts whose appearance in a filing defines that filing's own statement period end.
MAIN_STATEMENT_CONCEPTS = ("Assets", "Liabilities", "StockholdersEquity", "LiabilitiesAndStockholdersEquity",
                           "NetIncomeLoss", "ProfitLoss", "Revenues",
                           "RevenueFromContractWithCustomerExcludingAssessedTax",
                           "NetCashProvidedByUsedInOperatingActivities", "OperatingIncomeLoss")
MAIN_STATEMENT_EXT = _ifrs("Assets", "Liabilities", "Equity", "EquityAttributableToOwnersOfParent",
                           "EquityAndLiabilities", "ProfitLoss", "ProfitLossAttributableToOwnersOfParent",
                           "Revenue", "CashFlowsFromUsedInOperatingActivities", "ProfitLossFromOperatingActivities")


POS_TAGS = ("CostOfGoodsAndServicesSold", "CostOfRevenue", "CostOfGoodsSold", "CostOfServices")
# flows whose presented-but-blank current-period value is read as zero (nil-as-zero-v1)
NIL_CHAINS = ("capx", "txt", "xrd", "dvc", "prstkc", "sstk", "xint", "dvt")


def split_concept(name: str) -> tuple[str, str]:
    """``'ifrs-full:Revenue'`` -> ``('ifrs-full', 'Revenue')``; a plain name is us-gaap."""
    if ":" in name:
        tax, concept = name.split(":", 1)
        return tax, concept
    return USGAAP, name


def concept_table() -> list[tuple[int, str, str, str, str, bool]]:
    """(cid, taxonomy, concept, unit_kind, kind, period_defining) for every concept the stage reads.

    The v1 concepts keep their v1 ids and order (cid order fixes the application order of same-clock
    facts). ``unit_kind`` is ``shares`` or ``money`` (any ISO currency). ``period_defining`` concepts may
    set a filing's own period end when it carries no main-statement concept: the v1 us-gaap concepts and
    every ``ifrs-full`` concept (v2 us-gaap extension and class-sum concepts never do).
    """
    names: list[str] = []
    for chain in CHAINS.values():
        for c in chain:
            if c not in names:
                names.append(c)
    for c in MAIN_STATEMENT_CONCEPTS:
        if c not in names:
            names.append(c)
    rows: list[tuple[int, str, str, str, str, bool]] = []
    instants = {c for k in INSTANT_KEYS for c in CHAINS[k]} | {"LiabilitiesAndStockholdersEquity"}
    for i, c in enumerate(names):
        unit = "shares" if c in SHARE_CONCEPTS else "money"
        rows.append((i, USGAAP, c, unit, "instant" if c in instants else "duration", True))
    rows.append((len(names), DEI, DEI_SHARES, "shares", "instant", True))
    seen = {(USGAAP, c) for c in names} | {(DEI, DEI_SHARES)}
    ext_instants = {c for k in INSTANT_KEYS_EXT for c in CHAINS_EXT.get(k, ())} | set(CLS_INSTANT) | {
        f"{IFRS}:EquityAndLiabilities", f"{IFRS}:Equity", f"{IFRS}:EquityAttributableToOwnersOfParent",
        f"{IFRS}:Assets", f"{IFRS}:Liabilities"}
    ext_names = [c for chain in CHAINS_EXT.values() for c in chain] + list(MAIN_STATEMENT_EXT) + list(
        CLS_CONCEPTS.values())
    for full in ext_names:
        tax, c = split_concept(full)
        if (tax, c) in seen:
            continue
        seen.add((tax, c))
        unit = "shares" if full in SHARE_CONCEPTS_EXT else "money"
        rows.append((len(rows), tax, c, unit, "instant" if full in ext_instants else "duration", tax == IFRS))
    return rows


CONCEPTS = concept_table()
CID: dict[str, int] = {}
for _cid, _tax, _c, _u, _k, _p in CONCEPTS:
    CID[_c if _tax in (USGAAP, DEI) else f"{_tax}:{_c}"] = _cid
del _cid, _tax, _c, _u, _k, _p
CHAIN_IDS: dict[str, tuple[int, ...]] = {k: tuple(CID[c] for c in v) for k, v in CHAINS.items()}
CHAIN_EXT_IDS: dict[str, tuple[int, ...]] = {k: tuple(CID[c] for c in v) for k, v in CHAINS_EXT.items()}
CLS_IDS = {k: CID[v] for k, v in CLS_CONCEPTS.items()}
DEI_CID = CID[DEI_SHARES]
SHARE_CIDS = frozenset(cid for cid, _, _, u, _, _ in CONCEPTS if u == "shares")
FIRST_TRACKED = frozenset(CID[c] for c in SHARE_CONCEPTS) | frozenset(
    CID[c] for c in SHARE_CONCEPTS_EXT if not c.startswith(CLS + ":"))
CLS_CIDS = frozenset(CLS_IDS.values())
PERIOD_DEF_CIDS = frozenset(cid for cid, _, _, _, _, p in CONCEPTS if p)
MAIN_CIDS = frozenset(CID[c] for c in MAIN_STATEMENT_CONCEPTS) | frozenset(CID[c] for c in MAIN_STATEMENT_EXT)
NI_CIDS = frozenset(CHAIN_IDS["ni"]) | frozenset(CHAIN_EXT_IDS["ni"])
NIL_CID_CHAIN = {cid: name for name in NIL_CHAINS for cid in CHAIN_IDS.get(name, ()) + CHAIN_EXT_IDS.get(name, ())}
RAW_SHARE_CIDS = (DEI_CID, CID["CommonStockSharesOutstanding"], CID[f"{IFRS}:NumberOfSharesOutstanding"])

ITEM_COLUMNS = (
    "at", "lt", "che", "debt", "be", "seq", "sale_q", "sale_ttm", "cogs_ttm", "xsga_ttm", "gp_ttm", "oi_ttm",
    "ni_q", "ni_ttm", "cfo_ttm", "capx_ttm", "xrd_ttm", "dvc_ttm", "prstkc_ttm", "sstk_ttm", "dp_ttm", "txt_q",
    "shrs_q", "noa", "invt", "rect", "ppe",
    "at_lag4", "be_lag1q", "be_lag1q_lag4", "ni_q_lag4", "txt_q_lag4", "shrs_q_lag4", "noa_lag4", "sale_q_lag4",
    "sue", "fscore",
)
FSCORE_TERMS = ("f_roa", "f_cfo", "f_droa", "f_accrual", "f_dlever", "f_dliquid", "f_eq_offer", "f_dmargin",
                "f_dturn")
# v2 additive numeric columns (D6 items, flows' discrete quarters, F-score terms)
ITEM_COLUMNS_V2 = (
    "cogs_q", "xsga_q", "gp_q", "oi_q", "xint_q", "xint_ttm", "dp_q", "ebitda_q", "ebitda_ttm", "dvt_q", "dvt_ttm",
    "act", "lct", "ap", "drev", "ppegt", "gdwl", "intan", "mib", "pstk", "buyback_authorized", "buyback_remaining",
) + FSCORE_TERMS + ("fscore_n", "fscore_partial")
ALL_ITEMS = ITEM_COLUMNS + ITEM_COLUMNS_V2
# items computed in the reporting currency's knowledge (currency invariant)
UNITLESS_ITEMS = ("sue", "fscore") + FSCORE_TERMS + ("fscore_n", "fscore_partial")
SHARE_ITEMS = ("shrs_q", "shrs_q_lag4")
MONEY_ITEMS = tuple(c for c in ALL_ITEMS if c not in UNITLESS_ITEMS and c not in SHARE_ITEMS)
# v2 text/flag columns
SRC_COLUMNS = ("sale_src", "gp_src", "oi_src", "shrs_src")
FLAG_COLUMNS = ("currency", "fin_template", "staleness_days", "xrd_reported_zero", "zero_filled") + SRC_COLUMNS
# quarterly_history items: discrete-quarter flows and period-end stocks
HISTORY_FLOWS = ("sale_q", "cogs_q", "gp_q", "xsga_q", "oi_q", "ebitda_q", "xint_q", "ni_q", "txt_q", "xrd_q",
                 "dp_q", "cfo_q", "capx_q", "dvc_q", "prstkc_q", "sstk_q", "dvt_q")
HISTORY_STOCKS = ("at", "lt", "che", "debt", "be", "seq", "act", "lct", "invt", "rect", "ap", "drev", "ppe", "ppegt",
                  "gdwl", "intan", "mib", "pstk", "shrs")
HISTORY_ITEMS = HISTORY_FLOWS + HISTORY_STOCKS

# Period and tolerance policy (days).
Q_MIN, Q_MAX = 80, 120          # a discrete quarter: 12-17 weeks (13/14-week, 12/16-week retail calendars)
FY_MIN, FY_MAX = 350, 380       # a fiscal year: 52 or 53 weeks, calendar years
YEAR_TOL = 20                   # year-ago period end within +-20 days of end - 365
CHAIN_TOL = 10                  # previous quarter end within +-10 days of start - 1
PREV_Q_TARGET, PREV_Q_TOL = 91, 25
CARRY_DAYS = 400                # txditc / preferred carry-forward window for be
SHARE_COVER_BEFORE, SHARE_COVER_AFTER = 15, 120
SPLIT_TOL = 0.02
SHARE_MAX_LOG10_CHANGE = 2.0
FLOW_ZERO_LOOKBACK = 460        # zero-fill only if the chain has no fact ending this recently
SUE_WINDOW, SUE_MIN = 8, 4
CLS_TOL = 16                    # FSDS ddate is rounded to the nearest month end
QUARTERLY_LOOKBACK = 400        # staleness_days = 200 if a 10-Q/10-QT (/A) clock in (clock - 400d, clock]
STALE_QUARTERLY, STALE_ANNUAL = 200, 400
FSCORE_PARTIAL_MIN = 6
QUARTERLY_FORMS = frozenset({"10-Q", "10-QT", "10-Q/A", "10-QT/A"})

# ---------------------------------------------------------------------------
# Industry template (SIC in force at the event) and structural NaN
# ---------------------------------------------------------------------------

TEMPLATES = ("bank", "insurer", "reit", "utility", "other")
TEMPLATE_RULE = ("SIC in force (latest FSDS SUB SIC of the issuer's filings up to the event): 6000-6299 bank "
                 "(depository and non-depository credit institutions, security and commodity brokers, dealers, "
                 "exchanges); 6300-6399 insurer (insurance carriers: life, health and medical plans, property "
                 "and casualty, surety, title); 6798 reit; 4900-4949 utility (electric, gas, combination, water: "
                 "the FF12 Utils range); anything else, and an issuer with no SIC yet, other")
OI_FALLBACK_BLOCKED_SIC = (6000, 6199)  # interest expense is an operating cost of credit institutions

_BANK_LIKE = frozenset({"cogs_ttm", "cogs_q", "gp_ttm", "gp_q", "invt", "act", "lct", "f_dliquid", "f_dmargin",
                        "fscore"})
STRUCTURAL: dict[str, frozenset[str]] = {
    "bank": _BANK_LIKE | {"xsga_ttm", "xsga_q", "oi_ttm", "oi_q", "ebitda_ttm", "ebitda_q", "rect", "ap"},
    "insurer": _BANK_LIKE | {"xsga_ttm", "xsga_q"},
    "reit": _BANK_LIKE,
    "utility": frozenset({"gp_ttm", "gp_q", "cogs_ttm", "cogs_q", "f_dmargin", "fscore"}),
    "other": frozenset(),
}


def fin_template(sic: int | None) -> str:
    if sic is None:
        return "other"
    if 6000 <= sic <= 6299:
        return "bank"
    if 6300 <= sic <= 6399:
        return "insurer"
    if sic == 6798:
        return "reit"
    if 4900 <= sic <= 4949:
        return "utility"
    return "other"


def is_structural(template: str, item: str) -> bool:
    return item in STRUCTURAL.get(template, frozenset())


# ---------------------------------------------------------------------------
# FSDS PRE statement flags (built by fundamentals.prepare): which lines a filing's own statements show
# ---------------------------------------------------------------------------

PRE_FLAGS = ("has_is", "has_cf", "is_rd", "is_rev", "is_tax", "is_int", "cf_capx")


def _dur_days(start: dt.date, end: dt.date) -> int:
    return (end - start).days + 1


def _near_offsets(tol: int) -> Iterable[int]:
    yield 0
    for d in range(1, tol + 1):
        yield -d
        yield d


class Knowledge:
    """Latest-clock value of every retained fact of one issuer."""

    __slots__ = ("by_end", "by_start", "cls_clock", "dur", "inst", "present", "share_changes", "share_first",
                 "share_last")

    def __init__(self) -> None:
        self.dur: dict[int, dict[tuple[dt.date, dt.date], float]] = {}
        self.by_end: dict[int, dict[dt.date, set[dt.date]]] = {}
        self.by_start: dict[int, dict[dt.date, set[dt.date]]] = {}
        self.inst: dict[int, dict[dt.date, float]] = {}
        # share-count keys: latest (value, clock) and the ledger of restatements (split evidence)
        self.share_last: dict[tuple[int, dt.date | None, dt.date], tuple[float, dt.datetime]] = {}
        self.share_first: dict[tuple[int, dt.date | None, dt.date], tuple[float, dt.datetime]] = {}
        self.share_changes: list[tuple[dt.datetime, dt.datetime, float]] = []
        self.cls_clock: dict[tuple[int, dt.date | None, dt.date], dt.datetime] = {}
        self.present: set[int] = set()

    def apply(self, cid: int, start: dt.date | None, end: dt.date, value: float,
              clock: dt.datetime | None = None) -> None:
        self.present.add(cid)
        if cid in FIRST_TRACKED and clock is not None:
            key = (cid, start, end)
            prev = self.share_last.get(key)
            self.share_first.setdefault(key, (value, clock))
            if (prev is not None and cid != DEI_CID and prev[0] > 0 and value > 0 and prev[1] < clock
                    and abs(math.log(value / prev[0])) > math.log(1.0 + SPLIT_TOL)):
                self.share_changes.append((clock, prev[1], value / prev[0]))
            if prev is None or prev[1] <= clock:
                self.share_last[key] = (value, clock)
        elif cid in CLS_CIDS and clock is not None:
            self.cls_clock[(cid, start, end)] = clock
        if start is None:
            self.inst.setdefault(cid, {})[end] = value
            return
        self.dur.setdefault(cid, {})[(start, end)] = value
        self.by_end.setdefault(cid, {}).setdefault(end, set()).add(start)
        self.by_start.setdefault(cid, {}).setdefault(start, set()).add(end)


class Snapshot:
    """Item arithmetic over one issuer's knowledge at one event (memoised per event).

    Every public lookup runs on the v1 tier (``name``) first and on the extension tier (``name~x``) only
    when the v1 tier has no value for that same lookup.
    """

    def __init__(self, k: Knowledge) -> None:
        self.k = k
        self.memo: dict[tuple, Any] = {}
        self.chains: dict[str, tuple[int, ...]] = {
            name: tuple(c for c in ids if c in k.present) for name, ids in CHAIN_IDS.items()}
        for name, ids in CHAIN_EXT_IDS.items():
            self.chains[name + X] = tuple(c for c in ids if c in k.present)
            self.chains.setdefault(name, ())

    def tiers(self, name: str) -> tuple[str, ...]:
        return (name, name + X) if name + X in self.chains else (name,)

    # -- instants ---------------------------------------------------------
    def _inst(self, key: str, end: dt.date) -> float | None:
        for cid in self.chains[key]:
            v = self.k.inst.get(cid, {}).get(end)
            if v is not None:
                return v
        return None

    def inst(self, name: str, end: dt.date | None) -> float | None:
        if end is None:
            return None
        for key in self.tiers(name):
            v = self._inst(key, end)
            if v is not None:
                return v
        return None

    def inst_carry(self, name: str, end: dt.date, days: int) -> float | None:
        """Value at ``end``, else the latest value at an earlier end within ``days`` (per tier)."""
        for key in self.tiers(name):
            v = self._inst(key, end)
            if v is not None:
                return v
            best_end, best = None, None
            lo = end - dt.timedelta(days=days)
            for cid in self.chains[key]:
                for e, val in self.k.inst.get(cid, {}).items():
                    if lo <= e < end and (best_end is None or e > best_end):
                        best_end, best = e, val
                if best is not None:
                    return best
        return None

    def inst_ends(self, names: Sequence[str]) -> set[dt.date]:
        out: set[dt.date] = set()
        for name in names:
            for key in self.tiers(name):
                for cid in self.chains[key]:
                    out.update(self.k.inst.get(cid, {}).keys())
        return out

    # -- durations --------------------------------------------------------
    def q_one(self, cid: int, end: dt.date) -> tuple[float, dt.date] | None:
        """Discrete fiscal quarter ending at ``end`` for one concept: (value, quarter start)."""
        starts = self.k.by_end.get(cid, {}).get(end)
        if not starts:
            return None
        dur = self.k.dur[cid]
        for s in sorted(starts):
            if Q_MIN <= _dur_days(s, end) <= Q_MAX:
                return dur[(s, end)], s
        by_start = self.k.by_start[cid]
        for s in sorted(starts):
            n = _dur_days(s, end)
            if Q_MAX < n <= FY_MAX:
                for e0 in sorted(by_start.get(s, ())):
                    if Q_MIN <= (end - e0).days <= Q_MAX:
                        return dur[(s, end)] - dur[(s, e0)], e0 + DAY
        return None

    def _q(self, key: str, end: dt.date) -> tuple[float, dt.date] | None:
        mk = ("q", key, end)
        if mk in self.memo:
            return self.memo[mk]
        out = None
        for cid in self.chains[key]:
            out = self.q_one(cid, end)
            if out is not None:
                break
        self.memo[mk] = out
        return out

    def q(self, name: str, end: dt.date) -> tuple[float, dt.date] | None:
        for key in self.tiers(name):
            out = self._q(key, end)
            if out is not None:
                return out
        return None

    def qv(self, name: str, end: dt.date | None) -> float | None:
        if end is None:
            return None
        r = self.q(name, end)
        return None if r is None else r[0]

    def _annual(self, key: str, end: dt.date) -> float | None:
        for cid in self.chains[key]:
            starts = self.k.by_end.get(cid, {}).get(end)
            if starts:
                for s in sorted(starts):
                    if FY_MIN <= _dur_days(s, end) <= FY_MAX:
                        return self.k.dur[cid][(s, end)]
        return None

    def ttm(self, name: str, end: dt.date | None) -> float | None:
        if end is None:
            return None
        for key in self.tiers(name):
            v = self._ttm_key(key, end)
            if v is not None:
                return v
        return None

    def _ttm_key(self, key: str, end: dt.date) -> float | None:
        mk = ("ttm", key, end)
        if mk in self.memo:
            return self.memo[mk]
        out = self._ttm(key, end)
        self.memo[mk] = out
        return out

    def _ttm(self, key: str, end: dt.date) -> float | None:
        if not self.chains[key]:
            return None
        fy = self._annual(key, end)
        if fy is not None:
            return fy
        cur = self._q(key, end)
        if cur is not None:
            total, start, ok = cur[0], cur[1], True
            for _ in range(3):
                prev = None
                target = start - DAY
                for off in _near_offsets(CHAIN_TOL):
                    prev = self._q(key, target + dt.timedelta(days=off))
                    if prev is not None:
                        break
                if prev is None:
                    ok = False
                    break
                total += prev[0]
                start = prev[1]
            if ok:
                return total
        # YTD + prior FY - prior same YTD, within one concept.
        for cid in self.chains[key]:
            starts = self.k.by_end.get(cid, {}).get(end)
            if not starts:
                continue
            dur = self.k.dur[cid]
            for s in sorted(starts):
                n = _dur_days(s, end)
                if not (Q_MIN <= n < FY_MIN):
                    continue
                for off in _near_offsets(CHAIN_TOL):
                    fy_end = s - DAY + dt.timedelta(days=off)
                    fy_starts = self.k.by_end[cid].get(fy_end)
                    if not fy_starts:
                        continue
                    for fs in sorted(fy_starts):
                        if not (FY_MIN <= _dur_days(fs, fy_end) <= FY_MAX):
                            continue
                        for off2 in _near_offsets(YEAR_TOL):
                            pe = end - dt.timedelta(days=365 - off2)
                            for ps in sorted(self.k.by_end[cid].get(pe, ())):
                                if abs((ps - fs).days) <= CHAIN_TOL and abs(_dur_days(ps, pe) - n) <= CHAIN_TOL:
                                    return dur[(s, end)] + dur[(fs, fy_end)] - dur[(ps, pe)]
        return None

    def has_recent_flow(self, name: str, end: dt.date, days: int) -> bool:
        """Any non-zero fact of the chain (either tier; durations by end, instants by date) within ``days``.

        Only zero fills consult this, so a recent reported (or nil-as-zero) 0 never blocks a zero fill."""
        lo = end - dt.timedelta(days=days)
        for key in self.tiers(name):
            for cid in self.chains[key]:
                dur = self.k.dur.get(cid, {})
                for e, starts in self.k.by_end.get(cid, {}).items():
                    if lo <= e <= end and any(dur[(s, e)] != 0 for s in starts):
                        return True
                for e, v in self.k.inst.get(cid, {}).items():
                    if lo <= e <= end and v != 0:
                        return True
        return False

    def year_ago_q_end(self, name: str, end: dt.date) -> dt.date | None:
        target = end - dt.timedelta(days=365)
        for key in self.tiers(name):
            for off in _near_offsets(YEAR_TOL):
                d = target + dt.timedelta(days=off)
                if self._q(key, d) is not None:
                    return d
        return None

    def ttm_year_ago(self, name: str, end: dt.date) -> float | None:
        target = end - dt.timedelta(days=365)
        for key in self.tiers(name):
            for off in _near_offsets(YEAR_TOL):
                v = self._ttm_key(key, target + dt.timedelta(days=off))
                if v is not None:
                    return v
        return None


def _near_date(dates: set[dt.date], target: dt.date, tol: int) -> dt.date | None:
    for off in _near_offsets(tol):
        d = target + dt.timedelta(days=off)
        if d in dates:
            return d
    return None


def _add(*vals: float | None) -> float:
    return float(sum(v for v in vals if v is not None))


class ItemCalc:
    """All stage-F items for one issuer at one event period end."""

    def __init__(self, snap: Snapshot) -> None:
        self.s = snap
        self.bs_ends = snap.inst_ends(("at", "seq", "seq_nci"))

    # balance-sheet blocks at an exact end -----------------------------------
    def bs(self, end: dt.date | None) -> dict[str, float | None]:
        s = self.s
        if end is None:
            return {}
        key = ("bs", end)
        if key in s.memo:
            return s.memo[key]
        at = s.inst("at", end)
        seq_rep = s.inst("seq", end)
        seq_nci = s.inst("seq_nci", end)
        mib_rep = s.inst("mib", end)
        lt_rep = s.inst("lt", end)
        mib = mib_rep if mib_rep is not None else (seq_nci - seq_rep if seq_nci is not None and seq_rep is not None else None)
        seq = seq_rep
        if seq is None and seq_nci is not None:
            seq = seq_nci - (mib_rep or 0.0)
        if seq is None and at is not None and lt_rep is not None:
            seq = at - lt_rep
        lt = lt_rep
        if lt is None and at is not None:
            if seq_nci is not None:
                lt = at - seq_nci
            elif seq_rep is not None:
                lt = at - seq_rep - (mib or 0.0)
        cash_sti = s.inst("cash_sti", end)
        if cash_sti is not None:
            che = cash_sti
        else:
            cash, sti = s.inst("cash", end), s.inst("sti", end)
            che = None if cash is None and sti is None else _add(cash, sti)
        dcur, cur_ltd, stb = s.inst("debt_current", end), s.inst("cur_ltd", end), s.inst("stb", end)
        st = dcur if dcur is not None else (None if cur_ltd is None and stb is None else _add(cur_ltd, stb))
        ltd_nc = s.inst("ltd_nc", end)
        if ltd_nc is None:
            ltd_total = s.inst("ltd_total", end)
            if ltd_total is not None:
                ltd_nc = ltd_total - (cur_ltd or 0.0)
        if at is not None:
            debt = _add(st, ltd_nc)
            ltd_nc_z = ltd_nc if ltd_nc is not None else 0.0
        else:
            debt = None if st is None and ltd_nc is None else _add(st, ltd_nc)
            ltd_nc_z = ltd_nc
        txditc = s.inst_carry("txditc", end, CARRY_DAYS)
        pstk = s.inst_carry("pstk", end, CARRY_DAYS)
        be = None if seq is None else seq + (txditc or 0.0) - (pstk or 0.0)
        noa = None
        if at is not None and seq is not None:
            ps = pstk or 0.0
            ceq = seq - ps
            noa = (at - (che or 0.0)) - (at - (debt or 0.0) - (mib or 0.0) - ps - ceq)
        out = {"at": at, "lt": lt, "che": che, "debt": debt, "be": be, "seq": seq, "noa": noa,
               "ltd_nc": ltd_nc_z, "act": s.inst("act", end), "lct": s.inst("lct", end), "mib": mib,
               "pstk": pstk, "seq_nci": seq_nci}
        s.memo[key] = out
        return out

    def bs_near(self, target: dt.date, tol: int) -> tuple[dt.date | None, dict[str, float | None]]:
        d = _near_date(self.bs_ends, target, tol)
        return d, (self.bs(d) if d is not None else {})


def _plausible_shares(a: float | None, b: float | None) -> bool:
    if a is None or b is None or not (a > 0 and b > 0):
        return False
    return abs(math.log10(a / b)) < SHARE_MAX_LOG10_CHANGE


def _cls_near(snap: Snapshot, cid: int, end: dt.date, instant: bool) -> tuple[float, tuple] | None:
    k = snap.k
    if instant:
        vals = k.inst.get(cid, {})
        d = _near_date(set(vals), end, CLS_TOL) if vals else None
        return (vals[d], (cid, None, d)) if d is not None else None
    ends = k.by_end.get(cid, {})
    d = _near_date(set(ends), end, CLS_TOL) if ends else None
    if d is None:
        return None
    best_s = min(ends[d], key=lambda s: abs(_dur_days(s, d) - 91))
    return k.dur[cid][(best_s, d)], (cid, best_s, d)


def shares_at(snap: Snapshot, end: dt.date, *, latest_cover: bool = True,
              basis: str | None = None) -> tuple[float | None, tuple | None, str | None]:
    """Common shares for the fiscal period ending ``end``: (value, knowledge key, basis).

    dei cover count dated in [end - 15d, end + 120d] (the latest such cover for the current
    period, the earliest -- the period's own filing -- for a lag), else balance-sheet
    CommonStockSharesOutstanding at ``end``, else weighted-average basic (diluted) shares of the
    period ending at ``end``; v2 fallbacks (only when all of those are missing): the FSDS class-of-stock
    sum of balance-sheet shares (``cls_cso``), else of weighted-average basic (diluted) shares
    (``cls_waso``), dated within 16 days of ``end``. ``basis`` restricts the lookup to one source so a lag
    is measured like the current value.
    """
    k = snap.k
    dei = k.inst.get(DEI_CID, {})
    if dei and basis in (None, "dei"):
        lo = end - dt.timedelta(days=SHARE_COVER_BEFORE)
        hi = end + dt.timedelta(days=SHARE_COVER_AFTER)
        cands = [e for e in dei if lo <= e <= hi]
        if cands:
            best = max(cands) if latest_cover else min(cands)
            return dei[best], (DEI_CID, None, best), "dei"
    if basis in (None, "cso"):
        for key in snap.tiers("cso"):
            for cid in snap.chains[key]:
                v = k.inst.get(cid, {}).get(end)
                if v is not None:
                    return v, (cid, None, end), "cso"
    if basis in (None, "waso"):
        for name in ("waso_basic", "waso_diluted"):
            for key in snap.tiers(name):
                for cid in snap.chains[key]:
                    starts = k.by_end.get(cid, {}).get(end)
                    if starts:
                        best_s = min(starts, key=lambda s: abs(_dur_days(s, end) - 91))
                        return k.dur[cid][(best_s, end)], (cid, best_s, end), "waso"
    if basis in (None, "cls_cso") and CLS_IDS["cls_cso"] in k.present:
        r = _cls_near(snap, CLS_IDS["cls_cso"], end, True)
        if r is not None:
            return r[0], r[1], "cls_cso"
    if basis in (None, "cls_waso"):
        for name in ("cls_waso_basic", "cls_waso_diluted"):
            if CLS_IDS[name] in k.present:
                r = _cls_near(snap, CLS_IDS[name], end, False)
                if r is not None:
                    return r[0], r[1], "cls_waso"
    return None, None, None


SPLIT_GROUP_TOL = math.log(1.05)   # restatements in one filing agreeing within 5% form one split
SPLIT_JUMP_TOL = math.log(1.35)    # raw share jump across the split window must agree within 35%
SPLIT_MIN_LOG = math.log(1.09)     # smaller restatement ratios are corrections, not splits


def _split_like(r: float) -> bool:
    x = r if r >= 1.0 else 1.0 / r
    if x < 1.2:
        return False
    for nice in (1.25, 1.5, 2.5):
        if abs(x / nice - 1.0) < 0.01:
            return True
    return x >= 1.9 and abs(x - round(x)) / x < 0.01


def split_events(k: Knowledge) -> list[tuple[dt.datetime, dt.datetime, float]]:
    """Share splits evidenced by restated share counts: (lower_clock, restating_clock, ratio).

    A filing at clock ``t`` that restates share-count keys (weighted-average or balance-sheet
    shares) by a common ratio ``r`` evidences a split effective after the latest clock at which
    one of those keys was still reported on the old basis (``lower``) and by ``t``. Evidence with
    overlapping windows and the same ratio is one split. Ratios within 9% of 1 are ignored (not
    splits). A split is accepted when a first-reported share series (dei cover, current balance-sheet
    shares, current-quarter weighted-average shares) jumps by ``r`` between consecutive observations
    inside the window (within 15%) or across the window (within 35%); a pure scale factor (10^3,
    10^6) needs the dei or balance-sheet series, since XBRL scale errors show up in the
    weighted-average series itself. With no series around the window, ``r`` must be split-like
    (integer, 1.25, 1.5 or 2.5 or their inverses).
    """
    if not k.share_changes:
        return []
    by_clock: dict[dt.datetime, list[tuple[dt.datetime, float]]] = {}
    for t, t_prev, r in k.share_changes:
        by_clock.setdefault(t, []).append((t_prev, r))
    raw: list[tuple[dt.datetime, float, float]] = []  # (restating clock, lower, ratio) -> merged below
    for t in sorted(by_clock):
        entries = by_clock[t]
        logs = [math.log(r) for _, r in entries]
        # the ratio agreed by the most restated keys in this filing (ties: earliest entry)
        center = max(logs, key=lambda x: sum(abs(y - x) < SPLIT_GROUP_TOL for y in logs))
        agree = [(tp, lr) for (tp, _), lr in zip(entries, logs) if abs(lr - center) < SPLIT_GROUP_TOL]
        med = statistics.median(lr for _, lr in agree)
        if abs(med) >= SPLIT_MIN_LOG:
            raw.append((t, max(tp for tp, _ in agree), math.exp(med)))
    merged: list[list] = []  # [lower, upper, ratio]
    for t, lower, r in raw:
        for m in merged:
            if abs(math.log(r / m[2])) < SPLIT_GROUP_TOL and max(lower, m[0]) < min(t, m[1]):
                m[0], m[1] = max(lower, m[0]), min(t, m[1])
                break
        else:
            merged.append([lower, t, r])
    # raw share observation series as first reported: dei cover counts, balance-sheet common shares
    # reported for a period ending within 200 days before the reporting clock (not comparatives)
    # and current-quarter weighted-average shares
    series: dict[int, list[tuple[dt.datetime, float]]] = {}
    for (cid, start, end), (v, clk) in k.share_first.items():
        if v <= 0:
            continue
        current = (clk.date() - end).days <= 200
        if (cid == DEI_CID or (cid in RAW_SHARE_CIDS and current)
                or (start is not None and current and Q_MIN <= _dur_days(start, end) <= Q_MAX)):
            series.setdefault(cid, []).append((clk, v))
    for obs in series.values():
        obs.sort()
    out = []
    for lower, upper, r in merged:
        seen_any, ok = False, False
        scale = abs(abs(math.log10(r)) - 3) < 0.01 or abs(abs(math.log10(r)) - 6) < 0.01
        for cid, obs in series.items():
            if scale and cid not in RAW_SHARE_CIDS:
                continue
            inside = [(c, v) for c, v in obs if lower <= c <= upper]
            before = [(c, v) for c, v in obs if c <= lower]
            after = [(c, v) for c, v in obs if c >= upper]
            if len(inside) >= 2 or (before and after):
                seen_any = True
            # a jump by r between consecutive observations inside the window
            for (_, v1), (_, v2) in zip(inside, inside[1:]):
                if abs(math.log((v2 / v1) / r)) < math.log(1.15):
                    ok = True
            # or across the whole window (tolerates other issuance in between)
            if before and after and abs(math.log((after[0][1] / before[-1][1]) / r)) < SPLIT_JUMP_TOL:
                ok = True
        if not seen_any:
            ok = _split_like(r)
        if ok:
            out.append((lower, upper, r))
    return out


def split_adjustment(events: list[tuple[dt.datetime, dt.datetime, float]], obs_clock: dt.datetime,
                     obs_value: float, current: float | None) -> float:
    """Factor converting a share count reported at ``obs_clock`` to the latest share basis."""
    f = 1.0
    for lower, upper, r in events:
        if lower >= obs_clock:
            f *= r
        elif obs_clock < upper and current and current > 0 and obs_value > 0:
            # reported inside the split window: side with the basis nearer the current count
            if abs(math.log(current / (obs_value * f * r))) < abs(math.log(current / (obs_value * f))):
                f *= r
    return f


def sue_value(fr_ni: dict[dt.date, float], end: dt.date) -> float | None:
    """Livnat-Mendenhall seasonal random walk on first-reported quarterly net income."""
    cur = fr_ni.get(end)
    if cur is None:
        return None
    keys = set(fr_ni)
    lag = _near_date(keys, end - dt.timedelta(days=365), YEAR_TOL)
    if lag is None:
        return None
    lo, hi = end - dt.timedelta(days=760), end - dt.timedelta(days=60)
    prior = sorted(t for t in keys if lo <= t <= hi)[-SUE_WINDOW:]
    diffs = []
    for t in prior:
        t4 = _near_date(keys, t - dt.timedelta(days=365), YEAR_TOL)
        if t4 is not None:
            diffs.append(fr_ni[t] - fr_ni[t4])
    if len(diffs) < SUE_MIN:
        return None
    sd = statistics.stdev(diffs)
    if not (sd > 0) or not math.isfinite(sd):
        return None
    return (cur - fr_ni[lag]) / sd


# ---------------------------------------------------------------------------
# Item functions with their v2 fallbacks (each returns the value and a source code)
# ---------------------------------------------------------------------------

class Ctx:
    """Per-event context of the item functions: template, SIC and FSDS PRE statement flags by period end."""

    __slots__ = ("pre", "sic", "template")

    def __init__(self, sic: int | None = None, pre: dict[dt.date, dict[str, bool]] | None = None) -> None:
        self.sic = sic
        self.template = fin_template(sic)
        self.pre = pre or {}

    def flags(self, end: dt.date | None, tol: int = 0) -> dict[str, bool] | None:
        if end is None or not self.pre:
            return None
        if tol == 0:
            return self.pre.get(end)
        d = _near_date(set(self.pre), end, tol)
        return self.pre.get(d) if d is not None else None

    @property
    def oi_fallback(self) -> bool:
        lo, hi = OI_FALLBACK_BLOCKED_SIC
        return not (self.sic is not None and lo <= self.sic <= hi)


def _no_line(ctx: Ctx, s: Snapshot, chain: str, flag: str, stmt: str, end: dt.date,
             knowledge_ok: bool | None, tol: int = 0, period_only: bool = False) -> str | None:
    """Zero-fill evidence for a missing flow: the chain has no fact ending within 460 days and either the
    filing's own statement ``stmt`` (FSDS PRE) exists without a ``flag`` line (``'pre'``), or -- no PRE
    flags for that period -- ``knowledge_ok`` (the statement is evidenced by other items; ``'knowledge'``).
    ``knowledge_ok=None`` disables the knowledge rule. ``period_only`` (discrete-quarter items): on the PRE
    path it suffices that the chain has no fact ending at ``end`` (that quarter's statement shows no line).
    Returns the evidence code or None."""
    recent = s.has_recent_flow(chain, end, FLOW_ZERO_LOOKBACK)
    f = ctx.flags(end, tol)
    if f is not None:
        if not (f.get(stmt) and not f.get(flag)):
            return None
        if not recent:
            return "pre"
        if period_only and not s.has_recent_flow(chain, end, 0):
            return "pre_period"
        return None
    if knowledge_ok and not recent:
        return "knowledge"
    return None


def sale_q(s: Snapshot, end: dt.date | None) -> float | None:
    """v1 sale_q: sale chain, else the bank fallback (both v1 tier first, then the extension tier)."""
    v = s.qv("sale", end)
    if v is None:
        bank = s.qv("int_inc", end)
        if bank is not None:
            v = bank + (s.qv("nonint_inc", end) or 0.0)
    return v


def sale_ttm(s: Snapshot, end: dt.date | None) -> float | None:
    v = s.ttm("sale", end)
    if v is None:
        bank = s.ttm("int_inc", end)
        if bank is not None:
            v = bank + (s.ttm("nonint_inc", end) or 0.0)
    return v


def _sale(s: Snapshot, ctx: Ctx, end: dt.date, quarterly: bool) -> tuple[float | None, str | None]:
    """Revenue: v1 chain, else the bank fallback (both on the v1 tier), else the extension tier, else 0
    when the filing's own income statement (FSDS PRE) has no revenue-like line and the revenue chain
    has no fact within 460 days (not for bank, insurer or reit templates)."""
    get = s._q if quarterly else s._ttm_key
    v = get("sale", end)
    v = v[0] if quarterly and v is not None else v
    if v is not None:
        return v, "chain"
    bank = get("int_inc", end)
    bank = bank[0] if quarterly and bank is not None else bank
    if bank is not None:
        nonint = s.qv("nonint_inc", end) if quarterly else s.ttm("nonint_inc", end)
        return bank + (nonint or 0.0), "bank_int_nonint"
    v = get("sale" + X, end)
    v = v[0] if quarterly and v is not None else v
    if v is not None:
        return v, "chain_ext"
    if ctx.template not in ("bank", "insurer", "reit") and _no_line(ctx, s, "sale", "is_rev", "has_is", end, None):
        return 0.0, "zero_no_revenue_line"
    return None, None


def _xsga(s: Snapshot, end: dt.date, quarterly: bool) -> float | None:
    f = s.qv if quarterly else s.ttm
    v = f("xsga", end)
    if v is None:
        ga = f("xga", end)
        if ga is not None:
            v = ga + (f("xsm", end) or 0.0)
    return v


def _cogs(s: Snapshot, end: dt.date, quarterly: bool) -> float | None:
    return s.qv("cogs", end) if quarterly else s.ttm("cogs", end)


def _gp(s: Snapshot, end: dt.date, sale: float | None, quarterly: bool) -> tuple[float | None, str | None]:
    """GrossProfit (v1, then IFRS), else revenue - cost of revenue (v1 chain, then the extension chain), else
    (Compustat-style cost of revenue for a statement with no cost-of-revenue line) revenue - (total costs and
    expenses - SG&A - R&D - D&A), missing deductions 0, only when that cost is >= 0."""
    f = s.qv if quarterly else s.ttm
    v = f("gp", end)
    if v is not None:
        return v, "GrossProfit"
    cogs = _cogs(s, end, quarterly)
    if sale is not None and cogs is not None:
        return sale - cogs, "sale_minus_cogs"
    if sale is not None:
        tc = f("opcost", end)
        if tc is not None:
            derived = tc - (_xsga(s, end, quarterly) or 0.0) - (f("xrd", end) or 0.0) - (f("dp", end) or 0.0)
            if derived >= 0:
                return sale - derived, "sale_minus_opcost_ex_sga_rd_dp"
    return None, None


def _xint(s: Snapshot, ctx: Ctx, end: dt.date, debt: float | None, quarterly: bool) -> tuple[float | None, bool]:
    """Interest expense chain, else 0 when the chain has no fact within 460 days and the filing's income
    statement has no interest-expense line (FSDS PRE) or, without PRE flags, debt at the period end is 0."""
    v = s.qv("xint", end) if quarterly else s.ttm("xint", end)
    if v is not None:
        return v, False
    if _no_line(ctx, s, "xint", "is_int", "has_is", end, debt is not None and debt == 0.0):
        return 0.0, True
    return None, False


def _oi(s: Snapshot, ctx: Ctx, end: dt.date, debt: float | None, quarterly: bool) -> tuple[float | None, str | None]:
    """OperatingIncomeLoss (v1; IFRS ProfitLossFromOperatingActivities), else (not SIC 6000-6199) the reported
    pre-interest EBIT concept, else pre-tax income from continuing operations + interest expense."""
    f = s.qv if quarterly else s.ttm
    v = f("oi", end)
    if v is not None:
        return v, "OperatingIncomeLoss"
    if not ctx.oi_fallback:
        return None, None
    v = f("ebit_rep", end)
    if v is not None:
        return v, "ebit_reported"
    p = f("pretax", end)
    if p is None:
        return None, None
    xi, zero = _xint(s, ctx, end, debt, quarterly)
    if xi is None:
        return None, None
    return p + xi, ("pretax_no_interest_line" if zero else "pretax_plus_interest")


def _zero_stock(s: Snapshot, names: Sequence[str], end: dt.date, at: float | None) -> bool:
    """0 for an unreported stock (goodwill, intangibles, minority interest): assets exist at the end and none
    of the chains has a fact within 460 days."""
    return at is not None and not any(s.has_recent_flow(n, end, FLOW_ZERO_LOOKBACK) for n in names)


def compute_items(k: Knowledge, end: dt.date, fr_ni: dict[dt.date, float],
                  counters: dict[str, int], ctx: Ctx | None = None) -> dict[str, Any]:
    ctx = ctx or Ctx()
    snap = Snapshot(k)
    calc = ItemCalc(snap)
    s = snap
    cur = calc.bs(end)
    out: dict[str, Any] = {c: None for c in ALL_ITEMS}
    zero: list[str] = []

    def bump(key: str) -> None:
        counters[key] = counters.get(key, 0) + 1

    for c in ("at", "lt", "che", "debt", "be", "seq", "noa"):
        out[c] = cur.get(c)
    out["invt"] = s.inst("invt", end)
    out["rect"] = s.inst("rect", end)
    out["ppe"] = s.inst("ppe", end)
    at = out["at"]

    out["sale_q"], _ = _sale(s, ctx, end, True)
    out["sale_ttm"], out["sale_src"] = _sale(s, ctx, end, False)
    if out["sale_src"] == "zero_no_revenue_line":
        zero.append("sale_ttm")
    out["cogs_ttm"] = _cogs(s, end, False)
    out["xsga_ttm"] = _xsga(s, end, False)
    out["gp_ttm"], out["gp_src"] = _gp(s, end, out["sale_ttm"], False)
    out["oi_ttm"], out["oi_src"] = _oi(s, ctx, end, out["debt"], False)
    out["ni_q"] = s.qv("ni", end)
    out["ni_ttm"] = s.ttm("ni", end)
    out["cfo_ttm"] = s.ttm("cfo", end)
    out["capx_ttm"] = s.ttm("capx", end)
    if out["capx_ttm"] is None and _no_line(ctx, s, "capx", "cf_capx", "has_cf", end, out["cfo_ttm"] is not None):
        out["capx_ttm"] = 0.0
        zero.append("capx_ttm")
    out["xrd_ttm"] = s.ttm("xrd", end)
    out["xrd_reported_zero"] = False
    if out["xrd_ttm"] is None:
        ev = _no_line(ctx, s, "xrd", "is_rd", "has_is", end, out["ni_ttm"] is not None)
        if ev:
            out["xrd_ttm"], out["xrd_reported_zero"] = 0.0, True
            zero.append("xrd_ttm")
            bump(f"xrd_zero_{ev}")
    out["dp_ttm"] = s.ttm("dp", end)
    out["txt_q"] = s.qv("txt", end)
    if out["txt_q"] is None and _no_line(ctx, s, "txt", "is_tax", "has_is", end, out["ni_q"] is not None,
                                         period_only=True):
        out["txt_q"] = 0.0
        zero.append("txt_q")
    for name in ("dvc", "prstkc", "sstk"):
        v = s.ttm(name, end)
        if v is None and out["cfo_ttm"] is not None and not s.has_recent_flow(name, end, FLOW_ZERO_LOOKBACK):
            v = 0.0
            counters[f"{name}_zero_filled"] = counters.get(f"{name}_zero_filled", 0) + 1
            zero.append(f"{name}_ttm")
        out[f"{name}_ttm"] = v

    # v2 flows: discrete quarters and D6 items
    out["cogs_q"] = _cogs(s, end, True)
    out["xsga_q"] = _xsga(s, end, True)
    out["gp_q"], _ = _gp(s, end, out["sale_q"], True)
    out["oi_q"], _ = _oi(s, ctx, end, out["debt"], True)
    out["xint_ttm"], z = _xint(s, ctx, end, out["debt"], False)
    if z:
        zero.append("xint_ttm")
    out["xint_q"], z = _xint(s, ctx, end, out["debt"], True)
    if z:
        zero.append("xint_q")
    out["dp_q"] = s.qv("dp", end)
    if out["oi_ttm"] is not None and out["dp_ttm"] is not None:
        out["ebitda_ttm"] = out["oi_ttm"] + out["dp_ttm"]
    if out["oi_q"] is not None and out["dp_q"] is not None:
        out["ebitda_q"] = out["oi_q"] + out["dp_q"]
    for key, q in (("dvt_ttm", False), ("dvt_q", True)):
        v = s.qv("dvt", end) if q else s.ttm("dvt", end)
        if v is None and out["dvc_ttm"] == 0.0 and not s.has_recent_flow("dvt", end, FLOW_ZERO_LOOKBACK):
            v = 0.0
            zero.append(key)
        out[key] = v

    # v2 stocks
    out["act"], out["lct"] = cur.get("act"), cur.get("lct")
    out["ap"] = s.inst("ap", end)
    drev = s.inst("drev", end)
    if drev is None:
        dc, dn = s.inst("drev_cur", end), s.inst("drev_nc", end)
        drev = None if dc is None and dn is None else _add(dc, dn)
    out["drev"] = drev
    out["ppegt"] = s.inst("ppegt", end)
    gw = s.inst("gdwl", end)
    if gw is None and _zero_stock(s, ("gdwl", "intan_gw"), end, at):
        gw = 0.0
        zero.append("gdwl")
    out["gdwl"] = gw
    intan = s.inst("intan", end)
    if intan is None:
        fin, ind = s.inst("intan_fin", end), s.inst("intan_ind", end)
        if fin is not None or ind is not None:
            intan = _add(fin, ind)
        else:
            tot = s.inst("intan_gw", end)
            if tot is not None and gw is not None:
                intan = tot - gw
    if intan is None and _zero_stock(s, ("intan", "intan_fin", "intan_ind", "intan_gw"), end, at):
        intan = 0.0
        zero.append("intan")
    out["intan"] = intan
    mib = cur.get("mib")
    if mib is None and _zero_stock(s, ("mib", "seq_nci"), end, at):
        mib = 0.0
        zero.append("mib")
    out["mib"] = mib
    pstk = cur.get("pstk")
    if pstk is None and at is not None:
        pstk = 0.0
        zero.append("pstk")
    out["pstk"] = pstk
    out["buyback_authorized"] = s.inst_carry("buyback_auth", end, CARRY_DAYS)
    out["buyback_remaining"] = s.inst_carry("buyback_rem", end, CARRY_DAYS)

    # shares
    shrs, skey, basis = shares_at(s, end)
    out["shrs_q"] = shrs
    out["shrs_src"] = basis
    if basis:
        counters[f"shrs_basis_{basis}"] = counters.get(f"shrs_basis_{basis}", 0) + 1

    # lags ------------------------------------------------------------------
    e4_bs, bs4 = calc.bs_near(end - dt.timedelta(days=365), YEAR_TOL)
    out["at_lag4"] = bs4.get("at")
    out["noa_lag4"] = bs4.get("noa")
    e1_bs, bs1 = calc.bs_near(end - dt.timedelta(days=PREV_Q_TARGET), PREV_Q_TOL)
    out["be_lag1q"] = bs1.get("be")
    if e1_bs is not None:
        _, bs15 = calc.bs_near(e1_bs - dt.timedelta(days=365), YEAR_TOL)
        out["be_lag1q_lag4"] = bs15.get("be")
    e4_ni = s.year_ago_q_end("ni", end)
    out["ni_q_lag4"] = s.qv("ni", e4_ni)
    e4_txt = s.year_ago_q_end("txt", end)
    out["txt_q_lag4"] = s.qv("txt", e4_txt)
    if out["txt_q_lag4"] is None and out["txt_q"] is not None:
        lag_end = e4_ni or (e4_bs if e4_bs is not None else None)
        if lag_end is not None and _no_line(ctx, s, "txt", "is_tax", "has_is", lag_end,
                                            s.qv("ni", lag_end) is not None, tol=YEAR_TOL, period_only=True):
            out["txt_q_lag4"] = 0.0
            zero.append("txt_q_lag4")
    out["sale_q_lag4"] = sale_q(s, s.year_ago_q_end("sale", end) or s.year_ago_q_end("int_inc", end))

    # year-ago shares, split-consistent with shrs_q
    e4_sh = e4_bs or e4_ni or (end - dt.timedelta(days=365))
    lag_raw, lag_key, _ = shares_at(s, e4_sh, latest_cover=False, basis=basis) if basis else (None, None, None)
    if lag_raw is not None:
        obs_clock = k.share_last[lag_key][1] if lag_key in k.share_last else k.cls_clock.get(lag_key)
        f = 1.0
        if obs_clock is not None:
            f = split_adjustment(split_events(k), obs_clock, lag_raw, shrs)
        if f != 1.0:
            counters["shrs_lag_split_adjusted"] = counters.get("shrs_lag_split_adjusted", 0) + 1
        lag = lag_raw * f
        if shrs is not None and not _plausible_shares(shrs, lag):
            counters["shrs_pair_implausible"] = counters.get("shrs_pair_implausible", 0) + 1
            lag = None
        out["shrs_q_lag4"] = lag

    out["sue"] = sue_value(fr_ni, end)

    # Piotroski F-score ------------------------------------------------------
    sig = fscore_signals(s, calc, ctx, end, out, e4_bs, bs4)
    out.update(sig)
    terms = [v for v in sig.values() if v is not None]
    out["fscore_n"] = float(len(terms))
    out["fscore_partial"] = float(sum(terms)) if len(terms) >= FSCORE_PARTIAL_MIN else None
    if len(terms) == len(FSCORE_TERMS):
        out["fscore"] = float(sum(terms))
    else:
        counters["fscore_incomplete"] = counters.get("fscore_incomplete", 0) + 1
    out["zero_filled"] = ",".join(sorted(zero))
    return out


def _ratio(a: float | None, b: float | None) -> float | None:
    if a is None or b is None or b == 0:
        return None
    return a / b


def _gt(a: float | None, b: float | None) -> int | None:
    if a is None or b is None:
        return None
    return int(a > b)


def piotroski_terms(ni: float | None, cfo: float | None, roa: float | None, roa4: float | None,
                    lev: float | None, lev4: float | None, cr: float | None, cr4: float | None,
                    sstk: float | None, gm: float | None, gm4: float | None, turn: float | None,
                    turn4: float | None) -> dict[str, int | None]:
    """Piotroski (2000) nine binary terms (1 good news, 0 bad news, None when an input is missing)."""
    return {
        "f_roa": None if ni is None else int(ni > 0),
        "f_cfo": None if cfo is None else int(cfo > 0),
        "f_droa": _gt(roa, roa4),
        "f_accrual": _gt(cfo, ni),
        "f_dlever": _gt(lev4, lev),
        "f_dliquid": _gt(cr, cr4),
        "f_eq_offer": None if sstk is None else int(sstk <= 0),
        "f_dmargin": _gt(gm, gm4),
        "f_dturn": _gt(turn, turn4),
    }


def fscore_signals(s: Snapshot, calc: ItemCalc, ctx: Ctx, end: dt.date, cur: dict[str, Any],
                   e4: dt.date | None, bs4: dict[str, float | None]) -> dict[str, int | None]:
    """Piotroski (2000) nine binary signals on trailing-twelve-month flows (t) versus t-4 quarters."""
    at4 = bs4.get("at")
    at8 = None
    if e4 is not None:
        _, bs8 = calc.bs_near(e4 - dt.timedelta(days=365), YEAR_TOL)
        at8 = bs8.get("at")
    ni, cfo = cur["ni_ttm"], cur["cfo_ttm"]
    ni_l4 = s.ttm_year_ago("ni", end)
    sale = cur["sale_ttm"] if cur.get("sale_src") != "zero_no_revenue_line" else None
    sale_l4 = s.ttm_year_ago("sale", end)
    if sale_l4 is None and cur.get("sale_src") == "bank_int_nonint":
        target = end - dt.timedelta(days=365)
        for off in _near_offsets(YEAR_TOL):
            sale_l4 = sale_ttm(s, target + dt.timedelta(days=off))
            if sale_l4 is not None:
                break
    gp = cur["gp_ttm"]
    gp_l4 = s.ttm_year_ago("gp", end)
    if gp_l4 is None and sale_l4 is not None:
        cogs_l4 = s.ttm_year_ago("cogs", end)
        gp_l4 = None if cogs_l4 is None else sale_l4 - cogs_l4
    bs_cur = calc.bs(end)
    lev = _ratio(bs_cur.get("ltd_nc"), bs_cur.get("at"))
    lev4 = _ratio(bs4.get("ltd_nc"), at4)
    cr = _ratio(bs_cur.get("act"), bs_cur.get("lct"))
    cr4 = _ratio(bs4.get("act"), bs4.get("lct"))
    roa, roa4 = _ratio(ni, at4), _ratio(ni_l4, at8)
    return piotroski_terms(ni, cfo, roa, roa4, lev, lev4, cr, cr4, cur["sstk_ttm"], _ratio(gp, sale),
                           _ratio(gp_l4, sale_l4), _ratio(sale, at4), _ratio(sale_l4, at8))


# ---------------------------------------------------------------------------
# Quarterly history: discrete-quarter flows and period-end stocks at every period a filing reports
# ---------------------------------------------------------------------------

def history_values(k: Knowledge, end: dt.date, ctx: Ctx) -> dict[str, tuple[float, bool]]:
    """{item: (value, zero_filled)} of the ``HISTORY_ITEMS`` derivable for the period ending ``end``."""
    s = Snapshot(k)
    calc = ItemCalc(s)
    b = calc.bs(end)
    out: dict[str, tuple[float, bool]] = {}

    def put(name: str, v: float | None, z: bool = False) -> None:
        if v is not None:
            out[name] = (v, z)

    sale, src = _sale(s, ctx, end, True)
    put("sale_q", sale, src == "zero_no_revenue_line")
    put("cogs_q", _cogs(s, end, True))
    put("gp_q", _gp(s, end, sale if src != "zero_no_revenue_line" else None, True)[0])
    put("xsga_q", _xsga(s, end, True))
    oi, _ = _oi(s, ctx, end, b.get("debt"), True)
    put("oi_q", oi)
    xi, z = _xint(s, ctx, end, b.get("debt"), True)
    put("xint_q", xi, z)
    ni = s.qv("ni", end)
    put("ni_q", ni)
    txt = s.qv("txt", end)
    if txt is None and _no_line(ctx, s, "txt", "is_tax", "has_is", end, ni is not None, period_only=True):
        put("txt_q", 0.0, True)
    else:
        put("txt_q", txt)
    put("xrd_q", s.qv("xrd", end))
    dp = s.qv("dp", end)
    put("dp_q", dp)
    if oi is not None and dp is not None:
        put("ebitda_q", oi + dp)
    for name in ("cfo", "capx", "dvc", "prstkc", "sstk", "dvt"):
        put(f"{name}_q", s.qv(name, end))
    for name in ("at", "lt", "che", "debt", "be", "seq", "act", "lct"):
        put(name, b.get(name))
    for name in ("invt", "rect", "ap", "ppe", "ppegt", "gdwl", "intan"):
        put(name, s.inst(name, end))
    drev = s.inst("drev", end)
    if drev is None:
        dc, dn = s.inst("drev_cur", end), s.inst("drev_nc", end)
        drev = None if dc is None and dn is None else _add(dc, dn)
    put("drev", drev)
    put("mib", b.get("mib"))
    put("pstk", b.get("pstk"))
    sh, _, _ = shares_at(s, end, latest_cover=False)
    put("shrs", sh)
    return out


# ---------------------------------------------------------------------------
# Event loop for one issuer and one money currency
# ---------------------------------------------------------------------------

def nil_zeros(k: Knowledge, durs: list[tuple[int, dt.date, dt.date]], own: dt.date | None, clock: dt.datetime,
              counters: dict[str, int]) -> int:
    """nil-as-zero-v1: a filing that reports a ``NIL_CHAINS`` flow for the year-ago comparative period (end
    within 20 days of own - 365) but for no current period of the same duration (no concept of that chain,
    either tier, ends at ``own`` in the knowledge) presented a blank current value, which Company Facts drops:
    enter 0 for that concept over the filing's own current period of the same duration (a start taken from
    the filing's other current-period facts, duration within 10 days), at the filing's clock -- never when a
    shorter period of that chain from the same start is non-zero (a year-to-date total does not return to 0)."""
    if own is None or not durs:
        return 0
    cur: dict[int, dt.date] = {}
    for cid, s0, e0 in durs:
        if e0 == own:
            cur.setdefault(_dur_days(s0, e0), s0)
    if not cur:
        return 0
    lo, hi = own - dt.timedelta(days=365 + YEAR_TOL), own - dt.timedelta(days=365 - YEAR_TOL)
    done: set[tuple[str, int]] = set()
    n = 0
    for cid, s0, e0 in durs:
        chain = NIL_CID_CHAIN.get(cid)
        if chain is None or not (lo <= e0 <= hi):
            continue
        cids = CHAIN_IDS.get(chain, ()) + CHAIN_EXT_IDS.get(chain, ())
        if any(own in k.by_end.get(c, {}) for c in cids):
            continue
        n0 = _dur_days(s0, e0)
        best = min(cur, key=lambda d: abs(d - n0))
        if abs(best - n0) > CHAIN_TOL or (chain, best) in done:
            continue
        done.add((chain, best))
        sc = cur[best]
        # a year-to-date total cannot fall back to zero: skip when a shorter period from the same start is non-zero
        if any(v != 0 for c in cids for (st, en), v in k.dur.get(c, {}).items() if st == sc and en < own):
            continue
        k.apply(cid, sc, own, 0.0, clock)
        n += 1
    if n:
        counters["nil_zero_facts"] = counters.get("nil_zero_facts", 0) + n
    return n


def reporting_currency(units: Counter) -> str | None:
    """Most frequent money unit of a filing (ties: USD, then alphabetical)."""
    if not units:
        return None
    return min(units.items(), key=lambda kv: (-kv[1], kv[0] != "USD", kv[0]))[0]


def staleness_days(quarterly_clocks: list[dt.datetime], clock: dt.datetime) -> int:
    """200 if a 10-Q/10-QT (or /A) accession has clock in (clock - 400 d, clock], else 400."""
    lo = clock - dt.timedelta(days=QUARTERLY_LOOKBACK)
    for c in reversed(quarterly_clocks):
        if c <= clock:
            if c > lo:
                return STALE_QUARTERLY
            break
    return STALE_ANNUAL


def company_events(cik: int, rows: Sequence[tuple], emit_from: dt.datetime, counters: dict[str, int],
                   money_unit: str = "USD", sic_by_accn: dict[str, int] | None = None,
                   pre_by_accn: dict[str, dict[str, bool]] | None = None,
                   history: list[tuple] | None = None) -> tuple[list[dict[str, Any]], dict[str, str | None]]:
    """Rows: (clock, accn, cid, start, end, value, unit, form, filed, fy, fp, basis) sorted by (clock, accn).

    Money facts enter the knowledge only in ``money_unit``; share facts always. Returns one output dict per
    (cik, accession) event with clock >= ``emit_from`` and the reporting currency of every accession.
    ``history`` (optional) receives quarterly-history tuples (item, period_end, fiscal_period, accession,
    value, clock, zero_filled) for every accession (all clocks) whenever a value is new or changed.
    """
    k = Knowledge()
    fr_ni: dict[dt.date, float] = {}
    labels: dict[dt.date, tuple[int | None, str | None]] = {}
    pre_by_pe: dict[dt.date, dict[str, bool]] = {}
    latest_pe: dt.date | None = None
    sic: int | None = None
    q_clocks: list[dt.datetime] = []
    currency_by_accn: dict[str, str | None] = {}
    last_currency: str | None = None
    hist_last: dict[tuple[str, dt.date], float] = {}
    out: list[dict[str, Any]] = []
    sic_by_accn = sic_by_accn or {}
    pre_by_accn = pre_by_accn or {}
    i, n = 0, len(rows)
    while i < n:
        clock = rows[i][0]
        j = i
        while j < n and rows[j][0] == clock:
            j += 1
        group = rows[i:j]
        i = j
        accns: dict[str, dict[str, Any]] = {}
        seen_keys: dict[tuple, float] = {}
        for (_, accn, cid, start, end, value, unit, form, filed, fy, fp, basis) in group:
            a = accns.get(accn)
            if a is None:
                a = accns[accn] = {"forms": {}, "filed": filed, "fy": {}, "basis": basis, "own_pe": None,
                                   "any_end": None, "ni_ends": set(), "units": Counter(), "ends": set(),
                                   "durs": []}
            a["forms"][form] = a["forms"].get(form, 0) + 1
            if fy is not None or fp is not None:
                a["fy"][(fy, fp)] = a["fy"].get((fy, fp), 0) + 1
            share = cid in SHARE_CIDS
            if not share:
                a["units"][unit] += 1
            if cid in MAIN_CIDS and (a["own_pe"] is None or end > a["own_pe"]):
                a["own_pe"] = end
            if cid != DEI_CID and cid in PERIOD_DEF_CIDS and (a["any_end"] is None or end > a["any_end"]):
                a["any_end"] = end
            if not (unit == "shares" if share else unit == money_unit):
                continue
            key = (cid, start, end)
            prev = seen_keys.get(key)
            if prev is not None and prev != value:
                counters["same_clock_conflicts"] = counters.get("same_clock_conflicts", 0) + 1
            seen_keys[key] = value
            k.apply(cid, start, end, value, clock)
            if cid != DEI_CID:
                a["ends"].add(end)
                if start is not None:
                    a["durs"].append((cid, start, end))
            if cid in NI_CIDS and start is not None:
                a["ni_ends"].add(end)
        # period end, fiscal labels, SIC, statement flags, reporting currency, quarterly filer clocks
        for accn in sorted(accns):
            a = accns[accn]
            own = a["own_pe"] or a["any_end"]
            if own is not None:
                if own not in labels and a["fy"]:
                    labels[own] = max(a["fy"].items(), key=lambda kv: kv[1])[0]
                if latest_pe is None or own > latest_pe:
                    latest_pe = own
                if accn in pre_by_accn:
                    pre_by_pe[own] = pre_by_accn[accn]
            a["own"] = own
            nil_zeros(k, a["durs"], a["own_pe"], clock, counters)
            if accn in sic_by_accn:
                sic = sic_by_accn[accn]
            cur = reporting_currency(a["units"])
            if cur is not None:
                last_currency = cur
            currency_by_accn[accn] = cur if cur is not None else last_currency
            form = max(a["forms"].items(), key=lambda kv: kv[1])[0]
            a["form"] = form
            if form in QUARTERLY_FORMS:
                q_clocks.append(clock)
        # first-reported quarterly net income
        new_ni_ends = set().union(*(a["ni_ends"] for a in accns.values()))
        if new_ni_ends:
            snap = Snapshot(k)
            for e in sorted(new_ni_ends):
                if e not in fr_ni:
                    v = snap.qv("ni", e)
                    if v is not None:
                        fr_ni[e] = v
        ctx = Ctx(sic, pre_by_pe)
        if history is not None:
            done: set[dt.date] = set()
            for accn in sorted(accns):
                for e in sorted(accns[accn]["ends"]):
                    if e in done:
                        continue
                    done.add(e)
                    for item, (v, z) in history_values(k, e, ctx).items():
                        hk = (item, e)
                        old = hist_last.get(hk)
                        if old is None or abs(v - old) > 1e-9 * max(1.0, abs(old)):
                            hist_last[hk] = v
                            history.append((item, e, labels.get(e, (None, None))[1], accn, v, clock, z))
        if clock < emit_from:
            continue
        if latest_pe is None:
            counters["events_without_period"] = counters.get("events_without_period", 0) + len(accns)
            continue
        items = compute_items(k, latest_pe, fr_ni, counters, ctx)
        fy, fp = labels.get(latest_pe, (None, None))
        stale = staleness_days(q_clocks, clock)
        for accn in sorted(accns):
            a = accns[accn]
            if a["own"] is not None and a["own"] < latest_pe:
                counters["events_older_own_period"] = counters.get("events_older_own_period", 0) + 1
            out.append({"cik": cik, "accession": accn, "form": a["form"], "filed": a["filed"], "clock_utc": clock,
                        "clock_basis": a["basis"], "period_end": latest_pe, "fiscal_year": fy,
                        "fiscal_period": fp, **items, "currency": currency_by_accn[accn],
                        "fin_template": ctx.template, "sic_in_force": sic, "staleness_days": stale})
    return out, currency_by_accn


def native_currency(rows: Sequence[tuple]) -> str | None:
    """The issuer's most frequent non-USD money unit (None when every money fact is USD)."""
    units = Counter(r[6] for r in rows if r[2] not in SHARE_CIDS and r[6] != "USD")
    if not units:
        return None
    return min(units.items(), key=lambda kv: (-kv[1], kv[0]))[0]


def issuer_events(cik: int, rows: Sequence[tuple], emit_from: dt.datetime, counters: dict[str, int],
                  sic_by_accn: dict[str, int] | None = None, pre_by_accn: dict[str, dict[str, bool]] | None = None,
                  history: list[tuple] | None = None) -> list[dict[str, Any]]:
    """Events of one issuer: money and share items from the USD knowledge; when the issuer reports money
    facts in another currency, the currency-invariant items (``UNITLESS_ITEMS``) of events whose filing
    reports in that currency come from that currency's knowledge, and so do their quarterly-history rows
    (tuples gain an eighth element, the currency)."""
    hist_usd: list[tuple] | None = [] if history is not None else None
    events, cur_by_accn = company_events(cik, rows, emit_from, counters, "USD", sic_by_accn, pre_by_accn, hist_usd)
    native = native_currency(rows)
    hist_nat: list[tuple] | None = None
    if native is not None:
        counters["issuers_native_currency_pass"] = counters.get("issuers_native_currency_pass", 0) + 1
        scratch: dict[str, int] = {}
        hist_nat = [] if history is not None else None
        nat_events, _ = company_events(cik, rows, emit_from, scratch, native, sic_by_accn, pre_by_accn, hist_nat)
        by_accn = {e["accession"]: e for e in nat_events}
        for e in events:
            if e["currency"] == native and e["accession"] in by_accn:
                ne = by_accn[e["accession"]]
                for c in UNITLESS_ITEMS:
                    e[c] = ne[c]
                counters["events_unitless_from_native"] = counters.get("events_unitless_from_native", 0) + 1
    if history is not None:
        for row in hist_usd or ():
            if native is None or cur_by_accn.get(row[3]) != native:
                history.append(row + ("USD",))
        for row in hist_nat or ():
            if cur_by_accn.get(row[3]) == native:
                history.append(row + (native,))
    return events
