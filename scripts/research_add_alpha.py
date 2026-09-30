"""research_cycle.py add-alpha: one command from an idea to a locked cycle spec (platform v8 lane A, task A-2).

  research_cycle.py add-alpha --id X --dsl "..." --theme T --tier B --prior-sign 1 --citation "..." --origin prior
                              --parent v71 [--name v72] [--parent-spec SPEC] [--plan-json PATH] [--root R]
                              [--prior-sign-source S] [--form F] [--formula F] [--domain D] [--deviation D]

1. registers the alpha in atx-impl/strategies/alphas/registry.json (an identical registration of an existing id is
   reused, another definition of it refused; theme, tier and origin are checked against the registry);
2. defines library NAME (default: the parent's name with its trailing number + 1) = the parent's members + X in
   libraries/NAME.json (a second add-alpha into the same NAME and parent appends: one wave, several members) and builds
   its IC library <id>.json and slim recipe <id>.recipe.v2.json (generate_library.py);
3. validates through the exe's plan rows (contract K1): the parent spec's IC exe runs --plan-only on the new library,
   the role and the fields (metadata only), or --plan-json PATH gives a saved plan; every member needs its row and the
   new members must fit the house budget;
4. writes the pre-registration stub libraries/NAME.prereg.md (root registers it before any IC read);
5. derives scripts/specs/v8/lib-NAME.json from the parent's spec (--parent-spec, else scripts/specs/v8/lib-PARENT.json,
   else scripts/specs/PARENT.json) by name templates: every output name gets NAME for PARENT; the parent's cycle
   outputs become the reference inputs (admission, cell, combined signal, S2 daily CSV, orientations, daily IC); the
   parent's fields dir is pinned as built; the candidate cache and fit work dir are derived from the role; the gate lists
   the new members (require any); marginal IC on the parent's combined signal with the parent's composition weights as
   the theme regressors (inputs.reference_weights, the file the pool names); "receipts": "every-phase",
   "verdict": true, summ.dsr_n "ledger+1"; the OD-2 caps written into runner.phases when they apply;
6. locks it (every pin computed from its file). A missing input leaves the spec unlocked (exit 3: `lock --write` later).

Nothing is written when the registration, the library, the plan or the spec template fails (exit 2). A library that
already has cycle outputs is never extended (use a new --name).
"""
from __future__ import annotations

import argparse
import copy
import json
from pathlib import Path
import re
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parent))
import research_cycle as RC  # noqa: E402
import research_tree  # noqa: E402

sys.path.insert(0, str(research_tree.REPO / "atx-impl" / "strategies"))
import generate_library as G  # noqa: E402

STRATEGIES = "atx-impl/strategies"
SPECS_V8 = "scripts/specs/v8"
FIELD_BUILDER_INPUTS = ("identity_bridge", "fund_events", "sic_events", "reuse_fields") + RC.SEC_INPUTS + \
    RC.HOLDINGS_INPUTS                   # a child pins its parent's fields as built: nothing is rebuilt


class AddAlphaError(Exception):
    def __init__(self, message: str, code: int = RC.EXIT_USAGE):
        super().__init__(message)
        self.code = code


def next_name(parent: str) -> str:
    m = re.fullmatch(r"(.*?)(\d+)", parent)
    if not m:
        raise AddAlphaError(f"--name is needed: parent {parent!r} has no trailing number to increment")
    return f"{m[1]}{int(m[2]) + 1}"


def find_parent_spec(root: Path, parent: str, given: str | None) -> Path:
    candidates = [Path(given)] if given else [root / SPECS_V8 / f"lib-{parent}.json", root / "scripts" / "specs" /
                                              f"{parent}.json"]
    for p in candidates:
        p = p if p.is_absolute() else root / p
        if p.is_file():
            return p
    raise AddAlphaError(f"no parent spec: {', '.join(str(c) for c in candidates)} (pass --parent-spec)")


def parent_outputs(spec: dict, root: Path) -> dict:
    """The parent cycle's resolved outputs (out_root placement, the complete IC attempts) and its S2 CSV name."""
    c = RC.Cycle(spec, RC.Resolver(root), verify=False)
    f = spec["fields"]
    fd = f["output"] if RC.as_built(f) else c.placed(f["output"])
    ic = spec["ic"]
    u_base, w_base = c.out(ic["u_output"]), c.out(ic["w_output"])
    nav = c.out(spec["nav"]["output"])
    s2 = Path(spec["inputs"]["reference_daily"]["path"]).name if "reference_daily" in spec["inputs"] else None
    if s2 is None:
        scen = (c.res.read_json(f"{nav}/summary.json") or {}).get("primary_scenario")
        s2 = f"daily_{scen}.csv" if scen else None
    return {"fields": fd, "u": f"{u_base}-{c.ic_attempt('u', u_base)[0]}",
            "w": f"{w_base}-{c.ic_attempt('w', w_base)[0]}", "fit": c.out(spec["fit"]["output"]), "nav": nav, "s2": s2}


def derive_spec(parent: dict, parent_name: str, name: str, lib_rel: str, recipe_rel: str, parent_lib_rel: str,
                new_ids: list[str], outs: dict, root: Path) -> dict:
    """The child cycle spec: the parent's with every output renamed by the name template (see the module doc)."""
    ren = lambda text: G.rename(text, parent_name, name)  # noqa: E731
    s = copy.deepcopy(parent)
    s["name"] = name
    s["description"] = (f"Library {name} = {parent_name} + {', '.join(new_ids)} (research_cycle.py add-alpha; derived "
                        f"from the {parent_name} spec by name templates). Identity before the cell: the u pass "
                        "reproduces the parent's orientation and daily-IC member rows; ref is skipped when the fields "
                        "equal the parent's. `run --screen` reads the admission; `run` the cell.")
    fd, fdm = outs["fields"], f"{outs['fields']}/manifest.json"
    fields_sha = RC.Resolver(root).sha(fdm)
    if fields_sha is None:
        raise AddAlphaError(f"the parent's fields manifest {fdm} is missing: the child pins it as built")
    pin = lambda path, **extra: dict(extra, path=path, sha256=None)  # noqa: E731
    keep = {k: dict(v, sha256=None) for k, v in parent["inputs"].items() if k == "role"}
    s["inputs"] = {"library": pin(lib_rel), "recipe": pin(recipe_rel), "baseline_library": pin(parent_lib_rel),
                   **keep, "baseline_fields": pin(fdm, dir=fd),
                   "reference_admission": pin(f"{outs['fit']}/admission.json"),
                   "reference_cell": pin(f"{outs['nav']}/summary.json", dir=outs["nav"]),
                   "reference_combined": pin(f"{outs['w']}/train_combined.json"),
                   "reference_weights": pin(f"{outs['fit']}/composition_weights.json")}   # the pool's weights
    if outs["s2"]:
        s["inputs"]["reference_daily"] = pin(f"{outs['nav']}/{outs['s2']}")
    s["inputs"].update(reference_orientations=pin(f"{outs['u']}/orientations.json"),
                       reference_daily_ic=pin(f"{outs['u']}/train_daily_ic.csv"))
    assert not set(s["inputs"]) & set(FIELD_BUILDER_INPUTS)
    s["fields"] = {"output": fd, "manifest_sha256": fields_sha, "list": list(parent["fields"]["list"])}
    s.pop("static_check", None)                     # K1 validated the library at add time
    s.pop("compare", None)
    s["ic"] = {k: v for k, v in dict(parent["ic"], u_output=ren(parent["ic"]["u_output"]),
                                     w_output=ren(parent["ic"]["w_output"])).items() if k != "cache"}
    s["fit"] = {k: v for k, v in dict(parent["fit"], output=ren(parent["fit"]["output"])).items() if k != "work_dir"}
    for section in ("card", "monitor", "nav"):
        if section in parent:
            s[section] = dict(parent[section], output=ren(parent[section]["output"]))
    s["marginal"] = {"output": ren(parent["marginal"]["output"]) if "marginal" in parent else
                     f"{s['ic']['u_output']}-marginal", "pool": "reference_combined", "themes": "reference_weights"}
    s["gate"] = {"name": f"p1-{name}", "admitted": list(new_ids), "require": "any", "sign_agrees": True, "report": []}
    compare = [{"name": "parent-orientations", "after": "u", "mode": "json-rows", "array": "candidates", "key": "id",
                "keys": "{input:baseline_library}", "a": "{input:reference_orientations}",
                "b": "{out:u}/orientations.json"},
               {"name": "parent-train-daily-ic", "after": "u", "mode": "csv-rows", "key": "id",
                "keys": "{input:baseline_library}", "a": "{input:reference_daily_ic}",
                "b": "{out:u}/train_daily_ic.csv"}]
    s.pop("ref", None)
    if outs["s2"] and "nav" in s:                  # the reference construction on this cycle's fields (skipped when
        s["ref"] = {"output": f"{s['nav']['output']}-ref", "combined": "reference_combined"}   # they are the parent's)
        compare.insert(0, {"name": "ref-s2-daily", "after": "ref", "mode": "file", "a": "{input:reference_daily}",
                           "b": "{out:ref}/" + outs["s2"]})
    s["compare"] = compare
    if "summ" in s and s["summ"].get("ledger"):
        s["summ"] = {k: v for k, v in dict(s["summ"], dsr_n=RC.DSR_FROM_LEDGER, cells_from_ledger=True).items()
                     if k != "cells"}
    s["receipts"], s["verdict"] = "every-phase", True
    base = RC.Cycle(s, RC.Resolver(root), verify=False)
    caps = {p: base.phase_caps(p) for p in ("u", "w")}
    runner_caps = {k: s["runner"][k] for k in RC.CAP_KEYS}
    od2 = {p: {k: v for k, v in c.items() if runner_caps[k] != v} for p, c in caps.items()}
    if any(od2.values()) and not s["runner"].get("phases"):
        s["runner"] = dict(s["runner"], phases={p: c for p, c in od2.items() if c})   # the caps are spec data
    return s


def stub(name: str, lib: dict, parent: str, parent_lib_rel: str, new: list[dict], spec_rel: str) -> str:
    rows = "\n".join(f"| `{a['id']}` | {a['theme']} | {a['tier']} | {a['prior_sign']:+d} | {a['origin']} | `{a['dsl']}` |"
                     for a in new)
    notes = "\n".join(f"- `{a['id']}`: {a['citation']} (prior sign source: {a['prior_sign_source']}; form "
                      f"{a['form']}). Formula: {a['notes']['formula'] or 'n/a'}. Domain: {a['notes']['domain'] or 'n/a'}. "
                      f"Deviation: {a['notes']['deviation'] or 'n/a'}." for a in new)
    return (f"# Pre-registration stub: library {name} ({lib['id']}) = {parent} + {len(new)} new member(s)\n\n"
            "Written by `research_cycle.py add-alpha` (regenerated on every add into this library). Root registers "
            "it in the sprint pre-registration before any IC read of a new member. Nothing here was measured.\n\n"
            "| id | theme | tier | prior sign | origin | DSL |\n|---|---|---|---|---|---|\n"
            f"{rows}\n\n{notes}\n\n"
            f"Parent: {parent} (`{parent_lib_rel}`), {len(lib['members']) - len(new)} members unchanged. Trials: "
            f"{len(new)} admission trial(s); the cell's cross-cell N resolves from the ledger at scoring time "
            f"(summ.dsr_n \"ledger+1\"). Spec: `{spec_rel}`.\n\n"
            "Ruling: <decision> -- <why> -- <cost if wrong>   (root, before `run --screen`)\n")


def entry_of(a) -> dict:
    return {"id": a.id, "dsl": a.dsl, "theme": a.theme, "tier": a.tier, "prior_sign": a.prior_sign,
            "citation": a.citation, "prior_sign_source": a.prior_sign_source or a.citation,
            "form": a.form or "as in the DSL", "origin": a.origin,
            "notes": {"formula": a.formula, "domain": a.domain, "deviation": a.deviation}, "added_in": a.name}


def has_outputs(root: Path, spec_path: Path) -> bool:
    """Whether a derived spec's cycle has started (a u attempt or the fit output exists)."""
    spec = json.loads(spec_path.read_text(encoding="utf-8"))
    c = RC.Cycle(spec, RC.Resolver(root), verify=False)
    u = c.out(spec["ic"]["u_output"])
    return any(c.res.exists_dir(f"{u}-{k}") or c.res.exists_dir(f"{u}-run{k}") for k in range(1, RC.MAX_ATTEMPTS + 1)) \
        or c.res.exists_dir(c.out(spec["fit"]["output"]))


def plan_for(parent_spec: dict, root: Path, library_bytes: bytes, lib_id: str, outs: dict, plan_json: Path | None):
    """The K1 plan of the new library: a saved plan, else the parent spec's IC exe --plan-only (metadata only)."""
    if plan_json is not None:
        return G.load_json(plan_json if plan_json.is_absolute() else root / plan_json)
    c = RC.Cycle(parent_spec, RC.Resolver(root), verify=False)
    exe = Path(c.spec["exes"]["ic"])
    exe = exe if exe.is_absolute() else root / exe
    role = parent_spec["inputs"]["role"]["path"]
    res = RC.Resolver(root)
    fdm = f"{outs['fields']}/manifest.json"
    if res.sha(role) is None or res.sha(fdm) is None:
        raise AddAlphaError(f"K1 plan: the role {role} and the fields {fdm} must exist (or pass --plan-json)")
    with tempfile.TemporaryDirectory() as tmp:
        lib = Path(tmp) / f"{lib_id}.json"
        lib.write_bytes(library_bytes)
        return G.exe_plan(str(exe), lib, role, res.sha(role), outs["fields"], res.sha(fdm), cwd=root, env=c.env())


def add_alpha(a) -> int:
    root = Path(a.root).resolve()
    strategies = root / STRATEGIES
    name = a.name or next_name(a.parent)
    if name == a.parent:
        raise AddAlphaError("--name must differ from --parent")
    spec_path = root / SPECS_V8 / f"lib-{name}.json"
    if spec_path.is_file() and has_outputs(root, spec_path):
        raise AddAlphaError(f"library {name} already has cycle outputs ({spec_path}); add to a new --name")
    reg = G.load_registry(strategies)
    entry = entry_of(argparse.Namespace(**dict(vars(a), name=name)))
    created = G.register_alpha(reg, entry)
    prereg_rel = f"{STRATEGIES}/libraries/{name}.prereg.md"
    lib = G.child_library(strategies, a.parent, name, [a.id],
                          f"{prereg_rel} (add-alpha stub; root registers it in the sprint pre-registration before any "
                          "IC read)")
    G.validate_library(lib, name)
    doc = G.build_library(reg, lib)
    library_bytes = G.encode(doc)
    parent_file, parent_members = G.parent_library(strategies, a.parent)
    new_ids = [m for m in lib["members"] if m not in set(parent_members)]
    parent_spec_path = find_parent_spec(root, a.parent, a.parent_spec)
    parent_spec = RC.load_spec(parent_spec_path)
    outs = parent_outputs(parent_spec, root)
    plan = plan_for(parent_spec, root, library_bytes, lib["id"], outs, a.plan_json)
    problems = G.validate_plan(reg, lib, doc, library_bytes, plan, budget_ids=set(new_ids))
    if problems:
        raise AddAlphaError("K1 static validation failed: " + "; ".join(problems))
    lib_name, recipe_name = G.output_names(lib)
    spec = derive_spec(parent_spec, a.parent, name, f"{STRATEGIES}/{lib_name}", f"{STRATEGIES}/{recipe_name}",
                       f"{STRATEGIES}/{parent_file}", new_ids, outs, root)
    RC.validate_spec(spec)
    # every check passed: write
    (strategies / G.REGISTRY_PATH).write_bytes(G.encode_data(reg))
    G.library_file(strategies, name).parent.mkdir(parents=True, exist_ok=True)
    G.library_file(strategies, name).write_bytes(G.encode_data(lib))
    (strategies / lib_name).write_bytes(library_bytes)
    (strategies / recipe_name).write_bytes(G.encode(G.build_recipe(reg, lib, library_bytes, strategies)))
    alphas = {x["id"]: x for x in reg["alphas"]}
    spec_rel = f"{SPECS_V8}/lib-{name}.json"
    (root / prereg_rel).write_text(stub(name, lib, a.parent, f"{STRATEGIES}/{parent_file}", [alphas[i] for i in new_ids],
                                        spec_rel), encoding="utf-8", newline="\n")
    spec_path.parent.mkdir(parents=True, exist_ok=True)
    spec_path.write_text(json.dumps(spec, indent=2) + "\n", encoding="utf-8", newline="\n")
    print(f"add-alpha {a.id}: {'registered' if created else 'already registered (identical)'} in "
          f"{STRATEGIES}/{G.REGISTRY_PATH}")
    print(f"library {name} ({lib['id']}): {len(lib['members'])} members = {a.parent} + {new_ids}; "
          f"{STRATEGIES}/{lib_name} sha256 {G.sha256(library_bytes)}; K1 plan: {len(G.plan_rows(plan))} rows valid")
    print(f"prereg stub {prereg_rel}; spec {spec_rel} (from {parent_spec_path.as_posix()})")
    try:
        locked, notes = RC.lock(spec_path, root)
    except RC.CycleError as exc:
        print(f"UNLOCKED: {exc} -- run `research_cycle.py lock {spec_rel} --write` once it exists", file=sys.stderr)
        return RC.EXIT_PIN
    spec_path.write_text(json.dumps(locked, indent=2) + "\n", encoding="utf-8", newline="\n")
    print(f"locked {len(notes)} pins; next: research_cycle.py run {spec_rel} --screen (commit first)")
    return RC.EXIT_OK


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(prog="research_cycle.py add-alpha", description=__doc__.split("\n", 1)[0],
                                 epilog=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--id", required=True)
    ap.add_argument("--dsl", required=True)
    ap.add_argument("--theme", required=True)
    ap.add_argument("--tier", required=True)
    ap.add_argument("--prior-sign", type=int, required=True)
    ap.add_argument("--citation", required=True)
    ap.add_argument("--origin", required=True, help="prior | grid | mined (contract K5)")
    ap.add_argument("--parent", required=True, help="parent library name (libraries/PARENT.json or a legacy one)")
    ap.add_argument("--name", default=None, help="the new library's name (default: the parent's number + 1)")
    ap.add_argument("--parent-spec", default=None, help="the parent's cycle spec (default: by name)")
    ap.add_argument("--plan-json", type=Path, default=None, help="a saved --plan-only JSON (default: run the exe)")
    ap.add_argument("--prior-sign-source", default=None, help="default: the citation")
    ap.add_argument("--form", default=None)
    ap.add_argument("--formula", default=None)
    ap.add_argument("--domain", default=None)
    ap.add_argument("--deviation", default=None)
    ap.add_argument("--root", type=Path, default=research_tree.REPO)
    a = ap.parse_args(argv)
    try:
        return add_alpha(a)
    except (AddAlphaError, RC.CycleError) as exc:
        print(f"research_cycle add-alpha: {exc}", file=sys.stderr)
        return exc.code
    except G.LibraryError as exc:
        print(f"research_cycle add-alpha: {exc}", file=sys.stderr)
        return RC.EXIT_USAGE
