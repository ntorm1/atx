"""Registry coverage of the build root: manifests and Parquet files that no registry entry covers, entries whose
manifest is absent, and a ready-to-paste ``LAKE_STAGES`` entry for each gap (``tests/test_lake_registry.py``)."""

from __future__ import annotations

import json
import re
from collections.abc import Iterable
from pathlib import Path

from .contract import Stage, glob_match, read_manifest, recorded_files


def _skip(rel: str) -> bool:
    return any(part.startswith("_") for part in rel.split("/"))


def unregistered(root: Path, stages: Iterable[Stage]) -> tuple[list[str], list[str], list[str]]:
    """(manifests no entry registers, Parquet files no output glob covers, non-planned entries without manifest)."""
    stages = list(stages)
    known = {s.manifest for s in stages}
    manifests = sorted(p.relative_to(root).as_posix() for p in root.rglob("*manifest*.json"))
    manifests = [m for m in manifests if not _skip(m) and m not in known]
    globs = [o.glob for s in stages for o in s.outputs]
    orphans = []
    for p in sorted(root.rglob("*.parquet")):
        rel = p.relative_to(root).as_posix()
        if not _skip(rel) and not any(glob_match(rel, g) for g in globs):
            orphans.append(rel)
    absent = [s.name for s in stages if not s.planned and not (root / s.manifest).is_file()]
    return manifests, orphans, absent


def _collapse(rel: str) -> str:
    rel = re.sub(r"(?<=/)([a-z_]+)=[^/]+(?=/)", r"\1=*", rel)
    head, _, name = rel.rpartition("/")
    if re.search(r"\d", name.rsplit(".", 1)[0]):
        name = "*." + name.rsplit(".", 1)[1]
    return f"{head}/{name}" if head else name


def suggest_entry(root: Path, manifest_rel: str | None, files: Iterable[str]) -> str:
    """A ready-to-paste ``LAKE_STAGES`` dict for an unregistered manifest and/or orphan Parquet files."""
    import pyarrow.parquet as pq

    m = read_manifest(root / manifest_rel) if manifest_rel else None
    m = m or {}
    base = manifest_rel.rsplit("/", 1)[0] if manifest_rel and "/" in manifest_rel else ""
    rels = set(files)
    for k in (recorded_files(m) or {}):
        if k.endswith(".parquet"):
            rels.add(f"{base}/{k}" if base else k)
    name = str(m.get("stage") or (manifest_rel or next(iter(rels), "stage")).split("/")[0])
    name = re.sub(r"[^a-z0-9_]+", "_", name.lower())
    mods = [k[:-3] for k in (m.get("code") or {}) if k.endswith(".py") and k != "common.py"]
    outputs = []
    for g in sorted({_collapse(r) for r in rels}):
        sample = next((r for r in sorted(rels) if glob_match(r, g)), None)
        cols = pq.read_schema(root / sample).names if sample else []
        clock = "available_at" if "available_at" in cols else None
        entry = {"glob": g, "clock": clock}
        if clock is None:
            entry["doc"] = "<no available_at column: add one, or keep clock None for a static/audit table>"
        outputs.append(entry)
    d = {"name": name, "lane": "<LANE: one of PLAT ID MKT FUND OWN EVT TXT LIC CHAR OPS S0>",
         "schema": m.get("schema") or "<schema id>",
         "module": f"atx_db.alpha_panel.{mods[-1]}" if mods else "<dotted module, default: the declaring module>",
         "args": ["<cli args, e.g. build>"], "inputs": ["<registry names of the stages it reads>"],
         "outputs": outputs, "staleness": str(m.get("staleness") or m.get("staleness_rule") or "<rule>")[:120],
         "vintage": "event", "guard_gb": 0.6}
    if manifest_rel and manifest_rel != f"{name}/manifest.json":
        d["manifest"] = manifest_rel
    return json.dumps(d, indent=4).replace(": null", ": None").replace(": true", ": True").replace(": false", ": False")


def coverage_report(root: Path, stages: Iterable[Stage]) -> str:
    """Human instructions for every registry-coverage gap ('' when none)."""
    stages = list(stages)
    manifests, orphans, absent = unregistered(root, stages)
    if not (manifests or orphans or absent):
        return ""
    lines = ["Lake registry coverage gaps under " + root.as_posix() + ".",
             "Register each stage with one data-only entry: either in atx_db/stagelake/registry.py STAGES (lane PLAT) or",
             "as a module-level literal in your stage module (discovered by parsing, never imported):",
             "    LAKE_STAGES = [ {...}, ]", ""]
    claimed: set[str] = set()
    for m in manifests:
        base = m.rsplit("/", 1)[0] + "/" if "/" in m else ""
        mine = [o for o in orphans if o.startswith(base)] if base else []
        claimed.update(mine)
        lines += [f"* unregistered manifest {m}; add:", suggest_entry(root, m, mine), ""]
    rest = [o for o in orphans if o not in claimed]
    by_dir: dict[str, list[str]] = {}
    for o in rest:
        by_dir.setdefault(o.split("/")[0], []).append(o)
    for d, fs in sorted(by_dir.items()):
        owner = next((s for s in stages if s.manifest_dir.split("/")[0] == d), None)
        if owner:
            lines += [f"* Parquet files under {d}/ not covered by any output glob of stage {owner.name!r}: add to its "
                      f"'outputs' (or delete stale files): " + ", ".join(sorted({_collapse(f) for f in fs}))]
        else:
            lines += [f"* Parquet files under {d}/ belong to no registered stage (publish a manifest and add):",
                      suggest_entry(root, None, fs)]
        lines.append("")
    for a in absent:
        lines.append(f"* registered stage {a!r} has no manifest on disk: publish it, or mark the entry planned=True")
    return "\n".join(lines)
