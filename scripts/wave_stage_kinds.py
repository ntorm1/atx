"""The wave stages' part of the v2 kinds (wave_kinds.py, contract K-P9-9, P9 lane E2): the exes' rule tables read at
preflight, the field and role builds of the register stage, and the rule cell's derived constants of the spec stage.
A v1 manifest never reaches a function here that changes anything (``kind`` is None): its argv, receipts and bytes
are those of before.

  preflight  every kind: both rule tables (the IC exe's --list-rules --json, the NAV exe's nav --list-rules --json),
             their stdout SHA-256 recorded, and ruling E2-8's raw-rule-flag guard (rule_flag_guard: no flag a rule
             owns reaches the cell's resolved flag lists but through rules, by any route or spelling; every IC rule
             mapped in COMPOSITION_FLAGS; ruling E2-10: nav.leverage set or changed only by kind leverage; ruling
             E2-14: no --calibrate-gross / --calibrate-tolerance but the pm6-6-engine match stage's; ruling E2-15: the
             ref section only from the template's own pinned block, its resolved side judged as the cell's). Kind
             rule: every named rule validated and its flags derived (wave_kinds.rule_flags). Kind leverage: the value
             inside the NAV exe's fixed-v1 aim_leverage range (refused when the table lists none) and at most 2.0.
             Kind walk-forward (P9 lane D3): its fitter flags derived (owned by the kind), the parent fitted under
             --engine-fit without --theme-tsmom / --theme-resid / --era. Ruling C3-3: a NAV row's legacy spelling
             (x-flag-aliases) is owned like its x-flag; ruling E2-17: inputs.* only by the kinds and keys of
             wave_kinds.KIND_INPUTS (inputs_refusal)
  register   kind field: the fields builder on the parent's role (prepare_research_fields.py --registry R --fields
             <the wave's fields + the rows> --reuse <the wave's fields dir>) into field.output under the bounded
             runner; kind role: the role builder (prepare_recent_research.py ... --out role.output) then the fields
             builder on the new role (the wave's field names) into role.fields_output
  spec       the rule cell's constants: rule -> the derived flags; leverage -> nav.leverage and the renamed NAV
             output (a NAV-only template next to the parent spec when the manifest has no rule_cell); horizon -> its
             flags and every output renamed -h<days>; role -> inputs.role and the fields dir of the build;
             walk-forward -> its fitter flags and the fit, w and NAV outputs renamed -wf<lag>s<step> (the template
             next to the parent spec)
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
    """({kind, tables, flags?, leverage?}, problems) of a v2 manifest on its parent's resolved ``spec``; ({}, []) for
    v1. Every v2 kind reads both rule tables: ruling E2-8's raw-rule-flag guard (rule_flag_guard) needs them."""
    k, m = kind(w), w.manifest
    if k is None:
        return {}, []
    out: dict = {"kind": k}
    problems: list[str] = []
    if k in WK.SCHEMA_ONLY:
        problems.append(f"kind {k} is schema only until lane D3 wires it: the wave plans, it does not run")
    tables: dict = {}
    shas: dict = {}
    for table, nav in (("ic", False), ("nav", True)):
        tables[table], shas[table] = run_listing(w, spec, nav)
    out["tables"] = shas
    flags: dict = {}
    if k == "rule":
        flags, more = WK.rule_flags(m["rules"], tables)
        problems += more
        out["flags"] = flags
    elif k == "walk-forward":                       # P9 lane D3 (K-P9-8): the kind's own fitter flags, owned by it
        flags = WK.walk_forward_flags(m["walk_forward"])
        problems += WK.walk_forward_parent_problems(spec)
        out["walk_forward"] = dict(m["walk_forward"])
        out["flags"] = flags
    elif k == "leverage":                           # ruling E2-10: range-checked against the table, at most 2.0
        bounds, value = WK.leverage_bounds(tables), float(m["leverage"]["value"])
        if bounds is None:
            problems.append("the NAV exe's rule table lists no fixed-v1 aim_leverage range: leverage.value "
                            f"{m['leverage']['value']} cannot be range-checked (ruling E2-10)")
        elif not bounds[0] <= value <= bounds[1]:
            problems.append(f"leverage.value {m['leverage']['value']} lies outside the NAV exe's fixed-v1 "
                            f"aim_leverage range {list(bounds)}")
        if value > WK.LEVERAGE_MAX:
            problems.append(f"leverage.value {m['leverage']['value']} is above {WK.LEVERAGE_MAX}, the P9 leverage cap "
                            "(ruling E2-10, OD-P9-3)")
        out["leverage"] = {"value": m["leverage"]["value"], "bounds": list(bounds) if bounds else None}
    problems += rule_flag_guard(w, spec, tables, flags)
    return out, problems


# ------------------------------------------------------------------ preflight: ruling E2-8, the raw-rule-flag guard
FLAG_MAP_ROUTES = ("rule_cell.constants.flags", "rule_cell.template change.flags", "horizon.flags")
SET_ROUTES = ("rule_cell.constants.set", "rule_cell.template change.set", "rule_cell.template change.unset")


def kind_constants(w: Wave, parent: dict, derived: dict) -> tuple[dict, dict]:
    """(sets, flags) the kind derives for its rule cell on the ``parent`` spec: rule -> the named rules' flags
    (``derived``); leverage -> nav.leverage and the renamed NAV output; horizon -> its outputs renamed and its flags."""
    k, m = kind(w), w.manifest
    if k == "rule":
        return {}, derived
    if k == "leverage":
        return WK.leverage_set(parent, m["leverage"]["value"]), {}
    if k == "horizon":
        return WK.horizon_set(parent, m["horizon"]["days"]), m["horizon"]["flags"]
    if k == "walk-forward":                         # P9 lane D3: the refit's outputs renamed, its fitter flags
        return WK.walk_forward_set(parent, m["walk_forward"]), WK.walk_forward_flags(m["walk_forward"])
    return {}, {}


def raw_routes(w: Wave) -> tuple[dict, dict]:
    """({route: option map}, {route: {dotted key: the value it sets, or "(unset)"}}) of the manifest's and the
    template's raw inputs to the rule cell (FLAG_MAP_ROUTES, SET_ROUTES); empty for a library kind (add-alpha copies
    the parent's flag lists and nav: no route can set one) and a schema-only kind."""
    k, m = kind(w), w.manifest
    if k in WK.LIBRARY_KINDS or k in WK.SCHEMA_ONLY:
        return {}, {}
    rc, template, _ = base_rule_cell(w)
    const, change = rc.get("constants") or {}, (template or {}).get("change") or {}
    maps = {"rule_cell.constants.flags": const.get("flags"), "rule_cell.template change.flags": change.get("flags"),
            "horizon.flags": (m.get("horizon") or {}).get("flags") if k == "horizon" else None}
    unset_keys = change.get("unset")         # review E2 S4: research_spec.resolve iterates a dict-typed unset by key
    unset = list(unset_keys) if isinstance(unset_keys, (list, dict)) else []
    sets = {"rule_cell.constants.set": const.get("set") if isinstance(const.get("set"), dict) else {},
            "rule_cell.template change.set": change.get("set") if isinstance(change.get("set"), dict) else {},
            "rule_cell.template change.unset": {x: "(unset)" for x in unset if isinstance(x, str)}}
    return ({r: v for r, v in maps.items() if isinstance(v, dict) and v},
            {r: {x: val for x, val in v.items() if isinstance(x, str)} for r, v in sets.items() if v})


def flag_lists(spec: dict, section: str) -> dict:
    """{label: argv list} of a resolved spec's ``section``: <section>.flags, and for ic also the w pass's list
    (ic.w_flags applied to ic.flags, as cycle/phases runs it)."""
    sec: dict = spec[section] if isinstance(spec.get(section), dict) else {}
    flags: list = sec["flags"] if isinstance(sec.get("flags"), list) else []
    out = {f"{section}.flags": flags}
    if section == "ic":
        out["ic.w_flags"] = research_spec.apply_flags(flags, sec.get("w_flags") or {}, "ic.w_flags")
    return out


def option_tokens(flags: list, option: str) -> list[str]:
    """The tokens of ``flags`` (as written) argparse reads as ``option`` (cycle.spec.argparse_values: exact,
    ``=value``, prefix), each matched as WK.norm_option reads it (a single dash, another case: ruling E2-10)."""
    norm = [(t, WK.norm_option(t)) for t in flags]
    return [t for t, n in norm if isinstance(n, str) and n.startswith("--") and len(n) > 2 and
            option[2:].startswith(n[2:].split("=", 1)[0])]


def occurrences(flags: list, option: str) -> list:
    """argparse_values of ``option`` in ``flags`` with every token read as WK.norm_option reads it."""
    return RC.argparse_values([WK.norm_option(t) for t in flags], option)


def map_values(value) -> list:
    """The values an option map entry can put on a flag list: the value, or each of a parent-value map's."""
    return list(value.values()) if isinstance(value, dict) else [value]


def touching(section: str, option: str, maps: dict, sets: dict) -> list[str]:
    """The raw routes that can have changed ``option`` in ``section``'s flag lists: an option map entry whose key or
    value is it (or a prefix of it; review E2 S3: "--cadence --vol-target" puts it in a value slot; a parent-value
    map's values too), a set / unset key replacing the section, its flags or ic.w_flags."""
    out = [f"{r} {section} {key} {value}" if option_tokens(map_values(value), option) else f"{r} {section} {key}"
           for r, ops in maps.items() if isinstance(ops.get(section), dict) for key, value in ops[section].items()
           if option_tokens([key], option) or option_tokens(map_values(value), option)]
    lists = (f"{section}.flags",) + (("ic.w_flags",) if section == "ic" else ())
    return out + [f"{r} {key}" for r, ks in sets.items() for key in ks
                  if key == section or key in lists or any(key.startswith(k + ".") for k in lists)]


def rule_flag_guard(w: Wave, parent: dict, tables: dict, derived: dict) -> list[str]:
    """Ruling E2-8 (review E2 R2 / R2b, N1, N2): the refusals of a v2 wave whose cell would run a rule's flag that
    rules did not name, for every v2 kind, before register. (1) The IC table lists no composition row
    COMPOSITION_FLAGS does not map (WK.unmapped_ic_rules), and both tables were read. (2) No option map
    (FLAG_MAP_ROUTES) names an owned flag, ``=value`` stripped or abbreviated (WK.raw_rule_flags): the derived flags
    would otherwise silently override it. (3) The rule cell, resolved as the spec stage writes it (the kind's
    constants, constants.set, the template's change.set / unset and flag maps, research_spec.resolve), gives every
    owned option of its flag lists the occurrences (argparse_values: exact, ``=value``, abbreviations) of the
    ``parent``'s lists with only the derived flags applied; a difference is refused with the route that made it.
    Tokens are read as WK.norm_option reads them (a single dash, another case: ruling E2-10). (4) Ruling E2-10: the
    cell's nav.leverage (leverage_refusal). (5) Ruling E2-14 (review R3a): no option map and no flag list carries the
    gross-match stage's own flags (match_flag_map_hits, read before resolving; match_flag_list_hits; one refusal,
    match_flag_refusal). (6) Ruling E2-15: no route but the template's own pinned block sets the
    ref section (ref_refusal), and its resolved side (its own flags in (3), its own leverage in (4), its flags in (5))
    is judged as the cell's. A library kind's cell is add-alpha's copy of the parent's flag lists and nav, so (3), (4)
    and (6) have nothing to compare there; (5) reads the parent's lists, which that cell inherits."""
    problems = [f"the {t.upper()} exe's rule table could not be read: the raw-rule-flag guard needs both tables "
                "(ruling E2-8)" for t in ("ic", "nav") if tables.get(t) is None]
    unmapped = WK.unmapped_ic_rules(tables)
    if unmapped:
        problems.append(f"the IC exe's rule table lists {unmapped}, which wave_kinds.COMPOSITION_FLAGS does not map: "
                        "their fitter flags cannot be guarded (ruling E2-8): add their rows (e.g. MR-4) first")
    problems += WK.alias_problems(tables)           # ruling C3-3: an alias the guard cannot read stops the wave
    maps, sets = raw_routes(w)
    owned = WK.owned_flags(tables, derived)
    exempt = alias_exempt(w, tables)                # ruling C3-3: the named row may change its own legacy spelling
    owned["nav"] = owned.get("nav", set()) - exempt
    reported = set()
    for where, ops in maps.items():
        raw = WK.raw_rule_flags(ops, derived, tables, frozenset(exempt))
        if raw:
            problems.append(f"{where} sets {raw}, which the exes' rules own (review F-8, ruling E2-8: name the rule in "
                            "rules, never write its flags; its params are checked against its params_schema)")
            reported |= {(s, f) for s in ops if isinstance(ops[s], dict) for t in ops[s] if isinstance(t, str)
                         for f in owned.get(s, set()) if option_tokens([t], f)}
    if kind(w) in WK.SCHEMA_ONLY:
        return problems
    if kind(w) in WK.LIBRARY_KINDS:                 # no rule cell: add-alpha copies the parent's flag lists and nav
        try:
            return problems + match_flag_refusal(match_flag_list_hits(w, parent, None, maps, sets, set()))
        except (research_spec.TemplateError, KeyError, TypeError) as exc:
            return problems + [f"the parent's flag lists cannot be read for the match-flag guard (ruling E2-14): {exc}"]
    match_hits, match_named = match_flag_map_hits(maps)
    try:
        rc, template, tpath = base_rule_cell(w)
        problems += ref_refusal(maps, sets, template)     # ruling E2-15 (b), before resolve refuses a literal key
        sets_k, flags_k = kind_constants(w, parent, derived)
        doc = WS.rule_cell_doc(template or {}, tpath, w.manifest["parent"]["spec"],
                               WK.merged_constants(rc, sets_k, flags_k), w.manifest["wave"])
        cell = research_spec.resolve(doc, w.path(cell_path(w)), RC.load_spec, research_tree.REPO)
        base = dict(parent, **{s: dict(parent[s], flags=research_spec.apply_flags(
            flag_lists(parent, s)[f"{s}.flags"], ops, f"{s}.flags")) for s, ops in derived.items()
            if isinstance(parent.get(s), dict)})              # the parent's lists with only the named rules applied
        changes = []
        for s in sorted(owned):
            want_lists = flag_lists(base, s)
            for label, flags in flag_lists(cell, s).items():
                changes += [(s, label, opt, occurrences(flags, opt), occurrences(want_lists[label], opt),
                             option_tokens(flags, opt), option_tokens(want_lists[label], opt))
                            for opt in sorted(owned[s])]
        ref_flags = (ref_side(cell) or {}).get("flags")   # ruling E2-15 (a): a ref's own flags, judged as nav.flags
        if isinstance(ref_flags, list) and "nav" in owned:
            want = flag_lists(base, "nav")["nav.flags"]
            changes += [("nav", "ref.flags", opt, occurrences(ref_flags, opt), occurrences(want, opt),
                         option_tokens(ref_flags, opt), option_tokens(want, opt)) for opt in sorted(owned["nav"])]
        match_hits += match_flag_list_hits(w, parent, cell, maps, sets, match_named)
    except (research_spec.TemplateError, RC.CycleError, KeyError, TypeError) as exc:
        return problems + match_flag_refusal(match_hits) + [f"the rule cell cannot be resolved for the raw-rule-flag "
                                                            f"guard (ruling E2-8): {exc}"]
    problems += match_flag_refusal(match_hits)
    problems += leverage_refusal(w, parent, cell, sets_k, sets, maps)
    problems += inputs_refusal(w, parent, cell, sets, template)      # ruling E2-17
    problems += alias_beside_rule(w, cell, tables, derived)          # ruling C3-3: one of the two, never both
    grouped: dict = {}
    for s, label, opt, got, want, toks, base_toks in changes:
        if got == want or (s, opt) in reported:
            continue
        added = sorted({t for t in toks if toks.count(t) > base_toks.count(t)})
        removed = sorted({t for t in base_toks if base_toks.count(t) > toks.count(t)})
        what = (f"adds token(s) {added}" if added else f"drops token(s) {removed}" if removed else
                f"gives token(s) {sorted(set(toks))} other values")
        routes = ref_routes(maps, sets) if label.startswith(WK.REF_KEY + ".") else touching(s, opt, maps, sets)
        via = ", ".join(routes or ["the resolved cell (no raw route names it)"])
        grouped.setdefault((via, label, what), []).append(f"{opt} {got} (the parent's with only the named rules: "
                                                          f"{want})")
    for (via, label, what), opts in grouped.items():
        problems.append(f"{via}: the cell's {label} {what}, read as {'; '.join(opts)}, a flag the exes' rules own "
                        "(ruling E2-8: a rule's flag comes only from rules, never from constants.set / change.set / a "
                        "flag map, another spelling or an abbreviation)")
    return problems


def leverage_refusal(w: Wave, parent: dict, cell: dict, sets_k: dict, sets: dict, maps: dict | None = None
                     ) -> list[str]:
    """Ruling E2-10 (review E2 R2c): nav.leverage is a rule parameter, fixed-v1's aim_leverage (research_cycle puts
    it in --aim-leverage {leverage}, after the flag lists are compared). Only a kind leverage wave (leverage.value,
    range-checked at preflight, at most WK.LEVERAGE_MAX) or the gross-match stage (the -gm copy at L', written by
    match after preflight) sets it. Refused: any raw set / unset key nav.leverage in any kind (in a leverage wave
    leverage.value would silently override it), and a resolved rule cell whose nav.leverage differs from the parent's
    (any other kind) or from leverage.value (kind leverage), by whatever route (a whole nav section set, a template).
    Ruling E2-15 (a): the reference side alike: a ref's own leverage (research_cycle's ref_step runs the ref NAV at
    ref.leverage, else at nav.leverage, already judged) must be the leverage the cell side must have."""
    k = kind(w)
    want = sets_k.get(WK.LEVERAGE_KEY) if k == "leverage" else (parent.get("nav") or {}).get("leverage")
    got = (cell.get("nav") or {}).get("leverage")
    raw = [f"{r} {key} = {value!r}" for r, ks in sets.items() for key, value in ks.items()
           if key == WK.LEVERAGE_KEY or key.startswith(WK.LEVERAGE_KEY + ".")]
    where = f"{'leverage.value gives' if k == 'leverage' else 'the parent has'} {want!r}"
    rule = ("is a rule parameter (fixed-v1's aim_leverage, --aim-leverage {leverage}) that only a kind leverage wave "
            f"(leverage.value, inside the NAV table's range, at most {WK.LEVERAGE_MAX}) or the gross-match stage (L') "
            f"sets (ruling E2-10: kind {k} may not set or change it")
    out = []
    if raw or str(got) != str(want):
        via = (raw or [f"{r} nav" for r, ks in sets.items() if "nav" in ks] or
               ["the resolved cell (no raw route names it)"])
        out.append(f"{', '.join(via)}: the cell's {WK.LEVERAGE_KEY} would be {got!r} where {where}; "
                   f"{WK.LEVERAGE_KEY} {rule})")
    ref = ref_side(cell) or {}
    if "leverage" in ref and str(ref["leverage"]) != str(want):
        via = ref_routes(maps or {}, sets) or ["the resolved cell (no raw route names it)"]
        out.append(f"{', '.join(via)}: the cell's ref.leverage would be {ref['leverage']!r} where {where}; the "
                   f"reference side is judged as the cell's (ruling E2-15 (a)) and its leverage {rule})")
    return out


def named_rules(w: Wave) -> set:
    """The rule ids a kind rule wave names in ``rules`` (none for every other kind)."""
    if kind(w) != "rule":
        return set()
    return {r["name"] for r in w.manifest.get("rules") or [] if isinstance(r, dict) and isinstance(r.get("name"), str)}


def alias_exempt(w: Wave, tables: dict) -> set:
    """Ruling C3-3: the NAV aliases (x-flag-aliases) whose row the wave names in rules. Every other alias is an owned
    flag of the guard: inherited unchanged from the lineage parent, never added, removed or changed by a raw route."""
    named = named_rules(w)
    return {alias for alias, rid in WK.flag_aliases(tables).items() if rid in named}


def alias_beside_rule(w: Wave, cell: dict, tables: dict, derived: dict) -> list[str]:
    """Ruling C3-3 (the row's description: "one of the two, never both"): a wave naming a row whose x-flag it derives
    may not leave that row's legacy spelling on the cell's NAV flag list (the NAV exe would refuse the pair at run;
    here it stops at preflight, naming both)."""
    out = []
    nav_flags = flag_lists(cell, "nav").get("nav.flags", []) if isinstance(cell.get("nav"), dict) else []
    named, rows = named_rules(w), {}
    for row in (tables.get("nav") or {}).get("rules") or []:
        if isinstance(row, dict) and isinstance(row.get("id"), str):
            rows[row["id"]] = row
    for alias, rid in sorted(WK.flag_aliases(tables).items()):
        tokens = str((rows.get(rid) or {}).get("x-flag") or "").split()
        if rid not in named or not tokens or tokens[0] not in (derived or {}).get("nav", {}):
            continue
        if occurrences(nav_flags, alias):
            out.append(f"rules names {rid}, which derives nav {tokens[0]}, but the cell's nav.flags still carry its "
                       f"legacy spelling {alias} (ruling C3-3: one of the two, never both; drop {alias} in this wave)")
    return out


def inputs_routes(sets: dict, template: dict | None) -> dict:
    """{inputs key ("*" for the whole section): [route]} of every raw route that sets, replaces or unsets an inputs.*
    key: a set / unset key ``inputs`` or ``inputs.<k>[...]`` (rule_cell.constants.set, the template's change.set /
    change.unset) and the template's change.inputs.<k>."""
    out: dict = {}
    for r, ks in sets.items():
        for key in ks:
            if key == "inputs":
                out.setdefault("*", []).append(f"{r} inputs")
            elif key.startswith("inputs."):
                out.setdefault(key.split(".")[1], []).append(f"{r} {key}")
    for key in (((template or {}).get("change") or {}).get("inputs") or {}):
        out.setdefault(key, []).append(f"rule_cell.template change.inputs.{key}")
    return out


def inputs_changed(parent: dict, cell: dict) -> list[str]:
    """The inputs keys whose resolved value in the cell is not the one every cell of this parent gets: the parent's
    (its identity inputs dropped) for an ordinary key; for a derived key (research_spec.DERIVED: the paired reference
    and pool) the parent's own output (research_spec.parent_references), compared by path and dir."""
    p, c = parent.get("inputs") or {}, cell.get("inputs") or {}
    skip = set(research_spec.IDENTITY_INPUTS) | set(research_spec.DERIVED)
    out = {k for k in (set(p) | set(c)) - skip if p.get(k) != c.get(k)}
    try:
        refs = research_spec.parent_references(parent, "marginal" in cell, True)
    except research_spec.TemplateError:
        refs = {}
    for k in set(research_spec.DERIVED) & set(c):
        want, got = refs.get(k), c.get(k)
        if not (isinstance(want, dict) and isinstance(got, dict) and
                all(got.get(x) == want.get(x) for x in ("path", "dir"))):
            out.add(k)
    return sorted(out)


def inputs_refusal(w: Wave, parent: dict, cell: dict, sets: dict, template: dict | None) -> list[str]:
    """Ruling E2-17 (E2 re-review 4, O2): in a v2 wave inputs.* may be set or changed only by the kinds whose schema
    defines an input change, and only the keys that kind names (WK.KIND_INPUTS: a role wave's inputs.role). Any other
    kind, or another key, that sets or changes an inputs.* key by any route (rule_cell.constants.set, the template's
    change.set / change.unset / change.inputs, a whole inputs section) is refused, naming key and route; a resolved
    difference no raw route names is refused as "the resolved cell". Setting a key to its parent's value is still a
    route that sets it: refused."""
    k = kind(w)
    allowed = set(WK.KIND_INPUTS.get(k, ()))
    routes = inputs_routes(sets, template)
    keys = sorted((set(routes) | set(inputs_changed(parent, cell))) - allowed)
    if not keys:
        return []
    what = "; ".join(f"inputs.{key}" + (f" by {', '.join(routes[key])}" if key in routes else
                                        " by the resolved cell (no raw route names it)") for key in keys)
    kinds = ", ".join(f"{kk} ({', '.join('inputs.' + x for x in ks) or 'no key'})" for kk, ks in WK.KIND_INPUTS.items())
    return [f"{what}: kind {k} may not set or change an inputs.* key (ruling E2-17: inputs.* changes only in the kinds "
            f"whose schema defines one, and only the keys they name: {kinds}; a rule cell that also swaps its data is two "
            "changes in one registered trial)"]


def ref_side(cell: dict) -> dict | None:
    """The resolved cell's own ref section (its reference construction), None without one."""
    ref = cell.get(WK.REF_KEY)
    return ref if isinstance(ref, dict) else None


TEMPLATE_REF = f"rule_cell.template change.set {WK.REF_KEY}"    # the template's own pinned ref block (E2-15)


def ref_routes(maps: dict, sets: dict) -> list[str]:
    """The raw routes that add, replace, unset or edit the cell's ref section: a set / unset key ``ref`` or ``ref.*``
    and an option map's ``ref`` section (a ref's flags)."""
    return ([f"{r} {key}" for r, ks in sets.items() for key in ks
             if key == WK.REF_KEY or key.startswith(WK.REF_KEY + ".")] +
            [f"{r} {WK.REF_KEY}" for r, ops in maps.items() if WK.REF_KEY in ops])


def ref_refusal(maps: dict, sets: dict, template: dict | None) -> list[str]:
    """Ruling E2-15 (b) (review E2 S1): the reference of a v2 cell is half of its paired test, so a cell may not add,
    replace or edit its ref section by any route: rule_cell.constants.set / .flags, the template's change.set ``ref.*``
    / change.unset / change.flags, a literal top-level ``ref``. It comes only from the lineage parent's cell of record
    (research_spec.resolve drops the parent's own ref) or from the template's own pinned block, its change.set ``ref``
    whole (TEMPLATE_REF; the manifest pins the template's bytes), whose resolved side the E2-8 / E2-10 guards judge
    as the cell's (ruling E2-15 (a))."""
    bad = [x for x in ref_routes(maps, sets) if x != TEMPLATE_REF]
    if isinstance(template, dict) and WK.REF_KEY in template:
        bad.append(f"rule_cell.template {WK.REF_KEY} (a literal top-level key)")
    if not bad:
        return []
    return [f"{', '.join(bad)}: a cell may not add, replace or edit its {WK.REF_KEY} section by any route (ruling "
            "E2-15 (b)): the reference of a v2 cell comes only from its lineage parent's cell of record or from the "
            f"template's own pinned block ({TEMPLATE_REF}, whole); a wave that needs another reference names that "
            "cell as its lineage parent"]


def match_flag_map_hits(maps: dict) -> tuple[list[str], set]:
    """Ruling E2-14 (review E2 R3a): (the option map entries whose key or value, a parent-value map's too, reads as a
    WK.MATCH_FLAGS flag, the sections they name). Read before the cell is resolved: research_spec.apply_flags refuses
    some of them (a single dash, a parent value the map lacks) without naming the match flag."""
    hits, named = [], set()
    for r, ops in maps.items():
        for s, sec in ops.items():
            for key, value in (sec.items() if isinstance(sec, dict) else ()):
                if WK.match_flag_reads(key):
                    hits.append(f"{r} {s} {key} (reads as {WK.match_flag_reads(key)})")
                    named.add(s)
                elif any(WK.match_flag_reads(v) for v in map_values(value)):
                    hits.append(f"{r} {s} {key} {value}")
                    named.add(s)
    return hits, named


def match_flag_refusal(hits: list[str]) -> list[str]:
    """Ruling E2-14's refusal of ``hits`` (match_flag_map_hits, match_flag_list_hits); [] without one."""
    if not hits:
        return []
    return [f"{'; '.join(hits)}: {list(WK.MATCH_FLAGS)} are the gross-match stage's own NAV flags (the exe would "
            "publish its NAV at an L' it picks itself, a leverage no wave registered): only the match stage of "
            f"gross_match {WK.MATCH_STAGE} puts them on an argv, in its run recipe after preflight (ruling E2-14, "
            "review E2 R3a; ruling E2-10: only kind leverage or the gross-match stage sets the leverage)"]


def match_flag_list_hits(w: Wave, parent: dict, cell: dict | None, maps: dict, sets: dict, named: set) -> list[str]:
    """Ruling E2-14 (review E2 R3a): WK.MATCH_FLAGS (P9 C2's nav --calibrate-gross / --calibrate-tolerance: the NAV
    exe publishes its run at an L' it picks itself) are the gross-match stage's own: only the match stage of
    gross_match WK.MATCH_STAGE writes them, into its run recipe, after preflight (never read by this guard). Besides
    the option maps (match_flag_map_hits; their ``named`` sections are not reported twice), every token of the cell's
    flag lists (every section, the w pass's list, a ref's own flags) that reads as one (WK.match_flag_reads: case,
    single dash, ``=value``, abbreviation), whatever put it there (constants.set, change.set / unset, a whole section,
    the parent's lists it inherits), as "route: the cell's <list> carries [tokens]". ``cell`` None: a library kind,
    whose cell is add-alpha's copy of the ``parent``'s lists."""
    hits: list[str] = []
    src = parent if cell is None else cell
    lists = {label: flags for s in WK.FLAG_SECTIONS for label, flags in flag_lists(src, s).items()}
    ref_flags = (ref_side(cell) or {}).get("flags") if cell is not None else None
    if isinstance(ref_flags, list):
        lists[f"{WK.REF_KEY}.flags"] = ref_flags
    inherited = {label: flags for s in WK.FLAG_SECTIONS for label, flags in flag_lists(parent, s).items()}
    seen = set()
    for label, flags in lists.items():
        section = label.split(".", 1)[0]
        toks = [t for t in flags if WK.match_flag_reads(t)]
        if not toks or section in named or (section, tuple(toks)) in seen:     # ic.w_flags: ic.flags + the w map
            continue
        seen.add((section, tuple(toks)))
        if cell is None or [t for t in inherited.get(label, []) if WK.match_flag_reads(t)] == toks:
            via = [f"the parent spec {w.manifest['parent']['spec']} {label} (the cell inherits it)"]
        elif section == WK.REF_KEY:
            via = ref_routes(maps, sets)
        else:
            via = sorted({x for t in toks for f in WK.match_flag_reads(t) for x in touching(section, f, maps, sets)})
        hits.append(f"{', '.join(via or ['the resolved cell (no raw route names it)'])}: the cell's {label} carries "
                    f"{toks}")
    return hits


def rule_cell_kind(w: Wave) -> bool:
    """A v2 kind whose cell is a rule cell, a template on the parent (rule, horizon, role, leverage): its paired
    reference is always the parent's NAV (summ --reference inputs.reference_cell), never a ref construction, so verify
    judges it by ruling W1-I2 (a) whatever its ref phase (ruling E2-15 (c))."""
    k = kind(w)
    return k is not None and k not in WK.LIBRARY_KINDS and k not in WK.SCHEMA_ONLY


def preflight_kind_plan(w: Wave) -> list[str]:
    k, m = kind(w), w.manifest
    if k is None:
        return []
    lines = [f"#   kind {k} (contract K-P9-9)"]
    if k in WK.SCHEMA_ONLY:
        lines.append(f"#   kind {k}: schema only until lane D3 (preflight refuses to run it): {m[WK.BLOCKS[k]]}")
    spec = w.load_spec(m["parent"]["spec"])
    exes = RC.effective_exes(spec)
    for table, nav in (("ic", False), ("nav", True)):
        if exes.get(table):
            lines.append(WS.fmt_argv(WK.listing_argv(w.path(exes[table]).as_posix(), nav)))
    if k in ("rule", "leverage"):
        lines.append(f"#   then: {'rules ' + str(m['rules']) if k == 'rule' else 'leverage ' + str(m['leverage'])} "
                     "validated against those tables (params_schema; flags derived)")
    if k == "walk-forward":
        wf = m["walk_forward"]
        lines.append(f"#   then: walk_forward {wf}: the parent's fit refitted per block (fit flags "
                     f"{WK.walk_forward_flags(wf)['fit']}; the parent must fit under {WK.WALK_FORWARD_PARENT_FIT}); "
                     f"outputs renamed -{WK.walk_forward_token(wf)}; weights schema atx.dsl-composition-weights/v3")
    lines.append("#   then: ruling E2-8: every IC rule mapped in COMPOSITION_FLAGS; no rule's flag in the cell's "
                 "resolved flag lists but through rules (constants / template flags and sets, spellings, "
                 "abbreviations)")
    lines.append(
        f"#   then: ruling E2-14: no {' / '.join(WK.MATCH_FLAGS)} in any flag list (the {WK.MATCH_STAGE} match "
        "stage's own); ruling E2-15: no ref section but the template's own pinned block, its resolved side "
        "judged as the cell's")
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
    if m.get("rule_cell") is None and kind(w) == "walk-forward":     # P9 lane D3: never a rule_cell
        path, doc = WK.walk_forward_template(m["parent"]["spec"], m["wave"], m["walk_forward"])
        return {"template": path, "name": doc["name"]}, doc, path
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
    derived = ((done.get("preflight") or {}).get("kind") or {}).get("flags") if k == "rule" else {}
    if derived is None:
        raise StageError("spec: the preflight receipt has no derived rule flags (run preflight again)", EXIT_PIN)
    sets, flags = kind_constants(w, parent, derived)      # the constants preflight's guard resolved (ruling E2-8)
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
