"""Published factor compatibility definitions, separate from conventional metrics.

Each entry declares its input horizon, eligibility and arithmetic. Selection,
safe arithmetic, monthly projection, winsorization and standardization are shared
engines. Retained parent factors supply their existing governed cohorts and exact
lineage; no retired implementation is imported or copied here.
"""

from __future__ import annotations

from functools import lru_cache

from .derived_compatibility import (
    CompatibilityMetric,
    Relation,
    Selection,
    current,
    factor_relation,
    json_field,
    market_inputs,
    prior,
    shares,
    statements,
    trailing,
)

ASSET_PARENT = "investment_conservative_asset_growth"
ASSET_SOURCE = "atx-db PIT conservative asset growth v1"
QOP_PARENT = "profitability_quarterly_operating_profitability_lagged_assets"
QOP_SOURCE = "atx-db PIT quarterly operating profitability v1"
REVENUE_PARENT = "growth_quarterly_revenue_yoy"
REVENUE_SOURCE = "atx-db PIT quarterly revenue growth v1"
CFO_PARENT = "profitability_operating_cash_flow_to_assets"


def _columns(metrics: tuple[str, ...], aliases: tuple[str, ...] = ("cur", "prev")) -> dict[str, str]:
    return {f"{alias}_{metric}": f"b.{alias}.{metric}" for alias in aliases for metric in metrics}


def _finite(metrics: tuple[str, ...]) -> str:
    return " AND ".join(f"isfinite({metric})" for metric in metrics)


def _json(path: str, kind: str = "DOUBLE", alias: str = "b.parent") -> str:
    return json_field(alias, path, kind)


def _annual_gap(first: str, second: str) -> str:
    return f"b.{first}.period_end-b.{second}.period_end BETWEEN 300 AND 430"


def _parent_metric(
    code: str,
    expression: str,
    columns: dict[str, str],
    *,
    selections: tuple[Selection, ...],
    eligibility: str = "true",
    maximum: float | None = None,
) -> CompatibilityMetric:
    return CompatibilityMetric(
        metric_code=code,
        expression=expression,
        columns=columns,
        parent=ASSET_PARENT,
        parent_source=ASSET_SOURCE,
        selections=selections,
        eligibility=eligibility,
        maximum_absolute_value=maximum,
    )


@lru_cache(maxsize=1)
def compatibility_metrics() -> dict[str, CompatibilityMetric]:
    definitions: list[CompatibilityMetric] = []
    noa_metrics = ("total_assets", "total_liabilities", "cash_st_inv", "st_debt", "lt_debt")
    noa = statements(
        noa_metrics,
        horizon="instant",
        eligible="total_assets>0 AND total_liabilities>=0 AND cash_st_inv>=0 AND st_debt>=0 AND lt_debt>=0 AND "
        + _finite(noa_metrics),
    )
    assets = statements(("total_assets",), horizon="instant", eligible="total_assets>0 AND isfinite(total_assets)")
    noa_expression = "cur_total_assets-cur_cash_st_inv-cur_total_liabilities+cur_st_debt+cur_lt_debt"
    definitions.append(
        CompatibilityMetric(
            "legacy_noa_to_assets",
            f"({noa_expression})/prev_total_assets",
            _columns(noa_metrics),
            selections=(current("cur", noa), prior("prev", assets, history_days=100000)),
        )
    )
    definitions.append(
        CompatibilityMetric(
            "legacy_rsst_accruals",
            f"(({noa_expression})-({noa_expression.replace('cur_', 'prev_')}))"
            "/((cur_total_assets+prev_total_assets)/2)",
            _columns(noa_metrics),
            parent="quality_net_operating_assets",
            parent_source="atx-db derived factor projection v1",
            parent_clock_path="decision.input_available_at",
            selections=(current("cur", noa), prior("prev", noa, consecutive=True)),
            eligibility=_annual_gap("cur", "prev"),
            maximum_absolute_value=5,
        )
    )
    annual_sales = statements(
        ("revenue", "total_assets"),
        eligible="revenue>0 AND total_assets>0 AND isfinite(revenue) AND isfinite(total_assets)",
    )
    definitions.append(
        CompatibilityMetric(
            "legacy_asset_turnover_change",
            "cur_revenue/prev_total_assets-prev_revenue/old_total_assets",
            _columns(("revenue", "total_assets"), ("cur", "prev", "old")),
            selections=(
                current("cur", annual_sales),
                prior("prev", annual_sales, history_days=100000),
                prior("old", assets, "prev", history_days=100000),
            ),
        )
    )
    for variant, numerator in (("net", "net_income"), ("operating", "operating_income"), ("gross", "gross_profit")):
        events = statements(("revenue", numerator), eligible=f"revenue>0 AND isfinite(revenue) AND isfinite({numerator})")
        definitions.append(
            CompatibilityMetric(
                f"legacy_annual_{variant}_margin_change",
                f"cur_{numerator}/cur_revenue-prev_{numerator}/prev_revenue",
                _columns(("revenue", numerator)),
                selections=(current("cur", events), prior("prev", events, history_days=100000)),
            )
        )

    beneish_metrics = (
        "revenue", "ar", "cogs", "total_assets", "current_assets", "ppe_net", "da_cf", "da_is",
        "depreciation", "sga", "total_liabilities", "net_income", "operating_cash_flow",
    )
    beneish_base = statements(beneish_metrics)
    beneish = Relation(
        "SELECT *,coalesce(da_cf,da_is,depreciation) AS da FROM (" + beneish_base.sql + ") "
        "WHERE revenue>0 AND ar>=0 AND cogs>=0 AND total_assets>0 AND current_assets>=0 "
        "AND ppe_net>=0 AND coalesce(da_cf,da_is,depreciation)>0 AND sga>=0 AND total_liabilities>0 "
        "AND isfinite(coalesce(da_cf,da_is,depreciation)) AND "
        + _finite(tuple(m for m in beneish_metrics if m not in ("da_cf", "da_is", "depreciation")))
    )
    definitions.append(
        _parent_metric(
            "legacy_beneish_m",
            "-4.84+0.920*(cur_ar/cur_revenue)/(prev_ar/prev_revenue)"
            "+0.528*((prev_revenue-prev_cogs)/prev_revenue)/((cur_revenue-cur_cogs)/cur_revenue)"
            "+0.404*(1-(cur_current_assets+cur_ppe_net)/cur_total_assets)"
            "/(1-(prev_current_assets+prev_ppe_net)/prev_total_assets)"
            "+0.892*cur_revenue/prev_revenue"
            "+0.115*(prev_da/(prev_da+prev_ppe_net))/(cur_da/(cur_da+cur_ppe_net))"
            "-0.172*(cur_sga/cur_revenue)/(prev_sga/prev_revenue)"
            "+4.679*(cur_net_income-cur_operating_cash_flow)/cur_total_assets"
            "-0.327*(cur_total_liabilities/cur_total_assets)/(prev_total_liabilities/prev_total_assets)",
            _columns((*beneish_metrics, "da")),
            selections=(current("cur", beneish), prior("prev", beneish, consecutive=True)),
            eligibility=_annual_gap("cur", "prev")
            + " AND b.prev.ar>0 AND b.prev.sga>0 AND b.cur.revenue>b.cur.cogs AND b.prev.revenue>b.prev.cogs "
            "AND 1-(b.prev.current_assets+b.prev.ppe_net)/b.prev.total_assets<>0",
            maximum=100,
        )
    )

    accession = _json("assets.current.accession_number", "VARCHAR")
    annual_end = _json("assets.current.period_end", "DATE")
    prior_assets = _json("assets.prior.value")
    for code, metrics, expression in (
        ("legacy_external_financing", ("financing_cash_flow",), "cur_financing_cash_flow/prior_assets"),
        ("legacy_net_debt_financing", ("lt_debt_issued", "lt_debt_repaid"),
         "(cur_lt_debt_issued+cur_lt_debt_repaid)/prior_assets"),
    ):
        relation = statements(metrics, row_filter="isfinite(value)")
        definitions.append(
            _parent_metric(
                code,
                expression,
                {**_columns(metrics, ("cur",)), "prior_assets": prior_assets},
                selections=(Selection("cur", relation, f"s.accession_number={accession} AND s.period_end={annual_end} "
                                      "AND s.available_at<=b.decision_available_at"),),
                eligibility=f"{prior_assets}>0 AND isfinite({prior_assets})",
            )
        )

    rd_metrics = ("rd_expense", "revenue", "total_assets")
    rd = statements(rd_metrics, eligible="rd_expense>0 AND revenue>0 AND total_assets>0", row_filter="value>0 AND isfinite(value)")
    definitions.append(
        _parent_metric(
            "legacy_large_rd_increase",
            "indicator_gt(cur_rd_expense/cur_revenue,0.05)"
            "*indicator_gt(cur_rd_expense/((cur_total_assets+prev_total_assets)/2),0.05)"
            "*indicator_gt(cur_rd_expense/prev_rd_expense-1,0.05)"
            "*indicator_gt((cur_rd_expense/cur_revenue)/(prev_rd_expense/prev_revenue)-1,0.05)"
            "*indicator_gt((cur_rd_expense/((cur_total_assets+prev_total_assets)/2))"
            "/(prev_rd_expense/((prev_total_assets+old_total_assets)/2))-1,0.05)",
            _columns(rd_metrics, ("cur", "prev", "old")),
            selections=(current("cur", rd), prior("prev", rd, consecutive=True, history_days=1300),
                        prior("old", rd, "prev", consecutive=True, history_days=1300)),
            eligibility=_annual_gap("cur", "prev") + " AND " + _annual_gap("prev", "old"),
        )
    )
    tax = statements(("income_tax",), horizon="quarter", annual_forms=False, row_filter="isfinite(value)", by_start=True)
    tax_assets = statements(("total_assets",), horizon="all_instant", row_filter="value>0 AND isfinite(value)")
    tax_prior = Relation(
        f"SELECT t.*,a.total_assets,a.total_assets_id,a.available_at AS assets_at FROM ({tax.sql}) t "
        f"JOIN ({tax_assets.sql}) a USING (security_id,accession_number,period_end)"
    )
    tax_lag = prior("prev", tax_prior, history_days=100000)
    definitions.append(
        _parent_metric(
            "legacy_tax_expense_momentum",
            "(cur_income_tax-prev_income_tax)/prev_total_assets",
            _columns(("income_tax", "total_assets")),
            selections=(current("cur", tax, age=200), Selection(
                tax_lag.name, tax_lag.relation, tax_lag.predicate + " AND s.assets_at<=b.decision_available_at", tax_lag.order
            )),
            eligibility="b.cur.income_tax<>b.prev.income_tax AND b.prev.total_assets>0",
            maximum=1,
        )
    )
    taxes = statements(("current_tax", "net_income"), row_filter="isfinite(value)", by_start=True,
                       eligible="current_tax IS NOT NULL AND net_income IS NOT NULL")
    definitions.append(
        _parent_metric(
            "legacy_tax_to_book_income",
            "cur_current_tax*(1-statutory_rate)/statutory_rate/cur_net_income",
            {**_columns(("current_tax", "net_income"), ("cur",)),
             "statutory_rate": "CASE WHEN b.cur.period_end<DATE '2018-01-01' THEN 0.35 ELSE 0.21 END"},
            selections=(current("cur", taxes),),
            eligibility="b.cur.current_tax>0 AND b.cur.net_income>0",
            maximum=10,
        )
    )
    rd_annual = statements(("rd_expense",), duration=(329, 379), row_filter="value>0 AND isfinite(value)")
    definitions.append(
        CompatibilityMetric(
            "legacy_rd_to_market_equity", "rd/market_cap", {"rd": "b.cur.rd_expense", "market_cap": "b.market.market_cap"},
            grid="market_cap", selections=(current("cur", rd_annual),),
        )
    )

    for pair in (False, True):
        relation = shares(annual_pair=pair)
        order = "s.effective_date DESC,s.available_at DESC,CASE WHEN s.taxonomy='dei' THEN 0 ELSE 1 END,"
        order += "s.share_history_id DESC" if pair else "s.revision_sequence DESC,s.share_history_id DESC"
        share_selection = Selection("shares", relation, "s.period_end<=b.as_of_date AND s.available_at<=b.decision_available_at", order)
        market_cap = "b.market.close*b.shares.share_count*b.shares.split_index/b.market.split_index"
        eligibility = f"{market_cap}>=100000000 AND b.market.adv21_usd>=1000000"
        if pair:
            definitions.append(
                CompatibilityMetric(
                    "legacy_net_share_issuance", "ln(shares*split/(prior_shares*prior_split))",
                    {"shares": "b.shares.share_count", "split": "b.shares.split_index",
                     "prior_shares": "b.shares.prior_shares", "prior_split": "b.shares.prior_split"},
                    governed=False, selections=(share_selection,), period="b.shares.period_end",
                    eligibility=eligibility+" AND b.as_of_date-b.shares.period_end<=550", maximum_absolute_value=3,
                )
            )
        else:
            payout = trailing(("common_div_paid", "share_repurchases", "stock_issuance"), complete=True)
            payout = Relation(f"SELECT * FROM ({payout.sql}) WHERE common_div_paid IS NOT NULL "
                              "AND share_repurchases IS NOT NULL AND stock_issuance IS NOT NULL")
            definitions.append(
                CompatibilityMetric(
                    "legacy_net_payout_yield", "(-dividends-repurchases-issuance)/market_cap",
                    {"dividends": "b.cur.common_div_paid", "repurchases": "b.cur.share_repurchases",
                     "issuance": "b.cur.stock_issuance", "market_cap": market_cap},
                    governed=False, selections=(current("cur", payout, age=100000), share_selection),
                    eligibility=eligibility+" AND b.as_of_date-b.cur.period_end<=550", maximum_absolute_value=5,
                )
            )

    for code, numerator in (("ebit", "operating_income"), ("sales", "revenue")):
        events = trailing((numerator,), latest=True, by_accession=False)
        events = Relation(f"SELECT * FROM ({events.sql}) WHERE {numerator}>0 AND isfinite({numerator})")
        definitions.append(
            CompatibilityMetric(
                f"legacy_{code}_to_ev", "numerator/ev", {"numerator": f"b.cur.{numerator}", "ev": "b.market.enterprise_value"},
                grid="enterprise_value", selections=(current("cur", events),),
            )
        )
    ev = Relation(f"SELECT * FROM ({market_inputs(enterprise=True).sql}) WHERE enterprise_value>0 "
                  "AND isfinite(enterprise_value) AND available_at<=market_cap_available_at")
    for code, parent, path, id_path, period_path in (
        ("gross_profit", "profitability_gross_profitability", "gross_profit.value", "gross_profit.id", "gross_profit.period_end"),
        ("cfo", CFO_PARENT, "ttm.operating_cash_flow_ttm", "ttm.operating_cash_flow_ttm_id", "ttm.period_end"),
    ):
        numerator = _json(path)
        period = _json(period_path, "DATE")
        definitions.append(
            CompatibilityMetric(
                f"legacy_{code}_to_ev", "numerator/ev", {"numerator": numerator, "ev": "b.ev.enterprise_value"},
                parent=parent,
                selections=(Selection("ev", ev, "s.trade_date=b.as_of_date AND b.decision_available_at<=s.market_cap_available_at",
                                      "s.available_at DESC,s.enterprise_value_id DESC"),),
                period=period,
                eligibility=f"{numerator}>0 AND isfinite({numerator}) AND {_json(id_path, 'VARCHAR')} IS NOT NULL "
                f"AND b.as_of_date-{period}<=550",
            )
        )

    lag_assets = _json("statements.lagged_total_assets")
    definitions.append(
        CompatibilityMetric(
            "legacy_quarterly_working_capital_accruals", "change/assets",
            {"change": _json("net_operating_working_capital_change"), "assets": lag_assets},
            parent="profitability_quarterly_cash_operating_profitability_lagged_assets",
            parent_source="atx-db PIT quarterly cash profitability v1",
            period=_json("statements.current_period_end", "DATE"),
            eligibility=f"{lag_assets}>0", maximum_absolute_value=5,
        )
    )
    qop = factor_relation(QOP_PARENT, source=QOP_SOURCE)
    qop_selections = tuple(
        Selection(alias, qop, f"s.factor_value_id={_json(path+'.qop_factor_value_id', 'VARCHAR')}", "s.factor_value_id")
        for alias, path in (("cur", "current"), ("prev", "prior_year"))
    )
    definitions.append(
        CompatibilityMetric(
            "legacy_quarterly_profitability_change", "current_profitability-prior_profitability",
            {"current_profitability": "b.cur.raw_value", "prior_profitability": "b.prev.raw_value"},
            parent=REVENUE_PARENT, parent_source=REVENUE_SOURCE, selections=qop_selections,
            period=_json("current.period_end", "DATE"), maximum_absolute_value=10,
        )
    )
    margins = {}
    for alias in ("cur", "prev"):
        fields = {m: json_field(f"b.{alias}", f"quarterly_statement.{m}.value") for m in ("revenue", "cogs", "gross_profit", "sga", "rd_expense")}
        operating = json_field(f"b.{alias}", "quarterly_statement.operating_profit")
        numerator = f"CASE WHEN isfinite({fields['cogs']}) THEN {fields['revenue']}-{fields['cogs']} "
        numerator += f"WHEN isfinite({fields['gross_profit']}) THEN {fields['gross_profit']} "
        numerator += f"ELSE {operating}+{fields['sga']}-CASE WHEN isfinite({fields['rd_expense']}) THEN {fields['rd_expense']} ELSE 0 END END"
        margins[alias] = f"({numerator})/nullif({fields['revenue']},0)"
    definitions.append(
        CompatibilityMetric(
            "legacy_quarterly_gross_margin_change", "cur-prev", margins,
            parent=REVENUE_PARENT, parent_source=REVENUE_SOURCE, selections=qop_selections,
            period=_json("current.period_end", "DATE"),
            eligibility=f"abs({margins['cur']})<=5 AND abs({margins['prev']})<=5", maximum_absolute_value=5,
        )
    )

    book = factor_relation("value_book_to_market")
    altman_selections = [Selection("book", book, "s.as_of_date=b.as_of_date AND isfinite(s.value)", "s.available_at DESC,s.source_loaded_at DESC")]
    altman_columns = {"assets": _json("assets.current_value"), "market_cap": json_field("b.book", "market_cap_usd")}
    for metric in ("current_assets", "current_liabilities", "retained_earnings", "total_liabilities", "operating_income", "revenue"):
        is_ttm = metric in ("operating_income", "revenue")
        cutoff = "greatest(b.decision_available_at,b.book.available_at)"
        relation = trailing((metric,), by_accession=False, visible_at=cutoff) if is_ttm else statements(
            (metric,), horizon="all_instant", row_filter="isfinite(value)", visible_at=cutoff
        )
        period = _json("ttm.period_end" if is_ttm else "assets.current_period_end", "DATE")
        altman_selections.append(Selection(metric, relation, f"s.period_end={period} AND s.available_at<=greatest(b.decision_available_at,b.book.available_at)", "s.available_at DESC,s.accession_number DESC"))
        altman_columns[metric] = f"b.{metric}.{metric}"
    definitions.append(
        CompatibilityMetric(
            "legacy_altman_z", "1.2*(current_assets-current_liabilities)/assets+1.4*retained_earnings/assets"
            "+3.3*operating_income/assets+0.6*market_cap/total_liabilities+revenue/assets",
            altman_columns, parent=CFO_PARENT, selections=tuple(altman_selections),
            period=_json("assets.current_period_end", "DATE"),
            eligibility=f"{altman_columns['assets']}>0 AND {altman_columns['market_cap']}>0 AND b.total_liabilities.total_liabilities>0",
            maximum_absolute_value=100,
        )
    )
    return {definition.metric_code: definition for definition in definitions}
