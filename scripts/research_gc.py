"""research_cycle.py cache gc: list (and with --apply delete) the candidate caches and fit work dirs no spec references.

  research_cycle.py cache gc --keep-referenced-by SPEC [SPEC ...] [--under DIR ...] [--root R] [--apply]

Candidates (platform review P-14, plan section 14 disk risk) are only the store directories under each --under dir
(default build-equity): a direct child whose name contains "candidate-cache" or "fit-work" (the v7 per-version stores,
e.g. mega-candidate-cache-v61-r7), and each child of the derived stores candidate-cache/ and fit-work/ (the v8
<role sha16>-<window id> roots). A candidate is kept when a listed spec's cycle uses it (its ic.cache / fit.work_dir,
or the root derived from its role pin when the spec omits them; no --suffix), and a derived root <base>/<child> is kept
whole when a listed spec names its store base <base> (C-1: fit.work_dir build-equity/fit-work, which the fitter, the
card and the monitor extend with <role sha16>-<window id> themselves, the monitor reading every window of its role);
anything else is listed with its size and deleted only with --apply. A candidate holding research state at its top level (a receipt, a run start, a summary,
a manifest, a ledger, a daily CSV) is never deleted. Ledgers, receipts, roles, fields dirs and NAV cells are never
candidates. Sizes add file sizes: hard-linked copies (cp -al seeds) count in full, so the space freed can be smaller.
"""
from __future__ import annotations

import argparse
import os
from pathlib import Path
import re
import shutil
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
import research_cycle as RC  # noqa: E402

STORE_NAME = re.compile(r"candidate-cache|fit-work")
DERIVED_STORES = ("candidate-cache", "fit-work")
PROTECTED = re.compile(r"(receipt|start|summary|manifest)\.json|trials\.jsonl|daily_.*\.csv")


def referenced(specs: list[Path], root: Path) -> dict[str, list[str]]:
    """{store dir (root-relative, posix): [spec names]} of the stores the specs' cycles use."""
    out: dict[str, list[str]] = {}
    for path in specs:
        spec = RC.load_spec(path)
        c = RC.Cycle(spec, RC.Resolver(root), verify=False)
        dirs = ([c.cache_dir()] if "ic" in spec else []) + ([c.fit_work_dir()] if "fit" in spec else [])
        for d in dirs:
            out.setdefault(Path(d).as_posix().rstrip("/"), []).append(spec["name"])
    return out


def candidates(root: Path, under: list[str]) -> list[str]:
    out = []
    for base in under:
        top = root / base
        if not top.is_dir():
            continue
        for child in sorted(top.iterdir()):
            if not child.is_dir() or child.is_symlink():
                continue
            rel = f"{base.rstrip('/')}/{child.name}"
            if child.name in DERIVED_STORES:
                out += [f"{rel}/{g.name}" for g in sorted(child.iterdir()) if g.is_dir() and not g.is_symlink()]
            elif STORE_NAME.search(child.name):
                out.append(rel)
    return out


def size_of(path: Path) -> int:
    total = 0
    for dirpath, _, files in os.walk(path):
        for f in files:
            try:
                total += (Path(dirpath) / f).stat().st_size
            except OSError:
                pass
    return total


def protected(path: Path) -> str | None:
    """The first top-level file of `path` that is research state (never a store's content)."""
    return next((p.name for p in sorted(path.iterdir()) if p.is_file() and PROTECTED.fullmatch(p.name)), None)


def users(rel: str, keep: dict[str, list[str]]) -> str | None:
    """Why candidate `rel` is kept: a spec uses it, or names its store base (the parent dir of a derived root)."""
    if rel in keep:
        return f"referenced by {', '.join(keep[rel])}"
    base = rel.rsplit("/", 1)[0]
    return f"store base {base} named by {', '.join(keep[base])}" if base in keep else None


def gc(specs: list[Path], root: Path, under: list[str], apply: bool, log=print) -> dict:
    keep = referenced(specs, root)
    report = {"keep": [], "gc": [], "skip": [], "deleted": []}
    for rel in candidates(root, under):
        path = root / rel
        mib = size_of(path) / (1 << 20)
        why_kept = users(rel, keep)
        if why_kept:
            report["keep"].append(rel)
            log(f"keep  {rel}  {mib:,.1f} MiB  ({why_kept})")
            continue
        why = protected(path)
        if why:
            report["skip"].append(rel)
            log(f"skip  {rel}  {mib:,.1f} MiB  (holds research state: {why}; never deleted)")
            continue
        report["gc"].append(rel)
        log(f"gc    {rel}  {mib:,.1f} MiB")
        if apply:
            shutil.rmtree(path)
            report["deleted"].append(rel)
    total = sum(size_of(root / r) for r in report["gc"] if (root / r).exists()) / (1 << 20)
    log(f"== {len(report['gc'])} unreferenced store dirs" + (f" deleted ({len(report['deleted'])})" if apply else
        f", {total:,.1f} MiB (dry run: --apply deletes them)") + f"; {len(report['keep'])} kept, "
        f"{len(report['skip'])} skipped")
    return report


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(prog="research_cycle.py cache gc", description=__doc__.split("\n", 1)[0],
                                 epilog=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--keep-referenced-by", nargs="+", required=True, metavar="SPEC")
    ap.add_argument("--under", action="append", default=None, help="store parent dir (default build-equity)")
    ap.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[1])
    ap.add_argument("--apply", action="store_true", help="delete the unreferenced store dirs")
    a = ap.parse_args(argv)
    try:
        specs = [RC.find_spec(s) for s in a.keep_referenced_by]
        gc(specs, a.root.resolve(), a.under or [RC.DEFAULT_OUT_ROOT], a.apply)
    except RC.CycleError as exc:
        print(f"research_cycle cache gc: {exc}", file=sys.stderr)
        return exc.code
    return 0
