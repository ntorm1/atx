"""research_cycle.py add-alpha: one command from an idea to a locked cycle spec (platform v8 lane A, tasks A-2, A2).

  research_cycle.py add-alpha --id X --dsl "..." --theme T --tier B --prior-sign 1 --citation "..." --origin prior
                              --parent v71 [--name v72] [--parent-spec SPEC] [--plan-json PATH] [--root R]
                              [--removes ID ... | --replaces ID ... [--rescreen]]
                              [--exception LIMIT=N ... --exception-basis TEXT] [--fields DIR]
                              [--prior-sign-source S] [--form F] [--formula F] [--domain D] [--deviation D]

1. registers the alpha in atx-impl/strategies/alphas/registry.json (an identical registration of an existing id is
   reused, another definition of it refused; theme, tier and origin are checked against the registry);
2. defines library NAME (default: the parent's name with its trailing number + 1) = the parent's members + X in
   libraries/NAME.json (a second add-alpha into the same NAME and parent appends: one wave, several members) and builds
   its IC library <id>.json and slim recipe <id>.recipe.v2.json (generate_library.py). --removes ID (repeatable): X is
   appended and the IDs leave the library with their exceptions (library-v8-draft E3: earn_surprise_comp, sue, droe,
   chtax). --replaces ID (repeatable) puts X in place of the first ID (its roster position); the other IDs leave; X
   inherits their budget exceptions (E4: q5_eg_f49 takes q5_eg's). --rescreen (with one --replaces) marks X a re-screen
   of that member's hypothesis (Ruling R2-e: 0 admission trials; listed in the gate's report rows, not its admitted
   rows). --exception max_extra_fields=8 --exception-basis "Ruling R2-b" records X's budget exception;
3. validates through the exe's plan rows (contract K1): the parent spec's IC exe runs --plan-only on the new library,
   the role and the fields (metadata only), or --plan-json PATH gives a saved plan; every member needs its row and the
   new members must fit the house budget or their recorded exception;
4. writes the pre-registration stub libraries/NAME.prereg.md (root registers it before any IC read);
5. derives scripts/specs/v8/lib-NAME.json from the parent's spec (--parent-spec, else scripts/specs/v8/lib-PARENT.json,
   else scripts/specs/PARENT.json; a v8 base spec or a template, whose parent must be set and whose chain lists no open
   precondition) by the name rule (derive_name): every token PARENT of an output name becomes NAME, a name without the
   token gets "-NAME" appended; the parent's cycle outputs become the reference inputs (admission, cell, combined signal,
   S2 daily CSV, orientations, daily IC, composition weights); every other parent input that no fields build reads is
   kept (the role; a NAV label role, with the parent's own pin: review F-8, the child's nav and ref mark their books
   with the label role the parent's NAV was marked with, so the ref identity against the parent's labelled S2 daily
   CSV is on equal footing); the parent's fields dir is pinned as built, or --fields DIR (an as-built dir, e.g.
   fields v9 + grp_ff12f49 built with --reuse from v9; sticky: a later add into the same wave keeps it) with the
   parent's as the baseline of the ref identity; a parent on the shared stores (fit.work_dir = <out base>/fit-work, C-1) passes its candidate cache and fit store base on, a parent
   on per-version stores gets them derived from the role; the gate lists the new members that are admission trials
   (require any) and reports the re-screens; the parent identity compares after u (restricted to the members both
   libraries hold when members were replaced); marginal IC on the parent's combined signal with the parent's
   composition weights as the theme regressors (inputs.reference_weights); "receipts": "every-phase", "verdict": true,
   summ.dsr_n "ledger+1", summ.origin = the new members' origin class (the most searched of them; review C-2: the cell
   is scored under nav_summ --protocol v8, the parent's summ.extra kept without its --origin); the OD-2 caps written
   into runner.phases when they apply;
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
import research_spec  # noqa: E402
import research_tree  # noqa: E402

sys.path.insert(0, str(research_tree.REPO / "atx-impl" / "strategies"))
import generate_library as G  # noqa: E402

STRATEGIES = "atx-impl/strategies"
SPECS_V8 = "scripts/specs/v8"
FIELD_BUILDER_INPUTS = ("identity_bridge", "fund_events", "sic_events", "reuse_fields") + RC.SEC_INPUTS + \
    RC.HOLDINGS_INPUTS                   # a child pins its parent's fields as built: nothing is rebuilt
REBUILT_INPUTS = ("library", "recipe", "baseline_library", "baseline_fields")   # + every reference_* input
SHARED_FIT_STORE = "fit-work"           # C-1: the fitter / card / monitor store base <out base>/fit-work
PARENT_PINNED = ("label_role",)         # review F-8: kept with the parent's pin (a changed file stops `lock`, exit 3)


class AddAlphaError(Exception):
    def __init__(self, message: str, code: int = RC.EXIT_USAGE):
        super().__init__(message)
        self.code = code


def next_name(parent: str) -> str:
    m = re.fullmatch(r"(.*?)(\d+)", parent)
    if not m:
        raise AddAlphaError(f"--name is needed: parent {parent!r} has no trailing number to increment")
    return f"{m[1]}{int(m[2]) + 1}"


def derive_name(text: str, parent: str, name: str) -> str:
    """A child output name: every token `parent` becomes `name` (generate_library.rename, the v7 specs' convention); a
    name without the token (a v8 cell spec names its cell, not its library) gets "-name" appended."""
    try:
        return G.rename(text, parent, name)
    except G.LibraryError:
        return f"{text}-{name}"


def shared_stores(spec: dict) -> bool:
    """Whether a parent spec runs on the shared stores (C-1: fit.work_dir is the store base <out base>/fit-work)."""
    base = (spec.get("out_root") or RC.DEFAULT_OUT_ROOT).rstrip("/")
    return spec.get("fit", {}).get("work_dir") == f"{base}/{SHARED_FIT_STORE}"


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


def wave_origin(origins: list[str]) -> str:
    """The origin class of a library wave's cell (contract K5): its members' class, or the most searched of their
    classes (mined over grid over prior) when they differ."""
    return max(origins, key=G.ORIGINS.index)


def derive_spec(parent: dict, parent_name: str, name: str, lib_rel: str, recipe_rel: str, parent_lib_rel: str,
                lib: dict, parent_members: list[str], outs: dict, root: Path, fields_dir: str | None = None,
                origin: str = "prior") -> dict:
    """The child cycle spec: the parent's with every output renamed by the name rule (see the module doc).
    ``origin`` is the cell's origin class (summ.origin, review C-2: nav_summ --origin of its ledger line)."""
    ren = lambda text: derive_name(text, parent_name, name)  # noqa: E731
    new_ids = [m for m in lib["members"] if m not in set(parent_members)]
    rescreens = [m for m in lib.get("rescreens", []) if m in new_ids]
    trials = [m for m in new_ids if m not in rescreens]
    removed = [m for m in parent_members if m not in set(lib["members"])]
    s = copy.deepcopy(parent)
    s["name"] = name
    s["description"] = (f"Library {name} = {parent_name} + {', '.join(new_ids)}"
                        + (f" - {', '.join(removed)}" if removed else "")
                        + (f" (re-screens: {', '.join(rescreens)})" if rescreens else "")
                        + f" (research_cycle.py add-alpha; derived from the {parent_name} spec by the name rule). "
                        "Identity before the cell: the u pass reproduces the parent's orientation and daily-IC member "
                        "rows; ref is skipped when the fields equal the parent's. `run --screen` reads the admission; "
                        "`run` the cell.")
    base_fields = outs["fields"]
    fd = fields_dir.rstrip("/") if fields_dir else base_fields
    fdm = f"{fd}/manifest.json"
    res = RC.Resolver(root)
    fields_sha = res.sha(fdm)
    if fields_sha is None:
        raise AddAlphaError(f"the fields manifest {fdm} is missing: the child pins it as built")
    pin = lambda path, **extra: dict(extra, path=path, sha256=None)  # noqa: E731
    keep = {k: dict(v, sha256=v.get("sha256") if k in PARENT_PINNED else None) for k, v in parent["inputs"].items()
            if k not in REBUILT_INPUTS and not k.startswith("reference_") and k not in FIELD_BUILDER_INPUTS}
    s["inputs"] = {"library": pin(lib_rel), "recipe": pin(recipe_rel), "baseline_library": pin(parent_lib_rel),
                   **keep, "baseline_fields": pin(f"{base_fields}/manifest.json", dir=base_fields),
                   "reference_admission": pin(f"{outs['fit']}/admission.json"),
                   "reference_cell": pin(f"{outs['nav']}/summary.json", dir=outs["nav"]),
                   "reference_combined": pin(f"{outs['w']}/train_combined.json"),
                   "reference_weights": pin(f"{outs['fit']}/composition_weights.json")}   # the pool's weights
    if outs["s2"]:
        s["inputs"]["reference_daily"] = pin(f"{outs['nav']}/{outs['s2']}")
    s["inputs"].update(reference_orientations=pin(f"{outs['u']}/orientations.json"),
                       reference_daily_ic=pin(f"{outs['u']}/train_daily_ic.csv"))
    assert not set(s["inputs"]) & set(FIELD_BUILDER_INPUTS)
    if fd != base_fields:
        names = [row.get("name") for row in (res.read_json(fdm) or {}).get("fields", [])]
        s["fields"] = {"output": fd, "manifest_sha256": fields_sha, "list": names}
    else:
        s["fields"] = {"output": fd, "manifest_sha256": fields_sha, "list": list(parent["fields"]["list"])}
    s.pop("static_check", None)                     # K1 validated the library at add time
    s.pop("compare", None)
    stores = shared_stores(parent)                   # C-1 shared stores pass on; per-version v7 stores are derived
    s["ic"] = {k: v for k, v in dict(parent["ic"], u_output=ren(parent["ic"]["u_output"]),
                                     w_output=ren(parent["ic"]["w_output"])).items() if k != "cache" or stores}
    s["fit"] = {k: v for k, v in dict(parent["fit"], output=ren(parent["fit"]["output"])).items()
                if k != "work_dir" or stores}
    for section in ("card", "monitor", "nav"):
        if section in parent:
            s[section] = dict(parent[section], output=ren(parent[section]["output"]))
    s["marginal"] = {"output": ren(parent["marginal"]["output"]) if "marginal" in parent else
                     f"{s['ic']['u_output']}-marginal", "pool": "reference_combined", "themes": "reference_weights"}
    s["gate"] = {"name": f"p1-{name}", "admitted": trials or list(rescreens), "require": "any", "sign_agrees": True,
                 "report": list(rescreens) if trials else []}
    compare = [{"name": "parent-orientations", "after": "u", "mode": "json-rows", "array": "candidates", "key": "id",
                "keys": "{input:baseline_library}", "a": "{input:reference_orientations}",
                "b": "{out:u}/orientations.json"},
               {"name": "parent-train-daily-ic", "after": "u", "mode": "csv-rows", "key": "id",
                "keys": "{input:baseline_library}", "a": "{input:reference_daily_ic}",
                "b": "{out:u}/train_daily_ic.csv"}]
    if removed:                                      # replaced parent members: compare the members both libraries hold
        for c in compare:
            c["keys_in"] = "{input:library}"
    s.pop("ref", None)
    if outs["s2"] and "nav" in s:                  # the reference construction on this cycle's fields (skipped when
        s["ref"] = {"output": f"{s['nav']['output']}-ref", "combined": "reference_combined"}   # they are the parent's)
        compare.insert(0, {"name": "ref-s2-daily", "after": "ref", "mode": "file", "a": "{input:reference_daily}",
                           "b": "{out:ref}/" + outs["s2"]})
    s["compare"] = compare
    if "summ" in s and s["summ"].get("ledger"):
        s["summ"] = {k: v for k, v in dict(s["summ"], dsr_n=RC.DSR_FROM_LEDGER, cells_from_ledger=True).items()
                     if k != "cells"}
    if "summ" in s:   # review C-2: the cell's K5 class; the parent's extra (its --protocol v8, seed, draws) is kept
        extra = list(s["summ"].get("extra", []))
        if "--origin" in extra:
            k = extra.index("--origin")
            del extra[k:k + 2]
        s["summ"] = dict(s["summ"], extra=extra, origin=origin)
    s["receipts"], s["verdict"] = "every-phase", True
    base = RC.Cycle(s, RC.Resolver(root), verify=False)
    caps = {p: base.phase_caps(p) for p in ("u", "w")}
    runner_caps = {k: s["runner"][k] for k in RC.CAP_KEYS}
    od2 = {p: {k: v for k, v in c.items() if runner_caps[k] != v} for p, c in caps.items()}
    if any(od2.values()) and not s["runner"].get("phases"):
        s["runner"] = dict(s["runner"], phases={p: c for p, c in od2.items() if c})   # the caps are spec data
    return s


def stub(name: str, lib: dict, parent: str, parent_lib_rel: str, parent_members: list[str], new: list[dict],
         spec_rel: str) -> str:
    rescreens = set(lib.get("rescreens", []))
    removed = [m for m in parent_members if m not in set(lib["members"])]
    trials = [a for a in new if a["id"] not in rescreens]
    rows = "\n".join(f"| `{a['id']}` | {a['theme']} | {a['tier']} | {a['prior_sign']:+d} | {a['origin']} | "
                     f"{'re-screen (0 admission trials)' if a['id'] in rescreens else 'admission trial'} | `{a['dsl']}` |"
                     for a in new)
    notes = "\n".join(f"- `{a['id']}`: {a['citation']} (prior sign source: {a['prior_sign_source']}; form "
                      f"{a['form']}). Formula: {a['notes']['formula'] or 'n/a'}. Domain: {a['notes']['domain'] or 'n/a'}. "
                      f"Deviation: {a['notes']['deviation'] or 'n/a'}." for a in new)
    return (f"# Pre-registration stub: library {name} ({lib['id']}) = {parent} + {len(new)} new member(s)\n\n"
            "Written by `research_cycle.py add-alpha` (regenerated on every add into this library). Root registers "
            "it in the sprint pre-registration before any IC read of a new member. Nothing here was measured.\n\n"
            "| id | theme | tier | prior sign | origin | trial | DSL |\n|---|---|---|---|---|---|---|\n"
            f"{rows}\n\n{notes}\n\n"
            f"Parent: {parent} (`{parent_lib_rel}`), {len(parent_members) - len(removed)} members unchanged"
            + (f", replaced or removed: {', '.join(removed)}" if removed else "") + ". Trials: "
            f"{len(trials)} admission trial(s)" + (f" and {len(rescreens)} re-screen(s)" if rescreens else "") +
            "; the cell's cross-cell N resolves from the ledger at scoring time (summ.dsr_n \"ledger+1\"). "
            f"Spec: `{spec_rel}`.\n\n"
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


def plan_for(parent_spec: dict, root: Path, library_bytes: bytes, lib_id: str, fields_dir: str, plan_json: Path | None):
    """The K1 plan of the new library: a saved plan, else the parent spec's IC exe --plan-only (metadata only) on the
    child's fields dir."""
    if plan_json is not None:
        return G.load_json(plan_json if plan_json.is_absolute() else root / plan_json)
    c = RC.Cycle(parent_spec, RC.Resolver(root), verify=False)
    exe = Path(c.spec["exes"]["ic"])
    exe = exe if exe.is_absolute() else root / exe
    role = parent_spec["inputs"]["role"]["path"]
    res = RC.Resolver(root)
    fdm = f"{fields_dir}/manifest.json"
    if res.sha(role) is None or res.sha(fdm) is None:
        raise AddAlphaError(f"K1 plan: the role {role} and the fields {fdm} must exist (or pass --plan-json)")
    cap = RC.option_value(c.spec["ic"].get("flags", []), "--max-memory-mib")   # PM6-9: the plan under the run's cap
    with tempfile.TemporaryDirectory() as tmp:
        lib = Path(tmp) / f"{lib_id}.json"
        lib.write_bytes(library_bytes)
        return G.exe_plan(str(exe), lib, role, res.sha(role), fields_dir, res.sha(fdm), cwd=root, env=c.env(),
                          max_memory_mib=None if cap is None else int(cap))


def parse_limits(items: list[str]) -> dict:
    out = {}
    for item in items:
        key, _, value = item.partition("=")
        if key not in G.BUDGET or not value.isdigit():
            raise AddAlphaError(f"--exception {item}: LIMIT=N with LIMIT one of {', '.join(G.BUDGET)}")
        out[key] = int(value)
    return out


def child_definition(a, strategies: Path, name: str, prereg: str) -> dict:
    """libraries/NAME.json after this call: X appended, or in place of --replaces; its exception recorded."""
    replaces, removes = list(a.replaces or []), list(a.removes or [])
    if a.rescreen and len(replaces) != 1:
        raise AddAlphaError("--rescreen needs exactly one --replaces ID (the member whose hypothesis it re-screens)")
    if replaces and removes:
        raise AddAlphaError("--replaces (in place) and --removes (appended) exclude each other")
    if bool(a.exception) != bool(a.exception_basis):
        raise AddAlphaError("--exception LIMIT=N and --exception-basis TEXT (the ruling) go together")
    lib = G.child_library(strategies, a.parent, name, [] if replaces else [a.id], prereg)
    if replaces:
        lib = G.replace_members(lib, a.id, replaces, a.rescreen)
    if removes:
        lib = G.remove_members(lib, removes, a.id)
    if a.exception:
        lib = G.set_exception(lib, a.id, parse_limits(a.exception), a.exception_basis)
    return G.validate_library(lib, name)


def add_alpha(a) -> int:
    root = Path(a.root).resolve()
    strategies = root / STRATEGIES
    name = a.name or next_name(a.parent)
    if name == a.parent:
        raise AddAlphaError("--name must differ from --parent")
    spec_path = root / SPECS_V8 / f"lib-{name}.json"
    if spec_path.is_file() and has_outputs(root, spec_path):
        raise AddAlphaError(f"library {name} already has cycle outputs ({spec_path}); add to a new --name")
    parent_spec_path = find_parent_spec(root, a.parent, a.parent_spec)
    parent_spec = RC.load_spec(parent_spec_path)
    refusal = research_spec.run_refusal(parent_spec, parent_spec_path, root)
    if refusal:
        raise AddAlphaError(f"parent spec {parent_spec_path.name} is not a cell that ran: {refusal}")
    reg = G.load_registry(strategies)
    entry = entry_of(argparse.Namespace(**dict(vars(a), name=name)))
    created = G.register_alpha(reg, entry)
    prereg_rel = f"{STRATEGIES}/libraries/{name}.prereg.md"
    lib = child_definition(a, strategies, name, f"{prereg_rel} (add-alpha stub; root registers it in the sprint "
                                                "pre-registration before any IC read)")
    doc = G.build_library(reg, lib)
    library_bytes = G.encode(doc)
    parent_file, parent_members = G.parent_library(strategies, a.parent)
    new_ids = [m for m in lib["members"] if m not in set(parent_members)]
    outs = parent_outputs(parent_spec, root)
    fields_arg = a.fields.rstrip("/") if a.fields else None
    if fields_arg is None and spec_path.is_file():               # --fields is sticky within one wave
        earlier = json.loads(spec_path.read_text(encoding="utf-8"))["fields"]["output"]
        fields_arg = earlier if earlier != outs["fields"] else None
    fields_dir = fields_arg or outs["fields"]
    plan = plan_for(parent_spec, root, library_bytes, lib["id"], fields_dir, a.plan_json)
    problems = G.validate_plan(reg, lib, doc, library_bytes, plan, budget_ids=set(new_ids))
    if problems:
        raise AddAlphaError("K1 static validation failed: " + "; ".join(problems))
    lib_name, recipe_name = G.output_names(lib)
    alphas = {x["id"]: x for x in reg["alphas"]}
    spec = derive_spec(parent_spec, a.parent, name, f"{STRATEGIES}/{lib_name}", f"{STRATEGIES}/{recipe_name}",
                       f"{STRATEGIES}/{parent_file}", lib, parent_members, outs, root, fields_arg,
                       origin=wave_origin([alphas[i]["origin"] for i in new_ids]))
    RC.validate_spec(spec)
    # every check passed: write
    (strategies / G.REGISTRY_PATH).write_bytes(G.encode_data(reg))
    G.library_file(strategies, name).parent.mkdir(parents=True, exist_ok=True)
    G.library_file(strategies, name).write_bytes(G.encode_data(lib))
    (strategies / lib_name).write_bytes(library_bytes)
    (strategies / recipe_name).write_bytes(G.encode(G.build_recipe(reg, lib, library_bytes, strategies)))
    spec_rel = f"{SPECS_V8}/lib-{name}.json"
    (root / prereg_rel).write_text(stub(name, lib, a.parent, f"{STRATEGIES}/{parent_file}", parent_members,
                                        [alphas[i] for i in new_ids], spec_rel), encoding="utf-8", newline="\n")
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
    ap.add_argument("--parent-spec", default=None, help="the parent's cycle spec or template (default: by name)")
    ap.add_argument("--plan-json", type=Path, default=None, help="a saved --plan-only JSON (default: run the exe)")
    ap.add_argument("--removes", action="append", default=None, metavar="ID",
                    help="X is appended and ID leaves the library (with its budget exception)")
    ap.add_argument("--replaces", action="append", default=None, metavar="ID",
                    help="X takes the roster place of the first ID; the other IDs leave the library")
    ap.add_argument("--rescreen", action="store_true", help="X re-screens its one --replaces member (0 trials)")
    ap.add_argument("--exception", action="append", default=None, metavar="LIMIT=N",
                    help="a budget exception of X (max_extra_fields, max_slots, max_prior_bars)")
    ap.add_argument("--exception-basis", default=None, help="the ruling behind --exception")
    ap.add_argument("--fields", default=None, metavar="DIR", help="an as-built fields dir the child pins instead of "
                                                                  "the parent's (its manifest must exist)")
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
