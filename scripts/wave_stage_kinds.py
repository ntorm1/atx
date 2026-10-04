"""The wave stages' part of the v2 kinds (wave_kinds.py, contract K-P9-9, P9 lane E2): the exes' rule tables read at
preflight, the field and role builds of the register stage, and the rule cell's derived constants of the spec stage.
A v1 manifest never reaches a function here that changes anything (``kind`` is None): its argv, receipts and bytes
are those of before.

  preflight  kind rule: both rule tables (the IC exe's --list-rules --json, the NAV exe's nav --list-rules --json),
             their stdout SHA-256 recorded; every named rule validated and its flags derived (wave_kinds.rule_flags);
             neither the template's change.flags nor rule_cell.constants.flags may set a rule's flag, named or not
             (F-8, review E2 R2: wave_kinds.raw_rule_flags). Kind leverage: the value inside the NAV exe's
             fixed-v1 aim_leverage range when the table lists it. Kind walk-forward: refused (schema only, lane D3)
  register   kind field: the fields builder on the parent's role (prepare_research_fields.py --registry R --fields
             <the wave's fields + the rows> --reuse <the wave's fields dir>) into field.output under the bounded
             runner; kind role: the role builder (prepare_recent_research.py ... --out role.output) then the fields
             builder on the new role (the wave's field names) into role.fields_output
  spec       the rule cell's constants: rule -> the derived flags; leverage -> nav.leverage and the renamed NAV
             output (a NAV-only template next to the parent spec when the manifest has no rule_cell); horizon -> its
             flags and every output renamed -h<days>; role -> inputs.role and the fields dir of the build
"""
from __future__ import annotations

import hashlib
import time

import research_cycle as RC
from research_common import free_run_dir
import research_spec
import research_tree
from wave_context import Wave
import wave_kinds as WK
from wave_stage_util import EXIT_PIN, StageError
import wave_steps as WS

import research_window as RW   # atx-engine/tools (wave_context puts it on the path)


def kind(w: Wave) -> str | None:
    """The manifest's v2 kind; None for a v1 manifest."""
    return w.manifest.get("kind") if WK.is_v2(w.manifest) else None


# ------------------------------------------------------------------ preflight: the exes' rule tables
def run_listing(w: Wave, spec: dict, nav: bool) -> tuple[dict | None, str | None]:
    """(the envelope, the SHA-256 of the stdout) of the parent spec's IC or NAV exe's rule listing; (None, None)
    when the exe is not named, fails or prints no envelope of its schema."""
    exe = RC.effective_exes(spec).get("nav" if nav else "ic")
    if not exe:
        return None, None
    argv = WK.listing_argv(w.path(exe).as_posix(), nav)
    env = RC.Cycle(spec, RC.Resolver(w.root), verify=False).env()      # the exe's DLL dirs (env_path_prepend)
    what = f"{'nav' if nav else 'ic'} rule table"
    started = time.monotonic()
    done = w.executor(argv, w.root, env)
    w.note_process(what, time.monotonic() - started, done.returncode)
    w.console(argv, what, done)
    text = done.stdout or ""
    doc = WK.parse_listing(text, WK.NAV_RULES if nav else WK.IC_RULES) if done.returncode == 0 else None
    return doc, (hashlib.sha256(text.encode("utf-8")).hexdigest() if doc is not None else None)


def preflight_kind(w: Wave, spec: dict) -> tuple[dict, list[str]]:
    """({kind, tables?, flags?, leverage?}, problems) of a v2 manifest on its parent's resolved ``spec``; ({}, []) for
    v1."""
    k, m = kind(w), w.manifest
    if k is None:
        return {}, []
    out, problems = {"kind": k}, []
    if k in WK.SCHEMA_ONLY:
        problems.append(f"kind {k} is schema only until lane D3 wires it: the wave plans, it does not run")
    if k in ("rule", "leverage"):
        tables, shas = {}, {}
        for table, nav in (("ic", False), ("nav", True)):
            if k == "rule" or nav:
                tables[table], shas[table] = run_listing(w, spec, nav)
        out["tables"] = shas
        if k == "rule":
            flags, more = WK.rule_flags(m["rules"], tables)
            problems += more
            template = w.read_json(m["rule_cell"]["template"]) if w.exists(m["rule_cell"]["template"]) else None
            given = {"rule_cell.template change.flags": ((template or {}).get("change") or {}).get("flags"),
                     "rule_cell.constants.flags": (m["rule_cell"].get("constants") or {}).get("flags")}
            for where, raw in given.items():        # review E2 R2: the named rules' flags and every table rule's
                owned = WK.raw_rule_flags(raw or {}, flags, tables)
                if owned:
                    problems.append(f"{where} sets {owned}, which the exes' rules own (review F-8: name the rule in "
                                    "rules, never write its flags; its params are checked against its params_schema)")
            out["flags"] = flags
        else:
            bounds, value = WK.leverage_bounds(tables), float(m["leverage"]["value"])
            if bounds is not None and not bounds[0] <= value <= bounds[1]:
                problems.append(f"leverage.value {m['leverage']['value']} lies outside the NAV exe's fixed-v1 "
                                f"aim_leverage range {list(bounds)}")
            out["leverage"] = {"value": m["leverage"]["value"], "bounds": list(bounds) if bounds else None}
    return out, problems


def preflight_kind_plan(w: Wave) -> list[str]:
    k, m = kind(w), w.manifest
    if k is None:
        return []
    lines = [f"#   kind {k} (contract K-P9-9)"]
    if k in WK.SCHEMA_ONLY:
        lines.append(f"#   kind {k}: schema only until lane D3 (preflight refuses to run it): {m[WK.BLOCKS[k]]}")
    if k in ("rule", "leverage"):
        spec = w.load_spec(m["parent"]["spec"])
        exes = RC.effective_exes(spec)
        for table, nav in (("ic", False), ("nav", True)):
            if (k == "rule" or nav) and exes.get(table):
                lines.append(WS.fmt_argv(WK.listing_argv(w.path(exes[table]).as_posix(), nav)))
        lines.append(f"#   then: {'rules ' + str(m['rules']) if k == 'rule' else 'leverage ' + str(m['leverage'])} "
                     "validated against those tables (params_schema; flags derived)")
    return lines


# ------------------------------------------------------------------ register: the builds
BUILDS = ("fields", "role", "role-fields")   # each build's run dirs: <out_dir>/builds/<what>-run<k> (run_build)


def build_bases(w: Wave) -> list[str]:
    """The run-dir bases of every build a v2 wave can run (BUILDS; Wave.run_dirs lists their -run<k> dirs): the seal
    scan reads their logs (review E2 R1: a build reads real data)."""
    return [w.wave_path("builds", what) for what in BUILDS]


def build_caps(block: dict) -> dict:
    return {"seconds": block.get("seconds", WK.BUILD_CAPS["seconds"]),
            "max_rss_mib": block.get("max_rss_mib", WK.BUILD_CAPS["max_rss_mib"])}


def bounded(w: Wave, run_dir: str, binds: list[str], command: list[str], caps: dict) -> list[str]:
    """``command`` under the bounded runner with a build's caps (the readers' memory floor and launch options)."""
    argv = [w.python, WS.tool(WS.RUNNER, w.root), "--seconds", str(caps["seconds"]), "--max-rss-mib",
            str(caps["max_rss_mib"]), "--min-free-mib", WS.READER_CAPS["min_free_mib"],
            *WS.runner_launch_flags(w.manifest.get("driver")), "--output", run_dir]
    for b in binds:
        argv += ["--bind", b]
    return argv + ["--", *command]


def fields_names(w: Wave) -> list[str]:
    """The field names of the wave's fields dir (its manifest's rows, in order)."""
    doc = w.read_json(f"{w.manifest['fields']['dir']}/manifest.json") or {}
    return [row["name"] for row in doc.get("fields", []) if isinstance(row, dict) and isinstance(row.get("name"), str)]


def parent_role(w: Wave) -> tuple[str, str, str]:
    """(dir, manifest path, SHA-256 pin) of the parent cell's role (inputs.role)."""
    spec = w.load_spec(w.manifest["parent"]["spec"])
    role = spec["inputs"]["role"]
    return RC.input_dir(role), role["path"], role.get("sha256") or w.sha(role["path"])


def fields_command(w: Wave, registry: str, names: list[str], role: tuple[str, str], out: str,
                   reuse: tuple[str, str] | None, extra: list[str]) -> list[str]:
    cmd = [w.python, WS.tool(WK.FIELD_BUILDER, w.root), "--registry", registry, "--fields", ",".join(names),
           "--role", role[0], "--role-sha256", role[1]]
    if reuse is not None:
        cmd += ["--reuse", reuse[0], "--reuse-sha256", reuse[1]]
    return cmd + [*extra, "--output", out]


def built(w: Wave, out: str, what: str, names: list[str] | None = None) -> dict:
    """{dir, manifest_sha256, rows} of a build's output dir: its manifest complete, sealed no later than the
    research seal, and (fields) holding every name asked for; else StageError (exit 3)."""
    rel = f"{out}/manifest.json"
    doc = w.read_json(rel)
    if not isinstance(doc, dict):
        raise StageError(f"build {what}: no manifest {rel}", EXIT_PIN)
    end = (doc.get("seal") or {}).get("exclusive_end")
    if names is not None:
        if doc.get("status") != "complete" or not isinstance(end, str) or end > RW.SEAL_DATE:
            raise StageError(f"build {what}: {rel} is not complete or not sealed by {RW.SEAL_DATE} (seal {end!r})",
                             EXIT_PIN)
        rows = [r.get("name") for r in doc.get("fields", []) if isinstance(r, dict)]
        missing = [x for x in names if x not in rows]
        if missing:
            raise StageError(f"build {what}: {rel} lacks {missing}", EXIT_PIN)
    return {"dir": out, "manifest_sha256": w.sha(rel)}


def run_build(w: Wave, what: str, out: str, binds: list[str], command: list[str], caps: dict) -> str | None:
    """Run one build under the bounded runner. ``what`` is one of BUILDS (its run dirs are what the seal scan reads).
    An output whose manifest exists is reused (a resumed stage) only when this wave's last <what> run completed it
    with this command (its receipt's argv_sha256, K-P9-10); any other existing output is refused, never shared
    (review E2 S6: another manifest's build, or one made with other rows / pins)."""
    if what not in BUILDS:
        raise StageError(f"build {what}: not one of {BUILDS} (the seal scan would not read its logs)", EXIT_PIN)
    if w.exists(f"{out}/manifest.json"):
        runs = w.run_dirs(w.wave_path("builds", what))
        r = w.read_json(f"{runs[-1]}/receipt.json") if runs else None
        want = research_tree.argv_sha256(command[1:])
        if not (isinstance(r, dict) and r.get("outcome") == "completed" and r.get("exit_code") == 0 and
                r.get("argv_sha256") == want):
            raise StageError(f"build {what}: {out}/manifest.json exists but this wave's last {what} run "
                             f"({runs[-1] if runs else 'none'}) did not complete it with this command (argv sha256 "
                             f"{want}): name a new output, or move that one aside", EXIT_PIN)
        w.log(f"   build {what}: {out}/manifest.json exists, built by {runs[-1]} (resumed)")
        return None
    run_dir = free_run_dir(w.wave_path("builds", what), w.exists)
    w.run(bounded(w, run_dir, binds, command, caps), f"build {what}")
    return run_dir


def build_field(w: Wave) -> dict:
    f, base = w.manifest["field"], w.manifest["fields"]
    names = fields_names(w) + [r for r in f["rows"] if r not in fields_names(w)]
    rdir, rpath, rsha = parent_role(w)
    registry = f.get("registry", WK.FIELD_REGISTRY)
    cmd = fields_command(w, registry, names, (rdir, rsha), f["output"], (base["dir"], base["manifest_sha256"]),
                         list(f.get("builder_args", [])))
    run_dir = run_build(w, "fields", f["output"], [cmd[1], registry, rpath, f"{base['dir']}/manifest.json"], cmd,
                        build_caps(f))
    return {"fields": dict(built(w, f["output"], "fields", names), run_dir=run_dir, rows=list(f["rows"]))}


def build_role(w: Wave) -> dict:
    r = w.manifest["role"]
    cmd = [w.python, WS.tool(WK.ROLE_BUILDER, w.root), *r["builder_args"], "--out", r["output"]]
    role_run = run_build(w, "role", r["output"], [cmd[1]], cmd, build_caps(r))
    role = built(w, r["output"], "role")
    names, registry = fields_names(w), r.get("registry", WK.FIELD_REGISTRY)
    fcmd = fields_command(w, registry, names, (r["output"], role["manifest_sha256"]), r["fields_output"], None,
                          list(r.get("fields_args", [])))
    fields_run = run_build(w, "role-fields", r["fields_output"], [fcmd[1], registry, f"{r['output']}/manifest.json"],
                           fcmd, build_caps(r))
    return {"role": dict(role, run_dir=role_run),
            "fields": dict(built(w, r["fields_output"], "role-fields", names), run_dir=fields_run)}


def build(w: Wave) -> dict:
    """The register stage's build of a field or role wave ({} for every other kind)."""
    k = kind(w)
    return build_field(w) if k == "field" else build_role(w) if k == "role" else {}


def build_plan(w: Wave) -> list[str]:
    k, m = kind(w), w.manifest
    if k == "field":
        f, base = m["field"], m["fields"]
        rdir, rpath, rsha = parent_role(w)
        names = fields_names(w) + [r for r in f["rows"] if r not in fields_names(w)]
        cmd = fields_command(w, f.get("registry", WK.FIELD_REGISTRY), names, (rdir, rsha), f["output"],
                             (base["dir"], base["manifest_sha256"]), list(f.get("builder_args", [])))
        return [WS.fmt_argv(bounded(w, w.wave_path("builds", "fields-run1"), [], cmd, build_caps(f))),
                f"#   then: the strings are registered on {f['output']} (its manifest pinned by the build)"]
    if k == "role":
        r = m["role"]
        cmd = [w.python, WS.tool(WK.ROLE_BUILDER, w.root), *r["builder_args"], "--out", r["output"]]
        fcmd = fields_command(w, r.get("registry", WK.FIELD_REGISTRY), fields_names(w),
                              (r["output"], f"<sha256:{r['output']}/manifest.json>"), r["fields_output"], None,
                              list(r.get("fields_args", [])))
        return [WS.fmt_argv(bounded(w, w.wave_path("builds", "role-run1"), [], cmd, build_caps(r))),
                WS.fmt_argv(bounded(w, w.wave_path("builds", "role-fields-run1"), [], fcmd, build_caps(r)))]
    return []


def fields_dir(w: Wave, done: dict) -> str:
    """The fields dir the wave's strings are registered on: a field wave's build output, else the manifest's."""
    if kind(w) == "field":
        return w.manifest["field"]["output"]
    return w.manifest["fields"]["dir"]


# ------------------------------------------------------------------ spec: the rule cell
def base_rule_cell(w: Wave) -> tuple[dict, dict | None, str]:
    """(rule_cell, its template document, the template path) as the manifest gives them; a leverage wave without a
    rule_cell gets the NAV-only template (wave_kinds.leverage_template; never written, only the cell is)."""
    m = w.manifest
    if m.get("rule_cell") is None:
        path, doc = WK.leverage_template(m["parent"]["spec"], m["wave"], m["leverage"]["value"],
                                         m["leverage"].get("name"))
        return {"template": path, "name": doc["name"]}, doc, path
    rc = m["rule_cell"]
    return rc, w.read_json(rc["template"]), rc["template"]


def rule_cell(w: Wave, done: dict) -> tuple[dict, dict | None, str]:
    """``base_rule_cell`` with the kind's derived constants (unchanged for v1)."""
    m, k = w.manifest, kind(w)
    rc, doc, path = base_rule_cell(w)
    if k is None:
        return rc, doc, path
    parent = w.load_spec(m["parent"]["spec"])
    sets, flags = {}, {}
    if k == "rule":
        flags = ((done.get("preflight") or {}).get("kind") or {}).get("flags")
        if flags is None:
            raise StageError("spec: the preflight receipt has no derived rule flags (run preflight again)", EXIT_PIN)
    elif k == "leverage":
        sets = WK.leverage_set(parent, m["leverage"]["value"])
    elif k == "horizon":
        sets, flags = WK.horizon_set(parent, m["horizon"]["days"]), m["horizon"]["flags"]
    return WK.merged_constants(rc, sets, flags), doc, path


def role_change(doc: dict, done: dict) -> dict:
    """A role wave's cell document with inputs.role and the fields dir of the register stage's build."""
    b = (done.get("register") or {}).get("build") or {}
    role, fields = b.get("role"), b.get("fields")
    if not role or not fields:
        raise StageError("spec: the register receipt has no role build (run register again)", EXIT_PIN)
    change = doc.setdefault("change", {})
    change.setdefault("inputs", {})["role"] = {"dir": role["dir"], "path": f"{role['dir']}/manifest.json",
                                               "sha256": role["manifest_sha256"]}
    change["set"] = dict(change.get("set") or {}, **{"fields.output": fields["dir"],
                                                      "fields.manifest_sha256": fields["manifest_sha256"]})
    return doc


def cell_doc(w: Wave, done: dict, cell: str) -> dict:
    """The rule cell's file document (wave_steps.rule_cell_doc on the kind's rule_cell; a role wave's build set)."""
    rc, template, tpath = rule_cell(w, done)
    doc = WS.rule_cell_doc(template, tpath, w.manifest["parent"]["spec"], rc, w.manifest["wave"])
    if kind(w) == "role":
        doc = role_change(doc, done)
    if not research_spec.is_template(doc):
        raise StageError(f"rule cell {cell}: not a cell template", EXIT_PIN)
    return doc


def cell_path(w: Wave) -> str:
    """The rule cell's file (wave_steps.rule_cell_path: next to its template, named rule_cell.name or
    <template stem>-<wave>)."""
    rc, _, tpath = base_rule_cell(w)
    return WS.rule_cell_path(tpath, rc, w.manifest["wave"])
