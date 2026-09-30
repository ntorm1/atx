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
filing's own statement ``stmt`` (FSDS PRE, not parenthetical) exists without an item line, or -- no PRE lines for
that period, and only for ``knowledge_zero`` items whose absence is common and whose zero is unambiguous (treasury
stock, discontinued operations, acquisitions, debt issuance, ...) -- the statement is evidenced by the issuer's
knowledge (``at`` for BS, ``ni_ttm`` for IS, ``cfo_ttm`` for CF). Item line (catalog-pre-v2, ``line_flag``): a line of
the statement whose lower-case tag matches ``tags`` (standard or custom tag), or a custom-tag line (FSDS ``version`` =
the accession) whose lower-case label matches ``line`` and not ``exclude``; lines whose tag matches
``STMT_TAG_EXCLUDE`` of the statement (subtotals, comprehensive income, supplemental disclosures) never count.
``sic`` restricts an industry item to SIC ranges; outside them the item is structurally NaN (``structural``).
"""

from __future__ import annotations

import re
from dataclasses import dataclass
from functools import lru_cache


@dataclass(frozen=True)
class CatItem:
    col: str
    mnemonic: str
    seed: int | None
    kind: str
    chain: tuple[str, ...] = ()
    stmt: str | None = None
    line: str | None = None          # label pattern (custom-tag lines only)
    sic: tuple[tuple[int, int], ...] = ()
    note: str = ""
    knowledge_zero: bool = False
    tags: str | None = None          # tag pattern (any line)
    exclude: str | None = None       # label exclusion for ``line``


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
            "BS", r"short[- ]?term investments?|marketable securities|investments?,? current|current investments"
                  r"|time deposits|certificates? of deposit|short[- ]term deposits", knowledge_zero=True,
            tags=r"shortterminvest|marketablesecuritiescurrent|securitiescurrent$|securitiesamortizedcostcurrent"
                 r"|^currentinvestments|investmentscurrent$|shorttermdeposits|^timedeposits|certificatesofdeposit",
            exclude=r"non-?current|long[- ]term"),
    CatItem("xpp", "XPP", 1108, "stock", ("PrepaidExpenseCurrent", "PrepaidExpenseAndOtherAssetsCurrent",
                                          "PrepaidExpenseCurrentAndNoncurrent"), "BS", r"prepaid|prepayments",
            tags=r"prepaid", exclude=r"pension"),
    CatItem("aco", "ACO", 1109, "stock", ("OtherAssetsCurrent", "PrepaidExpenseAndOtherAssetsCurrent"),
            "BS", r"other current assets|other assets,? current|and other current assets|prepaids? .*and other",
            tags=r"otherassetscurrent|^prepaidexpenseandotherassets|othercurrentassets"),
    CatItem("dpact", "DPACT", 1112, "stock", ("AccumulatedDepreciationDepletionAndAmortizationPropertyPlantAndEquipment",
                                              "PropertyPlantAndEquipmentAndFinanceLeaseRightOfUseAssetAccumulatedDepreciationAndAmortization")),
    CatItem("intano", "INTANO", 1113, "derived", note="gdwl + intan (after their zero fills), else "
                                                       "IntangibleAssetsNetIncludingGoodwill"),
    CatItem("rou", "ROUANT", 1116, "stock", ("OperatingLeaseRightOfUseAsset",) + _i("RightofuseAssets"),
            "BS", r"right[- ]of[- ]use|operating lease assets?|lease assets?", knowledge_zero=True,
            tags=r"operatingleaserightofuse|^rightofuse|^operatingleaseasset|^operatingleasesrightofuse",
            exclude=r"liabilit|finance"),
    CatItem("ivao", "IVAO", 1117, "stock", ("LongTermInvestments", "EquityMethodInvestments",
                                            "AvailableForSaleSecuritiesNoncurrent", "MarketableSecuritiesNoncurrent",
                                            "LongTermInvestmentsAndReceivablesNet", "OtherLongTermInvestments",
                                            "AvailableForSaleSecuritiesDebtSecuritiesNoncurrent")
            + _i("InvestmentsAccountedForUsingEquityMethod", "NoncurrentInvestments"),
            "BS", r"investments?|advances to affiliates|equity method",
            tags=r"longterminvestments|^equitymethodinvestments?$|investmentsinaffiliates|investmentsinandadvancestoaffiliates"
                 r"|securitiesnoncurrent|^noncurrentinvestments|investmentsaccountedforusingequitymethod"
                 r"|^heldtomaturitysecurities$|^availableforsalesecurities$|^availableforsalesecuritiesdebtsecurities$"
                 r"|^investments$|^costmethodinvestments|^equitysecuritiesfvni|^equitysecuritieswithoutreadilydeterminable"
                 r"|investmentsinunconsolidated|^otherinvestments",
            exclude=r"current|short[- ]term|real estate|property|payable|liabilit|total"),
    CatItem("txdba", "TXDBA", 1118, "stock", ("DeferredIncomeTaxAssetsNet", "DeferredTaxAssetsNetNoncurrent",
                                              "DeferredTaxAssetsNet") + _i("DeferredTaxAssets"),
            "BS", r"deferred (income )?tax(es)? assets?",
            tags=r"deferredtaxasset|deferredincometaxasset|deferredincometaxesandotherassets"),
    CatItem("ao", "AO", 1119, "stock", ("OtherAssetsNoncurrent", "OtherAssets") + _i("OtherNoncurrentAssets"),
            "BS", r"other (non-?current |long[- ]term )?assets|deferred charges and other",
            tags=r"^otherassetsnoncurrent$|^otherassets$|^othernoncurrentassets$|^otherassetsmiscellaneousnoncurrent"
                 r"|^deferredcostsandotherassets|^deferredchargesandotherassets|^otherassetsandnoncurrent"),
    CatItem("xacc", "XACC", 1204, "stock", ("AccruedLiabilitiesCurrent", "EmployeeRelatedLiabilitiesCurrent",
                                            "AccruedLiabilitiesAndOtherLiabilities", "OtherAccruedLiabilitiesCurrent"),
            "BS", r"accrued|accruals",
            tags=r"^accruedliabilities|^employeerelatedliabilitiescurrent|^otheraccruedliabilities|^accruedsalaries"
                 r"|^accruedemployee|^accruedpayroll|^accruedcompensation|^interestpayablecurrent"
                 r"|^accountspayableandaccruedliabilities|^accountspayableandotheraccruedliabilities",
            exclude=r"tax|receivable|non-?current|long[- ]term"),
    CatItem("dlc", "DLC", 1205, "derived", note="debt in current liabilities as in debt: DebtCurrent, else current LTD + "
                                                "short-term borrowings; 0 when at exists and neither is reported"),
    CatItem("dd1", "DD1", 1206, "stock", ("LongTermDebtCurrent", "LongTermDebtAndCapitalLeaseObligationsCurrent")
            + _i("CurrentPortionOfLongtermBorrowings"),
            "BS", r"current (portion|maturities|installments)|due within one year|long[- ]term debt,? current|debt,? current",
            tags=r"^longtermdebtcurrent|^longtermdebtandcapitalleaseobligationscurrent|^longtermdebtandfinancelease.*current"
                 r"|^currentportionoflongterm|^debtcurrent$|^debtandcapitalleaseobligationscurrent"
                 r"|^longtermnotespayablecurrent|^seniornotescurrent|^seniorlongtermnotes.*current"
                 r"|^convertiblenotespayablecurrent|^convertibledebtcurrent|^secureddebtcurrent|^unsecureddebtcurrent"
                 r"|^otherlongtermdebtcurrent|^longtermloanspayablecurrent|^capitalleaseobligationscurrent"
                 r"|^financeleaseliabilitycurrent|currentportionoflongtermborrowings|currentmaturities",
            exclude=r"operating lease"),
    CatItem("dltt", "DLTT", 1207, "derived", note="long-term debt excluding current maturities as in debt (0 when at "
                                                  "exists and no long-term debt concept is reported)"),
    CatItem("llo", "LLO", 1209, "stock", ("OperatingLeaseLiability", "OperatingLeaseLiabilityNoncurrent") + _i("LeaseLiabilities"),
            "BS", r"operating lease|lease liabilit|lease obligation", knowledge_zero=True,
            tags=r"^operatingleaseliabilit|^leaseliabilit|^operatingleasesliabilit",
            exclude=r"asset|right[- ]of[- ]use|finance"),
    CatItem("txditc", "TXDITC", 1211, "derived", note="be's deferred-tax component: DeferredIncomeTaxLiabilitiesNet, else "
                                                      "DeferredTaxLiabilitiesNoncurrent, carried 400 days, else 0 when at exists"),
    CatItem("lo", "LO", 1212, "stock", ("OtherLiabilitiesNoncurrent", "OtherLiabilities") + _i("OtherNoncurrentLiabilities"),
            "BS", r"other (non-?current |long[- ]term )?liabilities|deferred credits and other",
            tags=r"^otherliabilitiesnoncurrent|^otherliabilities$|^othernoncurrentliabilities|^otheraccruedliabilitiesnoncurrent"
                 r"|^deferredcreditsandotherliabilities|^otheraccruedliabilitiescurrentandnoncurrent"
                 r"|^accruedliabilitiesandotherliabilities|^otherliabilitiesandaccruedexpenses|^otherlongtermliabilities"
                 r"|^otherliabilitiesmiscellaneousnoncurrent|^liabilitiesotherthanlongtermdebtnoncurrent"),
    CatItem("cstk", "CSTK", 1215, "stock", ("CommonStockValue", "CommonStocksIncludingAdditionalPaidInCapital",
                                            "CommonStockValueOutstanding") + _i("IssuedCapital", "ShareCapital"),
            "BS", r"common (stock|shares)|ordinary shares|share capital|capital stock|shares of beneficial interest"
                  r"|class [a-c] (common|shares|stock)|partners'? capital|members'? (capital|equity)|common units",
            tags=r"^commonstockvalue|^commonstocksincludingadditionalpaidincapital|^issuedcapital|^sharecapital"
                 r"|^commonstocksharessubscribed|^partnerscapital|^memberscapital|^memberequity|^limitedpartnerscapital"
                 r"|^generalpartnerscapital|^limitedliabilitycompanyllcorlimitedpartnershiplpmembersequity"
                 r"|^commonunit|^capitalunits|^commonstocknoparvalue|^commonstockparorstatedvalue",
            exclude=r"additional|paid[- ]in|excess|treasury|held in|subscri|warrant|repurchase|issuance|dividend"),
    CatItem("caps", "CAPS", 1216, "stock", ("AdditionalPaidInCapital", "AdditionalPaidInCapitalCommonStock",
                                            "CommonStocksIncludingAdditionalPaidInCapital") + _i("SharePremium"),
            "BS", r"paid[- ]in capital|capital in excess|share premium|capital surplus|contributed (capital|surplus)"
                  r"|additional capital",
            tags=r"additionalpaidincapital|^sharepremium|^commonstocksincludingadditionalpaidincapital|^capitalsurplus"
                 r"|contributedcapital|^additionalcapital|^paidincapital",
            exclude=r"total|retained"),
    CatItem("re", "RE", 1217, "stock", ("RetainedEarningsAccumulatedDeficit",) + _i("RetainedEarnings"),
            "BS", r"retained (earnings|income|deficit)|accumulated (deficit|earnings|losses)|earnings reinvested"
                  r"|reinvested earnings|(distributions|dividends) in excess|undivided profits|^deficit",
            tags=r"^retainedearnings|^accumulateddeficit|^developmentstageenterprisedeficit|distributionsinexcessof"
                 r"|^cumulativedividends|^dividendsinexcessof",
            exclude=r"comprehensive|appropriated"),
    CatItem("acominc", "ACOMINC", 1218, "stock", ("AccumulatedOtherComprehensiveIncomeLossNetOfTax",)
            + _i("AccumulatedOtherComprehensiveIncome", "OtherReserves"),
            "BS", r"accumulated other comprehensive|other comprehensive (income|loss)|^other reserves|^reserves",
            knowledge_zero=True,
            tags=r"^accumulatedothercomprehensive|^otherreserves|^reserveofexchangedifferences",
            exclude=r"non-?controlling|self[- ]insurance|loss reserves"),
    CatItem("tstk", "TSTK", 1219, "stock", ("TreasuryStockValue", "TreasuryStockCommonValue") + _i("TreasuryShares"),
            "BS", r"treasury|reacquired", knowledge_zero=True, tags=r"treasury"),
    CatItem("ceq", "CEQ", 1220, "derived", note="seq - pstk (pstk 0 when unreported)"),
    CatItem("teq", "TEQ", 1222, "derived", note="StockholdersEquityIncludingPortionAttributableToNoncontrollingInterest, else seq + mib"),
    CatItem("lse", "LSE", 1223, "stock", ("LiabilitiesAndStockholdersEquity",) + _i("EquityAndLiabilities"),
            note="else at (the balance sheet identity)"),
    CatItem("mibt", "MIBT/TEMP", 1224, "stock", ("TemporaryEquityCarryingAmountIncludingPortionAttributableToNoncontrollingInterests",
                                                 "RedeemableNoncontrollingInterestEquityCarryingAmount",
                                                 "TemporaryEquityCarryingAmount", "RedeemablePreferredStockCarryingAmount",
                                                 "TemporaryEquityCarryingAmountAttributableToParent"),
            "BS", r"temporary equity|redeemable|mezzanine", knowledge_zero=True,
            tags=r"temporaryequity|redeemablenoncontrolling|redeemablepreferred|redeemableconvertiblepreferred"
                 r"|preferredstockredeemable|redeemablecommonstock|mandatorilyredeemable|sharessubjecttomandatoryredemption",
            exclude=r"non-?redeemable|total"),
    CatItem("txp", "TXP", 1225, "stock", ("TaxesPayableCurrent", "AccruedIncomeTaxesCurrent", "AccruedIncomeTaxes",
                                          "IncomeTaxesPayable") + _i("CurrentTaxLiabilities"),
            "BS", r"(income )?tax(es)? payable|accrued (income )?taxes|current tax liabilit|^income taxes$",
            tags=r"^taxespayable|^accruedincometaxes(current|payable)?$|^incometaxe?s?payable|^currenttaxliabilit"
                 r"|^accruedtaxes|^incometaxesreceivablepayable|^currentincometaxliabilit",
            exclude=r"deferred|receivable|non-?current|long[- ]term|asset"),
    CatItem("lco", "LCO", 1226, "stock", ("OtherLiabilitiesCurrent", "OtherAccruedLiabilitiesCurrent")
            + _i("OtherCurrentLiabilities"),
            "BS", r"other current liabilities|other liabilities,? current|accrued (expenses|liabilities)|other accrued",
            tags=r"^otherliabilitiescurrent|^othercurrentliabilities|^otheraccruedliabilities|^accrued"
                 r"|^otherliabilitiesmiscellaneouscurrent|^employeerelatedliabilitiescurrent"
                 r"|^accountspayableandotheraccruedliabilities|^accountspayableandaccruedliabilities",
            exclude=r"non-?current|long[- ]term"),
    CatItem("wcap", "WCAP", 1321, "derived", note="act - lct (structural where act/lct are)"),
    # --- income statement (TTM) --------------------------------------------------------------------------------
    CatItem("xopr_ttm", "XOPR", 1010, "derived", note="sale_ttm - (oi_ttm + dp_ttm) (Compustat: OIBDP = SALE - XOPR)"),
    CatItem("xsell_ttm", "XSELL", 1006, "flow", ("SellingAndMarketingExpense", "SellingExpense", "MarketingExpense")
            + _i("DistributionCosts", "SellingExpense", "SalesAndMarketingExpense")),
    CatItem("xad_ttm", "XAD", 1009, "flow", ("AdvertisingExpense", "MarketingAndAdvertisingExpense")),
    CatItem("am_ttm", "AM", 1013, "flow", ("AmortizationOfIntangibleAssets", "FiniteLivedIntangibleAssetsAmortizationExpense",
                                          "AmortizationOfAcquiredIntangibleAssets"),
            "IS", r"amorti[sz]ation",
            tags=r"^amortizationof(intangible|acquired|acquisition|purchased|other)|^finitelivedintangibleassetsamortization"
                 r"|^depreciationandamorti|^depreciationdepletionandamorti|^depreciationamorti|^amorti[sz]ation$"
                 r"|^costdepreciationamortizationanddepletion|^otherdepreciationandamortization|^amortisationexpense",
            exclude=r"comprehensive|actuarial|prior service|pension|swap|excluding|exclusive|financing|debt|discount"),
    CatItem("idit_ttm", "IDIT", 1020, "flow", ("InvestmentIncomeInterest", "InvestmentIncomeInterestAndDividend",
                                              "InterestIncomeOther", "InterestAndOtherIncome", "InterestIncomeOperating",
                                              "InterestAndDividendIncomeOperating") + _i("InterestRevenueCalculatedUsingEffectiveInterestMethod", "FinanceIncome"),
            "IS", r"interest (income|and (other|investment|dividend) income|revenue)|investment income|finance income"
                  r"|interest and other|interest \((income|expense)\)|interest (income|expense),? net",
            tags=r"^investmentincome|^interestincome|^interestanddividendincome|^interestandotherincome|^financeincome"
                 r"|^interestrevenue|^netinvestmentincome"),
    CatItem("nopi_ttm", "NOPI", 1021, "flow", ("NonoperatingIncomeExpense", "OtherNonoperatingIncomeExpense",
                                              "OtherNonoperatingIncome") + _i("OtherIncomeExpense"),
            "IS", r"other (income|expense|\(income\)|\(expense\)|charges|gains|losses|, net|net|items)|non-?operating"
                  r"|miscellaneous|^other$",
            tags=r"^nonoperatingincomeexpense|^othernonoperating|^nonoperatinggainslosses|^otherincome|^otherexpenses?$"
                 r"|^othercostandexpense|^investmentincomenonoperating|^interestandotherincome|^incomeexpensefromnonoperating"
                 r"|^otheroperatingincomeexpensenet|^nonoperatingincome|^othergainslosses|^othernonoperatingexpense"
                 r"|^miscellaneous|^otherincomeexpense",
            exclude=r"comprehensive"),
    CatItem("spi_ttm", "SPI", 1022, "flow", ("RestructuringSettlementAndImpairmentProvisions", "RestructuringCharges",
                                            "AssetImpairmentCharges", "GoodwillImpairmentLoss",
                                            "ImpairmentOfIntangibleAssetsExcludingGoodwill",
                                            "GoodwillAndIntangibleAssetImpairment", "RestructuringCosts",
                                            "ImpairmentOfLongLivedAssetsHeldForUse"),
            "IS", r"restructur|impairment|special|severance|write-?(down|off)|merger|acquisition[- ]related|litigation"
                  r"|settlement|transaction (costs|expenses)|unusual|non-?recurring|exit costs|separation",
            tags=r"restructuring|impairment|^severance|writedown|writeoff|^businessexitcosts|^specialcharges|^specialitems"
                 r"|^litigationsettlement|^lossfromcatastrophes|^mergerrelatedcosts|^businesscombinationacquisitionrelatedcosts"
                 r"|^unusualorinfrequent|^othernonrecurring|^nonrecurring|^gainlossondispositionofbusiness"
                 r"|^gainlossonsaleofbusiness|^gainslossesonextinguishmentofdebt|^gainlossonsaleofpropertyplantequipment",
            exclude=r"comprehensive|reclassification|unrealized|portion",
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
            "IS", r"non-?controlling|minority", knowledge_zero=True,
            tags=r"attributabletononcontrolling|minorityinterest|attributabletoredeemablenoncontrolling"
                 r"|attributabletononredeemablenoncontrolling|profitlossattributabletononcontrollinginterests",
            exclude=r"comprehensive|including|before"),
    CatItem("esub_ttm", "ESUB", 1028, "flow", ("IncomeLossFromEquityMethodInvestments",
                                              "IncomeLossFromEquityMethodInvestmentsNetOfDividendsOrDistributions")
            + _i("ShareOfProfitLossOfAssociatesAndJointVenturesAccountedForUsingEquityMethod"),
            "IS", r"equity (in|method|earnings|income)|affiliates?|associates?|joint ventures?|unconsolidated",
            knowledge_zero=True,
            tags=r"^incomelossfromequitymethod|^shareofprofitlossofassociates|^equitymethodinvestment|^incomefromequitymethod"
                 r"|^equityinearnings|^equityinnetincome|^equityinlosses|^incomelossfromunconsolidated"
                 r"|^incomeexpensefromnonoperatingactivitiesandequitymethod",
            exclude=r"before|comprehensive|attributable|gain on sale"),
    CatItem("do_ttm", "DO", 1030, "flow", ("IncomeLossFromDiscontinuedOperationsNetOfTax",
                                          "IncomeLossFromDiscontinuedOperationsNetOfTaxAttributableToReportingEntity",
                                          "DiscontinuedOperationIncomeLossFromDiscontinuedOperationNetOfTax",
                                          "IncomeLossFromDiscontinuedOperationsNetOfTaxIncludingPortionAttributableToNoncontrollingInterest")
            + _i("ProfitLossFromDiscontinuedOperations"),
            "IS", r"discontinued", knowledge_zero=True,
            tags=r"^incomelossfromdiscontinued|^discontinuedoperation|^profitlossfromdiscontinued|^incomefromdiscontinued"
                 r"|^gainlossondisposalofdiscontinued|^lossfromdiscontinued"),
    CatItem("ib_ttm", "IB", 1029, "flow", ("IncomeLossFromContinuingOperations",
                                          "IncomeLossFromContinuingOperationsIncludingPortionAttributableToNoncontrollingInterest")
            + _i("ProfitLossFromContinuingOperations"), note="else ni_ttm - do_ttm"),
    CatItem("xido_ttm", "XIDO", 1051, "flow", ("ExtraordinaryItemNetOfTax", "ExtraordinaryItemsNetOfTax",
                                              "ExtraordinaryItemGross"),
            "IS", r"extraordinary", knowledge_zero=True, tags=r"^extraordinary", exclude=r"before"),
    CatItem("nicon_ttm", "NIADJ", 1032, "flow", ("NetIncomeLossAvailableToCommonStockholdersBasic",
                                                "NetIncomeLossAvailableToCommonStockholdersDiluted"),
            note="else ni_ttm - dvp_ttm"),
    CatItem("dvp_ttm", "DVP", 1033, "flow", ("PreferredStockDividendsIncomeStatementImpact",
                                            "PreferredStockDividendsAndOtherAdjustments", "PreferredStockDividends",
                                            "DividendsPreferredStock", "DividendsPreferredStockCash"),
            "IS", r"preferred|preference", knowledge_zero=True,
            tags=r"^preferredstockdividends|^dividendspreferredstock|^preferredunitdistributions|^preferredstockredemption"
                 r"|^cumulativepreferredstockdividends|^preferredstockaccretion|^preferreddividends"
                 r"|^dividendsandaccretiononpreferred",
            exclude=r"comprehensive|before"),
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
            "CF", r"(purchase|acquisition|addition|payment|investment|expenditure|capitali[sz]ation)s? (of|to|in|for) .*"
                  r"(intangible|software|licen|patent)|capitali[sz]ed (software|development)|software development",
            knowledge_zero=True,
            tags=r"^paymentstoacquireintangible|^paymentstodevelopsoftware|^paymentsforsoftware|^paymentstoacquiresoftware"
                 r"|^purchaseofintangible|^capitalizedcomputersoftwareadditions|^paymentsforcapitalizedsoftware"
                 r"|^paymentstoacquireproductiveassets|^paymentstoacquireotherproductiveassets",
            exclude=r"amorti"),
    CatItem("stkco_ttm", "STKCO", 1308, "flow", ("ShareBasedCompensation", "AllocatedShareBasedCompensationExpense",
                                                "ShareBasedCompensationArrangementByShareBasedPaymentAwardCompensationCost1")
            + _i("AdjustmentsForSharebasedPayments"),
            "CF", r"(stock|share|equity|unit)[- ]based|stock compensation|share compensation|equity compensation"
                  r"|stock awards|(restricted stock|stock option).*(expense|compensation|amorti)",
            knowledge_zero=True,
            tags=r"^sharebasedcompensation$|^allocatedsharebasedcompensation|^adjustmentsforsharebasedpayment"
                 r"|^sharebasedcompensationarrangementbysharebasedpaymentawardcompensationcost"
                 r"|^stockissuedduringperiodvaluesharebasedcompensation|^restrictedstockexpense"
                 r"|^employeebenefitsandsharebasedcompensation|^sharebasedpaymentarrangementnoncashexpense"
                 r"|^stockbasedcompensation|^sharebasedpaymentarrangementexpense",
            exclude=r"proceeds|tax|withh|repurchase|payment|excess|exercise"),
    CatItem("aqc_ttm", "AQC", 1309, "flow", ("PaymentsToAcquireBusinessesNetOfCashAcquired", "PaymentsToAcquireBusinessesGross",
                                            "PaymentsToAcquireBusinessesAndInterestInAffiliates")
            + _i("CashFlowsUsedInObtainingControlOfSubsidiariesOrOtherBusinessesClassifiedAsInvestingActivities"),
            "CF", r"acquisitions?(,| of| net)|business(es)? acquired|business combination|purchases? of business"
                  r"|acquired business|net of cash acquired|^acquisitions?$",
            knowledge_zero=True,
            tags=r"^paymentstoacquirebusinesses|^paymentstoacquireinterestinsubsidiar|^paymentstoacquireadditionalinterestinsubsidiar"
                 r"|^cashflowsusedinobtainingcontrol|^businessacquisitionnetofcashacquired|^paymentstoacquireinterestinjointventure"
                 r"|^paymentsforacquisition|^acquisitionsnetofcashacquired|^businesscombinationconsiderationtransferred",
            exclude=r"property|plant|equipment|intangible|software|investment|securities|treasury|stock|shares"
                    r"|real estate|loans?\b|land|leases?\b|rights|licen|patent|oil|gas|mineral|assets"),
    CatItem("sppiv_ttm", "SPPIV", 1310, "flow", ("ProceedsFromSaleOfPropertyPlantAndEquipment", "ProceedsFromSaleOfProductiveAssets",
                                                "ProceedsFromDivestitureOfBusinesses",
                                                "ProceedsFromDivestitureOfBusinessesNetOfCashDivested")
            + _i("ProceedsFromSalesOfPropertyPlantAndEquipmentClassifiedAsInvestingActivities"),
            "CF", r"(proceeds|sale|sales|disposal|disposition|divestiture)s? (from|of) .*(property|plant|equipment|business"
                  r"|fixed assets|land|building|premises|operations|subsidiar|assets)|divestiture",
            knowledge_zero=True,
            tags=r"^proceedsfromsaleofpropertyplant|^proceedsfromsaleofproductiveassets|^proceedsfromdivestiture"
                 r"|^proceedsfromsaleofotherproductiveassets|^proceedsfromsalesofassets|^proceedsfromsaleofrealestate$"
                 r"|^proceedsfromsaleofland|^proceedsfromsaleofbusiness|^proceedsfromsalesofbusiness|^proceedsfromdisposal"
                 r"|^proceedsfromsaleofintangible|^proceedsfromsalesofpropertyplant|^proceedsfromsaleofotherassets",
            exclude=r"securities|investments|loans|receivable|marketable"),
    CatItem("dltis_ttm", "DLTIS", 1313, "flow", ("ProceedsFromIssuanceOfLongTermDebt", "ProceedsFromNotesPayable",
                                                "ProceedsFromIssuanceOfSeniorLongTermDebt", "ProceedsFromLinesOfCredit",
                                                "ProceedsFromIssuanceOfDebt", "ProceedsFromConvertibleDebt",
                                                "ProceedsFromLongTermLinesOfCredit", "ProceedsFromBankDebt",
                                                "ProceedsFromIssuanceOfUnsecuredDebt", "ProceedsFromIssuanceOfSecuredDebt",
                                                "ProceedsFromDebtNetOfIssuanceCosts")
            + _i("ProceedsFromNoncurrentBorrowings", "ProceedsFromBorrowingsClassifiedAsFinancingActivities"),
            "CF", r"(proceeds|borrowings?|issuance|draws?)( from| of| under| on)? .*(debt|notes|borrowings?|credit|loans?"
                  r"|bonds|term loan|revolv|facility|mortgage|debentures)|^borrowings",
            knowledge_zero=True,
            tags=r"^proceedsfrom(issuanceof)?(longterm|senior|unsecured|secured|convertible|subordinated|notes|bank|term"
                 r"|lines|revolving|debt|borrowings|noncurrent|otherdebt|federalhomeloan|relatedpartydebt|construction"
                 r"|mortgage|bonds|debentures|issuanceofdebt)|^proceedsfromrepaymentsof(longterm|debt|notes|bank|lines"
                 r"|secured|related|otherdebt|borrowings)",
            exclude=r"sale|repay|payment|loans held|investment|receivable|securities|maturit|short[- ]term|commercial paper"
                    r"|issuance costs"),
    CatItem("dltr_ttm", "DLTR", 1314, "flow", ("RepaymentsOfLongTermDebt", "RepaymentsOfNotesPayable",
                                              "RepaymentsOfSeniorDebt", "RepaymentsOfLinesOfCredit", "RepaymentsOfDebt",
                                              "RepaymentsOfConvertibleDebt", "RepaymentsOfLongTermLinesOfCredit",
                                              "RepaymentsOfBankDebt", "RepaymentsOfUnsecuredDebt", "RepaymentsOfSecuredDebt",
                                              "RepaymentsOfLongTermDebtAndCapitalSecurities")
            + _i("RepaymentsOfNoncurrentBorrowings", "RepaymentsOfBorrowingsClassifiedAsFinancingActivities"),
            "CF", r"(repayment|repayments|payments?|redemption|retirement|extinguishment|prepayment|repurchase)s? (of|on|under"
                  r"|to) .*(debt|notes|borrowings?|credit|loans?|bonds|term loan|revolv|facility|mortgage|debentures)",
            knowledge_zero=True,
            tags=r"^repayments?of(longterm|senior|notes|lines|debt|convertible|bank|unsecured|secured|subordinated|otherdebt"
                 r"|relatedpartydebt|federalhomeloan|noncurrentborrowings|borrowings|mortgage|construction|term|firstmortgage"
                 r"|otherlongtermdebt|debentures|bonds)|^earlyrepayment|^proceedsfromrepaymentsof(longterm|debt|notes|bank"
                 r"|lines|secured|related|otherdebt|borrowings)|^paymentsforrepurchaseof(convertible|senior|notes|debt|longterm)"
                 r"|^paymentsoflongtermdebt|^paymentsondebt|^extinguishmentofdebt",
            exclude=r"issuance costs|financing costs|lease|interest|dividend|receivable|loans held|investment|short[- ]term"
                    r"|commercial paper"),
    CatItem("dlcch_ttm", "DLCCH", -100003, "flow", ("ProceedsFromRepaymentsOfShortTermDebt",
                                                   "ProceedsFromRepaymentsOfCommercialPaper",
                                                   "ProceedsFromShortTermDebt"),
            "CF", r"short[- ]term (debt|borrowings|notes|loans)|commercial paper|revolv|lines? of credit|credit facilit"
                  r"|notes payable", knowledge_zero=True,
            tags=r"^proceedsfromrepaymentsof(shortterm|commercialpaper|lines|bank|debt|relatedparty|federalfunds"
                 r"|securitiessold|otherdebt|borrowings)|^proceedsfromshortterm|^repaymentsofshortterm|commercialpaper"
                 r"|^increasedecreaseinshortterm|^proceedsfrom(repaymentsof)?linesofcredit|^repaymentsoflinesofcredit"
                 r"|^increasedecreaseinfederalfundspurchased|^proceedsfromrepaymentsoffederalhomeloanbank",
            exclude=r"investment|securities|marketable|receivable|assets"),
    CatItem("dvpd_ttm", "DVPD", 1317, "flow", ("PaymentsOfDividendsPreferredStockAndPreferenceStock",),
            "CF", r"prefer.*(dividend|distribution)|(dividend|distribution)s?.*prefer", knowledge_zero=True,
            tags=r"^paymentsofdividendspreferred|^dividendspreferredstockcash|^paymentsofpreferred.*dividend"
                 r"|^preferredstockdividendspaid|^distributionstopreferred|^paymentsofdistributionstopreferred"
                 r"|^preferredunitdistributions"),
    CatItem("dv_ttm", "DV", 1318, "derived", note="PaymentsOfDividends TTM, else dvc_ttm + dvpd_ttm"),
    CatItem("recch_ttm", "RECCH", 1319, "flow", ("IncreaseDecreaseInAccountsReceivable", "IncreaseDecreaseInReceivables",
                                                "IncreaseDecreaseInAccountsAndNotesReceivable")
            + _i("AdjustmentsForDecreaseIncreaseInTradeAndOtherReceivables", "AdjustmentsForDecreaseIncreaseInTradeAccountReceivable"),
            "CF", r"receivable|unbilled|contract assets",
            tags=r"^increasedecreasein.*receivable|^adjustmentsfor.*receivable|^increasedecreaseincontractwithcustomerasset"
                 r"|^increasedecreaseinunbilled|^increasedecreaseinoperatingcapital",
            exclude=r"loans|provision|allowance|doubtful|sale|proceeds|purchase|acqui|investment|collection"),
    CatItem("invch_ttm", "INVCH", 1320, "flow", ("IncreaseDecreaseInInventories",)
            + _i("AdjustmentsForDecreaseIncreaseInInventories"),
            "CF", r"inventor", knowledge_zero=True,
            tags=r"^increasedecreasein.*inventor|^adjustmentsfor.*inventor|^increasedecreaseinoperatingcapital",
            exclude=r"write|reserve|valuation|obsolesc|transfer|provision|impairment|charge|step-up|fair value|lifo"),
    CatItem("apalch_ttm", "APALCH", 1321, "flow", ("IncreaseDecreaseInAccountsPayable", "IncreaseDecreaseInAccountsPayableTrade",
                                                  "IncreaseDecreaseInAccountsPayableAndAccruedLiabilities")
            + _i("AdjustmentsForIncreaseDecreaseInTradeAndOtherPayables", "AdjustmentsForIncreaseDecreaseInTradeAccountPayable"),
            "CF", r"accounts payable|trade payables|accrued (expenses|liabilities|compensation)|payables",
            tags=r"^increasedecreasein(accountspayable|accruedliabilities|otheraccruedliabilities|employeerelated"
                 r"|accountspayabletrade|accountspayablerelated|otheraccountspayable|accruedsalaries|interestpayable"
                 r"|tradeandotherpayables|accountspayableandaccrued|accountspayableandother|accruedliabilitiesandother"
                 r"|accruedcompensation)|^adjustmentsforincreasedecreasein.*payable|^increasedecreaseinoperatingcapital",
            exclude=r"tax|notes payable|proceeds|repay|borrow|debt|dividend|interest"),
    CatItem("exre_ttm", "EXRE", 1323, "flow", ("EffectOfExchangeRateOnCashCashEquivalentsRestrictedCashAndRestrictedCashEquivalents",
                                              "EffectOfExchangeRateOnCashAndCashEquivalents",
                                              "EffectOfExchangeRateOnCashCashEquivalentsRestrictedCashAndRestrictedCashEquivalentsIncludingDisposalGroupAndDiscontinuedOperations")
            + _i("EffectOfExchangeRateChangesOnCashAndCashEquivalents"),
            "CF", r"exchange rate|currency|foreign exchange|translation", knowledge_zero=True,
            tags=r"^effectofexchangerate|exchangeratechanges|^effectofforeignexchange|^effectofcurrency",
            exclude=r"gain|loss on"),
    CatItem("chech_ttm", "CHECH", 1324, "flow", ("CashCashEquivalentsRestrictedCashAndRestrictedCashEquivalentsPeriodIncreaseDecreaseIncludingExchangeRateEffect",
                                                "CashAndCashEquivalentsPeriodIncreaseDecrease",
                                                "CashCashEquivalentsRestrictedCashAndRestrictedCashEquivalentsPeriodIncreaseDecreaseExcludingExchangeRateEffect",
                                                "CashAndCashEquivalentsPeriodIncreaseDecreaseExcludingExchangeRateEffect",
                                                "CashPeriodIncreaseDecrease")
            + _i("IncreaseDecreaseInCashAndCashEquivalents")),
    CatItem("txdc_ttm", "TXDC", 1327, "flow", ("DeferredIncomeTaxesAndTaxCredits", "DeferredIncomeTaxExpenseBenefit",
                                              "IncreaseDecreaseInDeferredIncomeTaxes"),
            "CF", r"deferred (income )?tax",
            tags=r"^deferredincometax|^deferredtax|^increasedecreaseindeferredincometax|^increasedecreaseindeferredtax"
                 r"|^adjustmentsfordeferredtax",
            exclude=r"assumed|acquisition|noncash|held for sale"),
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
            "IS", r"provision for (loan|credit|possible|probable)|credit loss|loan loss", sic=BANK_SIC,
            tags=r"^provisionforloan|^provisionforcredit|^creditlossexpense|^financingreceivablecreditlossexpense"
                 r"|^provisionfordoubtful|afterprovisionforloan|^provisionforotherlosses",
            exclude=r"after|tax"),
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
# statement lines that never count as an item line (subtotals, comprehensive income, supplemental disclosures)
STMT_TAG_EXCLUDE = {
    "BS": r"^(liabilitiesandstockholdersequity|stockholdersequity|liabilities$|assets$|liabilitiescurrent$|assetscurrent$"
          r"|liabilitiesnoncurrent$|assetsnoncurrent|equityandliabilities)",
    "IS": r"^(incomelossfromcontinuingoperationsbefore|incomelossfromcontinuingoperationsincludingportion"
          r"|incomelossbeforeincometax|comprehensiveincome|othercomprehensiveincome|profitloss$|netincomeloss$"
          r"|incomelossfromcontinuingoperations$|incomelossfromcontinuingoperationsper)",
    "CF": r"^(othercomprehensive|noncashorpartnoncash|businesscombinationrecognizedidentifiable|cashcashequivalentsrestrictedcash"
          r"|cashandcashequivalentsperiodincrease|cashperiodincrease|interestpaid|incometaxespaid)",
}
assert all(it.tags and it.line for it in CATALOG if it.stmt), "every zero-rule item names its tag and label patterns"


@lru_cache(maxsize=None)
def _rx(pattern: str) -> re.Pattern[str]:
    return re.compile(pattern)


def line_flag(item: CatItem, stmt: str, tag: str | None, version: str | None, adsh: str, label: str | None) -> bool:
    """catalog-pre-v2 (Python mirror of ``fundamentals.pre_flags_sql``): does this PRE line (not parenthetical) show
    ``item``?"""
    if not item.stmt or stmt not in PRE_STMTS[item.stmt]:
        return False
    t = (tag or "").lower()
    if _rx(STMT_TAG_EXCLUDE[item.stmt]).search(t):
        return False
    if item.tags and _rx(item.tags).search(t):
        return True
    if version != adsh or not item.line:
        return False
    lab = (label or "").lower()
    return bool(_rx(item.line).search(lab)) and not (item.exclude and _rx(item.exclude).search(lab))


def applicable(item: CatItem, sic: int | None) -> bool:
    """False when the item is structurally absent for this SIC (industry items outside their ranges)."""
    if not item.sic:
        return True
    return sic is not None and any(lo <= sic <= hi for lo, hi in item.sic)
