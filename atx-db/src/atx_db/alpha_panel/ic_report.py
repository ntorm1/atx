"""Write a reproducible research scorecard without choosing signs on validation.

The horizon and raw/industry variant are chosen exclusively by SELECTION NW t.
VALIDATION columns then use that same choice. P values are asymptotic two-sided
normal HAC approximations, with Holm correction across the frozen choices.
"""

from __future__ import annotations

import argparse
import math
from pathlib import Path
from typing import Any

from . import common as C


def frozen_ranking(evaluation: dict[str, Any]) -> list[dict[str, Any]]:
    rows: list[dict[str, Any]] = []
    for feature, entry in evaluation["features"].items():
        choices = []
        for h in (5, 21, 63):
            windows = entry.get("ic", {}).get(f"h{h}", {}).get("member_equity", {})
            selection = windows.get("SELECTION") or {}
            for variant, prefix in (("raw", ""), ("industry", "ind_")):
                t = selection.get(f"{prefix}nw_t")
                if t is not None and math.isfinite(t):
                    choices.append((float(t), h, variant, prefix, windows))
        if not choices:
            continue
        t, h, variant, prefix, windows = max(choices, key=lambda c: (c[0], -c[1], c[2] == "raw"))
        selection = windows["SELECTION"]
        validation = windows.get("VALIDATION") or {}
        vt = validation.get(f"{prefix}nw_t")
        p = math.erfc(abs(vt) / math.sqrt(2)) if vt is not None else None
        rows.append({"feature": feature, "horizon": h, "variant": variant,
                     "selection_ic": selection.get(f"{prefix}ic_mean"), "selection_nw_t": t,
                     "validation_ic": validation.get(f"{prefix}ic_mean"), "validation_nw_t": vt,
                     "validation_ci95": validation.get(f"{prefix}ic_ci95"),
                     "validation_sessions": validation.get("sessions" if not prefix else "ind_sessions"),
                     "validation_p_two_sided_approx": p,
                     "coverage": entry.get("coverage", {}), "coverage_sessions": entry.get("coverage_sessions", {})})
    tested = sorted((r for r in rows if r["validation_p_two_sided_approx"] is not None),
                    key=lambda r: r["validation_p_two_sided_approx"])
    corrected = 0.0
    for i, row in enumerate(tested):
        corrected = max(corrected, min(1., (len(tested) - i) * row["validation_p_two_sided_approx"]))
        row["validation_p_holm_approx"] = corrected
    return sorted(rows, key=lambda r: (-r["selection_nw_t"], r["feature"]))


def write_report(evaluation: dict[str, Any], source: Path, out: Path) -> dict[str, Any]:
    protocol = evaluation.get("validation_protocol", {})
    if protocol.get("selection_window") != "SELECTION":
        raise ValueError("report requires a v3 purged SELECTION/VALIDATION evaluation")
    rows = frozen_ranking(evaluation)
    payload = {"schema": "atx.alpha-panel.ic-scorecard/v1", "evaluation_sha256": C.sha256_file(source),
               "selection_rule": "maximum prior-signed SELECTION HAC t across h5/h21/h63 and raw/industry; no validation tuning",
               "validation_protocol": protocol, "input_manifests_sha256": evaluation.get("input_manifests_sha256", {}),
               "ranked_features": rows}
    C.write_json_atomic(out.with_suffix(".json"), payload)
    lines = ["# Alpha panel IC validation", "", f"Evidence: `{source}` (SHA-256 `{payload['evaluation_sha256']}`).",
             "", f"Panel source: `{evaluation.get('panel_stage', 'panel')}`. Label realized-return cutoff: {evaluation['label_cutoff']}.",
             "", f"Label source: `{evaluation.get('label_stage', 'labels')}`; quality policy: `{evaluation.get('label_policy', 'legacy')}`.",
             "", "Selection uses 2020-2021; each horizon is purged so its complete label ends before 2022. "
             "Validation uses 2022-2023. The reported validation horizon and variant are frozen from selection. "
             "2024-2026 are covered by feature/coverage reporting only; their return outcomes remain sealed.",
             "", "Labels are log close-to-close returns from session d+1 close to d+1+h close (returns d+2..d+1+h). "
             "They are measurement labels; execution at the next open has a different return interval. "
             "HAC uncertainty uses actual exchange-session lags and Bartlett weights for overlapping outcomes. "
             "The t and confidence interval are asymptotic; a discovery screen is not proof of tradable alpha. "
             "Holm p values correct the frozen feature tests and are approximate normal-HAC values.",
             "", "Strict labels require the entire observed horizon or complete observed terminal proceeds. "
             "Unknown or imputed terminal outcomes, incomplete horizons, and repaired decision/trade/forward identities are excluded. "
             "This makes IC conditional on observed outcomes; excluded delisted names can create survivor bias. "
             "Legacy-label sensitivity at the frozen selected choices remains necessary before a production alpha claim.",
             "", "| Feature | h | Variant | Selection IC | Selection t | Validation IC | Validation t | Validation sessions | Holm p |",
             "|---|---:|---|---:|---:|---:|---:|---:|---:|"]

    def cell(value: Any) -> str:
        return "—" if value is None else str(round(value, 6)) if isinstance(value, float) else str(value)

    for row in rows:
        columns = ("feature", "horizon", "variant", "selection_ic", "selection_nw_t", "validation_ic",
                   "validation_nw_t", "validation_sessions", "validation_p_holm_approx")
        lines.append("| " + " | ".join(cell(row.get(c)) for c in columns) + " |")
    lines.extend(["", "## Mean daily feature coverage", "", "| Feature | " + " | ".join(str(y) for y in range(2020, 2027)) + " |",
                  "|---|" + "---:|" * 7])
    for feature, entry in evaluation["features"].items():
        lines.append("| " + feature + " | " + " | ".join(cell(entry.get("coverage", {}).get(str(y))) for y in range(2020, 2027)) + " |")
    if evaluation.get("label_quality_attrition"):
        lines.extend(["", "## Strict label quality attrition", "",
                      "Counts include all silver decision rows, including non-members; they do not redefine the evaluated member universe.", "",
                      "Codes: 1 complete observed horizon; 2 observed terminal; 3 unknown terminal; 4 imputed terminal; "
                      "5 incomplete/invalid; 6 cutoff censored; 7 repaired decision; 8 repaired anchor/forward identity.", "",
                      "| Year | Horizon | Code | Rows |", "|---|---|---:|---:|"])
        for year, horizons in sorted(evaluation["label_quality_attrition"].items()):
            for h, codes in sorted(horizons.items()):
                for code, count in sorted(codes.items()):
                    lines.append(f"| {year} | {h} | {code} | {count} |")
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text("\n".join(lines) + "\n", encoding="utf-8")
    return payload


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--evaluation", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args(argv)
    result = write_report(C.read_json(args.evaluation), args.evaluation, args.out)
    print(f"ic_report: {len(result['ranked_features'])} frozen candidate choices -> {args.out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
