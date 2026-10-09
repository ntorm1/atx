#!/usr/bin/env python3
"""Run the real-data accuracy panel twice and require byte-identical CSV."""

from __future__ import annotations

import argparse
import csv
import hashlib
import pathlib
import subprocess
import sys
import tempfile


REQUIRED_COLUMNS = {
    "mode",
    "full_chain_valuation",
    "symbol",
    "input",
    "status",
    "error",
    "effective_preset",
    "curve_kind",
    "scored_quotes",
    "market_in_band_fraction",
    "market_reduced_chi_square",
    "market_vol_rmse",
    "market_bid_misses",
    "market_ask_misses",
    "market_max_price_error",
    "calendar_violations",
    "calendar_arb_free",
    "validation_mask",
    "arbitrage_mask",
    "valued_options",
    "fast_candidate_quotes",
    "fast_representative_routes",
    "fast_cold_fallbacks",
    "fast_vs_cold_scored",
    "fast_vs_cold_failures",
    "fast_vs_cold_greek_scored",
    "fast_vs_cold_greek_failures",
    "fast_vs_cold_price_error_p50",
    "fast_vs_cold_price_error_p95",
    "fast_vs_cold_price_error_max",
    "fast_vs_cold_vega_error_p95",
    "fast_vs_cold_vega_error_max",
    "fast_vs_cold_theta_error_p95",
    "fast_vs_cold_theta_error_max",
    "fast_vs_cold_delta_sign_flips",
    "fast_vs_cold_theta_sign_flips",
    "fast_vs_cold_half_spread_pass_fraction",
    "fast_vs_cold_half_tick_pass_fraction",
    "fast_vs_cold_economic_gate_fraction",
    "fast_screen_safe_fraction",
    "fast_sample_coverage_fraction",
    "fast_greek_sample_limit",
}


def _parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--exe", required=True, type=pathlib.Path)
    parser.add_argument("--spy-root", required=True, type=pathlib.Path)
    parser.add_argument("--universe-root", required=True, type=pathlib.Path)
    parser.add_argument("--symbols", required=True, type=pathlib.Path)
    parser.add_argument("--spy-limit", type=int, default=0)
    parser.add_argument("--universe-limit", type=int, default=0)
    parser.add_argument("--no-spy", action="store_true")
    parser.add_argument("--mode", choices=("commit", "exhaustive"), default="commit")
    parser.add_argument("--full-chain-valuation", action="store_true")
    parser.add_argument("--require-ok", action="store_true")
    return parser.parse_args()


def _sha256(path: pathlib.Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def _run_once(args: argparse.Namespace, output: pathlib.Path) -> None:
    command = [
        str(args.exe),
        "--spy-root",
        str(args.spy_root),
        "--universe-root",
        str(args.universe_root),
        "--symbols",
        str(args.symbols),
        "--out",
        str(output),
    ]
    if args.mode != "commit":
        command.extend(("--mode", args.mode))
    if args.spy_limit > 0:
        command.extend(("--spy-limit", str(args.spy_limit)))
    if args.universe_limit > 0:
        command.extend(("--universe-limit", str(args.universe_limit)))
    if args.no_spy:
        command.append("--no-spy")
    if args.full_chain_valuation:
        command.append("--full-chain-valuation")
    subprocess.run(command, check=True)


def _validate_schema(
    path: pathlib.Path,
    expected_mode: str,
    expected_full_chain_valuation: bool,
    require_ok: bool,
) -> int:
    with path.open("r", encoding="utf-8", newline="") as source:
        reader = csv.DictReader(source)
        fields = set(reader.fieldnames or ())
        missing = sorted(REQUIRED_COLUMNS - fields)
        if missing:
            raise AssertionError(f"accuracy panel is missing columns: {missing}")
        timing = sorted(field for field in fields if "timing" in field or field.endswith("_ms"))
        if timing:
            raise AssertionError(f"accuracy panel contains unstable timing columns: {timing}")
        rows = list(reader)
    for row in rows:
        if row["mode"] != expected_mode:
            raise AssertionError(
                f"accuracy panel mode mismatch: {row['mode']!r} != {expected_mode!r}"
            )
        expected_valuation = "1" if expected_full_chain_valuation else "0"
        if row["full_chain_valuation"] != expected_valuation:
            raise AssertionError(
                "accuracy panel full-chain valuation flag mismatch: "
                f"{row['full_chain_valuation']!r} != {expected_valuation!r}"
            )
        if row["status"] != "ok":
            continue
        scored = int(row["fast_vs_cold_scored"])
        failures = int(row["fast_vs_cold_failures"])
        greek_scored = int(row["fast_vs_cold_greek_scored"])
        greek_failures = int(row["fast_vs_cold_greek_failures"])
        representative_routes = int(row["fast_representative_routes"])
        attempted = scored + failures
        if (scored, failures) != (greek_scored, greek_failures):
            raise AssertionError("price and Greek scores must come from the same bundle requests")
        if expected_mode == "commit" and attempted > 64:
            raise AssertionError("commit mode exceeded its 64-point reference budget")
        if expected_mode == "exhaustive" and attempted != representative_routes:
            raise AssertionError("exhaustive mode did not compare every representative route")
        expected_coverage = attempted / representative_routes if representative_routes else None
        actual_coverage = row["fast_sample_coverage_fraction"]
        if expected_coverage is None:
            if actual_coverage:
                raise AssertionError("zero representative routes must have blank sample coverage")
        elif abs(float(actual_coverage) - expected_coverage) > 1.0e-15:
            raise AssertionError("fast sample coverage does not match attempted route bundles")
    if require_ok and not any(row["status"] == "ok" for row in rows):
        raise AssertionError("accuracy panel did not produce a successful row")
    keys = [(row["symbol"], row["input"]) for row in rows]
    if keys != sorted(keys):
        raise AssertionError("accuracy panel rows are not sorted by (symbol, input)")
    return len(rows)


def main() -> int:
    args = _parse_args()
    required_paths = (args.exe, args.spy_root, args.universe_root, args.symbols)
    missing_paths = [str(path) for path in required_paths if not path.exists()]
    if missing_paths:
        print(f"SKIP accuracy panel determinism; missing: {', '.join(missing_paths)}")
        return 0

    with tempfile.TemporaryDirectory(prefix="atx_accuracy_panel_") as temp:
        root = pathlib.Path(temp)
        first = root / "first.csv"
        second = root / "second.csv"
        _run_once(args, first)
        _run_once(args, second)
        first_rows = _validate_schema(
            first, args.mode, args.full_chain_valuation, args.require_ok
        )
        second_rows = _validate_schema(
            second, args.mode, args.full_chain_valuation, args.require_ok
        )
        if first.read_bytes() != second.read_bytes():
            print(
                "accuracy panel output changed between identical runs: "
                f"first={_sha256(first)} second={_sha256(second)}",
                file=sys.stderr,
            )
            return 1
        if first_rows != second_rows:
            print(f"accuracy panel row count changed: {first_rows} != {second_rows}", file=sys.stderr)
            return 1
        print(f"rows={first_rows} sha256={_sha256(first)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
