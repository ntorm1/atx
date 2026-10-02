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
"""

from __future__ import annotations

import argparse
import json
import shutil
import sys
import time
from typing import Any

from . import common as C
from . import notes_fetch as N

STAGE = "fundamentals_notes"
SCHEMA = "atx.alpha-panel.fundamentals-notes/v1"
CLOCK_RULE = "notes-accepted-v1"
FC1_HOURS = 46
DUCKDB_MEMORY = "300MB"                   # under a 0.6 GiB guard cap; tables live in a scratch db file
RECON_TOL = 0.01
MODULES = ("fund_notes", "notes_fetch", "common")

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
AXES = {"business": "BusinessSegments", "geographic": "Geographical"}
#: business-segment members that are not a reportable segment (reconciling lines, totals)
RECONCILING_RE = r"(?i)elimina|corporate|reconcil|unallocat|intersegment|consolidat|^total|segmenttotal|adjustment|nonsegment"
RECON_CONSOLIDATION_RE = r"(?i)elimina|reconcil|corporate|nonsegment|unallocat|other"
PENSION_FALLBACK = "RetirementPlanType=PensionPlansDefinedBenefit;"
SEGMENTS_ONLY_RE = r"^(BusinessSegments|Geographical)=[^;]*;(ConsolidationItems=[^;]*;)?$|^ConsolidationItems=[^;]*;(BusinessSegments|Geographical)=[^;]*;$"


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


def _registry_rows() -> list[tuple[str, str, str, str, int]]:
    rows = [(tag, item, grp, kind, i) for item, grp, kind, chain in ITEMS for i, tag in enumerate(chain)]
    rows += [(tag, m, "segment", "segment", i) for m, chain in SEGMENT_METRICS.items() for i, tag in enumerate(chain)]
    return rows


def build(years: list[int] | None = None) -> dict[str, Any]:
    t0 = time.perf_counter()
    receipt: dict[str, Any] = {"clock_rule": CLOCK_RULE}
    parts = N.parts_dir()
    nman = C.stage_dir(N.STAGE) / "manifest.json"
    if not nman.exists():
        raise RuntimeError("notes stage has no manifest: run notes_fetch finalize first")
    con = C.connect(memory=DUCKDB_MEMORY, threads=2, db_file="fundamentals_notes.duckdb")
    g_sub = (parts / "source=*" / "sub.parquet").as_posix()
    keys = [p.name.split("=", 1)[1] for p in parts.glob("source=*")]
    con.execute("CREATE TABLE src (source VARCHAR, ord INTEGER)")
    con.executemany("INSERT INTO src VALUES (?, ?)", [(k, i) for i, k in enumerate(sorted(keys, key=N.order_key))])
    con.execute("CREATE TABLE reg (tag VARCHAR, item VARCHAR, item_group VARCHAR, kind VARCHAR, prio INTEGER)")
    con.executemany("INSERT INTO reg VALUES (?, ?, ?, ?, ?)", _registry_rows())
    per = ", ".join(f"'{f}'" for f in N.PERIODIC_FORMS)
    con.execute(f"""
        CREATE TABLE sub AS
        SELECT s.adsh, s.cik, s.form, s.period, s.fy, s.fp, s.filed, s.source,
               coalesce(s.accepted_utc, CAST(s.filed AS TIMESTAMP) AT TIME ZONE 'UTC' + INTERVAL {FC1_HOURS} HOUR) AS available_at,
               CASE WHEN s.accepted_utc IS NULL THEN 'filed_plus_46h' ELSE 'accepted_utc' END AS clock_basis
        FROM read_parquet('{g_sub}', hive_partitioning = false) s JOIN src USING (source)
        WHERE s.form IN ({per})
        QUALIFY row_number() OVER (PARTITION BY s.adsh ORDER BY src.ord) = 1""")
    out = C.stage_dir(STAGE)
    all_years = [r[0] for r in con.execute("SELECT DISTINCT year(filed) FROM sub ORDER BY 1").fetchall()]
    receipt["items_rows"] = {}
    ax_b, ax_g = AXES["business"], AXES["geographic"]
    con.execute("""CREATE TABLE regt AS SELECT tag, first(item ORDER BY prio, item) AS item,
                          first(item_group ORDER BY prio, item) AS item_group FROM reg GROUP BY tag""")
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
                WHERE year(s.filed) = {y} AND x.version <> x.adsh AND x.value IS NOT NULL
                  AND x.tag IN (SELECT tag FROM regt)
                QUALIFY row_number() OVER (PARTITION BY x.adsh, x.tag, x.version, x.ddate, x.qtrs, x.uom, x.dimh,
                                                        coalesce(x.coreg, '') ORDER BY x.iprx) = 1""")
            # 2) the dimension strings those facts reference
            con.execute(f"""
                CREATE OR REPLACE TABLE dimy AS
                SELECT d.dimhash, d.segments, d.source FROM read_parquet({g_dim}, hive_partitioning = false) d
                SEMI JOIN (SELECT DISTINCT dimh, source FROM ny) u ON u.dimh = d.dimhash AND u.source = d.source""")
            sql = f"""
                SELECT s.cik, n.adsh, s.form, s.period, s.fy, s.fp, s.filed, s.available_at, s.clock_basis,
                       r.item_group, r.item, n.tag, n.segments, n.dimn,
                       CASE WHEN n.segments LIKE '%{ax_b}=%' THEN 'business' WHEN n.segments LIKE '%{ax_g}=%' THEN 'geographic' END AS axis_kind,
                       coalesce(nullif(regexp_extract(n.segments, '(^|;){ax_b}=([^;]*)', 2), ''),
                                nullif(regexp_extract(n.segments, '(^|;){ax_g}=([^;]*)', 2), '')) AS member,
                       nullif(regexp_extract(n.segments, '(^|;)ConsolidationItems=([^;]*)', 2), '') AS consolidation_member,
                       n.ddate, CAST(n.ddate - CAST(round(coalesce(n.datp, 0)) AS INTEGER) AS DATE) AS end_date,
                       n.qtrs, n.value, n.uom, n.coreg, n.source
                FROM (SELECT x.*, d.segments FROM ny x LEFT JOIN dimy d ON d.dimhash = x.dimh AND d.source = x.source) n
                JOIN sub s ON s.adsh = n.adsh AND s.source = n.source
                JOIN regt r ON r.tag = n.tag
                WHERE r.item_group <> 'segment' OR n.segments IS NULL OR n.segments LIKE '%{ax_b}=%'
                      OR n.segments LIKE '%{ax_g}=%' OR n.segments LIKE '%ConsolidationItems=%'
                ORDER BY s.cik, n.adsh, r.item_group, r.item, n.tag, n.ddate, n.qtrs, n.segments NULLS FIRST, n.uom,
                         n.coreg NULLS FIRST, n.value"""
            receipt["items_rows"][str(y)] = C.copy_to_parquet(con, sql, out / "items" / f"year={y}" / "items.parquet", 65536)
        print(f"fund_notes items {y}: {receipt['items_rows'][str(y)]:,}", flush=True)
    # segments, reconciliation and wide rows per filing year (each filing's facts live in its year's items file),
    # stitched in year order: bounded tables under the guard
    tmp = C.build_root() / "_tmp" / "fundamentals_notes_parts"
    if tmp.exists():
        shutil.rmtree(tmp)
    done_years = sorted(int(d.name.split("=", 1)[1]) for d in (out / "items").glob("year=*"))
    for kind in ("segments", "recon", "wide"):
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
            C.copy_to_parquet(con, f"""
                SELECT i.cik, i.adsh, i.form, i.period, i.fy, i.fp, i.filed, i.available_at, i.clock_basis,
                       i.item AS metric, i.axis_kind, i.member, i.consolidation_member, i.tag, i.segments, i.value, i.uom,
                       i.ddate, i.end_date, i.qtrs, i.source
                FROM read_parquet('{items}') i
                WHERE i.item_group = 'segment' AND i.coreg IS NULL AND i.segments IS NOT NULL
                  AND regexp_matches(i.segments, '{SEGMENTS_ONLY_RE}')
                ORDER BY i.cik, i.adsh, metric, i.axis_kind, i.member, i.tag, i.ddate, i.qtrs, i.segments, i.uom, i.value""", seg)
            C.copy_to_parquet(con, recon_sql(items, seg.as_posix()), tmp / "recon" / f"{y}.parquet")
            C.copy_to_parquet(con, wide_sql(items), tmp / "wide" / f"{y}.parquet", 32768)
    with C.timed(receipt, "stitch"):
        receipt["segments_rows"] = N.stitch_parquet([tmp / "segments" / f"{y}.parquet" for y in done_years], out / "segments.parquet")
        receipt["recon_rows"] = N.stitch_parquet([tmp / "recon" / f"{y}.parquet" for y in done_years], out / "segment_recon.parquet")
        receipt["wide_rows"] = N.stitch_parquet([tmp / "wide" / f"{y}.parquet" for y in done_years], out / "notes_wide.parquet", 32768)
    shutil.rmtree(tmp, ignore_errors=True)
    with C.timed(receipt, "coverage"):
        receipt["coverage"] = coverage(con, out)
    C.write_json_atomic(out / "coverage.json", receipt["coverage"])
    receipt["timings_s"]["total"] = round(time.perf_counter() - t0, 1)
    payload = {
        "clock_rule": CLOCK_RULE,
        "clock_text": ("available_at = SUB accepted (EDGAR America/New_York wall clock, DST-aware) in UTC; else filed "
                       f"00:00 UTC + {FC1_HOURS} h. Amendments are separate accessions with their own clock."),
        "input_manifests_sha256": {"notes": C.sha256_file(nman)},
        "items": {i: {"group": g, "kind": k, "chain": list(ch)} for i, g, k, ch in ITEMS},
        "segment_metrics": {k: list(v) for k, v in SEGMENT_METRICS.items()},
        "rules": {"reconciling_member_re": RECONCILING_RE, "recon_consolidation_re": RECON_CONSOLIDATION_RE,
                  "recon_tolerance": RECON_TOL, "segments_only_re": SEGMENTS_ONLY_RE, "pension_fallback": PENSION_FALLBACK},
        "receipt": receipt,
    }
    C.write_stage_manifest(STAGE, SCHEMA, MODULES, payload)
    con.close()
    _drop_scratch("fundamentals_notes.duckdb")
    return receipt


def recon_sql(items: str, seg: str) -> str:
    """Business-segment revenue reconciliation per original 10-K / 10-KT (current fiscal-year context)."""
    chain = SEGMENT_METRICS["revenue"]
    prio = " ".join(f"WHEN '{t}' THEN {i}" for i, t in enumerate(chain))
    return f"""
        WITH k AS (
            SELECT DISTINCT i.cik, i.adsh, i.form, i.period, i.fy, i.fp, i.filed, i.available_at, i.clock_basis, c.currency
            FROM read_parquet('{items}', hive_partitioning = false) i LEFT JOIN cur c USING (adsh)
            WHERE i.form IN ('10-K', '10-KT')
        ),
        mem AS (          -- business-segment members of the filing (any fact), reportable-looking only
            SELECT adsh, count(DISTINCT member) AS n_members_all,
                   count(DISTINCT member) FILTER (WHERE NOT regexp_matches(member, '{RECONCILING_RE}')) AS n_members
            FROM read_parquet('{items}', hive_partitioning = false)
            WHERE axis_kind = 'business' AND member IS NOT NULL AND coreg IS NULL GROUP BY 1
        ),
        rv AS (           -- one value per (filing, tag, member): segment-only context preferred over OperatingSegments
            SELECT s.adsh, s.tag, s.member, s.uom,
                   arg_min(s.value, CASE WHEN s.consolidation_member IS NULL THEN 0 ELSE 1 END) AS value
            FROM read_parquet('{seg}') s JOIN k USING (adsh)
            WHERE s.metric = 'revenue' AND s.axis_kind = 'business' AND s.qtrs = 4 AND s.ddate = k.period
              AND s.uom = k.currency AND (s.consolidation_member IS NULL OR s.consolidation_member = 'OperatingSegments')
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
                   CASE s.tag {prio} ELSE 99 END AS prio
            FROM sums s LEFT JOIN tot t USING (adsh, tag) LEFT JOIN rec r USING (adsh, tag)
        ),
        best AS (         -- the chain tag whose total exists, else the first tag with segment rows
            SELECT * FROM cand QUALIFY row_number() OVER (PARTITION BY adsh ORDER BY own_total IS NULL, prio) = 1
        ),
        anytot AS (
            SELECT adsh, arg_min(total, CASE tag {prio} ELSE 99 END) AS total, arg_min(tag, CASE tag {prio} ELSE 99 END) AS tag
            FROM tot GROUP BY 1
        )
        SELECT k.cik, k.adsh, k.form, k.period, k.fy, k.fp, k.filed, k.available_at, k.clock_basis, k.currency,
               coalesce(m.n_members, 0) AS n_members, coalesce(m.n_members_all, 0) AS n_members_all,
               coalesce(m.n_members, 0) >= 2 AS multi_segment,
               b.tag AS revenue_tag, coalesce(b.n_rev_members, 0) AS n_revenue_members, b.seg_sum,
               b.recon AS reconciling_items, coalesce(b.own_total, a.total) AS consolidated_revenue,
               CASE WHEN b.own_total IS NOT NULL THEN b.tag ELSE a.tag END AS consolidated_tag,
               (b.seg_sum - coalesce(b.own_total, a.total)) / nullif(abs(coalesce(b.own_total, a.total)), 0) AS rel_diff,
               {recon_class_sql('b.seg_sum', 'coalesce(b.own_total, a.total)', 'b.recon', 'coalesce(b.n_rev_members, 0)')} AS recon_class
        FROM k LEFT JOIN mem m USING (adsh) LEFT JOIN best b USING (adsh) LEFT JOIN anytot a USING (adsh)
        ORDER BY k.filed, k.cik, k.adsh"""


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


def coverage(con, out) -> dict[str, Any]:
    """S4.4 done tests per filing year: segment revenue for multi-segment 10-K filers and the reconciliation rate."""
    rc = (out / "segment_recon.parquet").as_posix()
    wide = (out / "notes_wide.parquet").as_posix()
    res: dict[str, Any] = {"segments": {}, "recon_classes": {}, "wide_nonnull_10k": {}}
    rows = con.execute(f"""
        SELECT year(filed) AS y, count(*) AS n10k, count(*) FILTER (WHERE multi_segment) AS multi,
               count(*) FILTER (WHERE multi_segment AND n_revenue_members >= 1) AS with_rev,
               count(*) FILTER (WHERE multi_segment AND n_revenue_members >= 1 AND recon_class IN ('direct', 'with_reconciling')) AS rec,
               count(*) FILTER (WHERE multi_segment AND n_revenue_members >= 1 AND recon_class = 'direct') AS rec_direct
        FROM read_parquet('{rc}') GROUP BY 1 ORDER BY 1""").fetchall()
    for y, n, m, w, r, rd in rows:
        res["segments"][str(y)] = {"ten_k": n, "multi_segment": m, "with_segment_revenue": w,
                                   "share_segment_revenue": round(w / m, 4) if m else None,
                                   "reconciled_1pct": r, "share_reconciled": round(r / w, 4) if w else None,
                                   "reconciled_direct_only": rd, "share_direct": round(rd / w, 4) if w else None}
    for y, cls, n in con.execute(f"""
            SELECT year(filed), recon_class, count(*) FROM read_parquet('{rc}')
            WHERE multi_segment AND n_revenue_members >= 1 GROUP BY 1, 2 ORDER BY 1, 3 DESC""").fetchall():
        res["recon_classes"].setdefault(str(y), {})[cls] = n
    cols = [i for i, *_ in ITEMS] + ["debt_mat_y2_5"]
    sel = ", ".join(f"round(avg(({c} IS NOT NULL)::INT), 4)" for c in cols)
    for r in con.execute(f"""SELECT year(filed), count(*), {sel} FROM read_parquet('{wide}')
                             WHERE form IN ('10-K', '10-KT') GROUP BY 1 ORDER BY 1""").fetchall():
        res["wide_nonnull_10k"][str(r[0])] = {"filings": r[1], **dict(zip(cols, r[2:], strict=True))}
    return res


def _drop_scratch(db_file: str) -> None:
    for p in (C.build_root() / "_tmp" / db_file, C.build_root() / "_tmp" / (db_file + ".wal")):
        if p.exists():
            p.unlink()


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    b = sub.add_parser("build")
    b.add_argument("--years", help="e.g. 2019-2026 (default: every filing year)")
    args = ap.parse_args(argv)
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
