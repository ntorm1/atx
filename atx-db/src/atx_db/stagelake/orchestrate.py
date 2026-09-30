"""Registry-driven orchestrator (task S1.4; supersedes ``alpha_panel/build.py``'s fixed order).

A stage is **due** when (checked in registry topological order):

* it has no manifest (never built), or
* its code changed: the LF-normalised SHA-256 of a module it recorded (or its registry ``code`` list) differs from
  the manifest's ``code`` (platform modules such as ``common.py`` only with ``--strict-platform``), or a module is
  not recorded at all, or
* an input's manifest SHA-256 differs from the binding the manifest recorded (``input_manifests_sha256``, a legacy
  layout, or this orchestrator's sidecar ``_lake/bindings/<stage>.json`` for exactly the published manifest), or
  no binding was recorded, or
* an input stage is due in this plan (its rebuild will change the input manifest).

``--dry-run`` prints the plan; ``--run`` executes the due stages in topological order, each command through the
memory guard (``run_memory_guarded.py --job-gb <registry guard_gb>``), stops at the first failure, and after a
stage succeeds writes the binding sidecar (input manifest SHAs taken at launch, code SHAs, the new manifest SHA),
so legacy stages without ``input_manifests_sha256`` become incremental after one orchestrated run.

    python -m atx_db.stagelake.orchestrate --dry-run [--only a,b | --from S] [--strict-platform] [--json OUT]
    python -m atx_db.stagelake.orchestrate --run [--fetch] [--max-gb 0.4] [--only ...]
"""

from __future__ import annotations

import datetime as dt
import json
import os
import subprocess
import sys
import time
from collections.abc import Callable, Iterable, Mapping
from dataclasses import asdict, dataclass, field
from pathlib import Path
from typing import Any

from .contract import (
    PACKAGE_ROOT,
    PLATFORM_MODULES,
    SRC_ROOT,
    Stage,
    code_modules,
    default_root,
    downstream,
    manifest_sha,
    module_path,
    read_manifest,
    recorded_code,
    sha256_file,
    sha256_lf,
)
from .verify import input_binding, sidecar_path

GUARD = PACKAGE_ROOT.parent / ".superpowers" / "sdd" / "tier1-parity" / "run_memory_guarded.py"
Runner = Callable[[Stage, list[str], float, Path], int]


@dataclass
class PlanItem:
    stage: str
    due: bool
    action: str  # fresh | run | manual | via <stage> | needs --fetch
    reasons: list[str] = field(default_factory=list)
    commands: list[list[str]] = field(default_factory=list)
    guard_gb: float = 0.0


def _code_reasons(stage: Stage, manifest: Mapping[str, Any], side: Mapping[str, Any] | None, src_root: Path,
                  strict_platform: bool) -> list[str]:
    rec = recorded_code(manifest) or dict((side or {}).get("code") or {})
    mods = code_modules(stage, manifest, src_root)
    if strict_platform and stage.module:
        pkg = stage.module.rsplit(".", 1)[0]
        for f in PLATFORM_MODULES:
            if f in recorded_code(manifest):
                mods[f] = module_path(f"{pkg}.{f[:-3]}", src_root)
    out = []
    for fname, path in sorted(mods.items()):
        if not path.is_file():
            out.append(f"code missing: {fname}")
        elif fname not in rec:
            out.append(f"code not recorded: {fname}")
        elif rec[fname] not in (sha256_lf(path), sha256_file(path)):
            out.append(f"code changed: {fname}")
    return out


def direct_reasons(root: Path, stage: Stage, by_name: Mapping[str, Stage], src_root: Path = SRC_ROOT,
                   strict_platform: bool = False) -> list[str]:
    """Why the stage itself is due (upstream propagation excluded); [] = fresh."""
    mpath = root / stage.manifest
    if not mpath.is_file():
        return ["never built" if stage.planned else "no manifest"]
    manifest = read_manifest(mpath) or {}
    msha = sha256_file(mpath)
    side = read_manifest(sidecar_path(root, stage.name))
    side = side if side and side.get("manifest_sha256") == msha else None
    reasons = _code_reasons(stage, manifest, side, src_root, strict_platform) if stage.module or stage.code else []
    got, _ = input_binding(stage, manifest, root, msha)
    for name in stage.inputs:
        cur = manifest_sha(root, by_name[name].manifest)
        if cur is None:
            reasons.append(f"input missing: {name}")
        elif by_name[name].manifest not in got:
            reasons.append(f"input binding not recorded: {name}")
        elif got[by_name[name].manifest] != cur:
            reasons.append(f"input changed: {name}")
    return reasons


def plan(root: Path, stages: list[Stage], only: Iterable[str] | None = None, start: str | None = None,
         src_root: Path = SRC_ROOT, strict_platform: bool = False, fetch: bool = False) -> list[PlanItem]:
    """Plan over ``stages`` (topological order). ``only`` restricts the reported stages; ``start`` = that stage
    and everything downstream of it."""
    by_name = {s.name: s for s in stages}
    selected = set(by_name)
    if only:
        selected = set(only)
    if start:
        selected = downstream(stages, [start])
    missing = selected - set(by_name)
    if missing:
        raise KeyError(f"not registered: {sorted(missing)}")
    reasons = {s.name: direct_reasons(root, s, by_name, src_root, strict_platform) for s in stages}
    # a due stage written by another stage's command makes that producer due
    for s in stages:
        if s.built_by and reasons[s.name] and not reasons[s.built_by]:
            reasons[s.built_by].append(f"rebuilds {s.name}")
    due: set[str] = set()
    items: list[PlanItem] = []
    for s in stages:  # topological
        r = list(reasons[s.name])
        up = sorted(i for i in s.inputs if i in due)
        if s.built_by and s.built_by in due and s.built_by not in up:
            up.append(s.built_by)
        r += [f"upstream due: {u}" for u in up]
        if r:
            due.add(s.name)
        if s.name not in selected:
            continue
        cmds = [list(c) for c in s.commands]
        if fetch and s.module and not s.built_by:
            cmds = [["-m", s.module, *a] for a in s.fetch] + cmds
        if not r:
            action = "fresh"
        elif s.built_by:
            action = f"via {s.built_by}"
        elif s.module is None:
            action = "manual"
        elif not cmds:
            action = "needs --fetch"
        else:
            action = "run"
        items.append(PlanItem(s.name, bool(r), action, r, cmds, s.guard_gb))
    return items


def render(items: list[PlanItem]) -> str:
    lines = []
    for n, it in enumerate(i for i in items if i.due):
        lines.append(f"{n + 1:>3}. {it.stage:<32} {it.action:<16} guard {it.guard_gb:.1f} GiB")
        for r in it.reasons:
            lines.append(f"       - {r}")
        for c in it.commands:
            lines.append("       $ python " + " ".join(c))
    fresh = [i.stage for i in items if not i.due]
    lines.append(f"due {sum(i.due for i in items)} of {len(items)}; fresh: {', '.join(fresh) if fresh else '-'}")
    return "\n".join(lines)


def guard_runner(root: Path, max_gb: float | None = None, wait_minutes: int = 60,
                 python: str = sys.executable) -> Runner:
    """Each command as ``python run_memory_guarded.py --job-gb CAP -- python *cmd`` (retried on 137 as build.py)."""
    def run(stage: Stage, cmd: list[str], cap: float, log: Path) -> int:
        cap = min(cap, max_gb) if max_gb else cap
        env = dict(os.environ, OPENBLAS_NUM_THREADS="1", PYTHONPATH=str(SRC_ROOT), ATX_ALPHA_PANEL_ROOT=str(root))
        full = [python, str(GUARD), "--job-gb", str(cap), "--wait-minutes", str(wait_minutes), "--quiet"]
        parent = os.environ.get("ATX_GUARD_JOB")
        if parent:
            full += ["--parent-job", parent]
        full += ["--", python, *cmd]
        flags = 0
        if os.name == "nt":
            flags = subprocess.CREATE_NO_WINDOW | (subprocess.CREATE_BREAKAWAY_FROM_JOB if parent else 0)
        rc = 1
        for attempt in range(3):
            with log.open("a", encoding="utf-8") as fh:
                fh.write(f"\n=== {stage.name} {' '.join(cmd)} attempt {attempt + 1} "
                         f"{dt.datetime.now(dt.UTC).isoformat(timespec='seconds')}\n")
                fh.flush()
                rc = subprocess.call(full, stdout=fh, stderr=subprocess.STDOUT, cwd=str(PACKAGE_ROOT), env=env,
                                     creationflags=flags)
            if rc != 137:
                break
        return rc
    return run


def write_binding(root: Path, stage: Stage, inputs: Mapping[str, str], src_root: Path = SRC_ROOT) -> Path:
    """Sidecar binding for the manifest a successful run just published."""
    manifest = read_manifest(root / stage.manifest) or {}
    code = {f: sha256_lf(p) for f, p in code_modules(stage, manifest, src_root).items() if p.is_file()}
    payload = {"stage": stage.name, "manifest": stage.manifest,
               "manifest_sha256": sha256_file(root / stage.manifest), "input_manifests_sha256": dict(inputs),
               "code": code, "run_at": dt.datetime.now(dt.UTC).isoformat(timespec="seconds")}
    path = sidecar_path(root, stage.name)
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_name(path.name + ".partial")
    tmp.write_text(json.dumps(payload, indent=2, sort_keys=True), encoding="utf-8")
    os.replace(tmp, path)
    return path


def execute(root: Path, stages: list[Stage], items: list[PlanItem], runner: Runner,
            src_root: Path = SRC_ROOT) -> list[dict[str, Any]]:
    """Run every ``run`` item in order; stop at the first failure. Returns one receipt per attempted stage."""
    by_name = {s.name: s for s in stages}
    log_dir = root / "_logs"
    log_dir.mkdir(parents=True, exist_ok=True)
    out: list[dict[str, Any]] = []
    for it in items:
        if it.action != "run":
            continue
        s = by_name[it.stage]
        inputs = {by_name[i].manifest: manifest_sha(root, by_name[i].manifest) for i in s.inputs}
        missing = [k for k, v in inputs.items() if v is None]
        rec: dict[str, Any] = {"stage": s.name, "commands": it.commands, "rc": None}
        if missing:
            rec["error"] = f"input manifests absent: {missing}"
            out.append(rec)
            break
        t0 = time.perf_counter()
        rc = 0
        for cmd in it.commands:
            rc = runner(s, cmd, it.guard_gb, log_dir / f"lake-{s.name}.log")
            if rc != 0:
                break
        rec.update(rc=rc, seconds=round(time.perf_counter() - t0, 1))
        if rc == 0 and (root / s.manifest).is_file():
            rec["binding"] = write_binding(root, s, {k: v for k, v in inputs.items() if v}, src_root).as_posix()
        elif rc == 0:
            rec.update(rc=-1, error=f"command succeeded but published no manifest at {s.manifest}")
        out.append(rec)
        print(json.dumps(rec), flush=True)
        if rec["rc"] != 0:
            break
    return out


def main(argv: list[str] | None = None) -> int:
    import argparse

    from . import registry

    ap = argparse.ArgumentParser(prog="python -m atx_db.stagelake.orchestrate", description=__doc__.splitlines()[0])
    mode = ap.add_mutually_exclusive_group(required=True)
    mode.add_argument("--dry-run", action="store_true")
    mode.add_argument("--run", action="store_true")
    ap.add_argument("--root", type=Path, default=None)
    ap.add_argument("--only", default=None, help="comma-separated stage names")
    ap.add_argument("--from", dest="start", default=None, help="this stage and everything downstream")
    ap.add_argument("--strict-platform", action="store_true", help="platform module changes (common.py) make stages due")
    ap.add_argument("--fetch", action="store_true", help="also run the stages' network landing commands")
    ap.add_argument("--max-gb", type=float, default=None, help="cap every stage's guard at this size")
    ap.add_argument("--wait-minutes", type=int, default=60)
    ap.add_argument("--json", type=Path, default=None)
    args = ap.parse_args(argv)
    root = args.root or default_root()
    stages = registry.load(strict=False)
    only = [x.strip() for x in args.only.split(",")] if args.only else None
    items = plan(root, stages, only, args.start, strict_platform=args.strict_platform, fetch=args.fetch)
    print(render(items), flush=True)
    receipts: list[dict[str, Any]] = []
    if args.run:
        receipts = execute(root, stages, items, guard_runner(root, args.max_gb, args.wait_minutes))
    if args.json:
        args.json.parent.mkdir(parents=True, exist_ok=True)
        args.json.write_text(json.dumps({"root": root.as_posix(), "plan": [asdict(i) for i in items],
                                         "receipts": receipts}, indent=2), encoding="utf-8")
    return 1 if any(r.get("rc") != 0 for r in receipts) else 0


if __name__ == "__main__":
    sys.exit(main())
