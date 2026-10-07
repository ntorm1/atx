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
    original = results["windows"]["original"]
    from .a0_check import zero_gate
    if zero_gate(original["rows"])["pass"] is not True:
        raise ValueError("original-window zero gate FAIL")
    if not results.get("sanity") or any(not line.startswith("PASS ") for line in results["sanity"]):
        raise ValueError("sanity FAIL or missing")
    if not {"r4", "pit", "link_table", "stage_i_validation"} <= set(results.get("d0", {})):
        raise ValueError("full D0 evidence missing")


def fixed_step_evidence(base: Path, *, commit: str, data_root: Path) -> dict:
    """Authenticate every planned step and retain hashes of its terminal evidence."""
    from .a0_check import plan_steps
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
        evidence[rel] = digest(path)
        for key in ("receipt", "stdout", "stderr"):
            output = Path(last[key]).absolute()
            relative = output.relative_to(base.absolute()).as_posix()
            evidence[relative] = digest(_file(base, relative))
        receipt = json.loads(Path(last["receipt"]).read_text(encoding="utf-8"))
        if receipt.get("status") != "completed" or receipt.get("returncode") != 0:
            raise ValueError(f"guard receipt contradicts success: {rel}")
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


def seal_results(report_root: Path, *, commit: str, evidence: dict, destination: Path) -> dict:
    """Save immutable report/identifier byte pins beside the actual step evidence."""
    results = json.loads(_file(report_root, "a0-results.json").read_text(encoding="utf-8"))
    validate_results(results, commit=commit)
    if evidence["commit"] != commit or evidence["baseline"] != "repinned" or evidence["reconstruction_batch_size"] != 64:
        raise ValueError("step evidence identity differs")
    _file(report_root, "a0-results.md")
    files = {p.relative_to(report_root).as_posix(): digest(_file(report_root, p.relative_to(report_root).as_posix()))
             for p in sorted(report_root.rglob("*")) if p.is_file()}
    if not any(rel.startswith("identifiers/") for rel in files):
        raise ValueError("identifier evidence missing")
    seal = {"commit": commit, "report_root": str(report_root.absolute()), "report_files": files, "steps": evidence}
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
