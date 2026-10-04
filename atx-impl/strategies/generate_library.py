#!/usr/bin/env python3
"""One generator for every registry-driven research library (platform v8 lane A, task A-1).

  generate_library.py --library NAME [--check] [--plan-json PATH] [--strategies DIR]

Data in, no per-version Python:
  alphas/registry.json   atx.alpha-registry/v1
                           alphas[]   {id, dsl, theme, tier, prior_sign, citation, prior_sign_source, form,
                                       origin (prior | grid | mined: contract K5), notes{formula, domain, deviation},
                                       added_in}
                           fields     {name: {formula_id, origin, producer, clock, basis}}  (origin "base": role field)
                           themes     {name: description}          (order = the library's family order)
                           tier_scores{grade: score}               (order = tier rank 1, 2, ...)
                           house_budget, candidate_defaults
  libraries/NAME.json    {id, parent, members[], budget_exceptions[], prereg[, rescreens[]]}  (rescreens: members that
                         re-screen a parent member's hypothesis in place, e.g. the FF49 regrouping; 0 admission trials)
Out, next to this file:
  <id>.json              atx.dsl-ic-library/v1, the IC runner's library (the legacy generators' exact encoding)
  <id>.recipe.v2.json    atx.dsl-ic-experiment/v2, the slim recipe: library pin, parent pin, lineage rows (theme, tier,
                         prior_sign, origin ... per member), the trials rule

Static validation comes from the IC exe, never from a Python parser (contract K1): `atx-equity-strategy-ic --plan-only`
prints candidates[] {id, dsl_sha256, num_slots, required_lookback, extra_fields[], node_count}. With --plan-json PATH
(that JSON, saved) every member needs a row carrying sha256(dsl) that fits the registry's house budget or the library's
recorded exception, and reading only registry fields. Writing needs a plan; --check compares the committed bytes (and
validates too when a plan is given). A library declares the role base fields and the registry fields its DSL strings
name (a token match on the registry's field table, cross-checked against the plan's extra_fields). A parent without a
libraries/<parent>.json is a frozen legacy library, fund_industry_ic_<parent>.json (v4..v7.1 keep their generators).
"""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys

HERE = Path(__file__).resolve().parent
REGISTRY_SCHEMA = "atx.alpha-registry/v1"
LIBRARY_SCHEMA = "atx.dsl-ic-library/v1"
RECIPE_SCHEMA = "atx.dsl-ic-experiment/v2"
ORIGINS = ("prior", "grid", "mined")                       # contract K5: copied into every ledger line
ALPHA_KEYS = ("id", "dsl", "theme", "tier", "prior_sign", "citation", "prior_sign_source", "form", "origin", "notes",
              "added_in")
NOTE_KEYS = ("formula", "domain", "deviation")
FIELD_KEYS = ("formula_id", "origin", "producer", "clock", "basis")
LIBRARY_KEYS = ("id", "parent", "members", "budget_exceptions", "prereg")
LIBRARY_OPTIONAL = ("rescreens",)                          # written only when non-empty (v7.1 bytes unchanged)
PLAN_KEYS = ("id", "dsl_sha256", "num_slots", "required_lookback", "extra_fields", "node_count")   # contract K1
BUDGET = {"max_extra_fields": "extra fields", "max_slots": "slots", "max_prior_bars": "prior bars"}  # per member
HOUSE_KEYS = tuple(BUDGET) + ("max_dsl_bytes", "max_roster")
BASE_ORIGIN = "base"                                       # a role field (close, raw_close, volume): always declared
LEGACY_PREFIX = "fund_industry_ic_"                        # frozen v4..v7.1 libraries: fund_industry_ic_<name>.json
REGISTRY_PATH = "alphas/registry.json"
ID_RE = re.compile(r"[a-z][a-z0-9_]*")
IDENT = re.compile(r"[A-Za-z_][A-Za-z0-9_]*")


class LibraryError(ValueError):
    pass


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def encode(obj) -> bytes:
    """The legacy generators' encoder (library and recipe files): 2-space indent, ASCII, no NaN, final newline."""
    return (json.dumps(obj, indent=2, ensure_ascii=True, allow_nan=False) + "\n").encode()


def encode_data(obj) -> bytes:
    """The encoder of the hand-edited data files (registry, libraries/<name>.json): UTF-8 text, final newline."""
    return (json.dumps(obj, indent=2, ensure_ascii=False, allow_nan=False) + "\n").encode("utf-8")


def load_json(path: Path) -> dict:
    try:
        return json.loads(Path(path).read_text(encoding="utf-8"))
    except (OSError, ValueError) as exc:
        raise LibraryError(f"{path}: {exc}") from exc


# ------------------------------------------------------------------ the registry
def validate_alpha(reg: dict, a: dict) -> None:
    aid = a.get("id") if isinstance(a, dict) else None
    if not isinstance(a, dict) or set(a) != set(ALPHA_KEYS):
        raise LibraryError(f"registry alpha {aid!r}: keys must be exactly {', '.join(ALPHA_KEYS)} "
                           f"(missing {sorted(set(ALPHA_KEYS) - set(a or {}))}, unknown "
                           f"{sorted(set(a or {}) - set(ALPHA_KEYS))})")
    if not isinstance(aid, str) or not ID_RE.fullmatch(aid):
        raise LibraryError(f"registry alpha {aid!r}: id must match {ID_RE.pattern}")
    if a["origin"] not in ORIGINS:
        raise LibraryError(f"registry alpha {aid}: origin must be one of {', '.join(ORIGINS)} (contract K5), "
                           f"got {a['origin']!r}")
    if a["theme"] not in reg["themes"]:
        raise LibraryError(f"registry alpha {aid}: unknown theme {a['theme']!r} (registry themes: "
                           f"{', '.join(reg['themes'])}; a new theme is a registry themes entry)")
    if a["tier"] not in reg["tier_scores"]:
        raise LibraryError(f"registry alpha {aid}: tier {a['tier']!r} is not one of {', '.join(reg['tier_scores'])}")
    if type(a["prior_sign"]) is not int or a["prior_sign"] not in (0, 1):
        raise LibraryError(f"registry alpha {aid}: prior_sign must be 1 (the literature sign embedded in the DSL) or 0 "
                           "(no prior)")
    if not isinstance(a["dsl"], str) or not a["dsl"] or len(a["dsl"].encode()) > reg["house_budget"]["max_dsl_bytes"]:
        raise LibraryError(f"registry alpha {aid}: dsl must be a non-empty string of at most "
                           f"{reg['house_budget']['max_dsl_bytes']} bytes")
    for key in ("citation", "prior_sign_source", "form", "added_in"):
        if not isinstance(a[key], str) or not a[key].strip():
            raise LibraryError(f"registry alpha {aid}: {key} must be a non-empty string")
    notes = a["notes"]
    if not isinstance(notes, dict) or set(notes) != set(NOTE_KEYS) or not all(v is None or isinstance(v, str)
                                                                               for v in notes.values()):
        raise LibraryError(f"registry alpha {aid}: notes must be {{{', '.join(NOTE_KEYS)}}}, each a string or null")


def validate_registry(reg: dict) -> dict:
    if not isinstance(reg, dict) or reg.get("schema") != REGISTRY_SCHEMA:
        raise LibraryError(f"registry schema must be {REGISTRY_SCHEMA}")
    for key, kind in (("alphas", list), ("fields", dict), ("themes", dict), ("tier_scores", dict),
                      ("house_budget", dict), ("candidate_defaults", dict)):
        if not isinstance(reg.get(key), kind) or not reg[key]:
            raise LibraryError(f"registry {key} must be a non-empty {kind.__name__}")
    if set(reg["house_budget"]) != set(HOUSE_KEYS) or not all(type(v) is int and v > 0 for v in
                                                              reg["house_budget"].values()):
        raise LibraryError(f"registry house_budget must give positive integers {', '.join(HOUSE_KEYS)}")
    if set(reg["candidate_defaults"]) != {"horizons", "sign_policy"}:
        raise LibraryError("registry candidate_defaults must give horizons and sign_policy")
    if not all(isinstance(v, str) and v for v in reg["themes"].values()):
        raise LibraryError("registry themes: every theme needs a description")
    for name, f in reg["fields"].items():
        if not isinstance(f, dict) or set(f) != set(FIELD_KEYS) or not isinstance(f["basis"], str) or not f["basis"]:
            raise LibraryError(f"registry field {name}: keys must be exactly {', '.join(FIELD_KEYS)} with a basis")
    ids = []
    for a in reg["alphas"]:
        validate_alpha(reg, a)
        ids.append(a["id"])
    if len(set(ids)) != len(ids):
        raise LibraryError(f"registry: duplicate alpha ids {sorted({i for i in ids if ids.count(i) > 1})}")
    return reg


def load_registry(root: Path = HERE) -> dict:
    return validate_registry(load_json(Path(root) / REGISTRY_PATH))


IDENTITY_KEYS = ("dsl", "theme", "tier", "prior_sign", "citation")   # what makes two registrations the same alpha


def register_alpha(reg: dict, entry: dict) -> bool:
    """Add one alpha to the registry (True), or accept an identical registration of an existing id (False). An id is
    never redefined and a DSL never registered twice under two ids."""
    validate_alpha(reg, entry)
    have = next((a for a in reg["alphas"] if a["id"] == entry["id"]), None)
    if have is not None:
        diff = [k for k in IDENTITY_KEYS if have[k] != entry[k]]
        if diff:
            raise LibraryError(f"alpha {entry['id']} is registered with another {', '.join(diff)}: a registered alpha "
                               "is immutable, register the new definition under a new id")
        return False
    twin = next((a["id"] for a in reg["alphas"] if a["dsl"] == entry["dsl"]), None)
    if twin is not None:
        raise LibraryError(f"alpha {entry['id']}: the same DSL is registered as {twin}")
    reg["alphas"].append(entry)
    return True


def tier_rank(reg: dict, tier: str) -> int:
    return list(reg["tier_scores"]).index(tier) + 1


def referenced_fields(reg: dict, dsl: str) -> list[str]:
    """The registry fields a DSL string names (identifier tokens that are registry field names), in registry order."""
    names = set(IDENT.findall(dsl))
    return [name for name in reg["fields"] if name in names]


def extra_fields(reg: dict, dsl: str) -> list[str]:
    return [n for n in referenced_fields(reg, dsl) if reg["fields"][n]["origin"] != BASE_ORIGIN]


# ------------------------------------------------------------------ library definitions
def library_file(root: Path, name: str) -> Path:
    return Path(root) / "libraries" / f"{name}.json"


def validate_library(lib: dict, name: str) -> dict:
    if not isinstance(lib, dict) or not set(LIBRARY_KEYS) <= set(lib) <= set(LIBRARY_KEYS + LIBRARY_OPTIONAL):
        raise LibraryError(f"libraries/{name}.json: keys must be exactly {', '.join(LIBRARY_KEYS)} (optional "
                           f"{', '.join(LIBRARY_OPTIONAL)})")
    rescreens = lib.get("rescreens", [])
    if "rescreens" in lib and (not isinstance(rescreens, list) or not rescreens or len(set(rescreens)) != len(rescreens)
                               or not set(rescreens) <= set(lib.get("members") or [])):
        raise LibraryError(f"libraries/{name}.json: rescreens must list distinct members (omitted when none)")
    if not isinstance(lib["id"], str) or not ID_RE.fullmatch(lib["id"]):
        raise LibraryError(f"libraries/{name}.json: id must match {ID_RE.pattern}")
    if lib["parent"] is not None and (not isinstance(lib["parent"], str) or not lib["parent"]):
        raise LibraryError(f"libraries/{name}.json: parent must be a library name or null")
    members = lib["members"]
    if not isinstance(members, list) or not members or len(set(members)) != len(members):
        raise LibraryError(f"libraries/{name}.json: members must be a non-empty list of distinct alpha ids")
    if not isinstance(lib["budget_exceptions"], list):
        raise LibraryError(f"libraries/{name}.json: budget_exceptions must be a list")
    for e in lib["budget_exceptions"]:
        limits = set(e) - {"id", "basis"} if isinstance(e, dict) else set()
        if not isinstance(e, dict) or not isinstance(e.get("basis"), str) or e.get("id") not in members or \
                not limits or not limits <= set(BUDGET) or not all(type(e[k]) is int and e[k] > 0 for k in limits):
            raise LibraryError(f"libraries/{name}.json: a budget exception is {{id (a member), basis, one or more of "
                               f"{', '.join(BUDGET)} (positive integers)}}")
    if not isinstance(lib["prereg"], str) or not lib["prereg"]:
        raise LibraryError(f"libraries/{name}.json: prereg must name the pre-registration")
    return lib


def load_library(root: Path, name: str) -> dict:
    return validate_library(load_json(library_file(root, name)), name)


def parent_library(root: Path, parent: str | None) -> tuple[str, list[str]] | None:
    """(library JSON file name next to the generator, member ids) of a parent: a registry library, else legacy."""
    if parent is None:
        return None
    if library_file(root, parent).is_file():
        lib = load_library(root, parent)
        return f"{lib['id']}.json", list(lib["members"])
    legacy = Path(root) / f"{LEGACY_PREFIX}{parent}.json"
    if not legacy.is_file():
        raise LibraryError(f"parent {parent}: no libraries/{parent}.json and no legacy {legacy.name}")
    return legacy.name, [c["id"] for c in load_json(legacy)["candidates"]]


LEGACY_EXCEPTIONS = (("max_extras_per_candidate_exceptions", "max_extras_exception_basis", "max_extra_fields"),
                     ("max_estimated_peak_slots_exceptions", "max_estimated_peak_slots_exception_basis", "max_slots"))


def parent_exceptions(root: Path, parent: str | None, members: list[str]) -> list[dict]:
    """The budget exceptions a child library inherits for its members: its parent library file's, else those the
    legacy parent's recipe recorded (static_validation, e.g. v7.0's q5_eg and qmj_safety)."""
    if parent is None:
        return []
    if library_file(root, parent).is_file():
        return [e for e in load_library(root, parent)["budget_exceptions"] if e["id"] in members]
    recipe = Path(root) / f"{LEGACY_PREFIX}{parent}.recipe.json"
    sv = (load_json(recipe).get("static_validation") or {}) if recipe.is_file() else {}
    return [{"id": cid, limit: n, "basis": sv.get(basis) or f"legacy {recipe.name}"}
            for key, basis, limit in LEGACY_EXCEPTIONS for cid, n in (sv.get(key) or {}).items() if cid in members]


def rename(text: str, old: str, new: str) -> str:
    """The name template: every token `old` (not inside a longer name or number) becomes `new`; unchanged text is an
    error (a derived name must differ from its parent's)."""
    out = re.sub(rf"(?<![A-Za-z0-9]){re.escape(old)}(?![0-9])", new, text)
    if out == text:
        raise LibraryError(f"name template: {text!r} does not contain the parent name {old!r}")
    return out


def child_library(root: Path, parent: str, name: str, add: list[str], prereg: str) -> dict:
    """The definition of library `name` = parent's members + `add` (appended in order); an existing libraries/NAME.json
    of the same parent gains the new members (a second add-alpha into one wave)."""
    if library_file(root, name).is_file():
        lib = load_library(root, name)
        if lib["parent"] != parent:
            raise LibraryError(f"libraries/{name}.json exists with parent {lib['parent']!r}, not {parent!r}")
        return dict(lib, members=lib["members"] + [m for m in add if m not in lib["members"]])
    parent_file, parent_members = parent_library(root, parent)
    dup = [m for m in add if m in parent_members]
    if dup:
        raise LibraryError(f"{', '.join(dup)} already a member of {parent}")
    members = parent_members + list(add)
    return dict(id=rename(Path(parent_file).stem, parent, name), parent=parent, members=members,
                budget_exceptions=parent_exceptions(root, parent, members), prereg=prereg)


def replace_members(lib: dict, new: str, replaces: list[str], rescreen: bool = False) -> dict:
    """Library `lib` with member `new` in place of `replaces` (research_cycle add-alpha --replaces): it takes the roster
    position of the first replaced member, the others leave, and it inherits their budget exceptions (per limit, the
    largest). `rescreen` (one replaced member: the same hypothesis in another peer group, e.g. the FF49 regrouping,
    Ruling R2-e) lists it in `rescreens` (0 admission trials). A call whose replacement is already in place is a no-op."""
    members = list(lib["members"])
    if not replaces or len(set(replaces)) != len(replaces):
        raise LibraryError("--replaces needs distinct member ids")
    if rescreen and len(replaces) != 1:
        raise LibraryError("a re-screen replaces exactly one member")
    gone = [r for r in replaces if r in members]
    if not gone and new in members:                           # an identical second call
        return lib
    if gone != list(replaces) or new in members:
        raise LibraryError(f"--replaces {', '.join(replaces)}: every replaced id must be a member of {lib['id']} and "
                           f"{new} must not be one (members replaced: {gone})")
    members[members.index(replaces[0])] = new
    members = [m for m in members if m not in replaces[1:]]
    inherited: dict = {}
    for e in lib["budget_exceptions"]:
        if e["id"] in replaces:
            for key in BUDGET:
                if key in e:
                    inherited[key] = max(inherited.get(key, 0), e[key])
    kept = [e for e in lib["budget_exceptions"] if e["id"] not in replaces]
    if inherited:
        basis = "; ".join(f"{e['id']}: {e['basis']}" for e in lib["budget_exceptions"] if e["id"] in replaces)
        kept.append(dict(id=new, **inherited, basis=f"inherited from {', '.join(replaces)} (replaced in place): {basis}"))
    out = dict(lib, members=members, budget_exceptions=kept)
    rescreens = [r for r in lib.get("rescreens", []) if r in members] + ([new] if rescreen else [])
    out.pop("rescreens", None)
    if rescreens:
        out["rescreens"] = rescreens
    return out


def remove_members(lib: dict, removes: list[str], keep: str) -> dict:
    """Library `lib` without the members `removes` (research_cycle add-alpha --removes, e.g. library-v8-draft E3: the
    composite `keep`, appended, stands for sue, droe and chtax), their budget exceptions and re-screen marks. A call
    whose removal is already done (none of them a member, `keep` one) is a no-op."""
    members = list(lib["members"])
    if not removes or len(set(removes)) != len(removes) or keep in removes:
        raise LibraryError("--removes needs distinct member ids other than the new one")
    gone = [r for r in removes if r in members]
    if not gone and keep in members:
        return lib
    if gone != list(removes):
        raise LibraryError(f"--removes {', '.join(removes)}: every removed id must be a member of {lib['id']} "
                           f"(members: {gone})")
    out = dict(lib, members=[m for m in members if m not in removes],
               budget_exceptions=[e for e in lib["budget_exceptions"] if e["id"] not in removes])
    rescreens = [r for r in lib.get("rescreens", []) if r not in removes]
    out.pop("rescreens", None)
    if rescreens:
        out["rescreens"] = rescreens
    return out


def set_exception(lib: dict, cid: str, limits: dict, basis: str) -> dict:
    """Library `lib` with a recorded budget exception of member `cid` (add-alpha --exception LIMIT=N; the ruling in
    `basis`): its limits are added to (or replace those of) the member's exception."""
    if cid not in lib["members"] or not limits or not set(limits) <= set(BUDGET) or \
            not all(type(v) is int and v > 0 for v in limits.values()) or not basis:
        raise LibraryError(f"exception for {cid}: a member, limits among {', '.join(BUDGET)} (positive integers) and "
                           "a basis (the ruling)")
    have = next((e for e in lib["budget_exceptions"] if e["id"] == cid), {})
    if all(have.get(k) == v for k, v in limits.items()) and basis in have.get("basis", ""):
        return lib                                            # an identical second call
    entry = {"id": cid, **{k: v for k, v in have.items() if k in BUDGET}, **limits,
             "basis": basis if not have else f"{have['basis']}; {basis}"}
    return dict(lib, budget_exceptions=[e for e in lib["budget_exceptions"] if e["id"] != cid] + [entry])


def build_library(reg: dict, lib: dict) -> dict:
    """The IC runner's library (atx.dsl-ic-library/v1) of a library definition."""
    alphas = {a["id"]: a for a in reg["alphas"]}
    missing = [m for m in lib["members"] if m not in alphas]
    if missing:
        raise LibraryError(f"library {lib['id']}: members not in the registry: {', '.join(missing)}")
    members = [alphas[m] for m in lib["members"]]
    if len(members) > reg["house_budget"]["max_roster"]:
        raise LibraryError(f"library {lib['id']}: {len(members)} members, roster cap {reg['house_budget']['max_roster']}")
    dsls = [a["dsl"] for a in members]
    if len(set(dsls)) != len(dsls):
        raise LibraryError(f"library {lib['id']}: two members share a DSL string")
    used = {n for a in members for n in referenced_fields(reg, a["dsl"])}
    fields = [dict(name=n, basis=f["basis"]) for n, f in reg["fields"].items() if f["origin"] == BASE_ORIGIN or n in used]
    themes = {a["theme"] for a in members}
    families = [dict(id=t, description=text) for t, text in reg["themes"].items() if t in themes]
    d = reg["candidate_defaults"]
    candidates = [dict(id=a["id"], family=a["theme"], dsl=a["dsl"], horizons=list(d["horizons"]),
                       sign_policy=d["sign_policy"], theme=a["theme"], tier=a["tier"],
                       tier_rank=tier_rank(reg, a["tier"]), prior_sign=a["prior_sign"], citation=a["citation"])
                  for a in members]
    return dict(schema=LIBRARY_SCHEMA, id=lib["id"], fields=fields, families=families, candidates=candidates)


def build_recipe(reg: dict, lib: dict, library_bytes: bytes, root: Path) -> dict:
    """The slim recipe (atx.dsl-ic-experiment/v2): what the fitter and the ledger read, nothing re-derived."""
    parent = parent_library(root, lib["parent"])
    parent_members = set(parent[1]) if parent else set()
    alphas = {a["id"]: a for a in reg["alphas"]}
    new = [m for m in lib["members"] if m not in parent_members]
    rescreens = [m for m in lib.get("rescreens", []) if m in new]
    lineage = []
    for k, m in enumerate(lib["members"], 1):
        a = alphas[m]
        lineage.append(dict(id=m, roster_order=k, theme=a["theme"], tier=a["tier"], tier_rank=tier_rank(reg, a["tier"]),
                            prior_sign=a["prior_sign"], prior_sign_source=a["prior_sign_source"], form=a["form"],
                            origin=a["origin"], added_in=a["added_in"], dsl_sha256=sha256(a["dsl"].encode()),
                            fields=sorted(referenced_fields(reg, a["dsl"]))))
    return dict(
        schema=RECIPE_SCHEMA, id=lib["id"],
        library=dict(path=f"{lib['id']}.json", sha256=sha256(library_bytes)),
        parent=None if parent is None else dict(name=lib["parent"], library=parent[0],
                                                sha256=sha256((Path(root) / parent[0]).read_bytes())),
        registry=dict(path=REGISTRY_PATH, schema=REGISTRY_SCHEMA),
        preregistration=lib["prereg"],
        generation=dict(generator="generate_library.py", rule="registry-library-v1: one registry entry per alpha, the "
                        "library = its ordered member ids", candidates=len(lib["members"]),
                        families=len({a["theme"] for a in (alphas[m] for m in lib["members"])}), family_is_theme=True,
                        new_members=new, house_budget=reg["house_budget"], budget_exceptions=lib["budget_exceptions"]),
        lineage=lineage,
        trials=dict(new_candidates=len(new), unchanged_candidates=len(lib["members"]) - len(new),
                    admission_trials=len(new) - len(rescreens), dsr_n_rule="cross-cell N = trial-ledger lines at run "
                    "time + 1 (research_cycle summ.dsr_n \"ledger+1\")",
                    **({"rescreens": rescreens, "removed_parent_members": sorted(parent_members - set(lib["members"]))}
                       if rescreens or parent_members - set(lib["members"]) else {})),
        static_validation=dict(source="atx-equity-strategy-ic --plan-only candidates[] (contract K1)",
                               rule="each member's plan row carries sha256(dsl), fits the house budget or its recorded "
                                    "exception and reads registry fields only; checked by generate_library.py "
                                    "--plan-json and research_cycle.py add-alpha, not recorded here"))


def output_names(lib: dict) -> tuple[str, str]:
    return f"{lib['id']}.json", f"{lib['id']}.recipe.v2.json"


def documents(root: Path, name: str, reg: dict | None = None) -> dict[str, bytes]:
    """{file name next to the generator: bytes} of library NAME: the IC library and its slim recipe."""
    reg = reg if reg is not None else load_registry(root)
    lib = load_library(root, name)
    library_bytes = encode(build_library(reg, lib))
    lib_name, recipe_name = output_names(lib)
    return {lib_name: library_bytes, recipe_name: encode(build_recipe(reg, lib, library_bytes, root))}


# ------------------------------------------------------------------ K1 plan rows: the static validation
def plan_rows(plan) -> dict[str, dict]:
    """{id: row} of the exe's --plan-only JSON (its candidates[] array, contract K1) or of a bare list of rows."""
    rows = plan.get("candidates") if isinstance(plan, dict) else plan
    if not isinstance(rows, list):
        raise LibraryError("plan: no candidates[] rows (contract K1: atx-equity-strategy-ic --plan-only after lane B-3)")
    out = {}
    for row in rows:
        if not isinstance(row, dict) or not set(PLAN_KEYS) <= set(row) or not isinstance(row["id"], str):
            raise LibraryError(f"plan row {row!r:.80}: needs {', '.join(PLAN_KEYS)}")
        if not all(type(row[k]) is int and row[k] >= 0 for k in ("num_slots", "required_lookback", "node_count")) or \
                not isinstance(row["extra_fields"], list) or not all(isinstance(f, str) for f in row["extra_fields"]):
            raise LibraryError(f"plan row {row['id']}: num_slots, required_lookback, node_count are integers >= 0 and "
                               "extra_fields a list of names")
        if row["id"] in out:
            raise LibraryError(f"plan: two rows for {row['id']}")
        out[row["id"]] = row
    return out


def validate_plan(reg: dict, lib: dict, library_doc: dict, library_bytes: bytes, plan,
                  budget_ids: set | None = None) -> list[str]:
    """Problems of a library against the exe's plan (empty: valid). Every member has a row with sha256(dsl), reads
    declared registry fields only, and the token match agrees with the exe; the members in `budget_ids` (default all)
    also fit the house budget or their recorded exception (add-alpha budgets the new members only)."""
    rows = plan_rows(plan)
    problems = []
    if isinstance(plan, dict) and plan.get("library_sha256") not in (None, sha256(library_bytes)):
        problems.append(f"plan was made for library sha256 {plan['library_sha256']}, not {sha256(library_bytes)}")
    house = reg["house_budget"]
    exceptions = {e["id"]: e for e in lib["budget_exceptions"]}
    declared = {f["name"] for f in library_doc["fields"]}
    measure = {"max_extra_fields": lambda r: len(r["extra_fields"]), "max_slots": lambda r: r["num_slots"],
               "max_prior_bars": lambda r: r["required_lookback"]}
    for c in library_doc["candidates"]:
        cid, row = c["id"], rows.get(c["id"])
        if row is None:
            problems.append(f"{cid}: no plan row")
            continue
        if row["dsl_sha256"] != sha256(c["dsl"].encode()):
            problems.append(f"{cid}: plan dsl_sha256 {row['dsl_sha256']} is not sha256 of the registry DSL")
        for key, what in (BUDGET.items() if budget_ids is None or cid in budget_ids else ()):
            limit = exceptions.get(cid, {}).get(key, house[key])
            if measure[key](row) > limit:
                problems.append(f"{cid}: {measure[key](row)} {what} > {limit} "
                                f"({'recorded exception' if key in exceptions.get(cid, {}) else 'house budget'})")
        undeclared = sorted(set(row["extra_fields"]) - declared)
        if undeclared:
            problems.append(f"{cid}: reads {undeclared}, not registry fields (add them to the registry fields table)")
        scan = extra_fields(reg, c["dsl"])
        if sorted(row["extra_fields"]) != sorted(scan):
            problems.append(f"{cid}: the exe reads {sorted(row['extra_fields'])}, the registry token match {scan}")
    stray = sorted(set(rows) - {c["id"] for c in library_doc["candidates"]})
    if stray:
        problems.append(f"plan rows of ids outside the library: {stray}")
    return problems


def exe_plan(exe: str, library: Path, role_manifest: str, role_sha: str, fields_dir: str, fields_sha: str, *,
             cwd: Path, env: dict | None = None, max_memory_mib: int | None = None) -> dict:
    """Run the IC exe's metadata-only plan on a library file (no payload is read) and return its JSON.
    `max_memory_mib` (PM6-9): the spec's IC memory cap, passed as --max-memory-mib so the plan is made under the
    run's cap; None leaves the argv as before."""
    argv = [exe, "--plan-only", "--library", str(library), "--library-sha256", sha256(Path(library).read_bytes()),
            "--train", role_manifest, "--train-sha256", role_sha, "--train-fields", fields_dir,
            "--train-fields-sha256", fields_sha]
    if max_memory_mib is not None:
        argv += ["--max-memory-mib", str(max_memory_mib)]
    try:
        done = subprocess.run(argv, cwd=cwd, env=env, capture_output=True, text=True, timeout=300)
    except (OSError, subprocess.SubprocessError) as exc:
        raise LibraryError(f"plan: {exe} --plan-only failed to run: {exc}") from exc
    if done.returncode != 0:
        raise LibraryError(f"plan: {exe} --plan-only exit {done.returncode}: {(done.stderr or '')[-400:]}")
    try:
        return json.loads(done.stdout)
    except ValueError as exc:
        raise LibraryError(f"plan: {exe} --plan-only printed no JSON ({exc})") from exc


# ------------------------------------------------------------------ CLI
def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n", 1)[0], epilog=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--library", required=True, help="library name: libraries/NAME.json")
    ap.add_argument("--check", action="store_true", help="compare the committed files instead of writing them")
    ap.add_argument("--plan-json", type=Path, default=None, help="the IC exe's --plan-only JSON (contract K1)")
    ap.add_argument("--strategies", type=Path, default=HERE, help=argparse.SUPPRESS)
    a = ap.parse_args(argv)
    try:
        reg = load_registry(a.strategies)
        docs = documents(a.strategies, a.library, reg)
        lib = load_library(a.strategies, a.library)
        lib_name = output_names(lib)[0]
        if a.plan_json is not None:
            problems = validate_plan(reg, lib, json.loads(docs[lib_name]), docs[lib_name], load_json(a.plan_json))
            for p in problems:
                print(f"generate_library: INVALID {p}", file=sys.stderr)
            if problems:
                return 1
            print(f"static validation: {len(lib['members'])} plan rows (K1) within budget")
        elif not a.check:
            raise LibraryError("writing a library needs its static validation: pass --plan-json (the IC exe's "
                               "--plan-only JSON)")
        for name, blob in docs.items():
            path = Path(a.strategies) / name
            if a.check:
                if not path.is_file() or path.read_bytes() != blob:
                    print(f"generate_library: fixed artifact differs: {name}", file=sys.stderr)
                    return 1
            else:
                path.write_bytes(blob)
            print(f"{name} {sha256(blob)} {len(blob)} bytes")
        return 0
    except LibraryError as exc:
        print(f"generate_library: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
