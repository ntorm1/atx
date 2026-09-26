"""Reusable PIT input selection for published legacy factor definitions.

The conventional quarterly metric catalog remains independent. Compatibility
definitions use the same restricted arithmetic compiler, with explicit filing,
decision-grid and fiscal-lag selection instead of implicitly changing a metric's
horizon. All SQL fragments are package-owned catalog data, never user input.
"""

from __future__ import annotations

import datetime as dt
from dataclasses import dataclass
from typing import Any

import pandas as pd

from ._vendor_artifact import bar_pick_order_sql, bars_relation_sql
from .connection import DuckDBStore
from .derived_dsl import LowerContext, compile_expression

ANNUAL_FORMS = "('10-K','10-K/A','10-KT','20-F','20-F/A','40-F','40-F/A')"
QUARTER_FORMS = "('10-Q','10-Q/A','10-QT','10-K','10-K/A','10-KT','20-F','20-F/A','40-F','40-F/A','6-K','6-K/A')"


@dataclass(frozen=True)
class Relation:
    """A reusable source relation with a security, period and availability clock."""

    sql: str


@dataclass(frozen=True)
class Selection:
    name: str
    relation: Relation
    predicate: str
    order: str = "s.period_end DESC,s.available_at DESC,s.accession_number DESC"


@dataclass(frozen=True)
class CompatibilityMetric:
    metric_code: str
    expression: str
    columns: dict[str, str]
    grid: str = "prices"
    governed: bool = True
    selections: tuple[Selection, ...] = ()
    eligibility: str = "true"
    period: str = "b.cur.period_end"
    parent: str | None = None
    parent_source: str | None = None
    parent_clock_path: str | None = None
    maximum_absolute_value: float | None = None
    parent_rank_after_selections: bool = False


def json_field(alias: str, path: str, kind: str = "DOUBLE") -> str:
    return f"try_cast(json_extract_string({alias}.input_lineage_json,'$.{path}') AS {kind})"


def statements(
    metrics: tuple[str, ...],
    *,
    horizon: str = "annual",
    eligible: str = "true",
    row_filter: str = "true",
    by_start: bool = False,
    annual_forms: bool = True,
    duration: tuple[int, int] | None = None,
    visible_at: str | None = None,
) -> Relation:
    """Pivot one accession; instant balances and matching duration facts coexist."""

    duration = duration or ((329, 399) if horizon == "annual" else (69, 114))
    period_filter = (
        "period_type='instant'"
        if horizon == "instant"
        else f"(period_type='instant' OR (period_type='duration' AND "
        f"period_end-period_start BETWEEN {duration[0]} AND {duration[1]}))"
    )
    forms = f"AND form IN {ANNUAL_FORMS if annual_forms else QUARTER_FORMS}" if horizon != "all_instant" else ""
    if horizon == "all_instant":
        period_filter = "period_type='instant'"
    pivots: list[str] = []
    for metric in metrics:
        pred = f"canonical_metric='{metric}'"
        order = "(available_at,revision_sequence,statement_point_id)"
        # The newest revision wins even when its value is NULL (arg_max would skip it and
        # revive an older value); value and id come from the same row of one total order.
        pivots.extend(
            (
                f"arg_max_null(value,{order}) FILTER (WHERE {pred}) AS {metric}",
                f"arg_max(statement_point_id,{order}) FILTER (WHERE {pred}) AS {metric}_id",
                f"max(available_at) FILTER (WHERE {pred}) AS {metric}_at",
            )
        )
    start = ",period_start" if by_start else ""
    codes = ",".join(f"'{m}'" for m in metrics)
    visibility = f" AND available_at<={visible_at}" if visible_at else ""
    # A row without a clock is never visible (it would also sort as the newest key).
    return Relation(
        f"SELECT * FROM (SELECT security_id,accession_number,period_end{start},"
        f"{','.join(pivots)},max(available_at) AS available_at "
        f"FROM fundamental_statement_points WHERE canonical_metric IN ({codes}) "
        f"AND unit='USD' AND accession_number IS NOT NULL AND period_end IS NOT NULL "
        f"AND available_at IS NOT NULL "
        f"AND {period_filter} {forms} AND ({row_filter}) {visibility} "
        f"GROUP BY security_id,accession_number,period_end{start}) WHERE {eligible}"
    )


def trailing(
    metrics: tuple[str, ...], *, complete: bool = False, latest: bool = False,
    by_accession: bool = True, visible_at: str | None = None,
) -> Relation:
    """Select reported TTM facts, optionally requiring four-quarter coverage.

    ``latest`` selects each TTM point's newest revision visible at the selecting grid row's
    decision clock (``b.decision_available_at``; the relation is read inside a selection's
    lateral join), never the stored ``is_latest_revision`` flag: that flag is today's
    knowledge and hides a restated point's original at every decision before the restatement.
    An explicit ``visible_at`` takes precedence.
    """

    fields: list[str] = []
    for metric in metrics:
        pred = f"canonical_metric='{metric}'"
        fields.extend(
            (
                f"arg_max_null(ttm_value,(available_at,revision_sequence,ttm_point_id)) FILTER (WHERE {pred}) AS {metric}",
                f"arg_max(ttm_point_id,(available_at,revision_sequence,ttm_point_id)) FILTER (WHERE {pred}) AS {metric}_id",
            )
        )
    codes = ",".join(f"'{m}'" for m in metrics)
    filters = "AND available_at IS NOT NULL"
    if latest and not visible_at:
        visible_at = "b.decision_available_at"
    if visible_at:
        filters += f" AND available_at<={visible_at}"
    if complete:
        filters += " AND unit_type='monetary' AND quarter_count=4 AND coverage_days BETWEEN 330 AND 400"
    accession = "accession_number" if by_accession else "'' AS accession_number"
    groups = ",accession_number" if by_accession else ""
    return Relation(
        f"SELECT security_id,{accession},ttm_end_date AS period_end,{','.join(fields)},"
        f"max(available_at) AS available_at FROM fundamental_ttm_points "
        f"WHERE canonical_metric IN ({codes}) {filters} "
        f"GROUP BY security_id,ttm_end_date{groups}"
    )


def factor_relation(factor_id: str, *, source: str | None = None) -> Relation:
    source_filter = f"AND source='{source}'" if source else ""
    return Relation(
        f"SELECT * FROM fundamental_factor_values WHERE factor_id='{factor_id}' "
        f"AND is_latest_revision {source_filter}"
    )


def current(name: str, relation: Relation, *, age: int = 550, predicate: str = "true") -> Selection:
    return Selection(
        name,
        relation,
        f"s.period_end<=b.as_of_date AND b.as_of_date-s.period_end<={age} "
        f"AND s.available_at<=b.decision_available_at AND ({predicate})",
    )


def prior(
    name: str,
    relation: Relation,
    anchor: str = "cur",
    *,
    consecutive: bool = False,
    history_days: int = 1000,
) -> Selection:
    """Nearest annual lag, or immediately preceding complete fiscal observation."""

    predicate = (
        f"s.period_end<b.{anchor}.period_end AND s.available_at<=b.decision_available_at "
        f"AND b.as_of_date-s.period_end<={history_days}"
    )
    order = "s.period_end DESC,s.available_at DESC,s.accession_number DESC"
    if not consecutive:
        predicate += f" AND b.{anchor}.period_end-s.period_end BETWEEN 300 AND 430"
        order = f"abs((b.{anchor}.period_end-s.period_end)-365)," + order
    return Selection(name, relation, predicate, order)


# One whole bar per (security, session): the publisher's total order over the rows visible
# at the session's 22:00 UTC cutoff, then the price is checked. Separate arg_max picks would
# skip a newest NULL volume or split factor and revive an older revision's (a phantom split).
_PRICE_RELATION = f"""
SELECT *,product(CASE WHEN split_factor>0 AND (split_factor<=0.8 OR split_factor>=1.25)
                     THEN split_factor ELSE 1 END)
             OVER (PARTITION BY security_id ORDER BY trade_date) AS split_index,
         avg(close*volume) OVER (PARTITION BY security_id ORDER BY trade_date
                                ROWS BETWEEN 20 PRECEDING AND CURRENT ROW) AS adv21_usd
FROM (
    SELECT security_id,symbol,trade_date,close,volume,split_factor,available_at
    FROM (
        SELECT security_id,symbol,trade_date,close,volume,split_factor,available_at,
               row_number() OVER (PARTITION BY security_id,trade_date
                                  ORDER BY {bar_pick_order_sql()}) AS bar_pick
        FROM {bars_relation_sql()} equity_daily_bars
        WHERE trade_date IS NOT NULL AND available_at IS NOT NULL
          AND available_at<=CAST(trade_date AS TIMESTAMP)+INTERVAL 22 HOUR
    )
    WHERE bar_pick=1 AND close>0
)
"""


def shares(*, annual_pair: bool = False) -> Relation:
    """PIT share observations with cumulative split-only adjustment and optional annual pairing."""

    annual_share_forms = "('10-Q','10-Q/A','10-QT','10-K','10-K/A','10-KT','20-F','20-F/A','40-F','40-F/A')"
    forms = f"AND form IN {annual_share_forms}" if annual_pair else ""
    base = f"""
        SELECT h.*,h.effective_date AS period_end,coalesce(p.split_index,1) AS split_index
        FROM shares_outstanding_history h ASOF LEFT JOIN ({_PRICE_RELATION}) p
          ON h.security_id=p.security_id AND h.effective_date>=p.trade_date
        WHERE share_count_type='shares_outstanding' AND share_count>0
          AND concept IN ('EntityCommonStockSharesOutstanding','CommonStockSharesOutstanding')
          AND effective_date IS NOT NULL AND h.available_at IS NOT NULL {forms}
    """
    if not annual_pair:
        return Relation(base)
    return Relation(
        f"""WITH observations AS ({base})
        SELECT c.*,p.share_count AS prior_shares,p.split_index AS prior_split,
               p.available_at AS prior_available_at,p.share_history_id AS prior_id
        FROM observations c JOIN LATERAL (
            SELECT * FROM observations p WHERE p.security_id=c.security_id
              AND p.taxonomy=c.taxonomy AND p.concept=c.concept
              AND c.effective_date-p.effective_date BETWEEN 300 AND 430
              AND p.available_at<=c.available_at
            ORDER BY abs((c.effective_date-p.effective_date)-365),p.effective_date DESC,
                     p.available_at DESC,p.revision_sequence DESC,p.share_history_id DESC LIMIT 1
        ) p ON true"""
    )


def market_inputs(*, enterprise: bool = False) -> Relation:
    """Legacy denominator conventions over current ladder inputs, without old tables.

    Shares-outstanding observations outrank diluted averages. Enterprise inputs
    require reported preferred stock, minority interest and cash at the same
    period; missing components do not silently become zero. Debt retains the
    reported-total / current-plus-noncurrent fallback used by the published
    factors. These are compatibility denominators, not the core daily EV code.
    """

    cap = f"""SELECT p.security_id,p.symbol,p.trade_date,p.close,
        p.close*s.share_count AS market_cap,greatest(p.available_at,s.available_at) AS available_at,
        greatest(p.available_at,s.available_at) AS market_cap_available_at,
        s.share_history_id AS market_cap_id
        FROM ({_PRICE_RELATION}) p JOIN LATERAL (
            SELECT * FROM shares_outstanding_history s WHERE s.security_id=p.security_id
              AND s.share_count_type IN ('shares_outstanding','shares_diluted_avg')
              AND s.effective_date<=p.trade_date AND s.as_of_date<=p.trade_date
              AND s.available_at<=CAST(p.trade_date AS TIMESTAMP)+INTERVAL 22 HOUR
              AND s.share_count>0 AND isfinite(s.share_count) AND s.source IS NOT NULL
            ORDER BY CASE WHEN s.share_count_type='shares_outstanding' THEN 1 ELSE 0 END DESC,
                     s.effective_date DESC,s.as_of_date DESC,s.available_at DESC,
                     coalesce(s.revision_sequence,0) DESC,s.share_history_id DESC LIMIT 1
        ) s ON true"""
    if not enterprise:
        return Relation(cap)
    canonical = {
        "total_debt": "TotalDebt", "pref_stock": "PreferredStockValue",
        "minority_int_bs": "MinorityInterest", "cash_st_inv": "CashAndCashEquivalentsAtCarryingValue",
    }
    debt = ("DebtCurrent", "LongTermDebtCurrent", "ShortTermBorrowings", "CommercialPaper", "LongTermDebtNoncurrent")
    concepts = (*canonical.values(), *debt)
    cases = " ".join(f"WHEN '{code}' THEN '{concept}'" for code, concept in canonical.items())
    codes = ",".join(f"'{code}'" for code in canonical)
    raw_concepts = ",".join(f"'{concept}'" for concept in concepts if concept != "TotalDebt")
    components = f"""SELECT security_id,period_end,CASE canonical_metric {cases} END AS concept,
               value,available_at,coalesce(revision_sequence,0) AS revision_sequence,statement_point_id AS id,0 AS priority
        FROM fundamental_statement_points WHERE is_latest_revision AND period_type='instant'
          AND canonical_metric IN ({codes}) AND value>=0 AND isfinite(value) AND available_at IS NOT NULL
        UNION ALL
        SELECT security_id,period_end,concept,value,available_at,coalesce(revision_sequence,0),fact_revision_id,1
        FROM fundamental_fact_revisions WHERE is_latest_revision AND period_start IS NULL AND unit='USD'
          AND concept IN ({raw_concepts}) AND value>=0 AND isfinite(value) AND available_at IS NOT NULL"""
    pivot = ",".join(f"max(value) FILTER (WHERE concept='{code}') AS {code}" for code in concepts)
    amount = "coalesce(TotalDebt,coalesce(DebtCurrent,coalesce(LongTermDebtCurrent,0)+coalesce(ShortTermBorrowings,CommercialPaper,0))+coalesce(LongTermDebtNoncurrent,0))"
    events = f"""WITH picked AS (
        SELECT * FROM ({components}) QUALIFY row_number() OVER (PARTITION BY security_id,period_end,concept
            ORDER BY priority DESC,available_at DESC,revision_sequence DESC,id DESC)=1
        ) SELECT security_id,period_end,{pivot},max(available_at) AS available_at,
            string_agg(id,'|' ORDER BY concept) AS enterprise_value_id
        FROM picked GROUP BY security_id,period_end"""
    events = f"""SELECT *,{amount} AS debt FROM ({events})
        WHERE PreferredStockValue IS NOT NULL AND MinorityInterest IS NOT NULL
          AND CashAndCashEquivalentsAtCarryingValue IS NOT NULL
          AND coalesce(TotalDebt,DebtCurrent,LongTermDebtCurrent,ShortTermBorrowings,CommercialPaper,LongTermDebtNoncurrent) IS NOT NULL"""
    # The original denominator prefers any filing known at trade-date midnight.
    # Only after choosing that filing do yield selectors enforce the market-cap
    # clock; prefiltering here could incorrectly fall back from a future winner.
    return Relation(f"""SELECT c.* EXCLUDE (available_at),greatest(c.available_at,f.available_at) AS available_at,
        c.market_cap+f.debt+f.PreferredStockValue+f.MinorityInterest-f.CashAndCashEquivalentsAtCarryingValue AS enterprise_value,
        f.enterprise_value_id,f.period_end
        FROM ({cap}) c JOIN LATERAL (
            SELECT * FROM ({events}) f WHERE f.security_id=c.security_id
              AND f.period_end<=c.trade_date
            ORDER BY (f.available_at<=CAST(c.trade_date AS TIMESTAMP)) DESC,
                     f.period_end DESC,f.available_at DESC,f.enterprise_value_id DESC LIMIT 1
        ) f ON true""")


def _grid(metric: CompatibilityMetric, universe_id: str) -> str:
    if metric.parent:
        source = factor_relation(metric.parent, source=metric.parent_source).sql
        clock = json_field("p", metric.parent_clock_path, "TIMESTAMP") if metric.parent_clock_path else "p.available_at"
        parent_grid = (
            f"SELECT p.security_id,p.symbol,p.as_of_date,{clock} AS decision_available_at,"
            "p.available_at AS dependency_available_at,p AS parent "
            f"FROM ({source}) p WHERE p.available_at<=CAST(p.as_of_date AS TIMESTAMP)+INTERVAL 22 HOUR "
            f"AND {clock}<=CAST(p.as_of_date AS TIMESTAMP)+INTERVAL 22 HOUR"
        )
        if not metric.parent_rank_after_selections:
            parent_grid += (
                " QUALIFY row_number() OVER (PARTITION BY p.security_id,p.as_of_date "
                "ORDER BY p.available_at DESC,p.source_loaded_at DESC,p.factor_value_id DESC)=1"
            )
        return parent_grid
    if metric.grid == "prices":
        source = _PRICE_RELATION
        clock = "m.available_at"
    elif metric.grid == "market_cap":
        source = f"SELECT * FROM ({market_inputs().sql}) WHERE market_cap>0 AND isfinite(market_cap)"
        clock = "m.available_at"
    elif metric.grid == "enterprise_value":
        source = (f"SELECT * FROM ({market_inputs(enterprise=True).sql}) "
                  "WHERE enterprise_value>0 AND isfinite(enterprise_value) AND available_at<=market_cap_available_at")
        clock = "m.market_cap_available_at"
    else:
        raise ValueError(f"Unknown compatibility grid: {metric.grid}")
    month = (
        f"SELECT * FROM ({source}) QUALIFY row_number() OVER (PARTITION BY security_id,"
        "year(trade_date),month(trade_date) ORDER BY trade_date DESC,available_at DESC)=1"
    )
    select = f"SELECT m.security_id,m.symbol,m.trade_date AS as_of_date,{clock} AS decision_available_at,"
    select += f"{clock} AS dependency_available_at,m AS market"
    if not metric.governed:
        return f"{select} FROM ({month}) m"
    escaped_universe = universe_id.replace("'", "''")
    return f"""{select} FROM ({month}) m JOIN universe_membership u
        ON u.security_id=m.security_id AND u.universe_id='{escaped_universe}'
       AND u.valid_from<=m.trade_date AND (u.valid_to IS NULL OR u.valid_to>=m.trade_date)
       AND u.as_of_date<=m.trade_date AND u.is_member
       AND u.available_at<={clock}
       QUALIFY row_number() OVER (PARTITION BY m.security_id,m.trade_date
           ORDER BY u.valid_from DESC,u.available_at DESC,u.source_loaded_at DESC,u.source DESC)=1"""


def build_compatibility_sql(metric: CompatibilityMetric, universe_id: str) -> str:
    """Compile one declarative selection pipeline and the standard arithmetic DSL."""

    ctes = [f"grid AS ({_grid(metric, universe_id)})"]
    previous = "grid"
    clocks = ["b.decision_available_at", "b.dependency_available_at"]
    for index, selection in enumerate(metric.selections):
        name = f"selected_{index}"
        ctes.append(
            f"{name} AS (SELECT b.*,s AS {selection.name} FROM {previous} b JOIN LATERAL "
            f"(SELECT * FROM ({selection.relation.sql}) s WHERE s.security_id=b.security_id "
            f"AND ({selection.predicate}) ORDER BY {selection.order} LIMIT 1) s ON true)"
        )
        clocks.append(f"b.{selection.name}.available_at")
        previous = name
    if metric.parent_rank_after_selections:
        # Some parent variants have a stricter clock supplied by a selected
        # market observation. Rank only the parent/input combinations that
        # survived that selection, so a later invisible row cannot hide one.
        ctes.append(
            f"eligible_parents AS (SELECT b.* FROM {previous} b "
            "QUALIFY row_number() OVER (PARTITION BY b.security_id,b.as_of_date "
            "ORDER BY b.parent.available_at DESC,b.parent.source_loaded_at DESC,"
            "b.parent.factor_value_id DESC)=1)"
        )
        previous = "eligible_parents"
    context = LowerContext(
        grid="quarter",
        columns=metric.columns,
        availability=dict.fromkeys(metric.columns, "b.decision_available_at"),
        partition_sql="b.security_id",
        order_sql="b.as_of_date",
    )
    lowered = compile_expression(metric.expression, context)
    clock = f"greatest({','.join(clocks)})" if len(clocks) > 1 else clocks[0]
    ctes.append(
        f"evaluated AS (SELECT b.security_id,b.symbol,b.as_of_date,{metric.period} AS period_end,"
        f"{lowered.value_sql} AS metric_value,{clock} AS metric_available_at,"
        f"{clock} AS decision_available_at,to_json(b) AS compatibility_inputs_json "
        f"FROM {previous} b WHERE {metric.eligibility})"
    )
    bound = f"AND abs(metric_value)<={metric.maximum_absolute_value}" if metric.maximum_absolute_value else ""
    return (
        "WITH " + ",\n".join(ctes) + f" SELECT * FROM evaluated WHERE isfinite(metric_value) {bound}"
        " AND decision_available_at<=CAST(as_of_date AS TIMESTAMP)+INTERVAL 22 HOUR"
    )


def load_compatibility_inputs(
    store: DuckDBStore,
    metric: CompatibilityMetric,
    *,
    universe_id: str,
    start_date: dt.date | None = None,
    end_date: dt.date | None = None,
) -> pd.DataFrame:
    sql = build_compatibility_sql(metric, universe_id)
    params: list[Any] = []
    for op, value in ((">=", start_date), ("<=", end_date)):
        if value is not None:
            sql += f" AND as_of_date {op} ?"
            params.append(value)
    return store.con.execute(sql + " ORDER BY as_of_date,security_id", params).df()
