"""Sequential, resumable alpha research build with sealed returns and manifest pins.

Run with the atx-db virtualenv Python. ``--dry-run`` prints the exact command plan.
``--skip-silver`` verifies the complete refreshed silver panel before proceeding.
The build produces selection-qualified candidates; validation evidence and source
limitations remain explicit, and no automatic predictive-power claim is made.
"""

from __future__ import annotations

import argparse
import contextlib
import ctypes
import datetime as dt
import json
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path
from typing import Any

PACKAGE = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(PACKAGE / "src"))

from atx_db.alpha_panel import common as C  # noqa: E402


def year_range(text: str) -> list[int]:
    first, _, last = text.partition("-")
    values = list(range(int(first), int(last or first) + 1))
    if not values or min(values) < 2020 or max(values) > 2026:
        raise ValueError("gold years must be inside 2020-2026")
    return values


def verify_stage(root: Path, stage: str, years: list[int], *, silver: bool = False) -> dict[str, Any]:
    """Verify published files, complete sessions, code and source dependencies."""
    manifest_path = root / stage / "manifest.json"
    manifest = C.read_json(manifest_path)
    if manifest.get("status") != "complete" or manifest.get("stage") != stage:
        raise ValueError(f"{stage}: no complete publication manifest")
    files = manifest.get("files", {})
    actual = {p.relative_to(root / stage).as_posix() for p in (root / stage).glob("**/*.parquet")}
    if set(files) != actual:
        raise ValueError(f"{stage}: manifest does not cover exactly the published parquet files")
    for rel, identity in files.items():
        path = root / stage / rel
        if path.stat().st_size != identity["bytes"] or C.sha256_file(path) != identity["sha256"]:
            raise ValueError(f"{stage}: file differs from publication manifest: {rel}")
    modules = [name[:-3] for name in manifest.get("code", {}) if name.endswith(".py")]
    current = C.code_identity(*modules)
    for name in (m + ".py" for m in modules):
        if current[name]["sha256_lf"] != manifest["code"][name].get("sha256_lf"):
            raise ValueError(f"{stage}: published code differs from current {name}")
    for rel, sha in manifest.get("input_manifests_sha256", {}).items():
        rel = dependency_path(rel)
        if C.sha256_file(root / rel) != sha:
            raise ValueError(f"{stage}: upstream manifest changed: {rel}")
    if silver and set(manifest.get("bindings", {}).get("link_tiers", [])) != {"strict", "dated", "name"}:
        raise ValueError("silver must use strict/dated/name point-in-time identity links")
    if silver:
        from atx_db.alpha_panel import panel as P
        from atx_db.alpha_panel import silver_refresh as S
        binding = manifest["bindings"]
        current_signature, _ = S.source_signature(P.bindings(stage, binding["fund_stage"], binding["link_table"]))
        if manifest.get("source_signature") != current_signature:
            raise ValueError("silver frozen source signature differs from current sources; resume silver refresh")
    con = C.connect(memory="250MB", threads=1)
    coverage = {}
    try:
        for year in years:
            paths = sorted((root / stage / f"year={year}").glob("*.parquet"))
            if not paths:
                raise ValueError(f"{stage}: missing year {year}")
            glob = (root / stage / f"year={year}" / "*.parquet").as_posix()
            expected = con.execute(f"SELECT count(*) FROM read_parquet('{C.calendar_path().as_posix()}') "
                                   "WHERE year(session_date)=?", [year]).fetchone()[0]
            rows, sessions = con.execute(f"SELECT count(*),count(DISTINCT session_date) FROM read_parquet('{glob}')").fetchone()
            if expected == 0 or sessions != expected or rows == 0:
                raise ValueError(f"{stage}/{year}: incomplete session coverage {sessions}/{expected}")
            if silver:
                bad = con.execute(f"SELECT count(*) FROM read_parquet('{glob}') WHERE cik IS NOT NULL AND "
                                  "(link_tier IS NULL OR link_tier NOT IN ('strict','dated','name'))").fetchone()[0]
                if bad:
                    raise ValueError(f"{stage}/{year}: {bad} issuer joins without PIT identity")
            coverage[str(year)] = {"rows": rows, "sessions": sessions}
    finally:
        con.close()
    return {"manifest_sha256": C.sha256_file(manifest_path), "coverage": coverage}


def plan(args: argparse.Namespace) -> list[dict[str, Any]]:
    root = args.root
    result_dir = root / "validation" / "gold_pipeline"
    ic = result_dir / "characteristics_strict_ic.json"
    gold_ic = result_dir / "gold_strict_ic.json"
    py = sys.executable
    panel = f"{args.panel_stage}/manifest.json"
    labels = f"{args.label_stage}/manifest.json"
    char = "characteristics/manifest.json"
    end = max(2023, max(year_range(args.years)))
    steps = []

    def add(name: str, module: str, flags: list[str], dependencies: list[str], artifacts: list[str]) -> None:
        steps.append({"name": name, "command": [py, "-m", f"atx_db.alpha_panel.{module}", *flags],
                      "dependencies": dependencies, "artifacts": artifacts})

    if not args.skip_silver:
        add("silver", "silver_refresh", ["--years", f"2018-{end}", "--out-stage", args.panel_stage],
            ["identity/manifest.json", "fundamentals_v10/manifest.json"], [panel])
    add("characteristics", "characteristics", ["--years", f"2019-{end}", "--memory", args.memory,
        "--threads", "1", "--isolate"], [panel, "calendar.parquet", "prices/manifest.json", "prices_history/manifest.json",
                                        "borrow_proxy/manifest.json"], [char])
    add("point_in_time", "characteristics", ["--pit", "2020-11-02,2021-06-15,2022-10-03", "--pit-buckets", "3,10",
        "--memory", args.memory, "--threads", "1", "--out", str(result_dir / "characteristics_pit.json")],
        [char, panel], ["validation/gold_pipeline/characteristics_pit.json"])
    add("strict_labels", "labels", ["--quality", "strict", "--out-stage", args.label_stage,
        "--years", "2019", "2020", "2021", "2022", "2023", "--memory", "300MB", "--threads", "1", "--force"],
        [panel, "calendar.parquet", "delisting/manifest.json"], [labels])
    add("characteristics_ic", "ic_eval", ["--features", "characteristics", "--out", str(ic), "--memory", "300MB", "--threads", "1"],
        [char, panel, labels, "calendar.parquet"], ["validation/gold_pipeline/characteristics_strict_ic.json"])
    add("characteristics_report", "ic_report", ["--evaluation", str(ic), "--out", str(result_dir / "characteristics_scorecard.md")],
        ["validation/gold_pipeline/characteristics_strict_ic.json"],
        ["validation/gold_pipeline/characteristics_scorecard.md", "validation/gold_pipeline/characteristics_scorecard.json"])
    add("gold", "gold", ["--ic", str(ic), "--years", args.years, "--memory", "300MB"],
        [char, panel, labels, "validation/gold_pipeline/characteristics_strict_ic.json"],
        ["gold/manifest.json", "gold/selection.json", "gold/frozen_recipe.json"])
    legacy_stage = args.label_stage + "_legacy"
    add("legacy_labels", "labels", ["--quality", "legacy", "--out-stage", legacy_stage, "--years",
        "2019", "2020", "2021", "2022", "2023", "--memory", "300MB", "--threads", "1", "--force"],
        [panel, "calendar.parquet", "delisting/manifest.json", "prices/manifest.json"], [f"{legacy_stage}/manifest.json"])
    add("legacy_sensitivity", "ic_eval", ["--features", "characteristics:<frozen-admitted-feature-names>", "--out",
        str(result_dir / "selected_legacy_ic.json"), "--memory", "300MB", "--threads", "1"],
        [char, panel, f"{legacy_stage}/manifest.json", "gold/selection.json"], ["validation/gold_pipeline/selected_legacy_ic.json"])
    steps[-1]["env"] = {"ATX_ALPHA_LABEL_STAGE": legacy_stage}
    add("gold_ic", "ic_eval", ["--features", "gold", "--out", str(gold_ic), "--memory", "300MB", "--threads", "1"],
        ["gold/manifest.json", char, panel, labels], ["validation/gold_pipeline/gold_strict_ic.json"])
    add("gold_report", "ic_report", ["--evaluation", str(gold_ic), "--out", str(result_dir / "gold_scorecard.md")],
        ["validation/gold_pipeline/gold_strict_ic.json"], ["validation/gold_pipeline/gold_scorecard.md", "validation/gold_pipeline/gold_scorecard.json"])
    if not args.role:
        native = root / "export" / "gold-qualified-native"
        add("consumer_role", "export_impl", ["--score-start", "2020-01-02", "--end", "2023-12-29",
            "--fields", "mkt_ret", "--out", str(native)], [panel, "calendar.parquet", "prices/manifest.json"],
            ["export/gold-qualified-native/role/manifest.json", "export/gold-qualified-native/fields/manifest.json"])
        add("consumer_gold", "export_impl", ["align-stage", "--role", str(native / "role"), "--out",
            str(native / "gold-fields"), "--stage", "gold"],
            ["gold/manifest.json", "export/gold-qualified-native/role/manifest.json"],
            ["export/gold-qualified-native/gold-fields/manifest.json"])
    for i, role in enumerate(args.role):
        add(f"consumer_export_{i}", "export_impl", ["align-stage", "--role", str(role), "--out",
            str(root / "export" / f"gold-qualified-{i}"), "--stage", "gold"],
            ["gold/manifest.json", str(role / "manifest.json")], [f"export/gold-qualified-{i}/manifest.json"])
    if getattr(args, "evaluate_frozen_oos", False):
        freeze = result_dir / "gold_freeze.json"
        add("freeze_gold", "gold_freeze", ["--out", str(freeze), "--selection-evidence", str(ic)],
            ["gold/manifest.json", "gold/selection.json", "gold/frozen_recipe.json", char, panel,
             "validation/gold_pipeline/characteristics_strict_ic.json"], ["validation/gold_pipeline/gold_freeze.json"])
        add("holdout_labels", "labels_holdout", ["--freeze", str(freeze), "--authorized-frozen-holdout",
            "--panel-stage", args.panel_stage, "--out-stage", "labels_holdout", "--memory", "300MB"],
            ["validation/gold_pipeline/gold_freeze.json", "gold/manifest.json", panel, "prices/manifest.json"], ["labels_holdout/manifest.json"])
        add("holdout_ic", "holdout_eval", ["--freeze", str(freeze), "--authorized-oos", "--labels-stage", "labels_holdout",
            "--out", str(result_dir / "frozen_oos_ic.json"), "--report", str(result_dir / "frozen_oos_scorecard.md"),
            "--memory", "300MB", "--threads", "1"],
            ["validation/gold_pipeline/gold_freeze.json", "gold/manifest.json", "labels_holdout/manifest.json"],
            ["validation/gold_pipeline/frozen_oos_ic.json", "validation/gold_pipeline/frozen_oos_scorecard.md"])
    return steps


def pins(root: Path, paths: list[str]) -> dict[str, str]:
    return {p: C.sha256_file(root / p) for p in paths}


def dependency_path(key: str) -> str:
    return key if "/" in key or "\\" in key or Path(key).suffix else f"{key}/manifest.json"


def code_pins(step: dict[str, Any]) -> dict[str, Any]:
    module = step["command"][2].rsplit(".", 1)[-1]
    transitive = {"silver_refresh": ("panel", "silver_refresh"),
                  "characteristics": ("characteristics", "char_registry"),
                  "gold": ("gold", "characteristics", "char_registry"),
                  "ic_eval": ("ic_eval", "labels"),
                  "export_impl": ("export_impl", "gold", "characteristics", "char_registry")}
    identity = C.code_identity("common", *transitive.get(module, (module,)))
    identity.pop("git_head", None)
    identity["pipeline.py"] = {"sha256": C.sha256_file(Path(__file__).resolve())}
    return identity


def reusable_step(root: Path, previous: dict[str, Any], step: dict[str, Any],
                  inputs: dict[str, str], code: dict[str, Any]) -> bool:
    return (previous.get("status") == "complete" and previous.get("command") == step["command"]
            and previous.get("input_pins") == inputs and previous.get("code_pins") == code
            and all((root / p).exists() for p in step["artifacts"])
            and previous.get("output_pins") == pins(root, step["artifacts"]))


def verify_consumer_export(role: Path, fields: Path) -> dict[str, Any]:
    """Check the native loader's axes, binary sizes, pins and sealed dates."""
    import numpy as np

    from atx_db.alpha_panel.export_impl import FIELDS_SCHEMA, ROLE_SCHEMA
    market = C.read_json(role / "manifest.json")
    scores = C.read_json(fields / "manifest.json")
    if market.get("schema") != ROLE_SCHEMA or scores.get("schema") != FIELDS_SCHEMA:
        raise ValueError("consumer schema mismatch")
    if market.get("status") != "complete" or scores.get("status") != "complete":
        raise ValueError("consumer manifests must be complete")
    n, d = market["instruments"], market["dates"]
    sessions = np.fromfile(role / "sessions.i64", dtype="<i8")
    ids = np.fromfile(role / "ids.u64", dtype="<u8")
    seal_ns = int(np.datetime64("2024-01-01", "ns").astype(np.int64))
    if (len(sessions) != d or len(ids) != n or np.any(np.diff(sessions) <= 0)
            or np.any(ids[1:] <= ids[:-1]) or np.any(ids == 0) or sessions[-1] >= seal_ns):
        raise ValueError("consumer axes invalid or return holdout unsealed")
    expected = {"sessions.i64": d * 8, "ids.u64": n * 8, "present.u8": d * n, "member.u8": d * n,
                "close.f64": d * n * 8, "raw_close.f64": d * n * 8, "volume.f64": d * n * 8}
    for folder, manifest in ((role, market), (fields, scores)):
        for name, identity in manifest.get("files", {}).items():
            path = folder / name
            if path.stat().st_size != identity["bytes"] or C.sha256_file(path) != identity["sha256"]:
                raise ValueError(f"consumer file differs from manifest: {path}")
    if any((role / name).stat().st_size != size for name, size in expected.items()):
        raise ValueError("consumer market binary shape mismatch")
    if scores.get("role", {}).get("manifest_sha256") != C.sha256_file(role / "manifest.json"):
        raise ValueError("consumer feature axes are not pinned to the market role")
    for field in scores.get("fields", []):
        if field.get("shape") != [d, n] or field.get("dtype") != "<f8" or field.get("layout") != "date-major":
            raise ValueError(f"consumer feature binary contract mismatch: {field.get('name')}")
        if (fields / field["file"]).stat().st_size != d * n * 8:
            raise ValueError("consumer feature binary size mismatch")
    return {"role": str(role), "fields": str(fields), "dates": d, "instruments": n,
            "last_session_ns": int(sessions[-1]), "field_count": len(scores.get("fields", [])), "pass": True}


def pid_alive(pid: int) -> bool:
    if pid <= 0:
        return False
    if os.name == "nt":
        kernel = ctypes.WinDLL("kernel32", use_last_error=True)
        kernel.OpenProcess.restype = ctypes.c_void_p
        handle = kernel.OpenProcess(0x1000, False, pid)
        if not handle:
            return ctypes.get_last_error() != 87  # access denied is conservatively alive
        status = ctypes.c_ulong()
        try:
            return not kernel.GetExitCodeProcess(ctypes.c_void_p(handle), ctypes.byref(status)) or status.value == 259
        finally:
            kernel.CloseHandle(ctypes.c_void_p(handle))
    try:
        os.kill(pid, 0)
        return True
    except ProcessLookupError:
        return False
    except PermissionError:
        return True


def acquire_lock(path: Path) -> None:
    if path.exists():
        pid = int(path.read_text(encoding="utf-8"))
        if pid_alive(pid):
            raise RuntimeError(f"gold pipeline is already running as PID {pid}")
        path.unlink()  # proven dead owner; single known lock file only
    with path.open("x", encoding="utf-8") as fh:
        fh.write(str(os.getpid()))


def publish_export(staged: Path, target: Path, export_root: Path) -> None:
    """Replace one pipeline-owned export only after its new build succeeds."""
    base = export_root.resolve()
    old = target.with_name(f".{target.name}.stale-{os.getpid()}")
    for path in (staged, target, old):
        resolved = path.resolve()
        if resolved == base or not resolved.is_relative_to(base):
            raise ValueError("export publish target must stay inside the named export directory")
    if old.exists():
        shutil.rmtree(old)
    if target.exists():
        target.rename(old)
    try:
        staged.rename(target)
    except Exception:
        if old.exists():
            old.rename(target)
        raise
    if old.exists():
        shutil.rmtree(old)


def execute(args: argparse.Namespace) -> dict[str, Any]:
    steps = plan(args)
    if args.dry_run:
        result = {"dry_run": True, "steps": steps, "panel_stage": args.panel_stage, "label_stage": args.label_stage,
                  "skip_silver_requires": "complete current manifest, file hashes, all requested sessions, PIT links"}
        print(json.dumps(result, indent=2))
        return result
    root = args.root.resolve()
    directory = root / "validation" / "gold_pipeline"
    directory.mkdir(parents=True, exist_ok=True)
    lock = directory / "pipeline.lock"
    acquire_lock(lock)
    ledger_path = directory / "orchestration.json"
    ledger = C.read_json(ledger_path) if ledger_path.exists() and not args.no_resume else {"steps": {}}
    env = os.environ.copy()
    env.update(ATX_ALPHA_PANEL_ROOT=str(root), ATX_ALPHA_PANEL_STAGE=args.panel_stage, ATX_ALPHA_LABEL_STAGE=args.label_stage)
    env["PYTHONPATH"] = str(PACKAGE / "src") + os.pathsep + env.get("PYTHONPATH", "")
    try:
        # Never compete with an already running silver writer. Scratch files are
        # specific to the current monthly build; stale files require explicit cleanup.
        active = list((root / "_tmp").glob(args.panel_stage + "_*.duckdb"))
        if active:
            raise ValueError(f"silver writer scratch exists; finish or inspect it before building: {active[0]}")
        if args.skip_silver:
            ledger["silver_verified"] = verify_stage(root, args.panel_stage, list(range(2018, max(2023, max(year_range(args.years))) + 1)), silver=True)
        for step in steps:
            name = step["name"]
            if name == "legacy_sensitivity":
                selected = C.read_json(root / "gold" / "selection.json")["selected"]
                step["command"][4] = "characteristics:" + ",".join(item["name"] for item in selected)
            before = pins(root, step["dependencies"])
            code = code_pins(step)
            previous = ledger["steps"].get(name, {})
            if not args.no_resume and reusable_step(root, previous, step, before, code):
                if name in ("characteristics", "gold"):
                    verify_stage(root, name, list(range(2019, max(2023, max(year_range(args.years))) + 1))
                                 if name == "characteristics" else year_range(args.years))
                if name in ("freeze_gold", "holdout_labels", "holdout_ic"):
                    from atx_db.alpha_panel.gold_freeze import verify
                    verify(directory / "gold_freeze.json", authorized_oos=True)
                print(f"resume {name}: unchanged published inputs and outputs", flush=True)
                continue
            record = {**step, "status": "running", "input_pins": before, "code_pins": code,
                      "started_utc": dt.datetime.now(dt.UTC).isoformat()}
            ledger["steps"][name] = record
            C.write_json_atomic(ledger_path, ledger)
            log = directory / f"{name}.log"
            print(f"start {name}: {subprocess.list2cmdline(step['command'])}; log={log}", flush=True)
            command = list(step["command"])
            staged, target = None, None
            if name.startswith("consumer_"):
                pos = command.index("--out") + 1
                target = Path(command[pos])
                staged = target.with_name(f".{target.name}.build-{os.getpid()}")
                if not staged.resolve().is_relative_to((root / "export").resolve()):
                    raise ValueError("staged export must stay inside the export root")
                if staged.exists():
                    shutil.rmtree(staged)
                command[pos] = str(staged)
            with log.open("w", encoding="utf-8") as fh:
                rc = subprocess.call(command, cwd=PACKAGE, env={**env, **step.get("env", {})}, stdout=fh, stderr=subprocess.STDOUT)
            if rc or before != pins(root, step["dependencies"]) or code != code_pins(step):
                if staged is not None and staged.exists():
                    shutil.rmtree(staged)
                record["status"] = "failed"
                record["return_code"] = rc
                C.write_json_atomic(ledger_path, ledger)
                raise RuntimeError(f"{name} failed or an upstream manifest changed; see {log}")
            if staged is not None:
                publish_export(staged, target, root / "export")
            if name == "silver":
                ledger["silver_verified"] = verify_stage(root, args.panel_stage, list(range(2018, max(2023, max(year_range(args.years))) + 1)), silver=True)
            elif name in ("characteristics", "gold"):
                verify_stage(root, name, list(range(2019, max(2023, max(year_range(args.years))) + 1))
                             if name == "characteristics" else year_range(args.years))
            record.update(status="complete", output_pins=pins(root, step["artifacts"]),
                          finished_utc=dt.datetime.now(dt.UTC).isoformat())
            C.write_json_atomic(ledger_path, ledger)
        selection = C.read_json(root / "gold" / "selection.json")
        if args.role:
            consumer = [verify_consumer_export(role, root / "export" / f"gold-qualified-{i}") for i, role in enumerate(args.role)]
        else:
            native = root / "export" / "gold-qualified-native"
            consumer = [verify_consumer_export(native / "role", native / "gold-fields")]
        chosen = {f"characteristics:{item['name']}": item for item in selection["selected"]}
        evidence = C.read_json(directory / "characteristics_strict_ic.json")
        legacy = C.read_json(directory / "selected_legacy_ic.json")
        frozen = []
        for feature, choice in chosen.items():
            window = evidence["features"][feature]["ic"][f"h{choice['horizon']}"]["member_equity"]
            legacy_window = legacy["features"].get(feature, {}).get("ic", {}).get(f"h{choice['horizon']}", {}).get("member_equity", {})
            frozen.append({"feature": feature, "selection_choice": choice, "validation_at_frozen_choice": window.get("VALIDATION"),
                           "legacy_validation_at_frozen_choice": legacy_window.get("VALIDATION"),
                           "legacy_selection_at_frozen_choice": legacy_window.get("SELECTION")})
        quality = {"schema": "atx.alpha-panel.gold-pipeline/v1", "status": "complete", "years": args.years,
                   "quality_status": "selection-qualified candidates with chronological validation evidence",
                   "predictive_power_certified": False, "source_vintages_certified": False,
                   "label_stage": args.label_stage, "label_policy": "strict", "label_cutoff": "2023-12-29",
                   "strict_label_attrition": evidence.get("label_quality_attrition", {}),
                   "population_limitation": "Strict labels exclude missing/imputed delisting proceeds. IC is conditional on observed outcomes; survivor bias remains possible.",
                   "legacy_label_sensitivity": "completed at frozen admitted choices; legacy imputes terminal proceeds and permits 90% observed windows",
                   "selected_count": len(chosen), "frozen_validation": frozen,
                   "validation_scorecard_sha256": C.sha256_file(directory / "characteristics_scorecard.json"),
                   "consumer_exports": len(args.role) or 1, "consumer_export_pending": False,
                   "consumer_acceptance": consumer,
                   "sealed_return_years": [2024, 2025, 2026], "orchestration_sha256": C.sha256_file(ledger_path),
                   "exploratory_multiplicity": "Selection screens many features; inspect validation and corrected scorecard evidence; no fitted sign flips."}
        if getattr(args, "evaluate_frozen_oos", False):
            quality.update(out_of_sample_status="evaluated frozen features at every predeclared horizon",
                           out_of_sample_years=[2024, 2025, 2026],
                           gold_freeze_sha256=C.sha256_file(directory / "gold_freeze.json"),
                           frozen_oos_ic_sha256=C.sha256_file(directory / "frozen_oos_ic.json"))
            quality.pop("sealed_return_years", None)
        C.write_json_atomic(directory / "quality.json", quality)
        return quality
    except Exception as exc:
        C.write_json_atomic(directory / "quality.json", {"schema": "atx.alpha-panel.gold-pipeline/v1", "status": "failed",
                                                       "reason": str(exc), "predictive_power_certified": False})
        raise
    finally:
        with contextlib.suppress(FileNotFoundError):
            lock.unlink()


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=C.build_root())
    parser.add_argument("--years", default="2020-2026")
    parser.add_argument("--panel-stage", default="panel_goldready")
    parser.add_argument("--label-stage", default="labels_goldready")
    parser.add_argument("--memory", default="560MB")
    parser.add_argument("--role", type=Path, action="append", default=[])
    parser.add_argument("--skip-silver", action="store_true")
    parser.add_argument("--dry-run", action="store_true")
    parser.add_argument("--no-resume", action="store_true")
    parser.add_argument("--evaluate-frozen-oos", action="store_true",
                        help="explicitly authorized final2024-2026 evaluation after recipe/artifact freeze; never retunes gold")
    args = parser.parse_args(argv)
    years = year_range(args.years)
    if args.evaluate_frozen_oos and not {2024, 2025, 2026}.issubset(years):
        parser.error("frozen 2024-2026 evaluation requires gold --years to include every 2024-2026 partition")
    if any(not re.fullmatch(r"[a-z][a-z0-9_]*", s) for s in (args.panel_stage, args.label_stage)) or args.label_stage == "labels":
        parser.error("use valid isolated stage names; strict labels must preserve the legacy labels stage")
    os.environ["ATX_ALPHA_PANEL_ROOT"] = str(args.root)
    execute(args)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
