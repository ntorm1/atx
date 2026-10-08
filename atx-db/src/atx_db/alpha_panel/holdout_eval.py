"""Evaluate immutable selected gold scores in authorized 2024-2026 OOS data.

Every frozen score and fixed composite is evaluated at all three predeclared
horizons. There is no ranking, fitted sign, admission, horizon choice or refitting.
The legacy evaluator's training seal remains its default outside this module.
"""

from __future__ import annotations

import argparse
import datetime as dt
import os
from contextlib import contextmanager
from pathlib import Path
from typing import Any

from . import common as C
from . import gold_freeze as F
from . import ic_eval as E


@contextmanager
def _authorized_context(receipt: dict[str, Any], labels_stage: str):
    overrides = {"LABEL_CUTOFF": dt.date.fromisoformat(receipt["oos_cutoff"]),
                 "FIRST_SESSION": F.OOS_START, "TRAIN_START": F.OOS_START,
                 "VALIDATION_START": F.OOS_START, "HORIZONS": F.HORIZONS, "ALLOW_CENSORED_TAIL": True}
    previous = {name: getattr(E, name) for name in overrides}
    env = {name: os.environ.get(name) for name in ("ATX_ALPHA_PANEL_STAGE", "ATX_ALPHA_LABEL_STAGE")}
    try:
        for name, value in overrides.items():
            setattr(E, name, value)
        os.environ["ATX_ALPHA_PANEL_STAGE"] = receipt["panel_stage"]
        os.environ["ATX_ALPHA_LABEL_STAGE"] = labels_stage
        yield
    finally:
        for name, value in previous.items():
            setattr(E, name, value)
        for name, value in env.items():
            if value is None:
                os.environ.pop(name, None)
            else:
                os.environ[name] = value


def write_report(result: dict[str, Any], out: Path) -> None:
    lines = ["# Frozen gold: 2024-2026 out-of-sample evaluation", "",
             f"Freeze receipt SHA-256: `{result['freeze_sha256']}`.", "",
             f"Available outcome cutoff: {result['label_cutoff']}. Decision dates run from 2024-01-01; "
             "each horizon is censored independently when d+1+h exceeds the cutoff.", "",
             "The scores, signs, industry variants, composite weights and normalization were frozen before "
             "the first OOS label build. All frozen scores and all predeclared 5/21/63-session horizons are shown; "
             "OOS outcomes never change admission, signs, horizons or weights.", "",
             "Labels require complete observed close-to-close horizons (d+1 close to d+1+h close), or complete "
             "observed terminal proceeds. Missing/imputed terminal proceeds and repaired decision/entry/return "
             "identities are excluded. IC is conditional on observed outcomes; survivor bias is possible. "
             "HAC confidence intervals account for overlapping labels using actual exchange-session distances. "
             "2026 is incomplete and long-horizon samples end earlier.", "",
             "| Frozen score | h | Window | Sessions | Mean names | Raw IC | HAC t | IC 95% CI | Industry IC | Risk IC |",
             "|---|---:|---|---:|---:|---:|---:|---|---:|---:|"]
    def cell(value: Any) -> str:
        if value is None:
            return "—"
        if isinstance(value, list):
            return "[" + ", ".join(cell(v) for v in value) + "]"
        return str(round(value, 6)) if isinstance(value, float) else str(value)
    for feature in result["frozen_columns"]:
        entry = result["features"][f"gold:{feature}"]
        for h in F.HORIZONS:
            windows = entry.get("ic", {}).get(f"h{h}", {}).get("member_equity", {})
            intervals = [("2024-2026 available", windows.get("FROZEN_OOS"))]
            intervals.extend((str(year), windows.get("years", {}).get(str(year))) for year in (2024, 2025, 2026))
            for label, stats in intervals:
                stats = stats or {}
                vals = (feature, h, label, stats.get("sessions"), stats.get("names_mean"), stats.get("ic_mean"),
                        stats.get("nw_t"), stats.get("ic_ci95"), stats.get("ind_ic_mean"), stats.get("risk_ic_mean"))
                lines.append("| " + " | ".join(cell(v) for v in vals) + " |")
    lines.extend(["", "## Label quality attrition", "", "Counts include all silver decision rows; evaluated IC uses member_equity rows.", "",
                  "| Year | Horizon | Quality code | Rows |", "|---|---|---:|---:|"])
    for year, horizons in sorted(result.get("label_quality_attrition", {}).items()):
        for h, codes in sorted(horizons.items()):
            for code, count in sorted(codes.items()):
                lines.append(f"| {year} | {h} | {code} | {count} |")
    lines.extend(["", "Codes: 1 complete observed horizon; 2 complete observed terminal payoff; 3 unknown terminal; "
                  "4 imputed terminal; 5 incomplete/invalid or missing anchor; 6 endpoint censored; "
                  "7 repaired decision; 8 repaired anchor/forward identity.", "",
                  "## Mean daily frozen-score coverage", "", "| Score | 2024 | 2025 | 2026 |", "|---|---:|---:|---:|"])
    for feature in result["frozen_columns"]:
        coverage = result["features"][f"gold:{feature}"].get("coverage", {})
        lines.append("| " + feature + " | " + " | ".join(cell(coverage.get(str(y))) for y in (2024, 2025, 2026)) + " |")
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text("\n".join(lines) + "\n", encoding="utf-8")


def verify_label_files(stage: str, manifest: dict[str, Any]) -> None:
    directory = C.build_root() / stage
    files = manifest.get("files", {})
    actual = {p.relative_to(directory).as_posix() for p in directory.glob("year=*/*.parquet")}
    if not files or set(files) != actual:
        raise ValueError("OOS label manifest must bind every exact published year file")
    for rel, info in files.items():
        if C.sha256_file(directory / rel) != info["sha256"]:
            raise ValueError(f"OOS label physical file differs from its manifest: {rel}")


def run(freeze_path: Path, out: Path, *, authorized_oos: bool = False,
        labels_stage: str = "labels_holdout", memory: str = "300MB", threads: int = 1,
        report: Path | None = None) -> dict[str, Any]:
    receipt = F.verify(freeze_path, authorized_oos=authorized_oos)
    freeze_sha = C.sha256_file(freeze_path)
    manifest_path = C.build_root() / labels_stage / "manifest.json"
    labels = C.read_json(manifest_path)
    if (labels.get("status") != "complete" or labels.get("label_policy") != "strict"
            or labels.get("freeze_sha256") != freeze_sha or labels.get("label_cutoff") != receipt["oos_cutoff"]):
        raise ValueError("OOS labels must be strict and pinned to this immutable freeze and cutoff")
    label_sha = C.sha256_file(manifest_path)
    verify_label_files(labels_stage, labels)
    with _authorized_context(receipt, labels_stage):
        result = E.run([("gold", receipt["features"])], memory=memory, threads=threads)
    result.update(schema="atx.alpha-panel.frozen-oos-ic/v1", freeze_sha256=freeze_sha,
                  frozen_columns=receipt["features"], frozen_feature_choices=receipt["feature_choices"],
                  frozen_composite_weights=receipt["composite_weights"],
                  windows={"FROZEN_OOS": [receipt["oos_start"], receipt["oos_cutoff"]]},
                  validation_protocol={"frozen_before_outcomes": True, "horizons_predeclared": list(F.HORIZONS),
                                       "outcome_access_authorized": True, "ranking_or_refitting": False,
                                       "normalization": receipt.get("normalization")},
                  outcome_return_years=[2024, 2025, 2026])
    result.pop("train_end", None)
    for entry in result["features"].values():
        for universe in entry["ic"].values():
            for windows in universe.values():
                windows["FROZEN_OOS"] = windows.pop("TRAIN", None)
                for name in ("SELECTION", "VALIDATION", "DISCOVERY"):
                    windows.pop(name, None)
    result["decision_end_by_horizon"] = {}
    con = C.connect(memory="150MB", threads=1)
    try:
        calendar = C.load_calendar(con)
        cut = calendar.index(dt.date.fromisoformat(receipt["oos_cutoff"]))
        result["decision_end_by_horizon"] = {f"h{h}": str(calendar[cut - h - 1]) for h in F.HORIZONS}
    finally:
        con.close()
    # Inputs and recipe remain identical after computation, before publication.
    F.verify(freeze_path, authorized_oos=True)
    if C.sha256_file(freeze_path) != freeze_sha or C.sha256_file(manifest_path) != label_sha:
        raise ValueError("freeze receipt or OOS labels changed during evaluation")
    verify_label_files(labels_stage, labels)
    C.write_json_atomic(out, result)
    if report is not None:
        write_report(result, report)
    return result


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--freeze", type=Path, required=True)
    parser.add_argument("--authorized-oos", action="store_true")
    parser.add_argument("--labels-stage", default="labels_holdout")
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--report", type=Path)
    parser.add_argument("--memory", default="300MB")
    parser.add_argument("--threads", type=int, default=1)
    args = parser.parse_args(argv)
    result = run(args.freeze, args.out, authorized_oos=args.authorized_oos, labels_stage=args.labels_stage,
                 memory=args.memory, threads=args.threads, report=args.report)
    print(f"holdout_eval: {len(result['features'])} frozen columns at 5/21/63 -> {args.out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
