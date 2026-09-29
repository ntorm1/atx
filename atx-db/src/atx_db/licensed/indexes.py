"""Index constituents and weights adapter: S&P 500 / S&P 1500, Russell 1000 / 2000 / 3000 (S8.1).

Raw layout: ``index_constituents*.csv`` holds membership spells as the index provider's constituent-change history
publishes them (INDEX_CODE, CUSIP, TICKER, EFFECTIVE_DATE, END_DATE, ANNOUNCE_DATE, ANNOUNCE_TIME ET);
``index_weights*.csv`` holds the end-of-day constituent files (INDEX_CODE, DATE, CUSIP, TICKER, INDEX_SHARES, IWF,
CLOSE_PRICE, WEIGHT as a fraction or in percent). S&P DJI SDL and FTSE Russell files differ in spelling only.

PIT rule ``index-pit-v1``:

* ``constituents``: a spell is known at its announcement (ANNOUNCE_DATE + ANNOUNCE_TIME ET; 23:59:59 ET when the
  time is missing) and applies to sessions ``effective_from <= d < effective_to``. Without an announcement it is
  known at the effective date's 09:30 ET open (``floor``, ``vintage_risk``). Consumers need both conditions, so an
  announced addition is usable for event studies before it is a member.
* ``weights``: the close file of date D (``vendor_snapshot_at`` = D 16:00 ET) is visible at D 23:59:59 ET
  (evening delivery, ``publication_lag``). Weights are normalized to fractions per (index, date).
* Survivorship: spells of delisted names are kept with their end date; a provider history that drops dead names or
  re-keys them to today's identifiers shows up as ``unmapped`` / ``cusip_undated`` rows in the report.
"""

from __future__ import annotations

import datetime as dt
from pathlib import Path
from typing import ClassVar

import duckdb
import pyarrow as pa

from .contract import Adapter, Substitute, TableSpec, ValidationReport, csv_write, write_receipt
from .mock import UNKNOWN_CUSIP, UNKNOWN_TICKER, MockUniverse

ET = "America/New_York"
CONS_ALIASES = {"index_code": "index_id", "indexcode": "index_id", "index": "index_id", "index_id": "index_id",
                "effective_date": "effective_from", "start_date": "effective_from", "date_added": "effective_from",
                "end_date": "effective_to", "date_removed": "effective_to", "announce_date": "announce_date",
                "announcement_date": "announce_date", "announce_time": "announce_time", "symbol": "ticker"}
WEIGHT_ALIASES = {"index_code": "index_id", "indexcode": "index_id", "index": "index_id", "date": "as_of_date",
                  "effective_date": "as_of_date", "index_shares": "index_shares", "indexshares": "index_shares",
                  "iwf": "float_factor", "investable_weight_factor": "float_factor", "close_price": "close_price",
                  "price": "close_price", "weight": "weight", "index_weight": "weight", "symbol": "ticker"}
CONSTITUENTS = TableSpec(
    name="constituents",
    payload=(pa.field("index_id", pa.string()), pa.field("effective_from", pa.date32()),
             pa.field("effective_to", pa.date32()), pa.field("announced_at", pa.timestamp("us")),
             pa.field("announced_before_effective", pa.bool_())),
    key=("index_id", "cusip", "ticker", "effective_from", "vendor_snapshot_at"),
    series=("index_id", "cusip", "ticker", "effective_from"),
    raw_glob="index_constituents*.csv",
    raw_columns=("vendor_security_id", "cusip", "ticker", "index_id", "effective_from", "effective_to",
                 "announce_date", "announce_time"),
    aliases=CONS_ALIASES, stale_days=None,
    pit_rule="known at announcement (else effective_from 09:30 ET, floor); member for effective_from <= d < effective_to",
)
WEIGHTS = TableSpec(
    name="weights",
    payload=(pa.field("index_id", pa.string()), pa.field("as_of_date", pa.date32()),
             pa.field("index_shares", pa.float64()), pa.field("float_factor", pa.float64()),
             pa.field("close_price", pa.float64()), pa.field("weight", pa.float64())),
    key=("index_id", "cusip", "ticker", "as_of_date"),
    series=("index_id", "cusip", "ticker"),
    raw_glob="index_weights*.csv",
    raw_columns=("vendor_security_id", "cusip", "ticker", "index_id", "as_of_date", "index_shares", "float_factor",
                 "close_price", "weight"),
    aliases=WEIGHT_ALIASES, stale_days=5,
    pit_rule="close file of D visible at D 23:59:59 ET; weights normalized to fractions per (index, date)",
)


class IndexesAdapter(Adapter):
    name = "indexes"
    product = "S&P DJI (S&P 500/400/600/1500) and FTSE Russell (1000/2000/3000) constituent history and EOD weights"
    pit_rule = "index-pit-v1: announcement clock for spells, evening clock for EOD weights"
    purchase = ("D3 decision point 4: official constituents + weights for S&P 500 and Russell 3000, 2019+ (D7), daily. "
                "Needed for benchmark-relative risk and index-event studies; the rule-based proxies cover universe "
                "construction.")
    tables: ClassVar[dict[str, TableSpec]] = {"constituents": CONSTITUENTS, "weights": WEIGHTS}

    def table_sql(self, table: str, raw: str) -> str:
        ids = ("lic_str(cusip) AS cusip, lic_str(ticker) AS ticker, lic_str(vendor_security_id) AS vendor_security_id, "
               "CAST(NULL AS BIGINT) AS native_security_id")
        if table == "constituents":
            ann = f"lic_utc(lic_date(announce_date), coalesce(lic_time(announce_time), TIME '23:59:59'), '{ET}')"
            opn = f"lic_utc(lic_date(effective_from), TIME '09:30:00', '{ET}')"
            return f"""
                SELECT _file, {ids}, lic_date(effective_from) AS id_date, upper(lic_str(index_id)) AS index_id,
                       lic_date(effective_from) AS effective_from, lic_date(effective_to) AS effective_to,
                       {ann} AS announced_at, lic_date(announce_date) < lic_date(effective_from) AS announced_before_effective,
                       coalesce({ann}, {opn}) AS vendor_snapshot_at, coalesce({ann}, {opn}) AS rule_available_at,
                       CASE WHEN lic_date(announce_date) IS NULL THEN 'floor' ELSE 'vendor_pit' END AS rule_basis,
                       lic_date(announce_date) IS NULL AS rule_vintage_risk,
                       CASE WHEN lic_str(index_id) IS NULL THEN 'missing_index'
                            WHEN lic_date(effective_from) IS NULL THEN 'missing_effective_from'
                            WHEN lic_date(effective_to) < lic_date(effective_from) THEN 'inverted_spell' END AS _reject
                FROM {raw}"""
        if table == "weights":
            return f"""
                WITH r AS (SELECT *, upper(lic_str(index_id)) AS _ix, lic_date(as_of_date) AS _d, lic_num(weight) AS _w
                           FROM {raw})
                SELECT _file, {ids}, _d AS id_date, _ix AS index_id, _d AS as_of_date,
                       lic_num(index_shares) AS index_shares, lic_num(float_factor) AS float_factor,
                       lic_num(close_price) AS close_price,
                       CASE WHEN sum(_w) OVER (PARTITION BY _ix, _d) > 1.5 THEN _w / 100 ELSE _w END AS weight,
                       lic_utc(_d, TIME '16:00:00', '{ET}') AS vendor_snapshot_at,
                       lic_utc(_d, TIME '23:59:59', '{ET}') AS rule_available_at,
                       'publication_lag' AS rule_basis, false AS rule_vintage_risk,
                       CASE WHEN _ix IS NULL THEN 'missing_index' WHEN _d IS NULL THEN 'missing_date' END AS _reject
                FROM r"""
        raise KeyError(table)

    def extra_checks(self, con: duckdb.DuckDBPyConnection, table: str, report: ValidationReport) -> None:
        if table == "weights":
            report.add(table, "weights_sum_to_one", con.execute("""
                SELECT count(*) FROM (SELECT index_id, as_of_date, sum(weight) s FROM t_weights GROUP BY 1, 2)
                WHERE abs(s - 1) > 0.01""").fetchone()[0])
        if table == "constituents":
            report.add(table, "spells_overlap", con.execute("""
                SELECT count(*) FROM (
                    SELECT effective_from, lag(coalesce(effective_to, DATE '9999-12-31'))
                           OVER (PARTITION BY index_id, security_id ORDER BY effective_from) AS prev_to
                    FROM t_constituents WHERE security_id IS NOT NULL)
                WHERE effective_from < prev_to""").fetchone()[0], fatal=False)

    def substitute(self) -> Substitute:
        return Substitute(
            stage="indexes", owner="MKT (S3.6)", keys=("security_id",),
            columns={"index_id": "index_id", "effective_from": "effective_from", "weight": "weight"},
            note=("Rule-based Russell 1000/2000/3000 proxies (public methodology: rank day, bands) and an S&P 500 "
                  "change list from public releases where terms allow; CRSP-style VW/EW market returns."))

    def mock(self, raw_dir: Path, universe: MockUniverse | None = None, seed: int = 0) -> list[Path]:
        u = universe or MockUniverse()
        rng = u.rng
        raw_dir = Path(raw_dir)
        raw_dir.mkdir(parents=True, exist_ok=True)
        spells = []
        for ln in u.lines[:8]:  # S&P 500-style: members from the start, the delisted line leaves
            end = ln.last + dt.timedelta(days=1) if ln.last < dt.date(2026, 6, 30) else None
            spells.append(["SP500", ln.cusip_on(ln.first), ln.ticker_on(ln.first), ln.first.isoformat(),
                           end.isoformat() if end else None, None, None])
        add = u.lines[9]  # an announced addition: public 2024-06-07 17:15 ET, effective 2024-06-24
        spells.append(["SP500", add.cusip_on(dt.date(2024, 6, 24)), add.ticker_on(dt.date(2024, 6, 24)), "2024-06-24",
                       None, "2024-06-07", "17:15:00"])
        for y in (2023, 2024):  # Russell 2000-style annual reconstitution after the last Friday of June
            eff = dt.date(y, 6, 26) if y == 2023 else dt.date(y, 7, 1)
            for ln in u.lines[10:15]:
                if ln.alive(eff):
                    spells.append(["R2000", ln.cusip_on(eff), ln.ticker_on(eff), eff.isoformat(),
                                   (dt.date(y + 1, 6, 24) if y == 2023 else dt.date(y + 1, 6, 30)).isoformat(),
                                   f"{y}-06-{'09' if y == 2023 else '07'}", "18:00:00"])
        spells.append(["R2000", UNKNOWN_CUSIP, UNKNOWN_TICKER, "2024-07-01", None, "2024-06-07", "18:00:00"])
        cp = csv_write(raw_dir / "index_constituents.csv",
                       ["INDEX_CODE", "CUSIP", "TICKER", "EFFECTIVE_DATE", "END_DATE", "ANNOUNCE_DATE", "ANNOUNCE_TIME"],
                       spells)
        wrows = []
        for d in [dt.date(2024, m, 28) for m in range(1, 13)]:
            if d.weekday() >= 5:
                d -= dt.timedelta(days=d.weekday() - 4)
            members = [ln for ln in u.lines[:8] if ln.alive(d)] + ([add] if d >= dt.date(2024, 6, 24) else [])
            caps = [rng.uniform(5e9, 3e11) for _ in members]
            tot = sum(caps)
            for ln, cap in zip(members, caps, strict=True):
                px = round(rng.uniform(20, 500), 2)
                wrows.append(["SP500", d.isoformat(), ln.cusip_on(d), ln.ticker_on(d), round(cap / px), 1.0, px,
                              round(100 * cap / tot, 6)])
        wp = csv_write(raw_dir / "index_weights_sp500.csv",
                       ["INDEX_CODE", "DATE", "CUSIP", "TICKER", "INDEX_SHARES", "IWF", "CLOSE_PRICE", "WEIGHT"], wrows)
        for p in (cp, wp):
            write_receipt(raw_dir, p, fetched_at=dt.datetime(2026, 7, 1, 6, 0), history_mode="pit_archive",
                          url=f"mock://index/{p.name}", http_status=200)
        return [cp, wp]


ADAPTER = IndexesAdapter()
