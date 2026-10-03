"""The register, screen and spec stages of a research wave (wave_stages.py): add-alpha into the wave's library,
the screen and the sign rule, the cell spec (screen library, b library or rule cell).
"""
from __future__ import annotations

import json
from pathlib import Path

import research_add_alpha as AA
import research_cycle as RC
import wave_manifest as WM
import wave_rules as WR
import wave_steps as WS
from wave_context import Wave
from wave_stage_util import (EXIT_PIN, MARGINAL_KEYS, ROW_KEYS, StageError, cyc, lib_spec, library_wave, lock_exes,
                             pin_exes, pinned, skipped, write_spec_file)


# ------------------------------------------------------------------ register (library waves)
def add_alpha(w: Wave, c: dict, name: str) -> list[str]:
    m = w.manifest
    return WS.add_alpha_argv(w.python, c, parent=m["parent"]["library"], parent_spec=m["parent"]["spec"], name=name,
                             fields_dir=m["fields"]["dir"], plan_out=w.wave_path("plans", name, f"{c['id']}.json"),
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
        return skipped("a rule wave registers no strings")
    m = w.manifest
    name, plans = m["library"], {}
    w.require_clean("register", add_alpha_files(w, name))
    for c in m["candidates"]:
        w.run(add_alpha(w, c, name), f"add-alpha {c['id']}")
        plans[c["id"]] = w.sha(w.wave_path("plans", name, f"{c['id']}.json"))
    if replaces(m["candidates"]):
        rewrite_spec(w, lib_spec(name), WS.pool_only_marginal, "marginal on the pool only (PM6-8 (i), PM7-32)")
    if m.get("marginal"):
        rewrite_spec(w, lib_spec(name), lambda d: WS.marginal_ruled(d, m["marginal"]),
                     f"marginal as ruled ({m['marginal']['ruling']}): {m['marginal']}")
    pin_exes(w, lib_spec(name))
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
    return [WS.fmt_argv(cyc(w,"run", lib_spec(w.manifest["library"]), "--screen")),
            f"#   then: the sign rule {w.manifest['sign_rule']} on every string's row of the fit's admission.json"]


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
        rewrite_spec(w, lib_spec(name), lambda d: WS.marginal_ruled(d, m["marginal"]),
                     f"marginal as ruled ({m['marginal']['ruling']}): {m['marginal']}")
    pin_exes(w, lib_spec(name))
    commit = w.commit_paths(f"wave {m['wave']}: cell library {name} = {m['parent']['library']} + {', '.join(kept)} "
                            f"({m['sign_rule']}: dropped {', '.join(sc['decision']['dropped'])})",
                            add_alpha_files(w, name))
    return dict(cell_out(w, done, lib_spec(name), "b-library", commit or w.committed(lib_spec(name)), name),
                marginal={"mode": want, "screen_mode": have, "reuse": reuse})


def spec_plan(w: Wave, done: dict) -> list[str]:
    m = w.manifest
    if not library_wave(w):
        rc = m["rule_cell"]
        cell = WS.rule_cell_path(rc["template"], rc, m["wave"])
        return [f"#   write {cell}: {rc['template']} with parent {m['parent']['spec']} and the constants",
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
    return pinned("screen", sc, {"admission_sha256": w.sha(sc["admission"])}) if sc.get("admission") else {}
