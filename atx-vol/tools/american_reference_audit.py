#!/usr/bin/env python3
"""Independent QuantLib finite-difference references for synthetic American audits.

This is an offline diagnostic. It neither reads vendor labels nor changes fits.
Refinement changes are uncertainty diagnostics, never rigorous error bounds.
QuantLib is optional for the library and required only when this script is run.
"""
from __future__ import annotations

import argparse
import hashlib
import importlib.metadata
import json
import math
import platform
import sys
import time
from collections import Counter
from dataclasses import asdict, dataclass
from fractions import Fraction
from pathlib import Path

import QuantLib as ql

INTERVAL_FIELDS = {"calendar_end", "variance_end", "rate", "continuous_yield",
                   "volatility", "cash_at_end"}
SOURCE_ROOT = "https://github.com/lballabio/QuantLib/blob/v1.44/"
PRIMARY_SOURCES = [SOURCE_ROOT + path for path in (
    "ql/pricingengines/vanilla/fdblackscholesvanillaengine.cpp",
    "ql/methods/finitedifferences/operators/fdmblackscholesop.cpp",
    "ql/methods/finitedifferences/meshers/fdmblackscholesmesher.cpp",
    "ql/methods/finitedifferences/stepconditions/fdmstepconditioncomposite.cpp",
    "ql/methods/finitedifferences/utilities/fdmdividendhandler.cpp",
)]


@dataclass(frozen=True)
class AuditConfig:
    time_steps: int = 200
    space_nodes: int = 400
    levels: int = 3
    damping_steps: int = 2
    absolute_tolerance: float = 0.001
    relative_tolerance: float = 0.000001
    max_synthetic_days: int = 36500
    max_unique_contracts: int = 5000

    def validate(self):
        for key, lower, upper in (("time_steps", 4, 4096), ("space_nodes", 20, 8192),
                                   ("levels", 2, 5), ("damping_steps", 0, 16),
                                   ("max_synthetic_days", 365, 36500),
                                   ("max_unique_contracts", 1, 10000)):
            value = getattr(self, key)
            if isinstance(value, bool) or not isinstance(value, int) or not lower <= value <= upper:
                raise ValueError(f"{key} outside supported bounds [{lower}, {upper}]")
        if self.time_steps * 2**self.levels > 16384 or self.space_nodes * 2**self.levels > 32768:
            raise ValueError("refined reference grid exceeds resource limits")
        for key in ("absolute_tolerance", "relative_tolerance"):
            if not math.isfinite(getattr(self, key)) or getattr(self, key) < 0:
                raise ValueError(f"{key} must be finite and nonnegative")


class UnsupportedReference(ValueError):
    """The independent solver does not represent this input convention."""


def finite(value, name: str, *, minimum=None, strict=False) -> float:
    if isinstance(value, bool) or not isinstance(value, (float, int)) or not math.isfinite(value):
        raise ValueError(f"{name} must be finite numeric data")
    if minimum is not None and (value <= minimum if strict else value < minimum):
        raise ValueError(f"{name} is outside its domain")
    return float(value)


def validate_intervals(intervals: list[dict]) -> list[dict]:
    if not isinstance(intervals, list) or not 1 <= len(intervals) <= 64:
        raise ValueError("intervals must contain 1..64 entries")
    previous_calendar = previous_variance = 0.0
    result = []
    for entry in intervals:
        if not isinstance(entry, dict) or set(entry) != INTERVAL_FIELDS:
            raise UnsupportedReference("interval schema is not supported; local volatility is not inferred")
        current = {key: finite(entry[key], key) for key in INTERVAL_FIELDS}
        if (current["calendar_end"] <= previous_calendar
                or current["variance_end"] < previous_variance
                or current["volatility"] < 0 or current["cash_at_end"] < 0):
            raise ValueError("intervals require increasing calendar, nondecreasing variance and nonnegative vol/cash")
        previous_calendar, previous_variance = current["calendar_end"], current["variance_end"]
        result.append(current)
    return result


def exact_date_map(intervals: list[dict], max_days: int) -> dict:
    """Represent decimal JSON endpoint ratios exactly, or explicitly refuse.

    Synthetic dates are a coordinate, not actual contract dates. Discount and
    total variance curves preserve the original integrals. No calendar/variance
    endpoint is rounded to a business day or to a numerical time step.
    """
    end = Fraction(str(intervals[-1]["calendar_end"]))
    fractions = [Fraction(str(item["calendar_end"])) / end for item in intervals]
    denominator = 1
    for fraction in fractions:
        denominator = math.lcm(denominator, fraction.denominator)
        if denominator > max_days:
            raise UnsupportedReference("exact interval/date map exceeds max_synthetic_days")
    horizon_days = denominator * max(1, math.ceil(365 / denominator))
    if horizon_days > max_days:
        raise UnsupportedReference("exact interval/date map exceeds max_synthetic_days")
    offsets = [int(fraction * horizon_days) for fraction in fractions]
    assert all(Fraction(day, horizon_days) == fraction for day, fraction in zip(offsets, fractions))
    return {"method": "exact rational decimal endpoint ratios; Actual365Fixed synthetic dates",
            "horizon_days": horizon_days, "day_offsets": offsets,
            "calendar_years": float(end), "synthetic_years": horizon_days / 365,
            "endpoint_ratios": [str(value) for value in fractions], "date_rounding": False}


@dataclass
class ReferenceProcess:
    process: object
    dividends: object
    today: object
    maturity: object
    day_counter: object
    clock_map: dict
    integrated_rate: float
    integrated_yield: float
    integrated_variance: float
    has_cash: bool


def variance_curve_vols(times: list[float], variances: list[float]) -> tuple[list[float], list[float]]:
    """Preserve exactly flat variance segments through QuantLib's vol interface.

    QuantLib reconstructs variance as time*vol*vol. Independent sqrt roundoff
    can make a zero-variance interval decrease by one ulp and be rejected. Find
    a common representable variance for each plateau within a bounded ulp
    neighborhood; never add fictitious positive variance to a stopped clock.
    """
    vols, represented = [], []
    first = 0
    while first < len(times):
        last = first + 1
        target = variances[first]
        while last < len(times) and variances[last] == target:
            last += 1
        candidates = []
        for t in times[first:last]:
            initial = math.sqrt(target / t)
            values = [initial]
            lower = upper = initial
            for _ in range(8):
                lower, upper = math.nextafter(lower, 0), math.nextafter(upper, math.inf)
                values.extend([lower, upper])
            candidates.append({t * value * value: value for value in values})
        common = set(candidates[0])
        for possible in candidates[1:]:
            common.intersection_update(possible)
        if not common:
            raise UnsupportedReference("flat variance cannot be represented through the QuantLib vol interface")
        selected = min(common, key=lambda value: (abs(value - target), value))
        if (represented and selected < represented[-1]) or (
                abs(selected - target) > 64 * sys.float_info.epsilon * target):
            raise UnsupportedReference("variance roundtrip exceeds floating-point tolerance")
        vols.extend(possible[selected] for possible in candidates)
        represented.extend([selected] * (last - first))
        first = last
    return vols, represented


def build_process(spot: float, intervals: list[dict], config: AuditConfig) -> ReferenceProcess:
    mapping = exact_date_map(intervals, config.max_synthetic_days)
    today, dc = ql.Date(1, 1, 2000), ql.Actual365Fixed()
    dates = [today + day for day in mapping["day_offsets"]]
    rate_integral = yield_integral = variance = previous_calendar = previous_variance = 0.0
    discounts, yields, variances = [1.0], [1.0], []
    forward = spot
    cash = False
    for date, entry in zip(dates, intervals):
        dt = entry["calendar_end"] - previous_calendar
        dv = entry["variance_end"] - previous_variance
        rate_integral += entry["rate"] * dt
        yield_integral += entry["continuous_yield"] * dt
        variance += entry["volatility"]**2 * dv
        if max(abs(rate_integral), abs(yield_integral), variance) > 100:
            raise UnsupportedReference("integrated market inputs exceed the reference domain")
        discounts.append(math.exp(-rate_integral))
        yields.append(math.exp(-yield_integral))
        variances.append(variance)
        forward = forward * math.exp((entry["rate"] - entry["continuous_yield"]) * dt)
        forward -= entry["cash_at_end"]
        if forward <= 0:
            raise UnsupportedReference("cash forward becomes nonpositive for QuantLib's automatic mesher")
        cash = cash or entry["cash_at_end"] != 0
        previous_calendar, previous_variance = entry["calendar_end"], entry["variance_end"]
    if variance <= 0:
        raise UnsupportedReference("zero integrated variance: use a deterministic stopping reference")
    rates = ql.YieldTermStructureHandle(ql.DiscountCurve([today, *dates], discounts, dc))
    carry = ql.YieldTermStructureHandle(ql.DiscountCurve([today, *dates], yields, dc))
    vols, represented = variance_curve_vols([dc.yearFraction(today, date) for date in dates], variances)
    mapping["requested_total_variance"] = variances
    mapping["represented_total_variance"] = represented
    mapping["variance_roundtrip_max_abs_error"] = max(abs(a - b) for a, b in zip(variances, represented))
    volatility = ql.BlackVolTermStructureHandle(ql.BlackVarianceCurve(today, dates, vols, dc))
    process = ql.BlackScholesMertonProcess(ql.QuoteHandle(ql.SimpleQuote(spot)), carry,
                                          rates, volatility)
    # Zero-cash entries force rollback stops at every parameter/clock breakpoint.
    # Omit a zero-cash terminal event to match the plain constant-parameter engine.
    events = [(date, entry["cash_at_end"]) for i, (date, entry) in enumerate(zip(dates, intervals))
              if i + 1 < len(intervals) or entry["cash_at_end"] != 0]
    dividends = ql.DividendVector([date for date, _ in events], [amount for _, amount in events])
    return ReferenceProcess(process, dividends, today, dates[-1], dc, mapping,
                            rate_integral, yield_integral, variance, cash)


def fd_price(model: ReferenceProcess, spot: float, strike: float, side: str, exercise: str,
             *, time_steps: int, space_nodes: int, damping_steps: int,
             epsilon: float = 0.0001, domain_scale: float = 1.5) -> dict:
    """QuantLib's vanilla FD construction, exposing its otherwise fixed domain knobs."""
    payoff = ql.PlainVanillaPayoff(ql.Option.Call if side == "call" else ql.Option.Put, strike)
    exercise_spec = (ql.AmericanExercise(model.today, model.maturity) if exercise == "american"
                     else ql.EuropeanExercise(model.maturity))
    maturity = model.clock_map["synthetic_years"]
    mesher1d = ql.FdmBlackScholesMesher(space_nodes, model.process, maturity, strike,
                                      None, None, epsilon, domain_scale, (strike, 0.1),
                                      model.dividends)
    mesher = ql.FdmMesherComposite(mesher1d)
    calculator = ql.FdmLogInnerValue(payoff, mesher, 0)
    conditions = ql.FdmStepConditionComposite.vanillaComposite(
        model.dividends, exercise_spec, mesher, calculator, model.today, model.day_counter)
    desc = ql.FdmSolverDesc(mesher, ql.FdmBoundaryConditionSet(), conditions, calculator,
                            maturity, time_steps, damping_steps)
    op = ql.FdmBlackScholesOp(mesher, model.process, strike, False)
    solver = ql.Fdm1DimSolver(desc, ql.FdmSchemeDesc.Douglas(), op)
    started = time.perf_counter()
    price = solver.interpolateAt(math.log(spot))
    seconds = time.perf_counter() - started
    if not math.isfinite(price):
        raise RuntimeError("QuantLib returned a nonfinite reference")
    locations = mesher1d.locations()
    return {"price": price, "time_steps": time_steps, "space_nodes": space_nodes,
            "damping_steps": damping_steps, "mesh_epsilon": epsilon, "domain_scale": domain_scale,
            "minimum_stock": math.exp(locations[0]), "maximum_stock": math.exp(locations[-1]),
            "rollback_seconds": seconds}


def european_lognormal(spot: float, strike: float, side: str,
                       rate_integral: float, yield_integral: float, variance: float) -> float:
    sd = math.sqrt(variance)
    d1 = (math.log(spot / strike) + rate_integral - yield_integral + variance / 2) / sd
    d2 = d1 - sd
    def normal(value):
        return math.erfc(-value / math.sqrt(2)) / 2
    s, k = spot * math.exp(-yield_integral), strike * math.exp(-rate_integral)
    return (s * normal(d1) - k * normal(d2) if side == "call"
            else k * normal(-d2) - s * normal(-d1))


def reference(spot: float, intervals: list[dict], strike: float, side: str,
               exercise: str, config: AuditConfig) -> dict:
    """Return a refinement study; observed stability never certifies exactness."""
    try:
        model = build_process(spot, intervals, config)
    except UnsupportedReference as error:
        return {"status": "unsupported", "reason": str(error)}
    except (RuntimeError, OverflowError) as error:
        return {"status": "solver_failure", "reason": str(error)}
    series = []
    try:
        for level in range(config.levels):
            result = fd_price(model, spot, strike, side, exercise,
                               time_steps=config.time_steps * 2**level,
                               space_nodes=config.space_nodes * 2**level,
                               damping_steps=config.damping_steps)
            series.append({"name": f"joint_refinement_{level}", **result})
        fine, previous = series[-1], series[-2]
        probes = {}
        for name, time_factor, space_factor, epsilon, scale in (
                ("time", 2, 1, .0001, 1.5), ("space", 1, 2, .0001, 1.5),
                ("domain", 1, 2, .0000001, 2.0)):
            probes[name] = fd_price(model, spot, strike, side, exercise,
                                    time_steps=fine["time_steps"] * time_factor,
                                    space_nodes=fine["space_nodes"] * space_factor,
                                    damping_steps=config.damping_steps,
                                    epsilon=epsilon, domain_scale=scale)
    except RuntimeError as error:
        return {"status": "solver_failure", "reason": str(error), "clock_map": model.clock_map,
                "refinements": series}
    changes = {"joint": abs(fine["price"] - previous["price"]),
               **{key: abs(value["price"] - fine["price"]) for key, value in probes.items()}}
    uncertainty = max(changes.values())
    threshold = config.absolute_tolerance + config.relative_tolerance * abs(fine["price"])
    analytic = None
    if not model.has_cash and (exercise == "european" or (
            side == "call" and all(i["continuous_yield"] == 0 and i["rate"] >= 0
                                   for i in intervals))):
        analytic = european_lognormal(spot, strike, side, model.integrated_rate,
                                       model.integrated_yield, model.integrated_variance)
    analytic_difference = None if analytic is None else fine["price"] - analytic
    intrinsic = max(spot - strike if side == "call" else strike - spot, 0)
    shortfall = max(0, (intrinsic if exercise == "american" else 0) - fine["price"])
    stable = uncertainty <= threshold and shortfall <= threshold
    if analytic_difference is not None:
        stable = stable and abs(analytic_difference) <= threshold
    return {"status": "observed_stable" if stable else "unresolved",
            "reference_price": fine["price"], "refinement_uncertainty_estimate": uncertainty,
            "stability_threshold": threshold, "refinement_changes": changes,
            "clock_map": model.clock_map, "refinements": series, "probes": probes,
            "analytic_reference_price": analytic,
            "analytic_reference_difference": analytic_difference,
            "payoff_lower_bound_shortfall": shortfall,
            "cash_boundary_scope": "cash shift clamps at finite mesher minimum stock; domain probe included"
                if model.has_cash else "no cash jumps",
            "uncertainty_scope": "observed grid/domain differences, not a proven error bound"}


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def summary(rows: list[dict]) -> dict:
    differences = [row["difference"] for row in rows if row.get("difference") is not None]
    return {"rows": len(rows), "reference_status_counts": dict(Counter(row["status"] for row in rows)),
            "compared_rows": len(differences),
            "mean_difference": sum(differences) / len(differences) if differences else None,
            "mae": sum(map(abs, differences)) / len(differences) if differences else None,
            "max_abs_difference": max(map(abs, differences), default=None)}


def audit_document(document: dict, config: AuditConfig, *, exercise="american",
                    selected_cases: set[str] | None = None, progress=None) -> dict:
    config.validate()
    if (type(document.get("schema_version")) is not int
            or document["schema_version"] not in (1, 2)
            or not isinstance(document.get("cases"), list)):
        raise ValueError("expected synthetic audit schema_version 1 or 2 with cases")
    if exercise not in ("american", "european", "all"):
        raise ValueError("unsupported exercise selection")
    cases, requests = [], {}
    for case in document["cases"]:
        if selected_cases and case.get("fixture") not in selected_cases:
            continue
        spot = finite(case["spot"], "spot", minimum=0, strict=True)
        output = {"fixture": case.get("fixture"), "grid": case.get("grid"), "spot": spot,
                  "source_model_status": case.get("model_status"),
                  "source_pricing_status": case.get("pricing_status"), "rows": [], "excluded_rows": 0}
        try:
            intervals = validate_intervals(case["intervals"])
        except UnsupportedReference as error:
            output["unsupported_reason"] = str(error)
            intervals = None
        contracts = set()
        for row in case.get("prices", []):
            if row["side"] not in ("call", "put") or row["exercise"] not in ("american", "european"):
                raise ValueError("unknown option side or exercise")
            if exercise != "all" and row["exercise"] != exercise:
                output["excluded_rows"] += 1
                continue
            expiry = row["expiry"]
            if isinstance(expiry, bool) or not isinstance(expiry, int) or not 0 <= expiry < len(case["intervals"]):
                raise ValueError("price row has invalid expiry index")
            strike = finite(row["strike"], "strike", minimum=0, strict=True)
            price = finite(row["price"], "price", minimum=0)
            identity = (expiry, strike, row["side"], row["exercise"])
            if identity in contracts:
                raise ValueError("duplicate price contract in source case")
            contracts.add(identity)
            item = {"expiry": expiry, "strike": strike, "side": row["side"],
                    "exercise": row["exercise"], "source_price": price}
            if intervals is None:
                item.update(status="unsupported", reason=output["unsupported_reason"])
            else:
                arguments = {"spot": spot, "intervals": intervals[:expiry + 1], "strike": strike,
                             "side": row["side"], "exercise": row["exercise"]}
                key = json.dumps(arguments, sort_keys=True, allow_nan=False)
                if key not in requests:
                    if len(requests) >= config.max_unique_contracts:
                        raise ValueError("audit exceeds max_unique_contracts")
                    requests[key] = {"reference_id": len(requests), "arguments": arguments}
                item["reference_id"] = requests[key]["reference_id"]
            output["rows"].append(item)
        cases.append(output)
    if not cases:
        raise ValueError("no audit cases selected")
    fine_scale = 2**(config.levels - 1)
    work_per_contract = config.time_steps * config.space_nodes * (
        sum(4**i for i in range(config.levels)) + 6 * fine_scale**2)
    if work_per_contract * len(requests) > 100_000_000_000:
        raise ValueError("reference grid work exceeds bounded audit budget; shard the cases")
    references = []
    # QuantLib settings are global. This offline tool is intentionally serial;
    # restore the caller's date even if a reference fails.
    original_date = ql.Settings.instance().evaluationDate
    try:
        ql.Settings.instance().evaluationDate = ql.Date(1, 1, 2000)
        for request in requests.values():
            outcome = reference(**request["arguments"], config=config)
            references.append({**request, **outcome})
            if progress:
                progress(len(references), len(requests))
    finally:
        ql.Settings.instance().evaluationDate = original_date
    for case in cases:
        for row in case["rows"]:
            if "reference_id" not in row:
                continue
            ref = references[row["reference_id"]]
            row["status"] = ref["status"]
            row["reference_price"] = ref.get("reference_price")
            row["refinement_uncertainty_estimate"] = ref.get("refinement_uncertainty_estimate")
            row["difference"] = (row["source_price"] - ref["reference_price"]
                                 if "reference_price" in ref else None)
        case["summary"] = summary(case["rows"])
        case["observed_stable_summary"] = summary(
            [row for row in case["rows"] if row["status"] == "observed_stable"])
        case["near_atm_summary"] = summary(
            [row for row in case["rows"] if abs(row["strike"] / case["spot"] - 1) < .05])
    package = Path(ql.__file__).resolve().parent
    native_modules = sorted([*package.glob("*.pyd"), *package.glob("*.so")])
    return {"schema_version": 1, "source_schema_version": document["schema_version"],
            "method": "QuantLib FdmBlackScholesOp/Fdm1DimSolver, Douglas with implicit damping",
            "dependency": {"QuantLib": ql.__version__, "distribution": importlib.metadata.version("QuantLib"),
                           "python": platform.python_version(), "platform": platform.platform(),
                           "native_modules": [{"name": p.name, "sha256": sha256(p)} for p in native_modules]},
            "settings": asdict(config), "estimated_grid_updates": work_per_contract * len(requests),
            "primary_sources": PRIMARY_SOURCES,
            "exercise_selection": exercise, "references": references, "cases": cases,
            "summary": summary([row for case in cases for row in case["rows"]]),
            "notes": ["independent continuous-American target; both solvers still discretize exercise",
                      "observed_stable means refinement diagnostics passed, not exactness or a certificate",
                      "all differences are source minus independent reference in price units",
                      "unique contracts are reused across source grids without inspecting source price",
                      "cash floor and tail effects are probed, not bounded rigorously",
                      "local-volatility input is explicitly unsupported"]}


def unique_object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError(f"duplicate JSON key: {key}")
        result[key] = value
    return result


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--case", action="append", default=[])
    parser.add_argument("--exercise", choices=["american", "european", "all"], default="american")
    for name in ("time_steps", "space_nodes", "levels", "damping_steps",
                 "max_synthetic_days", "max_unique_contracts"):
        parser.add_argument("--" + name.replace("_", "-"), type=int,
                             default=getattr(AuditConfig(), name))
    for name in ("absolute_tolerance", "relative_tolerance"):
        parser.add_argument("--" + name.replace("_", "-"), type=float,
                             default=getattr(AuditConfig(), name))
    args = parser.parse_args(argv)
    try:
        if args.output.exists():
            raise ValueError("output already exists; use a new path")
        if args.input.stat().st_size > 64 * 1024 * 1024:
            raise ValueError("input exceeds 64 MiB")
        content = args.input.read_bytes()
        document = json.loads(content.decode("utf-8-sig"), object_pairs_hook=unique_object)
        config = AuditConfig(**{key: getattr(args, key) for key in asdict(AuditConfig())})
        def progress(done, total):
            if done % 50 == 0 or done == total:
                print(f"reference contracts {done}/{total}", file=sys.stderr, flush=True)
        report = audit_document(document, config, exercise=args.exercise,
                                 selected_cases=set(args.case), progress=progress)
        report["input"] = str(args.input.resolve())
        report["input_sha256"] = hashlib.sha256(content).hexdigest()
        report["tool_sha256"] = sha256(Path(__file__))
        with args.output.open("x", encoding="utf-8") as stream:
            json.dump(report, stream, indent=2, allow_nan=False)
            stream.write("\n")
        print(json.dumps(report["summary"], allow_nan=False))
        return 0
    except (OSError, ValueError, RuntimeError, OverflowError) as error:
        print(f"american-reference-audit: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
