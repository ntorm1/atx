"""The research wave manifest v2 (contract K-P9-9, P9 lane E2): ``atx.research-wave/v2`` is the v1 manifest
(wave_manifest.py, every key and check of it) plus a top-level ``kind`` and that kind's block. A v1 manifest is
unchanged in every byte it plans and writes; only a v2 manifest reaches this module.

  kind           block                                       the cell
  add            candidates, each of kind add                a library wave, as v1
  replace        candidates, one or more of kind replace     a library wave, as v1
  rule           rule_cell + rules: [{name, params?}, ...]   a rule wave whose rule flags come from the exes' rule
                                                             tables (below), never from rule_cell.constants.flags nor
                                                             the template's change.flags (review F-8)
  leverage       leverage: {value, name?}, rule_cell?        the parent's construction at nav.leverage = value
                                                             (nav.output renamed -L<value>); gross_match "none";
                                                             without a rule_cell the spec stage writes the NAV-only
                                                             template
                                                             next to the parent spec
  horizon        rule_cell + horizon: {days, flags}          a rule wave whose flags change a horizon or holding period
                                                             (days in WINDOWS); every output the parent has of u, fit,
                                                             card, marginal, w, nav and monitor is renamed -h<days>, so
                                                             nothing is silently reused (OR section 6)
  field          candidates + field: {rows, output,          the register stage first builds OUTPUT = the wave's fields
                 builder_args?, seconds?, max_rss_mib?}      dir plus the registry rows (prepare_research_fields.py
                                                             --registry, the wave's fields reused), then registers the
                                                             strings on OUTPUT (closes OR section 6 "new field: NO")
  role           rule_cell + role: {builder_args, output,    the register stage runs the role builder (Python,
                 fields_output, fields_args?, seconds?,      prepare_recent_research.py role ...), then the fields
                 max_rss_mib?}                               builder on the new role (the wave's field names); the cell
                                                             is the template with inputs.role and its fields set to them
  walk-forward   walk_forward: {lag, step, window?,           P9 lane D3 (K-P9-8): the parent's construction with its
                 min_decisions?}                             fit refitted walk-forward (the fitter's --walk-forward
                                                             flags, walk_forward_flags; the C++ composition exe refits
                                                             per block on the decisions <= d - lag; weights v3); no
                                                             rule_cell: the spec stage writes the template next to the
                                                             parent spec; every output downstream of the fit (fit, card,
                                                             w, NAV, monitor) renamed -wf<lag>s<step>[m<min>]; the
                                                             parent must fit under --engine-fit (ALCOMB-7)

Rules (``rules``; K-P9-6, K-P9-7, ruling D1-PSCHEMA). Each name is a row of the IC exe's ``--list-rules --json``
table (atx.composition-rules/v1) or of the NAV exe's ``nav --list-rules --json`` (atx.nav-rules/v1); a name in both,
or in neither, is refused. Its params (default {}) must be valid under the row's params_schema (SCHEMA_KEYWORDS: the
JSON Schema subset the exes print), with the row's own name as its ``rule`` key when the schema pins one. The flags:
a NAV row's from its x-flag keys (the rule's, then each param's), a composition row's from COMPOSITION_FLAGS. The
preflight stage reads both tables (their SHA-256 in its receipt), validates and derives; the spec stage writes the
flags into the cell. The exe capabilities the cycle needs come from the same envelope (cycle/phases.exe_capabilities).
Ruling C3-13: a NAV row whose x-flag is ``--rule <id>`` (an aim rule; C3's aim-partial-v5) is the cell's nav.rule, which
the cycle puts on the NAV argv itself (cycle/phases.py nav_step): the spec stage SETS nav.rule to <id> (replacing the
parent's) and appends only the row's param flags (nav_rule_split); every other row kind keeps the append route.

Raw rule flags (ruling E2-8, one guard, every v2 kind; wave_stage_kinds.rule_flag_guard): no flag a rule owns
(owned_flags: every table rule's, named or not, plus the named rules' derived ones) reaches the cell any other way
than through rules. An option map (rule_cell.constants.flags, the template's change.flags, horizon.flags) may not
name one, with ``=value`` stripped or as an argparse abbreviation (raw_rule_flags); and the cell's RESOLVED flag lists
(after constants.set, change.set / unset and the flag maps) must give every owned option the occurrences of the
parent's lists with only the derived flags applied. The IC table may list no composition row COMPOSITION_FLAGS does not
map (unmapped_ic_rules: its flags could not be guarded). A token is matched as norm_option reads it (single dash,
case). nav.leverage (LEVERAGE_KEY) is fixed-v1's aim_leverage (ruling E2-10): only kind leverage (leverage.value,
inside the table's range and at most LEVERAGE_MAX) and the gross-match stage (L') set it; any other route is refused.
MATCH_FLAGS (ruling E2-14, review E2 R3a) are the gross-match stage's own: ``nav --calibrate-gross G_P`` makes the NAV
exe publish its run at an L' it chooses itself, so no flag list of any v2 cell may carry them, in any section, route or
spelling (match_flag_reads); only the match stage of MATCH_STAGE writes them, into its run recipe, after preflight.
The reference side (ruling E2-15): a cell's ``ref`` comes only from its lineage parent's cell of record or from the
template's own pinned ``change.set ref`` block (REF_KEY), whose resolved side is guarded as the cell's.
A NAV row's legacy spellings (``x-flag-aliases``, ruling C3-3; flag_aliases) are owned flags of that row: inherited
unchanged from the lineage parent, or changed only by a wave whose rules name the row. Ruling E2-17: ``inputs.*`` is set
or changed only by the kinds and keys of KIND_INPUTS (wave_stage_kinds.inputs_refusal). P9 lane D3: kind walk-forward's
fitter flags (KIND_FLAGS) are owned by that kind.
"""
from __future__ import annotations

import json
import math
from pathlib import Path
import re

import research_tree

SCHEMA_V2 = "atx.research-wave/v2"
KINDS = ("add", "replace", "rule", "field", "role", "horizon", "walk-forward", "leverage")
BLOCKS = {"rule": "rules", "leverage": "leverage", "horizon": "horizon", "field": "field", "role": "role",
          "walk-forward": "walk_forward"}            # kind -> its block key (add / replace: candidates)
LIBRARY_KINDS = ("add", "replace", "field")         # candidates
TEMPLATE_KINDS = ("rule", "horizon", "role")        # rule_cell required (leverage: optional)
SCHEMA_ONLY: tuple = ()                             # P9 lane D3 wired walk-forward (wave 3): none is left
# P9 lane D3 (K-P9-8, contract atx.dsl-composition-weights/v3): kind walk-forward's own fitter flags (the composition
# exe refits the fit per block of decisions; the fitter writes the per-date rows). Owned by the kind: every other v2
# kind is refused a raw route to them, as a rule's flags (ruling E2-8 via table_flags)
WALK_FORWARD_FLAGS = ("--walk-forward", "--walk-forward-lag", "--walk-forward-step", "--walk-forward-min-decisions")
KIND_FLAGS = {"fit": frozenset(WALK_FORWARD_FLAGS)}
WALK_FORWARD_WINDOWS = ("expanding",)
WALK_FORWARD_PARENT_FIT = "--engine-fit"            # the per-date weights are the C++ fit's (ruling ALCOMB-7)
WALK_FORWARD_REFUSED = ("--theme-tsmom", "--theme-resid", "--era")   # their own TRAIN schedules / pools: P10
# Ruling E2-17: the inputs.* keys a v2 kind may set or change (the kinds whose schema defines an input change, and only
# the keys that kind names: a role wave's inputs.role; field and horizon blocks name none today). Every other kind
# setting or changing an inputs.* key, by any route, is refused at preflight (wave_stage_kinds.inputs_refusal)
KIND_INPUTS = {"field": (), "role": ("role",), "horizon": ()}
ALIASES_KEY = "x-flag-aliases"                      # ruling C3-3: a K-P9-7 row's legacy spellings of its x-flag
# Ruling C3-13: the x-flag of a NAV row selected by nav.rule (an aim rule): the cycle emits "--rule", nav["rule"] on
# every NAV argv (scripts/cycle/phases.py nav_step), so a rule wave naming the row sets nav.rule (nav_rule_split)
NAV_RULE_FLAG, NAV_RULE_KEY = "--rule", "nav.rule"
WINDOWS = (5, 21, 63, 126, 252)                     # plan section 0.6: windows (here horizons) from this set
HORIZON_OUTPUTS = (("ic", "u_output"), ("fit", "output"), ("card", "output"), ("marginal", "output"),
                   ("ic", "w_output"), ("nav", "output"), ("monitor", "output"))
FLAG_SECTIONS = ("ic", "fit", "nav", "card")
FIELD_BUILDER = "atx-engine/tools/prepare_research_fields.py"
ROLE_BUILDER = "atx-engine/tools/prepare_recent_research.py"
FIELD_REGISTRY = "atx-engine/tools/field_registry.json"
BUILD_RESERVED = ("--output", "--fields", "--registry", "--reuse", "--reuse-sha256", "--role", "--role-sha256")
ROLE_RESERVED = ("--out",)                          # prepare_recent_research.py role --out DIR: the build sets it
BUILD_CAPS = {"seconds": 180, "max_rss_mib": 1536}  # a build's runner caps unless its block sets them
IC_RULES, NAV_RULES = "atx.composition-rules/v1", "atx.nav-rules/v1"
LIST_RULES = ("--list-rules", "--json")
# A composition row (K-P9-6) carries no argv: the fitter flag (and, for the sleeves, the NAV flag) that turns it on
COMPOSITION_FLAGS = {
    "within-theme-v1": {"fit": {"--composition": "ew-theme-v6"}},
    "ew-theme-std-v1": {"fit": {"--composition": "ew-theme-std-v1"}},
    "ic-shrink-v1": {"fit": {"--composition": "ic-shrink-v1"}},
    "ic-shrink-aim-v1": {"fit": {"--composition": "ic-shrink-aim-v1"}},
    "theme-erc-v1": {"fit": {"--theme-erc": "theme-erc-v1"}},
    "theme-resid-v1": {"fit": {"--theme-resid": "theme-resid-v1"}},
    "theme-tsmom-v1": {"fit": {"--theme-tsmom": "theme-tsmom-v1"}},
    "two-speed-v1": {"fit": {"--two-speed": "two-speed-v1"}, "nav": {"--two-speed": "two-speed-v1"}},
    "theme-cov-hedge-v1": {"fit": {"--theme-hedge": "theme-cov-hedge-v1"}},     # MR-4 (AL-COMB)
    "mom-volman-v1": {"fit": {"--mom-volman": "mom-volman-v1"}},                # MR-4 (AL-COMB)
}
SCHEMA_KEYWORDS = {"type", "const", "enum", "required", "properties", "additionalProperties", "items", "minItems",
                   "maxItems", "minProperties", "pattern", "minimum", "maximum", "exclusiveMinimum",
                   "exclusiveMaximum", "default", "x-flag", "description"}
NAME_RE = re.compile(r"[a-z0-9][a-z0-9.-]{0,63}")
PLACEHOLDER_RE = re.compile(r"[A-Z][A-Z0-9_]*")     # "--risk-target S" / "SIGMA": a param's value, not a constant
LEVERAGE_KEY = "nav.leverage"                       # fixed-v1's aim_leverage: research_cycle puts it in {leverage}
LEVERAGE_MAX = 2.0                                  # ruling E2-10 / OD-P9-3: no P9 leverage wave above it
# P9 C2's nav --calibrate-gross G_P [--calibrate-tolerance T]: the exe moves the leverage to L' itself. Only the match
# stage of gross_match MATCH_STAGE puts them on an argv (wave_stage_cell.calibrated_doc, after preflight; E2-14 / R3a)
MATCH_FLAGS = ("--calibrate-gross", "--calibrate-tolerance")
MATCH_STAGE = "pm6-6-engine"
REF_KEY = "ref"                                     # ruling E2-15: only the template's own change.set ref sets it


class KindError(ValueError):
    pass


def _text(v) -> bool:
    return isinstance(v, str) and bool(v.strip())


def _rel(v) -> bool:
    return isinstance(v, str) and bool(v) and not Path(v).is_absolute() and ".." not in Path(v).parts


def _argv(v) -> bool:
    return isinstance(v, list) and all(isinstance(x, str) and x for x in v)


def _norm(v: str) -> str:
    """A root-relative path as one comparable string ("./a/b/", "a\\b" and "A/b" are "a/b"; Windows is case-blind)."""
    return "/".join(p for p in Path(v).parts if p not in ("", ".")).casefold()


def in_code(value: str) -> bool:
    """A root-relative path inside research_tree.CODE_PATHSPEC (wave_manifest.in_code_pathspec's rule)."""
    parts = [p for p in Path(value).parts if p not in ("", ".")]
    return bool(parts) and parts[0].lower() in {s.lower() for s in research_tree.CODE_PATHSPEC}


def is_v2(m: dict) -> bool:
    return isinstance(m, dict) and m.get("schema") == SCHEMA_V2


# ------------------------------------------------------------------ the manifest
def problems(m: dict) -> list[str]:
    """The v2 problems of a manifest that passed the v1 checks of its shared keys: the kind, its block, and the
    blocks of other kinds absent."""
    kind = m.get("kind")
    if kind not in KINDS:
        return [f"kind must be one of {', '.join(KINDS)} (contract K-P9-9)"]
    out = [f"{key} is the block of kind {k}, not of kind {kind}" for k, key in BLOCKS.items()
           if k != kind and key in m]
    has_c, has_r = "candidates" in m, "rule_cell" in m
    if kind in LIBRARY_KINDS and (not has_c or has_r):
        out.append(f"kind {kind} is a library wave: candidates, no rule_cell")
    if kind in TEMPLATE_KINDS and (not has_r or has_c):
        out.append(f"kind {kind} is a rule wave: rule_cell, no candidates")
    if kind in ("leverage", "walk-forward") + SCHEMA_ONLY and has_c:
        out.append(f"kind {kind} has no candidates")
    if kind == "walk-forward" and has_r:
        out.append("kind walk-forward has no rule_cell (the spec stage writes its template next to the parent spec: "
                   "the parent's construction, its fit refitted walk-forward)")
    if kind in SCHEMA_ONLY and has_r:
        out.append(f"kind {kind} has no rule_cell (schema only)")
    if kind not in ("add", "replace") and BLOCKS[kind] not in m:
        out.append(f"kind {kind} needs its block {BLOCKS[kind]}")
        return out
    cands = m.get("candidates") if isinstance(m.get("candidates"), list) else []
    ckinds = [c.get("kind", "add") for c in cands if isinstance(c, dict)]
    if kind in ("add", "field") and any(k != "add" for k in ckinds):
        out.append(f"kind {kind}: every candidate is of kind add")
    if kind == "replace" and "replace" not in ckinds:
        out.append("kind replace: one or more candidates of kind replace")
    check = {"rule": rules_problems, "leverage": leverage_problems, "horizon": horizon_problems,
             "field": field_problems, "role": role_problems, "walk-forward": walk_forward_problems}.get(kind)
    return out + (check(m) if check else [])


def rules_problems(m: dict) -> list[str]:
    rules = m["rules"]
    if not isinstance(rules, list) or not rules:
        return ["rules must be a non-empty list of {name, params?}"]
    out = []
    for k, r in enumerate(rules):
        if not (isinstance(r, dict) and set(r) <= {"name", "params"} and isinstance(r.get("name"), str) and
                NAME_RE.fullmatch(r["name"]) and isinstance(r.get("params", {}), dict)):
            out.append(f"rules[{k}] must be {{name: a rule id, params?: {{...}}}}")
    names = [r.get("name") for r in rules if isinstance(r, dict)]
    if len(set(names)) != len(names):
        out.append("rules: a rule is named twice")
    flags = ((m.get("rule_cell") or {}).get("constants") or {}).get("flags") or {}
    taken = sorted({f for r in names if r in COMPOSITION_FLAGS for ops in COMPOSITION_FLAGS[r].values() for f in ops}
                   & {f for ops in flags.values() if isinstance(ops, dict) for f in ops})
    if taken:
        out.append(f"rule_cell.constants.flags sets {taken}, which the named rules own (review F-8: a registered rule "
                   "is named in rules, never written as raw flags)")
    return out


def leverage_problems(m: dict) -> list[str]:
    lv = m["leverage"]
    if not (isinstance(lv, dict) and set(lv) <= {"value", "name"} and isinstance(lv.get("value"), str) and
            re.fullmatch(r"[0-9]+(\.[0-9]+)?", lv["value"]) and ("name" not in lv or _text(lv["name"]))):
        return ['leverage must be {value: "L" (a decimal string, as nav.leverage), name?}']
    out = [] if m.get("gross_match") == "none" else ['kind leverage: gross_match must be "none" (the change IS the '
                                                       "scale; OR section 6)"]
    if (m.get("rule_cell") or {}).get("constants", {}).get("set", {}).get("nav.leverage") is not None:
        out.append("kind leverage: the value is leverage.value, not rule_cell.constants.set nav.leverage")
    return out


def horizon_problems(m: dict) -> list[str]:
    h = m["horizon"]
    if not (isinstance(h, dict) and set(h) == {"days", "flags"} and type(h["days"]) is int and h["days"] in WINDOWS
            and isinstance(h["flags"], dict) and h["flags"] and set(h["flags"]) <= set(FLAG_SECTIONS) and
            all(isinstance(ops, dict) and ops and all(isinstance(k, str) and k.startswith("--") and
                                                      (type(v) is bool or isinstance(v, str)) for k, v in ops.items())
                for ops in h["flags"].values())):
        return [f"horizon must be {{days: one of {list(WINDOWS)}, flags: {{section ({', '.join(FLAG_SECTIONS)}): "
                "{--opt: value | true | false}}} (non-empty)}"]
    return []


def _caps_problems(block: dict, where: str) -> list[str]:
    out = []
    if "seconds" in block:
        refusal = research_tree.seconds_cap_refusal(f"{where}.seconds", block["seconds"])
        if refusal or type(block["seconds"]) is not int:
            out.append(refusal or f"{where}.seconds must be an integer")
    if "max_rss_mib" in block and not (type(block["max_rss_mib"]) is int and 32 <= block["max_rss_mib"] <= 8192):
        out.append(f"{where}.max_rss_mib must be an integer in [32, 8192] (the bounded runner's range)")
    return out


def _builder_args_problems(args, where: str, reserved_flags: tuple = BUILD_RESERVED) -> list[str]:
    if not _argv(args):
        return [f"{where} must be a list of strings"]
    reserved = sorted({a.split("=", 1)[0] for a in args if a.split("=", 1)[0] in reserved_flags})
    return [f"{where} may not set {reserved} (the build sets them)"] if reserved else []


def field_problems(m: dict) -> list[str]:
    f = m["field"]
    keys = {"rows", "output", "registry", "builder_args", "seconds", "max_rss_mib"}
    if not (isinstance(f, dict) and {"rows", "output"} <= set(f) <= keys and isinstance(f["rows"], list) and
            f["rows"] and all(_text(r) for r in f["rows"]) and len(set(f["rows"])) == len(f["rows"]) and
            _rel(f["output"]) and _rel(f.get("registry", FIELD_REGISTRY))):
        return ["field must be {rows: [registry row names] (distinct, non-empty), output: a root-relative dir, "
                "registry?, builder_args?, seconds?, max_rss_mib?}"]
    out = _builder_args_problems(f.get("builder_args", []), "field.builder_args") + _caps_problems(f, "field")
    if _norm(f["output"]) == _norm(str(m.get("fields", {}).get("dir", ""))):      # review E2 S5: ./x, x/, X
        out.append("field.output is the wave's fields dir: the build writes a new dir")
    if in_code(f["output"]):
        out.append("field.output lies inside the code pathspec: a build output would dirty it")
    return out


def role_problems(m: dict) -> list[str]:
    r = m["role"]
    keys = {"builder_args", "output", "fields_output", "fields_args", "registry", "seconds", "max_rss_mib"}
    if not (isinstance(r, dict) and {"builder_args", "output", "fields_output"} <= set(r) <= keys and
            _rel(r["output"]) and _rel(r["fields_output"]) and r["output"] != r["fields_output"] and
            _rel(r.get("registry", FIELD_REGISTRY))):
        return ["role must be {builder_args: [...], output: DIR, fields_output: another DIR, fields_args?, registry?, "
                "seconds?, max_rss_mib?}"]
    out = _builder_args_problems(r["builder_args"], "role.builder_args", ROLE_RESERVED)
    out += _builder_args_problems(r.get("fields_args", []), "role.fields_args") + _caps_problems(r, "role")
    if in_code(r["output"]) or in_code(r["fields_output"]):
        out.append("role.output and role.fields_output lie outside the code pathspec (build outputs)")
    if not r["builder_args"] or r["builder_args"][0] != "role":
        out.append('role.builder_args start with the builder\'s verb "role" (prepare_recent_research.py role ...)')
    return out


def walk_forward_problems(m: dict) -> list[str]:
    wf = m["walk_forward"]
    ints = ("lag", "step", "min_decisions")
    if not (isinstance(wf, dict) and {"lag", "step"} <= set(wf) <= {"lag", "step", "window", "min_decisions"} and
            all(type(wf[k]) is int and wf[k] > 0 for k in ints if k in wf) and wf["lag"] >= 2 and
            wf.get("min_decisions", 2) >= 2 and wf.get("window", "expanding") in WALK_FORWARD_WINDOWS):
        return ["walk_forward must be {lag: >= 2 (contract K-P9-8: fitted on decisions <= d-2), step: > 0, "
                f"window?: {' | '.join(WALK_FORWARD_WINDOWS)}, min_decisions?: >= 2}}"]
    return []


def walk_forward_flags(wf: dict) -> dict:
    """The fitter flags of a walk-forward cell (K-P9-8, P9 lane D3): the composition exe refits the fit per block of
    ``step`` decisions on the expanding window of decisions <= d - ``lag`` (atx-research-composition fit --walk-forward;
    composition_engine_fit.add_argument). min_decisions only when the block names it (else the fitter's default)."""
    ops = {"--walk-forward": wf.get("window", "expanding"), "--walk-forward-lag": str(wf["lag"]),
           "--walk-forward-step": str(wf["step"])}
    if "min_decisions" in wf:
        ops["--walk-forward-min-decisions"] = str(wf["min_decisions"])
    return {"fit": ops}


def walk_forward_token(wf: dict) -> str:
    """The output token of a walk-forward cell: wf<lag>s<step>[m<min_decisions>]."""
    return f"wf{wf['lag']}s{wf['step']}" + (f"m{wf['min_decisions']}" if "min_decisions" in wf else "")


WALK_FORWARD_OUTPUTS = (("fit", "output"), ("card", "output"), ("ic", "w_output"), ("nav", "output"),
                        ("monitor", "output"))


def walk_forward_set(parent: dict, wf: dict) -> dict:
    """change.set of a walk-forward cell: every output downstream of the refit renamed -wf<lag>s<step>[m<min>], as a
    composition change renames them (research_spec): the fit (the v3 weights), the card and the monitor (they read the
    fit's dir), the weighted IC pass and the NAV, so nothing made from the parent's fit is silently reused. The u pass
    and the marginal read only the parent's inputs and pools, so they resume as the parent's."""
    token = walk_forward_token(wf)
    return {f"{s}.{k}": renamed(parent[s][k], token) for s, k in WALK_FORWARD_OUTPUTS
            if isinstance(parent.get(s), dict) and isinstance(parent[s].get(k), str)}


def walk_forward_parent_problems(parent: dict) -> list[str]:
    """A walk-forward cell refits its parent's fit: the parent must fit under --engine-fit (the per-date weights are
    the C++ composition fit's, and ruling ALCOMB-7: a cell's fit mode is its parent's) without --theme-tsmom,
    --theme-resid or --era (their TRAIN-fitted schedules, orders and pools are not refitted; the fitter refuses
    them)."""
    flags = (parent.get("fit") or {}).get("flags")
    flags = flags if isinstance(flags, list) else []
    norm = [norm_option(t).split("=", 1)[0] if isinstance(t, str) else t for t in flags]
    out = []
    if WALK_FORWARD_PARENT_FIT not in norm:
        out.append("kind walk-forward refits the parent's fit with the C++ composition exe: the parent spec's "
                   f"fit.flags must carry {WALK_FORWARD_PARENT_FIT} (ruling ALCOMB-7: the cell's fit mode is its "
                   "parent's); name a parent fitted under it")
    bad = sorted({f for f in WALK_FORWARD_REFUSED if f in norm})
    if bad:
        out.append(f"kind walk-forward is refused on a parent fitted with {bad} (their TRAIN-fitted schedules, orders "
                   "and pools are not refitted per block; P10)")
    return out


def walk_forward_template(parent_spec: str, wave: str, wf: dict) -> tuple[str, dict]:
    """(path, document) of the template a walk-forward wave writes next to the parent spec (never written: the spec
    stage writes the cell): parent = the parent's file, change.set and change.flags filled in by the spec stage
    (merged_constants with walk_forward_set / walk_forward_flags)."""
    p = Path(parent_spec)
    label = f"{p.stem}-{walk_forward_token(wf)}-{wave}"
    doc = {"schema": "atx.research-cycle-template/v1", "name": label,
           "description": f"wave {wave} (kind walk-forward, contract K-P9-9 / K-P9-8): {p.name}'s fit refitted "
                          f"walk-forward ({wf.get('window', 'expanding')}, lag {wf['lag']}, step {wf['step']}"
                          + (f", min_decisions {wf['min_decisions']}" if "min_decisions" in wf else "") +
                          "), weights schema atx.dsl-composition-weights/v3; written by the wave's spec stage",
           "parent": p.name, "change": {"set": {}}}
    return (p.parent / f"{label}.json").as_posix(), doc


# ------------------------------------------------------------------ JSON Schema (the subset the exes print)
def schema_problems(value, schema: dict, where: str) -> list[str]:
    """Problems of ``value`` under ``schema`` (SCHEMA_KEYWORDS; an unknown keyword is a problem, never ignored)."""
    if not isinstance(schema, dict):
        return [f"{where}: the schema is not an object"]
    unknown = sorted(set(schema) - SCHEMA_KEYWORDS)
    if unknown:
        return [f"{where}: schema keyword(s) {unknown} not supported (refused, not ignored)"]
    out = []
    types = {"object": dict, "array": list, "string": str, "boolean": bool}
    t = schema.get("type")
    if t is not None and (not isinstance(t, str) or t not in ("integer", "number", *types)):   # review E2 S3
        return [f"{where}: schema type {t!r} not supported (refused, not ignored)"]
    for key, kinds in (("properties", dict), ("required", list), ("items", dict), ("enum", list),
                       ("additionalProperties", (bool, dict))):
        if key in schema and not isinstance(schema[key], kinds):
            return [f"{where}: schema {key} {schema[key]!r} is not a JSON-Schema {key} (refused, not ignored)"]
    if type(value) is float and not math.isfinite(value):      # review E2 S2: json.loads reads NaN / Infinity, and
        return [f"{where}: {value!r} is not a finite number (NaN and Infinity are no JSON numbers; a range "
                "check cannot bound them)"]                     # NaN passes every minimum / maximum comparison
    if t is not None:
        ok = (type(value) is int if t == "integer" else
              type(value) in (int, float) if t == "number" else
              isinstance(value, types[t]) and not (t != "boolean" and type(value) is bool) if t in types else False)
        if not ok:
            return [f"{where}: {value!r} is not of type {t}"]
    if "const" in schema and value != schema["const"]:
        out.append(f"{where}: must be {schema['const']!r}")
    if "enum" in schema and value not in schema["enum"]:
        out.append(f"{where}: must be one of {schema['enum']}")
    if type(value) in (int, float):
        for key, bad in (("minimum", lambda v, b: v < b), ("maximum", lambda v, b: v > b),
                         ("exclusiveMinimum", lambda v, b: v <= b), ("exclusiveMaximum", lambda v, b: v >= b)):
            if key in schema and bad(value, schema[key]):
                out.append(f"{where}: {value} violates {key} {schema[key]}")
    if isinstance(value, str) and "pattern" in schema and not re.search(schema["pattern"], value):
        out.append(f"{where}: {value!r} does not match {schema['pattern']}")
    if isinstance(value, list):
        if len(value) < schema.get("minItems", 0) or len(value) > schema.get("maxItems", len(value)):
            out.append(f"{where}: {len(value)} items (allowed {schema.get('minItems', 0)}.."
                       f"{schema.get('maxItems', 'any')})")
        if "items" in schema:
            for k, item in enumerate(value):
                out += schema_problems(item, schema["items"], f"{where}[{k}]")
    if isinstance(value, dict):
        props = schema.get("properties", {})
        missing = [k for k in schema.get("required", []) if k not in value]
        if missing:
            out.append(f"{where}: missing {missing}")
        if len(value) < schema.get("minProperties", 0):
            out.append(f"{where}: fewer than {schema['minProperties']} properties")
        extra = schema.get("additionalProperties", True)
        for k, v in value.items():
            if k in props:
                out += schema_problems(v, props[k], f"{where}.{k}")
            elif extra is False:
                out.append(f"{where}: unknown key {k!r}")
            elif isinstance(extra, dict):
                out += schema_problems(v, extra, f"{where}.{k}")
    return out


# ------------------------------------------------------------------ the exes' rule tables
def listing_argv(exe: str, nav: bool) -> list[str]:
    """The argv that prints an exe's rule envelope: the IC exe's ``--list-rules --json`` (K-P9-6), the NAV verb's
    ``nav --list-rules --json`` (K-P9-7)."""
    return [exe, *(["nav"] if nav else []), *LIST_RULES]


def parse_listing(text: str, schema: str) -> dict | None:
    """The envelope {schema, capabilities, rules} an exe printed, or None (not JSON, or another schema)."""
    try:
        doc = json.loads(text)
    except ValueError:
        return None
    if not (isinstance(doc, dict) and doc.get("schema") == schema and isinstance(doc.get("rules"), list)):
        return None
    return doc


def rule_rows(tables: dict) -> dict:
    """{rule id: (table, row)} of the IC and NAV tables ({"ic": envelope | None, "nav": ...}); an id in both is
    KindError (the name would not say which exe decides it)."""
    out: dict = {}
    for table in ("ic", "nav"):
        for row in (tables.get(table) or {}).get("rules") or []:
            if not (isinstance(row, dict) and isinstance(row.get("id"), str)):
                continue
            if row["id"] in out:
                raise KindError(f"rule {row['id']} is listed by both the IC and the NAV exe")
            out[row["id"]] = (table, row)
    return out


def checked_params(name: str, params: dict, row: dict) -> dict:
    """The object the row's params_schema judges: ``params`` plus ``rule: name`` when the schema pins its rule key to
    the row's own id (the K-P9-6 blocks name their rule) and params leave it out."""
    schema = row.get("params_schema") or {}
    rule = ((schema.get("properties") or {}).get("rule") or {}) if isinstance(schema, dict) else {}
    return dict(params, rule=name) if rule.get("const") == name and "rule" not in params else dict(params)


def _value(v) -> str:
    return v if isinstance(v, str) else json.dumps(v)


def nav_flags(name: str, params: dict, row: dict) -> dict:
    """The NAV flags of a K-P9-7 row: each param's x-flag with its value, then the rule's x-flag (``--flag VALUE``
    a constant, ``--flag P`` a placeholder some param fills, ``--flag`` alone one a param sets)."""
    props = (row.get("params_schema") or {}).get("properties") or {}
    ops = {}
    for key, value in params.items():
        flag = (props.get(key) or {}).get("x-flag")
        if not isinstance(flag, str):
            raise KindError(f"rule {name}: param {key} names no x-flag in the NAV exe's table")
        ops[flag] = _value(value)
    tokens = str(row.get("x-flag") or "").split()
    if not tokens:
        raise KindError(f"rule {name}: the NAV exe's row has no x-flag")
    if tokens[0] not in ops:
        if len(tokens) != 2 or PLACEHOLDER_RE.fullmatch(tokens[1]):
            raise KindError(f"rule {name}: its flag {row['x-flag']!r} needs a param value (set it in params)")
        ops[tokens[0]] = tokens[1]
    return {"nav": ops}


def rule_flags(rules: list[dict], tables: dict) -> tuple[dict, list[str]]:
    """({section: {flag: value}} of every named rule, the problems): each name found in exactly one table, its params
    valid under the row's params_schema, its flags derived (COMPOSITION_FLAGS, nav_flags); two rules setting one flag
    to two values is a problem."""
    try:
        rows = rule_rows(tables)
    except KindError as exc:
        return {}, [str(exc)]
    flags: dict = {}
    out = []
    for r in rules:
        name, params = r["name"], r.get("params") or {}
        if name not in rows:
            have = {t: [x.get("id") for x in (tables.get(t) or {}).get("rules") or []] for t in ("ic", "nav")}
            out.append(f"rule {name} is in neither exe's rule table (IC {have['ic']}, NAV {have['nav']})")
            continue
        table, row = rows[name]
        out += schema_problems(checked_params(name, params, row), row.get("params_schema") or {}, f"rules.{name}")
        try:
            got = nav_flags(name, params, row) if table == "nav" else COMPOSITION_FLAGS.get(name)
        except KindError as exc:
            out.append(str(exc))
            continue
        if got is None:
            out.append(f"rule {name}: the composition row has no fitter flag in wave_kinds.COMPOSITION_FLAGS")
            continue
        for section, ops in got.items():
            for flag, value in ops.items():
                have = flags.setdefault(section, {})
                if flag in have and have[flag] != value:
                    out.append(f"rules: {section} {flag} set to {have[flag]!r} and {value!r}")
                have[flag] = value
    return flags, out


def nav_rule_split(derived: dict) -> tuple[dict, dict]:
    """Ruling C3-13: (sets, flags) of a rule wave's derived flags (rule_flags) for its rule cell. A named NAV row whose
    x-flag is ``--rule <id>`` (NAV_RULE_FLAG, an aim rule) SETS the cell's nav.rule (NAV_RULE_KEY) to <id>, replacing
    the parent's: the cycle puts ``--rule nav.rule`` on the NAV argv itself (cycle/phases.py nav_step), so the flag in
    nav.flags would be a second --rule the NAV exe refuses (root M2j item 8, the C3-5 probe). Only the row's param
    x-flags stay in the flags (the append route). Without a --rule entry ``derived`` comes back as given: every other
    row kind (--rate, --target-rule, --cost-law, a composition row's flags) keeps the append route."""
    nav = dict((derived or {}).get("nav") or {})
    if NAV_RULE_FLAG not in nav:
        return {}, derived
    value = nav.pop(NAV_RULE_FLAG)
    flags = {s: ops for s, ops in derived.items() if s != "nav"}
    if nav:
        flags["nav"] = nav
    return {NAV_RULE_KEY: value}, flags


def unmapped_ic_rules(tables: dict) -> list[str]:
    """The ids of the IC exe's composition rows that COMPOSITION_FLAGS does not map (ruling E2-8, review E2 N1): their
    fitter flags are unknown here, so a raw one could not be refused; the wave stops on them by name (a missed MR-4)."""
    rows = (tables.get("ic") or {}).get("rules") or []
    return sorted({r["id"] for r in rows if isinstance(r, dict) and isinstance(r.get("id"), str)} -
                  set(COMPOSITION_FLAGS))


def table_flags(tables: dict) -> dict:
    """{section: {flag}} every rule of the exes' tables turns on, named by the manifest or not: each composition
    row's fitter (and NAV) flags (COMPOSITION_FLAGS), each NAV row's x-flag (its first token), its params' x-flags
    (K-P9-7) and its legacy spellings (x-flag-aliases, ruling C3-3: an alias is the owned flag it stands for), and
    the flags a v2 kind derives itself (KIND_FLAGS: walk-forward's, P9 lane D3)."""
    out: dict = {}
    for row in (tables.get("ic") or {}).get("rules") or []:
        rid = row.get("id") if isinstance(row, dict) else None
        for section, ops in ((COMPOSITION_FLAGS.get(rid) or {}) if isinstance(rid, str) else {}).items():
            out.setdefault(section, set()).update(ops)
    for row in (tables.get("nav") or {}).get("rules") or []:
        if not isinstance(row, dict):
            continue
        tokens = str(row.get("x-flag") or "").split()
        props = (row.get("params_schema") or {}).get("properties") if isinstance(row.get("params_schema"), dict) \
            else None
        flags = tokens[:1] + [p.get("x-flag") for p in (props or {}).values() if isinstance(p, dict)]
        out.setdefault("nav", set()).update(f for f in flags if isinstance(f, str) and f)
    out.setdefault("nav", set()).update(flag_aliases(tables))
    for section, flags in KIND_FLAGS.items():
        out.setdefault(section, set()).update(flags)
    return out


def flag_aliases(tables: dict) -> dict:
    """{alias: NAV row id} of every legacy spelling a K-P9-7 row lists in its ``x-flag-aliases`` (ruling C3-3; C3's
    cost-sqrt-v2 lists --cost-v2). Ruling C3-3: the E2-8 guard reads each alias as the owned flag of its row, so in a
    v2 wave an alias appears only as inherited unchanged from the lineage parent's cell of record; adding, removing or
    changing it needs a rules entry naming the row (rule_flag_guard). Malformed fields: alias_problems."""
    out: dict = {}
    for row in (tables.get("nav") or {}).get("rules") or []:
        if isinstance(row, dict) and isinstance(row.get("id"), str) and isinstance(row.get(ALIASES_KEY), list):
            out.update({a: row["id"] for a in row[ALIASES_KEY] if _alias(a)})
    return out


def _alias(value) -> bool:
    return isinstance(value, str) and value.startswith("--") and len(value) > 2 and value == norm_option(value) and \
        "=" not in value and " " not in value


def alias_problems(tables: dict) -> list[str]:
    """Ruling C3-3, fail closed: a NAV row whose x-flag-aliases is not a non-empty list of distinct long options, whose
    x-flag is no ``--flag VALUE`` pair with a constant value (an alias stands for that whole pair, a bare switch, as
    C3's --cost-v2 for --cost-law cost-sqrt-v2; wave_stage_kinds.reads), that names an alias another row also lists,
    or a flag some rule owns as its own (a row's x-flag or param x-flag, a composition rule's NAV flag: naming the row
    would exempt another rule's flag) could not be guarded, so the wave stops on it by name."""
    out, seen = [], {}
    rows = [r for r in (tables.get("nav") or {}).get("rules") or [] if isinstance(r, dict)]
    flags = {f for ops in COMPOSITION_FLAGS.values() for f in ops.get("nav", {})}
    for row in rows:
        props = (row.get("params_schema") or {}).get("properties") if isinstance(row.get("params_schema"), dict) \
            else None
        flags.update(str(row.get("x-flag") or "").split()[:1])
        flags.update(p.get("x-flag") for p in (props or {}).values() if isinstance(p, dict))
    for row in rows:
        if ALIASES_KEY not in row:
            continue
        aliases = row[ALIASES_KEY]
        if not (isinstance(aliases, list) and aliases and all(_alias(a) for a in aliases) and
                len(set(aliases)) == len(aliases)):
            out.append(f"the NAV exe's row {row.get('id')!r} lists {ALIASES_KEY} {aliases!r}, not a non-empty list of "
                       "distinct long options: its legacy spelling cannot be guarded (ruling C3-3)")
            continue
        tokens = row.get("x-flag").split() if isinstance(row.get("x-flag"), str) else []
        if len(tokens) != 2 or PLACEHOLDER_RE.fullmatch(tokens[1]):
            out.append(f"the NAV exe's row {row.get('id')!r} lists {ALIASES_KEY} but its x-flag {row.get('x-flag')!r} "
                       "is no '--flag VALUE' pair with a constant value: an alias stands for the whole pair (a bare "
                       "switch), so the guard cannot read one that takes a value (ruling C3-3)")
            continue
        for a in aliases:
            if a in seen and seen[a] != row.get("id"):
                out.append(f"the NAV exe's rows {seen[a]!r} and {row.get('id')!r} both list the alias {a} (ruling "
                           "C3-3: an alias stands for one row's flag)")
            if a in flags:
                out.append(f"the NAV exe's row {row.get('id')!r} lists the alias {a}, which a rule of the tables owns "
                           "as its own flag (ruling C3-3: an alias is a legacy spelling of its row's flag only)")
            seen[a] = row.get("id")
    return out


def owned_flags(tables: dict, derived: dict) -> dict:
    """{section: {flag}} a rule owns: table_flags plus the named rules' derived flags."""
    owned = table_flags(tables)
    for section, ops in (derived or {}).items():
        owned.setdefault(section, set()).update(ops)
    return owned


def norm_option(token):
    """An option token as the raw-rule-flag guard matches it (ruling E2-10, review E2 S2): a leading single "-" made
    "--" and the option name (before "=") case-folded, so "-theme-erc" and "--Vol-Target" count as the owned
    "--theme-erc" / "--vol-target" at preflight (the exes and the fitter would only refuse them at run); any other
    token (a value, "--", "-") unchanged."""
    if not isinstance(token, str) or len(token) < 2 or not token.startswith("-") or token == "--":
        return token
    head, eq, value = (token if token.startswith("--") else "-" + token).partition("=")
    return head.casefold() + eq + value


def match_flag_reads(token) -> list[str]:
    """The MATCH_FLAGS ``token`` reads as (ruling E2-14, review E2 R3a): the token as norm_option reads it (a single
    dash, another case), ``=value`` stripped, equal to one or a proper prefix of one (an abbreviation); [] for a value
    or any other option."""
    norm = norm_option(token)
    head = norm.split("=", 1)[0] if isinstance(norm, str) else ""
    return [f for f in MATCH_FLAGS if head.startswith("--") and len(head) > 2 and f.startswith(head)]


def abbreviated(token: str, flags) -> list[str]:
    """The flags of ``flags`` that ``token`` (``=value`` stripped, norm_option) is a proper prefix of: what argparse's
    allow_abbrev reads it as (ruling E2-8: such a token is refused as an abbreviation)."""
    head = norm_option(token).split("=", 1)[0]
    return sorted(f for f in flags if len(head) > 2 and head.startswith("--") and f != head and f.startswith(head))


def raw_rule_flags(given: dict, derived: dict, tables: dict, exempt: frozenset = frozenset()) -> list[str]:
    """The "section token" entries of ``given`` (an option map: rule_cell.constants.flags, a template's change.flags,
    horizon.flags) that a rule owns: the token with any ``=value`` stripped is one the named rules derive
    (``derived``) or one any rule of the exes' tables turns on (table_flags), named or not; or the token is a proper
    prefix of such a flag, an argparse abbreviation, entered as "section token (abbreviates [...])" (review E2 R2 /
    R2b, ruling E2-8, F-8: a rule is named in rules and its params checked by its params_schema; a raw flag would run
    it unchecked). ``exempt``: the NAV aliases whose row the manifest names (ruling C3-3: the named row may add,
    remove or change its own legacy spelling); they are not owned for this call."""
    owned = owned_flags(tables, derived)
    owned["nav"] = owned.get("nav", set()) - set(exempt)
    out = []
    for s, ops in (given or {}).items():
        for f in (ops if isinstance(ops, dict) else {}):
            if not isinstance(f, str):
                continue
            if norm_option(f).split("=", 1)[0] in owned.get(s, set()):
                out.append(f"{s} {f}")
            elif abbreviated(f, owned.get(s, set())):
                out.append(f"{s} {f} (abbreviates {abbreviated(f, owned.get(s, set()))})")
    return sorted(out)


# ------------------------------------------------------------------ the cell's constants
def leverage_bounds(tables: dict) -> tuple[float, float] | None:
    """[minimum, maximum] of the NAV exe's fixed-v1 aim_leverage (K-P9-7), None when it lists no such row."""
    for row in (tables.get("nav") or {}).get("rules") or []:
        if isinstance(row, dict) and row.get("id") == "fixed-v1":
            prop = ((row.get("params_schema") or {}).get("properties") or {}).get("aim_leverage") or {}
            if "minimum" in prop and "maximum" in prop:
                return float(prop["minimum"]), float(prop["maximum"])
    return None


def renamed(name: str, token: str) -> str:
    return f"{name}-{token}"


def horizon_set(parent: dict, days: int) -> dict:
    """change.set of a horizon cell: every HORIZON_OUTPUTS name the parent has, renamed -h<days>."""
    return {f"{s}.{k}": renamed(parent[s][k], f"h{days}") for s, k in HORIZON_OUTPUTS
            if isinstance(parent.get(s), dict) and isinstance(parent[s].get(k), str)}


def leverage_set(parent: dict, value: str) -> dict:
    """change.set of a leverage cell: nav.leverage and the NAV output with the leverage token (wave_steps)."""
    import wave_steps  # noqa: PLC0415  (wave_steps imports wave_manifest, which imports this module)
    return {"nav.leverage": value,
            "nav.output": wave_steps.renamed_output(parent["nav"]["output"], str(parent["nav"].get("leverage")), value)}


def merged_constants(rc: dict, sets: dict, flags: dict) -> dict:
    """rule_cell with ``sets`` over constants.set and ``flags`` over constants.flags (the kind's derived values win)."""
    const = dict(rc.get("constants") or {})
    if sets:
        const["set"] = dict(const.get("set") or {}, **sets)
    if flags:
        have = {s: dict(o) for s, o in (const.get("flags") or {}).items()}
        for s, ops in flags.items():
            have[s] = dict(have.get(s) or {}, **ops)
        const["flags"] = have
    return dict(rc, constants=const)


def leverage_template(parent_spec: str, wave: str, value: str, name: str | None) -> tuple[str, dict]:
    """(path, document) of the NAV-only template a leverage wave without a rule_cell writes next to the parent spec:
    parent = the parent's file, change.set filled in by the spec stage (merged_constants)."""
    p = Path(parent_spec)
    label = name or f"{p.stem}-L{value}-{wave}"
    doc = {"schema": "atx.research-cycle-template/v1", "name": label,
           "description": f"wave {wave} (kind leverage, contract K-P9-9): {p.name} at nav.leverage {value}; NAV-only "
                          "(every other output the parent's); written by the wave's spec stage",
           "parent": p.name, "change": {"set": {}}}
    return (p.parent / f"{label}.json").as_posix(), doc
