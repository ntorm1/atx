"""The stages of one research wave (research_wave.py), in order; each is a stage_chain.Stage.

  preflight  the manifest is committed (its commit is the pre-registration) and every pin holds: the parent cell spec
             loads, may run and has its NAV; the fields manifest hashes to the pin, is complete and sealed no later
             than the research window's seal; a candidate's declared fields are in it; no path names a sealed year;
             the ledger's chain verifies, its N equals expect.n_before, and the budget holds the wave's new admission
             trials (and the cell); the code pathspec is clean (the bounded runner refuses otherwise)
  register   (library wave) one add-alpha per frozen string into library NAME (K1 plan of record saved under the wave
             dir), then one commit of exactly the files add-alpha wrote
  screen     research_cycle.py run lib-NAME.json --screen (u, fit, card, marginal, gate: the admission trials are
             ledgered by the gate); the sign rule applied by code to every string's row of the fit's admission.json
             (a string without a row stops the stage)
  spec       the cell: the screen library when every string stays, the b library (add-alpha --name NAME+b on the kept
             strings, same trial ids) when the sign rule dropped some, none when the gate stopped; a rule wave writes
             the template's cell file on the parent with its constants and locks it; one commit
  run        research_cycle.py run CELL --stop-after nav: the calibration run at the parent's L (no summ, no ledger line)
  match      gross matching by the named mode: the mechanics reader (keys only) on the calibration NAV and the parent's;
             within tolerance the calibration run is the cell, else the -gm copy at L', locked, committed, run
  verify     the mechanics rule on the cell's mechanics keys (before any return is read), the NAV's C-13 binding, a
             scan of every run log for a date at or after the seal; a failure stops for a ruling (no ledger line)
  judge      research_cycle.py run CELL (monitor and summ: nav_summ scores and ledgers the cell), the PM5-23 bundle
             against the parent, the book reader, the verdict by the named acceptance rule
  record     the ledger re-read (chain, the cell's line, N advanced by exactly the cell), wave-result.json, the
             ready-to-paste log section, copies to record.copy_to
A stage that does not apply (no strings in a rule wave, no cell after the gate) records why and passes.
"""
from __future__ import annotations

import datetime as dt
import hashlib
import json
from pathlib import Path
import re
import shutil

import research_add_alpha as AA
import research_cycle as RC
import research_ledger
import research_spec
import research_tree
import wave_manifest as WM
import wave_queue
import wave_result
import wave_rules as WR
import wave_steps as WS
from wave_context import Wave, stage_chain

import research_window as RW   # atx-engine/tools (wave_context puts it on the path): the one source of the seal

StageError = stage_chain.StageError
Stage = stage_chain.Stage
EXIT_PIN = 3
SPECS_V8 = "scripts/specs/v8"
YEAR = re.compile(r"(?<![0-9])((?:19|20)[0-9]{2})(?![0-9])")
DATE = re.compile(r"(?<![0-9])((?:19|20)[0-9]{2}-[01][0-9]-[0-3][0-9])(?![0-9])")
ROW_KEYS = ("id", "status", "runner_sign", "s_k", "sign_agrees", "failed_checks", "redundant_with")
MARGINAL_KEYS = ("id", "ic21", "ic21_hac_t", "marginal_ic21", "marginal_hac_t", "max_abs_rho", "max_rho_member")


def lib_spec(name: str) -> str:
    return f"{SPECS_V8}/lib-{name}.json"


def library_wave(w: Wave) -> bool:
    return "candidates" in w.manifest


def skipped(why: str) -> dict:
    return {"skipped": why}


def no_cell(done: dict) -> bool:
    return not (done.get("spec") or {}).get("cell_spec")


# ------------------------------------------------------------------ preflight
def preflight_inputs(w: Wave, done: dict) -> dict:
    m = w.manifest
    parent = m["parent"]["spec"]
    out = {"manifest_sha256": w.manifest_sha,
           "parent_spec_digest": w.spec_digest(parent) if w.exists(parent) else None,
           "fields_manifest_sha256": w.sha(f"{m['fields']['dir']}/manifest.json")}
    if not library_wave(w):
        out["template_sha256"] = w.sha(m["rule_cell"]["template"])
    return out


def sealed_years(text: str) -> list[str]:
    return sorted({y for y in YEAR.findall(text) if int(y) >= RW.FIRST_SEALED_YEAR})


LIBRARIES = "atx-impl/strategies/libraries"     # add-alpha's library definitions (a cycle's name is its library's)


def admission_trial_id(cid: str, dsl_sha: str, role_sha: str) -> str:
    """The trial_id the gate ledgers for one screened string (cycle_admission.admission_lines: [candidate, its DSL
    SHA-256, the role pin, the research window id]), so a string ledgered already is no new trial."""
    bi = research_ledger.backtest_integrity()
    ident = json.dumps([cid, dsl_sha, role_sha, bi.window_id()], separators=(",", ":"))
    return bi.trial_id("admission", hashlib.sha256(ident.encode()).hexdigest())


def role_pin(w: Wave) -> str | None:
    """The role pin of the wave's cells: the parent's inputs.role (add-alpha keeps it), else the file's SHA-256."""
    role = w.load_spec(w.manifest["parent"]["spec"])["inputs"].get("role") or {}
    return role.get("sha256") or (w.sha(role["path"]) if role.get("path") else None)


def rescreens_of(w: Wave, cycle, cache: dict) -> set:
    """The re-screen strings of a cycle's library: libraries/<cycle>.json "rescreens" (a cycle is named after its
    library; a -gm copy, e.g. v8x3b-gm, after its library plus -gm); none without the file."""
    if cycle not in cache:
        names = [str(cycle)] + ([str(cycle)[:-3]] if str(cycle).endswith("-gm") else []) if cycle else []
        doc = next((d for d in (w.read_json(f"{LIBRARIES}/{n}.json") for n in names) if isinstance(d, dict)), None)
        cache[cycle] = set((doc or {}).get("rescreens") or [])
    return cache[cycle]


def budget_selects(b: dict, origin) -> bool:
    return "admission_origin" not in b or origin == b["admission_origin"]


def admission_used(w: Wave, records: list[dict], b: dict) -> int:
    """The admission trials the budget has spent (the hand count): the ledger's admission lines of cycles with the
    budget's prefix and, when set, its origin class; a re-screen line (its candidate in the cycle library's
    ``rescreens``: Ruling R2-e, 0 admission trials, e.g. X-4's nine) is left out, as the hand count leaves it out."""
    bi = research_ledger.backtest_integrity()
    cache: dict = {}
    return sum(c for r, c in zip(records, bi.trial_counts(records))
               if r.get("kind") == "admission" and str(r.get("cycle", "")).startswith(b["admission_cycle_prefix"])
               and budget_selects(b, r.get("origin")) and r.get("candidate") not in rescreens_of(w, r.get("cycle"), cache))


def admission_new(w: Wave, records: list[dict], b: dict) -> list[str]:
    """The wave's strings that are new admission trials: not a re-screen, of the budget's origin class, and whose
    trial_id is not in the ledger yet (a fresh state dir of a screened wave adds nothing)."""
    have = {r.get("trial_id") for r in records}
    role = role_pin(w)
    return [c["id"] for c in w.manifest.get("candidates") or []
            if not c.get("rescreen") and budget_selects(b, c["origin"]) and
            admission_trial_id(c["id"], c["dsl_sha256"], role or "") not in have]


def ledger_state(w: Wave) -> tuple[list[dict], dict]:
    bi = research_ledger.backtest_integrity()
    p = w.path(w.manifest["ledger"])
    try:
        records = bi.ledger_read(p)
        head = bi.ledger_head(p)
    except (OSError, ValueError) as exc:
        raise StageError(f"ledger {w.manifest['ledger']}: {exc}", EXIT_PIN) from exc
    return records, {"path": w.manifest["ledger"], "lines": len(records), "head": head,
                     "n": bi.ledger_n(records, True)}


def budget_check(w: Wave, records: list[dict], n_before: int) -> tuple[dict, list[str]]:
    b = w.manifest["budget"]
    out, problems = {"id": b["id"]}, []
    if "admission_cap" in b:
        used, new = admission_used(w, records, b), admission_new(w, records, b) if library_wave(w) else []
        out.update(admission_used=used, admission_new=len(new), admission_new_ids=new, admission_cap=b["admission_cap"],
                   admission_cycle_prefix=b["admission_cycle_prefix"], admission_origin=b.get("admission_origin"))
        if used + len(new) > b["admission_cap"]:
            problems.append(f"budget {b['id']}: {used} admission trials used + {len(new)} new > cap "
                            f"{b['admission_cap']} (a ruling raises the cap; the wave does not)")
    if "construction_cap" in b:
        out["construction_cap"] = b["construction_cap"]
        if n_before + 1 > b["construction_cap"]:
            problems.append(f"budget {b['id']}: N {n_before} + this cell > construction cap {b['construction_cap']}")
    return out, problems


def fields_check(w: Wave) -> tuple[dict, list[str]]:
    f = w.manifest["fields"]
    rel = f"{f['dir']}/manifest.json"
    got, problems = w.sha(rel), []
    if got != f["manifest_sha256"]:
        return {}, [f"fields {rel} is {got}, the manifest pins {f['manifest_sha256']}"]
    doc = w.read_json(rel) or {}
    end = (doc.get("seal") or {}).get("exclusive_end")
    if doc.get("status") != "complete":
        problems.append(f"fields {rel}: status {doc.get('status')!r}, not complete")
    if not isinstance(end, str) or end > RW.SEAL_DATE:
        problems.append(f"fields {rel}: seal.exclusive_end {end!r} is not on or before the research seal "
                        f"{RW.SEAL_DATE} ({RW.WINDOW_ID})")
    names = [row.get("name") for row in doc.get("fields", []) if isinstance(row, dict)]
    for c in w.manifest.get("candidates") or []:
        miss = [x for x in c.get("fields", []) if x not in names]
        if miss:
            problems.append(f"candidate {c['id']}: fields {miss} are not in {rel}")
    return {"dir": f["dir"], "manifest_sha256": got, "rows": len(names), "seal_exclusive_end": end}, problems


def preflight(w: Wave, done: dict, log) -> dict:
    m, problems = w.manifest, []
    commit = w.committed(w.manifest_rel)
    if commit is None:
        problems.append(f"manifest {w.manifest_rel} is not committed as it is: commit it first (its commit is the "
                        "wave's pre-registration)")
    dirty = w.dirty_code()
    if dirty:
        problems.append(f"the code pathspec is dirty ({', '.join(dirty[:6])}): the bounded runner refuses; commit first")
    parent = m["parent"]["spec"]
    if not w.exists(parent):
        raise StageError(f"parent spec {parent} not found", EXIT_PIN)
    spec = w.load_spec(parent)
    refusal = research_spec.run_refusal(spec, w.path(parent), research_tree.REPO)
    if refusal:
        problems.append(f"parent {parent} is not a cell that ran: {refusal}")
    outs = w.outputs(parent)
    if not w.exists(f"{outs['nav']}/summary.json"):
        problems.append(f"parent cell {parent} has no NAV output {outs['nav']}/summary.json")
    if outs["ledger"] != m["ledger"]:
        problems.append(f"the parent's summ.ledger {outs['ledger']!r} is not the wave's ledger {m['ledger']!r}")
    fields, more = fields_check(w)
    problems += more
    for where in (m["fields"]["dir"], parent, m["out_dir"], m.get("library") or ""):
        years = sealed_years(where)
        if years:
            problems.append(f"{where} names sealed year(s) {years} (seal {RW.SEAL_DATE})")
    records, led = ledger_state(w)
    if led["n"] != m["expect"]["n_before"]:
        problems.append(f"ledger N is {led['n']}, the wave was planned on N {m['expect']['n_before']} (another cell "
                        "was ledgered first: re-pin expect.n_before)")
    budget, more = budget_check(w, records, led["n"])
    problems += more
    problems += wave_queue.queue_check(w.root, m)
    if not library_wave(w):
        rc = m["rule_cell"]
        doc = w.read_json(rc["template"]) if w.exists(rc["template"]) else None
        if w.sha(rc["template"]) != rc["template_sha256"] or not research_spec.is_template(doc):
            problems.append(f"rule_cell.template {rc['template']} is not the pinned cell template")
    if problems:
        raise StageError("preflight refused: " + "; ".join(problems), EXIT_PIN)
    log(f"   preflight: manifest commit {(commit or '')[:12]}, parent {outs['name']} (L {outs['leverage']}), fields "
        f"{fields['manifest_sha256'][:12]}, ledger {led['lines']} lines N {led['n']}, budget {budget}")
    return {"manifest": {"path": w.manifest_rel, "sha256": w.manifest_sha, "commit": commit},
            "parent": dict(outs, library=m["parent"]["library"], spec_digest=w.spec_digest(parent),
                           python=w.python),
            "fields": fields, "ledger": {"path": led["path"], "lines": led["lines"], "head": led["head"],
                                         "n_before": led["n"]},
            "budget": budget, "window": {"id": RW.WINDOW_ID, "seal": RW.SEAL_DATE}}


def preflight_plan(w: Wave, done: dict) -> list[str]:
    return [f"#   checks: manifest committed; parent {w.manifest['parent']['spec']} loads and has its NAV; fields pin; "
            f"seal {RW.SEAL_DATE}; ledger chain, N == {w.manifest['expect']['n_before']}; budget; code pathspec clean"]


# ------------------------------------------------------------------ register (library waves)
def add_alpha(w: Wave, c: dict, name: str) -> list[str]:
    m = w.manifest
    return WS.add_alpha_argv(w.python, c, parent=m["parent"]["library"], parent_spec=m["parent"]["spec"], name=name,
                             fields_dir=m["fields"]["dir"], plan_out=w.wave_path("plans", name, f"{c['id']}.json"))


def rewrite_spec(w: Wave, rel: str, fn, why: str) -> bool:
    """Apply ``fn`` to a spec add-alpha has just written (uncommitted); write and lock (dry: every pin verified) only
    when it changes. A resumed stage finds it applied already."""
    doc = w.read_json(rel)
    new = fn(doc)
    if new == doc:
        return False
    w.path(rel).write_text(json.dumps(new, indent=2) + "\n", encoding="utf-8", newline="\n")
    w.log(f"   {rel}: {why}")
    w.run(WS.cycle_argv(w.python, "lock", rel), f"lock (dry) {rel}")
    return True


def replaces(cands: list[dict]) -> bool:
    return any(c.get("kind") == WR.REPLACE for c in cands)


def add_alpha_files(w: Wave, name: str) -> set[str]:
    """The files add-alpha writes for library NAME (research_add_alpha.add_alpha): the alpha registry, the library
    definition and its prereg stub, the IC library and slim recipe named by the definition's id, the cell spec."""
    st = AA.STRATEGIES
    out = {f"{st}/{AA.G.REGISTRY_PATH}", f"{st}/libraries/{name}.json", f"{st}/libraries/{name}.prereg.md",
           lib_spec(name)}
    doc = w.read_json(f"{st}/libraries/{name}.json")
    if isinstance(doc, dict) and isinstance(doc.get("id"), str):
        out |= {f"{st}/{n}" for n in AA.G.output_names(doc)}
    return out


def register(w: Wave, done: dict, log) -> dict:
    if not library_wave(w):
        return skipped("a rule wave registers no strings")
    m = w.manifest
    name, plans = m["library"], {}
    w.require_clean("register", add_alpha_files(w, name))
    for c in m["candidates"]:
        w.run(add_alpha(w, c, name), f"add-alpha {c['id']}")
        plans[c["id"]] = w.sha(w.wave_path("plans", name, f"{c['id']}.json"))
    if replaces(m["candidates"]):
        rewrite_spec(w, lib_spec(name), WS.pool_only_marginal, "marginal on the pool only (PM6-8 (i), PM7-32)")
    commit = w.commit_paths(f"wave {m['wave']}: register library {name} ({len(m['candidates'])} frozen strings; "
                            f"manifest {w.manifest_rel} {w.manifest_sha[:12]})", add_alpha_files(w, name))
    spec = lib_spec(name)
    if not w.exists(spec):
        raise StageError(f"add-alpha wrote no {spec}")
    return {"library": name, "spec": spec, "spec_sha256": w.sha(spec), "commit": commit or w.head(), "plans": plans}


def register_plan(w: Wave, done: dict) -> list[str]:
    if not library_wave(w):
        return ["#   (rule wave: no strings to register)"]
    lines = [WS.fmt_argv(add_alpha(w, c, w.manifest["library"])) for c in w.manifest["candidates"]]
    return lines + ["#   then: git add -- <the files add-alpha wrote>; git commit -q -m \"wave ...: register ...\" -- "
                    "<the same paths>"]


# ------------------------------------------------------------------ screen
def screen(w: Wave, done: dict, log) -> dict:
    if not library_wave(w):
        return skipped("a rule wave has no admission strings")
    m, spec = w.manifest, done["register"]["spec"]
    done_screen = w.run(WS.cycle_argv(w.python, "run", spec, "--screen"), "screen", ok=(RC.EXIT_OK, RC.EXIT_GATE))
    outs = w.outputs(spec)
    # the rows: the fit's admission.json (every candidate). cycle_verdict.json lists only gate.admitted, from which
    # add-alpha leaves a mixed wave's re-screens out (they are gate.report), so it cannot carry the sign rule
    apath = f"{outs['fit']}/admission.json"
    adm = w.read_json(apath)
    if not isinstance(adm, dict) or not isinstance(adm.get("candidates"), list):
        raise StageError(f"screen: no candidate rows in {apath}")
    rows = [{k: x.get(k) for k in ROW_KEYS} for x in adm["candidates"] if isinstance(x, dict)]
    vpath = f"{outs['cycle_dir']}/cycle_verdict.json"
    doc = w.read_json(vpath)
    marginal = [{k: x.get(k) for k in MARGINAL_KEYS} for x in (doc or {}).get("marginal") or [] if isinstance(x, dict)]
    ids = {c["id"] for c in m["candidates"]}
    try:
        dec = WR.screen_decision(m["sign_rule"], m["candidates"], [r for r in rows if r["id"] in ids])
    except WR.RuleError as exc:
        raise StageError(f"screen: {apath}: {exc}") from exc
    gate_ok = done_screen.returncode == RC.EXIT_OK
    for row in dec["rows"]:
        log(f"   {row['id']}: status {row['status']}, runner sign {row['runner_sign']} vs prior "
            f"{row['prior_sign']:+d} -> {row['decision']} ({row['reason']})")
    log(f"   gate {'PASS' if gate_ok else 'FAIL (no string admitted with its prior sign): no cell'}; kept "
        f"{dec['kept']}, dropped {dec['dropped']}")
    return {"spec": spec, "gate_exit": done_screen.returncode, "admission": apath, "admission_sha256": w.sha(apath),
            "verdict": vpath, "verdict_sha256": w.sha(vpath), "rows": [r for r in rows if r["id"] in ids],
            "marginal": marginal, "decision": dec, "cell": bool(gate_ok and dec["kept"])}


def screen_plan(w: Wave, done: dict) -> list[str]:
    if not library_wave(w):
        return ["#   (rule wave: no screen)"]
    return [WS.fmt_argv(WS.cycle_argv(w.python, "run", lib_spec(w.manifest["library"]), "--screen")),
            f"#   then: the sign rule {w.manifest['sign_rule']} on every string's row of the fit's admission.json"]


# ------------------------------------------------------------------ spec
def write_spec_file(w: Wave, rel: str, doc: dict) -> None:
    """Write a cell spec once: an existing file must hold exactly these bytes (a resumed stage), else a stop."""
    text = json.dumps(doc, indent=2) + "\n"
    p = w.path(rel)
    if p.is_file():
        if p.read_text(encoding="utf-8") != text:
            raise StageError(f"{rel} exists with other content: never overwritten (rename the cell)", EXIT_PIN)
        return
    p.parent.mkdir(parents=True, exist_ok=True)
    p.write_text(text, encoding="utf-8", newline="\n")


def cell_out(w: Wave, done: dict, spec: str, kind: str, commit: str | None, library: str) -> dict:
    outs = w.outputs(spec)
    parent_nav = done["preflight"]["parent"]["nav"]
    if outs["ledger"] != w.manifest["ledger"]:
        raise StageError(f"cell {spec}: summ.ledger {outs['ledger']!r} is not the wave's ledger")
    if outs["reference_nav"] != parent_nav:
        raise StageError(f"cell {spec}: its paired reference {outs['reference_nav']!r} is not the parent's NAV "
                         f"{parent_nav!r}")
    return {"cell_spec": spec, "kind": kind, "library": library, "commit": commit, "spec_digest": w.spec_digest(spec),
            "name": outs["name"], "nav": outs["nav"], "leverage": outs["leverage"], "reference_nav": parent_nav}


def spec_stage(w: Wave, done: dict, log) -> dict:
    m = w.manifest
    if not library_wave(w):
        rc = m["rule_cell"]
        cell = WS.rule_cell_path(rc["template"], rc, m["wave"])
        doc = WS.rule_cell_doc(w.read_json(rc["template"]), rc["template"], m["parent"]["spec"], rc, m["wave"])
        w.require_clean("spec", {cell})
        if not w.exists(cell):            # a first write: the cell's NAV must be new (a template is used once)
            nav = w.outputs_of(doc, cell)["nav"]
            if w.exists(nav):
                raise StageError(f"rule cell {cell}: its NAV output {nav} exists already (a template is used once: "
                                 "copy it under a new name with new outputs)", EXIT_PIN)
        write_spec_file(w, cell, doc)
        w.run(WS.cycle_argv(w.python, "lock", cell, "--write"), "lock the rule cell")
        commit = w.commit_paths(f"wave {m['wave']}: rule cell {Path(cell).name} on {m['parent']['spec']}", {cell})
        return cell_out(w, done, cell, "rule", commit or w.committed(cell), m["parent"]["library"])
    sc = done["screen"]
    if not sc["cell"]:
        return {"cell_spec": None, "kind": "none", "reason": "the gate admitted no string with its prior sign"
                if sc["gate_exit"] != RC.EXIT_OK else "the sign rule kept no string"}
    kept = sc["decision"]["kept"]
    if kept == [c["id"] for c in m["candidates"]]:
        return cell_out(w, done, sc["spec"], "screen-library", None, m["library"])
    name = WM.b_library(m)
    w.require_clean("spec", add_alpha_files(w, name))
    for c in m["candidates"]:
        if c["id"] in kept:
            w.run(add_alpha(w, c, name), f"add-alpha {c['id']} into {name}")
    if WM.speed(m, "reuse_screen_marginal"):
        rewrite_spec(w, lib_spec(name), WS.without_marginal, "no second marginal pass: the screen's rows are carried "
                                                             "(speed.reuse_screen_marginal; report only)")
    elif replaces([c for c in m["candidates"] if c["id"] in kept]):
        rewrite_spec(w, lib_spec(name), WS.pool_only_marginal, "marginal on the pool only (PM6-8 (i), PM7-32)")
    commit = w.commit_paths(f"wave {m['wave']}: cell library {name} = {m['parent']['library']} + {', '.join(kept)} "
                            f"({m['sign_rule']}: dropped {', '.join(sc['decision']['dropped'])})",
                            add_alpha_files(w, name))
    return cell_out(w, done, lib_spec(name), "b-library", commit or w.committed(lib_spec(name)), name)


def spec_plan(w: Wave, done: dict) -> list[str]:
    m = w.manifest
    if not library_wave(w):
        rc = m["rule_cell"]
        cell = WS.rule_cell_path(rc["template"], rc, m["wave"])
        return [f"#   write {cell}: {rc['template']} with parent {m['parent']['spec']} and the constants",
                WS.fmt_argv(WS.cycle_argv(w.python, "lock", cell, "--write")), "#   then: git add / commit -- " + cell]
    kept = ((done.get("screen") or {}).get("decision") or {}).get("kept")
    cands = [c for c in m["candidates"] if kept is None or c["id"] in kept]
    head = ("#   if the sign rule drops a string (known after the screen): add-alpha of each kept string into "
            f"{WM.b_library(m)}, then commit" if kept is None else f"#   kept {kept}")
    return [head] + [WS.fmt_argv(add_alpha(w, c, WM.b_library(m))) for c in cands]


# ------------------------------------------------------------------ run
def phase_rows(w: Wave, spec_rel: str) -> list[dict]:
    """The bounded-runner receipts of a cell's phases (timings: seconds, peak MiB, outcome), every attempt."""
    spec = w.load_spec(spec_rel)
    c = RC.Cycle(spec, RC.Resolver(w.root), verify=False)
    bases = {"u": c.out(spec["ic"]["u_output"]), "fit": c.out(spec["fit"]["output"], keyed=False),
             "w": c.out(spec["ic"]["w_output"]), "nav": c.out(spec["nav"]["output"])}
    for key, phase in (("card", "card"), ("marginal", "marginal"), ("ref", "ref"), ("monitor", "monitor")):
        if key in spec:
            bases[phase] = c.out(spec[key]["output"])
    rows = []
    for phase, base in bases.items():
        rows += _receipts(w, phase, f"{base}-run")
    for phase in ("check", "summ"):
        rows += _receipts(w, phase, f"{c.cycle_dir()}/{phase}-run")
    return rows


def _receipts(w: Wave, phase: str, prefix: str) -> list[dict]:
    p = w.path(prefix)
    out = []
    for d in sorted(p.parent.glob(p.name + "*")) if p.parent.is_dir() else []:
        tail = d.name[len(p.name):]
        r = w.read_json(f"{w.rel(d)}/receipt.json") if d.is_dir() and (tail == "" or tail.isdigit()) else None
        if isinstance(r, dict):
            out.append({"phase": phase, "run_dir": w.rel(d), "outcome": r.get("outcome"), "exit_code": r.get("exit_code"),
                        "seconds": r.get("wall_seconds"), "peak_mib": (r.get("sampled_peak_tree_rss_bytes") or 0) >> 20})
    return out


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
        w.run(WS.cycle_argv(w.python, "run", cell, "--screen"), "the b library's screen (its gate re-read)")
    w.run(WS.cycle_argv(w.python, "run", cell, "--stop-after", "nav"), "calibration run (--stop-after nav)")
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
        lines.append(WS.fmt_argv(WS.cycle_argv(w.python, "run", cell, "--screen")))
    return lines + [WS.fmt_argv(WS.cycle_argv(w.python, "run", cell, "--stop-after", "nav"))]


# ------------------------------------------------------------------ readers
def read_once(w: Wave, kind: str, name: str, navs: dict) -> dict:
    """The reader's output for these NAV dirs, run once under the bounded runner (a resumed stage re-uses it while
    every NAV's daily CSV still hashes as read)."""
    out = w.wave_path("readers", f"{name}.json")
    doc = w.read_json(out)
    if doc is None:
        w.run(WS.reader_argv(w.python, kind, navs, out, w.free_run_dir(w.wave_path("readers", name))),
              f"{kind} reader")
        doc = w.read_json(out)
        if doc is None:
            raise StageError(f"{kind} reader wrote no {out}")
    for key, d in navs.items():
        row = (doc.get("navs") or {}).get(key) or {}
        if row.get("dir") != d or w.sha(f"{d}/daily_{row.get('scenario')}.csv") != row.get("daily_csv_sha256"):
            raise StageError(f"{out}: {key} was read from {row.get('dir')!r} at another daily CSV; not {d}", EXIT_PIN)
    return doc


def reader_plan(w: Wave, kind: str, name: str, navs: dict) -> str:
    out = w.wave_path("readers", f"{name}.json")
    return WS.fmt_argv(WS.reader_argv(w.python, kind, navs, out, w.wave_path("readers", f"{name}-run1")))


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
                    g_cell=gc, mechanics=cal["navs"]["cell"], phases=done["run"]["phases"])
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
    w.run(WS.cycle_argv(w.python, "lock", gm), "lock (dry: every pin of the -gm copy verified)")
    commit = w.commit_paths(f"wave {m['wave']}: {Path(gm).name} at L {new} (gross matching, PM6-6)", {gm}) \
        or w.committed(gm)
    w.run(WS.cycle_argv(w.python, "run", gm, "--stop-after", "nav"), "matched run (--stop-after nav)")
    got = read_once(w, "mechanics", "mech-matched", {"cell": nav})
    g2 = got["navs"]["cell"]["mean_gross_leverage_all_rows"]
    log(f"   matched: L {new}, G {g2} vs G_parent {gp}")
    if not WR.gross_matches(mode, gp, g2):
        raise StageError(f"gross match: the matched run's G {g2} is {abs(g2 - gp):.5f} from G_parent {gp} after "
                         f"{rule.get('corrections')} correction: stop for the PM's ruling (no ledger line)")
    return dict(out, corrected=True, cell_spec=gm, commit=commit, nav=nav, leverage=new, g_cell=g2,
                mechanics=got["navs"]["cell"], spec_digest=w.spec_digest(gm), phases=phase_rows(w, gm))


def match_plan(w: Wave, done: dict) -> list[str]:
    run, s = done.get("run") or {}, done.get("spec") or {}
    navs = {"cell": run.get("nav", "<calibration NAV>"), "parent": s.get("reference_nav", "<parent NAV>")}
    return [reader_plan(w, "mechanics", "mech-calibration", navs),
            f"#   |G - G_parent| > tolerance ({w.manifest['gross_match']}): write <cell>-gm.json at L', then",
            "#   research_cycle.py lock <cell>-gm.json ; git commit ; research_cycle.py run <cell>-gm.json --stop-after "
            "nav ; mechanics reader (mech-matched)"]


# ------------------------------------------------------------------ verify
def seal_scan(w: Wave, run_dirs: list[str]) -> dict:
    """Date tokens at or after the seal in the run logs (counts and file names only; no content is reported)."""
    files, hits = 0, []
    for d in sorted(set(run_dirs)):
        for name in ("stdout.log", "stderr.log"):
            p = w.path(f"{d}/{name}")
            if p.is_file():
                files += 1
                n = sum(1 for t in DATE.findall(p.read_text(encoding="utf-8", errors="replace")) if t >= RW.SEAL_DATE)
                if n:
                    hits.append({"file": f"{d}/{name}", "tokens": n})
    return {"files": files, "seal": RW.SEAL_DATE, "tokens_at_or_after_seal": sum(h["tokens"] for h in hits),
            "where": hits}


def verify(w: Wave, done: dict, log) -> dict:
    if no_cell(done):
        return skipped("no cell")
    mt = done["match"]
    chk = WR.mechanics_check(mt["mechanics"])
    for r in chk["rows"]:
        log(f"   mechanics {r['check']}: {r['value']} {r['limit']} -> {'pass' if r['pass'] else 'FAIL'}")
    nav_run = f"{mt['nav']}-run"
    binding = w.read_json(f"{nav_run}/cycle_binding.json")
    readers = [w.rel(p) for p in sorted(w.path(w.wave_path("readers")).glob("*-run*")) if p.is_dir()]
    seal = seal_scan(w, [r["run_dir"] for r in done["run"]["phases"] + mt["phases"]] + readers)
    problems = []
    if not chk["pass"]:
        problems.append("mechanics FAIL (" + ", ".join(r["check"] for r in chk["rows"] if not r["pass"]) + ")")
    if not isinstance(binding, dict) or not binding.get("argv_sha256"):
        problems.append(f"the cell's NAV has no C-13 binding {nav_run}/cycle_binding.json")
    if seal["tokens_at_or_after_seal"]:
        problems.append(f"{seal['tokens_at_or_after_seal']} date token(s) at or after the seal {RW.SEAL_DATE} in "
                        f"{[h['file'] for h in seal['where']]} (inspect: a data date is a seal breach)")
    if problems:
        raise StageError("verify: " + "; ".join(problems) + ": stop for the PM's ruling (the cell ran; no ledger "
                         "line was written)")
    log(f"   mechanics PASS; binding {binding['argv_sha256'][:12]}; seal scan {seal['files']} log(s), 0 tokens")
    return {"mechanics": chk, "binding": {"path": f"{nav_run}/cycle_binding.json", "argv_sha256":
                                          binding["argv_sha256"], "spec_sha256": binding.get("spec_sha256")},
            "seal_scan": seal, "identity": "the cycle's compare steps ran in every research_cycle run (a miss is its "
                                           "exit 4, which stops the stage)"}


def verify_plan(w: Wave, done: dict) -> list[str]:
    return ["#   mechanics rule v8-mech on the mechanics keys; C-13 binding of the cell's NAV; seal scan of every run log"]


# ------------------------------------------------------------------ judge
def bundle_once(w: Wave, base: str, cell: str) -> dict:
    out = w.wave_path("bundle.json")
    doc = w.read_json(out)
    if doc is None:
        w.run(WS.bundle_argv(w.python, base, cell, out, w.free_run_dir(w.wave_path("bundle"))), "bundle (PM5-23)")
        doc = w.read_json(out)
        if doc is None:
            raise StageError(f"bundle wrote no {out}")
    if Path(str(doc.get("base"))).as_posix() != base or Path(str(doc.get("final"))).as_posix() != cell:
        raise StageError(f"{out} is the bundle of {doc.get('final')} vs {doc.get('base')}, not {cell} vs {base}")
    p, lw = doc.get("paired") or {}, (doc.get("paired") or {}).get("lw") or {}
    return {"dsr": p.get("dsr"), "rho": p.get("rho"), "sessions": p.get("sessions"), "memmel_se": p.get("memmel_se"),
            "cbb_ci95": p.get("cbb_ci95"), "lw_ci95": lw.get("ci95"), "p_two_sided": lw.get("p_value"),
            "p_one_sided": lw.get("p_one_sided"), "freeze_gate": (doc.get("verdict") or {}).get("pass"),
            "path": out, "sha256": w.sha(out)}


def judge(w: Wave, done: dict, log) -> dict:
    if no_cell(done):
        return skipped("no cell")
    m, mt = w.manifest, done["match"]
    cell, nav, parent_nav = mt["cell_spec"], mt["nav"], done["spec"]["reference_nav"]
    w.run(WS.cycle_argv(w.python, "run", cell), "the cell's monitor and summ (nav_summ scores and ledgers the cell)")
    outs = w.outputs(cell)
    vpath = f"{outs['cycle_dir']}/cycle_verdict.json"
    v = w.read_json(vpath)
    if not isinstance(v, dict) or not isinstance(v.get("paired"), dict) or not isinstance(v.get("dsr"), dict):
        raise StageError(f"judge: {vpath} has no scoring blocks (paired, dsr): a verdict spec scores its cell")
    bundle = bundle_once(w, parent_nav, nav)
    book = read_once(w, "book", "book", {"cell": nav, "parent": parent_nav})["navs"]
    crit = WR.criteria_rows(m["acceptance"].get("printed", []), book["cell"], book["parent"])
    verdict = dict(WR.judge(m["acceptance"]["rule"], v["paired"].get("dsr"), done["verify"]["mechanics"]["pass"], crit),
                   criteria=crit)
    log(f"   verdict ({verdict['rule']}): {'ACCEPTED' if verdict['accepted'] else 'NOT ACCEPTED'} {verdict['checks']}")
    return {"cycle_verdict": vpath, "cycle_verdict_sha256": w.sha(vpath),
            "summ_json_sha256": w.sha(f"{outs['cycle_dir']}/summ.json"), "paired": v["paired"], "dsr": v["dsr"],
            "pbo": v.get("pbo"), "bundle": bundle, "book": book, "verdict": verdict,
            "phases": phase_rows(w, cell)}


def judge_plan(w: Wave, done: dict) -> list[str]:
    mt, s = done.get("match") or {}, done.get("spec") or {}
    cell, nav = mt.get("cell_spec", "<the cell spec>"), mt.get("nav", "<the cell NAV>")
    parent = s.get("reference_nav", "<parent NAV>")
    return [WS.fmt_argv(WS.cycle_argv(w.python, "run", cell)),
            WS.fmt_argv(WS.bundle_argv(w.python, parent, nav, w.wave_path("bundle.json"), w.wave_path("bundle-run1"))),
            reader_plan(w, "book", "book", {"cell": nav, "parent": parent}),
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
    doc = wave_result.build(w, done, ledger)
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


STAGES = [Stage("preflight", preflight, preflight_inputs, preflight_plan),
          Stage("register", register, None, register_plan),
          Stage("screen", screen, None, screen_plan),
          Stage("spec", spec_stage, None, spec_plan),
          Stage("run", run_stage, None, run_plan),
          Stage("match", match, None, match_plan),
          Stage("verify", verify, None, verify_plan),
          Stage("judge", judge, None, judge_plan),
          Stage("record", record, None, record_plan)]
