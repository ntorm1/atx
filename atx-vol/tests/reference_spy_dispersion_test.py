from __future__ import annotations

import csv
import importlib.util
from pathlib import Path
import tempfile
import unittest


SCRIPT = Path(__file__).parents[1] / "tools" / "reference_spy_dispersion.py"
SPEC = importlib.util.spec_from_file_location("reference_spy_dispersion", SCRIPT)
assert SPEC is not None and SPEC.loader is not None
REFERENCE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(REFERENCE)


SCHEDULE_FIELDS = [
    "roll_date", "valuation_ts_ns", "cohort", "expiry_ts_ns",
    "gross_index_vega_target", "net_vega", "gross_vega", "n_names",
    "is_index", "symbol", "uid", "instrument_id", "raw_symbol", "strike",
    "side", "quantity", "multiplier", "raw_bid", "raw_ask", "raw_mid",
    "model_mark", "delta_per_share", "vega_per_unit_vol",
    "vega_per_contract_per_vol_point", "normalized_weight",
    "target_straddle_vega", "achieved_leg_vega", "source_fingerprint",
    "surface_fingerprint",
]


def write_tsv(path: Path, fields: list[str], rows: list[dict[str, str]], magic: str | None = None) -> None:
    with path.open("w", encoding="utf-8", newline="") as stream:
        if magic is not None:
            stream.write(magic + "\n")
        writer = csv.DictWriter(stream, fieldnames=fields, delimiter="\t", lineterminator="\n")
        writer.writeheader()
        writer.writerows(rows)


def schedule_rows() -> list[dict[str, str]]:
    rows: list[dict[str, str]] = []
    specs = [
        ("1", "SPY", "1", "C", "-1", "0", "-100", "-50"),
        ("1", "SPY", "2", "P", "-1", "0", "-100", "-50"),
        ("0", "AAPL", "3", "C", "1", "1", "100", "50"),
        ("0", "AAPL", "4", "P", "1", "1", "100", "50"),
    ]
    for is_index, symbol, instrument_id, side, quantity, weight, target, achieved in specs:
        rows.append({
            "roll_date": "2026-07-10", "valuation_ts_ns": "100", "cohort": "1",
            "expiry_ts_ns": "100000", "gross_index_vega_target": "100",
            "net_vega": "0", "gross_vega": "200", "n_names": "1",
            "is_index": is_index, "symbol": symbol, "uid": "1" if is_index == "1" else "2",
            "instrument_id": instrument_id, "raw_symbol": f"{symbol}{instrument_id}",
            "strike": "100", "side": side, "quantity": quantity, "multiplier": "100",
            "raw_bid": "9", "raw_ask": "11", "raw_mid": "10", "model_mark": "10",
            "delta_per_share": "0", "vega_per_unit_vol": "50",
            "vega_per_contract_per_vol_point": "50", "normalized_weight": weight,
            "target_straddle_vega": target, "achieved_leg_vega": achieved,
            "source_fingerprint": instrument_id, "surface_fingerprint": "99",
        })
    return rows


MARK_FIELDS = [
    "date", "valuation_ts_ns", "role", "cohort", "symbol", "uid",
    "instrument_id", "raw_symbol", "expiry_ts_ns", "strike", "side",
    "quantity", "multiplier", "status", "raw_bid", "raw_ask", "raw_mid",
    "model_mark", "model_in_spread",
]


def mark_rows() -> list[dict[str, str]]:
    rows: list[dict[str, str]] = []
    endpoints = {
        "SPY1": ("SPY", "C", "-1", "10", "11"),
        "SPY2": ("SPY", "P", "-1", "9", "8"),
        "AAPL3": ("AAPL", "C", "1", "5", "6"),
        "AAPL4": ("AAPL", "P", "1", "5", "6"),
    }
    for raw, (symbol, side, quantity, first, second) in endpoints.items():
        for date, role, value, ts in (
            ("2026-07-10", "Entry", first, "100"),
            ("2026-07-11", "Held", second, "200"),
        ):
            rows.append({
                "date": date, "valuation_ts_ns": ts, "role": role, "cohort": "1",
                "symbol": symbol, "uid": "1" if symbol == "SPY" else "2",
                "instrument_id": raw[-1], "raw_symbol": raw, "expiry_ts_ns": "100000",
                "strike": "100", "side": side, "quantity": quantity, "multiplier": "100",
                "status": "Ok", "raw_bid": value, "raw_ask": value, "raw_mid": value,
                "model_mark": value, "model_in_spread": "1",
            })
    rows.sort(key=lambda row: (row["date"], row["raw_symbol"]))
    return rows


RECON_FIELDS = [
    "date", "valuation_ts_ns", "held_cohort", "model_option_pnl", "quote_mid_pnl",
    "model_minus_quote_pnl", "model_nav", "quote_mid_nav", "quote_mid_coverage",
    "n_held_lots", "n_quote_mid_lots",
]


def reconciliation_rows() -> list[dict[str, str]]:
    return [
        {"date": "2026-07-10", "valuation_ts_ns": "100", "held_cohort": "1",
         "model_option_pnl": "0", "quote_mid_pnl": "0", "model_minus_quote_pnl": "0",
         "model_nav": "0", "quote_mid_nav": "0", "quote_mid_coverage": "1",
         "n_held_lots": "4", "n_quote_mid_lots": "4"},
        {"date": "2026-07-11", "valuation_ts_ns": "200", "held_cohort": "1",
         "model_option_pnl": "200", "quote_mid_pnl": "200", "model_minus_quote_pnl": "0",
         "model_nav": "200", "quote_mid_nav": "200", "quote_mid_coverage": "1",
         "n_held_lots": "4", "n_quote_mid_lots": "4"},
    ]


BACKTEST_FIELDS = [
    "date", "ts_ns", "pnl_total", "pnl_delta", "pnl_gamma", "pnl_vega",
    "pnl_vanna", "pnl_volga", "pnl_theta", "pnl_rho", "pnl_charm",
    "pnl_unexplained", "pnl_settlement", "pnl_shares", "financing", "cost",
    "nav", "cash", "gross_delta", "gross_gamma", "gross_vega", "gross_theta",
    "turnover_notional", "turnover_vega", "n_open_lots", "n_unpriced_lots",
    "n_unpriced_greeks",
]


def backtest_rows() -> list[dict[str, str]]:
    rows = []
    for date, ts, total in (("2026-07-10", "100", "0"), ("2026-07-11", "200", "200")):
        row = {field: "0" for field in BACKTEST_FIELDS}
        row.update({"date": date, "ts_ns": ts, "pnl_total": total,
                    "pnl_unexplained": total, "nav": total, "n_open_lots": "4"})
        rows.append(row)
    return rows


class ReferenceSpyDispersionTest(unittest.TestCase):
    def build_run(self, root: Path) -> None:
        write_tsv(root / "trade_schedule.tsv", SCHEDULE_FIELDS, schedule_rows(),
                  "ATX_LISTED_DISPERSION_SCHEDULE\t1")
        write_tsv(root / "contract_marks.tsv", MARK_FIELDS, mark_rows())
        write_tsv(root / "reconciliation.tsv", RECON_FIELDS, reconciliation_rows())
        write_tsv(root / "backtest.tsv", BACKTEST_FIELDS, backtest_rows())

    def test_full_artifact_recomputes(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            self.build_run(root)
            output = REFERENCE.verify_run(root)
            self.assertTrue(output.is_file())
            text = output.read_text(encoding="utf-8")
            self.assertIn("roll\t2026-07-10", text)
            self.assertIn("date\t2026-07-11", text)

    def test_corrupt_quantity_fails(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            rows = schedule_rows()
            rows[0]["quantity"] = "-2"
            write_tsv(root / "trade_schedule.tsv", SCHEDULE_FIELDS, rows,
                      "ATX_LISTED_DISPERSION_SCHEDULE\t1")
            with self.assertRaises(REFERENCE.VerificationError):
                REFERENCE.verify_run(root, schedule_only=True)


if __name__ == "__main__":
    unittest.main()
