"""Task5 delivery primitives; real reads/copies require the shared heavy guard.

No command is executed and no production activation is performed by this module.
Failed publications retain their partial targets; retries require fresh targets.
"""
from __future__ import annotations

import ast
import difflib
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import uuid

EXPORTS = tuple(f"identity-bridge-v4-{kind}" for kind in ("all", "pit", "strict"))
IDENTITY_FILES = ("links.parquet", "links_name_days.parquet", "links_backfill.parquet",
                  "links_combined.parquet", "links_pit.parquet", "link_table.parquet",
                  "manifest.json", "name_manifest.json", "backfill_manifest.json", "link_table_manifest.json")


def digest(path: Path) -> str:
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def fixed_commands(*, python: Path, commit: str, base: Path, data_root: Path, guard: Path) -> dict:
    if not re.fullmatch(r"[0-9a-f]{40}", commit):
        raise ValueError("a full frozen commit is required")
    prefix = [str(python), "-m", "atx_db.alpha_panel.a0_check"]
    common = ["--base", str(base), "--data-root", str(data_root)]
    return {"arms": prefix + ["run-arms", "--commit", commit, "--baseline", "repinned",
                              "--reconstruction-batch-size", "64", "--python", str(python),
                              "--guard", str(guard)] + common,
            "report": prefix + ["report", "--pit-export", "identity-bridge-v4-pit",
                                 "--published-pit-export", "identity-bridge-v2-pit",
                                 "--window", "original", "--window", "2020-latest"] + common}


def _file(root: Path, relative: str) -> Path:
    rel = Path(relative)
    if rel.is_absolute() or ".." in rel.parts or not rel.parts:
        raise ValueError(f"unsafe relative path: {relative}")
    root = root.absolute()
    path = root / rel
    for node in (path, *path.parents):
        if node.is_symlink() or (hasattr(node, "is_junction") and node.is_junction()):
            raise ValueError(f"linked publication path: {node}")
    if root.resolve() not in path.resolve().parents or not path.is_file():
        raise ValueError(f"missing or escaping file: {path}")
    return path


def _declarations(manifest: dict):
    for field in ("files", "outputs"):
        entries = manifest.get(field, {})
        if isinstance(entries, dict):
            yield from ((name, info["sha256"]) for name, info in entries.items())
        else:
            yield from ((info["file"], info["sha256"]) for info in entries)
    yield from manifest.get("phases", {}).items()


def seal_tree(root: Path, *, manifests: tuple[str, ...], required: tuple[str, ...] = ()) -> dict:
    """Verify original declarations, then pin every byte in this explicit delivery tree."""
    declared = {}
    extra = set()
    for name in manifests:
        manifest = json.loads(_file(root, name).read_text(encoding="utf-8"))
        if manifest.get("status", "complete") != "complete":
            raise ValueError(f"uncompleted manifest: {name}")
        if "build_sha256" in manifest and digest(_file(root, "build.json")) != manifest["build_sha256"]:
            raise ValueError(f"rehearsal build hash mismatch: {name}")
        if manifest.get("launch"):
            _file(root, manifest["launch"])
            extra.add(manifest["launch"])
        for rel, expected in _declarations(manifest):
            # Rehearsal phases maps phase names to phase-receipt hashes.
            if rel in manifest.get("phases", {}):
                rel = f"phases/{rel}.json"
            if rel in declared and declared[rel] != expected:
                raise ValueError(f"conflicting declaration: {rel}")
            if digest(_file(root, rel)) != expected:
                raise ValueError(f"manifest hash mismatch: {rel}")
            declared[rel] = expected
    for name in required:
        _file(root, name)
    inventory = set(required) | set(manifests) | set(declared) | extra
    files = {rel: digest(_file(root, rel)) for rel in sorted(inventory)}
    if not files:
        raise ValueError("empty publication source")
    return {"source": str(root.absolute()), "files": files, "declared": declared,
            "delivery_only_pins": sorted(set(files) - set(declared) - set(manifests))}


def publish(trees: list[tuple[dict, Path]], receipt: Path) -> dict:
    """Stage privately, verify, then Windows no-overwrite rename into absent targets.

    Multi-tree publication is not atomic. Failures retain owned staging trees and
    any complete published trees; the final receipt exists only after all pass.
    """
    if os.name != "nt":
        raise RuntimeError("publication requires Windows no-overwrite rename semantics")
    trees = [(seal, target.absolute()) for seal, target in trees]
    receipt = receipt.absolute()
    targets = [target.absolute() for _, target in trees]
    if not targets or len(set(targets)) != len(targets):
        raise ValueError("distinct targets required")
    if receipt.exists() or receipt.is_symlink():
        raise FileExistsError(receipt)
    if not receipt.parent.is_dir():
        raise ValueError("receipt parent must already exist")
    for node in (receipt, *receipt.parents):
        if node.is_symlink() or (hasattr(node, "is_junction") and node.is_junction()):
            raise ValueError(f"linked receipt path: {node}")
    for target in targets:
        if target.exists() or target.is_symlink():
            raise FileExistsError(target)
        for other in targets:
            if target != other and target in other.parents:
                raise ValueError("overlapping publication targets")
        for node in (target, *target.parents):
            if node.is_symlink() or (hasattr(node, "is_junction") and node.is_junction()):
                raise ValueError(f"linked publication target: {node}")
    # Revalidate the entire seal before the first mutation.
    for seal, target in trees:
        source = Path(seal["source"])
        if receipt == target or target in receipt.parents or source.resolve() in receipt.resolve().parents:
            raise ValueError("receipt must be outside immutable source and target trees")
        if source.resolve() == target.resolve() or source.resolve() in target.resolve().parents:
            raise ValueError("publication overlaps source")
        for rel, expected in seal["files"].items():
            if digest(_file(source, rel)) != expected:
                raise ValueError(f"source changed: {rel}")
    staged = []
    for seal, target in trees:
        stage = target.with_name(target.name + ".delivery-" + uuid.uuid4().hex)
        stage.mkdir(parents=True, exist_ok=False)
        staged.append((seal, stage, target))
        source = Path(seal["source"])
        # Original manifests are copied last; incomplete targets are never sealed.
        order = sorted(seal["files"], key=lambda rel: (rel.endswith(".json"), rel))
        for rel in order:
            dest = stage / rel
            dest.parent.mkdir(parents=True, exist_ok=True)
            with _file(source, rel).open("rb") as src, dest.open("xb") as dst:
                shutil.copyfileobj(src, dst, 1024 * 1024)
            if digest(dest) != seal["files"][rel] or digest(_file(source, rel)) != seal["files"][rel]:
                raise ValueError(f"copy hash mismatch: {dest}")
    for seal, stage, target in staged:
        os.rename(stage, target)  # Windows refuses any existing destination.
        for rel, expected in seal["files"].items():
            if digest(_file(target, rel)) != expected:
                raise ValueError(f"published hash mismatch: {rel}")
    result = {"status": "complete", "trees": [{**seal, "target": str(target)} for seal, target in trees]}
    with receipt.open("x", encoding="utf-8") as stream:
        json.dump(result, stream, indent=2, sort_keys=True)
        stream.write("\n")
    return result


def publication_trees(arm: Path, rehearsal: Path, data_root: Path) -> list[tuple[dict, Path]]:
    """Only completed manifest declarations enter the fixed publication."""
    panel = data_root / "alpha_panel" / "v1"
    trees = [(seal_tree(arm / "export" / name, manifests=("manifest.json",)),
              panel / "export" / name) for name in EXPORTS]
    trees.append((seal_tree(arm / "identity", manifests=("manifest.json", "name_manifest.json",
                 "backfill_manifest.json", "link_table_manifest.json"), required=IDENTITY_FILES), panel / "identity_v4"))
    manifest = json.loads(_file(rehearsal, "manifest.json").read_text(encoding="utf-8"))
    from ..identity_links import REHEARSAL_PHASES
    if set(manifest["phases"]) != set(REHEARSAL_PHASES):
        raise ValueError("complete six-phase rehearsal required")
    phases = tuple(f"phases/{name}.json" for name in manifest["phases"])
    if not phases:
        raise ValueError("rehearsal has no completed phases")
    seal = seal_tree(rehearsal, manifests=("manifest.json", *phases), required=("build.json",))
    trees.append((seal, data_root / "research" / "identity_rehearsal" / "session9-pit-r5"))
    return trees


def validate_results(results: dict, *, commit: str) -> None:
    """Admission checks; D0 differences are reported as fix impact, not waived."""
    if (results.get("commit"), results.get("baseline"), results.get("pit_export"),
        results.get("published_pit_export")) != (commit, "repinned", "identity-bridge-v4-pit", "identity-bridge-v2-pit"):
        raise ValueError("fixed report identity differs")
    pairs = {"B1 vs A", "B2 vs A", "B12 vs A", "B12 vs B1"}
    for name in ("original", "2020-latest"):
        if set(results["windows"][name]["rows"]) != pairs:
            raise ValueError("incomplete four-arm report")
        for pair, row in results["windows"][name]["rows"].items():
            if row.get("comparison") != pair or row.get("status") != "CONDITIONAL_DIAGNOSTIC_COUNTS":
                raise ValueError("comparison identity/status missing")
            _counts(row.get("rows_1_4"), ("1_a_only", "1_b_only", "2_cik", "3_p_to_j", "3_j_to_p", "4_class"))
            _counts(row, ("row_5_distinct_sr_id", "row_6_distinct_cik", "complete_axis_cells"))
            for key in ("row_7", "row_10", "yearly"):
                if not isinstance(row.get(key), dict) or not row[key]:
                    raise ValueError(f"empty or missing {key}")
            for bucket in row["row_7"].values():
                _counts(bucket, ("a_only", "b_only", "a_total"))
            _counts(row["row_10"], tuple(row["row_10"]))
            for key in ("row_8", "row_9"):
                _counts(row.get(key), ("a_only", "b_only"))
            for year in row["yearly"].values():
                _counts(year.get("rows_1_4"), ("1_a_only", "1_b_only", "2_cik", "3_p_to_j", "3_j_to_p", "4_class"))
                _counts(year.get("row_7"), ("a_only", "b_only", "a_total"))
            pins = row.get("input_sha256")
            if not isinstance(pins, dict) or not pins or any(not _sha(sha) for sha in pins.values()):
                raise ValueError("comparison input pins missing")
            for side in ("baseline", "variant"):
                identity = row.get(side, {})
                for key in ("pit", "link_table"):
                    if not identity.get(key) or identity[key] not in pins:
                        raise ValueError("comparison input identity missing")
            if not row.get("files") or any(not _sha(info.get("sha256")) for info in row["files"].values()):
                raise ValueError("comparison identifier pins missing")
    if set(results.get("row_11", {})) != {"B1", "B2", "B12"}:
        raise ValueError("row11 evidence missing")
    for row in results["row_11"].values():
        _counts(row, ("backfill_rows", "symbol_snapshot_match"))
        if type(row.get("name_tier")) is not bool:
            raise ValueError("row11 name tier missing")
        _validation_counts(row.get("validation_got"))
    original = results["windows"]["original"]
    from .a0_check import zero_gate
    if zero_gate(original["rows"])["pass"] is not True:
        raise ValueError("original-window zero gate FAIL")
    if not results.get("sanity") or any(not line.startswith("PASS ") for line in results["sanity"]):
        raise ValueError("sanity FAIL or missing")
    if not {"r4", "pit", "link_table", "stage_i_validation"} <= set(results.get("d0", {})):
        raise ValueError("full D0 evidence missing")
    for kind in ("r4", "pit", "link_table"):
        row = results["d0"][kind]
        if row.get("kind") != kind:
            raise ValueError("D0 product identity missing")
        _counts(row, ("rebuilt_rows", "published_rows"))
        for field in ("rebuilt_schema", "published_schema"):
            if not isinstance(row.get(field), list) or not row[field] or any(
                    not isinstance(column, (list, tuple)) or len(column) != 2 or
                    any(not isinstance(value, str) or not value for value in column) for column in row[field]):
                raise ValueError("D0 schema evidence missing")
        for field in ("schema_matches", "rows_identical", "d0_pass", "source_quality_certified"):
            if type(row.get(field)) is not bool:
                raise ValueError("D0 measured flag missing")
        if row["schema_matches"] != (row["rebuilt_schema"] == row["published_schema"]):
            raise ValueError("D0 schema flag contradicts evidence")
        for field in ("rebuilt_only_rows", "published_only_rows", "rebuilt_duplicate_key_groups",
                      "published_duplicate_key_groups", "rebuilt_null_key_rows", "published_null_key_rows"):
            if field not in row or (row[field] is None and row["schema_matches"] and
                                   (field.endswith("only_rows") or kind == "r4")):
                raise ValueError("D0 measured counts missing")
            if row[field] is not None:
                _counts(row, (field,))
        if row.get("excluded_columns") != (["created_at"] if kind == "r4" else []):
            raise ValueError("D0 projection evidence differs")
    stage_i = results["d0"]["stage_i_validation"]
    if type(stage_i.get("pass")) is not bool:
        raise ValueError("D0 StageI evidence missing")
    if not stage_i["pass"]:
        _validation_counts(stage_i.get("got"))
        _validation_counts(stage_i.get("expected"))
    if not _sha(results["d0"].get("published_pit_manifest_sha256")):
        raise ValueError("published D0 manifest pin missing")


def _sha(value) -> bool:
    return isinstance(value, str) and re.fullmatch(r"[0-9a-f]{64}", value) is not None


def _counts(row, keys) -> None:
    if not isinstance(row, dict) or any(type(row.get(key)) is not int or row[key] < 0 for key in keys):
        raise ValueError("measured counts missing or invalid")


def _validation_counts(row) -> None:
    _counts(row, ("rows", "lines", "ciks", "ambiguous_line_days"))
    if not isinstance(row.get("min_start"), str) or not re.fullmatch(r"\d{4}-\d{2}-\d{2}", row["min_start"]):
        raise ValueError("validation min_start missing")
    _counts(row.get("rows_by_tier"), ("high", "medium"))


def fixed_step_evidence(base: Path, *, commit: str, data_root: Path) -> dict:
    """Authenticate every planned step and retain hashes of its terminal evidence."""
    from .a0_check import plan_steps, guard_command
    record = json.loads(_file(base, "arms.json").read_text(encoding="utf-8"))
    if (record["commit"], record["baseline"], record.get("reconstruction_batch_size")) != (commit, "repinned", 64):
        raise ValueError("fixed arm configuration differs")
    if set(record["arms"]) != {"A", "B1", "B2", "B12"}:
        raise ValueError("four arms required")
    evidence = {"arms.json": digest(_file(base, "arms.json"))}
    code = base / "code" / commit[:7] / "atx-db" / "src"
    if not record.get("modules"):
        raise ValueError("frozen module pins missing")
    for rel, hashes in record["modules"].items():
        if digest(_file(code, rel)) != hashes["sha256"]:
            raise ValueError(f"frozen module changed: {rel}")
    for arm in ("A", "B2"):
        rel = f"W_{arm}/preparation.json"
        prepared = json.loads(_file(base, rel).read_text(encoding="utf-8"))
        if bool(prepared["pin"].get("a0_s1")) != (arm == "B2"):
            raise ValueError(f"preparation S1 identity differs: {rel}")
        reconstruction = prepared["steps"]["reconstruct"]
        driver = digest(_file(code, "atx_db/identity_links.py"))
        ir = digest(_file(code, "atx_db/identity_reconstruction.py"))
        if (reconstruction.get("reconstruction_batch_size"), reconstruction.get("candidate_discovery_batch_size"),
            reconstruction.get("driver_code_sha256"), prepared["pin"]["reconstruction_code_sha256"]) != (64, 64, driver, ir):
            raise ValueError(f"actual reconstruction configuration differs: {rel}")
        evidence[rel] = digest(_file(base, rel))
        for result in prepared["steps"].values():
            for output in result.get("files", []):
                path = _file(base / f"W_{arm}", output["file"])
                if digest(path) != output["sha256"]:
                    raise ValueError(f"completed preparation output changed: {path}")
    terminal_receipts = set()
    for step in plan_steps(base, commit, data_root=data_root, inputs=record["inputs"],
                           baseline="repinned", reconstruction_batch_size=64):
        rel = f"steps/{step['id']}.json"
        path = _file(base, rel)
        saved = json.loads(path.read_text(encoding="utf-8"))
        if saved.get("complete") is not True or saved.get("commit") != commit:
            raise ValueError(f"uncompleted or wrong-commit step: {rel}")
        if any(saved.get(key) != step[key] for key in ("id", "argv", "env", "cap")):
            raise ValueError(f"step configuration changed: {rel}")
        last = saved["attempts"][-1]
        if last["exit_code"] != 0 or last["guard_status"] != "completed":
            raise ValueError(f"unsuccessful terminal attempt: {rel}")
        receipt_path = Path(last["receipt"]).resolve()
        if receipt_path in terminal_receipts:
            raise ValueError("terminal receipt reused across steps")
        terminal_receipts.add(receipt_path)
        command = guard_command(step, receipt=Path(last["receipt"]), stdout=Path(last["stdout"]),
                                stderr=Path(last["stderr"]), python=Path(record["python"]), guard=Path(record["guard"]))
        if last.get("command") != command:
            raise ValueError(f"terminal invocation differs: {rel}")
        evidence[rel] = digest(path)
        for key in ("receipt", "stdout", "stderr"):
            output = Path(last[key]).absolute()
            relative = output.relative_to(base.absolute()).as_posix()
            evidence[relative] = digest(_file(base, relative))
        receipt = json.loads(Path(last["receipt"]).read_text(encoding="utf-8"))
        if receipt.get("status") != "completed" or receipt.get("returncode") != 0:
            raise ValueError(f"guard receipt contradicts success: {rel}")
        if receipt.get("command") != [record["python"], *step["argv"]]:
            raise ValueError(f"guard child invocation differs: {rel}")
        if receipt.get("heavy") is not True or receipt.get("job_limit_gb") != step["cap"] or receipt.get("cap_hit") is not False:
            raise ValueError(f"guard resource configuration differs: {rel}")
    return {"commit": commit, "baseline": "repinned", "reconstruction_batch_size": 64, "files": evidence}


def compare_published(con, *, published: Path, fixed: Path, calendar: str,
                      members: list[str], pins: dict, output: Path, window=None) -> dict:
    """Rows1-10 with explicit published-v2 baseline and fixed-v4 variant.

    The existing kernel's admitted pair labels A/B1 are transport labels only.
    The returned identities make this separate fix-impact comparison explicit.
    """
    from .private_identity_arm_comparison import compare_arms
    baseline = {"arm": "A", "pit": str((published / "export" / "identity-bridge-v2-pit" / "links.parquet").resolve()),
                "link_table": str((published / "identity" / "link_table.parquet").resolve())}
    variant = {"arm": "B1", "pit": str((fixed / "export" / "identity-bridge-v4-pit" / "links.parquet").resolve()),
               "link_table": str((fixed / "identity" / "link_table.parquet").resolve())}
    rows = compare_arms(con, baseline=baseline, variant=variant, calendar=calendar, members=members,
                        pins=pins, output=output, window=window)
    return {"comparison": "fixed A vs original published v2", "baseline": baseline,
            "variant": variant, "kernel_labels": {"A": "published v2", "B1": "fixed arm A"}, "rows": rows}


def save_activation_patch(path: Path, *, common_source: str, identity_source: str, got: dict) -> str:
    patch = activation_patch(common_source, identity_source, got)
    with path.open("x", encoding="utf-8", newline="\n") as stream:
        stream.write(patch)
    return digest(path)


def d0_input_evidence(base: Path, *, data_root: Path) -> dict:
    """Additional delivery evidence: original report does not retain these input pins."""
    from .a0_check import PUBLISHED_ROOT, PUBLISHED_R4, rehearsal_root, _files_sha
    arm, rehearsal = base / "P_A", rehearsal_root(base, "A")
    published, original = data_root / PUBLISHED_ROOT, data_root / PUBLISHED_R4
    r4 = "phases/export-001/security_company_links.part-0000.parquet"
    selected = {
        "r4": ((rehearsal, r4, "phases/export.json"), (original, r4, "manifest.json")),
        "pit": ((arm, "export/identity-bridge-v4-pit/links.parquet", "export/identity-bridge-v4-pit/manifest.json"),
                (published, "export/identity-bridge-v2-pit/links.parquet", "export/identity-bridge-v2-pit/manifest.json")),
        "link_table": ((arm, "identity/link_table.parquet", "identity/link_table_manifest.json"),
                       (published, "identity/link_table.parquet", "identity/link_table_manifest.json"))}
    inputs = {}
    for kind, sides in selected.items():
        inputs[kind] = {}
        for side, (root, rel, manifest_rel) in zip(("rebuilt", "published"), sides):
            manifest_path = _file(root, manifest_rel)
            manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
            expected = (_files_sha(manifest["files"], table="security_company_links") if kind == "r4" else
                        _files_sha(manifest["files"], name=Path(rel).name))
            source = _file(root, rel)
            if digest(source) != expected:
                raise ValueError(f"D0 input differs from manifest: {source}")
            inputs[kind][side] = {"path": str(source.absolute()), "sha256": expected,
                                 "manifest": str(manifest_path.absolute()), "manifest_sha256": digest(manifest_path)}
    stage = _file(arm, "identity/manifest.json")
    validation = json.loads(stage.read_text(encoding="utf-8"))["validation"]
    if validation.get("seal") != "2025-01-01" or type(validation.get("pass")) is not bool:
        raise ValueError("StageI validation evidence missing")
    _validation_counts(validation.get("got"))
    _validation_counts(validation.get("expected"))
    return {"products": inputs, "stage_i": {"manifest": str(stage.absolute()), "manifest_sha256": digest(stage),
            "validation": validation}, "basis": "additional delivery inventory of exact existing D0 selections"}


def seal_results(report_root: Path, *, commit: str, evidence: dict, d0_inputs: dict, destination: Path) -> dict:
    """Save immutable report/identifier byte pins beside the actual step evidence."""
    results = json.loads(_file(report_root, "a0-results.json").read_text(encoding="utf-8"))
    validate_results(results, commit=commit)
    if evidence["commit"] != commit or evidence["baseline"] != "repinned" or evidence["reconstruction_batch_size"] != 64:
        raise ValueError("step evidence identity differs")
    if set(d0_inputs.get("products", {})) != {"r4", "pit", "link_table"}:
        raise ValueError("D0 input inventory missing")
    for sides in d0_inputs["products"].values():
        if set(sides) != {"rebuilt", "published"}:
            raise ValueError("D0 input sides missing")
        for item in sides.values():
            for path_key, pin_key in (("path", "sha256"), ("manifest", "manifest_sha256")):
                path = Path(item[path_key])
                if not _sha(item.get(pin_key)) or digest(_file(path.parent, path.name)) != item[pin_key]:
                    raise ValueError("D0 input byte pin differs")
    pit = d0_inputs["products"]["pit"]
    if (pit["rebuilt"]["path"] != results["pit_paths"]["arms"]["A"] or
        pit["published"]["path"] != results["pit_paths"]["published_reference"] or
        pit["published"]["manifest_sha256"] != results["d0"]["published_pit_manifest_sha256"]):
        raise ValueError("D0 input selection differs from saved report")
    stage = d0_inputs["stage_i"]
    path = Path(stage["manifest"])
    if digest(_file(path.parent, path.name)) != stage["manifest_sha256"]:
        raise ValueError("StageI manifest pin differs")
    validation = json.loads(path.read_text(encoding="utf-8"))["validation"]
    if validation != stage["validation"] or validation.get("seal") != "2025-01-01":
        raise ValueError("StageI delivery evidence differs")
    _validation_counts(validation.get("got"))
    _validation_counts(validation.get("expected"))
    report_validation = results["d0"]["stage_i_validation"]
    wanted = {"pass": True} if validation["pass"] else {key: validation[key] for key in ("pass", "got", "expected")}
    if report_validation != wanted:
        raise ValueError("StageI report evidence differs")
    _file(report_root, "a0-results.md")
    files = {p.relative_to(report_root).as_posix(): digest(_file(report_root, p.relative_to(report_root).as_posix()))
             for p in sorted(report_root.rglob("*")) if p.is_file()}
    if not any(rel.startswith("identifiers/") for rel in files):
        raise ValueError("identifier evidence missing")
    seal = {"commit": commit, "report_root": str(report_root.absolute()), "report_files": files,
            "steps": evidence, "d0_inputs": d0_inputs}
    with destination.open("x", encoding="utf-8") as stream:
        json.dump(seal, stream, indent=2, sort_keys=True)
        stream.write("\n")
    return seal


def activation_patch(common_source: str, identity_source: str, got: dict) -> str:
    """Return a two-file unified patch from exact source text; never apply it."""
    sources = {"common.py": common_source, "identity_links.py": identity_source}
    changes = {}
    for name, text in sources.items():
        target = "IDENTITY_R4_DIR" if name == "common.py" else "VALIDATION_EXPECTED"
        assignments = [n for n in ast.walk(ast.parse(text)) if isinstance(n, ast.Assign)
                       and any(isinstance(t, ast.Name) and t.id == target for t in n.targets)]
        if len(assignments) != 1:
            raise ValueError(f"expected one {target} assignment")
        node = assignments[0]
        lines = text.splitlines(keepends=True)
        if name == "common.py":
            replacement = 'IDENTITY_R4_DIR = PACKAGE_ROOT / "data" / "research" / "identity_rehearsal" / "session9-pit-r5"\n'
        else:
            old = ast.literal_eval(node.value)
            if set(got) != set(old):
                raise ValueError("validation keys differ")
            replacement = "VALIDATION_EXPECTED = " + repr(got) + "\n"
        changes[name] = "".join(lines[:node.lineno - 1]) + replacement + "".join(lines[node.end_lineno:])
    return "".join("".join(difflib.unified_diff(sources[name].splitlines(keepends=True),
                                               changes[name].splitlines(keepends=True),
                    fromfile=f"a/atx-db/src/atx_db/alpha_panel/{name}",
                    tofile=f"b/atx-db/src/atx_db/alpha_panel/{name}")) for name in sources)
