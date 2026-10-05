"""M2d root item (iii): D2 flag-absent identity on the tiny world (synthetic, 0 trials, no statistic read).

Runs the fitter flag-absent (no --engine-fit) twice on the same tiny-world u output: once with the pre-D2 tools tree
(git archive of f6444417: atx-impl/tools, atx-engine/tools, research_window.json) and once with the merged head's
tools. Then compares every output file of each variant by SHA-256 and, for JSON, by JSON path (key names and value
equality only; no value is printed except SHA / fingerprint strings).

usage: d2_flag_absent_identity.py TINY_ROOT OLD_TREE NEW_TREE OUT_JSON
"""
from __future__ import annotations

import hashlib
import json
import os
import subprocess
import sys
from pathlib import Path

PY = "C:/Program Files/Python312/python.exe"


def sha(p: Path) -> str:
    return hashlib.sha256(p.read_bytes()).hexdigest()


def flatten(doc, prefix="$"):
    out = {}
    if isinstance(doc, dict):
        if not doc:
            out[prefix] = "{}"
        for k, v in doc.items():
            out.update(flatten(v, f"{prefix}.{k}"))
    elif isinstance(doc, list):
        if not doc:
            out[prefix] = "[]"
        for i, v in enumerate(doc):
            out.update(flatten(v, f"{prefix}[{i}]"))
    else:
        out[prefix] = json.dumps(doc, sort_keys=True)
    return out


def compare(old: Path, new: Path) -> dict:
    files_old = {p.relative_to(old).as_posix() for p in old.rglob("*") if p.is_file()}
    files_new = {p.relative_to(new).as_posix() for p in new.rglob("*") if p.is_file()}
    res = {"only_old": sorted(files_old - files_new), "only_new": sorted(files_new - files_old), "files": {}}
    for f in sorted(files_old & files_new):
        a, b = old / f, new / f
        row = {"identical": sha(a) == sha(b)}
        if not row["identical"] and f.endswith(".json"):
            fa = flatten(json.loads(a.read_bytes()))
            fb = flatten(json.loads(b.read_bytes()))
            row["paths_only_old"] = sorted(set(fa) - set(fb))
            row["paths_only_new"] = sorted(set(fb) - set(fa))
            row["paths_value_differs"] = sorted(k for k in set(fa) & set(fb) if fa[k] != fb[k])
        elif not row["identical"]:
            la, lb = a.read_bytes().splitlines(), b.read_bytes().splitlines()
            row["lines"] = [len(la), len(lb)]
            row["lines_differ"] = sum(1 for x, y in zip(la, lb) if x != y) + abs(len(la) - len(lb))
        res["files"][f] = row
    return res


def main() -> int:
    root, old_tree, new_tree, out_json = (Path(a) for a in sys.argv[1:5])
    u = root / "tiny-u-1"
    base = ["--library", "tiny_world_v1.json", "--library-sha256", sha(root / "tiny_world_v1.json"),
            "--train", "role/manifest.json", "--train-sha256", sha(root / "role/manifest.json"),
            "--orientations", "tiny-u-1/orientations.json", "--orientations-sha256", sha(u / "orientations.json"),
            "--runner-summary", "tiny-u-1/summary.json", "--runner-summary-sha256", sha(u / "summary.json"),
            "--orientation", "prior", "--screen", "v4-prior-v1",
            "--recipe", "tiny_world_v1.recipe.json", "--recipe-sha256", sha(root / "tiny_world_v1.recipe.json"),
            "--max-seconds", "120"]
    # The tiny world has three themes: the standardised / shrink rules refuse it (member cap 1/(2T) cannot place a
    # theme's excess), so only the canary's own fit argv (ew-theme-v1) runs here; the module blocks are covered by
    # d2_flag_absent_fixture.py on the fitter's synthetic aim_world.
    variants = {
        "A-ew-theme": ["--composition", "ew-theme-v1"],
    }
    env = dict(os.environ, PYTHONDONTWRITEBYTECODE="1")
    report = {"old_tree": str(old_tree), "new_tree": str(new_tree), "variants": {}}
    for name, args in variants.items():
        report["variants"][name] = {}
        outs = {}
        for tag, tree in (("old", old_tree), ("new", new_tree)):
            out = f"m2d-fit-{tag}-{name}"
            argv = [PY, str(tree / "atx-impl/tools/fit_composition_weights.py"), *base, *args,
                    "--work-dir", f"m2d-work-{tag}-{name}", "--output", out]
            if name == "E-std-resid-child":
                parent = root / f"m2d-fit-{tag}-F-std" / "composition_weights.json"
                argv += ["--theme-resid-parent", str(parent.relative_to(root).as_posix()),
                         "--theme-resid-parent-sha256", sha(parent)]
            done = subprocess.run(argv, cwd=root, env=env, capture_output=True, text=True)
            report["variants"][name][f"{tag}_exit"] = done.returncode
            if done.returncode != 0:
                report["variants"][name][f"{tag}_stderr_tail"] = done.stderr[-1500:]
            outs[tag] = root / out
        if all(report["variants"][name][f"{t}_exit"] == 0 for t in ("old", "new")):
            report["variants"][name]["compare"] = compare(outs["old"], outs["new"])
    Path(out_json).write_text(json.dumps(report, indent=1, sort_keys=True) + "\n", encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
