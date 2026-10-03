"""The run and match stages of a research wave (wave_stages.py), and the bounded readers they and the judge use:
the calibration run, gross matching (Ruling PM6-6) and the -gm copy.
"""
from __future__ import annotations

from pathlib import Path

import wave_manifest as WM
import wave_rules as WR
import wave_steps as WS
from wave_context import Wave
from wave_stage_util import (EXIT_PIN, StageError, cyc, no_cell, phase_rows, pinned, reader_digests, skipped,
                             write_spec_file)


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
    return {"spec": cell, "nav": nav, "summary_sha256": w.sha(f"{nav}/summary.json"), "phases": phase_rows(w, cell)}


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


def stale_code(w: Wave, run_dir: str | None) -> list[str]:
    """The code files (*.py) ``run_dir``'s receipt binds that no longer hash as bound: the reader or the bundle changed
    since that run wrote the output it would reuse (P9 OR section 3). Empty without a run or bound code."""
    r = (w.read_json(f"{run_dir}/receipt.json") if run_dir else None) or {}
    return [b["path"] for b in r.get("bindings") or [] if isinstance(b, dict) and
            str(b.get("path", "")).endswith(".py") and w.sha(str(b["path"])) != b.get("sha256")]


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
