"""Seal immutable gold recipes and artifacts before authorized OOS outcome access."""

from __future__ import annotations

import argparse
import datetime as dt
from pathlib import Path
from typing import Any

from . import common as C

SCHEMA = "atx.alpha-panel.gold-freeze/v1"
OOS_START = dt.date(2024, 1, 1)
HORIZONS = (5, 21, 63)
MODULES = ("gold_freeze", "holdout_eval", "labels_holdout", "gold", "characteristics", "char_registry",
           "ic_eval", "labels", "common", "export_impl", "panel", "prices", "silver_refresh")


def verify(path: Path, *, authorized_oos: bool = False) -> dict[str, Any]:
    """Refuse access unless explicitly authorized and every frozen artifact is intact."""
    if not authorized_oos:
        raise ValueError("OOS outcome access requires explicit --authorized-oos and a frozen gold receipt")
    receipt = C.read_json(path)
    if receipt.get("schema") != SCHEMA or receipt.get("status") != "frozen":
        raise ValueError("invalid gold freeze receipt")
    if receipt.get("label_cutoff_training") != "2023-12-29" or receipt.get("horizons") != list(HORIZONS):
        raise ValueError("frozen training cutoff or OOS horizons are invalid")
    if receipt.get("oos_start") != str(OOS_START) or dt.date.fromisoformat(receipt["oos_cutoff"]) < OOS_START:
        raise ValueError("invalid frozen OOS window")
    if not receipt.get("features") or any(not c.startswith(("g_", "c_")) for c in receipt["features"]):
        raise ValueError("freeze must declare only selected gold scores and fixed composites")
    for rel, expected in receipt.get("artifact_sha256", {}).items():
        artifact = C.build_root() / rel
        if not artifact.is_file() or C.sha256_file(artifact) != expected:
            raise ValueError(f"frozen artifact changed or is missing: {rel}")
    if not receipt.get("artifact_sha256"):
        raise ValueError("freeze has no artifact hash pins")
    root = C.build_root()
    required = {"gold/manifest.json", "gold/selection.json", "gold/frozen_recipe.json", "characteristics/manifest.json",
                "prices/manifest.json", "calendar.parquet", f"{receipt.get('panel_stage')}/manifest.json"}
    if not required.issubset(receipt["artifact_sha256"]):
        raise ValueError("freeze omits a required gold or source artifact")
    for rel in receipt["artifact_sha256"]:
        if not rel.endswith("/manifest.json"):
            continue
        manifest = C.read_json(root / rel)
        stage = rel.rsplit("/", 1)[0]
        actual = {p.relative_to(root / stage).as_posix() for p in (root / stage).glob("**/*.parquet")}
        if "files" in manifest and actual != set(manifest["files"]):
            raise ValueError(f"unmanifested or missing physical files in frozen stage: {stage}")
        for name, info in manifest.get("files", {}).items():
            if receipt["artifact_sha256"].get(f"{stage}/{name}") != info["sha256"]:
                raise ValueError(f"freeze omits a declared physical artifact: {stage}/{name}")
        if stage == "prices" and "files" not in manifest:
            listed = set()
            for year in manifest.get("years", {}):
                physical = list((root / stage / f"year={year}").glob("*.parquet"))
                listed.update(p.relative_to(root / stage).as_posix() for p in physical)
                if not physical or any(p.relative_to(root).as_posix() not in receipt["artifact_sha256"] for p in physical):
                    raise ValueError(f"freeze omits canonical physical prices for year {year}")
            if actual != listed:
                raise ValueError("legacy prices has an unlisted physical partition")
    recipe = C.read_json(root / "gold" / "frozen_recipe.json")
    normalization = recipe.get("normalization", recipe.get("score_normalization"))
    if (receipt["features"] != recipe.get("evaluation_columns")
            or receipt.get("feature_choices") != recipe.get("features", {})
            or receipt.get("composite_weights") != recipe.get("composites", {})
            or receipt.get("normalization") != normalization):
        raise ValueError("freeze choices differ from the immutable gold recipe")
    gold = C.read_json(root / "gold" / "manifest.json")
    selection = C.read_json(root / "gold" / "selection.json")
    expected_evidence = receipt["artifact_sha256"].get(receipt.get("selection_evidence"))
    if not expected_evidence or any(expected_evidence != source.get("ic_sha256") for source in (gold, recipe, selection)):
        raise ValueError("freeze does not bind the exact gold selection evidence")
    bound = {s.split("/", 1)[0] for s in gold.get("input_manifests_sha256", {})}
    if receipt.get("panel_stage") not in bound:
        raise ValueError("freeze panel differs from the gold source")
    here = Path(__file__).resolve().parent
    if set(receipt.get("code_sha256", {})) != {f"{name}.py" for name in MODULES}:
        raise ValueError("freeze does not bind every required evaluation and source module")
    for name, expected in receipt.get("code_sha256", {}).items():
        if C.sha256_file(here / name) != expected:
            raise ValueError(f"frozen code changed: {name}")
    return receipt


def create(out: Path, oos_cutoff: dt.date | None = None, selection_evidence: Path | None = None) -> dict[str, Any]:
    """Publish a one-time receipt after the gold recipe and all inputs are complete."""
    if out.exists():
        return verify(out, authorized_oos=True)  # never overwrite a frozen recipe
    root = C.build_root()
    gold = C.read_json(root / "gold" / "manifest.json")
    selection = C.read_json(root / "gold" / "selection.json")
    recipe_path = root / "gold" / "frozen_recipe.json"
    recipe = C.read_json(recipe_path)
    if gold.get("status") != "complete" or gold.get("frozen_recipe_sha256") != C.sha256_file(recipe_path):
        raise ValueError("gold must publish and pin a complete frozen recipe before OOS access")
    if (recipe.get("evaluation_horizons") != list(HORIZONS) or not recipe.get("evaluation_columns")
            or selection.get("label_cutoff") != "2023-12-29"):
        raise ValueError("gold recipe does not fix pre-2024 selection and all OOS horizons")
    if selection.get("rules", {}).get("validation_used_for_selection") is not False:
        raise ValueError("gold recipe must document that validation never selected candidates")
    features = recipe["evaluation_columns"]
    if any(not c.startswith(("g_", "c_")) for c in features):
        raise ValueError("OOS evaluation columns must be gold scores or fixed composites")
    stages = {"gold", "characteristics"}
    for rel in gold.get("input_manifests_sha256", {}):
        stages.add(rel.split("/", 1)[0])
    panel_stages = sorted(s for s in stages if s.startswith("panel"))
    if len(panel_stages) != 1:
        raise ValueError("gold must pin one exact source panel stage")
    panel_stage = panel_stages[0]
    stages.update(("prices", "prices_history", "borrow_proxy", "delisting"))
    pins = {"calendar.parquet": C.sha256_file(C.calendar_path()),
            "gold/selection.json": C.sha256_file(root / "gold" / "selection.json"),
            "gold/frozen_recipe.json": C.sha256_file(recipe_path)}
    evidence_path = selection_evidence or root / "validation" / "gold_pipeline" / "characteristics_strict_ic.json"
    evidence_sha = C.sha256_file(evidence_path)
    if any(evidence_sha != source.get("ic_sha256") for source in (gold, recipe, selection)):
        raise ValueError("selection evidence JSON differs from gold, recipe or selection pins")
    evidence_rel = (evidence_path.relative_to(root).as_posix() if evidence_path.is_relative_to(root)
                    else evidence_path.resolve().as_posix())
    pins[evidence_rel] = evidence_sha
    for stage in sorted(stages):
        path = root / stage / "manifest.json"
        if not path.exists():
            if stage in {"gold", "characteristics", panel_stage, "prices"}:
                raise ValueError(f"missing required frozen source manifest {stage}")
            continue
        manifest = C.read_json(path)
        legacy_prices = stage == "prices" and "status" not in manifest and bool(manifest.get("years"))
        if manifest.get("status") != "complete" and not legacy_prices:
            raise ValueError(f"source stage {stage} is not complete")
        pins[f"{stage}/manifest.json"] = C.sha256_file(path)
        for rel, info in manifest.get("files", {}).items():
            physical = root / stage / rel
            if not physical.exists() or C.sha256_file(physical) != info["sha256"]:
                raise ValueError(f"source artifact differs from its manifest: {stage}/{rel}")
            pins[f"{stage}/{rel}"] = info["sha256"]
        if legacy_prices:
            # The canonical prices builder historically published a row-count
            # receipt. Adapt it without mutating a source used by silver refresh.
            con = C.connect(memory="150MB", threads=1)
            try:
                for year, expected in manifest["years"].items():
                    files = sorted((root / stage / f"year={year}").glob("*.parquet"))
                    if not files:
                        raise ValueError(f"legacy prices missing year {year}")
                    glob = (root / stage / f"year={year}" / "*.parquet").as_posix()
                    rows = con.execute(f"SELECT count(*) FROM read_parquet('{glob}')").fetchone()[0]
                    if rows != expected:
                        raise ValueError(f"legacy prices row receipt differs for year {year}")
                    for physical in files:
                        pins[physical.relative_to(root).as_posix()] = C.sha256_file(physical)
            finally:
                con.close()
    for rel, expected in gold.get("input_manifests_sha256", {}).items():
        path = root / rel if "/" in rel else root / rel / "manifest.json"
        if C.sha256_file(path) != expected:
            raise ValueError(f"gold upstream source changed before freezing: {rel}")
    if gold.get("selection_sha256") != pins["gold/selection.json"]:
        raise ValueError("gold selection differs from the manifest")
    con = C.connect(memory="150MB", threads=1)
    try:
        calendar = C.load_calendar(con)
    finally:
        con.close()
    cutoff = oos_cutoff or calendar[-1]
    if cutoff not in calendar or cutoff < OOS_START or cutoff.year > 2026:
        raise ValueError("OOS cutoff must be an available exchange session in 2024-2026")
    here = Path(__file__).resolve().parent
    if any(not (here / f"{name}.py").exists() for name in MODULES):
        raise ValueError("all required evaluation modules must exist before freezing")
    code = {f"{name}.py": C.sha256_file(here / f"{name}.py") for name in MODULES}
    receipt = {"schema": SCHEMA, "status": "frozen", "frozen_utc": dt.datetime.now(dt.UTC).isoformat(),
               "label_cutoff_training": "2023-12-29", "oos_start": str(OOS_START), "oos_cutoff": str(cutoff),
               "horizons": list(HORIZONS), "features": features, "panel_stage": panel_stage,
               "feature_choices": recipe.get("features", {}), "composite_weights": recipe.get("composites", {}),
               "selection_evidence": evidence_rel,
               "normalization": recipe.get("normalization", recipe.get("score_normalization")), "artifact_sha256": pins, "code_sha256": code,
               "policy": "evaluate every frozen column at every predeclared horizon; never re-fit, re-sign, choose horizon or change admission from OOS outcomes"}
    C.write_json_atomic(out, receipt)
    return verify(out, authorized_oos=True)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--oos-cutoff", type=dt.date.fromisoformat)
    parser.add_argument("--selection-evidence", type=Path)
    args = parser.parse_args(argv)
    receipt = create(args.out, args.oos_cutoff, args.selection_evidence)
    print(f"gold_freeze: {len(receipt['features'])} immutable columns -> {args.out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
