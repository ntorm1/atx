"""Stage F S4.2 catalog: Compustat-analog items beyond the v9 event columns (pure data; ``fund_items`` computes them).

Every entry names its Compustat mnemonic and the ``seeds/fundamental_items.csv`` item it analogues. ``kind``:

* ``stock``: the value at ``period_end`` (balance-sheet instant);
* ``flow``: trailing twelve months (fiscal-year fact, else four chained quarters, else YTD arithmetic; the S4.1
  cross-concept paths apply);
* ``per_share``: like ``flow`` on ``<CCY>/shares`` facts (a sum of quarters approximates the annual figure, as
  Compustat's 12-month EPS does);
* ``avg_shares``: weighted-average share count of the fiscal year (the annual fact, else the mean of four chained
  quarters);
* ``derived``: a formula over other items (``fund_items.catalog_items``).

``chain`` lists us-gaap names (plain) and ``ifrs-full:`` names in priority order (the seed aliases first). Zero rule
(Compustat reports these as 0 when the statement shows no such line): when ``stmt`` is set, the item is 0 (listed in
``catalog_zero_filled``) if the chain has no non-zero fact ending within 460 days of ``period_end`` and either the
filing's own statement ``stmt`` (FSDS PRE, not parenthetical) exists without a line whose tag or label matches
``line`` (case-insensitive), or -- no PRE lines for that period, and only for ``knowledge_zero`` items whose absence
is common and whose zero is unambiguous (treasury stock, discontinued operations, acquisitions, debt issuance, ...)
-- the statement is evidenced by the issuer's knowledge (``at`` for BS, ``ni_ttm`` for IS, ``cfo_ttm`` for CF). ``sic`` restricts an industry item to SIC ranges;
outside them the item is structurally NaN (``structural``).
"""

from __future__ import annotations

from dataclasses import dataclass


@dataclass(frozen=True)
class CatItem:
    col: str
    mnemonic: str
    seed: int | None
    kind: str
    chain: tuple[str, ...] = ()
    stmt: str | None = None
    line: str | None = None
    sic: tuple[tuple[int, int], ...] = ()
    note: str = ""
    knowledge_zero: bool = False


def _i(*names: str) -> tuple[str, ...]:
    return tuple(f"ifrs-full:{n}" for n in names)


BANK_SIC = ((6000, 6199), (6710, 6712))        # depository and credit institutions, bank holding companies
DEPOSIT_SIC = ((6000, 6099), (6710, 6712))
INSURER_SIC = ((6300, 6399),)

CATALOG: tuple[CatItem, ...] = (
    # --- balance sheet ------------------------------------------------------------------------------------------
    CatItem("ch", "CH", 1104, "stock", ("CashAndCashEquivalentsAtCarryingValue", "Cash", "CashAndDueFromBanks",
                                        "CashCashEquivalentsRestrictedCashAndRestrictedCashEquivalents")
            + _i("CashAndCashEquivalents", "Cash")),
    CatItem("ivst", "IVST", 1105, "stock", ("ShortTermInvestments", "MarketableSecuritiesCurrent",
                                            "AvailableForSaleSecuritiesCurrent", "OtherShortTermInvestments",
                                            "AvailableForSaleSecuritiesDebtSecuritiesCurrent",
                                            "HeldToMaturitySecuritiesCurrent", "TradingSecuritiesCurrent")
            + _i("ShorttermDepositsNotClassifiedAsCashEquivalents", "CurrentInvestments"),
            "BS", r"shortterminvest|marketablesecuritiescurrent|securitiescurrent|currentinvestments|short-term invest"
                  r"|marketable securities|investments, current|short term invest", knowledge_zero=True),
    CatItem("xpp", "XPP", 1108, "stock", ("PrepaidExpenseCurrent", "PrepaidExpenseAndOtherAssetsCurrent",
                                          "PrepaidExpenseCurrentAndNoncurrent"), "BS", r"prepaid"),
    CatItem("aco", "ACO", 1109, "stock", ("OtherAssetsCurrent", "PrepaidExpenseAndOtherAssetsCurrent"),
            "BS", r"otherassetscurrent|other current assets|othercurrentassets|prepaidexpenseandother|prepaid expenses and other"),
    CatItem("dpact", "DPACT", 1112, "stock", ("AccumulatedDepreciationDepletionAndAmortizationPropertyPlantAndEquipment",
                                              "PropertyPlantAndEquipmentAndFinanceLeaseRightOfUseAssetAccumulatedDepreciationAndAmortization")),
    CatItem("intano", "INTANO", 1113, "derived", note="gdwl + intan (after their zero fills), else "
                                                       "IntangibleAssetsNetIncludingGoodwill"),
    CatItem("rou", "ROUANT", 1116, "stock", ("OperatingLeaseRightOfUseAsset",) + _i("RightofuseAssets"),
            "BS", r"rightofuse|right-of-use|right of use|operatinglease", knowledge_zero=True),
    CatItem("ivao", "IVAO", 1117, "stock", ("LongTermInvestments", "EquityMethodInvestments",
                                            "AvailableForSaleSecuritiesNoncurrent", "MarketableSecuritiesNoncurrent",
                                            "LongTermInvestmentsAndReceivablesNet", "OtherLongTermInvestments",
                                            "AvailableForSaleSecuritiesDebtSecuritiesNoncurrent")
            + _i("InvestmentsAccountedForUsingEquityMethod", "NoncurrentInvestments"),
            "BS", r"investment|equitymethod|marketablesecuritiesnoncurrent|securitiesnoncurrent"),
    CatItem("txdba", "TXDBA", 1118, "stock", ("DeferredIncomeTaxAssetsNet", "DeferredTaxAssetsNetNoncurrent",
                                              "DeferredTaxAssetsNet") + _i("DeferredTaxAssets"),
            "BS", r"deferredtaxasset|deferredincometaxasset|deferred tax asset|deferred income tax asset"),
    CatItem("ao", "AO", 1119, "stock", ("OtherAssetsNoncurrent", "OtherAssets") + _i("OtherNoncurrentAssets"),
            "BS", r"otherassets|other assets|othernoncurrentassets|other non-current assets|other long-term assets"),
    CatItem("xacc", "XACC", 1204, "stock", ("AccruedLiabilitiesCurrent", "EmployeeRelatedLiabilitiesCurrent",
                                            "AccruedLiabilitiesAndOtherLiabilities", "OtherAccruedLiabilitiesCurrent"),
            "BS", r"accrued|payableandaccrued|payables and accrued|accountspayableandaccrued"),
    CatItem("dlc", "DLC", 1205, "derived", note="debt in current liabilities as in debt: DebtCurrent, else current LTD + "
                                                "short-term borrowings; 0 when at exists and neither is reported"),
    CatItem("dd1", "DD1", 1206, "stock", ("LongTermDebtCurrent", "LongTermDebtAndCapitalLeaseObligationsCurrent")
            + _i("CurrentPortionOfLongtermBorrowings"),
            "BS", r"longtermdebtcurrent|current portion|current maturities|currentportion|currentmaturities|debtcurrent"),
    CatItem("dltt", "DLTT", 1207, "derived", note="long-term debt excluding current maturities as in debt (0 when at "
                                                  "exists and no long-term debt concept is reported)"),
    CatItem("llo", "LLO", 1209, "stock", ("OperatingLeaseLiability", "OperatingLeaseLiabilityNoncurrent") + _i("LeaseLiabilities"),
            "BS", r"operatinglease|lease liabilit|leaseliabilit|lease obligation|leaseobligation", knowledge_zero=True),
    CatItem("txditc", "TXDITC", 1211, "derived", note="be's deferred-tax component: DeferredIncomeTaxLiabilitiesNet, else "
                                                      "DeferredTaxLiabilitiesNoncurrent, carried 400 days, else 0 when at exists"),
    CatItem("lo", "LO", 1212, "stock", ("OtherLiabilitiesNoncurrent", "OtherLiabilities") + _i("OtherNoncurrentLiabilities"),
            "BS", r"otherliabilities|other liabilities|othernoncurrentliabilities|other long-term liabilities|other non-current liabilities"),
    CatItem("cstk", "CSTK", 1215, "stock", ("CommonStockValue", "CommonStocksIncludingAdditionalPaidInCapital",
                                            "CommonStockValueOutstanding") + _i("IssuedCapital", "ShareCapital"),
            "BS", r"commonstock|common stock|ordinary shares|share capital|sharecapital|issuedcapital|common shares|commonshares"
                  r"|capital stock|capitalstock|members|partners|units"),
    CatItem("caps", "CAPS", 1216, "stock", ("AdditionalPaidInCapital", "AdditionalPaidInCapitalCommonStock",
                                            "CommonStocksIncludingAdditionalPaidInCapital") + _i("SharePremium"),
            "BS", r"paidincapital|paid-in capital|paid in capital|capital in excess|capitalinexcess|share premium|sharepremium"),
    CatItem("re", "RE", 1217, "stock", ("RetainedEarningsAccumulatedDeficit",) + _i("RetainedEarnings"),
            "BS", r"retainedearnings|retained earnings|accumulated deficit|accumulateddeficit|earnings reinvested|reinvested"),
    CatItem("acominc", "ACOMINC", 1218, "stock", ("AccumulatedOtherComprehensiveIncomeLossNetOfTax",)
            + _i("AccumulatedOtherComprehensiveIncome", "OtherReserves"),
            "BS", r"comprehensive|othercomprehensive|reserves", knowledge_zero=True),
    CatItem("tstk", "TSTK", 1219, "stock", ("TreasuryStockValue", "TreasuryStockCommonValue") + _i("TreasuryShares"),
            "BS", r"treasury", knowledge_zero=True),
    CatItem("ceq", "CEQ", 1220, "derived", note="seq - pstk (pstk 0 when unreported)"),
    CatItem("teq", "TEQ", 1222, "derived", note="StockholdersEquityIncludingPortionAttributableToNoncontrollingInterest, else seq + mib"),
    CatItem("lse", "LSE", 1223, "stock", ("LiabilitiesAndStockholdersEquity",) + _i("EquityAndLiabilities"),
            note="else at (the balance sheet identity)"),
    CatItem("mibt", "MIBT/TEMP", 1224, "stock", ("TemporaryEquityCarryingAmountIncludingPortionAttributableToNoncontrollingInterests",
                                                 "RedeemableNoncontrollingInterestEquityCarryingAmount",
                                                 "TemporaryEquityCarryingAmount", "RedeemablePreferredStockCarryingAmount",
                                                 "TemporaryEquityCarryingAmountAttributableToParent"),
            "BS", r"temporaryequity|temporary equity|redeemable|mezzanine", knowledge_zero=True),
    CatItem("txp", "TXP", 1225, "stock", ("TaxesPayableCurrent", "AccruedIncomeTaxesCurrent", "AccruedIncomeTaxes",
                                          "IncomeTaxesPayable") + _i("CurrentTaxLiabilities"),
            "BS", r"taxes|tax payable|taxpayable|incometax|income tax"),
    CatItem("lco", "LCO", 1226, "stock", ("OtherLiabilitiesCurrent", "OtherAccruedLiabilitiesCurrent")
            + _i("OtherCurrentLiabilities"),
            "BS", r"otherliabilitiescurrent|other current liabilities|othercurrentliabilities|accruedliabilitiesandother"
                  r"|accrued expenses and other|accrued liabilities and other|other accrued"),
    CatItem("wcap", "WCAP", 1321, "derived", note="act - lct (structural where act/lct are)"),
    # --- income statement (TTM) --------------------------------------------------------------------------------
    CatItem("xopr_ttm", "XOPR", 1010, "derived", note="sale_ttm - (oi_ttm + dp_ttm) (Compustat: OIBDP = SALE - XOPR)"),
    CatItem("xsell_ttm", "XSELL", 1006, "flow", ("SellingAndMarketingExpense", "SellingExpense", "MarketingExpense")
            + _i("DistributionCosts", "SellingExpense", "SalesAndMarketingExpense")),
    CatItem("xad_ttm", "XAD", 1009, "flow", ("AdvertisingExpense", "MarketingAndAdvertisingExpense")),
    CatItem("am_ttm", "AM", 1013, "flow", ("AmortizationOfIntangibleAssets", "FiniteLivedIntangibleAssetsAmortizationExpense",
                                          "AmortizationOfAcquiredIntangibleAssets"),
            "IS", r"amortization|amortisation"),
    CatItem("idit_ttm", "IDIT", 1020, "flow", ("InvestmentIncomeInterest", "InvestmentIncomeInterestAndDividend",
                                              "InterestIncomeOther", "InterestAndOtherIncome", "InterestIncomeOperating",
                                              "InterestAndDividendIncomeOperating") + _i("InterestRevenueCalculatedUsingEffectiveInterestMethod", "FinanceIncome"),
            "IS", r"interestincome|interest income|investmentincome|investment income|interestandother|interest and other"
                  r"|interestanddividend|financeincome|finance income|interest revenue"),
    CatItem("nopi_ttm", "NOPI", 1021, "flow", ("NonoperatingIncomeExpense", "OtherNonoperatingIncomeExpense",
                                              "OtherNonoperatingIncome") + _i("OtherIncomeExpense"),
            "IS", r"nonoperating|non-operating|other income|otherincome|other expense|otherexpense|other \(income\)|other, net"),
    CatItem("spi_ttm", "SPI", 1022, "flow", ("RestructuringSettlementAndImpairmentProvisions", "RestructuringCharges",
                                            "AssetImpairmentCharges", "GoodwillImpairmentLoss",
                                            "ImpairmentOfIntangibleAssetsExcludingGoodwill",
                                            "GoodwillAndIntangibleAssetImpairment", "RestructuringCosts",
                                            "ImpairmentOfLongLivedAssetsHeldForUse"),
            "IS", r"restructur|impairment|special|severance|writedown|write-down|write-off|writeoff",
            note="first reported concept of the chain (not the sum of all special items)"),
    CatItem("pi_ttm", "PI", 1023, "flow", ("IncomeLossFromContinuingOperationsBeforeIncomeTaxesExtraordinaryItemsNoncontrollingInterest",
                                          "IncomeLossFromContinuingOperationsBeforeIncomeTaxesMinorityInterestAndIncomeLossFromEquityMethodInvestments",
                                          "IncomeLossFromContinuingOperationsBeforeIncomeTaxesDomestic")
            + _i("ProfitLossBeforeTax"), note="else ni_ttm + txt_ttm"),
    CatItem("txt_ttm", "TXT", 1024, "derived", note="income-tax chain TTM; 0 (catalog_zero_filled) when the income "
                                                    "statement has no tax line and no tax fact within 460 days"),
    CatItem("txc_ttm", "TXC", 1025, "flow", ("CurrentIncomeTaxExpenseBenefit",) + _i("CurrentTaxExpenseIncome")),
    CatItem("txdi_ttm", "TXDI", 1026, "flow", ("DeferredIncomeTaxExpenseBenefit", "DeferredIncomeTaxesAndTaxCredits")
            + _i("DeferredTaxExpenseIncome")),
    CatItem("mii_ttm", "MII", 1027, "flow", ("NetIncomeLossAttributableToNoncontrollingInterest",
                                            "MinorityInterestInNetIncomeLossOfConsolidatedEntities",
                                            "NetIncomeLossAttributableToRedeemableNoncontrollingInterest",
                                            "NetIncomeLossAttributableToNonredeemableNoncontrollingInterest")
            + _i("ProfitLossAttributableToNoncontrollingInterests"),
            "IS", r"noncontrolling|non-controlling|minority", knowledge_zero=True),
    CatItem("esub_ttm", "ESUB", 1028, "flow", ("IncomeLossFromEquityMethodInvestments",
                                              "IncomeLossFromEquityMethodInvestmentsNetOfDividendsOrDistributions")
            + _i("ShareOfProfitLossOfAssociatesAndJointVenturesAccountedForUsingEquityMethod"),
            "IS", r"equitymethod|equity method|equity in|equityin|affiliates|associates|jointventure|joint venture|unconsolidated", knowledge_zero=True),
    CatItem("do_ttm", "DO", 1030, "flow", ("IncomeLossFromDiscontinuedOperationsNetOfTax",
                                          "IncomeLossFromDiscontinuedOperationsNetOfTaxAttributableToReportingEntity",
                                          "DiscontinuedOperationIncomeLossFromDiscontinuedOperationNetOfTax",
                                          "IncomeLossFromDiscontinuedOperationsNetOfTaxIncludingPortionAttributableToNoncontrollingInterest")
            + _i("ProfitLossFromDiscontinuedOperations"),
            "IS", r"discontinued", knowledge_zero=True),
    CatItem("ib_ttm", "IB", 1029, "flow", ("IncomeLossFromContinuingOperations",
                                          "IncomeLossFromContinuingOperationsIncludingPortionAttributableToNoncontrollingInterest")
            + _i("ProfitLossFromContinuingOperations"), note="else ni_ttm - do_ttm"),
    CatItem("xido_ttm", "XIDO", 1051, "flow", ("ExtraordinaryItemNetOfTax", "ExtraordinaryItemsNetOfTax",
                                              "ExtraordinaryItemGross"),
            "IS", r"extraordinary", knowledge_zero=True),
    CatItem("nicon_ttm", "NIADJ", 1032, "flow", ("NetIncomeLossAvailableToCommonStockholdersBasic",
                                                "NetIncomeLossAvailableToCommonStockholdersDiluted"),
            note="else ni_ttm - dvp_ttm"),
    CatItem("dvp_ttm", "DVP", 1033, "flow", ("PreferredStockDividendsIncomeStatementImpact",
                                            "PreferredStockDividendsAndOtherAdjustments", "PreferredStockDividends",
                                            "DividendsPreferredStock", "DividendsPreferredStockCash"),
            "IS", r"preferred|preference", knowledge_zero=True),
    CatItem("epspx_ttm", "EPSPX", 1034, "per_share", ("EarningsPerShareBasic", "EarningsPerShareBasicAndDiluted")
            + _i("BasicEarningsLossPerShare")),
    CatItem("epsfx_ttm", "EPSFX", 1035, "per_share", ("EarningsPerShareDiluted", "EarningsPerShareBasicAndDiluted")
            + _i("DilutedEarningsLossPerShare")),
    CatItem("epspi_ttm", "EPSPI", 1036, "per_share", ("IncomeLossFromContinuingOperationsPerBasicShare",
                                                     "IncomeLossFromContinuingOperationsPerBasicAndDilutedShare"),
            note="else epspx when do_ttm = 0"),
    CatItem("epsfi_ttm", "EPSFI", 1037, "per_share", ("IncomeLossFromContinuingOperationsPerDilutedShare",
                                                     "IncomeLossFromContinuingOperationsPerBasicAndDilutedShare"),
            note="else epsfx when do_ttm = 0"),
    CatItem("cshpri", "CSHPRI", 1040, "avg_shares", ("WeightedAverageNumberOfSharesOutstandingBasic",
                                                     "WeightedAverageNumberOfShareOutstandingBasicAndDiluted",
                                                     "WeightedAverageNumberOfSharesIssuedBasic") + _i("WeightedAverageShares")),
    CatItem("cshfd", "CSHFD", 1041, "avg_shares", ("WeightedAverageNumberOfDilutedSharesOutstanding",
                                                   "WeightedAverageNumberOfShareOutstandingBasicAndDiluted")
            + _i("AdjustedWeightedAverageShares")),
    CatItem("cshi", "CSHI", 1039, "stock", ("CommonStockSharesIssued",) + _i("NumberOfSharesIssued"),
            note="shares issued (csho is shrs_q)"),
    CatItem("ci_ttm", "CITOTAL", None, "flow", ("ComprehensiveIncomeNetOfTax",
                                               "ComprehensiveIncomeNetOfTaxIncludingPortionAttributableToNoncontrollingInterest")
            + _i("ComprehensiveIncome", "ComprehensiveIncomeAttributableToOwnersOfParent")),
    # --- cash flow (TTM) -----------------------------------------------------------------------------------------
    CatItem("ivncf_ttm", "IVNCF", 1303, "flow", ("NetCashProvidedByUsedInInvestingActivities",
                                                "NetCashProvidedByUsedInInvestingActivitiesContinuingOperations")
            + _i("CashFlowsFromUsedInInvestingActivities")),
    CatItem("fincf_ttm", "FINCF", 1304, "flow", ("NetCashProvidedByUsedInFinancingActivities",
                                                "NetCashProvidedByUsedInFinancingActivitiesContinuingOperations")
            + _i("CashFlowsFromUsedInFinancingActivities")),
    CatItem("capxint_ttm", "CAPXINT", 1306, "flow", ("PaymentsToAcquireIntangibleAssets", "PaymentsToDevelopSoftware",
                                                    "PaymentsForSoftware")
            + _i("PurchaseOfIntangibleAssetsClassifiedAsInvestingActivities"),
            "CF", r"intangible|software|capitalized|capitalised", knowledge_zero=True),
    CatItem("stkco_ttm", "STKCO", 1308, "flow", ("ShareBasedCompensation", "AllocatedShareBasedCompensationExpense",
                                                "ShareBasedCompensationArrangementByShareBasedPaymentAwardCompensationCost1")
            + _i("AdjustmentsForSharebasedPayments"),
            "CF", r"sharebased|share-based|share based|stockbased|stock-based|stock based|equitybased|equity-based"
                  r"|stockcompensation|stock compensation|equity compensation|restricted stock|stockoption", knowledge_zero=True),
    CatItem("aqc_ttm", "AQC", 1309, "flow", ("PaymentsToAcquireBusinessesNetOfCashAcquired", "PaymentsToAcquireBusinessesGross",
                                            "PaymentsToAcquireBusinessesAndInterestInAffiliates")
            + _i("CashFlowsUsedInObtainingControlOfSubsidiariesOrOtherBusinessesClassifiedAsInvestingActivities"),
            "CF", r"acqui|business combination|businesscombination|purchase of business|purchaseofbusiness", knowledge_zero=True),
    CatItem("sppiv_ttm", "SPPIV", 1310, "flow", ("ProceedsFromSaleOfPropertyPlantAndEquipment", "ProceedsFromSaleOfProductiveAssets",
                                                "ProceedsFromDivestitureOfBusinesses",
                                                "ProceedsFromDivestitureOfBusinessesNetOfCashDivested")
            + _i("ProceedsFromSalesOfPropertyPlantAndEquipmentClassifiedAsInvestingActivities"),
            "CF", r"proceedsfromsale|proceeds from sale|proceeds from the sale|disposal|divest|sale of property|saleofproperty", knowledge_zero=True),
    CatItem("dltis_ttm", "DLTIS", 1313, "flow", ("ProceedsFromIssuanceOfLongTermDebt", "ProceedsFromNotesPayable",
                                                "ProceedsFromIssuanceOfSeniorLongTermDebt", "ProceedsFromLinesOfCredit",
                                                "ProceedsFromIssuanceOfDebt", "ProceedsFromConvertibleDebt",
                                                "ProceedsFromLongTermLinesOfCredit", "ProceedsFromBankDebt",
                                                "ProceedsFromIssuanceOfUnsecuredDebt", "ProceedsFromIssuanceOfSecuredDebt",
                                                "ProceedsFromDebtNetOfIssuanceCosts")
            + _i("ProceedsFromNoncurrentBorrowings", "ProceedsFromBorrowingsClassifiedAsFinancingActivities"),
            "CF", r"proceedsfrom.*(debt|notes|borrow|credit|loan|bond)|proceeds from .*(debt|notes|borrow|credit|loan|bond)"
                  r"|borrowings|issuance of .*(debt|notes)|issuanceof.*(debt|notes)", knowledge_zero=True),
    CatItem("dltr_ttm", "DLTR", 1314, "flow", ("RepaymentsOfLongTermDebt", "RepaymentsOfNotesPayable",
                                              "RepaymentsOfSeniorDebt", "RepaymentsOfLinesOfCredit", "RepaymentsOfDebt",
                                              "RepaymentsOfConvertibleDebt", "RepaymentsOfLongTermLinesOfCredit",
                                              "RepaymentsOfBankDebt", "RepaymentsOfUnsecuredDebt", "RepaymentsOfSecuredDebt",
                                              "RepaymentsOfLongTermDebtAndCapitalSecurities")
            + _i("RepaymentsOfNoncurrentBorrowings", "RepaymentsOfBorrowingsClassifiedAsFinancingActivities"),
            "CF", r"repayment|repaymentsof|payments on|paymentson|payments of .*(debt|notes|borrow|loan)|redemption|retirement", knowledge_zero=True),
    CatItem("dlcch_ttm", "DLCCH", -100003, "flow", ("ProceedsFromRepaymentsOfShortTermDebt",
                                                   "ProceedsFromRepaymentsOfCommercialPaper",
                                                   "ProceedsFromShortTermDebt"),
            "CF", r"shortterm|short-term|short term|commercialpaper|commercial paper|revolv", knowledge_zero=True),
    CatItem("dvpd_ttm", "DVPD", 1317, "flow", ("PaymentsOfDividendsPreferredStockAndPreferenceStock",),
            "CF", r"preferred|preference", knowledge_zero=True),
    CatItem("dv_ttm", "DV", 1318, "derived", note="PaymentsOfDividends TTM, else dvc_ttm + dvpd_ttm"),
    CatItem("recch_ttm", "RECCH", 1319, "flow", ("IncreaseDecreaseInAccountsReceivable", "IncreaseDecreaseInReceivables",
                                                "IncreaseDecreaseInAccountsAndNotesReceivable")
            + _i("AdjustmentsForDecreaseIncreaseInTradeAndOtherReceivables", "AdjustmentsForDecreaseIncreaseInTradeAccountReceivable"),
            "CF", r"receivable"),
    CatItem("invch_ttm", "INVCH", 1320, "flow", ("IncreaseDecreaseInInventories",)
            + _i("AdjustmentsForDecreaseIncreaseInInventories"),
            "CF", r"inventor", knowledge_zero=True),
    CatItem("apalch_ttm", "APALCH", 1321, "flow", ("IncreaseDecreaseInAccountsPayable", "IncreaseDecreaseInAccountsPayableTrade",
                                                  "IncreaseDecreaseInAccountsPayableAndAccruedLiabilities")
            + _i("AdjustmentsForIncreaseDecreaseInTradeAndOtherPayables", "AdjustmentsForIncreaseDecreaseInTradeAccountPayable"),
            "CF", r"payable"),
    CatItem("exre_ttm", "EXRE", 1323, "flow", ("EffectOfExchangeRateOnCashCashEquivalentsRestrictedCashAndRestrictedCashEquivalents",
                                              "EffectOfExchangeRateOnCashAndCashEquivalents",
                                              "EffectOfExchangeRateOnCashCashEquivalentsRestrictedCashAndRestrictedCashEquivalentsIncludingDisposalGroupAndDiscontinuedOperations")
            + _i("EffectOfExchangeRateChangesOnCashAndCashEquivalents"),
            "CF", r"exchange|currency|foreign", knowledge_zero=True),
    CatItem("chech_ttm", "CHECH", 1324, "flow", ("CashCashEquivalentsRestrictedCashAndRestrictedCashEquivalentsPeriodIncreaseDecreaseIncludingExchangeRateEffect",
                                                "CashAndCashEquivalentsPeriodIncreaseDecrease",
                                                "CashCashEquivalentsRestrictedCashAndRestrictedCashEquivalentsPeriodIncreaseDecreaseExcludingExchangeRateEffect",
                                                "CashAndCashEquivalentsPeriodIncreaseDecreaseExcludingExchangeRateEffect",
                                                "CashPeriodIncreaseDecrease")
            + _i("IncreaseDecreaseInCashAndCashEquivalents")),
    CatItem("txdc_ttm", "TXDC", 1327, "flow", ("DeferredIncomeTaxesAndTaxCredits", "DeferredIncomeTaxExpenseBenefit",
                                              "IncreaseDecreaseInDeferredIncomeTaxes"),
            "CF", r"deferred.*tax|deferredtax|deferredincometax"),
    CatItem("txpd_ttm", "TXPD", -100001, "flow", ("IncomeTaxesPaidNet", "IncomeTaxesPaid") + _i("IncomeTaxesPaidRefundClassifiedAsOperatingActivities")),
    CatItem("intpn_ttm", "INTPN", None, "flow", ("InterestPaidNet", "InterestPaid") + _i("InterestPaidClassifiedAsOperatingActivities",
                                                                                         "InterestPaidClassifiedAsFinancingActivities")),
    # --- industry items (structurally NaN outside their SIC ranges) ----------------------------------------------
    CatItem("niint_ttm", "NIINT", 1501, "flow", ("InterestIncomeExpenseNet", "InterestIncomeExpenseAfterProvisionForLoanLoss",
                                                "InterestIncomeExpenseOperatingNet"), sic=BANK_SIC,
            note="bank net interest income"),
    CatItem("idit_bank_ttm", "IDITBK", 1503, "flow", ("InterestAndDividendIncomeOperating", "InterestIncomeOperating",
                                                     "InterestAndFeeIncomeLoansAndLeases"), sic=BANK_SIC),
    CatItem("pcl_ttm", "PCL", 1505, "flow", ("ProvisionForLoanAndLeaseLosses", "ProvisionForLoanLeaseAndOtherLosses",
                                            "ProvisionForCreditLosses", "ProvisionForLoanLossesExpensed",
                                            "FinancingReceivableCreditLossExpenseReversal", "CreditLossExpense"),
            "IS", r"provision|credit loss|creditloss|loan loss|loanloss", sic=BANK_SIC),
    CatItem("lntal", "LNTAL", 1509, "stock", ("LoansAndLeasesReceivableNetReportedAmount",
                                              "LoansAndLeasesReceivableNetOfDeferredIncome",
                                              "FinancingReceivableExcludingAccruedInterestAfterAllowanceForCreditLoss",
                                              "LoansAndLeasesReceivableGrossCarryingAmount",
                                              "FinancingReceivableExcludingAccruedInterestBeforeAllowanceForCreditLoss",
                                              "LoansReceivableNet", "NotesReceivableNet"), sic=BANK_SIC),
    CatItem("dptc", "DPTC", 1510, "stock", ("Deposits", "DepositsDomestic", "InterestBearingDepositLiabilities"),
            sic=DEPOSIT_SIC),
    CatItem("prem_ttm", "PREM", 1601, "flow", ("PremiumsEarnedNet", "PremiumsEarnedNetPropertyAndCasualty",
                                              "PremiumsEarnedNetLife", "SupplementaryInsuranceInformationPremiumRevenue",
                                              "InsurancePremiumsRevenueRecognized"), sic=INSURER_SIC),
    CatItem("benef_ttm", "BENEF", 1604, "flow", ("PolicyholderBenefitsAndClaimsIncurredNet", "BenefitsLossesAndExpenses",
                                                "IncurredClaimsPropertyCasualtyAndLiability",
                                                "PolicyholderBenefitsAndClaimsIncurredLifeAndAnnuity",
                                                "LiabilityForFuturePolicyBenefitsPeriodExpense"), sic=INSURER_SIC),
)

BY_COL = {c.col: c for c in CATALOG}
assert len(BY_COL) == len(CATALOG), "duplicate catalog column"
CAT_COLUMNS = tuple(c.col for c in CATALOG)
STOCK_KINDS = ("stock",)
PRE_STMTS = {"BS": ("BS",), "IS": ("IS", "CI"), "CF": ("CF",)}


def applicable(item: CatItem, sic: int | None) -> bool:
    """False when the item is structurally absent for this SIC (industry items outside their ranges)."""
    if not item.sic:
        return True
    return sic is not None and any(lo <= sic <= hi for lo, hi in item.sic)
