"""Stage ``fundamentals_notes`` (S4.4): segments and notes items from the SEC Financial Statement and Notes data sets.

Input: stage ``notes`` (``notes_fetch.py``): ``parts/source=*/{sub,num_items,dim}.parquet`` (periodic
forms 10-K/10-Q/10-KT/10-QT/20-F/40-F and amendments). Every fact is "as filed"; an accession present in two data
sets is read from the earliest; a fact repeated with ``iprx`` > 0 is read once (lowest ``iprx``).

Clock (every row): ``available_at`` = the filing's SUB acceptance time in UTC (EDGAR America/New_York wall clock,
DST-aware), else ``filed 00:00 UTC + 46 h`` (``clock_basis``). Amendments are separate accessions with their own
clock (new rows, nothing overwritten). ``period`` is SEC's balance-sheet date rounded to month end; ``end_date`` is
the fact's reported context end (``ddate`` - ``datp`` days); ``qtrs`` its duration in quarters (0 = instant).

Outputs:

* ``items/year=YYYY/items.parquet`` (filing year): long table, one row per fact of a registry tag (:data:`ITEMS`,
  :data:`SEGMENT_METRICS`): ``cik, adsh, form, period, fy, fp, filed, available_at, clock_basis, item_group, item,
  tag, segments (SEC DIM string, NULL = no dimensions), dimn, axis_kind (business / geographic / NULL), member,
  ddate, end_date, qtrs, value, uom, coreg, source``. Notes families keep every dimensional breakdown (plan type,
  jurisdiction, debt instrument, ...); segment metrics keep the non-dimensional total and the rows on a
  business-segment or geographic axis.
* ``segments.parquet``: segment measures per filing: rows of :data:`SEGMENT_METRICS` tags whose dimensions are one
  business-segment (``StatementBusinessSegmentsAxis``) or geographic (``srt:StatementGeographicalAxis``) member,
  alone or with a ``ConsolidationItemsAxis`` member (``consolidation_member``: OperatingSegments,
  IntersegmentElimination, ...): ``metric`` (revenue, operating_income, assets, pretax_income, capex, dda,
  long_lived_assets), ``axis_kind``, ``member``, ``tag``, ``value``, ``uom``, ``qtrs``, ``end_date``, clock.
* ``segment_recon.parquet``: per 10-K / 10-KT (not amendments) of a multi-segment filer (>= 2 business-segment
  members that are not reconciling-like, :data:`RECONCILING_RE`, over every fact of the filing): the revenue tag
  used, member count, segment sum S (current fiscal-year context, filer currency, one value per member), tagged
  reconciling items E (ConsolidationItems elimination / reconciling / corporate members without a segment
  member), the consolidated total T (the same tag without dimensions, else the first revenue-chain tag), and
  ``recon_class``: ``direct`` (|S - T| <= 1% |T|), ``with_reconciling`` (|S +/- E - T| <= 1%), else a miss class
  (``segments_exceed_total``: untagged intersegment eliminations; ``segments_below_total``: an untagged "all
  other" / corporate line or partial tagging; ``double_count``: S >= 1.9 T; ``scale_or_unit``; ``no_total``;
  ``no_segment_revenue``).
* ``notes_wide.parquet``: one row per filing: the :data:`ITEMS` values of the current period: instants at
  ``ddate = period``, flows year-to-date (``flow_qtrs`` = 4 for FY, 1/2/3 for Q1/Q2(H1)/Q3(M9)), non-dimensional,
  filer context (``coreg`` NULL), in the filer currency (the most frequent monetary unit of the filing), the first
  tag of each chain that is present. Pension items fall back to the ``RetirementPlanType=PensionPlansDefinedBenefit``
  context when the filer tags no plan total (``pension_basis``). Derived: ``debt_mat_y2_5`` = y2 + y3 + y4 + y5
  when all four are present; ``intang_impairment`` falls back to indefinite + finite-lived.
* ``segment_features.parquet`` (tier1-v4 Task 14; view ``fundamentals_notes_segment_features``): one row per
  periodic filing with at least one feature, keys ``(cik, period, adsh)``, clock ``available_at`` (the filing's
  acceptance clock above; a 10-K/A is a new row). Current-period context (``ddate = period``, ``qtrs = flow_qtrs``,
  a 10-Q falling back to its discrete quarter ``qtrs = 1``), filer currency, filer context. Revenue rows use the
  revenue chain tag whose consolidated total exists, by chain priority (the reconciliation rule), one value per
  member (segment-only context preferred over ``OperatingSegments``):

  - ``bseg_n_tagged``: reportable business-segment members (not :data:`RECONCILING_RE`) over every fact of the
    filing; ``bseg_n`` = that count, or 1 for an annual filing with consolidated revenue and no tagged member;
  - ``bseg_rev_hhi``: sum of squared shares of the positive reportable segment revenues (>= 2 needed when >= 2
    members are tagged); 1.0 for a single-segment annual filing with positive consolidated revenue;
  - ``foreign_sales_share``: 1 - domestic / consolidated revenue (domestic: the member equal to the SUB business
    country ``countryba``, US synonyms for a US filer, or ``Domestic``), else the aggregate foreign member
    (:data:`FOREIGN_RE`; :data:`NON_US_RE` for a US filer) / consolidated revenue; values outside [-1%, 101%] are
    NULL (``foreign_basis = out_of_range``), the rest clipped to [0, 1];
  - ``major_cust_share``: the largest single-customer revenue on ``srt:MajorCustomersAxis`` (alone or with the
    ``ConcentrationRiskBy*`` axes of a customer-concentration context; customer classes :data:`CUSTOMER_CLASS_RE`
    excluded) / consolidated revenue, in (0, 1];
  - debt-maturity ladder ``dd1``..``dd5``, ``dd_after5``, ``dd_rem_fy`` (Compustat DD1-DD5), ``dd_n_buckets``,
    ``dd_sched_total`` (tagged buckets, missing = 0; the remainder bucket only when ``dd1`` is absent),
    ``dd1_share``; operating / finance leases (``oplease_liab`` falls back to current + noncurrent) and the
    ASC 842 payment ladder ``mrc1``..``mrc5``, ``mrcta``; ``stkco`` (stock compensation; year-to-date in a 10-Q).

  A later vintage of the same ``(cik, period)`` (an amendment) that does not report a block inherits it from the
  latest earlier vintage that does (:data:`CARRY_BLOCKS`, counted in ``carry_n``), so each row is the state of
  knowledge at its ``available_at``; the earlier row is never changed.
"""

from __future__ import annotations

import argparse
import bisect
import json
import re
import shutil
import sys
import time
from pathlib import Path
from typing import Any

from . import common as C
from . import notes_fetch as N

STAGE = "fundamentals_notes"
SCHEMA = "atx.alpha-panel.fundamentals-notes/v1"
CLOCK_RULE = "notes-accepted-v1"
FC1_HOURS = 46
DUCKDB_MEMORY = "300MB"                   # under a 0.6 GiB guard cap; tables live in a scratch db file
THREADS = 1                               # derived pivots: 0.68 GiB peak with 2 threads, 0.42 with 1 (2026-10-03)
STITCH_MEMORY = "150MB"                   # DuckDB limit while pyarrow stitches the yearly parts
ITEMS_THREADS = 2                         # items extraction (joins, no wide pivot): within the 0.6 GiB cap
RECON_TOL = 0.01
MODULES = ("fund_notes", "fund_notes_structure", "fund_notes_semantic", "fund_notes_disclosure", "notes_fetch", "common")

#: (item, group, kind, concept chain in priority order)
ITEMS: tuple[tuple[str, str, str, tuple[str, ...]], ...] = (
    # debt maturities
    ("debt_mat_y1", "debt", "instant", ("LongTermDebtMaturitiesRepaymentsOfPrincipalInNextTwelveMonths",
                                         "LongTermDebtMaturitiesRepaymentsOfPrincipalInNextRollingTwelveMonths")),
    ("debt_mat_y2", "debt", "instant", ("LongTermDebtMaturitiesRepaymentsOfPrincipalInYearTwo",
                                         "LongTermDebtMaturitiesRepaymentsOfPrincipalInRollingYearTwo")),
    ("debt_mat_y3", "debt", "instant", ("LongTermDebtMaturitiesRepaymentsOfPrincipalInYearThree",
                                         "LongTermDebtMaturitiesRepaymentsOfPrincipalInRollingYearThree")),
    ("debt_mat_y4", "debt", "instant", ("LongTermDebtMaturitiesRepaymentsOfPrincipalInYearFour",
                                         "LongTermDebtMaturitiesRepaymentsOfPrincipalInRollingYearFour")),
    ("debt_mat_y5", "debt", "instant", ("LongTermDebtMaturitiesRepaymentsOfPrincipalInYearFive",
                                         "LongTermDebtMaturitiesRepaymentsOfPrincipalInRollingYearFive")),
    ("debt_mat_after5", "debt", "instant", ("LongTermDebtMaturitiesRepaymentsOfPrincipalAfterYearFive",
                                             "LongTermDebtMaturitiesRepaymentsOfPrincipalInRollingAfterYearFive")),
    ("debt_mat_remainder_fy", "debt", "instant", ("LongTermDebtMaturitiesRepaymentsOfPrincipalRemainderOfFiscalYear",)),
    ("debt_lt", "debt", "instant", ("LongTermDebt", "LongTermDebtAndCapitalLeaseObligationsIncludingCurrentMaturities",
                                     "DebtAndCapitalLeaseObligations")),
    ("debt_lt_current", "debt", "instant", ("LongTermDebtCurrent", "LongTermDebtAndCapitalLeaseObligationsCurrent")),
    ("debt_lt_noncurrent", "debt", "instant", ("LongTermDebtNoncurrent", "LongTermDebtAndCapitalLeaseObligations")),
    # leases
    ("oplease_liab", "lease", "instant", ("OperatingLeaseLiability",)),
    ("oplease_liab_current", "lease", "instant", ("OperatingLeaseLiabilityCurrent",)),
    ("oplease_liab_noncurrent", "lease", "instant", ("OperatingLeaseLiabilityNoncurrent",)),
    ("oplease_rou", "lease", "instant", ("OperatingLeaseRightOfUseAsset",)),
    ("oplease_pay_y1", "lease", "instant", ("LesseeOperatingLeaseLiabilityPaymentsDueNextTwelveMonths",
                                             "LesseeOperatingLeaseLiabilityPaymentsDueNextRollingTwelveMonths")),
    ("oplease_pay_y2", "lease", "instant", ("LesseeOperatingLeaseLiabilityPaymentsDueYearTwo",
                                             "LesseeOperatingLeaseLiabilityPaymentsDueInRollingYearTwo")),
    ("oplease_pay_y3", "lease", "instant", ("LesseeOperatingLeaseLiabilityPaymentsDueYearThree",
                                             "LesseeOperatingLeaseLiabilityPaymentsDueInRollingYearThree")),
    ("oplease_pay_y4", "lease", "instant", ("LesseeOperatingLeaseLiabilityPaymentsDueYearFour",
                                             "LesseeOperatingLeaseLiabilityPaymentsDueInRollingYearFour")),
    ("oplease_pay_y5", "lease", "instant", ("LesseeOperatingLeaseLiabilityPaymentsDueYearFive",
                                             "LesseeOperatingLeaseLiabilityPaymentsDueInRollingYearFive")),
    ("oplease_pay_after5", "lease", "instant", ("LesseeOperatingLeaseLiabilityPaymentsDueAfterYearFive",
                                                 "LesseeOperatingLeaseLiabilityPaymentsDueInRollingAfterYearFive")),
    ("oplease_pay_total", "lease", "instant", ("LesseeOperatingLeaseLiabilityPaymentsDue",)),
    ("oplease_rate", "lease", "instant", ("OperatingLeaseWeightedAverageDiscountRatePercent",)),
    ("finlease_liab", "lease", "instant", ("FinanceLeaseLiability",)),
    ("finlease_liab_current", "lease", "instant", ("FinanceLeaseLiabilityCurrent",)),
    ("finlease_liab_noncurrent", "lease", "instant", ("FinanceLeaseLiabilityNoncurrent",)),
    ("finlease_rou", "lease", "instant", ("FinanceLeaseRightOfUseAsset",)),
    ("finlease_pay_total", "lease", "instant", ("FinanceLeaseLiabilityPaymentsDue",)),
    ("oplease_cost", "lease", "flow", ("OperatingLeaseCost",)),
    ("oplease_payments", "lease", "flow", ("OperatingLeasePayments",)),
    ("finlease_amort", "lease", "flow", ("FinanceLeaseRightOfUseAssetAmortization",)),
    ("finlease_interest", "lease", "flow", ("FinanceLeaseInterestExpense",)),
    ("lease_cost", "lease", "flow", ("LeaseCost",)),
    ("variable_lease_cost", "lease", "flow", ("VariableLeaseCost",)),
    ("rent_expense", "lease", "flow", ("OperatingLeasesRentExpenseNet", "OperatingLeasesRentExpenseMinimumRentals")),
    # pension / OPEB
    ("pension_pbo", "pension", "instant", ("DefinedBenefitPlanBenefitObligation",)),
    ("pension_assets", "pension", "instant", ("DefinedBenefitPlanFairValueOfPlanAssets",)),
    ("pension_funded_status", "pension", "instant", ("DefinedBenefitPlanFundedStatusOfPlan",)),
    ("pension_abo", "pension", "instant", ("DefinedBenefitPlanAccumulatedBenefitObligation",)),
    ("pension_expense", "pension", "flow", ("DefinedBenefitPlanNetPeriodicBenefitCost",
                                             "DefinedBenefitPlanNetPeriodicBenefitCostCredit")),
    ("pension_service_cost", "pension", "flow", ("DefinedBenefitPlanServiceCost",)),
    ("pension_interest_cost", "pension", "flow", ("DefinedBenefitPlanInterestCost",)),
    ("pension_expected_return", "pension", "flow", ("DefinedBenefitPlanExpectedReturnOnPlanAssets",)),
    ("pension_employer_contrib", "pension", "flow", ("DefinedBenefitPlanContributionsByEmployer",
                                                      "DefinedBenefitPlanPlanAssetsContributionsByEmployer")),
    ("pension_discount_rate", "pension", "instant", ("DefinedBenefitPlanAssumptionsUsedCalculatingBenefitObligationDiscountRate",)),
    ("dc_expense", "pension", "flow", ("DefinedContributionPlanCostRecognized", "DefinedContributionPlanCost")),
    # share-based compensation
    ("sbc", "sbc", "flow", ("ShareBasedCompensation", "AllocatedShareBasedCompensationExpense",
                             "ShareBasedPaymentArrangementNoncashExpense", "ShareBasedPaymentArrangementExpense")),
    ("sbc_expense", "sbc", "flow", ("AllocatedShareBasedCompensationExpense", "ShareBasedPaymentArrangementExpense")),
    ("sbc_tax_benefit", "sbc", "flow", ("EmployeeServiceShareBasedCompensationTaxBenefitFromCompensationExpense",
                                         "ShareBasedPaymentArrangementExpenseTaxBenefit",
                                         "AllocatedShareBasedCompensationExpenseNetOfTax")),
    ("sbc_unrecognized", "sbc", "instant", ("EmployeeServiceShareBasedCompensationNonvestedAwardsTotalCompensationCostNotYetRecognized",
                                             "ShareBasedPaymentArrangementNonvestedAwardCostNotYetRecognizedAmount")),
    # income tax
    ("tax_total", "tax", "flow", ("IncomeTaxExpenseBenefit",)),
    ("tax_current", "tax", "flow", ("CurrentIncomeTaxExpenseBenefit",)),
    ("tax_deferred", "tax", "flow", ("DeferredIncomeTaxExpenseBenefit",)),
    ("tax_cur_federal", "tax", "flow", ("CurrentFederalTaxExpenseBenefit",)),
    ("tax_cur_state", "tax", "flow", ("CurrentStateAndLocalTaxExpenseBenefit",)),
    ("tax_cur_foreign", "tax", "flow", ("CurrentForeignTaxExpenseBenefit",)),
    ("tax_def_federal", "tax", "flow", ("DeferredFederalIncomeTaxExpenseBenefit",)),
    ("tax_def_state", "tax", "flow", ("DeferredStateAndLocalIncomeTaxExpenseBenefit",)),
    ("tax_def_foreign", "tax", "flow", ("DeferredForeignIncomeTaxExpenseBenefit",)),
    ("pretax_domestic", "tax", "flow", ("IncomeLossFromContinuingOperationsBeforeIncomeTaxesDomestic",)),
    ("pretax_foreign", "tax", "flow", ("IncomeLossFromContinuingOperationsBeforeIncomeTaxesForeign",)),
    ("tax_rate_effective", "tax", "flow", ("EffectiveIncomeTaxRateContinuingOperations",)),
    ("tax_rate_statutory", "tax", "flow", ("EffectiveIncomeTaxRateReconciliationAtFederalStatutoryIncomeTaxRate",)),
    ("tax_utb", "tax", "instant", ("UnrecognizedTaxBenefits",)),
    ("tax_dta_gross", "tax", "instant", ("DeferredTaxAssetsGross", "DeferredTaxAssetsGrossNoncurrent")),
    ("tax_dta_valuation_allowance", "tax", "instant", ("DeferredTaxAssetsValuationAllowance",)),
    ("tax_dta_net", "tax", "instant", ("DeferredTaxAssetsNetOfValuationAllowance", "DeferredTaxAssetsNet")),
    ("tax_dtl", "tax", "instant", ("DeferredTaxLiabilities", "DeferredIncomeTaxLiabilities")),
    ("tax_nol", "tax", "instant", ("OperatingLossCarryforwards",)),
    # goodwill, intangibles, impairments
    ("goodwill", "impairment", "instant", ("Goodwill",)),
    ("gw_impairment", "impairment", "flow", ("GoodwillImpairmentLoss",)),
    ("intang_impairment", "impairment", "flow", ("ImpairmentOfIntangibleAssetsExcludingGoodwill",)),
    ("intang_impairment_indef", "impairment", "flow", ("ImpairmentOfIntangibleAssetsIndefinitelivedExcludingGoodwill",)),
    ("intang_impairment_finite", "impairment", "flow", ("ImpairmentOfIntangibleAssetsFinitelived",)),
    ("gw_intang_impairment", "impairment", "flow", ("GoodwillAndIntangibleAssetImpairment",)),
    ("asset_impairment", "impairment", "flow", ("AssetImpairmentCharges",)),
    ("lla_impairment", "impairment", "flow", ("ImpairmentOfLongLivedAssetsHeldForUse",
                                               "ImpairmentOfLongLivedAssetsToBeDisposedOf")),
)
#: segment metric -> tags (priority order: the revenue chain is also the reconciliation chain)
SEGMENT_METRICS: dict[str, tuple[str, ...]] = {
    "revenue": ("Revenues", "RevenueFromContractWithCustomerExcludingAssessedTax",
                "RevenueFromContractWithCustomerIncludingAssessedTax", "SalesRevenueNet", "SalesRevenueGoodsNet",
                "SalesRevenueServicesNet", "RevenuesNetOfInterestExpense", "SegmentReportingInformationRevenue",
                "RevenueFromExternalCustomers", "InterestAndDividendIncomeOperating"),
    "operating_income": ("OperatingIncomeLoss", "SegmentReportingInformationOperatingIncomeLoss"),
    "pretax_income": ("IncomeLossFromContinuingOperationsBeforeIncomeTaxesExtraordinaryItemsNoncontrollingInterest",
                      "IncomeLossFromContinuingOperationsBeforeIncomeTaxesMinorityInterestAndIncomeLossFromEquityMethodInvestments"),
    "assets": ("Assets", "SegmentReportingInformationAssets"),
    "long_lived_assets": ("LongLivedAssets", "NoncurrentAssets", "PropertyPlantAndEquipmentNet"),
    "capex": ("PaymentsToAcquirePropertyPlantAndEquipment", "SegmentExpenditureAdditionToLongLivedAssets"),
    "dda": ("DepreciationDepletionAndAmortization", "DepreciationAndAmortization", "DepreciationAmortizationAndAccretionNet"),
}
#: tier-2 segment revenue: industry formats (insurance, health care, utilities, lessors), chosen only when no tier-1
#: chain tag has segment rows; their consolidated totals are mostly not landed, so T falls back to the tier-1 total
REVENUE_TIER2: tuple[str, ...] = (
    "PremiumsEarnedNet", "SupplementaryInsuranceInformationPremiumRevenue", "InsuranceServicesRevenue",
    "HealthCareOrganizationRevenue", "HealthCareOrganizationRevenueNetOfPatientServiceRevenueProvisions",
    "RegulatedAndUnregulatedOperatingRevenue", "RegulatedOperatingRevenue", "ElectricUtilityRevenue",
    "OperatingLeasesIncomeStatementLeaseRevenue", "OperatingLeaseLeaseIncome", "RevenueNotFromContractWithCustomer",
    "RealEstateRevenueNet")
#: tier-3 segment revenue: filer extension tags named as revenue / sales, on a business-segment axis only
CUSTOM_REVENUE_RE = r"(?i)(segment|net|total|operating)*(revenues?|sales)(net|fromcontracts?withcustomers?|fromexternalcustomers)?"
TIER2_PRIO, CUSTOM_PRIO = 100, 200
FULL_REVENUE_TAGS = ('Revenues', 'RevenueFromContractWithCustomerExcludingAssessedTax',
                    'RevenueFromContractWithCustomerIncludingAssessedTax', 'SalesRevenueNet',
                    'RevenuesNetOfInterestExpense', 'SegmentReportingInformationRevenue',
                    'RevenueFromExternalCustomers', 'HealthCareOrganizationRevenue',
                    'RegulatedAndUnregulatedOperatingRevenue', 'InsuranceServicesRevenue')
AXES = {"business": "BusinessSegments", "geographic": "Geographical"}
# Filing-specific primary-disclosure corroboration; generic geographic/legal containers
# elsewhere do not establish operating-segment identity. Source receipts: Task14 round2 pilot.
DISCLOSED_REPORTABLE_CONTAINERS = {
    '0001140361-23-014888': 'ReportableLegalEntities',
    '0001722482-24-000012': 'ReportableGeographicalComponents',
    '0001982518-25-000007': 'ReportableGeographicalComponents',
}


def reportable_container_sql(alias: str = '') -> str:
    prefix = alias + '.' if alias else ''
    exceptions = ' OR '.join(f"({prefix}adsh='{adsh}' AND {prefix}consolidation_member='{member}')"
                             for adsh, member in DISCLOSED_REPORTABLE_CONTAINERS.items()) or 'false'
    return f"({prefix}consolidation_member IS NULL OR {prefix}consolidation_member='OperatingSegments' OR {exceptions})"

#: business-segment members that are not a reportable segment (reconciling lines, totals)
RECONCILING_RE = (r"(?i)elimina|corporate|reconcil|unallocat|intersegment|intercompany|consolidat|^total|segmenttotal|"
                  r"adjustment|nonsegment")
RECON_CONSOLIDATION_RE = r"(?i)elimina|reconcil|corporate|nonsegment|unallocat|other"
PENSION_FALLBACK = "RetirementPlanType=PensionPlansDefinedBenefit;"
SEGMENTS_ONLY_RE = r"^(BusinessSegments|Geographical)=[^;]*;(ConsolidationItems=[^;]*;)?$|^ConsolidationItems=[^;]*;(BusinessSegments|Geographical)=[^;]*;$"
CUSTOMER_AXIS = "MajorCustomers"
#: a customer-only context once the concentration-risk qualifiers are removed
CONC_AXES_RE = r"ConcentrationRiskBy(Benchmark|Type)=[^;]*;"
#: MajorCustomers members that are customer classes or aggregates, not a single customer
CUSTOMER_CLASS_RE = (r"(?i)other|commercial|industrial|residential|consumer|retail|wholesale|municipal|institution|"
                     r"individual|^total|^all|affiliate|thirdpart|external|domestic|foreign|international|"
                     r"distributor|reseller|^top|largest\d|customers$")
#: geographic members: the filer's home market and the aggregate foreign member
DOMESTIC_RE = r"(?i)^(domestic|countryofdomicile|homecountry)$"
US_RE = r"(?i)^(us|usa|unitedstates|unitedstatesofamerica)$"
FOREIGN_RE = r"(?i)^(foreign|foreigncountries|allforeigncountries|totalforeign|international|totalinternational|nondomestic)$"
NON_US_RE = r"(?i)^(non-?us|nonunitedstates|outside(of)?(the)?(us|unitedstates)|foreignexcludingus)$"
SHARE_TOL = 0.01
DEDUP_REL = 1e-4                          # a roll-up / duplicate member equals its children within 0.01%
DEDUP_MAX_N = 16                          # subset search bound (2^16 sums per candidate parent)
#: S4.4 gates (gap-analysis item 12), per filing year of the score window
GATE_YEARS = tuple(range(2020, 2026))
GATE_SEGMENT_REVENUE = 0.90
GATE_RECONCILED = 0.95
RECONCILED = "'direct', 'with_reconciling', 'reconciled_search'"
STALE_DAYS = 548                          # member-cell coverage: an annual row is fresh for 18 months after period


def fp_quarters(fp: str | None, form: str | None) -> int | None:
    """Year-to-date quarters of a filing's own flow context (FY -> 4, Q1 -> 1, Q2/H1 -> 2, Q3/M9 -> 3)."""
    m = {"FY": 4, "Q4": 4, "CY": 4, "H2": 4, "Q1": 1, "Q2": 2, "H1": 2, "Q3": 3, "M9": 3}
    if fp and fp.upper() in m:
        return m[fp.upper()]
    if form and form.upper().split("/")[0] in ("10-K", "10-KT", "20-F", "40-F"):
        return 4
    return None


def fp_quarters_sql(fp: str, form: str) -> str:
    return (f"CASE upper({fp}) WHEN 'FY' THEN 4 WHEN 'Q4' THEN 4 WHEN 'CY' THEN 4 WHEN 'H2' THEN 4 WHEN 'Q1' THEN 1 "
            f"WHEN 'Q2' THEN 2 WHEN 'H1' THEN 2 WHEN 'Q3' THEN 3 WHEN 'M9' THEN 3 "
            f"ELSE CASE WHEN split_part(upper({form}), '/', 1) IN ('10-K', '10-KT', '20-F', '40-F') THEN 4 END END")


def rev_prio_sql(col: str) -> str:
    """Revenue tag priority: tier-1 chain 0.., tier-2 industry tags 100.., anything else (filer extensions) 200."""
    t1 = " ".join(f"WHEN '{t}' THEN {i}" for i, t in enumerate(SEGMENT_METRICS["revenue"]))
    t2 = " ".join(f"WHEN '{t}' THEN {TIER2_PRIO + i}" for i, t in enumerate(REVENUE_TIER2))
    return f"CASE {col} {t1} {t2} ELSE {CUSTOM_PRIO} END"


def recon_class(seg_sum: float | None, total: float | None, recon: float | None, n_members: int,
                tol: float = RECON_TOL) -> str:
    """Python twin of the SQL reconciliation classes (tested)."""
    if n_members == 0 or seg_sum is None:
        return "no_segment_revenue"
    if total is None:
        return "no_total"
    if total == 0:
        return "direct" if seg_sum == 0 else "zero_total"
    t = abs(total)
    if abs(seg_sum - total) <= tol * t:
        return "direct"
    if recon is not None and (abs(seg_sum + recon - total) <= tol * t or abs(seg_sum - recon - total) <= tol * t):
        return "with_reconciling"
    r = seg_sum / total
    if 900 <= abs(r) <= 1100 or 0.0009 <= abs(r) <= 0.0011:
        return "scale_or_unit"
    if r >= 1.9:
        return "double_count"
    return "segments_exceed_total" if seg_sum > total else "segments_below_total"


def recon_class_sql(s: str, t: str, e: str, n: str, tol: float = RECON_TOL) -> str:
    return f"""CASE WHEN {n} = 0 OR {s} IS NULL THEN 'no_segment_revenue'
        WHEN {t} IS NULL THEN 'no_total'
        WHEN {t} = 0 THEN CASE WHEN {s} = 0 THEN 'direct' ELSE 'zero_total' END
        WHEN abs({s} - {t}) <= {tol} * abs({t}) THEN 'direct'
        WHEN {e} IS NOT NULL AND (abs({s} + {e} - {t}) <= {tol} * abs({t}) OR abs({s} - {e} - {t}) <= {tol} * abs({t}))
             THEN 'with_reconciling'
        WHEN abs({s} / {t}) BETWEEN 900 AND 1100 OR abs({s} / {t}) BETWEEN 0.0009 AND 0.0011 THEN 'scale_or_unit'
        WHEN {s} / {t} >= 1.9 THEN 'double_count'
        WHEN {s} > {t} THEN 'segments_exceed_total' ELSE 'segments_below_total' END"""


def _fits(s: float, total: float, recon: float | None = None, tol: float = RECON_TOL) -> bool:
    t = abs(total)
    if recon is None:
        return abs(s - total) <= tol * t
    return abs(s + recon - total) <= tol * t or abs(s - recon - total) <= tol * t


def dedup_members(vals: dict[str, float], dups: bool = True, rel: float = DEDUP_REL,
                  max_n: int = DEDUP_MAX_N) -> tuple[dict[str, float], list[str]]:
    """Diagnostic arithmetic candidates only; never evidence of an economic disclosure hierarchy.
    Drop business-segment members that roll up others (a member equal, within ``rel``, to the sum of >= 2
    smaller members) and, with ``dups``, members that repeat another member (same non-zero value): XBRL member
    hierarchies on one axis (group and sub-segments) read as one flat list. Returns (kept, dropped sorted)."""
    keep = dict(sorted(vals.items()))
    dropped: list[str] = []
    seen: dict[float, str] = {}
    for m, v in list(keep.items()):                 # duplicates: keep the first member name
        if v == 0 or not dups:
            continue
        key = next((k for k in seen if abs(k - v) <= rel * abs(v)), None)
        if key is not None:
            dropped.append(m)
            del keep[m]
        else:
            seen[v] = m
    while True:                                     # roll-ups: drop the largest, then search again
        pos = sorted((v, m) for m, v in keep.items() if v > 0)
        if len(pos) < 3 or len(pos) > max_n:
            break
        sums: list[tuple[float, int]] = [(0.0, 0)]  # subset sums (and sizes) of the members below the candidate
        hit = None
        for v, m in pos:
            multi = sorted(s for s, k in sums if k >= 2)
            i = bisect.bisect_left(multi, v * (1 - rel))
            if i < len(multi) and multi[i] <= v * (1 + rel):
                hit = m                             # ascending pass: the last hit is the largest roll-up
            sums += [(s + v, k + 1) for s, k in sums]
        if hit is None:
            break
        dropped.append(hit)
        del keep[hit]
    return keep, sorted(dropped)


def recon_search(members: dict[str, dict[str, float]], total: float | None, tag: str | None,
                 comps: list[float], prio: tuple[str, ...] = SEGMENT_METRICS["revenue"] + REVENUE_TIER2,
                 tol: float = RECON_TOL) -> dict[str, Any] | None:
    """Second-pass reconciliation of a strict miss. ``members`` = {tag: {member: value}} (business axis,
    segment-only context, current fiscal year), ``total`` = consolidated revenue T, ``tag`` = the strict tag,
    ``comps`` = disclosed reconciling amounts (each ConsolidationItems member without a segment, their sum, the
    per-segment elimination sum). First success wins:

    1. ``components``: S +/- one disclosed component (overlapping ConsolidationItems members were double-summed);
    2. ``dedup``: S of :func:`dedup_members`, directly or +/- one component;
    3. ``reportable``: 1-2 without the reconciling-like members (:data:`RECONCILING_RE`: corporate, intercompany, ...);
    4. ``mixed_tags``: per member the highest-priority chain tag it is tagged with, then 1-2.
    """
    if total is None or total == 0 or not tag or not members.get(tag):
        return None

    def attempt(vals: dict[str, float], rule: str) -> dict[str, Any] | None:
        s = sum(vals.values())
        for e in comps:
            if e is not None and _fits(s, total, e, tol):
                return {"rule": f"{rule}components", "members": vals, "dropped": [], "seg_sum": s, "reconciling": e}
        for dups in (False, True):                  # roll-ups alone first: equal segments are not always copies
            kept, dropped = dedup_members(vals, dups)
            if not dropped:
                continue
            s2 = sum(kept.values())
            for e in (None, *comps):
                if (e is None and _fits(s2, total, None, tol)) or (e is not None and _fits(s2, total, e, tol)):
                    return {"rule": f"{rule}dedup" + ("" if e is None else "+components"), "members": kept,
                            "dropped": dropped, "seg_sum": s2, "reconciling": e}
        return None

    if (r := attempt(members[tag], "")) is not None:
        return r
    rep_only = {m: v for m, v in members[tag].items() if not re.search(RECONCILING_RE, m)}
    if rep_only and rep_only != members[tag]:
        if _fits(sum(rep_only.values()), total, None, tol):
            return {"rule": "reportable", "members": rep_only, "dropped": sorted(set(members[tag]) - set(rep_only)),
                    "seg_sum": sum(rep_only.values()), "reconciling": None}
        if (r := attempt(rep_only, "reportable+")) is not None:
            return r
    rank = {t: i for i, t in enumerate(prio)}
    merged: dict[str, float] = {}
    for t in sorted(members, key=lambda t: rank.get(t, 99)):
        for m, v in members[t].items():
            merged.setdefault(m, v)
    if merged != members[tag]:
        if _fits(sum(merged.values()), total, None, tol):
            return {"rule": "mixed_tags", "members": merged, "dropped": [], "seg_sum": sum(merged.values()),
                    "reconciling": None}
        return attempt(merged, "mixed_tags+")
    return None


def _registry_rows() -> list[tuple[str, str, str, str, int]]:
    rows = [(tag, item, grp, kind, i) for item, grp, kind, chain in ITEMS for i, tag in enumerate(chain)]
    rows += [(tag, m, "segment", "segment", i) for m, chain in SEGMENT_METRICS.items() for i, tag in enumerate(chain)]
    rows += [(tag, "revenue", "segment", "segment", TIER2_PRIO + i) for i, tag in enumerate(REVENUE_TIER2)]
    return rows


def build(years: list[int] | None = None) -> dict[str, Any]:
    import pyarrow as pa

    pa.set_memory_pool(pa.system_memory_pool())     # freed Arrow buffers go back to the OS (stitch under the cap)
    t0 = time.perf_counter()
    receipt: dict[str, Any] = {"clock_rule": CLOCK_RULE}
    parts = N.parts_dir()
    nman = C.stage_dir(N.STAGE) / "manifest.json"
    if not nman.exists():
        raise RuntimeError("notes stage has no manifest: run notes_fetch finalize first")
    con = C.connect(memory=DUCKDB_MEMORY, threads=THREADS, db_file="fundamentals_notes.duckdb")
    g_sub = (parts / "source=*" / "sub.parquet").as_posix()
    keys = [p.name.split("=", 1)[1] for p in parts.glob("source=*")]
    con.execute("CREATE TABLE src (source VARCHAR, ord INTEGER)")
    con.executemany("INSERT INTO src VALUES (?, ?)", [(k, i) for i, k in enumerate(sorted(keys, key=N.order_key))])
    con.execute("CREATE TABLE reg (tag VARCHAR, item VARCHAR, item_group VARCHAR, kind VARCHAR, prio INTEGER)")
    con.executemany("INSERT INTO reg VALUES (?, ?, ?, ?, ?)", _registry_rows())
    per = ", ".join(f"'{f}'" for f in N.PERIODIC_FORMS)
    con.execute(f"""
        CREATE TABLE sub AS
        SELECT s.adsh, s.cik, s.form, s.period, s.fy, s.fp, s.filed, s.source, s.countryba,
               coalesce(s.accepted_utc, CAST(s.filed AS TIMESTAMP) AT TIME ZONE 'UTC' + INTERVAL {FC1_HOURS} HOUR) AS available_at,
               CASE WHEN s.accepted_utc IS NULL THEN 'filed_plus_46h' ELSE 'accepted_utc' END AS clock_basis
        FROM read_parquet('{g_sub}', hive_partitioning = false) s JOIN src USING (source)
        WHERE s.form IN ({per})
        QUALIFY row_number() OVER (PARTITION BY s.adsh ORDER BY src.ord) = 1""")
    out = C.stage_dir(STAGE)
    con.execute('CREATE TABLE business_census (adsh VARCHAR, member VARCHAR)')
    all_years = [r[0] for r in con.execute("SELECT DISTINCT year(filed) FROM sub ORDER BY 1").fetchall()]
    receipt["items_rows"] = {}
    ax_b, ax_g, ax_c = AXES["business"], AXES["geographic"], CUSTOMER_AXIS
    con.execute("""CREATE TABLE regt AS SELECT tag, first(item ORDER BY prio, item) AS item,
                          first(item_group ORDER BY prio, item) AS item_group FROM reg GROUP BY tag""")
    con.execute(f"SET threads = {ITEMS_THREADS}")
    for y in years or all_years:
        # the data sets that supplied this year's accessions (in practice the year's own: filed dates lie in the span)
        srcs = [r[0] for r in con.execute(f"SELECT DISTINCT source FROM sub WHERE year(filed) = {y} ORDER BY 1").fetchall()]
        g_num = [(parts / f"source={k}" / "num_items.parquet").as_posix() for k in srcs]
        g_dim = [(parts / f"source={k}" / "dim.parquet").as_posix() for k in srcs]
        with C.timed(receipt, f"items_{y}"):
            # 1) registry-tag facts of the year's filings, one per fact key (lowest iprx), in the scratch db
            con.execute(f"""
                CREATE OR REPLACE TABLE ny AS
                SELECT x.* FROM read_parquet({g_num}, hive_partitioning = false) x
                JOIN sub s ON s.adsh = x.adsh AND s.source = x.source
                WHERE year(s.filed) = {y} AND x.value IS NOT NULL
                  AND ((x.version <> x.adsh AND x.tag IN (SELECT tag FROM regt))
                       OR (x.version = x.adsh AND regexp_full_match(x.tag, '{CUSTOM_REVENUE_RE}')))
                QUALIFY row_number() OVER (PARTITION BY x.adsh, x.tag, x.version, x.ddate, x.qtrs, x.uom, x.dimh,
                                                        coalesce(x.coreg, '') ORDER BY x.iprx) = 1""")
            # 2) the dimension strings those facts reference
            con.execute(f"""
                CREATE OR REPLACE TABLE dimy AS
                SELECT d.dimhash, d.segments, d.source FROM read_parquet({g_dim}, hive_partitioning = false) d
                SEMI JOIN (SELECT DISTINCT dimh, source FROM ny) u ON u.dimh = d.dimhash AND u.source = d.source""")
            sql = f"""
                SELECT s.cik, n.adsh, s.form, s.period, s.fy, s.fp, s.filed, s.available_at, s.clock_basis,
                       coalesce(r.item_group, 'segment') AS item_group, coalesce(r.item, 'revenue') AS item, n.tag,
                       n.version = n.adsh AS is_custom, n.segments, n.dimn,
                       CASE WHEN n.segments LIKE '%{ax_b}=%' THEN 'business' WHEN n.segments LIKE '%{ax_g}=%' THEN 'geographic'
                            WHEN n.segments LIKE '%{ax_c}=%' THEN 'customer' END AS axis_kind,
                       coalesce(nullif(regexp_extract(n.segments, '(^|;){ax_b}=([^;]*)', 2), ''),
                                nullif(regexp_extract(n.segments, '(^|;){ax_g}=([^;]*)', 2), ''),
                                nullif(regexp_extract(n.segments, '(^|;){ax_c}=([^;]*)', 2), '')) AS member,
                       nullif(regexp_extract(n.segments, '(^|;)ConsolidationItems=([^;]*)', 2), '') AS consolidation_member,
                       n.ddate, CAST(n.ddate - CAST(round(coalesce(n.datp, 0)) AS INTEGER) AS DATE) AS end_date,
                       n.qtrs, n.value, n.uom, n.coreg, n.source
                FROM (SELECT x.*, d.segments FROM ny x LEFT JOIN dimy d ON d.dimhash = x.dimh AND d.source = x.source) n
                JOIN sub s ON s.adsh = n.adsh AND s.source = n.source
                LEFT JOIN regt r ON r.tag = n.tag AND n.version <> n.adsh
                WHERE (r.tag IS NOT NULL AND (r.item_group <> 'segment' OR n.segments IS NULL OR n.segments LIKE '%{ax_b}=%'
                       OR n.segments LIKE '%{ax_g}=%' OR n.segments LIKE '%ConsolidationItems=%'
                       OR (r.item = 'revenue' AND n.segments LIKE '%{ax_c}=%')))
                   OR (r.tag IS NULL AND n.segments LIKE '%{ax_b}=%')       -- filer revenue extension on a segment axis
                ORDER BY s.cik, n.adsh, item_group, item, n.tag, n.ddate, n.qtrs, n.segments NULLS FIRST, n.uom,
                         n.coreg NULLS FIRST, n.value"""
            receipt["items_rows"][str(y)] = C.copy_to_parquet(con, sql, out / "items" / f"year={y}" / "items.parquet", 65536)
        print(f"fund_notes items {y}: {receipt['items_rows'][str(y)]:,}", flush=True)
    # segments, reconciliation and wide rows per filing year (each filing's facts live in its year's items file),
    # stitched in year order: bounded tables under the guard
    tmp = C.build_root() / "_tmp" / "fundamentals_notes_parts"
    if tmp.exists():
        shutil.rmtree(tmp)
    con.execute(f"SET threads = {THREADS}")
    # Restricted --years controls item extraction, but stitching republishes every existing year.
    # Recompute the independent census for the same complete output scope, including omitted years.
    done_years = sorted(int(d.name.split("=", 1)[1]) for d in (out / "items").glob("year=*"))
    with C.timed(receipt, 'business_census'):
        collect_business_census(con, parts, done_years)
    con.execute('CREATE OR REPLACE TEMP TABLE business_census_original AS SELECT * FROM business_census')
    con.execute('CREATE OR REPLACE TEMP TABLE structure_evidence(adsh VARCHAR,tag VARCHAR,proof_json VARCHAR)')
    C.copy_to_parquet(con, """SELECT s.cik,s.adsh,s.form,s.period,s.filed,s.available_at,
        count(DISTINCT b.member) AS n_members, count(DISTINCT b.member)>=2 AS multi_segment
        FROM sub s LEFT JOIN business_census b USING(adsh)
        WHERE year(s.filed) IN (""" + ','.join(map(str,done_years)) + """ )
        GROUP BY ALL ORDER BY s.filed,s.adsh""",
        out / 'segment_census.parquet')
    for kind in ("segments", "recon_strict", "recon", "wide", "features"):
        (tmp / kind).mkdir(parents=True)
    for y in done_years:
        items = (out / "items" / f"year={y}" / "items.parquet").as_posix()
        with C.timed(receipt, f"derived_{y}"):
            con.execute(f"""
                CREATE OR REPLACE TABLE cur AS
                SELECT adsh, arg_max(uom, n) AS currency FROM (
                    SELECT adsh, uom, count(*) AS n FROM read_parquet('{items}')
                    WHERE uom NOT IN ('shares', 'pure') AND regexp_full_match(uom, '[A-Z]{{3}}') GROUP BY 1, 2)
                GROUP BY 1""")
            seg = tmp / "segments" / f"{y}.parquet"
            C.copy_to_parquet(con, segments_sql(items), seg)
            rc, wd = tmp / "recon" / f"{y}.parquet", tmp / "wide" / f"{y}.parquet"
            strict = tmp / "recon_strict" / f"{y}.parquet"
            C.copy_to_parquet(con, recon_sql(items, seg.as_posix(), y), strict)
            con.register("recon_ov", recon_overlay(con, items, seg.as_posix(), strict.as_posix()))
            C.copy_to_parquet(con, recon_final_sql(strict.as_posix()), rc)
            con.unregister("recon_ov")
            C.copy_to_parquet(con, wide_sql(items), wd, 32768)
            C.copy_to_parquet(con, features_sql(items, seg.as_posix(), wd.as_posix(), rc.as_posix()),
                              tmp / "features" / f"{y}.parquet", 32768)
    # Apply source-verified role identities to the economic census, retaining raw counts.
    C.copy_to_parquet(con,"""SELECT s.cik,s.adsh,s.form,s.period,s.filed,s.available_at,
        count(DISTINCT b.member) AS n_members,count(DISTINCT b.member)>=2 AS multi_segment,
        count(DISTINCT raw.member) AS n_members_source_tagged,
        CASE WHEN max(e.adsh) IS NULL THEN 'current_period_numeric_tags' ELSE 'source_role_structure' END AS member_basis
        FROM sub s LEFT JOIN business_census b USING(adsh)
        LEFT JOIN business_census_original raw USING(adsh) LEFT JOIN structure_evidence e USING(adsh)
        WHERE year(s.filed) IN ("""+','.join(map(str,done_years))+""") GROUP BY ALL ORDER BY s.filed,s.adsh""",
        out/'segment_census.parquet')
    C.copy_to_parquet(con,'''SELECT e.*,s.cik,s.period,s.filed,s.available_at,s.form
        FROM structure_evidence e JOIN (SELECT DISTINCT adsh,cik,period,filed,available_at,form FROM sub) s USING(adsh)
        ORDER BY adsh,tag''',out/'segment_structure.parquet')
    con.execute("DROP TABLE IF EXISTS ny")
    con.execute("DROP TABLE IF EXISTS dimy")
    con.execute(f"SET memory_limit = '{STITCH_MEMORY}'")   # pyarrow stitches next (0xC0000005 at the cap, 2026-10-03)
    with C.timed(receipt, "stitch"):
        receipt["segments_rows"] = N.stitch_parquet([tmp / "segments" / f"{y}.parquet" for y in done_years], out / "segments.parquet")
        receipt["recon_rows"] = N.stitch_parquet([tmp / "recon" / f"{y}.parquet" for y in done_years], out / "segment_recon.parquet")
        receipt["wide_rows"] = N.stitch_parquet([tmp / "wide" / f"{y}.parquet" for y in done_years], out / "notes_wide.parquet", 32768)
        raw = tmp / "features_raw.parquet"
        N.stitch_parquet([tmp / "features" / f"{y}.parquet" for y in done_years], raw, 32768)
        receipt["features_rows"] = C.copy_to_parquet(con, carry_sql(raw.as_posix()), out / "segment_features.parquet", 32768)
    shutil.rmtree(tmp, ignore_errors=True)
    with C.timed(receipt, "coverage"):
        receipt["coverage"] = coverage(con, out)
        receipt["coverage"]["cells"] = cell_coverage(con, out)
    receipt["gate_status"] = gate_status(receipt["coverage"])
    receipt["coverage"]["gate_status"] = receipt["gate_status"]
    C.write_json_atomic(out / "coverage.json", receipt["coverage"])
    receipt["timings_s"]["total"] = round(time.perf_counter() - t0, 1)
    payload = {
        "gate_status": receipt["gate_status"],
        "clock_rule": CLOCK_RULE,
        "clock_text": ("available_at = SUB accepted (EDGAR America/New_York wall clock, DST-aware) in UTC; else filed "
                       f"00:00 UTC + {FC1_HOURS} h. Amendments are separate accessions with their own clock."),
        "input_manifests_sha256": {"notes": C.sha256_file(nman)},
        "items": {i: {"group": g, "kind": k, "chain": list(ch)} for i, g, k, ch in ITEMS},
        "segment_metrics": {k: list(v) for k, v in SEGMENT_METRICS.items()},
        "rules": {"reconciling_member_re": RECONCILING_RE, "recon_consolidation_re": RECON_CONSOLIDATION_RE,
                  "recon_tolerance": RECON_TOL, "segments_only_re": SEGMENTS_ONLY_RE, "pension_fallback": PENSION_FALLBACK,
                  "customer_axis": CUSTOMER_AXIS, "conc_axes_re": CONC_AXES_RE, "customer_class_re": CUSTOMER_CLASS_RE,
                  "domestic_re": DOMESTIC_RE, "us_re": US_RE, "foreign_re": FOREIGN_RE, "non_us_re": NON_US_RE,
                  "share_tolerance": SHARE_TOL, "stale_days_member_coverage": STALE_DAYS},
        "verification": {"arithmetic_candidates": "diagnostic only, never economic member deletion",
                         "disclosed_reportable_container_exceptions": DISCLOSED_REPORTABLE_CONTAINERS,
                         "reportable_container_provenance": "Task14 round2 primary-document/linkbase receipts; exceptions accession-scoped",
                         "restricted_year_scope": "item extraction restricted; census recomputed for every republished partition year",
                         "extra_context_projection": "explicit exhaustive timing_transfer_partition only",
                         "full_revenue_tags": list(FULL_REVENUE_TAGS),
                         "source_structure": "filing/role/concept/period/context IDs + compressed source SHA; conflicting/incomplete graphs rejected; source identities independent of arithmetic verification",
                         "monthly_cutoff": "month-end session 22:00 UTC",
                         "denominator_certified": False},
        "receipt": receipt,
    }
    C.write_stage_manifest(STAGE, SCHEMA, MODULES, payload)
    con.close()
    _drop_scratch("fundamentals_notes.duckdb")
    return receipt


def collect_business_census(con, parts: Path, years: list[int]) -> None:
    """Independent current-period members for every republished year, one bounded source at a time."""
    for year in years:
        srcs = con.execute(f'SELECT DISTINCT source FROM sub WHERE year(filed)={year} ORDER BY source').fetchall()
        for (source,) in srcs:
            part = parts / f'source={source}'
            con.execute(f"""CREATE OR REPLACE TEMP TABLE census_dim AS SELECT dimhash,
                regexp_extract(segments,'(^|;)BusinessSegments=([^;]*)',2) AS member
                FROM read_parquet('{(part/'dim.parquet').as_posix()}')
                WHERE segments LIKE '%BusinessSegments=%' AND NOT regexp_matches(
                    regexp_extract(segments,'(^|;)BusinessSegments=([^;]*)',2),'{RECONCILING_RE}')""")
            con.execute(f"""INSERT INTO business_census SELECT DISTINCT n.adsh,d.member
                FROM read_parquet('{(part/'num_items.parquet').as_posix()}') n
                JOIN sub s ON n.adsh=s.adsh AND n.source=s.source JOIN census_dim d ON n.dimh=d.dimhash
                WHERE year(s.filed)={year} AND n.ddate=s.period AND n.coreg IS NULL""")


def segments_sql(items: str) -> str:
    """Project only the exhaustive, mutually exclusive ASC 606 timing partition.

    Both explicit timing members must occur, with no other extra axis, and no plain
    segment total for that fact key. No equality or subset fitting is used. Original
    contexts are retained as provenance. Other extra-axis contexts stay in items.
    """
    timing = 'TimingOfTransferOfGoodOrService'
    return f"""
      WITH i AS (SELECT * FROM read_parquet('{items}') WHERE item_group='segment' AND coreg IS NULL
                        AND segments IS NOT NULL),
      b AS (SELECT * FROM i WHERE regexp_matches(segments,'{SEGMENTS_ONLY_RE}')),
      p AS (SELECT *, regexp_replace(segments,
           '{timing}=(TransferredOverTime|TransferredAtPointInTime);','','g') AS projected,
           regexp_extract(segments,'{timing}=([^;]*)',1) AS timing_member FROM i WHERE item='revenue'),
      t AS (SELECT cik,adsh,form,period,fy,fp,filed,available_at,clock_basis,item,axis_kind,member,
            consolidation_member,tag,projected AS segments,sum(value) AS value,uom,ddate,end_date,qtrs,source,
            list(segments ORDER BY segments) AS context_provenance
            FROM p WHERE regexp_matches(projected,'{SEGMENTS_ONLY_RE}') AND timing_member IN
                 ('TransferredOverTime','TransferredAtPointInTime')
            GROUP BY ALL HAVING count(*)=2 AND count(value)=2 AND count(DISTINCT timing_member)=2)
      SELECT cik,adsh,form,period,fy,fp,filed,available_at,clock_basis,item AS metric,axis_kind,member,
             consolidation_member,tag,segments,value,uom,ddate,end_date,qtrs,source,
             'plain' AS context_basis,[segments] AS context_provenance FROM b
      UNION ALL
      SELECT t.* EXCLUDE(context_provenance), 'timing_transfer_partition' AS context_basis,context_provenance
      FROM t WHERE NOT EXISTS (SELECT 1 FROM b WHERE b.adsh=t.adsh AND b.item=t.item AND b.tag=t.tag
           AND b.member=t.member AND b.ddate=t.ddate AND b.qtrs=t.qtrs AND b.uom=t.uom)
      ORDER BY cik,adsh,metric,axis_kind,member,tag,ddate,qtrs,segments,uom,value"""


def recon_sql(items: str, seg: str, filing_year: int) -> str:
    """Business-segment revenue reconciliation per original 10-K / 10-KT (current fiscal-year context)."""
    full = ','.join(repr(t) for t in FULL_REVENUE_TAGS)
    total = f"CASE WHEN b.tag IN ({full}) THEN coalesce(nullif(b.own_total, 0), a.total) END"
    return f"""
        WITH k AS (
            SELECT i.cik, i.adsh, i.form, i.period, i.fy, i.fp, i.filed, i.available_at, i.clock_basis, c.currency
            FROM sub i LEFT JOIN cur c USING (adsh)
            WHERE i.form IN ('10-K', '10-KT') AND year(i.filed)={filing_year}
        ),
        mem AS (          -- business-segment members of the filing (any fact), reportable-looking only
            SELECT adsh,count(DISTINCT member) AS n_members_all,count(DISTINCT member) AS n_members
            FROM business_census GROUP BY 1
        ),
        rv AS (           -- one value per (filing, tag, member): segment-only context preferred over OperatingSegments
            SELECT s.adsh, s.tag, s.member, s.uom,
                   arg_min(s.value, CASE WHEN s.consolidation_member IS NULL THEN 0 ELSE 1 END) AS value
            FROM read_parquet('{seg}') s JOIN k USING (adsh)
            WHERE s.metric = 'revenue' AND s.axis_kind = 'business' AND s.qtrs = 4 AND s.ddate = k.period
              AND s.uom = k.currency AND {reportable_container_sql('s')}
              AND NOT regexp_matches(s.member, '(?i)^total|^consolidat|segmenttotal')
            GROUP BY 1, 2, 3, 4
        ),
        sums AS (
            SELECT adsh, tag, count(*) AS n_rev_members, sum(value) AS seg_sum FROM rv GROUP BY 1, 2
        ),
        tot AS (
            SELECT i.adsh, i.tag, arg_min(i.value, i.ddate) AS total
            FROM read_parquet('{items}', hive_partitioning = false) i JOIN k USING (adsh)
            WHERE i.item_group = 'segment' AND i.item = 'revenue' AND i.segments IS NULL AND i.coreg IS NULL
              AND i.qtrs = 4 AND i.ddate = k.period AND i.uom = k.currency
            GROUP BY 1, 2
        ),
        rec AS (          -- tagged reconciling items (no segment member): eliminations, corporate, reconciling items
            SELECT i.adsh, i.tag, sum(i.value) AS recon
            FROM read_parquet('{items}', hive_partitioning = false) i JOIN k USING (adsh)
            WHERE i.item_group = 'segment' AND i.item = 'revenue' AND i.coreg IS NULL AND i.qtrs = 4 AND i.ddate = k.period
              AND i.uom = k.currency AND i.member IS NULL AND i.segments IS NOT NULL
              AND regexp_full_match(i.segments, 'ConsolidationItems=[^;]*;')
              AND regexp_matches(i.consolidation_member, '{RECON_CONSOLIDATION_RE}')
            GROUP BY 1, 2
        ),
        cand AS (
            SELECT s.adsh, s.tag, s.n_rev_members, s.seg_sum, t.total AS own_total, r.recon,
                   {rev_prio_sql('s.tag')} AS prio
            FROM sums s LEFT JOIN tot t USING (adsh, tag) LEFT JOIN rec r USING (adsh, tag)
        ),
        best AS (         -- tier 1 first; then the chain tag whose (non-zero) total exists; then priority
            SELECT * FROM cand
            QUALIFY row_number() OVER (PARTITION BY adsh ORDER BY prio >= {TIER2_PRIO}, coalesce(own_total, 0) = 0, prio) = 1
        ),
        anytot AS (       -- consolidated revenue fallback: the first tier-1 chain tag with a non-zero total
            SELECT adsh, arg_min(total, {rev_prio_sql('tag')}) AS total, arg_min(tag, {rev_prio_sql('tag')}) AS tag
            FROM tot WHERE total <> 0 AND tag IN ({full}) GROUP BY 1
        )
        SELECT k.cik, k.adsh, k.form, k.period, k.fy, k.fp, k.filed, k.available_at, k.clock_basis, k.currency,
               coalesce(m.n_members, 0) AS n_members, coalesce(m.n_members_all, 0) AS n_members_all,
               coalesce(m.n_members, 0) >= 2 AS multi_segment,
               b.tag AS revenue_tag,
               CASE WHEN b.prio < {TIER2_PRIO} THEN 1 WHEN b.prio < {CUSTOM_PRIO} THEN 2 WHEN b.prio IS NOT NULL THEN 3 END
                   AS revenue_tier,
               coalesce(b.n_rev_members, 0) AS n_revenue_members, b.seg_sum,
               b.recon AS reconciling_items, {total} AS consolidated_revenue,
               coalesce(nullif(b.own_total,0),a.total) AS candidate_total,
               CASE WHEN b.tag IN ({full}) THEN 'full_revenue_concept' ELSE 'component_or_extension_unverified' END AS total_basis,
               CASE WHEN b.tag IN ({full}) THEN CASE WHEN coalesce(b.own_total, 0) <> 0 THEN b.tag ELSE a.tag END END AS consolidated_tag,
               (b.seg_sum - coalesce(nullif(b.own_total, 0), a.total)) / nullif(abs(coalesce(nullif(b.own_total, 0), a.total)), 0)
                   AS rel_diff,
               {recon_class_sql('b.seg_sum', total, 'b.recon', 'coalesce(b.n_rev_members, 0)')}
                   AS recon_class
        FROM k LEFT JOIN mem m USING (adsh) LEFT JOIN best b USING (adsh) LEFT JOIN anytot a USING (adsh)
        ORDER BY k.filed, k.cik, k.adsh"""


def _overlay_schema():
    import pyarrow as pa
    return pa.schema([("adsh", pa.string()), ("recon_rule", pa.string()), ("candidate_only", pa.bool_()), ("dropped_members", pa.list_(pa.string())),
                      ("seg_sum_reconciled", pa.float64()), ("reconciling_used", pa.float64()),
                      ("seg_members", pa.list_(pa.struct([("member", pa.string()), ("value", pa.float64())])))])


def recon_overlay(con, items: str, seg: str, strict: str):
    """Arrow table of :func:`recon_search` results for the strict misses of one filing year (multi-segment 10-Ks
    with segment revenue and a consolidated total, class not direct / with_reconciling)."""
    import pyarrow as pa
    con.execute(f"""
        CREATE OR REPLACE TEMP TABLE miss AS
        SELECT adsh, period, currency, revenue_tag, consolidated_revenue, reconciling_items FROM read_parquet('{strict}')
        WHERE multi_segment AND n_revenue_members >= 1 AND consolidated_revenue IS NOT NULL AND consolidated_revenue <> 0
          AND recon_class NOT IN ('direct', 'with_reconciling')""")
    mem: dict[str, dict[str, dict[str, float]]] = {}
    for adsh, tag, member, value in con.execute(f"""
            SELECT s.adsh, s.tag, s.member, arg_min(s.value, CASE WHEN s.consolidation_member IS NULL THEN 0 ELSE 1 END)
            FROM read_parquet('{seg}') s JOIN miss k USING (adsh)
            WHERE s.metric = 'revenue' AND s.axis_kind = 'business' AND s.qtrs = 4 AND s.ddate = k.period
              AND s.uom = k.currency AND {reportable_container_sql('s')}
              AND NOT regexp_matches(s.member, '(?i)^total|^consolidat|segmenttotal')
            GROUP BY 1, 2, 3 ORDER BY 1, 2, 3""").fetchall():
        mem.setdefault(adsh, {}).setdefault(tag, {})[member] = value
    comps: dict[str, list[float]] = {}
    for adsh, value in con.execute(f"""
            SELECT i.adsh, sum(i.value) FROM read_parquet('{items}', hive_partitioning = false) i JOIN miss k USING (adsh)
            WHERE i.item_group = 'segment' AND i.item = 'revenue' AND i.tag = k.revenue_tag AND i.coreg IS NULL
              AND i.qtrs = 4 AND i.ddate = k.period AND i.uom = k.currency AND i.member IS NULL
              AND regexp_full_match(i.segments, 'ConsolidationItems=[^;]*;')
              AND regexp_matches(i.consolidation_member, '{RECON_CONSOLIDATION_RE}')
            GROUP BY i.adsh, i.consolidation_member ORDER BY 1, 2
            """).fetchall():
        comps.setdefault(adsh, []).append(value)
    for adsh, value in con.execute(f"""
            SELECT s.adsh, sum(s.value) FROM read_parquet('{seg}') s JOIN miss k USING (adsh)
            WHERE s.metric = 'revenue' AND s.axis_kind = 'business' AND s.tag = k.revenue_tag AND s.qtrs = 4
              AND s.ddate = k.period AND s.uom = k.currency AND regexp_matches(s.consolidation_member, '(?i)elimina')
            GROUP BY 1 ORDER BY 1""").fetchall():
        comps.setdefault(adsh, []).append(value)
    rows = []
    from . import fund_notes_structure as FS
    rawroot = Path('data/raw/sec_notes_t14_pilot')
    cached_keys=FS.cached_accessions(rawroot)
    for adsh, _per, _cur, tag, total, recon in con.execute("SELECT * FROM miss ORDER BY adsh").fetchall():
        values=mem.get(adsh, {}).get(tag, {})
        proof=FS.cached_structure(rawroot,adsh,tag,str(_per),set(values)) if adsh in cached_keys else {'status':'missing_links'}
        if proof['status']=='verified_structure':
            own_totals=con.execute(f"""SELECT DISTINCT value FROM read_parquet('{items}',hive_partitioning=false)
              WHERE adsh=? AND tag=? AND ddate=? AND qtrs=4 AND uom=? AND coreg IS NULL
                AND segments IS NULL AND value IS NOT NULL""",[adsh,tag,_per,_cur]).fetchall()
            if len(own_totals)!=1 or own_totals[0][0]!=total:
                # A role-correct segment concept cannot borrow a different GAAP/component total.
                continue
            selected={m:values[m] for m in proof['members']}
            bridge=[]
            bridge_contexts={m:{'context_ids':ids,'segments':f'ConsolidationItems={m};'} for m,ids in proof.get('bridge_context_ids',{}).items()}
            bridge_contexts.update(proof.get('source_bridge_contexts',{}))
            for member,source_context in bridge_contexts.items():
                context_ids=source_context['context_ids']
                facts=con.execute(f"""SELECT DISTINCT value,segments FROM read_parquet('{items}',hive_partitioning=false)
                  WHERE adsh=? AND tag=? AND ddate=? AND qtrs=4 AND uom=? AND coreg IS NULL
                    AND segments=? AND value IS NOT NULL""",
                  [adsh,tag,_per,_cur,source_context['segments']]).fetchall()
                if len(facts)==1:
                    bridge.append({'member':member,'value':facts[0][0],'segments':facts[0][1],'context_ids':context_ids})
            proof['source_bridge_terms']=bridge
            # Structural identity is independent of reconciliation success and arithmetic values.
            con.execute('DELETE FROM business_census WHERE adsh=?',[adsh])
            con.execute('INSERT INTO business_census SELECT ?,unnest(?)',[adsh,proof['members']])
            con.execute('INSERT INTO structure_evidence VALUES (?,?,?)',[adsh,tag,json.dumps(proof)])
            rows.append({'adsh':adsh,'recon_rule':'source_role_structure','candidate_only':False,
                         'dropped_members':sorted(set(values)-set(selected)),
                         'seg_sum_reconciled':sum(selected.values()),'reconciling_used':sum(t['value'] for t in bridge) if bridge else None,
                         'seg_members':[{'member':m,'value':v} for m,v in sorted(selected.items())]})
            continue
        cs = comps.get(adsh, []) + ([recon] if recon is not None else [])
        r = recon_search(mem.get(adsh, {}), total, tag, cs)
        if r is not None:
            rows.append({"adsh": adsh, "recon_rule": r["rule"], "dropped_members": r["dropped"],
                         "candidate_only": r["rule"] != "components",
                         "seg_sum_reconciled": r["seg_sum"], "reconciling_used": r["reconciling"],
                         "seg_members": [{"member": m, "value": v} for m, v in sorted(r["members"].items())]})
    return pa.Table.from_pylist(rows, schema=_overlay_schema())


def recon_final_sql(strict: str) -> str:
    """Strict recon rows plus the search overlay (registered as ``recon_ov``): ``recon_class`` becomes
    ``reconciled_search`` where the search succeeded; ``recon_class_strict`` keeps the strict class."""
    return f"""
        SELECT r.* REPLACE (CASE WHEN o.adsh IS NOT NULL AND NOT o.candidate_only
               AND (o.recon_rule<>'source_role_structure' OR abs(o.seg_sum_reconciled+coalesce(o.reconciling_used,0)-r.consolidated_revenue)<=.01*abs(r.consolidated_revenue))
               THEN 'reconciled_search' ELSE r.recon_class END AS recon_class,
               CASE WHEN o.recon_rule='source_role_structure' THEN len(o.seg_members) ELSE r.n_members END AS n_members,
               CASE WHEN o.recon_rule='source_role_structure' THEN len(o.seg_members) ELSE r.n_revenue_members END AS n_revenue_members,
               CASE WHEN o.recon_rule='source_role_structure' THEN o.reconciling_used ELSE r.reconciling_items END AS reconciling_items,
               CASE WHEN o.recon_rule='source_role_structure' THEN (o.seg_sum_reconciled-r.consolidated_revenue)/nullif(abs(r.consolidated_revenue),0) ELSE r.rel_diff END AS rel_diff,
               CASE WHEN o.recon_rule='source_role_structure' THEN o.seg_sum_reconciled ELSE r.seg_sum END AS seg_sum),
               r.recon_class AS recon_class_strict,r.rel_diff AS rel_diff_strict,
               CASE WHEN o.recon_rule='source_role_structure' THEN (o.seg_sum_reconciled+coalesce(o.reconciling_used,0)-r.consolidated_revenue)/nullif(abs(r.consolidated_revenue),0) END AS rel_diff_after_source_bridge,
               o.recon_rule, o.dropped_members, o.seg_sum_reconciled,
               o.reconciling_used, o.seg_members, coalesce(o.candidate_only, false) AS candidate_only
        FROM read_parquet('{strict}') r LEFT JOIN recon_ov o USING (adsh)
        ORDER BY r.filed, r.cik, r.adsh"""


def wide_sql(items: str) -> str:
    notes_items = [i for i, g, k, ch in ITEMS]
    pv = ",\n".join(f"max(value) FILTER (WHERE item = '{i}') AS {i}" for i in notes_items)
    return f"""
        WITH f AS (
            SELECT DISTINCT i.cik, i.adsh, i.form, i.period, i.fy, i.fp, i.filed, i.available_at, i.clock_basis,
                   c.currency, {fp_quarters_sql('i.fp', 'i.form')} AS flow_qtrs
            FROM read_parquet('{items}', hive_partitioning = false) i LEFT JOIN cur c USING (adsh)
        ),
        v AS (
            SELECT i.adsh, r.item, r.prio, i.value, i.segments, i.tag
            FROM read_parquet('{items}', hive_partitioning = false) i
            JOIN reg r ON r.tag = i.tag            -- a tag may serve several items (e.g. sbc and sbc_expense)
            JOIN f USING (adsh)
            WHERE r.item_group <> 'segment' AND i.coreg IS NULL AND i.ddate = f.period
              AND (i.segments IS NULL OR (r.item_group = 'pension' AND i.segments = '{PENSION_FALLBACK}'))
              AND (i.uom = f.currency OR i.uom IN ('pure', 'shares') OR f.currency IS NULL)
              AND ((r.kind = 'instant' AND i.qtrs = 0) OR (r.kind = 'flow' AND i.qtrs = f.flow_qtrs))
            QUALIFY row_number() OVER (PARTITION BY i.adsh, r.item ORDER BY i.segments IS NOT NULL, r.prio, i.tag) = 1
        ),
        p AS (
            SELECT adsh, {pv},
                   max(CASE WHEN item LIKE 'pension%' THEN CASE WHEN segments IS NULL THEN 'total' ELSE 'defined_benefit_pension_member' END END) AS pension_basis
            FROM v GROUP BY 1
        )
        SELECT f.*, p.* EXCLUDE (adsh, intang_impairment),
               coalesce(p.intang_impairment,
                        CASE WHEN p.intang_impairment_indef IS NOT NULL OR p.intang_impairment_finite IS NOT NULL
                             THEN coalesce(p.intang_impairment_indef, 0) + coalesce(p.intang_impairment_finite, 0) END) AS intang_impairment,
               p.debt_mat_y2 + p.debt_mat_y3 + p.debt_mat_y4 + p.debt_mat_y5 AS debt_mat_y2_5
        FROM f LEFT JOIN p USING (adsh)
        ORDER BY f.filed, f.cik, f.adsh"""


#: segment_features value columns (a filing gets a row when any of these is non-null)
FEATURE_COLS = ("bseg_n", "bseg_rev_hhi", "foreign_sales_share", "major_cust_share", "dd1", "dd2", "dd3", "dd4",
                "dd5", "dd_after5", "dd_rem_fy", "oplease_liab", "oplease_rou", "oplease_cost", "oplease_rate",
                "finlease_liab", "mrc1", "mrc2", "mrc3", "mrc4", "mrc5", "mrcta", "stkco", "sbc_unrecognized")
DD_BUCKETS = ("dd1", "dd2", "dd3", "dd4", "dd5", "dd_after5")
#: (key column, columns) inherited together by a later vintage of the same (cik, period) whose key is NULL, from
#: the latest earlier vintage whose key is set: each row is the state of knowledge at its available_at
CARRY_BLOCKS: tuple[tuple[str, tuple[str, ...]], ...] = (
    ("bseg_n", ("bseg_n", "bseg_basis", "bseg_n_tagged", "bseg_rev_n", "bseg_rev_tag", "bseg_rev_rule", "bseg_rev_hhi",
                "bseg_recon_class", "bseg_partition_quality", "bseg_partition_source_adsh")),
    ("geo_rev_n", ("geo_rev_n", "foreign_sales_share", "foreign_basis", "foreign_sales_quality")),
    ("major_cust_n", ("major_cust_n", "major_cust_share")),
    ("dd_sched_total", (*DD_BUCKETS, "dd_rem_fy", "dd_n_buckets", "dd_sched_total", "dd1_share")),
    *((c, (c,)) for c in ("oplease_liab", "oplease_rou", "oplease_cost", "oplease_rate", "finlease_liab", "mrc1",
                          "mrc2", "mrc3", "mrc4", "mrc5", "mrcta", "stkco", "sbc_unrecognized")),
)


def carry_sql(raw: str) -> str:
    """Stitched features with :data:`CARRY_BLOCKS` inheritance across vintages of one (cik, period); ``carry_n``
    counts the blocks a row inherited (0 for an original filing)."""
    win = [f"last_value(CASE WHEN {k} IS NOT NULL THEN struct_pack({', '.join(f'{c} := {c}' for c in cols)}) END "
           f"IGNORE NULLS) OVER win AS _c{i}" for i, (k, cols) in enumerate(CARRY_BLOCKS)]
    rep = [f"CASE WHEN {k} IS NULL AND _c{i} IS NOT NULL THEN _c{i}.{c} ELSE {c} END AS {c}"
           for i, (k, cols) in enumerate(CARRY_BLOCKS) for c in cols]
    n = " + ".join(f"({k} IS NULL AND _c{i} IS NOT NULL)::INTEGER" for i, (k, _) in enumerate(CARRY_BLOCKS))
    ex = ", ".join(f"_c{i}" for i in range(len(CARRY_BLOCKS)))
    return f"""
        WITH w AS (
            SELECT r.*, {', '.join(win)} FROM read_parquet('{raw}') r
            WINDOW win AS (PARTITION BY cik, period ORDER BY available_at, adsh
                           ROWS BETWEEN UNBOUNDED PRECEDING AND 1 PRECEDING)
        )
        SELECT * EXCLUDE ({ex}) REPLACE ({', '.join(rep)}), {n} AS carry_n
        FROM w ORDER BY filed, cik, adsh"""


def features_sql(items: str, seg: str, wide: str, recon: str) -> str:
    """Per filing segment features (see the module docstring) from one filing year's items / segments / wide /
    recon files; ``sub`` (with ``countryba``) and ``cur`` are tables of the connection."""
    tot_like = "(?i)^total|^consolidat|segmenttotal"
    dom = (f"(g.member = f.country OR regexp_full_match(g.member, '{DOMESTIC_RE}') "
           f"OR (f.country = 'US' AND regexp_full_match(g.member, '{US_RE}')))")
    frgn = f"(regexp_full_match(g.member, '{FOREIGN_RE}') OR (f.country = 'US' AND regexp_full_match(g.member, '{NON_US_RE}')))"
    nn = " OR ".join(f"{c} IS NOT NULL" for c in FEATURE_COLS)
    dd_n = " + ".join(f"({c} IS NOT NULL)::INTEGER" for c in DD_BUCKETS)
    dd_sum = " + ".join(f"coalesce({c}, 0)" for c in DD_BUCKETS)
    lo, hi = -SHARE_TOL, 1 + SHARE_TOL
    full = ','.join(repr(t) for t in FULL_REVENUE_TAGS)
    return f"""
        WITH f AS (
            SELECT w.cik, w.adsh, w.form, w.period, w.fy, w.fp, w.filed, w.available_at, w.clock_basis, w.currency,
                   w.flow_qtrs, coalesce(nullif(upper(trim(s.countryba)), ''), 'US') AS country
            FROM read_parquet('{wide}') w LEFT JOIN sub s USING (adsh)
        ),
        ctx AS (          -- current-period revenue facts of the filing (filer context and currency)
            SELECT i.adsh, i.tag, i.qtrs, i.axis_kind, i.member, i.consolidation_member, i.segments, i.value,
                   {rev_prio_sql('i.tag')} AS prio, f.flow_qtrs
            FROM (SELECT adsh,tag,qtrs,axis_kind,member,consolidation_member,segments,value,item_group,item,coreg,ddate,uom
                  FROM read_parquet('{items}', hive_partitioning = false)
                  UNION ALL SELECT adsh,tag,qtrs,axis_kind,member,consolidation_member,segments,value,
                                   'segment',metric,NULL,ddate,uom
                  FROM read_parquet('{seg}') WHERE context_basis='timing_transfer_partition') i JOIN f USING (adsh)
            WHERE i.item_group = 'segment' AND i.item = 'revenue' AND i.coreg IS NULL AND i.ddate = f.period
              AND i.uom = f.currency AND (i.qtrs = f.flow_qtrs OR (f.flow_qtrs IN (2, 3) AND i.qtrs = 1))
        ),
        tot AS (SELECT adsh, tag, qtrs, min(prio) AS prio, min(value) AS total FROM ctx WHERE segments IS NULL GROUP BY 1, 2, 3),
        anytot AS (SELECT adsh, qtrs, arg_min(total, prio) AS total FROM tot WHERE total <> 0 AND tag IN ({full})
                   GROUP BY 1, 2),
        sm AS (           -- one value per (filing, axis, tag, qtrs, member)
            SELECT adsh, axis_kind, tag, qtrs, member, min(prio) AS prio, max(flow_qtrs) AS flow_qtrs,
                   arg_min(value, CASE WHEN consolidation_member IS NULL THEN 0 ELSE 1 END) AS value
            FROM ctx
            WHERE member IS NOT NULL AND (
                  (axis_kind IN ('business', 'geographic') AND regexp_matches(segments, '{SEGMENTS_ONLY_RE}')
                   AND {reportable_container_sql()}
                   AND NOT regexp_matches(member, '{tot_like}'))
               OR (axis_kind = 'customer'
                   AND regexp_full_match(regexp_replace(segments, '{CONC_AXES_RE}', '', 'g'), '{CUSTOMER_AXIS}=[^;]*;')
                   AND (segments NOT LIKE '%ConcentrationRiskByType=%'
                        OR segments LIKE '%ConcentrationRiskByType=CustomerConcentrationRisk;%')
                   AND NOT regexp_matches(member, '{CUSTOMER_CLASS_RE}')))
            GROUP BY 1, 2, 3, 4, 5
        ),
        cand AS (
            SELECT s.adsh, s.axis_kind, s.tag, s.qtrs, min(s.prio) AS prio, max(s.flow_qtrs) AS flow_qtrs,
                   max(t.total) AS own_total
            FROM sm s LEFT JOIN tot t USING (adsh, tag, qtrs) GROUP BY 1, 2, 3, 4
        ),
        best AS (         -- the full-period context, tier 1, then the chain tag whose (non-zero) total exists
            SELECT c.adsh, c.axis_kind, c.tag, c.qtrs, coalesce(nullif(c.own_total, 0), a.total) AS total
            FROM cand c LEFT JOIN anytot a USING (adsh, qtrs)
            QUALIFY row_number() OVER (PARTITION BY c.adsh, c.axis_kind
                                       ORDER BY c.qtrs = c.flow_qtrs DESC, c.prio >= {TIER2_PRIO},
                                                coalesce(c.own_total, 0) = 0, c.prio) = 1
        ),
        sel AS (SELECT s.*, b.total FROM sm s JOIN best b USING (adsh, axis_kind, tag, qtrs)),
        ov AS (           -- the reconciled member set of an annual filing reconciled by the search pass
            SELECT adsh, recon_rule, dropped_members, unnest(seg_members, recursive := true)
            FROM read_parquet('{recon}') WHERE seg_members IS NOT NULL AND NOT candidate_only
        ),
        bus AS (
            SELECT adsh, any_value(tag) AS bseg_rev_tag, NULL::VARCHAR AS bseg_rev_rule,
                   count(*) FILTER (WHERE rep AND value > 0) AS bseg_rev_n,
                   sum(value) FILTER (WHERE rep AND value > 0) AS s1, sum(value * value) FILTER (WHERE rep AND value > 0) AS s2
            FROM (SELECT *, NOT regexp_matches(member, '{RECONCILING_RE}') AS rep FROM sel
                  WHERE axis_kind = 'business' AND adsh NOT IN (SELECT adsh FROM ov))
            GROUP BY 1
            UNION ALL
            SELECT o.adsh, CASE WHEN any_value(o.recon_rule) LIKE 'mixed_tags%' THEN 'mixed' ELSE any_value(r.revenue_tag) END,
                   any_value(o.recon_rule),
                   count(*) FILTER (WHERE o.rep AND o.value > 0),
                   sum(o.value) FILTER (WHERE o.rep AND o.value > 0), sum(o.value * o.value) FILTER (WHERE o.rep AND o.value > 0)
            FROM (SELECT ov.*, CASE WHEN recon_rule='source_role_structure' THEN true
                       ELSE NOT regexp_matches(ov.member, '{RECONCILING_RE}') END AS rep FROM ov) o
            JOIN read_parquet('{recon}') r ON r.adsh = o.adsh
            GROUP BY 1
        ),
        geo AS (
            SELECT g.adsh, count(*) AS geo_rev_n, any_value(g.total) AS geo_total,
                   sum(g.value) FILTER (WHERE {dom}) AS dom, sum(g.value) FILTER (WHERE NOT {dom} AND {frgn}) AS frgn
            FROM sel g JOIN f USING (adsh) WHERE g.axis_kind = 'geographic' GROUP BY 1
        ),
        geo2 AS (
            SELECT *, CASE WHEN geo_total > 0 AND dom IS NOT NULL THEN 1 - dom / geo_total
                           WHEN geo_total > 0 AND frgn IS NOT NULL THEN frgn / geo_total END AS fs_raw
            FROM geo
        ),
        cus AS (
            SELECT adsh, count(*) AS major_cust_n, max(value) AS cmax, any_value(total) AS cus_total
            FROM sel WHERE axis_kind = 'customer' GROUP BY 1
        ),
        mem AS (          -- reportable business-segment members over every fact of the filing (the recon rule),
                          -- without the roll-up / duplicate members the search pass dropped
            SELECT b.adsh,count(DISTINCT b.member) AS n,count(DISTINCT raw.member) AS n_tagged
            FROM business_census b LEFT JOIN business_census_original raw USING(adsh) GROUP BY 1
        ),
        x AS (
            SELECT f.cik, f.adsh, f.form, f.period, f.fy, f.fp, f.filed, f.available_at, f.clock_basis, f.currency,
                   f.flow_qtrs, f.flow_qtrs = 4 AS is_annual, f.form LIKE '%/A' AS is_amendment, f.country AS countryba,
                   ft.total AS rev_consolidated,
                   coalesce(m.n_tagged, 0) AS bseg_n_tagged,
                   CASE WHEN coalesce(m.n, 0) >= 1 THEN m.n
                        WHEN f.flow_qtrs = 4 AND f.form NOT LIKE '%/A' AND ft.total IS NOT NULL THEN 1 END AS bseg_n,
                   CASE WHEN f.adsh IN (SELECT adsh FROM structure_evidence) THEN 'source_role_structure'
                        WHEN coalesce(m.n,0)>=1 THEN 'current_period_numeric_tags'
                        WHEN f.flow_qtrs=4 AND f.form NOT LIKE '%/A' AND ft.total IS NOT NULL THEN 'inferred_singleton' END AS bseg_basis,
                   CASE WHEN f.adsh IN (SELECT adsh FROM structure_evidence) THEN 'source_verified_reportable'
                        WHEN coalesce(m.n,0)>=1 THEN 'tagged_unverified_reportable'
                        WHEN f.flow_qtrs=4 AND f.form NOT LIKE '%/A' AND ft.total IS NOT NULL THEN 'inferred_singleton_unverified' END AS bseg_partition_quality,
                   CASE WHEN f.adsh IN (SELECT adsh FROM structure_evidence) THEN f.adsh END AS bseg_partition_source_adsh,
                   b.bseg_rev_n, b.bseg_rev_tag, b.bseg_rev_rule,
                   CASE WHEN coalesce(m.n, 0) >= 2 AND b.bseg_rev_n >= 2 THEN b.s2 / (b.s1 * b.s1)
                        WHEN coalesce(m.n, 0) <= 1 AND (b.bseg_rev_n = 1 OR
                             (f.flow_qtrs = 4 AND f.form NOT LIKE '%/A' AND ft.total > 0)) THEN 1.0
                        END AS bseg_rev_hhi,
                   r.recon_class AS bseg_recon_class,
                   g.geo_rev_n,
                   CASE WHEN g.fs_raw BETWEEN {lo} AND {hi} THEN greatest(least(g.fs_raw, 1.0), 0.0) END AS foreign_sales_share,
                   CASE WHEN g.fs_raw IS NULL THEN NULL WHEN g.fs_raw NOT BETWEEN {lo} AND {hi} THEN 'out_of_range'
                        WHEN g.dom IS NOT NULL THEN 'domestic_member' ELSE 'foreign_member' END AS foreign_basis,
                   CASE WHEN g.fs_raw IS NOT NULL THEN 'unverified_tagged_geography' END AS foreign_sales_quality,
                   c.major_cust_n,
                   CASE WHEN c.cus_total > 0 AND c.cmax > 0 AND c.cmax <= c.cus_total THEN c.cmax / c.cus_total END AS major_cust_share,
                   w.debt_mat_y1 AS dd1, w.debt_mat_y2 AS dd2, w.debt_mat_y3 AS dd3, w.debt_mat_y4 AS dd4,
                   w.debt_mat_y5 AS dd5, w.debt_mat_after5 AS dd_after5, w.debt_mat_remainder_fy AS dd_rem_fy,
                   coalesce(w.oplease_liab, w.oplease_liab_current + w.oplease_liab_noncurrent) AS oplease_liab,
                   w.oplease_rou, w.oplease_cost,
                   CASE WHEN w.oplease_rate BETWEEN 0 AND 1 THEN w.oplease_rate END AS oplease_rate,   -- 5.1 for 5.1%: NULL
                   coalesce(w.finlease_liab, w.finlease_liab_current + w.finlease_liab_noncurrent) AS finlease_liab,
                   w.oplease_pay_y1 AS mrc1, w.oplease_pay_y2 AS mrc2, w.oplease_pay_y3 AS mrc3, w.oplease_pay_y4 AS mrc4,
                   w.oplease_pay_y5 AS mrc5, w.oplease_pay_after5 AS mrcta,
                   w.sbc AS stkco, w.sbc_unrecognized
            FROM f JOIN read_parquet('{wide}') w USING (adsh)
            LEFT JOIN (SELECT a.adsh, a.total FROM anytot a JOIN f USING (adsh) WHERE a.qtrs = f.flow_qtrs) ft USING (adsh)
            LEFT JOIN mem m USING (adsh) LEFT JOIN bus b USING (adsh) LEFT JOIN geo2 g USING (adsh)
            LEFT JOIN cus c USING (adsh) LEFT JOIN read_parquet('{recon}') r USING (adsh)
        )
        SELECT x.*, {dd_n} AS dd_n_buckets,
               CASE WHEN {dd_n} > 0 THEN {dd_sum} + CASE WHEN dd1 IS NULL THEN coalesce(dd_rem_fy, 0) ELSE 0 END
                    WHEN dd_rem_fy IS NOT NULL THEN dd_rem_fy END AS dd_sched_total,
               CASE WHEN dd1 IS NOT NULL AND {dd_sum} > 0 THEN dd1 / ({dd_sum}) END AS dd1_share
        FROM x WHERE {nn} OR is_amendment
        ORDER BY filed, cik, adsh"""


def coverage(con, out) -> dict[str, Any]:
    """S4.4 done tests per filing year: segment revenue for multi-segment 10-K filers and the reconciliation rate."""
    rc = (out / "segment_recon.parquet").as_posix()
    wide = (out / "notes_wide.parquet").as_posix()
    res: dict[str, Any] = {"segments": {}, "recon_classes": {}, "wide_nonnull_10k": {},
                          "denominator_certified": False,
                          "denominator_basis": "tag-observable original 10-K/10-KT filings; no independent census of untagged multi-segment disclosures"}
    rows = con.execute(f"""
        SELECT year(filed) AS y, count(*) AS n10k, count(*) FILTER (WHERE multi_segment) AS multi,
               count(*) FILTER (WHERE multi_segment AND n_revenue_members >= 1) AS with_rev,
               count(*) FILTER (WHERE multi_segment AND n_revenue_members >= 1 AND revenue_tier = 1) AS with_rev_t1,
               count(*) FILTER (WHERE multi_segment AND n_revenue_members >= 1 AND recon_class IN ({RECONCILED})) AS rec,
               count(*) FILTER (WHERE multi_segment AND n_revenue_members >= 1
                                AND recon_class_strict IN ('direct', 'with_reconciling')) AS rec_strict,
               count(*) FILTER (WHERE multi_segment AND n_revenue_members >= 1 AND recon_class = 'direct') AS rec_direct
        FROM read_parquet('{rc}') GROUP BY 1 ORDER BY 1""").fetchall()
    for y, n, m, w, w1, r, rs, rd in rows:
        res["segments"][str(y)] = {"ten_k": n, "multi_segment": m, "with_segment_revenue": w,
                                   "share_segment_revenue": round(w / m, 4) if m else None,
                                   "share_segment_revenue_tier1": round(w1 / m, 4) if m else None,
                                   "reconciled_1pct": r, "share_reconciled": round(r / w, 4) if w else None,
                                   "reconciled_strict": rs, "share_reconciled_strict": round(rs / w, 4) if w else None,
                                   "reconciled_direct_only": rd, "share_direct": round(rd / w, 4) if w else None}
    for y, rule, n in con.execute(f"""
            SELECT year(filed), recon_rule, count(*) FROM read_parquet('{rc}')
            WHERE multi_segment AND recon_rule IS NOT NULL GROUP BY 1, 2 ORDER BY 1, 3 DESC""").fetchall():
        res.setdefault("recon_rules", {}).setdefault(str(y), {})[rule] = n
    for y, cls, n in con.execute(f"""
            SELECT year(filed), recon_class, count(*) FROM read_parquet('{rc}')
            WHERE multi_segment AND n_revenue_members >= 1 GROUP BY 1, 2 ORDER BY 1, 3 DESC""").fetchall():
        res["recon_classes"].setdefault(str(y), {})[cls] = n
    cols = [i for i, *_ in ITEMS] + ["debt_mat_y2_5"]
    sel = ", ".join(f"round(avg(({c} IS NOT NULL)::INT), 4)" for c in cols)
    for r in con.execute(f"""SELECT year(filed), count(*), {sel} FROM read_parquet('{wide}')
                             WHERE form IN ('10-K', '10-KT') GROUP BY 1 ORDER BY 1""").fetchall():
        res["wide_nonnull_10k"][str(r[0])] = {"filings": r[1], **dict(zip(cols, r[2:], strict=True))}
    # segment features on every original 10-K / 10-KT with a registry fact (the recon frame)
    feat = (out / "segment_features.parquet").as_posix()
    fcols = [*FEATURE_COLS, "dd_sched_total"]
    sel = ", ".join(f"round(avg((x.{c} IS NOT NULL)::INT), 4)" for c in fcols)
    res["features_10k"] = {}
    for r in con.execute(f"""
            SELECT year(k.filed), count(*), round(avg((x.adsh IS NOT NULL)::INT), 4),
                   round(avg((x.bseg_rev_hhi IS NOT NULL)::INT) FILTER (WHERE k.multi_segment), 4), {sel}
            FROM read_parquet('{rc}') k LEFT JOIN read_parquet('{feat}') x USING (adsh)
            GROUP BY 1 ORDER BY 1""").fetchall():
        res["features_10k"][str(r[0])] = {"ten_k": r[1], "with_row": r[2], "bseg_rev_hhi_multi_segment": r[3],
                                          **dict(zip(fcols, r[4:], strict=True))}
    if member_ciks(con):
        res["segments_member"] = {}
        for y, m, w, rr in con.execute(f"""
                SELECT year(k.filed), count(*) FILTER (WHERE multi_segment),
                       count(*) FILTER (WHERE multi_segment AND n_revenue_members >= 1),
                       count(*) FILTER (WHERE multi_segment AND n_revenue_members >= 1 AND recon_class IN ({RECONCILED}))
                FROM read_parquet('{rc}') k SEMI JOIN memcik c ON c.cik = k.cik AND c.year = year(k.filed)
                GROUP BY 1 ORDER BY 1""").fetchall():
            res["segments_member"][str(y)] = {"multi_segment": m, "with_segment_revenue": w,
                                              "share_segment_revenue": round(w / m, 4) if m else None,
                                              "reconciled_1pct": rr, "share_reconciled": round(rr / w, 4) if w else None}
    return res


def _panel_parts(years: tuple[int, ...]) -> list[tuple[int, str]]:
    """(year, glob) of the panel stage's yearly files that exist."""
    root = C.build_root() / "panel"
    return [(y, (root / f"year={y}" / "*.parquet").as_posix()) for y in years if any((root / f"year={y}").glob("*.parquet"))]


def member_ciks(con, years: tuple[int, ...] = tuple(range(2019, 2027))) -> bool:
    """Temp table ``memcik (year, cik)``: the CIKs of panel ``member_equity`` cells that year (``identity/
    link_table_v3`` valid on the session, else the panel's own CIK); False when those inputs are absent."""
    lt = C.build_root() / "identity" / "link_table_v3.parquet"
    parts = _panel_parts(years)
    if not lt.exists() or not parts:
        return False
    union = " UNION ALL ".join(
        f"""SELECT {y} AS year, coalesce(l.cik, m.cik) AS cik
            FROM read_parquet('{g}', union_by_name = true, hive_partitioning = false) m
            LEFT JOIN read_parquet('{lt.as_posix()}') l ON l.security_id = m.security_id
             AND m.session_date BETWEEN l.valid_from AND coalesce(l.valid_to, DATE '2100-01-01') AND l.cik IS NOT NULL
            WHERE m.member_equity""" for y, g in parts)
    con.execute(f"CREATE OR REPLACE TEMP TABLE memcik AS SELECT DISTINCT year, cik FROM ({union}) WHERE cik IS NOT NULL")
    return True


UNIVERSE_PANELS = ("univ_all_common", "univ_r3000p")


def cell_coverage(con, out, years: tuple[int, ...] = GATE_YEARS) -> dict[str, Any]:
    """GC3 coverage per panel: ``member_equity`` always, ``univ_all_common`` / ``univ_r3000p`` once the Task 2
    ``universe`` stage is published (its manifest exists)."""
    res = {"member_equity": member_coverage(con, out, years)}
    if (C.build_root() / "universe" / "manifest.json").exists():
        for flag in UNIVERSE_PANELS:
            res[flag] = member_coverage(con, out, years, flag)
    else:
        res["universe"] = "not published (tier1-v4 Task 2): univ_all_common / univ_r3000p coverage pending"
    return res


def member_coverage(con, out, years: tuple[int, ...] = GATE_YEARS, panel: str = "member_equity") -> dict[str, Any]:
    """Coverage on month-end cells (last session of each month) of ``panel``: panel ``member_equity`` or a
    ``universe`` flag; the cell's CIK is ``identity/link_table_v3`` valid that day, else the cell row's own CIK. The
    cell sees, among annual ``segment_features`` rows with ``available_at`` < 22:00 UTC of the month-end session and
    ``period`` >= session - :data:`STALE_DAYS` days, the latest period (then the latest vintage)."""
    root = C.build_root()
    lt, cal = root / "identity" / "link_table_v3.parquet", C.calendar_path()
    if panel == "member_equity":
        parts = _panel_parts(years)
        sel = ("SELECT session_date, security_id, cik AS cik0 FROM read_parquet('{p}', union_by_name = true, "
               "hive_partitioning = false) WHERE member_equity")
    else:
        parts = [(y, (root / "universe" / f"year={y}" / "universe.parquet").as_posix()) for y in years]
        parts = [(y, g) for y, g in parts if Path(g).exists()]
        sel = "SELECT session_date, security_id, cik AS cik0 FROM read_parquet('{p}') WHERE " + panel
    if not lt.exists() or not cal.exists() or not parts:
        return {"note": f"inputs absent for {panel} (identity/link_table_v3, calendar, cells)"}
    feat = (out / "segment_features.parquet").as_posix()
    cols = ("bseg_n", "bseg_rev_hhi", "foreign_sales_share", "major_cust_share", "dd1", "oplease_liab", "stkco")
    union = " UNION ALL ".join(sel.format(p=g) for _, g in parts)
    con.execute(f"""
        CREATE OR REPLACE TEMP TABLE mcells AS
        WITH cal AS (SELECT session_date, lag(session_date) OVER (ORDER BY session_date) AS prev
                     FROM read_parquet('{cal.as_posix()}')),
             me AS (SELECT max(session_date) AS d FROM cal GROUP BY date_trunc('month', session_date))
        SELECT m.session_date, m.security_id, coalesce(l.cik, m.cik0) AS cik,
               CAST(c.session_date AS TIMESTAMP) + INTERVAL {C.MARK_HOUR_UTC} HOUR AS cutoff
        FROM ({union}) m JOIN me ON me.d = m.session_date JOIN cal c ON c.session_date = m.session_date
        LEFT JOIN read_parquet('{lt.as_posix()}') l ON l.security_id = m.security_id
             AND m.session_date BETWEEN l.valid_from AND coalesce(l.valid_to, DATE '2100-01-01') AND l.cik IS NOT NULL
        QUALIFY row_number() OVER (PARTITION BY m.session_date, m.security_id
                                   ORDER BY l.is_issuer_primary DESC NULLS LAST, l.cik) = 1""")
    con.execute(f"""
        CREATE OR REPLACE TEMP TABLE mfeat AS
        SELECT cik, CAST(available_at AS TIMESTAMP) AS ts, period, {', '.join(cols)}
        FROM read_parquet('{feat}') WHERE is_annual""")
    agg = ", ".join(f"round(avg((fresh AND {c} IS NOT NULL)::INT), 4)" for c in cols)
    res: dict[str, Any] = {"basis": f"{panel} month-end cells; among annual rows visible at the cell "
                                    f"(available_at < month-end session {C.MARK_HOUR_UTC}:00 UTC) with period within "
                                    f"{STALE_DAYS} days, the latest period, then the latest vintage", "by_year": {}}
    for r in con.execute(f"""
            SELECT year(session_date), count(*), round(avg((cik IS NOT NULL)::INT), 4), round(avg(fresh::INT), 4), {agg}
            FROM (SELECT c.session_date, c.security_id, c.cik, f.period IS NOT NULL AS fresh, f.* EXCLUDE (cik, ts, period)
                  FROM mcells c LEFT JOIN mfeat f ON f.cik = c.cik AND f.ts < c.cutoff
                       AND f.period >= CAST(c.session_date - INTERVAL {STALE_DAYS} DAY AS DATE)
                  QUALIFY row_number() OVER (PARTITION BY c.session_date, c.security_id ORDER BY f.period DESC, f.ts DESC) = 1)
            GROUP BY 1 ORDER BY 1""").fetchall():
        res["by_year"][str(r[0])] = {"cells": r[1], "linked": r[2], "annual_row": r[3], **dict(zip(cols, r[4:], strict=True))}
    return res


def gate_status(cov: dict[str, Any], years: tuple[int, ...] = GATE_YEARS) -> dict[str, Any]:
    """GC9 gate status of S4.4 (gap-analysis item 12) per filing year of the score window, all 10-K filers."""
    seg = cov.get("segments", {})
    out: dict[str, Any] = {}
    for name, key, thr in (("segment_revenue_multi_segment_10k", "share_segment_revenue", GATE_SEGMENT_REVENUE),
                           ("segment_sum_within_1pct", "share_reconciled", GATE_RECONCILED)):
        measured = {str(y): seg.get(str(y), {}).get(key) for y in years}
        fails = [y for y, v in measured.items() if v is None or v < thr]
        out[name] = {"threshold": thr, "measured": measured, "fails": fails, "pass": not fails}
        if name == "segment_revenue_multi_segment_10k":
            out[name]["observable_threshold_pass"] = not fails
            out[name]["denominator_certified"] = cov.get("denominator_certified", False)
            out[name]["pass"] = not fails and out[name]["denominator_certified"]
            if not out[name]["denominator_certified"]:
                out[name]["verification_gap"] = "Independent all-filer multi-segment disclosure denominator unavailable"
    out["pass"] = all(v["pass"] for v in out.values() if isinstance(v, dict))
    return out


def remeasure() -> dict[str, Any]:
    """Recompute ``coverage.json`` and the manifest's ``gate_status`` / coverage from the published files (no
    rebuild): run once the Task 2 ``universe`` stage is published to add its panels."""
    import datetime as dt

    out = C.stage_dir(STAGE)
    man = C.read_json(out / "manifest.json")
    con = C.connect(memory=DUCKDB_MEMORY, threads=THREADS)
    cov = coverage(con, out)
    cov["cells"] = cell_coverage(con, out)
    cov["gate_status"] = gate_status(cov)
    con.close()
    C.write_json_atomic(out / "coverage.json", cov)
    man["gate_status"] = cov["gate_status"]
    man["receipt"]["coverage"], man["receipt"]["gate_status"] = cov, cov["gate_status"]
    man["remeasured_at"] = dt.datetime.now(dt.UTC).isoformat(timespec="seconds")
    C.write_json_atomic(out / "manifest.json", man)
    return cov


def _drop_scratch(db_file: str) -> None:
    for p in (C.build_root() / "_tmp" / db_file, C.build_root() / "_tmp" / (db_file + ".wal")):
        if p.exists():
            p.unlink()


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    b = sub.add_parser("build")
    b.add_argument("--years", help="e.g. 2019-2026 (default: every filing year)")
    sub.add_parser("coverage", help="re-measure coverage and gates from the published files")
    args = ap.parse_args(argv)
    if args.cmd == "coverage":
        cov = remeasure()
        print(json.dumps({"gate_status": cov["gate_status"], "segments": cov["segments"]}, default=str), flush=True)
        return 0
    years = None
    if args.years:
        a, _, z = args.years.partition("-")
        years = list(range(int(a), int(z or a) + 1))
    rec = build(years)
    print(json.dumps({k: v for k, v in rec.items() if k != "coverage"}, default=str)[:4000], flush=True)
    print(json.dumps(rec.get("coverage", {}).get("segments"), default=str), flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
