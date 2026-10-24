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


def verify(args, eligibility):
    rows = load_quotes(args.input)
    audit_dir = args.audit_dir
    report = read_json(audit_dir / "native.json")
    receipt = read_json(audit_dir / "receipt.json")
    predictions = audit_dir / "predictions.csv"
    metrics = prediction_metrics(rows, predictions, eligibility)
    violations = api_violations(report)
    checks = {
        "audit_run_succeeded": receipt["exit_code"] == 0,
        "input_frozen": receipt["input_sha256"] == eligibility["input_sha256"],
        "audit_report_hash": receipt["report_sha256"] == sha256(audit_dir / "native.json"),
        "prediction_hash": receipt["predictions_sha256"] == sha256(predictions),
        "all_eligible_rows_served": metrics["served"] == eligibility["eligible_rows"],
        "all_valid_slices_fully_served": metrics["fully_served_slices"] == eligibility["required_slices"],
        "containment_995": metrics["containment_of_served"] >= eligibility["containment_min"],
        "actual_api_shape_parity_calendar": not violations,
    }
    audit = report.get("graded_stats", {}).get("acceptance_audit", {})
    slices = {entry["chain_id"]: entry for snapshot in audit.get("snapshots", []) for entry in snapshot}
    with predictions.open(newline="", encoding="utf-8-sig") as stream:
        predicted = {int(row["row_id"]): row for row in csv.DictReader(stream)}
    eligible_ids = set(eligibility["eligible_row_ids"])
    row_failures = []
    for row in rows:
        if row["row_id"] not in eligible_ids:
            continue
        entry = slices.get(row["chain_id"])
        if entry is None or entry["grade"] not in ("A", "B"):
            row_failures.append({"row_id": row["row_id"], "kind": "missing_published_slice"})
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
    checks["every_eligible_prediction_matches_actual_api"] = not row_failures
    result = {"schema": "atx-vol-spy-acceptance-v1", "checks": checks,
              "correctness_passed": all(checks.values()), "goal_achieved": False,
              "metrics": metrics, "api_violation_count": len(violations),
              "api_violations_first100": violations[:100], "row_failure_count": len(row_failures),
              "row_failures_first100": row_failures[:100],
              "eligibility_sha256": hashlib.sha256(json.dumps(eligibility, sort_keys=True).encode()).hexdigest(),
              "evidence_scope": "Static American node bounds, all-pair vertical spreads, global "
                  "convex envelopes, affine call-put, strike-specific terminal European lower "
                  "bounds including analytical between-node extrema, and all-pair calendar on actual queried PWL "
                  "domain. Between-knot preservation follows PWL linearity. No extrapolation, "
                  "model-realizability, discrete-dividend model accuracy or live-tick claim."}
    if args.command == "qualify":
        manifest = read_json(args.cold_manifest)
        trials = manifest.get("trials", [])
        timing = {
            "at_least100_fresh_processes": manifest.get("trials_ok", 0) >= 100,
            "same_binary": manifest.get("binary", {}).get("sha256") == receipt["binary_sha256"],
            "same_input": manifest.get("input", {}).get("sha256") == receipt["input_sha256"],
            "same_policy": manifest.get("policy", {}).get("sha256") == receipt["policy_sha256"],
            "same_predictions": manifest.get("predictions_identical") is True and
                manifest.get("predictions_sha256") == receipt["predictions_sha256"],
            "one_pcore": manifest.get("pcore_proof") is True and bool(trials) and
                all(one_core_trial(trial) for trial in trials),
            "idle_sibling": manifest.get("smt_sibling_idle_all_trials") is True,
            "fast_math_off": manifest.get("fast_math") is False,
            "all_cold_under1second": bool(trials) and all(
                t.get("exit_code") == 0 and 0 < t.get("cold_calibration_s", math.inf) < 1
                for t in trials),
            "unaudited_timing_command": "--acceptance-audit" not in manifest.get("command_template", ""),
            "source_clean": manifest.get("source", {}).get("dirty") is False,
        }
        # Inspect every native timing report, not only the launcher summary.
        for trial in trials:
            native_path = args.cold_manifest.parent / trial["report"]
            native = read_json(native_path)
            timing["native_reports_verified"] = timing.get("native_reports_verified", True) and \
                sha256(native_path) == trial.get("report_sha256") and \
                native.get("policy_definition") == report.get("policy_definition") and \
                "acceptance_audit" not in native.get("graded_stats", {})
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
                                            "verify", "qualify"))
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
                                     "api_violations_first100", "row_failures_first100")}, indent=2))
    if args.command == "verify" and not result["correctness_passed"]:
        raise SystemExit(2)
    if args.command == "qualify" and not result["goal_achieved"]:
        raise SystemExit(2)


if __name__ == "__main__":
    main()
