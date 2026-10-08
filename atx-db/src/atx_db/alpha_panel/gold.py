"""Publish point-in-time gold scores, selected only from purged 2020-2021 evidence.

The evaluator's 2022-2023 validation and later returns cannot determine feature,
horizon, sign, variant or weights. Daily normalization uses the current eligible
cross section. Literature signs are already applied in the characteristic registry.
"""
from __future__ import annotations

import argparse
import collections
import datetime as dt
import math
import os
from pathlib import Path
from statistics import NormalDist
from typing import Any, Sequence

import numpy as np
import pyarrow as pa
import pyarrow.compute as pc
import pyarrow.parquet as pq

from . import char_registry as R
from . import common as C
from .characteristics import panel_stage

SCHEMA = "atx.alpha-panel.gold/v1"
HORIZONS = (5, 21, 63)
CUTOFF = "2023-12-29"
_NORMAL = NormalDist()


def _finite(value: Any) -> bool:
    return isinstance(value, (int, float)) and math.isfinite(value)


def select(report: dict[str, Any], features: Sequence[R.Feature] = R.REGISTRY) -> dict[str, Any]:
    """Apply predeclared gates; fail closed on incomplete chronological evidence.

    Admission coverage uses 2019-2021 only. Later coverage is diagnostic.
    No validation metric is read.
    Missing pairwise correlations exclude a candidate rather than silently declaring
    it independent. Controls ship regardless of their IC.
    """
    if report.get("label_cutoff") != CUTOFF:
        raise ValueError("gold requires the sealed 2023-12-29 label cutoff")
    if "SELECTION" not in report.get("windows", {}):
        raise ValueError("gold requires a purged SELECTION window")
    if report["windows"]["SELECTION"][1] >= "2022-01-01":
        raise ValueError("SELECTION reaches the validation window")
    protocol = report.get("validation_protocol", {})
    ends = protocol.get("selection_decision_ends", {})
    purge = protocol.get("purge_sessions", {})
    if not all(ends.get(f"h{h}") and ends[f"h{h}"] < "2022-01-01" and purge.get(f"h{h}") == h + 1
               for h in HORIZONS):
        raise ValueError("gold requires per-horizon purged SELECTION evidence")
    redundancy = report.get("pairwise_redundancy", {})
    if redundancy.get("window") != "SELECTION":
        raise ValueError("redundancy must use SELECTION evidence only")
    pairs = redundancy.get("pairs", {})
    candidates, audit, controls = [], {}, []
    for f in features:
        if f.kind == "control":
            controls.append(f.name)
            continue
        e = report.get("features", {}).get(f"characteristics:{f.name}", {})
        cov = e.get("coverage", {})
        yearly = [cov.get(str(y)) for y in range(2019, 2022)]
        coverage = all(_finite(v) and v >= .30 for v in yearly)
        # Mean of daily shares, weighted by observed sessions when counts are supplied.
        counts = e.get("coverage_sessions", {})
        weights = [counts.get(str(y), 0) for y in range(2019, 2022)]
        avg = (sum(v * w for v, w in zip(yearly, weights)) / sum(weights)
               if all(_finite(v) for v in yearly) and sum(weights) else None)
        coverage = coverage and avg is not None and avg >= .50
        options = []
        for h in HORIZONS:
            stats = e.get("ic", {}).get(f"h{h}", {}).get("member_equity", {})
            sel, discovery = stats.get("SELECTION") or {}, stats.get("DISCOVERY") or {}
            for variant, mean, tkey in (("raw", "ic_mean", "nw_t"),
                                        ("industry", "ind_ic_mean", "ind_nw_t")):
                ic, t, confirmation = sel.get(mean), sel.get(tkey), discovery.get(mean)
                if (_finite(ic) and ic > 0 and _finite(t) and t >= 2.5
                        and _finite(confirmation) and confirmation > 0):
                    options.append({"horizon": h, "variant": variant, "selection_t": t,
                                    "selection_ic": ic, "discovery_ic": confirmation})
        best = max(options, key=lambda o: (o["selection_t"], -o["horizon"], o["variant"] == "raw")) if options else None
        audit[f.name] = {"family": f.family, "coverage_mean": avg, "coverage_pass": coverage,
                         "coverage_diagnostics": cov,
                         "selection_discovery_pass": best is not None, "selected": False,
                         "evidence": best, "redundancy_pass": None}
        if coverage and best:
            candidates.append((f, best))
    accepted = []
    for f, best in sorted(candidates, key=lambda x: (-x[1]["selection_t"], x[0].name)):
        reject = None
        for prior in accepted:
            a, b = f"characteristics:{f.name}", f"characteristics:{prior['name']}"
            rho = pairs.get(f"{a}|{b}", pairs.get(f"{b}|{a}"))
            if not _finite(rho) or abs(rho) >= .90:
                reject = {"with": prior["name"], "rho": rho,
                          "reason": "missing evidence" if not _finite(rho) else "redundant"}
                break
        audit[f.name]["redundancy_pass"] = reject is None
        audit[f.name]["redundancy_rejection"] = reject
        if reject is None:
            accepted.append({"name": f.name, "family": f.family, "fill_zero": f.fill_zero, **best})
            audit[f.name]["selected"] = True
    return {"schema": "atx.alpha-panel.gold-selection/v1", "selection_window": report["windows"]["SELECTION"],
            "label_cutoff": CUTOFF, "rules": {"min_coverage": .50, "min_year_coverage": .30,
            "coverage_years": [2019, 2020, 2021], "min_t": 2.5,
            "max_redundancy_exclusive": .90, "discovery_positive": True,
            "validation_used_for_selection": False}, "selected": accepted, "controls": controls, "audit": audit}


def feature_quality_flags(feature: R.Feature) -> list[str]:
    """Source provenance limits are separate from measured predictive quality."""
    flags = ["vendor price/identity historical vintages are not independently certified"]
    # The panel's shares_out is a lag-90 vendor proxy, not a reconstructed SEC vintage.
    # Market equity inherits it even when a feature registry lists only me_company.
    shares_inputs = ("shares_out", "me_company", "me_line")
    if any(any(name in source for name in shares_inputs) for source in feature.inputs):
        flags.append("shares/market equity use lag-90 restated vendor proxy; historical shares vintage unverified")
    if any("iv_atm" in source for source in feature.inputs):
        flags.append("vendor IV snapshot vintages unverified; feature delivery lag is one session")
    if any("borrow_proxy" in source for source in feature.inputs):
        flags.append("borrow data are short-constraint proxies, not observed borrow fees")
    return flags


def frozen_recipe(selection: dict[str, Any]) -> dict[str, Any]:
    """Predeclare every score, horizon and missing-aware equal composite weight."""
    features = {f"g_{f['name']}": {"raw_feature": f["name"], "family": f["family"],
        "selected_horizon": f["horizon"], "variant": f["variant"], "fill_zero": f["fill_zero"],
        "sign": "registry prior sign already applied"} for f in selection["selected"]}
    families = collections.defaultdict(list)
    for name, entry in features.items():
        families[entry["family"]].append(name)
    composites = {f"c_{family}": {"weights": {n: 1 / len(names) for n in names},
        "minimum_present": math.ceil(len(names) / 2), "missing_weight_policy": "equal weights among present components",
        "normalization": "same-session centre and population standard deviation"}
        for family, names in sorted(families.items()) if len(names) >= 2}
    if len(composites) >= 3:
        composites["c_all"] = {"weights": {n: 1 / len(composites) for n in composites}, "minimum_present": 3,
            "missing_weight_policy": "equal weights among present families",
            "normalization": "same-session centre and population standard deviation"}
    return {"schema": "atx.alpha-panel.gold-frozen-recipe/v1", "features": features, "composites": composites,
            "evaluation_columns": list(features) + list(composites), "evaluation_horizons": list(HORIZONS),
            "controls": [f"ctl_{n}" for n in selection["controls"]],
            "score_normalization": "same-session 1/99 winsorized average-tie rank Gaussian; selected industry variant demeaned",
            "source_repair_policy": "current repaired decisions excluded; trailing 1260-session repair quarantine stays null",
            "return_fitted_weights": False, "out_of_sample_used_for_selection": False}


def standardize(values: np.ndarray) -> np.ndarray:
    """Centre and scale finite values; a constant cross section has no signal."""
    out = np.full(len(values), np.nan)
    ok = np.isfinite(values)
    if ok.sum() < 2:
        return out
    sd = float(np.std(values[ok]))
    if sd > 1e-12:
        out[ok] = (values[ok] - np.mean(values[ok])) / sd
    return out


def normalize(values: np.ndarray, groups: np.ndarray, *, industry: bool = False,
              fill_zero: bool = False) -> np.ndarray:
    """1/99 winsorization, average-tie inverse-normal ranks and optional FF49 demeaning."""
    x = np.asarray(values, dtype=float).copy()
    if fill_zero:
        x[~np.isfinite(x)] = 0
    ok = np.isfinite(x)
    out = np.full(len(x), np.nan)
    if ok.sum() < 2:
        return out
    v = np.clip(x[ok], *np.quantile(x[ok], [.01, .99]))
    order = np.argsort(v, kind="stable")
    sorted_v = v[order]
    edges = np.r_[0, np.flatnonzero(sorted_v[1:] != sorted_v[:-1]) + 1, len(v)]
    rank = np.empty(len(v))
    for lo, hi in zip(edges[:-1], edges[1:]):
        rank[order[lo:hi]] = (lo + hi + 1) / 2
    out[ok] = [_NORMAL.inv_cdf(u) for u in (rank - .5) / len(v)]
    if industry:
        for group in set(groups[ok].tolist()):
            if group is None or (isinstance(group, float) and not math.isfinite(group)):
                continue
            use = ok & (groups == group)
            if use.sum() >= 3:
                out[use] -= np.mean(out[use])
    return standardize(out)


def composite(columns: Sequence[np.ndarray], minimum: int) -> np.ndarray:
    arr = np.column_stack(columns)
    n = np.isfinite(arr).sum(axis=1)
    mean = np.divide(np.nansum(arr, axis=1), n, out=np.full(len(n), np.nan), where=n >= minimum)
    return standardize(mean)


def score_session(table: pa.Table, selection: dict[str, Any]) -> pa.Table:
    """Score one complete member_equity cross section, keeping keys and controls."""
    table = table.filter(pc.fill_null(table["member_equity"], False))
    if "source_repaired_now" in table.column_names:
        table = table.filter(pc.invert(pc.fill_null(table["source_repaired_now"], False)))
    groups = np.asarray(table["grp_ff49"].to_pylist(), dtype=object)
    eligible = (np.logical_not(np.asarray(table["source_repaired_window"].to_pylist(), dtype=bool))
                if "source_repaired_window" in table.column_names else np.ones(len(groups), dtype=bool))
    columns = {c: table[c] for c in ("session_date", "security_id", "cik", "grp_ff49", "grp_ff12")}
    scores, families = {}, collections.defaultdict(list)
    for f in selection["selected"]:
        score = np.full(len(groups), np.nan)
        score[eligible] = normalize(table[f["name"]].to_numpy(zero_copy_only=False)[eligible], groups[eligible],
                                   industry=f["variant"] == "industry", fill_zero=f["fill_zero"])
        name = f"g_{f['name']}"
        scores[name] = score
        families[f["family"]].append(score)
    comps = []
    for family, cols in sorted(families.items()):
        if len(cols) >= 2:
            val = composite(cols, math.ceil(len(cols) / 2))
            scores[f"c_{family}"] = val
            comps.append(val)
    if len(comps) >= 3:
        scores["c_all"] = composite(comps, 3)
    for name, score in scores.items():
        columns[name] = pa.array(score, type=pa.float32(), from_pandas=True)
    for name in selection["controls"]:
        columns[f"ctl_{name}"] = table[name].cast(pa.float32())
    return pa.table(columns)


def run(ic_path: Path, years: Sequence[int], memory: str = "300MB") -> dict[str, Any]:
    """Stream one session at a time; write each year atomically and manifest last."""
    report = C.read_json(ic_path)
    selection = select(report)
    root = C.build_root()
    manifests = {s: C.sha256_file(root / s / "manifest.json") for s in ("characteristics", panel_stage())}
    label_stage = report.get("label_stage", "labels")
    if report.get("label_policy") != "strict":
        raise ValueError("production gold requires strictly observed forward labels")
    if not isinstance(label_stage, str) or not label_stage.replace("_", "").isalnum():
        raise ValueError("invalid label stage")
    manifests[label_stage] = C.sha256_file(root / label_stage / "manifest.json")
    evidence = report.get("input_manifests_sha256", {})
    if any(evidence.get(f"{s}/manifest.json") != sha for s, sha in manifests.items()):
        raise ValueError("IC evidence is not bound to the current feature and panel manifests")
    selection["ic_sha256"] = C.sha256_file(ic_path)
    selection["input_manifests_sha256"] = manifests
    dest = C.stage_dir("gold")
    C.write_json_atomic(dest / "selection.json", selection)
    if not selection["selected"]:
        raise ValueError("no feature passed gold gates; audit written, refusing to publish an empty quality claim")
    recipe = frozen_recipe(selection)
    recipe.update(selection_sha256=C.sha256_file(dest / "selection.json"), ic_sha256=selection["ic_sha256"],
                  code=C.code_identity("gold", "char_registry", "characteristics", "common"),
                  input_manifests_sha256=manifests)
    C.write_json_atomic(dest / "frozen_recipe.json", recipe)
    C.write_json_atomic(dest / "manifest.json", {"schema": SCHEMA, "status": "building",
                                               "selection_sha256": C.sha256_file(dest / "selection.json")})
    scratch = f"gold-{os.getpid()}.duckdb"
    con = C.connect(memory=memory, threads=1, db_file=scratch)
    receipt = {}
    names = [f["name"] for f in selection["selected"]] + selection["controls"]
    cols = ", ".join(f'c."{name}"' for name in names)
    try:
        for year in sorted(years):
            if year < 2020 or year > 2026:
                raise ValueError("gold publication years must be 2020-2026")
            cg = (root / "characteristics" / f"year={year}" / "*.parquet").as_posix()
            pg = (root / panel_stage() / f"year={year}" / "*.parquet").as_posix()
            sch = {r[0] for r in con.execute(f"DESCRIBE SELECT * FROM read_parquet('{cg}')").fetchall()}
            repair_cols = ",c.source_repaired_now,c.source_repaired_window" if "source_repaired_now" in sch else ""
            eligible_sql = "member_equity AND NOT source_repaired_now" if repair_cols else "member_equity"
            sessions = con.execute(f"SELECT DISTINCT session_date FROM read_parquet('{cg}') WHERE {eligible_sql} ORDER BY 1").fetchall()
            path = dest / f"year={year}" / "gold.parquet"
            path.parent.mkdir(parents=True, exist_ok=True)
            tmp = path.with_name(path.name + ".partial")
            writer, n = None, 0
            try:
                current_month = None
                for (session,) in sessions:
                    if current_month != session.month:
                        lo = session.replace(day=1)
                        hi = dt.date(year + (session.month == 12), session.month % 12 + 1, 1)
                        con.execute(f"""CREATE OR REPLACE TABLE gold_month AS SELECT
                            c.session_date,c.security_id,p.cik,c.grp_ff49,c.grp_ff12,c.member_equity,{cols}{repair_cols}
                            FROM read_parquet('{cg}') c JOIN read_parquet('{pg}') p USING (session_date,security_id)
                            WHERE c.session_date >= ? AND c.session_date < ? AND c.{eligible_sql}""", [lo, hi])
                        total, unique = con.execute("SELECT count(*),count(DISTINCT (session_date,security_id)) FROM gold_month").fetchone()
                        expected = con.execute(f"SELECT count(*) FROM read_parquet('{cg}') WHERE session_date>=? AND session_date<? AND {eligible_sql}",
                                               [lo, hi]).fetchone()[0]
                        if total != unique or total != expected:
                            raise ValueError("gold keys are duplicate or a characteristic row lacks a matching silver row")
                        current_month = session.month
                    data = con.execute("SELECT * FROM gold_month WHERE session_date=? ORDER BY security_id", [session]).to_arrow_table()
                    scored = score_session(data, selection)
                    if writer is None:
                        writer = pq.ParquetWriter(tmp, scored.schema, compression="zstd")
                    writer.write_table(scored, row_group_size=32768)
                    n += scored.num_rows
            finally:
                if writer is not None:
                    writer.close()
            if writer is None:
                raise ValueError(f"no eligible rows for {year}")
            os.replace(tmp, path)
            receipt[str(year)] = {"rows": n, "sessions": len(sessions), "bytes": path.stat().st_size}
            print("gold", year, receipt[str(year)], flush=True)
    finally:
        con.close()
        for path in (root / "_tmp" / scratch, root / "_tmp" / (scratch + ".wal")):
            path.unlink(missing_ok=True)
    # Refuse publication if an upstream stage changed while the build was running.
    if any(C.sha256_file(root / s / "manifest.json") != sha for s, sha in manifests.items()):
        raise ValueError("upstream manifest changed during gold build")
    quality = {f"g_{f.name}" if f.kind == "signal" else f"ctl_{f.name}": feature_quality_flags(f)
               for f in R.REGISTRY if f.name in names}
    families = collections.defaultdict(list)
    for f in selection["selected"]:
        families[f["family"]].append(f"g_{f['name']}")
    for family, members in families.items():
        if len(members) >= 2:
            quality[f"c_{family}"] = sorted({flag for member in members for flag in quality[member]})
    if sum(len(members) >= 2 for members in families.values()) >= 3:
        quality["c_all"] = sorted({flag for name, flags in quality.items() if name.startswith("c_") for flag in flags})
    C.write_stage_manifest("gold", SCHEMA, ("gold", "char_registry", "common"),
                          {"selection_sha256": C.sha256_file(dest / "selection.json"),
                           "frozen_recipe_sha256": C.sha256_file(dest / "frozen_recipe.json"),
                           "ic_sha256": selection["ic_sha256"], "input_manifests_sha256": manifests,
                           "registry": R.as_manifest([f for f in R.REGISTRY if f.name in names]),
                           "receipt": receipt, "normalization": "same-session member_equity; rank-normal; equal weights",
                           "validation_used_for_selection": False,
                           "quality_status": "selection-qualified candidates; independent validation required",
                           "predictive_power_certified": False, "source_vintages_certified": False,
                           "label_stage": label_stage, "label_policy": "strict",
                           "feature_quality_flags": quality,
                           "horizon_policy": "g_* scores use the frozen selected horizon and variant in selection.json; "
                                             "c_* equal-weight family scores combine selected horizons and are evaluated at each horizon"})
    return receipt


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--ic", type=Path, required=True)
    ap.add_argument("--years", default="2020-2026")
    ap.add_argument("--memory", default="300MB")
    args = ap.parse_args(argv)
    a, b = map(int, args.years.split("-"))
    run(args.ic, range(a, b + 1), args.memory)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
