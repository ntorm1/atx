"""Tier1-S2: every Tier-1 spec item carries a deep alias set, or an explicit reason."""
from __future__ import annotations

from collections import Counter

from atx_db.item_registry import read_fundamental_item_seed
from atx_db.standardization import default_standardization_rules
from atx_db.statement_map_seed import read_statement_map_seed

MINIMUM_ALIASES = 3

# Wave A-1 income statement + cash flow, Wave A-2 balance sheet. Task 8 adds
# the industry-overlay ids.
SPEC_ITEM_IDS = frozenset(
    {
        # Wave A-1 income statement + cash flow
        1001, 1003, 1004, 1005, 1008, 1011, 1014, 1016, 1018, 1021, 1022, 1023,
        1024, 1027, 1029, 1030, 1031, 1032, 1033, 1034, 1035, 1040, 1041, 1051,
        1301, 1303, 1304, 1305, 1307, 1308, 1309, 1311, 1312, 1313, 1314, 1316,
        1318, 1322, 1324, 1327,
        # Wave A-2 balance sheet
        1039, 1101, 1102, 1104, 1105, 1106, 1107, 1109, 1110, 1111, 1112, 1114,
        1115, 1117, 1119, 1120, 1201, 1202, 1203, 1205, 1207, 1211, 1212, 1213,
        1214, 1217, 1219, 1220, 1221, 1225, 1226, 1227,
    }
)

# item_id -> (exact alias count, why us-gaap offers no more).
# An exact count, not a floor, so a silent regression fails the test.
ALIAS_DEPTH_EXCEPTIONS: dict[int, tuple[int, str]] = {
    1004: (1, "GrossProfit is the only us-gaap gross-profit element; the rest is composition from 1001-1003."),
    1021: (1, "NonoperatingIncomeExpense is the only us-gaap total-nonoperating-income-or-expense element; the components (interest, investment income) belong to items 1017-1020."),
    1040: (2, "WeightedAverageNumberOfSharesOutstandingBasic and WeightedAverageNumberOfSharesIssuedBasic are the only two us-gaap basic weighted-average-share elements; the rest of the item's rows are vendor field mappings, not us-gaap aliases."),
    1005: (2, "us-gaap splits SG&A into SellingGeneralAndAdministrativeExpense and OtherSellingGeneralAndAdministrativeExpense; GeneralAndAdministrativeExpense belongs to item 1007."),
    1014: (1, "OperatingIncomeLoss is the only us-gaap operating-income element."),
    1016: (0, "EBITDA has no us-gaap element; composed from operating_income + cf_depreciation."),
    1023: (2, "the two us-gaap pretax elements; the Domestic/Foreign splits are components, not totals."),
    1024: (2, "IncomeTaxExpenseBenefit and its continuing-operations variant; Current/Deferred belong to 1025/1026."),
    1029: (2, "IncomeLossFromContinuingOperations and its including-NCI variant; ProfitLoss belongs to 1031."),
    1031: (2, "NetIncomeLoss and ProfitLoss; NetIncomeLossAvailableToCommonStockholdersBasic belongs to 1032."),
    1032: (2, "the basic and diluted available-to-common elements; the rest is composition from 1031-1033."),
    1034: (2, "EarningsPerShareBasic and EarningsPerShareBasicAndDiluted; the continuing-ops per-share element belongs to 1036."),
    1035: (1, "EarningsPerShareDiluted is the only diluted-EPS element not already owned by 1034 or 1037."),
    1041: (1, "WeightedAverageNumberOfDilutedSharesOutstanding is the only diluted weighted-average element."),
    1051: (2, "ExtraordinaryItemNetOfTax and ExtraordinaryItemGross; ASU 2015-01 eliminated the category, so nothing newer exists."),
    1301: (1, "NetCashProvidedByUsedInOperatingActivities; the continuing-operations element is item 1302, reached by the coalesce_or_sum fallback."),
    1303: (2, "the total and continuing-operations investing elements."),
    1304: (2, "the total and continuing-operations financing elements."),
    1307: (2, "DepreciationDepletionAndAmortization and DepreciationAmortizationAndAccretionNet; DepreciationAndAmortization belongs to item 1011."),
    1308: (2, "ShareBasedCompensation and AllocatedShareBasedCompensationExpense."),
    1316: (1, "PaymentsOfDividendsCommonStock is the only common-dividend payment element."),
    1318: (1, "PaymentsOfDividends; the rest is composition from 1316-1317."),
    1322: (0, "no us-gaap working-capital-change total; composed from 1319+1320+1321."),
    1327: (1, "DeferredIncomeTaxesAndTaxCredits is the only cash-flow deferred-tax element; the income-statement element belongs to 1026."),
    1101: (1, "Assets is the only us-gaap total-assets element."),
    1102: (1, "AssetsCurrent is the only us-gaap current-assets element."),
    1109: (1, "OtherAssetsCurrent is the only us-gaap other-current-assets element."),
    1110: (2, "PropertyPlantAndEquipmentNet plus the finance-lease-ROU-inclusive variant."),
    1111: (1, "PropertyPlantAndEquipmentGross is the only us-gaap gross PP&E element."),
    1112: (1, "AccumulatedDepreciationDepletionAndAmortizationPropertyPlantAndEquipment is the only accumulated-depreciation element."),
    1114: (2, "Goodwill and GoodwillGross; the goodwill-plus-intangibles total belongs to 1113."),
    1119: (2, "OtherAssetsNoncurrent and OtherAssets."),
    1120: (2, "CapitalizedComputerSoftwareNet and CapitalizedComputerSoftwareGross; the accumulated-amortization element is a contra account, not the asset."),
    1201: (1, "Liabilities; the rest is composition from total_liab_equity - equity_incl_NCI."),
    1202: (1, "LiabilitiesCurrent is the only us-gaap current-liabilities element."),
    1211: (2, "DeferredIncomeTaxLiabilitiesNet and DeferredTaxLiabilitiesNoncurrent."),
    1212: (2, "OtherLiabilitiesNoncurrent and OtherLiabilities."),
    1213: (1, "MinorityInterest is the only balance-sheet noncontrolling-interest element; the including-NCI equity total belongs to 1222."),
    1214: (2, "PreferredStockValue and PreferredStockLiquidationPreferenceValue."),
    1217: (1, "RetainedEarningsAccumulatedDeficit is the only retained-earnings element."),
    1219: (2, "TreasuryStockValue and TreasuryStockCommonValue."),
    1220: (0, "no us-gaap common-equity element; composed from stockholders_equity - preferred_stock."),
    1221: (1, "StockholdersEquity; the including-NCI element belongs to 1222 and is reached by the coalesce_or_difference fallback."),
    1226: (2, "OtherLiabilitiesCurrent and OtherAccruedLiabilitiesCurrent."),
}


def _alias_counts() -> Counter[int]:
    counts: Counter[int] = Counter()
    for row in read_fundamental_item_seed():
        if row.alias_scheme and row.alias_code:
            counts[row.item_id] += 1
    return counts


def test_every_spec_item_has_a_deep_alias_set_or_a_stated_reason():
    counts = _alias_counts()
    shallow = {
        item_id: counts.get(item_id, 0)
        for item_id in sorted(SPEC_ITEM_IDS)
        if counts.get(item_id, 0) < MINIMUM_ALIASES and item_id not in ALIAS_DEPTH_EXCEPTIONS
    }
    assert shallow == {}


def test_exception_counts_are_exact():
    counts = _alias_counts()
    drifted = {
        item_id: (counts.get(item_id, 0), expected)
        for item_id, (expected, _reason) in sorted(ALIAS_DEPTH_EXCEPTIONS.items())
        if counts.get(item_id, 0) != expected
    }
    assert drifted == {}


def test_every_exception_carries_a_reason():
    assert all(reason.strip() for _count, reason in ALIAS_DEPTH_EXCEPTIONS.values())


def test_every_exception_id_is_a_spec_item():
    assert set(ALIAS_DEPTH_EXCEPTIONS) <= set(SPEC_ITEM_IDS)


def test_zero_alias_spec_items_have_a_composition_rule():
    """An item with no alias must be reachable by source_item_ids composition."""
    counts = _alias_counts()
    rules_by_item: dict[int, set[str]] = {}
    for rule in default_standardization_rules():
        if rule.is_active and rule.source_item_ids:
            rules_by_item.setdefault(rule.item_id, set()).add(rule.combination_rule)
    orphans = sorted(
        item_id
        for item_id in SPEC_ITEM_IDS
        if counts.get(item_id, 0) == 0 and item_id not in rules_by_item
    )
    assert orphans == []


def test_every_registry_alias_has_a_statement_map_row():
    """An alias that is not in the statement map is never ingested."""
    mapped = {(row.taxonomy, row.concept) for row in read_statement_map_seed()}
    missing = sorted(
        f"{row.alias_scheme}:{row.alias_code}"
        for row in read_fundamental_item_seed()
        if row.alias_scheme in {"us-gaap", "dei"}
        and row.alias_code
        and (row.alias_scheme, row.alias_code) not in mapped
    )
    assert missing == []


def test_every_statement_map_alias_appears_in_its_rule_alias_json():
    """The rule JSON is the reviewable record of what each item accepts."""
    by_item: dict[int, set[tuple[str, str]]] = {}
    for row in read_statement_map_seed():
        if row.item_id is None or not row.is_active or row.is_derived:
            continue
        if row.taxonomy not in {"us-gaap", "dei"} or row.concept.startswith("__"):
            continue
        by_item.setdefault(int(row.item_id), set()).add((row.taxonomy, row.concept))

    declared: dict[int, set[tuple[str, str]]] = {}
    for rule in default_standardization_rules():
        if not rule.is_active:
            continue
        declared.setdefault(rule.item_id, set()).update(
            (alias.alias_scheme, alias.alias_code) for alias in rule.source_aliases
        )

    missing = {
        item_id: sorted(f"{s}:{c}" for s, c in aliases - declared.get(item_id, set()))
        for item_id, aliases in sorted(by_item.items())
        if item_id in SPEC_ITEM_IDS and aliases - declared.get(item_id, set())
    }
    assert missing == {}
