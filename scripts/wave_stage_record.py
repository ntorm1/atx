"""The verify, judge and record stages of a research wave (wave_stages.py): mechanics, binding and seal scan;
scoring, bundle, book and verdict; the ledger re-read, wave-result.json, the log section and the queue.
"""
from __future__ import annotations

from concurrent.futures import ThreadPoolExecutor
import datetime as dt
from pathlib import Path
import shutil

import cycle_resume as CR
import research_ledger
import wave_manifest as WM
import wave_queue
import wave_result
import wave_rules as WR
import wave_seal
import wave_steps as WS
from wave_context import Wave
from wave_stage_cell import nav_series, read_once, reader_plan, unbound
from wave_stage_preflight import ledger_state
import research_cycle as RC
from wave_stage_util import (EXIT_PIN, MARGINAL_KEYS, StageError, cyc, no_cell, phase_rows, pinned, reader_digests,
                             skipped)


# ------------------------------------------------------------------ verify
def verify(w: Wave, done: dict, log) -> dict:
    if no_cell(done):
        return skipped("no cell")
    mt = done["match"]
    chk = WR.mechanics_check(mt["mechanics"])
    for r in chk["rows"]:
        log(f"   mechanics {r['check']}: {r['value']} {r['limit']} -> {'pass' if r['pass'] else 'FAIL'}")
    # the binding of the NAV attempt that made the cell's NAV: the last completed one (a retry writes <nav>-run<k>)
    rows = phase_rows(w, mt["cell_spec"])
    navs = [r for r in rows if r["phase"] == "nav" and r["outcome"] == "completed" and r["exit_code"] == 0]
    nav_run = navs[-1]["run_dir"] if navs else f"{mt['nav']}-run"
    bpath = f"{nav_run}/cycle_binding.json"
    binding = w.read_json(bpath)
    digest = mt.get("spec_digest") or done["spec"]["spec_digest"]
    seal = wave_seal.scan(w, wave_seal.wave_logs(w, done), wave_seal.rulings(w, done))
    problems = []
    exe = nav_exe(w, done)
    if exe["equal"] is False and exe["ref"] == "missing":
        problems.append(f"the cell's NAV exe {exe['cell']} differs from the parent NAV's {exe['parent']} and the cell "
                        "spec's reference construction did not run on it (no completed ref run with that exe)")
    if not chk["pass"]:
        problems.append("mechanics FAIL (" + ", ".join(r["check"] for r in chk["rows"] if not r["pass"]) + ")")
    if not isinstance(binding, dict) or not binding.get("argv_sha256"):
        problems.append(f"the cell's NAV has no C-13 binding {bpath}")
    elif binding.get("spec_rule") != CR.SPEC_RULE or binding.get("spec_sha256") != digest:
        problems.append(f"the cell's NAV binding {bpath} names spec {binding.get('spec_sha256')} "
                        f"({binding.get('spec_rule')}), not the cell spec {mt['cell_spec']} ({CR.SPEC_RULE} {digest})")
    problems += wave_seal.problems(seal)
    if problems:
        raise StageError("verify: " + "; ".join(problems) + ": stop for the PM's ruling (the cell ran; no ledger "
                         "line was written)")
    log(f"   mechanics PASS; binding {binding['argv_sha256'][:12]} (spec {digest[:12]}); seal scan {seal['files']} "
        f"log(s), 0 tokens; NAV exe {str(exe['cell'])[:12]} vs parent {str(exe['parent'])[:12]} (ref {exe['ref']})")
    return {"mechanics": chk, "binding": {"path": bpath, "sha256": w.sha(bpath), "argv_sha256": binding["argv_sha256"],
                                          "spec_sha256": binding["spec_sha256"]},
            "nav_exe": exe,
            "seal_scan": seal, "identity": "the cycle's compare steps ran in every research_cycle run (a miss is its "
                                           "exit 4, which stops the stage)"}


def nav_exe(w: Wave, done: dict) -> dict:
    """P9 OR-2: the executable_sha256 of the parent NAV's and the cell NAV's last completed run receipts, whether they
    are equal (None when either is unknown) and the cell's reference construction: "ran" (a completed ref run on the
    cell's exe: research_cycle runs the ref when the exes differ), "missing" (the cell spec has a ref phase and none
    ran on that exe), or "none" (the spec has no ref phase, e.g. a rule cell). Every NAV run dir is enumerated as
    research_cycle enumerates them (cycle_resume.completed_exes, its attempt bound: review E1t0 minor 3)."""
    parent = CR.completed_exe_sha256(w, done["preflight"]["parent"]["nav"], RC.MAX_ATTEMPTS)
    bases = w.phase_bases(done["match"]["cell_spec"])
    cell = CR.completed_exe_sha256(w, bases["nav"], RC.MAX_ATTEMPTS)
    ran = "ref" in bases and cell in CR.completed_exes(w, bases["ref"], RC.MAX_ATTEMPTS)
    return {"parent": parent, "cell": cell, "equal": parent == cell if parent and cell else None,
            "ref": "none" if "ref" not in bases else "unknown" if not cell else "ran" if ran else "missing"}


def verify_plan(w: Wave, done: dict) -> list[str]:
    return ["#   mechanics rule v8-mech on the mechanics keys; C-13 binding of the cell's NAV; NAV exe sha256 of the parent "
            "vs the cell (recorded; differing, the cell's ref must have run on the cell's exe); seal scan of every log "
            "the wave produced (run dirs of the screen, the cell and its -gm copy, readers, consoles)"]


# ------------------------------------------------------------------ judge
def bundle_once(w: Wave, base: str, cell: str) -> dict:
    """The PM5-23 bundle of the cell against the parent, run once; its run's receipt must bind both NAVs' daily series
    at their SHA-256 now (which daily series the bundle read)."""
    out = w.wave_path("bundle.json")
    doc = w.read_json(out)
    series = nav_series(w, [base, cell])
    if doc is None:
        w.run(WS.bundle_argv(w.python, base, cell, out, w.free_run_dir(w.wave_path("bundle")), w.root, series,
                             w.manifest.get("driver")), "bundle (PM5-23)")
        doc = w.read_json(out)
        if doc is None:
            raise StageError(f"bundle wrote no {out}")
    if Path(str(doc.get("base"))).as_posix() != base or Path(str(doc.get("final"))).as_posix() != cell:
        raise StageError(f"{out} is the bundle of {doc.get('final')} vs {doc.get('base')}, not {cell} vs {base}")
    runs = [d for d in w.run_dirs(w.wave_path("bundle"))
            if (w.read_json(f"{d}/receipt.json") or {}).get("outcome") == "completed"]
    daily = [f for f in series if Path(f).name.startswith("daily_")]
    loose = unbound(w, runs[-1], daily) if runs else daily
    if not daily or loose:
        raise StageError(f"{out}: its run {runs[-1] if runs else '(none)'} did not bind the daily series {loose or daily}"
                         " at their SHA-256 now (the bundle read other series)", EXIT_PIN)
    p, lw = doc.get("paired") or {}, (doc.get("paired") or {}).get("lw") or {}
    return {"dsr": p.get("dsr"), "rho": p.get("rho"), "sessions": p.get("sessions"), "memmel_se": p.get("memmel_se"),
            "cbb_ci95": p.get("cbb_ci95"), "lw_ci95": lw.get("ci95"), "p_two_sided": lw.get("p_value"),
            "p_one_sided": lw.get("p_one_sided"), "freeze_gate": (doc.get("verdict") or {}).get("pass"),
            "path": out, "sha256": w.sha(out)}


def scored_verdict(w: Wave, cell: str) -> tuple[str, dict]:
    """(path, document) of the cell's cycle_verdict.json, which must carry the scoring blocks (paired, dsr)."""
    vpath = f"{w.outputs(cell)['cycle_dir']}/cycle_verdict.json"
    v = w.read_json(vpath)
    if not isinstance(v, dict) or not isinstance(v.get("paired"), dict) or not isinstance(v.get("dsr"), dict):
        raise StageError(f"judge: {vpath} has no scoring blocks (paired, dsr): a verdict spec scores its cell")
    return vpath, v


def judge(w: Wave, done: dict, log) -> dict:
    if no_cell(done):
        return skipped("no cell")
    m, mt = w.manifest, done["match"]
    cell, nav, parent_nav = mt["cell_spec"], mt["nav"], done["spec"]["reference_nav"]
    navs = {"cell": nav, "parent": parent_nav}

    def score() -> None:
        w.run(cyc(w, "run", cell), "the cell's monitor and summ (nav_summ scores and ledgers the cell)")
    budget = WM.driver(m, "host_budget_mib")
    if budget:          # P9 OR section 5: none reads what another writes; each bounded process holds its claim
        log(f"   the cell's summ || bundle || book reader, side by side under the host memory budget {budget} MiB")
        with ThreadPoolExecutor(max_workers=3) as pool:
            jobs = [pool.submit(score), pool.submit(bundle_once, w, parent_nav, nav),
                    pool.submit(read_once, w, "book", "book", navs)]
            _, bundle, read = [j.result() for j in jobs]        # the first failure, in this order, stops the stage
        vpath, v = scored_verdict(w, cell)
    else:
        score()
        vpath, v = scored_verdict(w, cell)
        bundle, read = bundle_once(w, parent_nav, nav), read_once(w, "book", "book", navs)
    outs = w.outputs(cell)
    book = read["navs"]
    crit = WR.criteria_rows(m["acceptance"].get("printed", []), book["cell"], book["parent"])
    verdict = dict(WR.judge(m["acceptance"]["rule"], v["paired"].get("dsr"), done["verify"]["mechanics"]["pass"], crit),
                   criteria=crit)
    log(f"   verdict ({verdict['rule']}): {'ACCEPTED' if verdict['accepted'] else 'NOT ACCEPTED'} {verdict['checks']}")
    return {"cycle_verdict": vpath, "cycle_verdict_sha256": w.sha(vpath),
            "summ_json_sha256": w.sha(f"{outs['cycle_dir']}/summ.json"), "paired": v["paired"], "dsr": v["dsr"],
            "pbo": v.get("pbo"), "bundle": bundle, "book": book, "readers": reader_digests(w, ["book"]),
            "marginal": [{k: x.get(k) for k in MARGINAL_KEYS} for x in v.get("marginal") or [] if isinstance(x, dict)],
            "verdict": verdict, "phases": phase_rows(w, cell)}


def judge_plan(w: Wave, done: dict) -> list[str]:
    mt, s = done.get("match") or {}, done.get("spec") or {}
    cell, nav = mt.get("cell_spec", "<the cell spec>"), mt.get("nav", "<the cell NAV>")
    parent = s.get("reference_nav", "<parent NAV>")
    budget = WM.driver(w.manifest, "host_budget_mib")
    side = [f"#   the three run side by side under the host memory budget {budget} MiB"] if budget else []
    return [WS.fmt_argv(cyc(w,"run", cell)),
            WS.fmt_argv(WS.bundle_argv(w.python, parent, nav, w.wave_path("bundle.json"), w.wave_path("bundle-run1"),
                                       w.root, nav_series(w, [parent, nav]), w.manifest.get("driver"))),
            reader_plan(w, "book", "book", {"cell": nav, "parent": parent})] + side + [
            f"#   verdict by {w.manifest['acceptance']['rule']}"]


# ------------------------------------------------------------------ record
def queue_files(w: Wave) -> set[str]:
    return {f"{wave_queue.QUEUE_DIR}/{c['id']}.json" for c in w.manifest.get("candidates") or []}


def record(w: Wave, done: dict, log) -> dict:
    m = w.manifest
    w.require_clean("record", queue_files(w))
    records, led = ledger_state(w)
    pre = done["preflight"]["ledger"]
    new_lines = records[pre["lines"]:]
    trial = None
    if not no_cell(done):
        trial = research_ledger.scored_trial_id(w.path(done["match"]["nav"]))
        if trial is None or not any(r.get("trial_id") == trial for r in new_lines):
            earlier = any(r.get("trial_id") == trial for r in records[:pre["lines"]])
            raise StageError(f"record: the cell's ledger line (trial {trial}) is not among the {len(new_lines)} lines "
                             "appended since preflight" + (": its daily series equals a trial ledgered before the wave "
                                                           "(an identity re-run adds no trial); root rules"
                                                           if earlier else ""))
    want = pre["n_before"] + (0 if no_cell(done) else 1)
    if led["n"] != want:
        raise StageError(f"record: ledger N is {led['n']}, expected {want} (N {pre['n_before']} + this wave's cell): "
                         "another trial was ledgered during the wave; root rules", EXIT_PIN)
    ledger = {"path": led["path"], "lines_before": pre["lines"], "lines_after": led["lines"], "head": led["head"],
              "n_before": pre["n_before"], "n_after": led["n"], "trial_id": trial,
              "admission_lines": [r.get("candidate") for r in new_lines if r.get("kind") == "admission"]}
    seal = wave_seal.scan(w, wave_seal.wave_logs(w, done), wave_seal.rulings(w, done))   # after the judge: the line
    bad = wave_seal.problems(seal)
    if bad:
        raise StageError("record: " + "; ".join(bad) + ": stop for the PM's ruling (no wave result is written)")
    doc = wave_result.build(w, done, ledger, seal)
    path, md = w.wave_path(wave_result.RESULT), w.wave_path(wave_result.LOG)
    sha = w.write_json(path, doc)
    w.path(md).write_text(wave_result.log_section(doc), encoding="utf-8", newline="\n")
    try:
        queued = wave_queue.record_wave(w.root, m, doc, dt.date.today().isoformat())
    except wave_queue.QueueError as exc:
        raise StageError(f"record: {exc}") from exc
    commit = w.commit_paths(f"wave {m['wave']}: queue status of {', '.join(queued)} ({wave_result.verdict_word(doc)})",
                            {f"{wave_queue.QUEUE_DIR}/{cid}.json" for cid in queued}) if queued else None
    copies = []
    if (m.get("record") or {}).get("copy_to"):
        dst = w.path(f"{m['record']['copy_to'].rstrip('/')}/{m['wave']}")
        dst.mkdir(parents=True, exist_ok=True)
        for rel in (path, md):
            shutil.copyfile(w.path(rel), dst / Path(rel).name)
            copies.append(w.rel(dst / Path(rel).name))
    log(f"   {path} ({sha[:12]}); log section {md}; N {ledger['n_before']} -> {ledger['n_after']}")
    return {"wave_result": path, "wave_result_sha256": sha, "log_section": md, "copies": copies, "ledger": ledger,
            "verdict": doc["verdict"], "next_parent": doc["next_parent"], "queue": {"set": queued, "commit": commit}}


def record_plan(w: Wave, done: dict) -> list[str]:
    return [f"#   ledger re-read (N {w.manifest['expect']['n_before']} -> + the cell); write "
            f"{w.wave_path(wave_result.RESULT)} and {w.wave_path(wave_result.LOG)}"]


def verify_inputs(w: Wave, done: dict) -> dict:
    mt = done.get("match") or {}
    if not mt.get("readers"):
        return {}
    now: dict = {"readers": reader_digests(w, list(mt["readers"]))}
    if mt.get("spec_digest"):
        now["spec_digest"] = w.spec_digest(mt["cell_spec"]) if w.exists(mt["cell_spec"]) else None
    return pinned("match", mt, now)


def judge_inputs(w: Wave, done: dict) -> dict:
    b = (done.get("verify") or {}).get("binding")
    return pinned("verify.binding", b, {"sha256": w.sha(b["path"])}) if b else {}


def record_inputs(w: Wave, done: dict) -> dict:
    jd = done.get("judge") or {}
    if not jd.get("cycle_verdict"):
        return {}
    return dict(pinned("judge", jd, {"cycle_verdict_sha256": w.sha(jd["cycle_verdict"]),
                                     "readers": reader_digests(w, list(jd["readers"]))}),
                **pinned("judge.bundle", jd["bundle"], {"sha256": w.sha(jd["bundle"]["path"])}))
