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
  walk-forward   walk_forward: {lag, step, ...}              schema only until lane D3 wires the fold driver: the plan
                                                             prints it, preflight refuses to run it

Rules (``rules``; K-P9-6, K-P9-7, ruling D1-PSCHEMA). Each name is a row of the IC exe's ``--list-rules --json``
table (atx.composition-rules/v1) or of the NAV exe's ``nav --list-rules --json`` (atx.nav-rules/v1); a name in both,
or in neither, is refused. Its params (default {}) must be valid under the row's params_schema (SCHEMA_KEYWORDS: the
JSON Schema subset the exes print), with the row's own name as its ``rule`` key when the schema pins one. The flags:
a NAV row's from its x-flag keys (the rule's, then each param's), a composition row's from COMPOSITION_FLAGS. The
preflight stage reads both tables (their SHA-256 in its receipt), validates and derives; the spec stage writes the
flags into the cell. The exe capabilities the cycle needs come from the same envelope (cycle/phases.exe_capabilities).
"""
from __future__ import annotations

import json
from pathlib import Path
import re

import research_tree

SCHEMA_V2 = "atx.research-wave/v2"
KINDS = ("add", "replace", "rule", "field", "role", "horizon", "walk-forward", "leverage")
BLOCKS = {"rule": "rules", "leverage": "leverage", "horizon": "horizon", "field": "field", "role": "role",
          "walk-forward": "walk_forward"}            # kind -> its block key (add / replace: candidates)
LIBRARY_KINDS = ("add", "replace", "field")         # candidates
TEMPLATE_KINDS = ("rule", "horizon", "role")        # rule_cell required (leverage: optional)
SCHEMA_ONLY = ("walk-forward",)                     # lane D3 wires it (wave 3)
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
}
SCHEMA_KEYWORDS = {"type", "const", "enum", "required", "properties", "additionalProperties", "items", "minItems",
                   "maxItems", "minProperties", "pattern", "minimum", "maximum", "exclusiveMinimum",
                   "exclusiveMaximum", "default", "x-flag", "description"}
NAME_RE = re.compile(r"[a-z0-9][a-z0-9.-]{0,63}")
PLACEHOLDER_RE = re.compile(r"[A-Z][A-Z0-9_]*")     # "--risk-target S" / "SIGMA": a param's value, not a constant


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
    if kind in ("leverage",) + SCHEMA_ONLY and has_c:
        out.append(f"kind {kind} has no candidates")
    if kind in SCHEMA_ONLY and has_r:
        out.append(f"kind {kind} has no rule_cell (schema only until lane D3)")
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
    if not (isinstance(wf, dict) and {"lag", "step"} <= set(wf) <= {"lag", "step", "window", "min_decisions"} and
            all(type(v) is int and v > 0 for v in wf.values()) and wf["lag"] >= 2 and
            wf.get("window", "expanding") in ("expanding",)):
        return ["walk_forward must be {lag: >= 2 (contract K-P9-8: fitted on decisions <= d-2), step: > 0, "
                "min_decisions?: > 0} (schema only until lane D3)"]
    return []


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


def table_flags(tables: dict) -> dict:
    """{section: {flag}} every rule of the exes' tables turns on, named by the manifest or not: each composition
    row's fitter (and NAV) flags (COMPOSITION_FLAGS), each NAV row's x-flag (its first token) and its params'
    x-flags (K-P9-7)."""
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
    return out


def raw_rule_flags(given: dict, derived: dict, tables: dict) -> list[str]:
    """The "section flag" entries of ``given`` (rule_cell.constants.flags, a template's change.flags) that a rule
    owns: one the named rules derive (``derived``) or one any rule of the exes' tables turns on (table_flags), named
    or not (review E2 R2, F-8: a rule is named in rules and its params checked by its params_schema; a raw flag would
    run it unchecked)."""
    owned = table_flags(tables)
    for section, ops in (derived or {}).items():
        owned.setdefault(section, set()).update(ops)
    return sorted(f"{s} {f}" for s, ops in (given or {}).items() if isinstance(ops, dict)
                  for f in ops if f in owned.get(s, set()))


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
