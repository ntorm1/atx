"""The register, screen and spec stages of a research wave (wave_stages.py): add-alpha into the wave's library,
the screen and the sign rule (the IC exe's sign verb, P9 lane SIGN), the cell spec (screen library, b library or rule
cell).
"""
from __future__ import annotations

import json
from pathlib import Path

import research_add_alpha as AA
import research_cycle as RC
import wave_manifest as WM
import wave_rules as WR
import wave_kinds as WK
import wave_stage_kinds as SK
import wave_steps as WS
from wave_context import Wave
from wave_stage_util import (EXIT_PIN, MARGINAL_KEYS, ROW_KEYS, StageError, cyc, lib_spec, library_wave, lock_exes,
                             phase_rows, pin_exes, pinned, skipped, timings_on, verdict_file, write_spec_file)


# ------------------------------------------------------------------ register (library waves)
def add_alpha(w: Wave, c: dict, name: str) -> list[str]:
    m = w.manifest
    return WS.add_alpha_argv(w.python, c, parent=m["parent"]["library"], parent_spec=m["parent"]["spec"], name=name,
                             fields_dir=SK.fields_dir(w, {}), plan_out=w.wave_path("plans", name, f"{c['id']}.json"),
                             root=w.root)


def rewrite_spec(w: Wave, rel: str, fn, why: str) -> bool:
    """Apply ``fn`` to a spec add-alpha has just written (uncommitted); write and lock (dry: every pin verified) only
    when it changes. A resumed stage finds it applied already."""
    doc = w.read_json(rel)
    new = fn(doc)
    if new == doc:
        return False
    w.path(rel).write_text(json.dumps(new, indent=2) + "\n", encoding="utf-8", newline="\n")
    w.log(f"   {rel}: {why}")
    w.run(cyc(w,"lock", rel), f"lock (dry) {rel}")
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
        if SK.kind(w) == "role":       # v2 kind role (K-P9-9): the role and its fields are built here
            return {"build": SK.build(w)}
        return skipped("a rule wave registers no strings")
    m = w.manifest
    name, plans = m["library"], {}
    w.require_clean("register", add_alpha_files(w, name))
    build = SK.build(w)                # v2 kind field: the fields dir the strings are registered on ({} otherwise)
    for c in m["candidates"]:
        w.run(add_alpha(w, c, name), f"add-alpha {c['id']}")
        plans[c["id"]] = w.sha(w.wave_path("plans", name, f"{c['id']}.json"))
    if replaces(m["candidates"]):
        rewrite_spec(w, lib_spec(name), WS.pool_only_marginal, "marginal on the pool only (PM6-8 (i), PM7-32)")
    if m.get("marginal"):
        ids = [c["id"] for c in m["candidates"]]              # candidates_only (P9 ruling P4): the wave's strings
        rewrite_spec(w, lib_spec(name), lambda d: WS.marginal_ruled(d, m["marginal"], ids),
                     f"marginal as ruled ({m['marginal']['ruling']}): {m['marginal']}")
    pin_exes(w, lib_spec(name))
    commit = w.commit_paths(f"wave {m['wave']}: register library {name} ({len(m['candidates'])} frozen strings; "
                            f"manifest {w.manifest_rel} {w.manifest_sha[:12]})", add_alpha_files(w, name))
    spec = lib_spec(name)
    if not w.exists(spec):
        raise StageError(f"add-alpha wrote no {spec}")
    out = {"library": name, "spec": spec, "spec_sha256": w.sha(spec), "commit": commit or w.head(), "plans": plans}
    return dict(out, build=build) if build else out


def register_plan(w: Wave, done: dict) -> list[str]:
    if not library_wave(w):
        return SK.build_plan(w) or ["#   (rule wave: no strings to register)"]
    lines = SK.build_plan(w) + [WS.fmt_argv(add_alpha(w, c, w.manifest["library"])) for c in w.manifest["candidates"]]
    if w.manifest.get("marginal"):
        lines.append(f"#   then: marginal as ruled ({w.manifest['marginal']['ruling']}): {w.manifest['marginal']} "
                     "(the library spec rewritten, lock dry)")
    if lock_exes(w):
        lines.append(WS.fmt_argv(cyc(w, "lock", lib_spec(w.manifest["library"]), "--exes", "--write")))
    return lines + ["#   then: git add -- <the files add-alpha wrote>; git commit -q -m \"wave ...: register ...\" -- "
                    "<the same paths>"]


# ------------------------------------------------------------------ screen
def screen(w: Wave, done: dict, log) -> dict:
    if not library_wave(w):
        return skipped("a rule wave has no admission strings")
    m, spec = w.manifest, done["register"]["spec"]
    done_screen = w.run(cyc(w,"run", spec, "--screen"), "screen", ok=(RC.EXIT_OK, RC.EXIT_GATE))
    outs = w.outputs(spec)
    # the rows: the fit's admission.json (every candidate). cycle_verdict.json lists only gate.admitted, from which
    # add-alpha leaves a mixed wave's re-screens out (they are gate.report), so it cannot carry the sign rule
    apath = f"{outs['fit']}/admission.json"
    adm = w.read_json(apath)
    if not isinstance(adm, dict) or not isinstance(adm.get("candidates"), list):
        raise StageError(f"screen: no candidate rows in {apath}")
    rows = [{k: x.get(k) for k in ROW_KEYS} for x in adm["candidates"] if isinstance(x, dict)]
    vpath = verdict_file(w, outs["cycle_dir"], "screen")
    doc = w.read_json(vpath)
    marginal = [{k: x.get(k) for k in MARGINAL_KEYS} for x in (doc or {}).get("marginal") or [] if isinstance(x, dict)]
    ids = {c["id"] for c in m["candidates"]}
    try:
        sign_in = WR.sign_rows(m["candidates"], [r for r in rows if r["id"] in ids])
    except WR.RuleError as exc:
        raise StageError(f"screen: {apath}: {exc}") from exc
    sign = sign_verb(w, spec, m["sign_rule"], sign_in)          # the sign rule: the IC exe's sign verb (C++)
    dec = sign.pop("decision")
    gate_ok = done_screen.returncode == RC.EXIT_OK
    for row in dec["rows"]:
        log(f"   {row['id']}: status {row['status']}, runner sign {row['runner_sign']} vs prior "
            f"{row['prior_sign']:+d} -> {row['decision']} ({row['reason']})")
    log(f"   gate {'PASS' if gate_ok else 'FAIL (no string admitted with its prior sign): no cell'}; kept "
        f"{dec['kept']}, dropped {dec['dropped']}")
    out = {"spec": spec, "gate_exit": done_screen.returncode, "admission": apath, "admission_sha256": w.sha(apath),
           "verdict": vpath, "verdict_sha256": w.sha(vpath), "rows": [r for r in rows if r["id"] in ids],
           "marginal": marginal, "decision": dec, "sign": sign, "cell": bool(gate_ok and dec["kept"])}
    if timings_on(w):                  # P9 OR section 5: the screen's u, fit, card, marginal rows (a b library's cell
        out["phases"] = phase_rows(w, spec)                       # spec is another spec)
    return out


def screen_plan(w: Wave, done: dict) -> list[str]:
    if not library_wave(w):
        return ["#   (rule wave: no screen)"]
    rule = w.manifest["sign_rule"]
    return [WS.fmt_argv(cyc(w,"run", lib_spec(w.manifest["library"]), "--screen")),
            f"#   then: the sign rule {rule} on every string's row of the fit's admission.json, by the IC exe's sign "
            f"verb: <exes.ic> sign --rule {rule} --rows {w.wave_path(SIGN_DIR, 'rows-<k>.json')} --output "
            f"{w.wave_path(SIGN_DIR, 'receipt-<k>.json')}"]


# ------------------------------------------------------------------ the sign verb (P9 lane SIGN, ruling C3-11)
SIGN_DIR = "sign"


def sign_exe(w: Wave, spec: str) -> str:
    """The IC exe the screen spec runs (its effective exes.ic, research_cycle's rule; load_spec refuses a spec with an
    ic section and no exes.ic), held to its exes_sha256 pin when the spec has one (exit 3 otherwise: the sign verb
    runs on the exe the screen ran on)."""
    doc = w.load_spec(spec)
    exe = RC.effective_exes(doc)["ic"]
    pin, now = (doc.get(RC.EXES_PIN) or {}).get("ic"), w.sha(exe)
    if pin is not None and now != pin:
        raise StageError(f"screen: the IC exe {exe} is sha256 {str(now)[:12]}, not its pin {str(pin)[:12]} ({spec} "
                         f"{RC.EXES_PIN}.ic): the sign verb runs only on the pinned exe", EXIT_PIN)
    return exe


def sign_verb(w: Wave, spec: str, rule: str, rows: list[dict]) -> dict:
    """The sign rule ``rule`` on the screen's rows (WR.sign_rows) by the IC exe's sign verb (atx-equity-strategy-ic
    sign: the engine's research/admission pm7_35, so no Python copy decides). Each call writes the rows file
    <out_dir>/sign/rows-<k>.json and the verb writes its receipt-<k>.json (k the first free number: a retried screen
    gets its own pair, nothing is overwritten); the decision is read from the receipt (WR.verb_decision, exit 3 when
    it is not the verb's answer to these rows). Returns {exe, exe_sha256, rows, rows_sha256, receipt, receipt_sha256,
    decision}."""
    exe = sign_exe(w, spec)
    k = 1
    while w.exists(w.wave_path(SIGN_DIR, f"rows-{k}.json")) or w.exists(w.wave_path(SIGN_DIR, f"receipt-{k}.json")):
        k += 1
    rows_rel, receipt_rel = w.wave_path(SIGN_DIR, f"rows-{k}.json"), w.wave_path(SIGN_DIR, f"receipt-{k}.json")
    rows_sha = w.write_json(rows_rel, {"rule": rule, "rows": rows}, exclusive=True)
    w.run([str(w.path(exe)), "sign", "--rule", rule, "--rows", rows_rel, "--output", receipt_rel], f"sign ({rule})")
    try:
        dec = WR.verb_decision(rule, rows, w.read_json(receipt_rel), rows_sha)
    except (WR.RuleError, ValueError) as exc:
        raise StageError(f"screen: {receipt_rel}: {exc}", EXIT_PIN) from exc
    return {"exe": exe, "exe_sha256": w.sha(exe), "rows": rows_rel, "rows_sha256": rows_sha, "receipt": receipt_rel,
            "receipt_sha256": w.sha(receipt_rel), "decision": dec}


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
    if SK.kind(w) in WK.SCHEMA_ONLY:   # preflight refuses it; never reached
        raise StageError(f"kind {SK.kind(w)} is schema only (lane D3)", EXIT_PIN)
    if not library_wave(w):
        cell = SK.cell_path(w)         # v2 kinds (K-P9-9): the template with the kind's derived constants
        doc = SK.cell_doc(w, done, cell)
        w.require_clean("spec", {cell})
        if not w.exists(cell):            # a first write: the cell's NAV must be new (a template is used once)
            nav = w.outputs_of(doc, cell)["nav"]
            if w.exists(nav):
                raise StageError(f"rule cell {cell}: its NAV output {nav} exists already (a template is used once: "
                                 "copy it under a new name with new outputs)", EXIT_PIN)
        write_spec_file(w, cell, doc, same=WS.unpinned)      # a resumed stage finds it locked already
        w.run(cyc(w,"lock", cell, "--write", *(["--exes"] if lock_exes(w) else [])), "lock the rule cell")
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
    # the b library's marginal mode: pool-only when a kept string replaces a member (PM6-8 (i)), else themes. The
    # screen's rows are carried only when the screen ran the same mode (marginal_ic21 and its HAC t depend on it)
    want = "pool-only" if (replaces([c for c in m["candidates"] if c["id"] in kept]) or
                           (m.get("marginal") or {}).get("pool_only")) else "themes"
    have = WS.marginal_mode(w.read_json(sc["spec"]))
    reuse = WM.speed(m, "reuse_screen_marginal") and have == want
    if reuse:
        rewrite_spec(w, lib_spec(name), WS.without_marginal, "no second marginal pass: the screen's per-row marginal "
                                                             f"fields ({want}) are carried (report only)")
    elif want == "pool-only":
        rewrite_spec(w, lib_spec(name), WS.pool_only_marginal, "marginal on the pool only (PM6-8 (i), PM7-32)")
    if not reuse and m.get("marginal"):
        rewrite_spec(w, lib_spec(name), lambda d: WS.marginal_ruled(d, m["marginal"], kept),   # P4: the kept ids
                     f"marginal as ruled ({m['marginal']['ruling']}): {m['marginal']}")
    pin_exes(w, lib_spec(name))
    commit = w.commit_paths(f"wave {m['wave']}: cell library {name} = {m['parent']['library']} + {', '.join(kept)} "
                            f"({m['sign_rule']}: dropped {', '.join(sc['decision']['dropped'])})",
                            add_alpha_files(w, name))
    return dict(cell_out(w, done, lib_spec(name), "b-library", commit or w.committed(lib_spec(name)), name),
                marginal={"mode": want, "screen_mode": have, "reuse": reuse})


def spec_plan(w: Wave, done: dict) -> list[str]:
    m = w.manifest
    if SK.kind(w) in WK.SCHEMA_ONLY:
        return [f"#   (kind {SK.kind(w)}: schema only until lane D3, no cell)"]
    if not library_wave(w):
        _, _, template = SK.base_rule_cell(w)
        cell = SK.cell_path(w)
        return [f"#   write {cell}: {template} with parent {m['parent']['spec']} and the constants"
                + (f" (kind {SK.kind(w)}: the derived constants, wave_kinds.py)" if SK.kind(w) else ""),
                WS.fmt_argv(cyc(w,"lock", cell, "--write", *(["--exes"] if lock_exes(w) else []))),
                "#   then: git add / commit -- " + cell]
    kept = ((done.get("screen") or {}).get("decision") or {}).get("kept")
    cands = [c for c in m["candidates"] if kept is None or c["id"] in kept]
    head = ("#   if the sign rule drops a string (known after the screen): add-alpha of each kept string into "
            f"{WM.b_library(m)}, then commit" if kept is None else f"#   kept {kept}")
    pin = [WS.fmt_argv(cyc(w, "lock", lib_spec(WM.b_library(m)), "--exes", "--write"))] if lock_exes(w) else []
    return [head] + [WS.fmt_argv(add_alpha(w, c, WM.b_library(m))) for c in cands] + pin


def screen_inputs(w: Wave, done: dict) -> dict:
    r = done.get("register") or {}
    return pinned("register", r, {"spec_sha256": w.sha(r["spec"])}) if r.get("spec") else {}


def spec_inputs(w: Wave, done: dict) -> dict:
    sc = done.get("screen") or {}
    if not sc.get("admission"):
        return {}
    out = pinned("screen", sc, {"admission_sha256": w.sha(sc["admission"])})
    if sc.get("sign"):       # P9 lane SIGN: the receipt the decision was read from (a screen before SIGN has none)
        out.update(pinned("screen.sign", sc["sign"], {"receipt_sha256": w.sha(sc["sign"]["receipt"])}))
    return out
