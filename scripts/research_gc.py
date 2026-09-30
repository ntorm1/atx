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

Review C-12: a candidate and a spec's store are compared as absolute paths under the root with '.', '..', separators,
a trailing slash and (on Windows) case normalised, so --under ./build-equity, build-equity\\ or an absolute path keeps
exactly the stores the canonical spelling keeps. Candidates are listed root-relative ('/'-separated), each once.
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


def path_key(root: Path, path) -> str:
    """One spelling of a store path (review C-12), for comparing a candidate with the specs' stores: absolute under
    the root (a relative path is taken from the root), '.' and '..' resolved, separators and a trailing slash
    normalised, case folded where the file system folds it (os.path.normcase: Windows)."""
    return os.path.normcase(os.path.normpath(os.path.join(os.path.abspath(root), str(path))))


def shown(root: Path, path) -> str:
    """How a store path is listed: root-relative and '/'-separated under the root, else absolute."""
    base = os.path.abspath(root)
    full = os.path.normpath(os.path.join(base, str(path)))
    try:
        rel = os.path.relpath(full, base)
    except ValueError:                      # another drive
        return Path(full).as_posix()
    return Path(full).as_posix() if rel == os.pardir or rel.startswith(os.pardir + os.sep) else Path(rel).as_posix()


def referenced(specs: list[Path], root: Path) -> dict[str, list[str]]:
    """{store dir (``path_key``): [spec names]} of the stores the specs' cycles use."""
    out: dict[str, list[str]] = {}
    for path in specs:
        spec = RC.load_spec(path)
        c = RC.Cycle(spec, RC.Resolver(root), verify=False)
        dirs = ([c.cache_dir()] if "ic" in spec else []) + ([c.fit_work_dir()] if "fit" in spec else [])
        for d in dirs:
            out.setdefault(path_key(root, d), []).append(spec["name"])
    return out


def candidates(root: Path, under: list[str]) -> list[str]:
    """The candidate store dirs under each --under dir (``shown`` spelling), each once however --under is spelled."""
    out, seen = [], set()
    for base in under:
        top = Path(os.path.normpath(os.path.join(os.path.abspath(root), base)))
        if not top.is_dir():
            continue
        for child in sorted(top.iterdir()):
            if not child.is_dir() or child.is_symlink():
                continue
            if child.name in DERIVED_STORES:
                found = [g for g in sorted(child.iterdir()) if g.is_dir() and not g.is_symlink()]
            else:
                found = [child] if STORE_NAME.search(child.name) else []
            for g in found:
                if path_key(root, g) not in seen:
                    seen.add(path_key(root, g))
                    out.append(shown(root, g))
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


def users(root: Path, rel: str, keep: dict[str, list[str]]) -> str | None:
    """Why candidate `rel` is kept: a spec uses it, or names its store base (the parent dir of a derived root). Both
    compared by ``path_key`` (review C-12)."""
    key = path_key(root, rel)
    if key in keep:
        return f"referenced by {', '.join(keep[key])}"
    base, base_key = rel.rsplit("/", 1)[0], os.path.dirname(key)
    return f"store base {base} named by {', '.join(keep[base_key])}" if base_key in keep else None


def gc(specs: list[Path], root: Path, under: list[str], apply: bool, log=print) -> dict:
    keep = referenced(specs, root)
    report = {"keep": [], "gc": [], "skip": [], "deleted": []}
    for rel in candidates(root, under):
        path = root / rel
        mib = size_of(path) / (1 << 20)
        why_kept = users(root, rel, keep)
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
