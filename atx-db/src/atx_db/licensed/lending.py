"""Securities-lending adapter: fee, rebate, cost-of-borrow score, utilization, lendable, on-loan (S8.1).

Raw layout: one daily CSV per business date (``lending_*.csv``) in the S&P Global Market Intelligence Securities
Finance (ex IHS Markit) equity layout; the aliases also take DataLend / EquiLend and Hazeltree-style spellings. Field
names are to be confirmed against the vendor dictionary at purchase; the normalized schema below is the contract.

PIT rule ``lending-pit-v1``: the record for business date D describes end-of-day D positions and is published on the
next weekday (London morning). ``vendor_snapshot_at`` = D 23:59:59 America/New_York; ``available_at`` = the next
weekday after D at 12:00 UTC (``publication_lag``). Vendor history files re-cut after the fact (revised utilisation,
re-mapped instruments) are ``backfill`` deliveries and are clocked at delivery. Units: fee and rebate in bps per
annum, utilization in percent [0, 100], quantities in shares, values in USD.
"""

from __future__ import annotations

import datetime as dt
from pathlib import Path
from typing import ClassVar

import duckdb
import pyarrow as pa

from .contract import Adapter, Substitute, TableSpec, ValidationReport, csv_write, write_receipt
from .mock import UNKNOWN_CUSIP, MockUniverse, make_isin

ALIASES = {
    "businessdate": "data_date", "business_date": "data_date", "datadate": "data_date", "date": "data_date",
    "asofdate": "data_date", "ticker": "ticker", "symbol": "ticker", "instrumentname": "name",
    "indicativefee": "fee_bps", "saf": "fee_bps", "indicative_fee": "fee_bps", "fee": "fee_bps",
    "indicativerebate": "rebate_bps", "sar": "rebate_bps", "rebate": "rebate_bps",
    "dcbs": "cost_to_borrow_score", "dailycostofborrowscore": "cost_to_borrow_score",
    "utilisationbyquantity": "utilization_pct", "utilizationbyquantity": "utilization_pct",
    "utilisation": "utilization_pct", "utilization": "utilization_pct",
    "lendablequantity": "lendable_quantity", "lendablevalue": "lendable_value_usd",
    "activelendablequantity": "lendable_quantity", "quantityonloan": "on_loan_quantity",
    "onloanquantity": "on_loan_quantity", "valueonloan": "on_loan_value_usd", "onloanvalue": "on_loan_value_usd",
    "shortloanquantity": "short_loan_quantity", "borrowerconcentration": "borrower_concentration",
}
LENDING = TableSpec(
    name="lending_daily",
    payload=(pa.field("data_date", pa.date32()), pa.field("isin", pa.string()),
             pa.field("fee_bps", pa.float64()), pa.field("rebate_bps", pa.float64()),
             pa.field("cost_to_borrow_score", pa.int64()), pa.field("utilization_pct", pa.float64()),
             pa.field("lendable_quantity", pa.float64()), pa.field("lendable_value_usd", pa.float64()),
             pa.field("on_loan_quantity", pa.float64()), pa.field("on_loan_value_usd", pa.float64()),
             pa.field("short_loan_quantity", pa.float64()), pa.field("borrower_concentration", pa.float64())),
    key=("cusip", "isin", "ticker", "data_date"),
    series=("cusip", "isin", "ticker"),
    raw_glob="lending_*.csv",
    raw_columns=("vendor_security_id", "cusip", "isin", "ticker", "name", "data_date", "fee_bps", "rebate_bps",
                 "cost_to_borrow_score", "utilization_pct", "lendable_quantity", "lendable_value_usd",
                 "on_loan_quantity", "on_loan_value_usd", "short_loan_quantity", "borrower_concentration"),
    aliases=ALIASES, stale_days=5,
    pit_rule="business date D visible from the next weekday 12:00 UTC; snapshot = D 23:59:59 America/New_York",
)


class LendingAdapter(Adapter):
    name = "lending"
    product = "S&P Global Securities Finance (ex IHS Markit) equity daily file; DataLend/EquiLend accepted via aliases"
    pit_rule = "lending-pit-v1: business date D -> next weekday 12:00 UTC"
    purchase = ("D3 decision point 2: US equity lending daily file (fee/rebate, DCBS, utilisation, lendable, on-loan), "
                "history from 2019 (D7), daily delivery. Replaces the public borrow proxy for short-side "
                "constraints; acceptance = these contract tests on the real files plus >= 95% member_equity cells.")
    tables: ClassVar[dict[str, TableSpec]] = {"lending_daily": LENDING}

    def table_sql(self, table: str, raw: str) -> str:
        return f"""
            SELECT _file, lic_str(cusip) AS cusip, lic_str(ticker) AS ticker, lic_str(vendor_security_id) AS vendor_security_id,
                   CAST(NULL AS BIGINT) AS native_security_id, lic_date(data_date) AS id_date,
                   lic_date(data_date) AS data_date, upper(lic_str(isin)) AS isin,
                   lic_num(fee_bps) AS fee_bps, lic_num(rebate_bps) AS rebate_bps,
                   lic_int(cost_to_borrow_score) AS cost_to_borrow_score, lic_num(utilization_pct) AS utilization_pct,
                   lic_num(lendable_quantity) AS lendable_quantity, lic_num(lendable_value_usd) AS lendable_value_usd,
                   lic_num(on_loan_quantity) AS on_loan_quantity, lic_num(on_loan_value_usd) AS on_loan_value_usd,
                   lic_num(short_loan_quantity) AS short_loan_quantity,
                   lic_num(borrower_concentration) AS borrower_concentration,
                   lic_utc(lic_date(data_date), TIME '23:59:59', 'America/New_York') AS vendor_snapshot_at,
                   CAST(lic_next_weekday(lic_date(data_date)) + TIME '12:00:00' AS TIMESTAMP) AS rule_available_at,
                   'publication_lag' AS rule_basis, false AS rule_vintage_risk,
                   CASE WHEN lic_date(data_date) IS NULL THEN 'missing_date'
                        WHEN coalesce(lic_str(cusip), lic_str(isin), lic_str(ticker)) IS NULL THEN 'missing_identifier'
                   END AS _reject
            FROM {raw}"""

    def extra_checks(self, con: duckdb.DuckDBPyConnection, table: str, report: ValidationReport) -> None:
        report.add(table, "utilization_domain", con.execute(
            "SELECT count(*) FROM t_lending_daily WHERE utilization_pct < 0 OR utilization_pct > 100").fetchone()[0])
        report.add(table, "dcbs_domain", con.execute(
            "SELECT count(*) FROM t_lending_daily WHERE cost_to_borrow_score NOT BETWEEN 1 AND 10").fetchone()[0])
        report.add(table, "on_loan_le_lendable", con.execute(
            "SELECT count(*) FROM t_lending_daily WHERE on_loan_quantity > lendable_quantity * 1.0001").fetchone()[0],
            fatal=False)
        report.add(table, "fee_nonnegative", con.execute(
            "SELECT count(*) FROM t_lending_daily WHERE fee_bps < 0").fetchone()[0])

    def substitute(self) -> Substitute:
        return Substitute(
            stage="borrow_proxy", owner="controller (S0.1)", keys=("session_date", "security_id", "cutoff_utc"),
            columns={"utilization_pct": "si_to_io", "on_loan_quantity": "si_shares", "lendable_quantity": "inst_shares",
                     "cost_to_borrow_score": "on_threshold_list"},
            note=("Public short-side proxy: FINRA short interest over 13F institutional shares (utilisation), 13F "
                  "shares (lendable supply), Reg SHO threshold lists and FTDs (hard-to-borrow flags). No fee proxy."))

    def mock(self, raw_dir: Path, universe: MockUniverse | None = None, seed: int = 0) -> list[Path]:
        """Every fourth business date of 2024 (one file each) plus a backfill history file and an unknown CUSIP."""
        u = universe or MockUniverse()
        rng = u.rng
        raw_dir = Path(raw_dir)
        raw_dir.mkdir(parents=True, exist_ok=True)
        header = ["BusinessDate", "CUSIP", "ISIN", "Ticker", "InstrumentName", "IndicativeFee", "IndicativeRebate",
                  "DCBS", "UtilisationByQuantity", "LendableQuantity", "LendableValue", "QuantityOnLoan",
                  "ValueOnLoan", "ShortLoanQuantity"]
        paths = []
        for d in u.weekdays(dt.date(2024, 1, 2), dt.date(2024, 12, 31), step=4):
            rows = []
            for ln in u.lines:
                if not ln.alive(d):
                    continue
                cus = ln.cusip_on(d)
                lend = float(rng.randrange(1_000_000, 50_000_000))
                util = round(rng.uniform(0.5, 95.0), 2)
                fee = round(rng.choice([25, 30, 35, 50, 150, 800, 2500]) * rng.uniform(0.9, 1.1), 1)
                dcbs = 1 if fee < 50 else (3 if fee < 200 else (6 if fee < 1000 else 9))
                onloan = round(lend * util / 100)
                rows.append([d.isoformat(), cus, make_isin(cus), ln.ticker_on(d), ln.name, fee, round(430 - fee, 1),
                             dcbs, util, lend, lend * 20, onloan, onloan * 20, round(onloan * 0.9)])
            rows.append([d.isoformat(), UNKNOWN_CUSIP, make_isin(UNKNOWN_CUSIP), None, "UNKNOWN", 30, 400, 1, 1.0,
                         1e6, 2e7, 1e4, 2e5, 9e3])
            p = csv_write(raw_dir / f"lending_{d:%Y%m%d}.csv", header, rows)
            fetched = dt.datetime.combine(d + dt.timedelta(days=1 if d.weekday() < 4 else 3), dt.time(7, 5))
            write_receipt(raw_dir, p, fetched_at=fetched, history_mode="daily", url=f"mock://lending/{p.name}",
                          http_status=200)
            paths.append(p)
        ln = u.lines[2]  # a vendor history re-cut delivered in 2026 for a line delisted in 2022
        hp = csv_write(raw_dir / "lending_history_recut.csv", header,
                       [[d.isoformat(), ln.cusip_on(d), None, ln.ticker_on(d), ln.name, 40, 390, 2, 12.5, 5e6, 1e8,
                         625000, 1.25e7, 600000] for d in u.weekdays(dt.date(2021, 3, 1), dt.date(2021, 3, 5))])
        write_receipt(raw_dir, hp, fetched_at=dt.datetime(2026, 6, 30, 9, 0), history_mode="backfill",
                      url="mock://lending/history", http_status=200)
        return [*paths, hp]


ADAPTER = LendingAdapter()
