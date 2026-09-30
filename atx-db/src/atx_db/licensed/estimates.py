"""Analyst estimates adapter: consensus, detail, actuals, recommendations, price targets (S8.1).

Raw layout: I/B/E/S History (LSEG) as distributed through WRDS, one CSV family per table: ``statsum_*`` (summary
statistics), ``det_*`` (detail), ``act_*`` (actuals), ``recddet_*`` (recommendations), ``ptgdet_*`` (price targets).
Column aliases also accept the FactSet/Zacks-style names the v2 loaders took. I/B/E/S times are US/Eastern.

PIT rules (rule ``estimates-pit-v1``; ``docs/LICENSED_ADAPTERS.md``):

* ``consensus``: the summary computed as of STATPERS (the Thursday before the third Friday for the monthly file; any
  snapshot date for daily products) is visible from the next weekday after STATPERS at 12:00 America/New_York
  (publication lag, ``clock_basis = publication_lag``). ``vendor_snapshot_at`` = STATPERS 23:59:59 ET.
* ``detail``: an estimate is visible from its activation (ACTDATS + ACTTIMS ET: its entry into the vendor
  database), never from ANNDATS (the analyst's date, often earlier) or REVDATS (a later confirmation). A missing
  ACTTIMS takes 23:59:59 ET of ACTDATS (``floor``); a missing ACTDATS rejects the row.
* ``actuals``, ``recommendations``, ``price_targets``: ``greatest(activation, announcement)``.
* Vendor as-of backfills (restated consensus, re-keyed history) arrive in ``backfill`` files and are clocked at
  delivery by the contract; only files the vendor attests as-first-published are ``pit_archive``.

Reused from v2 ``atx_db.estimates`` (sound, ported so this package does not import the retired warehouse):
``IBES_MEASURE_MAP``, the EPS PDF rule (P -> basic, D -> diluted), the FPI -> period-type map, the
``RECOMMENDATION_TEXT_MAP`` / ``RECOMMENDATION_LABELS`` 1-5 scale and the upgrade/downgrade rule, and the column
aliases. Superseded: the hash-of-symbol ``security_id`` (now TickerHistory3 ids via CUSIP/ticker history with
``link_tier``), the STATPERS end-of-day consensus clock (now + publication lag), the ``now()`` clock for rows
without one (now the delivery clock), and EPS without PDF defaulting to diluted (now ``EPS``: the majority basis).
"""

from __future__ import annotations

import datetime as dt
from pathlib import Path
from typing import Any, ClassVar

import duckdb
import pyarrow as pa

from .contract import Adapter, Substitute, TableSpec, ValidationReport, csv_write, write_receipt
from .mock import UNKNOWN_CUSIP, UNKNOWN_TICKER, MockUniverse

# --- ported from atx_db.estimates._columns / _common (v2) ----------------------------------------------------------
IBES_MEASURE_MAP = {"SAL": "REVENUE", "SALES": "REVENUE", "REV": "REVENUE", "NET": "NET_INCOME", "INC": "NET_INCOME",
                    "NI": "NET_INCOME", "OPR": "OPERATING_INCOME"}
FPI_PERIOD_TYPE = {"0": "LTG", "1": "FY", "2": "FY", "3": "FY", "4": "FY", "5": "FY", "6": "FQ", "7": "FQ",
                   "8": "FQ", "9": "FQ", "A": "SEMI", "B": "SEMI", "Y": "YTD"}
FISCALP_PERIOD_TYPE = {"ANN": "FY", "QTR": "FQ", "SAN": "SEMI", "LTG": "LTG"}
RECOMMENDATION_LABELS = {1: "Strong Buy", 2: "Buy", 3: "Hold", 4: "Underperform", 5: "Sell"}
RECOMMENDATION_TEXT_MAP = {
    "ACCUMULATE": 2, "ADD": 2, "BUY": 2, "EQUAL-WEIGHT": 3, "EQUAL WEIGHT": 3, "HOLD": 3, "MARKET PERFORM": 3,
    "MARKET-PERFORM": 3, "NEUTRAL": 3, "OUTPERFORM": 2, "OUTPERFORMER": 2, "OVERWEIGHT": 2, "REDUCE": 4,
    "SECTOR PERFORM": 3, "SECTOR-PERFORM": 3, "SELL": 5, "STRONG BUY": 1, "STRONG-BUY": 1, "STRONG SELL": 5,
    "STRONG-SELL": 5, "UNDERPERFORM": 4, "UNDERPERFORMER": 4, "UNDERWEIGHT": 4,
}
CONSENSUS_LAG_HOUR_ET = 12
ET = "America/New_York"

_ID = {"ticker": "vendor_security_id", "ibes_ticker": "vendor_security_id", "ibesticker": "vendor_security_id",
       "oftic": "ticker", "tic": "ticker", "official_ticker": "ticker", "symbol": "ticker",
       "measure_code": "measure", "curr": "currency", "curcode": "currency", "estcur": "currency",
       "currency_code": "currency", "curr_act": "currency", "fpedats": "period_end", "pends": "period_end",
       "fiscal_period_end": "period_end", "period_end_date": "period_end",
       "anndats": "announce_date", "anntims": "announce_time", "actdats": "activation_date",
       "acttims": "activation_time", "actual_activation_date": "activation_date",
       "revdats": "revision_date", "revtims": "revision_time"}
CONSENSUS_ALIASES = {**_ID, "statpers": "consensus_date", "snapshot_date": "consensus_date",
                     "consensus_date": "consensus_date", "asof": "consensus_date", "numest": "num_estimates",
                     "num_est": "num_estimates", "numup": "num_up", "num_up_30d": "num_up", "numdown": "num_down",
                     "num_down_30d": "num_down", "medest": "median", "median_est": "median", "meanest": "mean",
                     "mean_est": "mean", "stdev_est": "stdev", "stddev": "stdev", "highest": "high",
                     "high_est": "high", "lowest": "low", "low_est": "low"}
DETAIL_ALIASES = {**_ID, "estimator": "broker_id", "broker_code": "broker_id", "brokerid": "broker_id",
                  "analys": "analyst_id", "analyst_code": "analyst_id", "analystid": "analyst_id",
                  "estimate": "value", "est_value": "value"}
ACTUALS_ALIASES = {**_ID, "pdicity": "pdicity", "periodicity": "pdicity"}
RECS_ALIASES = {**_ID, "estimid": "broker_id", "estimator": "broker_id", "amaskcd": "analyst_id",
                "analys": "analyst_id", "analyst": "analyst_name", "ireccd": "rec_code", "recd": "rec_code",
                "rating_code": "rec_code", "itext": "rec_text", "recommendation_text": "rec_text",
                "ereccd": "broker_rec_code", "etext": "broker_rec_text"}
PT_ALIASES = {**_ID, "estimid": "broker_id", "estimator": "broker_id", "amaskcd": "analyst_id",
              "analys": "analyst_id", "alysnam": "analyst_name", "analyst": "analyst_name",
              "horizon": "horizon_months", "target": "value", "target_price": "value", "ptg": "value"}

_IDS = ("vendor_security_id", "cusip", "ticker")
_S = pa.string()
_D = pa.date32()
_F = pa.float64()
_I = pa.int64()


def _f(name: str, typ: pa.DataType) -> pa.Field:
    return pa.field(name, typ)


def _case(expr: str, mapping: dict[Any, Any], default: str = "NULL") -> str:
    def lit(v: Any) -> str:
        return str(v) if isinstance(v, int) else "'" + str(v).replace("'", "''") + "'"
    return "CASE " + expr + " " + " ".join(f"WHEN {lit(k)} THEN {lit(v)}" for k, v in mapping.items()) + f" ELSE {default} END"


def measure_sql(measure: str, pdf: str) -> str:
    """EPS + PDF P -> EPS_BASIC, D -> EPS_DILUTED, none -> EPS (majority basis); other codes via IBES_MEASURE_MAP."""
    m = f"upper(lic_str({measure}))"
    return (f"CASE WHEN {m} = 'EPS' THEN CASE upper(coalesce(lic_str({pdf}), '')) WHEN 'P' THEN 'EPS_BASIC' "
            f"WHEN 'D' THEN 'EPS_DILUTED' ELSE 'EPS' END ELSE {_case(m, IBES_MEASURE_MAP, m)} END")


_REC_TEXT_NORM = {k.replace("-", " "): v for k, v in RECOMMENDATION_TEXT_MAP.items()}


def rec_code_sql(code: str, text: str) -> str:
    """Numeric 1-5 code, else the analyst/broker text through ``RECOMMENDATION_TEXT_MAP`` (v2 rule; ``-``/``_``
    and runs of spaces normalised to one space)."""
    txt = f"regexp_replace(upper(lic_str({text})), '[\\s_-]+', ' ', 'g')"
    return (f"coalesce(CASE WHEN lic_int({code}) BETWEEN 1 AND 5 THEN lic_int({code}) END, "
            f"{_case(txt, _REC_TEXT_NORM)})")


def quarter_end(y: int, m: int) -> dt.date:
    qm = ((m - 1) // 3 + 1) * 3
    return dt.date(y, 12, 31) if qm == 12 else dt.date(y, qm + 1, 1) - dt.timedelta(days=1)


def _ts(d: str, t: str) -> str:
    return f"lic_utc(lic_date({d}), coalesce(lic_time({t}), TIME '23:59:59'), '{ET}')"


CONSENSUS = TableSpec(
    name="consensus",
    payload=(_f("measure_code", _S), _f("fpi", _S), _f("period_type", _S), _f("period_end", _D),
             _f("fiscal_year", _I), _f("consensus_date", _D), _f("num_estimates", _I), _f("num_up", _I),
             _f("num_down", _I), _f("mean", _F), _f("median", _F), _f("stdev", _F), _f("high", _F), _f("low", _F),
             _f("currency", _S)),
    key=("vendor_security_id", "measure_code", "period_type", "period_end", "consensus_date"),
    series=("vendor_security_id", "measure_code", "period_type", "period_end"),
    raw_glob="statsum*.csv",
    raw_columns=(*_IDS, "consensus_date", "measure", "fiscalp", "fpi", "period_end", "num_estimates", "num_up",
                 "num_down", "median", "mean", "stdev", "high", "low", "currency", "pdf"),
    aliases=CONSENSUS_ALIASES, stale_days=45,
    pit_rule="visible from the weekday after STATPERS at 12:00 America/New_York; snapshot = STATPERS 23:59:59 ET",
)
DETAIL = TableSpec(
    name="detail",
    payload=(_f("measure_code", _S), _f("fpi", _S), _f("period_type", _S), _f("period_end", _D),
             _f("fiscal_year", _I), _f("broker_id", _S), _f("analyst_id", _S), _f("value", _F), _f("currency", _S),
             _f("announce_date", _D), _f("revision_date", _D)),
    key=("vendor_security_id", "broker_id", "analyst_id", "measure_code", "period_type", "period_end",
         "vendor_snapshot_at"),
    series=("vendor_security_id", "broker_id", "analyst_id", "measure_code", "period_type", "period_end"),
    raw_glob="det*.csv",
    raw_columns=(*_IDS, "broker_id", "analyst_id", "pdf", "fpi", "measure", "value", "period_end", "revision_date",
                 "revision_time", "announce_date", "announce_time", "activation_date", "activation_time", "currency"),
    aliases=DETAIL_ALIASES, stale_days=105,
    pit_rule="visible from activation ACTDATS+ACTTIMS ET (missing time: 23:59:59 ET, floor); never ANNDATS/REVDATS",
)
ACTUALS = TableSpec(
    name="actuals",
    payload=(_f("measure_code", _S), _f("period_type", _S), _f("period_end", _D), _f("fiscal_year", _I),
             _f("value", _F), _f("currency", _S), _f("announce_at", pa.timestamp("us"))),
    key=("vendor_security_id", "measure_code", "period_type", "period_end", "vendor_snapshot_at"),
    series=("vendor_security_id", "measure_code", "period_type", "period_end"),
    raw_glob="act*.csv",
    raw_columns=(*_IDS, "measure", "pdicity", "period_end", "value", "announce_date", "announce_time",
                 "activation_date", "activation_time", "currency", "pdf"),
    aliases=ACTUALS_ALIASES, stale_days=None,
    pit_rule="visible from greatest(activation, announcement) ET; a restated actual is a new vintage row",
)
RECOMMENDATIONS = TableSpec(
    name="recommendations",
    payload=(_f("broker_id", _S), _f("analyst_id", _S), _f("analyst_name", _S), _f("rec_code", _I),
             _f("rec_label", _S), _f("rec_text", _S), _f("broker_rec_text", _S), _f("prior_rec_code", _I),
             _f("action", _S), _f("announce_date", _D), _f("revision_date", _D)),
    key=("vendor_security_id", "broker_id", "analyst_id", "vendor_snapshot_at"),
    series=("vendor_security_id", "broker_id", "analyst_id"),
    raw_glob="recd*.csv",
    raw_columns=(*_IDS, "broker_id", "analyst_id", "analyst_name", "rec_code", "rec_text", "broker_rec_code",
                 "broker_rec_text", "announce_date", "announce_time", "activation_date", "activation_time",
                 "revision_date"),
    aliases=RECS_ALIASES, stale_days=365,
    pit_rule="visible from greatest(activation, announcement) ET; prior code and action from earlier rows only",
)
PRICE_TARGETS = TableSpec(
    name="price_targets",
    payload=(_f("broker_id", _S), _f("analyst_id", _S), _f("analyst_name", _S), _f("horizon_months", _I),
             _f("target", _F), _f("currency", _S), _f("prior_target", _F), _f("announce_date", _D)),
    key=("vendor_security_id", "broker_id", "analyst_id", "horizon_months", "vendor_snapshot_at"),
    series=("vendor_security_id", "broker_id", "analyst_id", "horizon_months"),
    raw_glob="ptg*.csv",
    raw_columns=(*_IDS, "broker_id", "analyst_id", "analyst_name", "horizon_months", "value", "currency",
                 "announce_date", "announce_time", "activation_date", "activation_time"),
    aliases=PT_ALIASES, stale_days=365,
    pit_rule="visible from greatest(activation, announcement) ET",
)


class EstimatesAdapter(Adapter):
    name = "estimates"
    product = ("I/B/E/S History (LSEG) via WRDS: statsum, det, act, recddet, ptgdet (US); FactSet Estimates "
               "and Zacks accepted through the column aliases")
    pit_rule = "estimates-pit-v1: consensus = STATPERS + next-weekday-noon-ET lag; detail/actuals/recs/targets = activation clock"
    purchase = ("D3 decision point 1 (highest research value): I/B/E/S or FactSet Estimates, US, detail + summary + "
                "actuals + recommendations + targets, history from 2015 (5-year revision lookback for the 2020+ "
                "score window), daily delivery. Acceptance: this adapter's contract tests on the real files.")
    tables: ClassVar[dict[str, TableSpec]] = {s.name: s for s in (CONSENSUS, DETAIL, ACTUALS, RECOMMENDATIONS, PRICE_TARGETS)}

    def table_sql(self, table: str, raw: str) -> str:
        ids = ("lic_str(cusip) AS cusip, lic_str(ticker) AS ticker, upper(lic_str(vendor_security_id)) "
               "AS vendor_security_id, CAST(NULL AS BIGINT) AS native_security_id")
        ptype_fpi = _case("upper(lic_str(fpi))", FPI_PERIOD_TYPE)
        act = _ts("activation_date", "activation_time")
        ann = _ts("announce_date", "announce_time")
        if table == "consensus":
            return f"""
                SELECT _file, {ids}, lic_date(consensus_date) AS id_date,
                       {measure_sql('measure', 'pdf')} AS measure_code, upper(lic_str(fpi)) AS fpi,
                       coalesce({_case('upper(lic_str(fiscalp))', FISCALP_PERIOD_TYPE)}, {ptype_fpi}) AS period_type,
                       lic_date(period_end) AS period_end, year(lic_date(period_end)) AS fiscal_year,
                       lic_date(consensus_date) AS consensus_date, lic_int(num_estimates) AS num_estimates,
                       lic_int(num_up) AS num_up, lic_int(num_down) AS num_down, lic_num(mean) AS mean,
                       lic_num(median) AS median, lic_num(stdev) AS stdev, lic_num(high) AS high, lic_num(low) AS low,
                       upper(lic_str(currency)) AS currency,
                       lic_utc(lic_date(consensus_date), TIME '23:59:59', '{ET}') AS vendor_snapshot_at,
                       lic_utc(lic_next_weekday(lic_date(consensus_date)), TIME '{CONSENSUS_LAG_HOUR_ET:02d}:00:00',
                               '{ET}') AS rule_available_at,
                       'publication_lag' AS rule_basis, false AS rule_vintage_risk,
                       CASE WHEN lic_date(consensus_date) IS NULL THEN 'missing_statpers'
                            WHEN lic_date(period_end) IS NULL THEN 'missing_period_end'
                            WHEN lic_str(measure) IS NULL THEN 'missing_measure'
                            WHEN coalesce(lic_num(mean), lic_num(median), lic_int(num_estimates)) IS NULL
                                 THEN 'missing_statistic' END AS _reject
                FROM {raw}"""
        if table == "detail":
            return f"""
                SELECT _file, {ids}, coalesce(lic_date(announce_date), lic_date(activation_date)) AS id_date,
                       {measure_sql('measure', 'pdf')} AS measure_code, upper(lic_str(fpi)) AS fpi,
                       {ptype_fpi} AS period_type, lic_date(period_end) AS period_end,
                       year(lic_date(period_end)) AS fiscal_year, lic_str(broker_id) AS broker_id,
                       lic_str(analyst_id) AS analyst_id, lic_num(value) AS value, upper(lic_str(currency)) AS currency,
                       lic_date(announce_date) AS announce_date, lic_date(revision_date) AS revision_date,
                       {act} AS vendor_snapshot_at, {act} AS rule_available_at,
                       CASE WHEN lic_time(activation_time) IS NULL THEN 'floor' ELSE 'vendor_pit' END AS rule_basis,
                       lic_time(activation_time) IS NULL AS rule_vintage_risk,
                       CASE WHEN lic_date(activation_date) IS NULL THEN 'missing_activation'
                            WHEN lic_date(period_end) IS NULL THEN 'missing_period_end'
                            WHEN lic_num(value) IS NULL THEN 'missing_value'
                            WHEN lic_str(measure) IS NULL THEN 'missing_measure' END AS _reject
                FROM {raw}"""
        if table == "actuals":
            ptype = _case("upper(lic_str(pdicity))", FISCALP_PERIOD_TYPE)
            return f"""
                SELECT _file, {ids}, coalesce(lic_date(announce_date), lic_date(activation_date)) AS id_date,
                       {measure_sql('measure', 'pdf')} AS measure_code, {ptype} AS period_type,
                       lic_date(period_end) AS period_end, year(lic_date(period_end)) AS fiscal_year,
                       lic_num(value) AS value, upper(lic_str(currency)) AS currency, {ann} AS announce_at,
                       {act} AS vendor_snapshot_at, greatest({act}, {ann}) AS rule_available_at,
                       CASE WHEN lic_time(activation_time) IS NULL THEN 'floor' ELSE 'vendor_pit' END AS rule_basis,
                       false AS rule_vintage_risk,
                       CASE WHEN lic_date(activation_date) IS NULL THEN 'missing_activation'
                            WHEN lic_date(period_end) IS NULL THEN 'missing_period_end'
                            WHEN lic_num(value) IS NULL THEN 'missing_value' END AS _reject
                FROM {raw}"""
        if table == "recommendations":
            code = rec_code_sql("rec_code", "coalesce(lic_str(rec_text), lic_str(broker_rec_text))")
            return f"""
                WITH r AS (
                    SELECT *, {code} AS _code, {act} AS _act, {ann} AS _ann,
                           upper(lic_str(vendor_security_id)) AS _v, lic_str(broker_id) AS _b, lic_str(analyst_id) AS _a
                    FROM {raw}),
                s AS (SELECT *, lag(_code) OVER (PARTITION BY _v, _b, _a ORDER BY _act) AS _prior FROM r)
                SELECT _file, {ids}, coalesce(lic_date(announce_date), lic_date(activation_date)) AS id_date,
                       _b AS broker_id, _a AS analyst_id, lic_str(analyst_name) AS analyst_name, _code AS rec_code,
                       {_case('_code', RECOMMENDATION_LABELS)} AS rec_label, lic_str(rec_text) AS rec_text,
                       lic_str(broker_rec_text) AS broker_rec_text, _prior AS prior_rec_code,
                       CASE WHEN _prior IS NULL THEN 'INITIATE' WHEN _code < _prior THEN 'UPGRADE'
                            WHEN _code > _prior THEN 'DOWNGRADE' ELSE 'REITERATE' END AS action,
                       lic_date(announce_date) AS announce_date, lic_date(revision_date) AS revision_date,
                       _act AS vendor_snapshot_at, greatest(_act, _ann) AS rule_available_at,
                       CASE WHEN lic_time(activation_time) IS NULL THEN 'floor' ELSE 'vendor_pit' END AS rule_basis,
                       false AS rule_vintage_risk,
                       CASE WHEN _act IS NULL THEN 'missing_activation' WHEN _code IS NULL THEN 'unknown_rating' END AS _reject
                FROM s"""
        if table == "price_targets":
            return f"""
                WITH r AS (
                    SELECT *, {act} AS _act, {ann} AS _ann, lic_num(value) AS _t, lic_int(horizon_months) AS _h,
                           upper(lic_str(vendor_security_id)) AS _v, lic_str(broker_id) AS _b, lic_str(analyst_id) AS _a
                    FROM {raw}),
                s AS (SELECT *, lag(_t) OVER (PARTITION BY _v, _b, _a, _h ORDER BY _act) AS _prior FROM r)
                SELECT _file, {ids}, coalesce(lic_date(announce_date), lic_date(activation_date)) AS id_date,
                       _b AS broker_id, _a AS analyst_id, lic_str(analyst_name) AS analyst_name,
                       _h AS horizon_months, _t AS target, upper(lic_str(currency)) AS currency,
                       _prior AS prior_target, lic_date(announce_date) AS announce_date,
                       _act AS vendor_snapshot_at, greatest(_act, _ann) AS rule_available_at,
                       CASE WHEN lic_time(activation_time) IS NULL THEN 'floor' ELSE 'vendor_pit' END AS rule_basis,
                       false AS rule_vintage_risk,
                       CASE WHEN _act IS NULL THEN 'missing_activation' WHEN _t IS NULL OR _t <= 0 THEN 'bad_target'
                       END AS _reject
                FROM s"""
        raise KeyError(table)

    def extra_checks(self, con: duckdb.DuckDBPyConnection, table: str, report: ValidationReport) -> None:
        if table == "consensus":
            report.add(table, "statistics_ordered", con.execute("""
                SELECT count(*) FROM t_consensus WHERE low > high OR mean < low - 1e-9 OR mean > high + 1e-9
                    OR median < low - 1e-9 OR median > high + 1e-9 OR stdev < 0 OR num_estimates < 1""").fetchone()[0],
                fatal=False)
        if table == "recommendations":
            report.add(table, "rec_code_domain", con.execute(
                "SELECT count(*) FROM t_recommendations WHERE rec_code NOT BETWEEN 1 AND 5").fetchone()[0])

    def substitute(self) -> Substitute:
        return Substitute(
            stage="events/guidance.parquet", owner="EVT (S6.4)", keys=("cik", "available_at"),
            columns={"measure_code": "measure", "period_type": "period_type", "period_end": "period_end",
                     "low": "low", "high": "high", "mean": "mid"},
            note=("Management guidance ranges parsed from 8-K EX-99 releases stand in for consensus; the time-series "
                  "SUE (panel `sue`) stands in for the consensus surprise. No free substitute for detail, "
                  "recommendations or price targets."))

    # -- mock -------------------------------------------------------------------------------------------------------
    def mock(self, raw_dir: Path, universe: MockUniverse | None = None, seed: int = 0) -> list[Path]:
        """I/B/E/S-layout files for 2023-01..2024-12 plus the edge cases the contract tests assert on."""
        u = universe or MockUniverse()
        rng = u.rng
        raw_dir = Path(raw_dir)
        raw_dir.mkdir(parents=True, exist_ok=True)
        months = [(y, m) for y in (2023, 2024) for m in range(1, 13)]
        brokers = [("1001", "A0001", "SMITH J"), ("1002", "A0002", "DOE K"), ("1003", "A0003", "LEE M")]
        cons, det, acts, recs, ptgs = [], [], [], [], []

        def idcols(ln, d):
            return [f"I{ln.security_id:04d}", (ln.cusip_on(d) or "")[:8], ln.ticker_on(d)]

        for ln in u.lines:
            base_eps, base_sal = round(rng.uniform(0.4, 3.0), 2), round(rng.uniform(150, 4000), 1)
            for y, m in months:
                sp = u.third_thursday(y, m)
                if not ln.alive(sp):
                    continue
                for measure, level in (("EPS", base_eps), ("SAL", base_sal)):
                    for fiscalp, fpi, pend in (("ANN", "1", dt.date(y, 12, 31)), ("QTR", "6", quarter_end(y, m))):
                        scale = 1.0 if fiscalp == "ANN" else 0.25
                        mean = round(level * scale * (1 + 0.01 * (m - 6)), 4)
                        n = rng.randint(3, 12)
                        sd = round(abs(mean) * 0.05, 4)
                        cons.append([*idcols(ln, sp), sp.isoformat(), measure, fiscalp, fpi, pend.isoformat(), n,
                                    rng.randint(0, 2), rng.randint(0, 2), mean, mean, sd, round(mean + 2 * sd, 4),
                                    round(mean - 2 * sd, 4), "USD"])
            for q, (b, a, _nm) in enumerate(brokers):
                for k, (y, m) in enumerate(months[::3]):
                    d = dt.date(y, m, 5 + q)
                    if not ln.alive(d):
                        continue
                    act_d = d + dt.timedelta(days=1)
                    det.append([*idcols(ln, d), b, a, "D", "1", "EPS", round(base_eps * (1 + 0.02 * k), 3),
                               dt.date(y, 12, 31).isoformat(), None, None, d.isoformat(), "08:15:00",
                               act_d.isoformat(), f"{9 + q:02d}:30:00", "USD"])
            for y, m in months[::3]:
                pend = dt.date(y, m, 1) - dt.timedelta(days=1)
                ann = pend + dt.timedelta(days=30)
                if not ln.alive(ann):
                    continue
                acts.append([*idcols(ln, ann), "EPS", "QTR", pend.isoformat(), round(base_eps / 4, 3),
                            ann.isoformat(), "16:05:00", ann.isoformat(), "18:40:00", "USD"])
            codes = [rng.randint(1, 5) for _ in range(4)]
            for k, (b, a, nm) in enumerate(brokers):
                for j, code in enumerate(codes[k:k + 2]):
                    d = dt.date(2023 + j, 2 + 3 * k, 10)
                    if ln.alive(d):
                        recs.append([*idcols(ln, d), b, a, nm, code, None, None, None, d.isoformat(), "07:00:00",
                                    d.isoformat(), "07:45:00", None])
                        ptgs.append([*idcols(ln, d), b, a, nm, 12, round(base_eps * 20 * (1 + 0.1 * j), 2), "USD",
                                    d.isoformat(), "07:00:00", d.isoformat(), "07:45:00"])
        # edge rows: unknown identifiers (unmapped), ticker-only rows across the REUS reuse (ticker_dated),
        # a restated actual (second vintage), a detail row without ACTTIMS (floor) and one without ACTDATS (reject)
        sp = u.third_thursday(2024, 3)
        cons.append(["IUNK", UNKNOWN_CUSIP[:8], UNKNOWN_TICKER, sp.isoformat(), "EPS", "ANN", "1", "2024-12-31",
                     4, 0, 0, 1.0, 1.0, 0.1, 1.2, 0.8, "USD"])
        for d in (dt.date(2020, 6, 18), dt.date(2023, 6, 15)):
            cons.append(["IREUS", "", "REUS", d.isoformat(), "EPS", "ANN", "1", f"{d.year}-12-31",
                         5, 1, 0, 2.0, 2.0, 0.1, 2.2, 1.8, "USD"])
        ln2 = u.lines[2]  # the delisted line keeps its pre-delisting history
        for m in range(1, 7):
            d = u.third_thursday(2022, m)
            cons.append([f"I{ln2.security_id:04d}", ln2.cusip_on(d)[:8], ln2.ticker_on(d), d.isoformat(), "EPS", "ANN",
                         "1", "2022-12-31", 6, 1, 1, 1.5, 1.5, 0.1, 1.7, 1.3, "USD"])
        ln1 = u.lines[1]  # pre-reorganisation CUSIP: dated in 2021, still quoted by the vendor in 2023 (undated)
        for d in (dt.date(2021, 9, 16), dt.date(2023, 3, 16)):
            cons.append([f"I{ln1.security_id:04d}", ln1.cusips[0][0][:8], "", d.isoformat(), "EPS", "ANN", "2",
                         f"{d.year + 1}-12-31", 5, 1, 0, 2.0, 2.0, 0.1, 2.2, 1.8, "USD"])
        ln = u.lines[5]
        pend, ann = dt.date(2024, 6, 30), dt.date(2024, 7, 30)
        acts.append([f"I{ln.security_id:04d}", ln.cusips[0][0][:8], ln.ticker_on(ann), "EPS", "QTR", pend.isoformat(),
                     0.123, ann.isoformat(), "16:05:00", "2024-09-03", "10:00:00", "USD"])
        det.append([f"I{ln.security_id:04d}", ln.cusips[0][0][:8], ln.ticker_on(ann), "1009", "A0009", "P", "6",
                    "EPS", 0.5, pend.isoformat(), None, None, "2024-05-01", None, "2024-05-02", None, "USD"])
        det.append([f"I{ln.security_id:04d}", ln.cusips[0][0][:8], ln.ticker_on(ann), "1009", "A0009", "P", "6",
                    "EPS", 0.6, pend.isoformat(), None, None, "2024-05-20", None, None, None, "USD"])
        cons_h = ["TICKER", "CUSIP", "OFTIC", "STATPERS", "MEASURE", "FISCALP", "FPI", "FPEDATS", "NUMEST", "NUMUP",
                  "NUMDOWN", "MEANEST", "MEDEST", "STDEV", "HIGHEST", "LOWEST", "CURCODE"]
        det_h = ["TICKER", "CUSIP", "OFTIC", "ESTIMATOR", "ANALYS", "PDF", "FPI", "MEASURE", "VALUE", "FPEDATS",
                 "REVDATS", "REVTIMS", "ANNDATS", "ANNTIMS", "ACTDATS", "ACTTIMS", "CURR"]
        act_h = ["TICKER", "CUSIP", "OFTIC", "MEASURE", "PDICITY", "PENDS", "VALUE", "ANNDATS", "ANNTIMS", "ACTDATS",
                 "ACTTIMS", "CURR_ACT"]
        rec_h = ["TICKER", "CUSIP", "OFTIC", "ESTIMID", "AMASKCD", "ANALYST", "IRECCD", "ITEXT", "ERECCD", "ETEXT",
                 "ANNDATS", "ANNTIMS", "ACTDATS", "ACTTIMS", "REVDATS"]
        ptg_h = ["TICKER", "CUSIP", "OFTIC", "ESTIMID", "AMASKCD", "ALYSNAM", "HORIZON", "VALUE", "ESTCUR", "ANNDATS",
                 "ANNTIMS", "ACTDATS", "ACTTIMS"]
        fetched = dt.datetime(2026, 7, 1, 6, 0)
        paths = [csv_write(raw_dir / "statsum_epsus.csv", cons_h, cons), csv_write(raw_dir / "det_epsus.csv", det_h, det),
                 csv_write(raw_dir / "act_epsus.csv", act_h, acts), csv_write(raw_dir / "recddet.csv", rec_h, recs),
                 csv_write(raw_dir / "ptgdet.csv", ptg_h, ptgs)]
        for p in paths:
            write_receipt(raw_dir, p, fetched_at=fetched, history_mode="pit_archive", url=f"mock://ibes/{p.name}",
                          http_status=200)
        # a vendor restated-consensus backfill (as-of history re-cut in 2026): delivered, never historical
        ln0 = u.lines[7]
        sp = u.third_thursday(2023, 5)
        bf = csv_write(raw_dir / "statsum_restated.csv", cons_h,
                       [[f"I{ln0.security_id:04d}", ln0.cusips[0][0][:8], ln0.ticker_on(sp), sp.isoformat(), "EPS",
                         "ANN", "1", "2025-12-31", 6, 0, 0, 9.99, 9.99, 0.1, 10.2, 9.8, "USD"]])
        write_receipt(raw_dir, bf, fetched_at=dt.datetime(2026, 6, 30, 12, 0), history_mode="backfill",
                      url="mock://ibes/restated", http_status=200)
        return [*paths, bf]


ADAPTER = EstimatesAdapter()
