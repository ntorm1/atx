"""Independent SPY acceptance evidence. No vendor labels are read.

The LP is an offline feasibility witness, never a fitting input. Acceptance needs
an actual API audit plus a fresh-process timing manifest from the same executable
and policy. Numerical LP success alone never qualifies the implementation.
"""
from __future__ import annotations

import argparse
import bisect
import csv
import hashlib
import json
import math
import platform
import statistics
import os
import subprocess
from collections import Counter, defaultdict
from pathlib import Path

INPUT_SHA256 = "b3fafd80f09326ce039ebdcfd7c26914ecbeb39817fdd00b35c6d49dcb2bf33e"
SERVED = {"ok", "ok_price_only"}
NUMERIC = ("strike", "bid", "ask", "spot", "rate", "sdiv", "ddiv_pv", "years",
           "years_calendar")


def sha256(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def read_json(path):
    return json.loads(Path(path).read_text(encoding="utf-8-sig"))


def write_json(path, value):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("x", encoding="utf-8") as stream:
        json.dump(value, stream, indent=2, allow_nan=False)
        stream.write("\n")


def load_quotes(path):
    with Path(path).open(newline="", encoding="utf-8-sig") as stream:
        result = list(csv.DictReader(stream))
    for row in result:
        for key in NUMERIC:
            row[key] = float(row[key])
        for key in ("row_id", "chain_id", "expiry_days", "is_call", "snapshot_ns",
                    "observation_ns"):
            row[key] = int(row[key])
    return result


def invalid_reason(row):
    # A calibration failure, low vega, wide spread, or a difficult carry is NOT
    # an exclusion. Bidless/locked finite markets stay in the denominator.
    if not all(math.isfinite(row[key]) for key in NUMERIC):
        return "nonfinite_input"
    if row["years"] <= 0 or row["years_calendar"] <= 0:
        return "nonpositive_time"
    if row["strike"] <= 0 or row["spot"] <= 0:
        return "nonpositive_strike_or_spot"
    if row["bid"] < 0 or row["ask"] < row["bid"]:
        return "invalid_quote_box"
    if row["is_call"] not in (0, 1) or row["exercise"] != "American":
        return "unsupported_contract"
    if row["input_status"] != "ok":
        return "upstream_" + row["input_status"]
    return None


def freeze(path):
    if sha256(path) != INPUT_SHA256:
        raise ValueError("SPY input differs from the frozen input SHA256")
    rows = load_quotes(path)
    groups = defaultdict(list)
    for row in rows:
        groups[row["chain_id"]].append(row)
    chains = []
    eligible_ids = []
    for chain_id, group in sorted(groups.items(), key=lambda item: item[1][0]["expiry_days"]):
        reasons = Counter(invalid_reason(row) or "valid" for row in group)
        good = [row for row in group if invalid_reason(row) is None]
        contracts = Counter((row["strike"], row["is_call"]) for row in good)
        if any(count != 1 for count in contracts.values()):
            raise ValueError(f"ambiguous duplicate contracts in chain {chain_id}")
        eligible_ids.extend(row["row_id"] for row in good)
        chains.append({"chain_id": chain_id, "expiry_days": group[0]["expiry_days"],
                       "rows": len(group), "eligible_rows": len(good),
                       "valid_strikes": len({row["strike"] for row in good}),
                       "raw_reasons": dict(reasons), "required": bool(good)})
    return {"schema": "atx-vol-spy-eligibility-v1", "input_sha256": sha256(path),
            "rows": len(rows), "eligible_rows": len(eligible_ids),
            "required_slices": sum(c["required"] for c in chains), "chains": chains,
            "eligible_row_ids": sorted(eligible_ids),
            "rules": "Finite American positive-time valid quote boxes; no fit-output exclusions. "
                     "Locked/bidless/wide quotes and price-only contracts remain eligible.",
            "containment_min": 0.995, "containment_tolerance_usd": 1e-12,
            "price_tolerance": "1e-8 + 1e-9 * max(spot,strike)",
            "cold_seconds_strict_max": 1.0}


def snapshot_spot(rows):
    target = rows[0]["snapshot_ns"]
    instant = min({row["observation_ns"] for row in rows if row["observation_ns"] > 0},
                  key=lambda value: (abs(value - target), value))
    return statistics.median(row["spot"] for row in rows if row["observation_ns"] == instant)


def market_bounds(group, spot):
    raw_spot = statistics.median(row["spot"] for row in group)
    tc = statistics.median(row["years_calendar"] for row in group)
    rate = statistics.median(row["rate"] for row in group)
    shift = math.log(spot / raw_spot) / tc
    # Calendar-rate yields; funding-PV gives terminal prepaid S exp(-q Tc)-PV.
    # Certificate envelopes are over the resulting EFFECTIVE yield, including
    # aggregate cash. Raw q extrema with PV subtracted afterwards are too loose.
    yields = [row["sdiv"] + shift for row in group]
    pv = statistics.median(row["ddiv_pv"] for row in group)
    terminal = [spot * math.exp(-yield_ * tc) - pv for yield_ in yields]
    df = math.exp(-rate * tc)
    return {"spot": spot, "discount_min": min(1.0, df), "discount_max": max(1.0, df),
            "prepaid_min": max(min(spot, min(terminal)), 0.0),
            "prepaid_max": max(spot, max(terminal))}


def interpolation(strikes, variable, side, strike):
    index = bisect.bisect_left(strikes, strike)
    if index < len(strikes) and strikes[index] == strike:
        return {variable(index, side): 1.0}
    if index == 0 or index == len(strikes):
        raise ValueError("interpolation outside the certified domain")
    weight = (strike - strikes[index - 1]) / (strikes[index] - strikes[index - 1])
    return {variable(index - 1, side): 1.0 - weight, variable(index, side): weight}


def feasibility(rows, joint=True, min_slack=0.0, spot=None, european_floors=False,
                repair_floor_asks=False):
    import numpy as np
    import scipy
    from scipy.optimize import linprog
    from scipy.sparse import coo_matrix

    groups = defaultdict(list)
    for row in rows:
        if invalid_reason(row) is None:
            groups[row["expiry_days"]].append(row)
    if spot is None:
        spot = snapshot_spot([row for group in groups.values() for row in group])
    rr, cc, vv, rhs, tags = [], [], [], [], []
    slices = []
    nvar = 0
    quote_variables = []
    floor_repairs = []

    def add(coefs, bound, tag, slack=0.0):
        index = len(rhs)
        for col, value in coefs.items():
            rr.append(index); cc.append(col); vv.append(-value)
        if slack:
            rr.append(index); cc.append(-1); vv.append(slack)
        rhs.append(-bound); tags.append(tag)

    for expiry, group in sorted(groups.items()):
        strikes = sorted({row["strike"] for row in group})
        bounds = market_bounds(group, spot)
        base = nvar
        nvar += 2 * len(strikes)
        def variable(index, side, base=base):
            return base + 2 * index + side
        boxes = {(row["strike"], 0 if row["is_call"] else 1): row for row in group}
        tc = statistics.median(row["years_calendar"] for row in group)
        rate = statistics.median(row["rate"] for row in group)
        raw_spot = statistics.median(row["spot"] for row in group)
        shift = math.log(spot / raw_spot) / tc
        pv = statistics.median(row["ddiv_pv"] for row in group)
        df = math.exp(-rate * tc)
        for index, strike in enumerate(strikes):
            local_yields = [row["sdiv"] + shift for row in group if row["strike"] == strike]
            canonical = 0.5 * (min(local_yields) + max(local_yields))
            terminal = spot * math.exp(-canonical * tc) - pv
            for side in (0, 1):
                lower = max(spot - strike, 0.0) if side == 0 else max(strike - spot, 0.0)
                if european_floors:
                    european = terminal - strike * df if side == 0 else strike * df - terminal
                    lower = max(lower, european)
                upper = bounds["prepaid_max"] if side == 0 else strike * bounds["discount_max"]
                add({variable(index, side): 1}, lower, (expiry, strike, side, "intrinsic"))
                add({variable(index, side): -1}, -upper, (expiry, strike, side, "upper"))
                row = boxes.get((strike, side))
                if row:
                    ask = row["ask"]
                    if repair_floor_asks and ask < lower:
                        ask = lower + price_tolerance(bounds, strike)
                        floor_repairs.append({"row_id": row["row_id"], "expiry": expiry,
                                              "strike": strike, "side": side, "original_ask": row["ask"],
                                              "repaired_ask": ask, "floor": lower,
                                              "widening_usd": ask - row["ask"]})
                    quote_variables.append((variable(index, side), row))
                    half = 0.5 * (ask - row["bid"])
                    add({variable(index, side): 1}, row["bid"],
                        (expiry, strike, side, "bid", row["row_id"]), half)
                    add({variable(index, side): -1}, -ask,
                        (expiry, strike, side, "ask", row["row_id"]), half)
            add({variable(index, 0): 1, variable(index, 1): -1},
                bounds["prepaid_min"] - strike * bounds["discount_max"],
                (expiry, strike, "parity_lower"))
            add({variable(index, 0): -1, variable(index, 1): 1},
                -(bounds["prepaid_max"] - strike * bounds["discount_min"]),
                (expiry, strike, "parity_upper"))
        for side in (0, 1):
            for index in range(1, len(strikes)):
                width = strikes[index] - strikes[index - 1]
                coefficients = {variable(index, side): 1, variable(index - 1, side): -1}
                add(coefficients, -bounds["discount_max"] * width if side == 0 else 0,
                    (expiry, strikes[index], side, "slope_lower"))
                add({key: -value for key, value in coefficients.items()},
                    0 if side == 0 else -bounds["discount_max"] * width,
                    (expiry, strikes[index], side, "slope_upper"))
            for index in range(1, len(strikes) - 1):
                left = strikes[index] - strikes[index - 1]
                right = strikes[index + 1] - strikes[index]
                add({variable(index - 1, side): right / (left + right),
                     variable(index + 1, side): left / (left + right),
                     variable(index, side): -1}, 0, (expiry, strikes[index], side, "convexity"))
        slices.append((expiry, strikes, variable))
    calendar_rows = 0
    if joint:
        # All pairs, not merely adjacent expiry domains: nonadjacent domains may
        # overlap where the intervening expiry has no strike support.
        for early in range(len(slices)):
            ea, ka, va = slices[early]
            for eb, kb, vb in slices[early + 1:]:
                lo, hi = max(ka[0], kb[0]), min(ka[-1], kb[-1])
                for strike in sorted({k for k in ka + kb if lo <= k <= hi}):
                    for side in (0, 1):
                        coefficients = interpolation(kb, vb, side, strike)
                        for key, value in interpolation(ka, va, side, strike).items():
                            coefficients[key] = coefficients.get(key, 0.0) - value
                        add(coefficients, 0, (ea, eb, strike, side, "calendar"))
                        calendar_rows += 1
    cc = [nvar if col == -1 else col for col in cc]
    matrix = coo_matrix((vv, (rr, cc)), shape=(len(rhs), nvar + 1)).tocsr()
    objective = np.zeros(nvar + 1)
    objective[-1] = -1
    result = linprog(objective, A_ub=matrix, b_ub=np.array(rhs),
                     bounds=[(None, None)] * nvar + [(min_slack, 1.0)],
                     method="highs", options={"primal_feasibility_tolerance": 1e-9,
                                              "dual_feasibility_tolerance": 1e-9,
                                              "time_limit": 120})
    output = {"status": result.status, "message": result.message, "slices": len(slices),
              "constraints": len(rhs), "calendar_constraints": calendar_rows,
              "variables": nvar + 1, "spot": spot, "min_slack": min_slack,
              "european_lower_bounds": european_floors,
              "floor_ask_repairs": floor_repairs,
              "scipy": scipy.__version__, "numpy": np.__version__,
              "least_box_slack_half_spreads": float(result.x[-1]) if result.success else None,
              "maximum_constraint_violation_usd":
                  float(max(0, np.max(matrix @ result.x - rhs))) if result.success else None,
              "scope": "PWL node/shape/affine American bounds and all-pair union-knot calendar; "
                       "necessary static constraints, not model-price realizability"}
    if result.success and abs(result.x[-1]) <= 1e-8:
        dual = result.ineqlin.marginals
        output["zero_slack_dual_support"] = [
            {"constraint": tags[index], "dual": float(dual[index])}
            for index in np.flatnonzero(np.abs(dual) > 1e-9)][:100]
    if result.success:
        outside = []
        for index, row in quote_variables:
            price = float(result.x[index])
            if not row["bid"] - 1e-12 <= price <= row["ask"] + 1e-12:
                outside.append({"row_id": row["row_id"], "expiry": row["expiry_days"],
                                "strike": row["strike"], "is_call": row["is_call"],
                                "price": price, "bid": row["bid"], "ask": row["ask"],
                                "excess_usd": max(row["bid"] - price, price - row["ask"])})
        output["outside_original_quotes"] = outside
        output["contained_rows"] = len(quote_variables) - len(outside)
        output["total_rows"] = len(quote_variables)
        output["containment"] = 1 - len(outside) / len(quote_variables)
    return output


def prediction_metrics(input_rows, prediction_path, eligibility):
    with Path(prediction_path).open(newline="", encoding="utf-8-sig") as stream:
        predictions = list(csv.DictReader(stream))
    ids = [int(row["row_id"]) for row in predictions]
    if len(ids) != len(set(ids)):
        raise ValueError("duplicate prediction row ids")
    by_id = dict(zip(ids, predictions))
    expected = set(eligibility["eligible_row_ids"])
    served = inside = 0
    chains = defaultdict(lambda: {"eligible": 0, "served": 0, "inside": 0, "status": Counter()})
    for row in input_rows:
        if row["row_id"] not in expected:
            continue
        record = chains[row["chain_id"]]
        record["eligible"] += 1
        prediction = by_id.get(row["row_id"], {})
        status = prediction.get("status", "missing")
        record["status"][status] += 1
        try:
            price = float(prediction.get("predicted_price") or "nan")
        except ValueError:
            price = math.nan
        if status not in SERVED or not math.isfinite(price):
            continue
        served += 1
        record["served"] += 1
        if row["bid"] - 1e-12 <= price <= row["ask"] + 1e-12:
            inside += 1
            record["inside"] += 1
    return {"prediction_sha256": sha256(prediction_path), "eligible": len(expected),
            "served": served, "inside": inside,
            "containment_of_served": inside / served if served else 0,
            "contained_of_eligible": inside / len(expected),
            "fully_served_slices": sum(row["served"] == row["eligible"] for row in chains.values()),
            "partially_served_slices": sum(0 < row["served"] < row["eligible"] for row in chains.values()),
            "chains": chains}


def price_tolerance(bounds, strike):
    return 1e-8 + 1e-9 * max(bounds["spot"], strike)


def pwl(nodes, strike, side):
    strikes = [node["strike"] for node in nodes]
    index = bisect.bisect_left(strikes, strike)
    if index < len(strikes) and strikes[index] == strike:
        return nodes[index][side]
    if index == 0 or index == len(strikes):
        return None
    weight = (strike - strikes[index - 1]) / (strikes[index] - strikes[index - 1])
    return nodes[index - 1][side] * (1 - weight) + nodes[index][side] * weight


def withheld(nodes, mask, strike, side):
    if not mask:
        return False
    bit = 1 if side == "call" else 2
    strikes = [node["strike"] for node in nodes]
    index = bisect.bisect_left(strikes, strike)
    if index < len(strikes) and strikes[index] == strike:
        return bool(mask[index] & bit)
    return bool(mask[index - 1] & bit or mask[index] & bit)


def shape_violations(entry):
    nodes, bounds = entry["nodes"], entry["bounds"]
    problems = []

    def fail(kind, strike, excess):
        problems.append({"kind": kind, "chain_id": entry["chain_id"],
                         "strike": strike, "excess_usd": excess})

    for node in nodes:
        strike, call, put = node["strike"], node["call"], node["put"]
        tol = price_tolerance(bounds, strike)
        checks = {"call_intrinsic": max(bounds["spot"] - strike, 0) - call,
                  "put_intrinsic": max(strike - bounds["spot"], 0) - put,
                  "call_upper": call - bounds["prepaid_max"],
                  "put_upper": put - strike * bounds["discount_max"],
                  "parity_lower": bounds["prepaid_min"] - strike * bounds["discount_max"]
                                  - call + put,
                  "parity_upper": call - put - bounds["prepaid_max"]
                                  + strike * bounds["discount_min"]}
        for kind, excess in checks.items():
            if excess > tol:
                fail(kind, strike, excess)
    for side in ("call", "put"):
        # Running extrema prove vertical constraints for ALL pairs; adjacent
        # tolerance alone permits accumulated violations.
        min_price = max_price = nodes[0][side]
        min_affine = max_affine = (nodes[0][side] + bounds["discount_max"] *
                                  nodes[0]["strike"] * (1 if side == "call" else -1))
        for node in nodes[1:]:
            strike, price = node["strike"], node[side]
            affine = price + bounds["discount_max"] * strike * (1 if side == "call" else -1)
            excess = max(price - min_price, max_affine - affine) if side == "call" else \
                     max(max_price - price, affine - min_affine)
            if excess > price_tolerance(bounds, strike):
                fail(side + "_vertical", strike, excess)
            min_price, max_price = min(min_price, price), max(max_price, price)
            min_affine, max_affine = min(min_affine, affine), max(max_affine, affine)
        hull = []
        for index, node in enumerate(nodes):
            while len(hull) >= 2:
                left, middle = nodes[hull[-2]], nodes[hull[-1]]
                fraction = (middle["strike"] - left["strike"]) / (node["strike"] - left["strike"])
                envelope = left[side] * (1 - fraction) + node[side] * fraction
                if middle[side] < envelope:
                    break
                hull.pop()
            hull.append(index)
        for left_index, right_index in zip(hull, hull[1:]):
            left, right = nodes[left_index], nodes[right_index]
            for node in nodes[left_index + 1:right_index]:
                fraction = (node["strike"] - left["strike"]) / (right["strike"] - left["strike"])
                excess = node[side] - left[side] * (1 - fraction) - right[side] * fraction
                if excess > price_tolerance(bounds, node["strike"]):
                    fail(side + "_convexity", node["strike"], excess)
    return problems


def terminal_floor_violations(entry):
    market = entry.get("market")
    model = entry.get("dividend_model")
    if not market or model not in ("continuous_yield", "aggregate_pv_approximation",
                                  "aggregate_funding_pv_approximation"):
        return [{"kind": "missing_terminal_carry_evidence", "chain_id": entry["chain_id"]}]
    spot, tc, pv = market["spot"], market["calendar_years"], market["dividend_pv"]
    df = math.exp(-market["rate"] * tc)
    base = spot - pv if model == "aggregate_pv_approximation" else spot
    cash = pv if model == "aggregate_funding_pv_approximation" else 0.0
    curve = entry.get("carry_curve", [])
    carry_strikes = [point["strike"] for point in curve]

    def yield_at(strike):
        if not curve:
            return market["continuous_yield"]
        index = bisect.bisect_left(carry_strikes, strike)
        if index == 0:
            return curve[0]["continuous_yield"]
        if index == len(curve):
            return curve[-1]["continuous_yield"]
        left, right = curve[index - 1], curve[index]
        weight = math.log(strike / left["strike"]) / math.log(right["strike"] / left["strike"])
        return left["continuous_yield"] * (1 - weight) + right["continuous_yield"] * weight

    nodes = entry["nodes"]
    lo, hi = nodes[0]["strike"], nodes[-1]["strike"]
    knots = sorted({node["strike"] for node in nodes} |
                   {strike for strike in carry_strikes if lo <= strike <= hi})
    candidates = set(knots)
    # On every union segment the price is affine and terminal prepaid is c*K^a
    # minus constant cash. Endpoints and the single possible derivative root
    # prove the lower bound continuously, beyond a finite midpoint stencil.
    for left, right in zip(knots, knots[1:]):
        qleft, qright = yield_at(left), yield_at(right)
        exponent = -(qright - qleft) * tc / math.log(right / left)
        if exponent == 0 or exponent == 1:
            continue
        log_coefficient = math.log(base) - qleft * tc - exponent * math.log(left)
        for side in ("call", "put"):
            slope = (pwl(nodes, right, side) - pwl(nodes, left, side)) / (right - left)
            numerator = slope + df if side == "call" else df - slope
            if numerator == 0 or numerator / exponent <= 0:
                continue
            log_strike = (math.log(abs(numerator / exponent)) - log_coefficient) / (exponent - 1)
            if math.log(left) < log_strike < math.log(right):
                candidates.add(math.exp(log_strike))
    problems = []
    for strike in sorted(candidates):
        prepaid = base * math.exp(-yield_at(strike) * tc) - cash
        for side in ("call", "put"):
            floor = max(0.0, prepaid - strike * df if side == "call" else strike * df - prepaid)
            price = pwl(nodes, strike, side)
            if floor - price > price_tolerance(entry["bounds"], strike):
                problems.append({"kind": "terminal_european_floor", "chain_id": entry["chain_id"],
                                 "strike": strike, "side": side, "floor": floor,
                                 "price": price, "excess_usd": floor - price})
    return problems


def valid_query_metadata(query):
    required = {"status", "reason", "volatility", "iv_lower", "iv_upper", "vol_convention"}
    if not required <= query.keys():
        return False
    lo, hi = query["iv_lower"], query["iv_upper"]
    if any(value is not None and (not isinstance(value, (int, float)) or
            not math.isfinite(value) or value < 0) for value in (lo, hi)):
        return False
    if lo is not None and hi is not None and lo > hi:
        return False
    if query["status"] == "success":
        return query["reason"] == "none" and lo is None and hi is None and \
            query["vol_convention"] in ("fitted_curve", "reimplied_repaired")
    if query["status"] == "price_only":
        return query["volatility"] is None and query["vol_convention"] == "none" and \
            query["reason"] in ("floor_vol", "flat_exercise_region", "low_vega", "unidentified_interval")
    return False


def cold_root_audit(report, rows=()):
    """Cold Reference-profile forward signs at the unchanged +/-1e-7 IV target.

    Roundoff-only sign matches are diagnostic, never a strict accuracy pass.
    The 32-epsilon USD allowance is explicit and is not an IV-tolerance increase.
    """
    row_ids = defaultdict(list)
    for row in rows:
        if invalid_reason(row) is None:
            row_ids[row["chain_id"], row["strike"], "call" if row["is_call"] else "put"].append(row["row_id"])
    failures, roundoff = [], []
    identified = price_only = strict = 0
    max_residual = 0.0
    entries = [entry for snapshot in report.get("graded_stats", {}).get("acceptance_audit", {})
               .get("snapshots", []) for entry in snapshot]
    if not entries:
        failures.append({"kind": "missing_actual_api_audit"})
    for entry in entries:
        if entry.get("grade") not in ("A", "B"):
            continue
        queries = {(q["strike"], q["side"]): q for q in entry.get("queries", [])}
        market = entry.get("market", {})
        curve = entry.get("carry_curve", [])
        strikes = [point["strike"] for point in curve]
        for node in entry.get("nodes", []):
            strike = node["strike"]
            for side in ("call", "put"):
                query = queries.get((strike, side), {})
                if query.get("status") == "price_only":
                    price_only += 1
                    continue
                context = {"chain_id": entry["chain_id"], "strike": strike, "side": side,
                           "row_ids": row_ids[entry["chain_id"], strike, side],
                           "query": query, "market": market, "dividend_model": entry.get("dividend_model")}
                if query.get("status") != "success":
                    failures.append(dict(context, kind="missing_identified_node_query"))
                    continue
                identified += 1
                if any(not isinstance(query.get(key), (int, float)) or
                       isinstance(query[key], bool) or not math.isfinite(query[key])
                       for key in ("volatility", "price")):
                    failures.append(dict(context, kind="nonfinite_identified_query"))
                    continue
                cold = query.get("cold_node", {})
                numeric = ("spot", "strike", "variance_years", "effective_rate", "calendar_yield",
                           "effective_yield", "sigma_lower", "sigma_center", "sigma_upper",
                           "price_lower", "price_center", "price_upper", "radius")
                if cold.get("status") != "evaluated" or cold.get("model") != "andersen_lake" or any(
                    not isinstance(cold.get(key), (int, float)) or isinstance(cold[key], bool) or
                    not math.isfinite(cold[key]) for key in numeric):
                    failures.append(dict(context, kind="missing_unsupported_or_nonfinite_cold_node"))
                    continue
                index = bisect.bisect_left(strikes, strike)
                if not curve:
                    calendar_q = market["continuous_yield"]
                elif index == len(curve):
                    calendar_q = curve[-1]["continuous_yield"]
                elif index == 0 or strikes[index] == strike:
                    calendar_q = curve[index]["continuous_yield"]
                else:
                    weight = math.log1p((strike - strikes[index - 1]) / strikes[index - 1]) / \
                             math.log1p((strikes[index] - strikes[index - 1]) / strikes[index - 1])
                    calendar_q = curve[index - 1]["continuous_yield"] * (1 - weight) + \
                                 curve[index]["continuous_yield"] * weight
                spot, tv, tc = market["spot"], market["variance_years"], market["calendar_years"]
                terminal = spot * math.exp(-calendar_q * tc)
                if entry["dividend_model"] == "aggregate_funding_pv_approximation":
                    terminal -= market["dividend_pv"]
                elif entry["dividend_model"] == "aggregate_pv_approximation":
                    terminal = (spot - market["dividend_pv"]) * math.exp(-calendar_q * tc)
                elif entry["dividend_model"] != "continuous_yield":
                    failures.append(dict(context, kind="unsupported_declared_model"))
                    continue
                if terminal <= 0 or tv <= 0:
                    failures.append(dict(context, kind="invalid_declared_carry"))
                    continue
                expected = {"spot": spot, "strike": strike, "variance_years": tv,
                            "effective_rate": market["rate"] * tc / tv,
                            "calendar_yield": calendar_q, "effective_yield": -math.log(terminal / spot) / tv}
                sigma = query["volatility"]
                options = cold.get("options", {})
                if options != entry.get("al_options") or options.get("profile") != 0 or any(
                    not math.isclose(cold[key], value, rel_tol=1e-12, abs_tol=1e-13)
                    for key, value in expected.items()) or cold["sigma_center"] != sigma or \
                    cold["sigma_lower"] != sigma - 1e-7 or cold["sigma_upper"] != sigma + 1e-7 or \
                    cold["radius"] != 1e-7 or cold["sigma_lower"] <= 0:
                    failures.append(dict(context, kind="cold_contract_or_radius_mismatch", expected=expected))
                    continue
                target = query["price"]
                lower, center, upper = (cold["price_" + name] for name in ("lower", "center", "upper"))
                allowance = 32 * math.ulp(1.0) * max(1.0, spot, strike, abs(target),
                                                   abs(lower), abs(center), abs(upper))
                max_residual = max(max_residual, abs(center - target))
                diagnostic = dict(context, price_allowance_usd=allowance,
                                  center_residual_usd=center - target,
                                  lower_excess_usd=lower - target, upper_shortfall_usd=target - upper)
                if center < lower - allowance or center > upper + allowance or upper < lower - allowance:
                    failures.append(dict(diagnostic, kind="cold_forward_nonmonotone"))
                elif lower < target < upper:
                    strict += 1
                elif lower - allowance <= target <= upper + allowance:
                    roundoff.append(dict(diagnostic, kind="roundoff_only_or_flat"))
                else:
                    failures.append(dict(diagnostic, kind="cold_root_outside_1e7_bracket"))
    return {"schema": "atx-vol-cold-node-root-audit-v1", "identified_nodes": identified,
            "price_only_nodes": price_only, "strict_straddles": strict,
            "roundoff_only_count": len(roundoff), "failure_count": len(failures),
            "strict_accuracy_passed": identified > 0 and strict == identified and not failures,
            "max_center_residual_usd": max_residual,
            "iv_radius": 1e-7, "price_allowance": "32*epsilon*max(1,S,K,abs(target),abs(cold prices))",
            "failures": failures, "roundoff_only": roundoff}


def prediction_metadata_matches(query, prediction):
    if not valid_query_metadata(query):
        return False
    expected_status = "ok" if query["status"] == "success" else "ok_price_only"
    if prediction.get("status") != expected_status or \
       prediction.get("price_only_reason") != query["reason"] or \
       prediction.get("vol_convention") != query["vol_convention"]:
        return False
    for exported, column in (("volatility", "predicted_vol"), ("iv_lower", "iv_lower"),
                              ("iv_upper", "iv_upper")):
        if column not in prediction:
            return False
        value = prediction[column]
        if query[exported] is None:
            if value != "":
                return False
        else:
            try:
                if float(value) != query[exported]:
                    return False
            except (TypeError, ValueError):
                return False
    return True


def api_violations(report):
    audit = report.get("graded_stats", {}).get("acceptance_audit")
    if not audit or audit.get("schema") != "atx-vol-final-api-audit-v1":
        return [{"kind": "missing_native_api_audit"}]
    if audit.get("representation") != "certified_piecewise_linear":
        return [{"kind": "unproved_representation"}]
    if not audit.get("snapshots"):
        return [{"kind": "empty_native_api_audit"}]
    problems = []
    for snapshot in audit.get("snapshots", []):
        entries = [entry for entry in snapshot if entry["nodes"]]
        if not entries:
            problems.append({"kind": "no_certified_slices"})
            continue
        union = sorted({node["strike"] for entry in entries for node in entry["nodes"]})
        for entry in entries:
            nodes, mask = entry["nodes"], entry["withheld_nodes"]
            if entry.get("record_attached") is not False:
                # Finite probes can find record-route errors, but cannot prove
                # continuous shape/calendar for a generally nonlinear record.
                problems.append({"kind": "unproved_record_representation",
                                 "chain_id": entry["chain_id"]})
            strikes = [node["strike"] for node in nodes]
            if len(nodes) < 2 or strikes != sorted(set(strikes)) or \
               any(not math.isfinite(node[key]) for node in nodes for key in ("strike", "call", "put")):
                problems.append({"kind": "malformed_nodes", "chain_id": entry["chain_id"]})
                continue
            if mask and len(mask) != len(nodes):
                problems.append({"kind": "malformed_mask", "chain_id": entry["chain_id"]})
                continue
            problems.extend(shape_violations(entry))
            problems.extend(terminal_floor_violations(entry))
            queries = {(q["strike"], q["side"]): q for q in entry["queries"]}
            if len(queries) != len(entry["queries"]):
                problems.append({"kind": "duplicate_query", "chain_id": entry["chain_id"]})
            required = {strike for strike in union if strikes[0] <= strike <= strikes[-1]}
            for left, right in zip(strikes, strikes[1:]):
                required.update(left * (1 - f) + right * f for f in (.25, .5, .75))
            for strike in required:
                for side in ("call", "put"):
                    query = queries.get((strike, side))
                    if query is None:
                        problems.append({"kind": "missing_query", "chain_id": entry["chain_id"],
                                         "strike": strike, "side": side})
                        continue
                    unsupported = withheld(nodes, mask, strike, side)
                    if unsupported:
                        if query["price"] is not None or query["served_price"] is not None:
                            problems.append({"kind": "withheld_price_leak",
                                             "chain_id": entry["chain_id"], "strike": strike})
                        continue
                    expected = pwl(nodes, strike, side)
                    tol = price_tolerance(entry["bounds"], strike)
                    if query["status"] not in ("success", "price_only") or \
                       any(query[key] is None or not math.isfinite(query[key]) or
                           abs(query[key] - expected) > tol for key in ("price", "served_price")):
                        problems.append({"kind": "api_price_or_status", "chain_id": entry["chain_id"],
                                         "strike": strike, "side": side})
                    if query["status"] == "price_only" and query.get("volatility") is not None:
                        problems.append({"kind": "false_price_only_vol", "chain_id": entry["chain_id"]})
                    if query["status"] == "success" and (query.get("volatility") is None or
                            not math.isfinite(query["volatility"]) or query["volatility"] <= 0):
                        problems.append({"kind": "invalid_identified_vol", "chain_id": entry["chain_id"]})
                    if not valid_query_metadata(query):
                        problems.append({"kind": "invalid_api_metadata", "chain_id": entry["chain_id"],
                                         "strike": strike, "side": side})
            entry_queries = queries
            # Local-only index: never trust an audit-supplied precomputed arb result.
            entry["_queries"] = entry_queries
        ordered = sorted(entries, key=lambda entry: entry["calendar_years"])
        for i, early in enumerate(ordered):
            for later in ordered[i + 1:]:
                if "_queries" not in early or "_queries" not in later:
                    continue
                lo = max(early["nodes"][0]["strike"], later["nodes"][0]["strike"])
                hi = min(early["nodes"][-1]["strike"], later["nodes"][-1]["strike"])
                knots = {n["strike"] for entry in (early, later) for n in entry["nodes"]
                         if lo <= n["strike"] <= hi}
                for strike in knots:
                    for side in ("call", "put"):
                        a = early["_queries"].get((strike, side), {}).get("served_price")
                        b = later["_queries"].get((strike, side), {}).get("served_price")
                        if a is not None and b is not None and a - b > \
                           max(price_tolerance(early["bounds"], strike),
                               price_tolerance(later["bounds"], strike)):
                            problems.append({"kind": "calendar", "early": early["chain_id"],
                                             "later": later["chain_id"], "strike": strike,
                                             "side": side, "excess_usd": a - b})
        for entry in entries:
            entry.pop("_queries", None)
    return problems


def run_audit(args):
    args.out.mkdir(parents=True, exist_ok=False)
    predictions, report = args.out / "predictions.csv", args.out / "native.json"
    command = [str(args.binary.resolve()), "--input", str(args.input.resolve()),
               "--output", str(predictions.resolve()), "--report", str(report.resolve()),
               "--model", "quote-slice", "--policy", str(args.policy.resolve()),
               "--dividends", "funding-pv", "--acceptance-audit", "--timing-detail"]
    env = dict(os.environ, ATX_VOL_FIT_WORKERS="1")
    process = subprocess.run(command, capture_output=True, text=True, env=env, timeout=1800)
    (args.out / "stdout.txt").write_text(process.stdout, encoding="utf-8")
    (args.out / "stderr.txt").write_text(process.stderr, encoding="utf-8")
    receipt = {"command": command, "fit_workers": 1, "exit_code": process.returncode,
               "binary_sha256": sha256(args.binary), "policy_sha256": sha256(args.policy),
               "input_sha256": sha256(args.input), "timing_qualifying": False,
               "predictions_sha256": sha256(predictions) if predictions.exists() else None,
               "report_sha256": sha256(report) if report.exists() else None}
    write_json(args.out / "receipt.json", receipt)
    if process.returncode:
        raise RuntimeError(f"Native audit failed: {process.returncode}: {process.stderr}")
    return receipt


def one_core_trial(trial):
    affinity = trial.get("affinity", {})
    workers = affinity.get("fit_workers", {})
    cpu = affinity.get("requested_cpu")
    try:
        mask = int(affinity.get("process_affinity_mask", "0"), 0)
    except (TypeError, ValueError):
        return False
    return (isinstance(cpu, int) and 0 <= cpu < 64 and mask == 1 << cpu and
            affinity.get("pinned") is True and affinity.get("is_pcore") is True and
            affinity.get("current_cpu_at_receipt") == cpu and
            isinstance(workers, dict) and workers.get("env_ATX_VOL_FIT_WORKERS") == "1" and
            workers.get("resolved_auto") == 1 and
            affinity.get("nested_pools", {}).get("surface_expiry_fanout") == 1)


def cold_timing_checks(manifest, directory, receipt, audit_report):
    """Join launcher records to independently hashed native reports and CSVs.

    Summary counters are assertions to cross-check, never evidence of trials.
    Kernel PID/creation identities are recorded independently by both processes.
    """
    trials = manifest.get("trials", [])
    successful = [trial for trial in trials if trial.get("exit_code") == 0]
    count = len(trials)
    checks = {
        "at_least100_fresh_processes": count >= 100 and len(successful) == count and
            manifest.get("trials_ok") == count and manifest.get("trials_requested") == count,
        "same_binary": manifest.get("binary", {}).get("sha256") == receipt["binary_sha256"],
        "same_input": manifest.get("input", {}).get("sha256") == receipt["input_sha256"],
        "same_policy": manifest.get("policy", {}).get("sha256") == receipt["policy_sha256"],
        "same_predictions": manifest.get("predictions_identical") is True and
            manifest.get("predictions_sha256") == receipt["predictions_sha256"],
        "one_pcore": bool(trials), "idle_sibling": bool(trials),
        "fast_math_off": manifest.get("fast_math") is False,
        "all_cold_under1second": bool(trials), "native_reports_verified": bool(trials),
        "distinct_trial_report_process_evidence": bool(trials),
        "unaudited_timing_command": "--acceptance-audit" not in manifest.get("command_template", ""),
        "source_clean": manifest.get("source", {}).get("dirty") is False,
    }
    trial_ids, report_paths, report_hashes, process_ids = set(), set(), set(), set()
    for trial in trials:
        try:
            native_path = (directory / trial["report"]).resolve(strict=True)
            prediction_path = (directory / trial["predictions"]).resolve(strict=True)
            native = read_json(native_path)
            bench = native["bench"]
            reps = bench["per_rep"]
            rep = reps[0]
            cold = rep["cold_calibration_s"]
            affinity = bench["affinity"]
            identity = bench["process_identity"]
            process_id = (identity["pid"], identity["creation_filetime"])
            identity_valid = (isinstance(process_id[0], int) and process_id[0] > 0 and
                              isinstance(process_id[1], str) and process_id[1].isdigit() and
                              int(process_id[1]) > 0)
            report_hash = sha256(native_path)
            distinct = (trial["trial"] not in trial_ids and native_path not in report_paths and
                        report_hash not in report_hashes and process_id not in process_ids)
            checks["distinct_trial_report_process_evidence"] &= distinct and identity_valid and \
                identity == trial.get("process_identity")
            trial_ids.add(trial["trial"]); report_paths.add(native_path)
            report_hashes.add(report_hash); process_ids.add(process_id)
            checks["native_reports_verified"] &= (report_hash == trial.get("report_sha256") and
                native.get("policy_definition") == audit_report.get("policy_definition") and
                "acceptance_audit" not in native.get("graded_stats", {}) and
                bench.get("reps") == 1 and len(reps) == 1 and rep.get("rep") == 1 and
                rep.get("deam_memo_entries_at_start") == 0)
            checks["all_cold_under1second"] &= (trial.get("exit_code") == 0 and
                isinstance(cold, (int, float)) and math.isfinite(cold) and 0 < cold < 1 and
                cold == trial.get("cold_calibration_s"))
            checks["same_predictions"] &= (sha256(prediction_path) ==
                trial.get("predictions_sha256") == receipt["predictions_sha256"])
            checks["one_pcore"] &= one_core_trial({"affinity": affinity}) and all(
                affinity.get(key) == trial.get("affinity", {}).get(key)
                for key in ("requested_cpu", "pinned", "is_pcore", "current_cpu_at_receipt",
                            "process_affinity_mask", "fit_workers", "nested_pools"))
            siblings = affinity.get("smt_siblings", [])
            busy = affinity.get("busy", {}).get("smt_sibling_pct", [])
            checks["idle_sibling"] &= (len(siblings) == len(busy) and
                (bool(siblings) or affinity.get("smt") is False) and
                all(isinstance(value, (int, float)) and math.isfinite(value) and
                    0 <= value < 5 for value in busy) and
                busy == trial.get("affinity", {}).get("smt_sibling_busy_pct"))
            checks["fast_math_off"] &= bench.get("build_info", {}).get("fast_math") is False
        except (KeyError, TypeError, ValueError, OSError, IndexError):
            checks["native_reports_verified"] = False
            checks["distinct_trial_report_process_evidence"] = False
    return checks


def raw_model_violations(report, rows):
    """SPY contract: independently reconstruct nearest-spot funding-PV inputs.

    This prevents a fitter/exporter error from silently relaxing its own oracle.
    The snapshot's per-strike carry is frozen from raw quotes before observing fits.
    """
    policy = report.get("policy_definition", {}).get("snapshot", {})
    if policy.get("one_spot") != "nearest" or not \
       0 <= policy.get("carry_price_tolerance", 0) <= 1e-6:
        return [{"kind": "unsupported_snapshot_normalization"}]
    eligible = [row for row in rows if invalid_reason(row) is None]
    spot = snapshot_spot(eligible)
    groups = defaultdict(list)
    for row in eligible:
        groups[row["chain_id"]].append(row)
    problems = []
    audit = report.get("graded_stats", {}).get("acceptance_audit", {})
    for snapshot in audit.get("snapshots", []):
        for entry in snapshot:
            if entry.get("grade") not in ("A", "B"):
                continue
            chain = entry["chain_id"]
            group = groups.get(chain)
            if not group:
                problems.append({"kind": "unfrozen_chain", "chain_id": chain})
                continue
            tc = statistics.median(row["years_calendar"] for row in group)
            shift = math.log(spot / statistics.median(row["spot"] for row in group)) / tc
            expected = {"spot": spot, "rate": statistics.median(row["rate"] for row in group),
                "calendar_years": tc, "variance_years": statistics.median(row["years"] for row in group),
                "dividend_pv": statistics.median(row["ddiv_pv"] for row in group),
                "continuous_yield": statistics.median(row["sdiv"] for row in group) + shift}
            if entry.get("dividend_model") != "aggregate_funding_pv_approximation" or any(
                not isinstance(entry.get("market", {}).get(key), (int, float)) or
                not math.isfinite(entry["market"][key]) or
                not math.isclose(entry["market"][key], value, rel_tol=1e-13, abs_tol=1e-13)
                for key, value in expected.items()):
                problems.append({"kind": "market_differs_from_raw", "chain_id": chain})
            local = defaultdict(list)
            for row in group:
                local[row["strike"]].append(row["sdiv"] + shift)
            curve = {point["strike"]: point["continuous_yield"]
                     for point in entry.get("carry_curve", [])}
            canonical = {strike: 0.5 * (min(values) + max(values)) for strike, values in local.items()}
            if len(curve) != len(entry.get("carry_curve", [])) or curve.keys() != canonical.keys() or any(
                not isinstance(curve.get(strike), (int, float)) or
                not math.isfinite(curve[strike]) or
                not math.isclose(curve[strike], value, rel_tol=1e-13, abs_tol=1e-13)
                for strike, value in canonical.items()):
                problems.append({"kind": "carry_differs_from_raw", "chain_id": chain})
            terminal = [spot * math.exp(-value * tc) - expected["dividend_pv"]
                        for value in canonical.values()]
            df = math.exp(-expected["rate"] * tc)
            bounds = {"spot": spot, "discount_min": min(1.0, df), "discount_max": max(1.0, df),
                      "prepaid_min": min(spot, min(terminal)), "prepaid_max": max(spot, max(terminal))}
            if any(not isinstance(entry.get("bounds", {}).get(key), (int, float)) or
                   not math.isfinite(entry["bounds"][key]) or entry["bounds"][key] <= 0 or
                   not math.isclose(entry["bounds"][key], value, rel_tol=1e-12, abs_tol=1e-10)
                   for key, value in bounds.items()):
                problems.append({"kind": "bounds_differ_from_raw", "chain_id": chain})
    return problems


def verify(args, eligibility):
    rows = load_quotes(args.input)
    audit_dir = args.audit_dir
    report = read_json(audit_dir / "native.json")
    receipt = read_json(audit_dir / "receipt.json")
    predictions = audit_dir / "predictions.csv"
    metrics = prediction_metrics(rows, predictions, eligibility)
    violations = api_violations(report) + raw_model_violations(report, rows)
    roots = cold_root_audit(report, rows)
    checks = {
        "audit_run_succeeded": receipt["exit_code"] == 0,
        "input_frozen": receipt["input_sha256"] == eligibility["input_sha256"],
        "audit_report_hash": receipt["report_sha256"] == sha256(audit_dir / "native.json"),
        "prediction_hash": receipt["predictions_sha256"] == sha256(predictions),
        "all_eligible_rows_served": metrics["served"] == eligibility["eligible_rows"],
        "all_valid_slices_fully_served": metrics["fully_served_slices"] == eligibility["required_slices"],
        "containment_995": metrics["containment_of_served"] >= eligibility["containment_min"],
        "actual_api_shape_parity_calendar": not violations,
        "identified_node_cold_root_accuracy": roots["strict_accuracy_passed"],
    }
    audit = report.get("graded_stats", {}).get("acceptance_audit", {})
    slices = {entry["chain_id"]: entry for snapshot in audit.get("snapshots", []) for entry in snapshot}
    with predictions.open(newline="", encoding="utf-8-sig") as stream:
        predicted = {int(row["row_id"]): row for row in csv.DictReader(stream)}
    eligible_ids = set(eligibility["eligible_row_ids"])
    row_failures = []
    metadata_failures = []
    for row in rows:
        if row["row_id"] not in eligible_ids:
            continue
        entry = slices.get(row["chain_id"])
        if entry is None or entry["grade"] not in ("A", "B"):
            row_failures.append({"row_id": row["row_id"], "kind": "missing_published_slice"})
            metadata_failures.append({"row_id": row["row_id"], "kind": "missing_published_slice"})
            continue
        if "_query_index" not in entry:
            entry["_query_index"] = {(q["strike"], q["side"]): q for q in entry["queries"]}
        query = entry["_query_index"].get((row["strike"], "call" if row["is_call"] else "put"))
        prediction = predicted.get(row["row_id"], {})
        value = float(prediction.get("predicted_price") or "nan")
        if query is None or query["status"] not in ("success", "price_only") or \
           query["price"] is None or not math.isfinite(value) or \
           abs(value - query["price"]) > price_tolerance(entry["bounds"], row["strike"]):
            row_failures.append({"row_id": row["row_id"], "kind": "prediction_not_actual_api"})
        if query is None or not prediction_metadata_matches(query, prediction):
            metadata_failures.append({"row_id": row["row_id"], "kind": "metadata_not_actual_api"})
    checks["every_eligible_prediction_matches_actual_api"] = not row_failures
    checks["every_eligible_metadata_matches_actual_api"] = not metadata_failures
    result = {"schema": "atx-vol-spy-acceptance-v1", "checks": checks,
              "correctness_passed": all(checks.values()), "goal_achieved": False,
              "metrics": metrics, "api_violation_count": len(violations),
              "api_violations_first100": violations[:100], "row_failure_count": len(row_failures),
              "row_failures_first100": row_failures[:100],
              "metadata_failure_count": len(metadata_failures),
              "metadata_failures_first100": metadata_failures[:100],
              "cold_root_audit": roots,
              "eligibility_sha256": hashlib.sha256(json.dumps(eligibility, sort_keys=True).encode()).hexdigest(),
              "evidence_scope": "Static American node bounds, all-pair vertical spreads, global "
                  "convex envelopes, affine call-put, strike-specific terminal European lower "
                  "bounds including analytical between-node extrema, and all-pair calendar on actual queried PWL "
                  "domain. Between-knot preservation follows PWL linearity. No extrapolation, "
                  "model-realizability, discrete-dividend model accuracy or live-tick claim."}
    if args.command == "qualify":
        manifest = read_json(args.cold_manifest)
        timing = cold_timing_checks(manifest, args.cold_manifest.parent, receipt, report)
        cache = args.build_cache.read_text(encoding="utf-8-sig") if args.build_cache else ""
        timing["thin_lto_build_evidence"] = any(
            line.startswith("CMAKE_INTERPROCEDURAL_OPTIMIZATION:") and line.endswith("=ON")
            for line in cache.splitlines())
        result["timing_checks"] = timing
        result["goal_achieved"] = result["correctness_passed"] and all(timing.values())
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", choices=("freeze", "feasibility", "predictions", "run-audit",
                                            "verify", "qualify", "cold-roots"))
    parser.add_argument("--input", required=True, type=Path)
    parser.add_argument("--out", required=True, type=Path)
    parser.add_argument("--predictions", type=Path)
    parser.add_argument("--per-slice", action="store_true")
    parser.add_argument("--european-floors", action="store_true")
    parser.add_argument("--repair-floor-asks", action="store_true")
    parser.add_argument("--binary", type=Path)
    parser.add_argument("--policy", type=Path)
    parser.add_argument("--audit-dir", type=Path)
    parser.add_argument("--cold-manifest", type=Path)
    parser.add_argument("--build-cache", type=Path)
    args = parser.parse_args()
    eligibility = freeze(args.input)
    if args.command == "run-audit":
        print(json.dumps(run_audit(args), indent=2))
        return
    if args.command in ("verify", "qualify"):
        result = verify(args, eligibility)
    elif args.command == "cold-roots":
        report = read_json(args.audit_dir / "native.json")
        receipt = read_json(args.audit_dir / "receipt.json")
        rows = load_quotes(args.input)
        result = cold_root_audit(report, rows)
        result["raw_model_failures"] = raw_model_violations(report, rows)
        result["report_hash_verified"] = sha256(args.audit_dir / "native.json") == receipt["report_sha256"]
        result["strict_accuracy_passed"] &= not result["raw_model_failures"] and result["report_hash_verified"]
    elif args.command == "freeze":
        result = eligibility
    elif args.command == "predictions":
        result = prediction_metrics(load_quotes(args.input), args.predictions, eligibility)
    else:
        rows = load_quotes(args.input)
        if args.per_slice:
            result = {}
            spot = snapshot_spot([row for row in rows if invalid_reason(row) is None])
            for expiry in sorted({row["expiry_days"] for row in rows if invalid_reason(row) is None}):
                group = [row for row in rows if row["expiry_days"] == expiry]
                result[str(expiry)] = feasibility(group, joint=False, spot=spot,
                                                   european_floors=args.european_floors,
                                                   repair_floor_asks=args.repair_floor_asks)
        else:
            result = feasibility(rows, european_floors=args.european_floors,
                                  repair_floor_asks=args.repair_floor_asks)
    result["environment"] = {"python": platform.python_version(), "platform": platform.platform()}
    write_json(args.out, result)
    print(json.dumps({key: value for key, value in result.items()
                      if key not in ("chains", "eligible_row_ids", "metrics",
                                     "api_violations_first100", "row_failures_first100",
                                     "metadata_failures_first100", "cold_root_audit",
                                     "failures", "roundoff_only")}, indent=2))
    if args.command == "verify" and not result["correctness_passed"]:
        raise SystemExit(2)
    if args.command == "qualify" and not result["goal_achieved"]:
        raise SystemExit(2)


if __name__ == "__main__":
    main()
