"""The run and match stages of a research wave (wave_stages.py), and the bounded readers they and the judge use:
the calibration run, gross matching (Ruling PM6-6) and the -gm copy.

A reader's or the bundle's output is reused only while its code is the code that made it (stale_code, P9 OR section
3, ruling E1-REUSE (b)): every *.py its run's receipt binds hashes as bound, and every in-repo module those import
(code_closure: e.g. nav_summ.py's backtest_integrity.py and dsr_total.py) is unchanged since the run's commit
(receipt source_sha). Cycle-level Python phases (card, the fields builder) are not code-keyed: research_cycle's
resume compares their argv and executable (the Python interpreter) only.
"""
from __future__ import annotations

import ast
from pathlib import Path
import re

import wave_manifest as WM
import wave_rules as WR
import wave_steps as WS
from wave_context import Wave
from wave_stage_util import (EXIT_PIN, StageError, cyc, exe_notes, no_cell, phase_rows, pinned, reader_digests,
                             skipped, write_spec_file)

# the code-keyed reuse of a reader's or the bundle's output (stale_code): where an in-repo module they import lives,
# and the loader calls that import a module by name (backtest_integrity: atx-engine/tools/<name>.py)
CODE_DIRS = ("scripts", "atx-impl/tools", "atx-engine/tools")
DYNAMIC_LOADERS = ("_engine_module",)


# ------------------------------------------------------------------ run
def screen_first(w: Wave, done: dict) -> bool:
    """speed.screen_first: a b library runs --screen before its cell, so its u pass is the screen's (no u-pass blend:
    the IC exe's --no-composition, which nothing downstream reads) and the cell resumes it, as a screen library's
    cell resumes its screen's."""
    return done["spec"].get("kind") == "b-library" and WM.speed(w.manifest, "screen_first")


def run_stage(w: Wave, done: dict, log) -> dict:
    if no_cell(done):
        return skipped("no cell")
    cell = done["spec"]["cell_spec"]
    if screen_first(w, done):
        w.run(cyc(w,"run", cell, "--screen"), "the b library's screen (its gate re-read)")
    w.run(cyc(w,"run", cell, "--stop-after", "nav"), "calibration run (--stop-after nav)")
    nav = w.outputs(cell)["nav"]
    if not w.exists(f"{nav}/summary.json"):
        raise StageError(f"run: no NAV output {nav}/summary.json")
    out = {"spec": cell, "nav": nav, "summary_sha256": w.sha(f"{nav}/summary.json"), "phases": phase_rows(w, cell)}
    notes = exe_notes(w, out["phases"])     # ruling E1-REUSE-a2: outputs reused on a rebuilt, unpinned exe
    if notes:
        log(f"   exe notes (reused on a rebuilt exe without an exes_sha256 pin, E1-REUSE-a2): {'; '.join(notes)}")
        out["exe_notes"] = notes
    return out


def run_plan(w: Wave, done: dict) -> list[str]:
    sp = done.get("spec") or {}
    cell = sp.get("cell_spec") or "<the cell spec (known after the spec stage)>"
    lines = []
    if not sp:
        lines.append("#   (a b library with speed.screen_first runs `run <cell> --screen` first)")
    elif screen_first(w, done):
        lines.append(WS.fmt_argv(cyc(w,"run", cell, "--screen")))
    return lines + [WS.fmt_argv(cyc(w,"run", cell, "--stop-after", "nav"))]


# ------------------------------------------------------------------ readers
def nav_series(w: Wave, navs) -> list[str]:
    """The series files of NAV dirs a reader or the bundle reads (daily_<scen>.csv, capacity_curve.csv): bound in the
    bounded runner's receipt with their SHA-256 (names only; nothing is read here)."""
    out = []
    for d in navs:
        p = w.path(d)
        out += [w.rel(f) for f in sorted(p.glob("daily_*.csv"))] if p.is_dir() else []
        out += [f"{d}/capacity_curve.csv"] if w.path(f"{d}/capacity_curve.csv").is_file() else []
    return out


def unbound(w: Wave, run_dir: str, files: list[str]) -> list[str]:
    """The files not bound in ``run_dir``'s receipt at their SHA-256 now (empty: the run read exactly these)."""
    r = w.read_json(f"{run_dir}/receipt.json") or {}
    got = {Path(b.get("path", "")).resolve(): b.get("sha256") for b in r.get("bindings") or [] if isinstance(b, dict)}
    return [f for f in files if got.get(w.path(f).resolve()) != w.sha(f)]


def last_completed(w: Wave, base: str) -> str | None:
    """The last run dir of ``base`` whose receipt completed (the run that wrote the output a stage reuses)."""
    runs = [d for d in w.run_dirs(base) if (w.read_json(f"{d}/receipt.json") or {}).get("outcome") == "completed"]
    return runs[-1] if runs else None


def imported_names(path: Path) -> set[str]:
    """The top-level module names ``path`` imports anywhere in its code (``import X``, ``from X import ...``, absolute
    only) plus the names it loads through DYNAMIC_LOADERS (backtest_integrity's ``_engine_module("era_pool")``)."""
    out = set()
    for node in ast.walk(ast.parse(path.read_text(encoding="utf-8"), filename=str(path))):
        if isinstance(node, ast.Import):
            out |= {a.name.split(".")[0] for a in node.names}
        elif isinstance(node, ast.ImportFrom) and not node.level and node.module:
            out.add(node.module.split(".")[0])
        elif isinstance(node, ast.Call) and getattr(node.func, "id", None) in DYNAMIC_LOADERS and node.args and \
                isinstance(node.args[0], ast.Constant) and isinstance(node.args[0].value, str):
            out.add(node.args[0].value)
    return out


def code_base(path: Path) -> Path | None:
    """The repository directory holding ``path`` (the first parent with CODE_DIRS' scripts or atx-impl/tools)."""
    return next((d for d in path.parents if (d / "scripts").is_dir() or (d / "atx-impl" / "tools").is_dir()), None)


def code_closure(paths) -> list[Path]:
    """The transitive in-repo import closure of the Python files ``paths`` (P9 ruling E1-REUSE (b)): each file and every
    module it imports (imported_names) that resolves to ``<name>.py`` in the importer's directory or in one of
    CODE_DIRS of its repository (code_base), recursively; standard-library and installed modules resolve to none. For
    the readers and the bundle: wave_readers.py, nav_summ.py and what they import (backtest_integrity.py,
    dsr_total.py, atx-engine/tools era_pool.py and research_window.py, ...)."""
    seen: dict[Path, None] = {}
    todo = [Path(p).resolve() for p in paths]
    while todo:
        p = todo.pop()
        if p in seen or not p.is_file():
            continue
        seen[p] = None
        base = code_base(p)
        dirs = [p.parent] + ([base / c for c in CODE_DIRS] if base else [])
        for name in sorted(imported_names(p)):
            hit = next((d / f"{name}.py" for d in dirs if (d / f"{name}.py").is_file()), None)
            if hit is not None:
                todo.append(hit.resolve())
    return sorted(seen, key=str)


def changed_imports(w: Wave, receipt: dict, bound: list[Path]) -> list[str]:
    """The in-repo modules the bound code imports (code_closure; the bound files aside, they are hashed in the
    receipt) that differ now from the commit the run recorded (receipt source_sha: the bounded runner refuses a dirty
    code pathspec, so that commit holds the code it ran): `git diff --name-only <source_sha> -- <modules>`. Modules
    outside the wave root, or a receipt without source_sha (--no-git), are not checked (empty), as before."""
    src = receipt.get("source_sha")
    if not bound or not (isinstance(src, str) and re.fullmatch(r"[0-9a-f]{40}", src)):
        return []
    root, own = Path(w.root).resolve(), {p.resolve() for p in bound}
    mods = [p.relative_to(root).as_posix() for p in code_closure(bound) if p not in own and p.is_relative_to(root)]
    if not mods:
        return []
    return [w.path(x).as_posix() for x in w.git("diff", "--name-only", src, "--", *mods).split()]


def stale_code(w: Wave, run_dir: str | None) -> list[str]:
    """The code files (*.py) ``run_dir``'s receipt binds that no longer hash as bound, and the in-repo modules they
    import that changed since the run's commit (changed_imports, P9 ruling E1-REUSE (b)): the reader or the bundle
    changed since that run wrote the output it would reuse (P9 OR section 3). Empty without a run or bound code."""
    r = (w.read_json(f"{run_dir}/receipt.json") if run_dir else None) or {}
    code = [b for b in r.get("bindings") or [] if isinstance(b, dict) and str(b.get("path", "")).endswith(".py")]
    stale = [b["path"] for b in code if w.sha(str(b["path"])) != b.get("sha256")]
    return stale + changed_imports(w, r, [Path(b["path"]) for b in code if w.path(str(b["path"])).is_file()])


def refuse_stale_code(w: Wave, out: str, run_dir: str | None) -> None:
    stale = stale_code(w, run_dir)
    if stale:
        raise StageError(f"{out}: its code {', '.join(stale)} changed since the run {run_dir} wrote it: never reused "
                         "(move the output aside, or start a new state dir)", EXIT_PIN)


def read_once(w: Wave, kind: str, name: str, navs: dict) -> dict:
    """The reader's output for these NAV dirs, run once under the bounded runner (a resumed stage re-uses it while
    every NAV's daily CSV still hashes as read and the reader's code as its run bound it: P9 OR section 3)."""
    out = w.wave_path("readers", f"{name}.json")
    doc = w.read_json(out)
    if doc is None:
        w.run(WS.reader_argv(w.python, kind, navs, out, w.free_run_dir(w.wave_path("readers", name)), w.root,
                             nav_series(w, navs.values()), w.manifest.get("driver")), f"{kind} reader")
        doc = w.read_json(out)
        if doc is None:
            raise StageError(f"{kind} reader wrote no {out}")
    refuse_stale_code(w, out, last_completed(w, w.wave_path("readers", name)))
    for key, d in navs.items():
        row = (doc.get("navs") or {}).get(key) or {}
        if row.get("dir") != d or w.sha(f"{d}/daily_{row.get('scenario')}.csv") != row.get("daily_csv_sha256"):
            raise StageError(f"{out}: {key} was read from {row.get('dir')!r} at another daily CSV; not {d}", EXIT_PIN)
    return doc


def reader_plan(w: Wave, kind: str, name: str, navs: dict) -> str:
    out = w.wave_path("readers", f"{name}.json")
    return WS.fmt_argv(WS.reader_argv(w.python, kind, navs, out, w.wave_path("readers", f"{name}-run1"), w.root,
                                      nav_series(w, navs.values()), w.manifest.get("driver")))


# ------------------------------------------------------------------ match
def match(w: Wave, done: dict, log) -> dict:
    if no_cell(done):
        return skipped("no cell")
    m, s = w.manifest, done["spec"]
    mode = m["gross_match"]
    cal = read_once(w, "mechanics", "mech-calibration", {"cell": done["run"]["nav"], "parent": s["reference_nav"]})
    gp = cal["navs"]["parent"]["mean_gross_leverage_all_rows"]
    gc = cal["navs"]["cell"]["mean_gross_leverage_all_rows"]
    rule = WR.GROSS_MATCH[mode] or {}
    out = {"mode": mode, "tolerance": rule.get("tolerance"), "g_parent": gp, "g_calibration": gc,
           "leverage_calibration": s["leverage"]}
    log(f"   gross: G {gc} vs G_parent {gp} (mode {mode}, tolerance {rule.get('tolerance')})")
    if WR.gross_matches(mode, gp, gc):
        return dict(out, corrected=False, cell_spec=s["cell_spec"], nav=done["run"]["nav"], leverage=s["leverage"],
                    g_cell=gc, mechanics=cal["navs"]["cell"], phases=done["run"]["phases"],
                    readers=reader_digests(w, ["mech-calibration"]))
    new = WR.matched_leverage(s["leverage"], gp, gc)
    gm = WS.gm_path(s["cell_spec"])
    w.require_clean("match", {gm})
    note = (f"Wave {m['wave']} at matched gross (Ruling PM6-6, research_cycle.py wave): calibration at L "
            f"{s['leverage']} gave all-rows S2 gross {gc:.10f} vs G_parent {gp:.10f}; L = {s['leverage']} x "
            f"{gp:.10f} / {gc:.10f} -> {new}.")
    write_spec_file(w, gm, WS.gm_doc(w.read_json(s["cell_spec"]), w.load_spec(s["cell_spec"]), new, note))
    nav = w.outputs(gm)["nav"]
    if nav == done["run"]["nav"]:
        raise StageError(f"{gm}: the matched NAV output equals the calibration's ({nav})")
    w.run(cyc(w,"lock", gm), "lock (dry: every pin of the -gm copy verified)")
    commit = w.commit_paths(f"wave {m['wave']}: {Path(gm).name} at L {new} (gross matching, PM6-6)", {gm}) \
        or w.committed(gm)
    w.run(cyc(w,"run", gm, "--stop-after", "nav"), "matched run (--stop-after nav)")
    got = read_once(w, "mechanics", "mech-matched", {"cell": nav})
    g2 = got["navs"]["cell"]["mean_gross_leverage_all_rows"]
    log(f"   matched: L {new}, G {g2} vs G_parent {gp}")
    if not WR.gross_matches(mode, gp, g2):
        raise StageError(f"gross match: the matched run's G {g2} is {abs(g2 - gp):.5f} from G_parent {gp} after "
                         f"{rule.get('corrections')} correction: stop for the PM's ruling (no ledger line)")
    return dict(out, corrected=True, cell_spec=gm, commit=commit, nav=nav, leverage=new, g_cell=g2,
                mechanics=got["navs"]["cell"], spec_digest=w.spec_digest(gm), phases=phase_rows(w, gm),
                readers=reader_digests(w, ["mech-calibration", "mech-matched"]))


def match_plan(w: Wave, done: dict) -> list[str]:
    run, s = done.get("run") or {}, done.get("spec") or {}
    navs = {"cell": run.get("nav", "<calibration NAV>"), "parent": s.get("reference_nav", "<parent NAV>")}
    return [reader_plan(w, "mechanics", "mech-calibration", navs),
            f"#   |G - G_parent| > tolerance ({w.manifest['gross_match']}): write <cell>-gm.json at L', then",
            "#   research_cycle.py lock <cell>-gm.json ; git commit ; research_cycle.py run <cell>-gm.json --stop-after "
            "nav ; mechanics reader (mech-matched)"]


def run_inputs(w: Wave, done: dict) -> dict:
    sp = done.get("spec") or {}
    if not sp.get("cell_spec"):
        return {}
    return pinned("spec", sp, {"spec_digest": w.spec_digest(sp["cell_spec"]) if w.exists(sp["cell_spec"]) else None})


def match_inputs(w: Wave, done: dict) -> dict:
    rn = done.get("run") or {}
    return pinned("run", rn, {"summary_sha256": w.sha(f"{rn['nav']}/summary.json")}) if rn.get("nav") else {}
