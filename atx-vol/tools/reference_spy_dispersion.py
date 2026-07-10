#!/usr/bin/env python3
"""Independent arithmetic verifier for the listed SPY dispersion artifact."""

from __future__ import annotations

import argparse
import csv
from decimal import Decimal, InvalidOperation, getcontext
from pathlib import Path
import sys
from typing import Iterable


getcontext().prec = 50
ZERO = Decimal(0)
ONE = Decimal(1)
VEGA_SCALE = Decimal("0.01")
VEGA_REL_TOL = Decimal("1e-10")
PNL_ABS_TOL = Decimal("1e-7")


class VerificationError(RuntimeError):
    pass


def decimal(row: dict[str, str], name: str) -> Decimal:
    try:
        value = Decimal(row[name])
    except (KeyError, InvalidOperation) as exc:
        raise VerificationError(f"invalid decimal column {name!r}") from exc
    if not value.is_finite():
        raise VerificationError(f"nonfinite decimal column {name!r}")
    return value


def integer(row: dict[str, str], name: str) -> int:
    try:
        return int(row[name])
    except (KeyError, ValueError) as exc:
        raise VerificationError(f"invalid integer column {name!r}") from exc


def read_tsv(path: Path, *, magic: str | None = None) -> list[dict[str, str]]:
    if not path.is_file():
        raise VerificationError(f"missing artifact: {path}")
    with path.open("r", encoding="utf-8", newline="") as stream:
        if magic is not None:
            first = stream.readline().rstrip("\r\n")
            if first != magic:
                raise VerificationError(f"bad magic in {path.name}")
        rows = list(csv.DictReader(stream, delimiter="\t"))
    if not rows:
        raise VerificationError(f"empty artifact: {path}")
    return rows


def close(actual: Decimal, expected: Decimal, tolerance: Decimal, label: str) -> None:
    if abs(actual - expected) > tolerance:
        raise VerificationError(
            f"{label}: actual={actual} expected={expected} tolerance={tolerance}"
        )


def verify_schedule(path: Path) -> list[dict[str, str]]:
    rows = read_tsv(path, magic="ATX_LISTED_DISPERSION_SCHEDULE\t1")
    seen_contracts: set[tuple[str, str, str, str]] = set()
    grouped: dict[tuple[str, int], list[dict[str, str]]] = {}
    ordered_keys: list[tuple[str, int]] = []
    for row in rows:
        key = (row["roll_date"], integer(row, "cohort"))
        if key not in grouped:
            if ordered_keys and key <= ordered_keys[-1]:
                raise VerificationError("schedule rolls are not strictly ordered")
            ordered_keys.append(key)
            grouped[key] = []
        contract_key = (
            row["roll_date"],
            row["symbol"],
            row["raw_symbol"],
            row["side"],
        )
        if contract_key in seen_contracts:
            raise VerificationError(f"duplicate schedule contract: {contract_key}")
        seen_contracts.add(contract_key)
        grouped[key].append(row)

    output: list[dict[str, str]] = []
    for key in ordered_keys:
        legs = grouped[key]
        if len(legs) < 4 or len(legs) % 2:
            raise VerificationError(f"invalid leg count for roll {key}")
        target = decimal(legs[0], "gross_index_vega_target")
        if target <= ZERO:
            raise VerificationError(f"nonpositive gross vega target for roll {key}")
        computed_net = ZERO
        computed_gross = ZERO
        weight_sum = ZERO
        basket_target = ZERO
        name_count = 0
        for pair_index in range(0, len(legs), 2):
            call, put = legs[pair_index], legs[pair_index + 1]
            pair_fields = (
                "roll_date",
                "cohort",
                "expiry_ts_ns",
                "is_index",
                "symbol",
                "uid",
                "strike",
                "quantity",
                "multiplier",
                "normalized_weight",
                "target_straddle_vega",
            )
            if call["side"] != "C" or put["side"] != "P" or any(
                call[field] != put[field] for field in pair_fields
            ):
                raise VerificationError(f"invalid call/put pair for roll {key}")
            is_index = call["is_index"] == "1"
            if is_index != (pair_index == 0):
                raise VerificationError(f"index pair ordering mismatch for roll {key}")

            quantity = decimal(call, "quantity")
            pair_vega = ZERO
            pair_achieved = ZERO
            for leg in (call, put):
                multiplier = decimal(leg, "multiplier")
                unit_vega = decimal(leg, "vega_per_unit_vol")
                contract_vega = unit_vega * multiplier * VEGA_SCALE
                close(
                    decimal(leg, "vega_per_contract_per_vol_point"),
                    contract_vega,
                    ZERO,
                    "per-contract vega",
                )
                achieved = decimal(leg, "quantity") * contract_vega
                close(
                    decimal(leg, "achieved_leg_vega"),
                    achieved,
                    ZERO,
                    "achieved leg vega",
                )
                close(
                    decimal(leg, "raw_mid"),
                    (decimal(leg, "raw_bid") + decimal(leg, "raw_ask")) / Decimal(2),
                    ZERO,
                    "raw midpoint",
                )
                pair_vega += contract_vega
                pair_achieved += achieved
                computed_net += achieved
                computed_gross += abs(achieved)

            pair_target = decimal(call, "target_straddle_vega")
            expected_quantity = pair_target / pair_vega
            close(quantity, expected_quantity, abs(expected_quantity) * VEGA_REL_TOL,
                  "vega-flat quantity")
            close(pair_achieved, pair_target, max(ONE, abs(pair_target)) * VEGA_REL_TOL,
                  "straddle target")
            if is_index:
                if decimal(call, "normalized_weight") != ZERO or abs(pair_target) != target:
                    raise VerificationError(f"invalid index target for roll {key}")
            else:
                weight = decimal(call, "normalized_weight")
                if weight <= ZERO:
                    raise VerificationError(f"nonpositive basket weight for roll {key}")
                weight_sum += weight
                basket_target += pair_target
                name_count += 1

        close(weight_sum, ONE, VEGA_REL_TOL, "normalized basket weight")
        close(basket_target, -decimal(legs[0], "target_straddle_vega"),
              target * VEGA_REL_TOL, "basket/index target")
        close(computed_net, decimal(legs[0], "net_vega"), ZERO, "persisted net vega")
        close(computed_gross, decimal(legs[0], "gross_vega"), ZERO,
              "persisted gross vega")
        if name_count != integer(legs[0], "n_names"):
            raise VerificationError(f"name count mismatch for roll {key}")
        relative = abs(computed_net) / target
        if relative > VEGA_REL_TOL:
            raise VerificationError(f"vega residual exceeds tolerance for roll {key}")
        output.append(
            {
                "record_type": "roll",
                "date": key[0],
                "cohort": str(key[1]),
                "computed_net_vega": str(computed_net),
                "computed_gross_vega": str(computed_gross),
                "relative_vega_residual": str(relative),
                "computed_model_option_pnl": "NA",
                "computed_quote_mid_pnl": "NA",
                "computed_model_nav": "NA",
                "computed_quote_mid_nav": "NA",
                "quote_mid_coverage": "NA",
                "status": "Ok",
            }
        )
    return output


def raw_ok(row: dict[str, str]) -> bool:
    return row["status"] == "Ok"


def mark_key(row: dict[str, str]) -> tuple[int, str, str]:
    return integer(row, "cohort"), row["raw_symbol"], row["side"]


def verify_marks_and_reconciliation(
    marks_path: Path, reconciliation_path: Path
) -> list[dict[str, str]]:
    marks = read_tsv(marks_path)
    expected_rows = read_tsv(reconciliation_path)
    by_date: dict[str, list[dict[str, str]]] = {}
    dates: list[str] = []
    seen_marks: set[tuple[str, str, int, str, str]] = set()
    for mark in marks:
        key = (
            mark["date"],
            mark["role"],
            integer(mark, "cohort"),
            mark["raw_symbol"],
            mark["side"],
        )
        if key in seen_marks:
            raise VerificationError(f"duplicate contract mark: {key}")
        seen_marks.add(key)
        if mark["date"] not in by_date:
            if dates and mark["date"] <= dates[-1]:
                raise VerificationError("contract mark dates are not ordered")
            dates.append(mark["date"])
            by_date[mark["date"]] = []
        by_date[mark["date"]].append(mark)

    expected_by_date = {row["date"]: row for row in expected_rows}
    if dates != list(expected_by_date):
        raise VerificationError("contract mark/reconciliation dates disagree")

    previous: dict[tuple[int, str, str], dict[str, str]] = {}
    model_nav = ZERO
    quote_nav = ZERO
    output: list[dict[str, str]] = []
    for date_index, date in enumerate(dates):
        daily = by_date[date]
        entries = [row for row in daily if row["role"] == "Entry"]
        held = [row for row in daily if row["role"] == "Held"]
        model_pnl = ZERO
        quote_pnl = ZERO
        quote_count = 0
        if date_index == 0:
            if held or not entries:
                raise VerificationError("inception must contain entry marks only")
            previous = {mark_key(row): row for row in entries}
            held_count = len(entries)
            quote_count = sum(raw_ok(row) for row in entries)
            held_cohort = integer(entries[0], "cohort")
        else:
            if not held:
                raise VerificationError(f"date {date} has no held marks")
            current: dict[tuple[int, str, str], dict[str, str]] = {}
            for row in held:
                key = mark_key(row)
                if key not in previous:
                    raise VerificationError(f"missing previous endpoint for {key}")
                prior = previous[key]
                scale = decimal(row, "quantity") * decimal(row, "multiplier")
                model_pnl += scale * (decimal(row, "model_mark") - decimal(prior, "model_mark"))
                if raw_ok(row) and raw_ok(prior):
                    quote_pnl += scale * (decimal(row, "raw_mid") - decimal(prior, "raw_mid"))
                    quote_count += 1
                current[key] = row
            held_count = len(held)
            held_cohort = integer(held[0], "cohort")
            previous = {mark_key(row): row for row in entries} if entries else current
        model_nav += model_pnl
        quote_nav += quote_pnl
        coverage = Decimal(quote_count) / Decimal(held_count)
        expected = expected_by_date[date]
        close(decimal(expected, "model_option_pnl"), model_pnl, PNL_ABS_TOL,
              "model option P&L")
        close(decimal(expected, "quote_mid_pnl"), quote_pnl, PNL_ABS_TOL,
              "quote-mid P&L")
        close(decimal(expected, "model_minus_quote_pnl"), model_pnl - quote_pnl,
              PNL_ABS_TOL, "model-minus-quote P&L")
        close(decimal(expected, "model_nav"), model_nav, PNL_ABS_TOL, "model NAV")
        close(decimal(expected, "quote_mid_nav"), quote_nav, PNL_ABS_TOL, "quote NAV")
        close(decimal(expected, "quote_mid_coverage"), coverage, PNL_ABS_TOL,
              "quote coverage")
        if integer(expected, "n_held_lots") != held_count or integer(
            expected, "n_quote_mid_lots"
        ) != quote_count:
            raise VerificationError(f"coverage counts disagree on {date}")
        output.append(
            {
                "record_type": "date",
                "date": date,
                "cohort": str(held_cohort),
                "computed_net_vega": "NA",
                "computed_gross_vega": "NA",
                "relative_vega_residual": "NA",
                "computed_model_option_pnl": str(model_pnl),
                "computed_quote_mid_pnl": str(quote_pnl),
                "computed_model_nav": str(model_nav),
                "computed_quote_mid_nav": str(quote_nav),
                "quote_mid_coverage": str(coverage),
                "status": "Ok",
            }
        )
    return output


def verify_backtest(backtest_path: Path, reconciliation_path: Path) -> None:
    rows = read_tsv(backtest_path)
    reconciliation = read_tsv(reconciliation_path)
    if [row["date"] for row in rows] != [row["date"] for row in reconciliation]:
        raise VerificationError("backtest/reconciliation dates disagree")
    nav = ZERO
    axes = (
        "pnl_delta",
        "pnl_gamma",
        "pnl_vega",
        "pnl_vanna",
        "pnl_volga",
        "pnl_theta",
        "pnl_rho",
        "pnl_charm",
        "pnl_unexplained",
        "pnl_settlement",
        "pnl_shares",
        "financing",
    )
    for row, reference in zip(rows, reconciliation, strict=True):
        total = decimal(row, "pnl_total")
        closure = sum((decimal(row, name) for name in axes), ZERO) - decimal(row, "cost")
        close(total, closure, PNL_ABS_TOL, "backtest P&L closure")
        nav += total
        close(decimal(row, "nav"), nav, PNL_ABS_TOL, "backtest NAV")
        option_pnl = (
            total
            - decimal(row, "pnl_settlement")
            - decimal(row, "pnl_shares")
            - decimal(row, "financing")
            + decimal(row, "cost")
        )
        close(option_pnl, decimal(reference, "model_option_pnl"), PNL_ABS_TOL,
              "backtest/model-mark option P&L")
        if decimal(row, "n_unpriced_lots") != ZERO or decimal(
            row, "n_unpriced_greeks"
        ) != ZERO:
            raise VerificationError("backtest contains unpriced lots")


def write_output(path: Path, rows: Iterable[dict[str, str]]) -> None:
    fields = (
        "record_type",
        "date",
        "cohort",
        "computed_net_vega",
        "computed_gross_vega",
        "relative_vega_residual",
        "computed_model_option_pnl",
        "computed_quote_mid_pnl",
        "computed_model_nav",
        "computed_quote_mid_nav",
        "quote_mid_coverage",
        "status",
    )
    pending = path.with_name(path.name + ".pending")
    path.parent.mkdir(parents=True, exist_ok=True)
    with pending.open("w", encoding="utf-8", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=fields, delimiter="\t", lineterminator="\n")
        writer.writeheader()
        writer.writerows(rows)
    pending.replace(path)


def verify_run(run_dir: Path, schedule_only: bool = False) -> Path:
    output = verify_schedule(run_dir / "trade_schedule.tsv")
    if not schedule_only:
        output.extend(
            verify_marks_and_reconciliation(
                run_dir / "contract_marks.tsv", run_dir / "reconciliation.tsv"
            )
        )
        verify_backtest(run_dir / "backtest.tsv", run_dir / "reconciliation.tsv")
    output_path = run_dir / "reference_reconciliation.tsv"
    write_output(output_path, output)
    return output_path


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--run", required=True, type=Path)
    parser.add_argument("--schedule-only", action="store_true")
    args = parser.parse_args(argv)
    try:
        output = verify_run(args.run, args.schedule_only)
    except VerificationError as exc:
        print(f"reference verification failed: {exc}", file=sys.stderr)
        return 1
    print(output)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
