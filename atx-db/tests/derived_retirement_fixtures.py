"""Independent, realistic facts and genuine pre-retirement baseline support.

There are no imports of retired modules at import time. Only the explicitly
invoked baseline recorder loads them. Normal parity tests read frozen evidence.
All money is USD. Annual flows are sums of four independently varying quarters;
balances change every quarter. Native and standardized aliases carry identical
facts, including period start, availability, and amendment identity.
"""

from __future__ import annotations

import calendar
import datetime as dt
import gzip
import hashlib
import importlib
import json
from dataclasses import asdict
from pathlib import Path

import pandas as pd

BASELINE_COMMIT = "5b11a272"
DATA_DIR = Path(__file__).parent / "data"
SOURCE = "derived-retirement-fixture-v1"
SECURITY_COUNT = 28
YEARS = tuple(range(2016, 2024))
KEY_COLUMNS = ["factor_id", "security_id", "as_of_date"]
EVIDENCE_COLUMNS = [*KEY_COLUMNS, "raw_value", "value", "available_at"]

# Explicit independent contract; never derived from the replacement seed.
MODULE_FACTORS = {
    "altman_distress": ("distress_altman_z_score",),
    "net_operating_assets": ("quality_net_operating_assets",),
    "quarterly_working_capital_accruals": ("quality_low_quarterly_operating_working_capital_accruals",),
    "asset_turnover_change": ("efficiency_annual_asset_turnover_change",),
    "annual_margin_change": (
        "profitability_annual_net_margin_change", "profitability_annual_operating_margin_change",
        "profitability_annual_gross_margin_change",
    ),
    "quarterly_gross_margin_change": ("profitability_quarterly_gross_margin_change_yoy",),
    "quarterly_profitability_change": ("profitability_quarterly_operating_profitability_change_yoy",),
    "net_issuance": ("financing_low_net_share_issuance",),
    "net_payout": ("financing_net_payout_yield",),
    "enterprise_yield": (
        "valuation_enterprise_yield_ebit", "valuation_gross_profit_enterprise_yield",
        "valuation_operating_cash_flow_enterprise_yield", "valuation_enterprise_yield_sales",
    ),
    "beneish_m_score": ("quality_low_beneish_m_score",),
    "rsst_accruals": ("quality_low_rsst_accruals",),
    "external_financing": ("financing_low_external_financing",),
    "net_debt_financing": ("financing_low_net_debt_financing",),
    "rd_intensity": ("valuation_rd_to_market_equity",),
    "rd_increase": ("intangibles_large_rd_increase",),
    "tax_expense_momentum": ("earnings_tax_expense_momentum",),
    "tax_to_book_income": ("earnings_tax_to_book_income",),
}
FACTOR_IDS = tuple(sorted(factor for factors in MODULE_FACTORS.values() for factor in factors))
RETAINED_PARENTS = (
    "asset_growth", "fundamental_signals", "cash_flow_profitability",
    "quarterly_operating_profitability", "quarterly_cash_profitability", "quarterly_revenue_growth",
)
ALIASES = {
    "ar": "accounts_receivable", "ap": "accounts_payable", "cash_st_inv": "cash_and_st_investments",
    "st_debt": "short_term_debt", "lt_debt": "long_term_debt", "ppe_net": "pp_and_e_net",
    "pref_stock": "preferred_stock", "minority_int_bs": "minority_interest_bs",
    "cogs": "cost_of_revenue_cogs", "gross_profit": "gross_profit__1004", "sga": "sg_and_a",
    "rd_expense": "r_and_d_expense", "interest_expense": "interest_expense_total",
    "net_income": "net_income_total", "income_tax": "income_tax_total",
    "operating_cash_flow": "cash_flow_from_operations", "financing_cash_flow": "cash_flow_from_financing",
    "da_cf": "d_and_a_cash_flow", "share_repurchases": "stock_repurchases_buybacks",
    "common_div_paid": "common_dividends_paid",
}


def canonical_bytes(value) -> bytes:
    """UTF-8, sorted object keys, compact separators, ISO dates, finite numbers."""
    return json.dumps(value, sort_keys=True, separators=(",", ":"), allow_nan=False,
                      default=lambda item: item.isoformat()).encode("utf-8")


def digest(value) -> str:
    return hashlib.sha256(canonical_bytes(value)).hexdigest()


def _quarter_end(year, quarter):
    month = quarter * 3
    return dt.date(year, month, calendar.monthrange(year, month)[1])


def _values(i, year, quarter):
    """Distinct security, year, seasonal, and interaction effects per component."""
    y = year - 2016
    t = y * 4 + quarter
    assets = 420_000_000 + 19_000_000 * i + (4_000_000 + 95_000 * i) * t + 80_000 * t * t
    revenue = 76_000_000 + 2_300_000 * i + 2_100_000 * t + 190_000 * ((i + 2 * quarter) % 7) * y
    revenue *= (0.91, 1.04, 0.97, 1.16)[quarter - 1]
    cogs = revenue * (0.48 + 0.004 * (i % 9) + 0.002 * quarter - 0.0009 * y * (1 + i % 3))
    sga = revenue * (0.12 + 0.001 * (i % 8) + 0.0008 * quarter * y)
    # Half the cohort has increasing R&D intensity; the rest have low intensity.
    rd = revenue * ((0.10 + 0.012 * y + 0.0005 * i) if i % 2 == 0 else (0.012 + 0.0001 * y))
    operating = revenue - cogs - sga - rd
    tax = operating * (0.21 + 0.002 * (i % 7) + 0.001 * quarter)
    income = operating - tax - 400_000 - 15_000 * i * quarter
    cash = 34_000_000 + 720_000 * i + 570_000 * t + 7_000 * i * t
    short_debt = 11_000_000 + 430_000 * i + 210_000 * t
    long_debt = 67_000_000 + 1_900_000 * i + 310_000 * t + 19_000 * t * t
    liabilities = assets * (0.49 + 0.001 * (i % 7)) + 110_000 * t * (i % 4)
    issued = 1_300_000 + 81_000 * i + 32_000 * t
    repaid = -(740_000 + 41_000 * i + 12_000 * t)
    repurchases = 900_000 + 94_000 * i + 15_000 * t * (i % 3 + 1)
    dividends = 520_000 + 24_000 * i + 31_000 * t
    stock_issuance = 450_000 + 43_000 * i + 42_000 * ((t + i) % 5)
    balances = {
        "total_assets": assets, "total_liabilities": liabilities,
        "stockholders_equity": assets - liabilities, "current_assets": assets * (0.31 + 0.002 * (i % 5)),
        "current_liabilities": liabilities * (0.41 + 0.001 * quarter),
        "cash_st_inv": cash, "st_debt": short_debt, "lt_debt": long_debt,
        "total_debt": short_debt + long_debt, "ppe_net": assets * (0.39 + 0.0003 * i),
        "ar": 23_000_000 + 410_000 * i + 230_000 * t + 13_000 * t * quarter,
        "inventory": 28_000_000 + 820_000 * i + 180_000 * t + 17_000 * quarter * i,
        "ap": 17_000_000 + 370_000 * i + 120_000 * t + 8_000 * quarter * t,
        "deferred_revenue": 4_000_000 + 125_000 * i + 63_000 * t,
        "retained_earnings": 42_000_000 + 1_150_000 * i + 610_000 * t,
        "long_term_investments": 19_000_000 + 550_000 * i + 170_000 * t + 11_000 * t * t,
        "pref_stock": 600_000 + 25_000 * i, "minority_int_bs": 350_000 + 21_000 * i + 8_000 * t,
    }
    flows = {
        "revenue": revenue, "cogs": cogs, "gross_profit": revenue - cogs, "sga": sga,
        "rd_expense": rd, "operating_income": operating, "net_income": income,
        "income_tax": tax, "current_tax": tax * (0.83 + 0.002 * (i % 6)),
        "operating_cash_flow": income + 2_500_000 + 130_000 * i + 49_000 * t * quarter,
        "da_cf": 3_000_000 + 79_000 * i + 31_000 * t, "interest_expense": 800_000 + 32_000 * i,
        "common_div_paid": dividends, "share_repurchases": repurchases, "stock_issuance": stock_issuance,
        "lt_debt_issued": issued, "lt_debt_repaid": repaid,
        "financing_cash_flow": issued + repaid + stock_issuance - repurchases - dividends,
    }
    return balances, flows


def build_retirement_fact_tables():
    """Return deterministic source rows without opening a warehouse."""
    tables = {name: [] for name in (
        "fundamental_statement_points", "fundamental_standardized", "fundamental_ttm_points",
        "shares_outstanding_history", "equity_daily_bars", "universe_membership",
    )}

    def add_fact(i, period, start, code, value, available, accession, basis, revision=1):
        sid = f"RET{i:03d}"
        form = "10-K" if period.month == 12 else "10-Q"
        if revision > 1:
            form += "/A"
        for alias in sorted({code, ALIASES.get(code, code)}):
            identity = f"{sid}|{alias}|{basis}|{period}|{accession}"
            point = {
                "statement_point_id": identity, "fact_revision_id": identity, "revision_group_id": identity,
                "source": SOURCE, "security_id": sid, "symbol": sid, "cik": f"{i + 1:010d}",
                "statement_type": "fixture", "statement_section": "fixture", "canonical_metric": alias,
                "canonical_label": alias, "taxonomy": "us-gaap", "concept": alias, "unit": "USD",
                "unit_type": "monetary", "normal_balance": "debit",
                "period_type": "instant" if start is None else "duration",
                "period_start": start, "period_end": period, "as_of_date": available.date(),
                "available_at": available, "fiscal_year": period.year, "fiscal_period": f"Q{period.month // 3}",
                "form": form, "accession_number": accession, "source_accession": accession,
                "filed_date": available.date(), "revision_sequence": revision, "revision_count": revision,
                "is_latest_revision": True, "is_value_changed": True, "raw_value": value, "value": value,
                "source_loaded_at": available, "source_url": "https://fixture.invalid/filing",
            }
            tables["fundamental_statement_points"].append(point)
            tables["fundamental_standardized"].append({
                "standardized_id": identity, "source": SOURCE, "security_id": sid, "item_id": 1,
                "canonical_code": alias, "basis": basis, "period_start": start, "period_end": period, "value": value,
                "source_accession": accession, "unit_type": "monetary", "source_loaded_at": available,
                "as_of_date": available.date(), "available_at": available, "input_codes_json": "[]",
                "input_item_ids_json": "[]", "rule_id": "fixture", "combination_rule": "direct",
                "revision_sequence": revision, "is_latest_revision": True,
            })

    for i in range(SECURITY_COUNT):
        sid = f"RET{i:03d}"
        # Names 24/25 enter/leave; 26 stops reporting; 27 has sparse components.
        valid_from = dt.date(2019, 1, 1) if i == 24 else dt.date(2016, 1, 1)
        valid_to = dt.date(2020, 12, 31) if i == 25 else None
        tables["universe_membership"].append({
            "universe_id": "us_common_equity_liquid_v1", "security_id": sid, "symbol": sid,
            "valid_from": valid_from, "valid_to": valid_to, "as_of_date": valid_from,
            "is_member": True, "is_latest_revision": True, "reason": "fixture member",
            "rules_json": "{}", "available_at": dt.datetime.combine(valid_from, dt.time()),
            "source": SOURCE, "source_loaded_at": dt.datetime.combine(valid_from, dt.time()),
        })
        flow_history = []
        for year in YEARS:
            for quarter in range(1, 5):
                if i == 26 and year >= 2020:
                    continue
                period = _quarter_end(year, quarter)
                start = dt.date(year, 3 * quarter - 2, 1)
                available = dt.datetime.combine(period + dt.timedelta(days=44 + i % 6), dt.time(18, i))
                accession = f"{sid}-{year}Q{quarter}"
                balances, flows = _values(i, year, quarter)
                if i == 21 and year == 2020 and quarter == 3:
                    balances["total_assets"] = 0.0  # Erroneous filed denominator, not imputed.
                if i == 22 and year == 2020:
                    flows["net_income"] = -abs(flows["net_income"])
                    flows["current_tax"] = 0.0
                if i == 23 and year == 2020 and quarter == 2:
                    flows.update(revenue=0.0, cogs=0.0, gross_profit=0.0,
                                 operating_income=-8_000_000.0, net_income=-9_000_000.0)
                if i == 27 and year >= 2018:
                    balances.pop("deferred_revenue")
                    flows.pop("common_div_paid")
                    flows.pop("current_tax")
                for code, value in balances.items():
                    add_fact(i, period, None, code, value, available, accession, "instant")
                for code, value in flows.items():
                    add_fact(i, period, start, code, value, available, accession, "quarterly")
                flow_history.append((period, start, available, accession, flows))
                if quarter == 4:
                    for code in flows:
                        if all(code in item[4] for item in flow_history[-4:]):
                            annual = sum(item[4][code] for item in flow_history[-4:])
                            add_fact(i, period, dt.date(year, 1, 1), code, annual, available, accession, "annual")
                if len(flow_history) >= 4:
                    window = flow_history[-4:]
                    for code in flows:
                        if not all(code in item[4] for item in window):
                            continue
                        identity = f"{accession}|ttm|{code}"
                        tables["fundamental_ttm_points"].append({
                            "ttm_point_id": identity, "ttm_revision_group_id": identity,
                            "anchor_statement_point_id": f"{sid}|{code}|quarterly|{period}|{accession}",
                            "source": SOURCE, "security_id": sid, "symbol": sid, "cik": f"{i + 1:010d}",
                            "statement_type": "fixture", "statement_section": "fixture",
                            "canonical_metric": code, "canonical_label": code, "unit": "USD", "unit_type": "monetary",
                            "ttm_start_date": window[0][1], "ttm_end_date": period, "as_of_date": available.date(),
                            "available_at": available, "accession_number": accession, "quarter_count": 4,
                            "coverage_days": (period - window[0][1]).days + 1,
                            "min_input_available_at": window[0][2], "max_input_available_at": available,
                            "input_statement_point_ids_json": json.dumps([
                                f"{sid}|{code}|quarterly|{row[0]}|{row[3]}" for row in window]),
                            "input_accessions_json": json.dumps([row[3] for row in window]),
                            "input_period_ends_json": json.dumps([str(row[0]) for row in window]),
                            "ttm_value": sum(row[4][code] for row in window), "revision_sequence": 1,
                            "revision_count": 1, "is_latest_revision": True, "is_value_changed": True,
                            "calculation_method": "four_actual_quarters", "source_loaded_at": available,
                        })
                # Real two-for-one splits: share count doubles while split factor is 0.5.
                share_count = (14_000_000 + 360_000 * i) * (1 + (0.003 + 0.0002 * i) * (4 * (year - 2016) + quarter))
                if i % 5 == 0 and period >= dt.date(2020, 6, 30):
                    share_count *= 2
                tables["shares_outstanding_history"].append({
                    "share_history_id": f"{accession}|shares", "source": SOURCE, "security_id": sid,
                    "symbol": sid, "cik": f"{i + 1:010d}", "share_count_type": "shares_outstanding",
                    "taxonomy": "dei", "concept": "EntityCommonStockSharesOutstanding", "unit": "shares",
                    "period_type": "instant", "period_end": period, "effective_date": period,
                    "as_of_date": available.date(), "available_at": available, "form": "10-K" if quarter == 4 else "10-Q",
                    "accession_number": accession, "revision_sequence": 1, "revision_count": 1,
                    "is_latest_revision": True, "share_count": share_count,
                    "source_url": "https://fixture.invalid/filing", "source_loaded_at": available,
                })
        # A late amendment to an old annual balance, after newer quarters exist.
        # Preserve original rows: consumers must make the PIT revision decision.
        if i in (2, 7, 13):
            period = dt.date(2019, 12, 31)
            available = dt.datetime(2021, 7, 15, 19, i)
            balances, _ = _values(i, 2019, 4)
            for code, value in balances.items():
                add_fact(i, period, None, code, value * (1.04 if code == "total_assets" else 1.015),
                         available, f"{sid}-2019Q4-amend", "instant", revision=2)
            amendment_accession = f"{sid}-2019Q4-amend"
            original_flows = _values(i, 2019, 4)[1]
            revised_flows = {
                code: value * (1.08 if code in ("revenue", "net_income", "current_tax") else 1.025)
                for code, value in original_flows.items()
            }
            for code, value in revised_flows.items():
                add_fact(i, period, dt.date(2019, 10, 1), code, value, available,
                         amendment_accession, "quarterly", revision=2)
                annual = sum(_values(i, 2019, q)[1][code] for q in (1, 2, 3)) + value
                add_fact(i, period, dt.date(2019, 1, 1), code, annual, available,
                         amendment_accession, "annual", revision=2)
            # Revise all and only four-quarter windows containing the amended quarter.
            for original in list(tables["fundamental_ttm_points"]):
                if original["security_id"] != sid:
                    continue
                if not dt.date(2019, 12, 31) <= original["ttm_end_date"] <= dt.date(2020, 9, 30):
                    continue
                code = original["canonical_metric"]
                input_ids = json.loads(original["input_statement_point_ids_json"])
                old_id = f"{sid}|{code}|quarterly|{period}|{sid}-2019Q4"
                new_id = f"{sid}|{code}|quarterly|{period}|{amendment_accession}"
                assert old_id in input_ids
                input_ids[input_ids.index(old_id)] = new_id
                point_values = {point["statement_point_id"]: point["value"]
                                for point in tables["fundamental_statement_points"] if point["security_id"] == sid}
                tables["fundamental_ttm_points"].append({
                    **original, "ttm_point_id": original["ttm_point_id"] + "-amend",
                    "anchor_statement_point_id": new_id, "as_of_date": available.date(),
                    "available_at": available, "max_input_available_at": available,
                    "accession_number": original["accession_number"] + "-amend",
                    "input_statement_point_ids_json": json.dumps(input_ids),
                    "input_accessions_json": json.dumps([
                        amendment_accession if accession == f"{sid}-2019Q4" else accession
                        for accession in json.loads(original["input_accessions_json"])]),
                    "ttm_value": sum(point_values[identity] for identity in input_ids),
                    "revision_sequence": 2, "revision_count": 2, "source_loaded_at": available,
                })
        # Bars start early enough to establish split indices and share pair history.
        for year in YEARS:
            for month in range(1, 13):
                date = dt.date(year, month, calendar.monthrange(year, month)[1])
                t = (year - 2016) * 12 + month
                price = 25 + 0.63 * i + 0.071 * t + 0.013 * ((i * month) % 11)
                split = i % 5 == 0 and date >= dt.date(2020, 6, 30)
                if split:
                    price /= 2
                if i == 20 and year == 2021 and month == 6:
                    price = 2.0  # Below the legacy $100m capitalization floor.
                # Deliberately varying publication times expose unsafe legacy cohort timestamps.
                available = dt.datetime.combine(date, dt.time(21, i))
                tables["equity_daily_bars"].append({
                    "source": SOURCE, "security_id": sid, "symbol": sid, "trade_date": date,
                    "open": price, "high": price, "low": price, "close": price, "adjusted_close": price,
                    "volume": 100 if i == 19 and 2019 <= year <= 2021 else 1_500_000 + 17_000 * i,
                    "available_at": available,
                    "split_factor": 0.5 if split and date == dt.date(2020, 6, 30) else 1.0,
                    "is_adjusted": False,
                })
    return tables


def write_retirement_facts(store) -> None:
    """Populate both financial layers, real TTM, shares, prices, universe and legacy denominators."""
    from atx_db.enterprise_value import refresh_enterprise_value
    from atx_db.valuation_multiples import refresh_market_cap
    from atx_db.warehouse import insert_frame

    for table, rows in build_retirement_fact_tables().items():
        insert_frame(store, pd.DataFrame(rows), table, f"retirement_{table}")
    refresh_market_cap(store)
    refresh_enterprise_value(store)


def validate_retirement_fact_tables(tables) -> None:
    """Assert same facts and the actual four vintages behind every TTM value."""
    points = {row["statement_point_id"]: row for row in tables["fundamental_statement_points"]}
    assert len(points) == len(tables["fundamental_statement_points"])
    standardized = tables["fundamental_standardized"]
    assert len({row["standardized_id"] for row in standardized}) == len(points)
    for row in standardized:
        point = points[row["standardized_id"]]
        assert (row["canonical_code"], row["period_end"], row["value"], row["available_at"]) == (
            point["canonical_metric"], point["period_end"], point["value"], point["available_at"],
        )
        assert row["period_start"] == point["period_start"]
        assert row["source_accession"] == point["accession_number"]
    for row in tables["fundamental_ttm_points"]:
        inputs = [points[identity] for identity in json.loads(row["input_statement_point_ids_json"])]
        assert len(inputs) == row["quarter_count"] == 4
        assert row["coverage_days"] in (365, 366)
        assert row["ttm_value"] == sum(point["value"] for point in inputs)
        assert row["available_at"] == max(point["available_at"] for point in inputs)
        assert row["ttm_start_date"] == inputs[0]["period_start"]
        assert row["ttm_end_date"] == inputs[-1]["period_end"]


def _module_options(name):
    module = importlib.import_module(f"atx_db.{name}")
    options_name = "".join(word.capitalize() for word in name.split("_")) + "Options"
    if name == "fundamental_signals":
        options_name = "FundamentalSignalOptions"
    return module, getattr(module, options_name)()


def refresh_retained_parents(store) -> None:
    """Run the genuine retained parent chain, without synthesized factor lineage."""
    for name in RETAINED_PARENTS:
        module, options = _module_options(name)
        function = "refresh_fundamental_signal_values" if name == "fundamental_signals" else f"refresh_{name}_values"
        getattr(module, function)(store, options)


def legacy_retirement_rows(store) -> pd.DataFrame:
    """Run genuine legacy refreshers, with annual-margin frame as its sole exception.

    Explicit baseline-only call: removed modules are imported only inside this
    function. Enterprise yield MUST use its refresher: its frame path differs.
    """
    refresh_retained_parents(store)
    frames = []
    for name, factor_ids in MODULE_FACTORS.items():
        module, options = _module_options(name)
        if name == "annual_margin_change":
            frame = module.compute_annual_margin_change_rows(module.load_annual_margin_change_inputs(store, options), options)
        else:
            getattr(module, f"refresh_{name}_values")(store, options)
            frame = store.con.execute(
                "SELECT factor_id, security_id, as_of_date, raw_value, value, available_at "
                "FROM fundamental_factor_values WHERE source = ? ORDER BY 1, 2, 3", [options.source],
            ).df()
        assert set(frame["factor_id"]) == set(factor_ids), f"empty/missing legacy evidence: {name}"
        assert frame.groupby("factor_id")["security_id"].nunique().min() >= 20, name
        print(f"legacy {name}: {len(frame)} rows / {len(factor_ids)} factors", flush=True)
        frames.append(frame[EVIDENCE_COLUMNS])
    rows = pd.concat(frames, ignore_index=True)
    rows["as_of_date"] = pd.to_datetime(rows["as_of_date"]).dt.date
    rows = rows.sort_values(KEY_COLUMNS).reset_index(drop=True)
    assert not rows.duplicated(KEY_COLUMNS).any()
    assert set(rows["factor_id"]) == set(FACTOR_IDS)
    return rows


def baseline_options():
    return {name: asdict(_module_options(name)[1]) for name in (*RETAINED_PARENTS, *MODULE_FACTORS)}


def frozen_expected_rows() -> pd.DataFrame:
    """Load and validate frozen complete keys and genuine legacy timestamps."""
    with gzip.open(DATA_DIR / "derived_retirement_expected.json.gz", "rt", encoding="utf-8") as stream:
        artifact = json.load(stream)
    assert digest(artifact["rows"]) == artifact["metadata"]["output_sha256"]
    rows = pd.DataFrame(artifact["rows"], columns=EVIDENCE_COLUMNS)
    rows["as_of_date"] = pd.to_datetime(rows["as_of_date"]).dt.date
    rows["available_at"] = pd.to_datetime(rows["available_at"])
    assert not rows.duplicated(KEY_COLUMNS).any()
    assert set(rows["factor_id"]) == set(FACTOR_IDS)
    return rows
