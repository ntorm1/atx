"""The research wave manifest (``atx.research-wave/v1``) and the candidate registration it shares with the queue.

One wave is one cell on the current book, declared before anything is measured:

  {"schema": "atx.research-wave/v1",
   "wave": ID,                                   letters, digits, '-', '_' (names the state dir and the result)
   "description": TEXT,
   "parent": {"spec": PATH, "library": NAME},    the last accepted cell's spec (a spec or a template) and its library
   "fields": {"dir": DIR, "manifest_sha256": PIN},   the fields version the wave runs on (as built)
   "library": NAME,                              add-alpha --name of the screen library (library waves); the cell after
                                                 a sign-rule drop is NAME + b_suffix (default "b")
   "candidates": [CANDIDATE, ...]                a library wave: the frozen strings, in roster order
     or "rule_cell": {"template": PATH, "template_sha256": PIN, "name": NAME?,
                      "constants": {"set": {dotted: value}, "flags": {section: {"--opt": value}}}}
                                                 a rule wave: a cell template on the parent with its registered constants
   "acceptance": {"rule": "pm7-34", "printed": [criterion, ...]},
   "sign_rule": "pm7-35",                         (library waves)
   "gross_match": "pm6-6" | "none",
   "budget": {"id": ID, "admission_cap": N?, "admission_cycle_prefix": TEXT? | "admission_cycle_prefixes": [TEXT]?,
              "admission_origin": CLASS?, "construction_cap": N?}
                                          the hand count: admission lines of cycles with the prefix, or with any of
                                          the prefixes (P9 DEC-2: a wave whose library is not under the older prefix,
                                          e.g. ["v8x", "v8ys"]; one of the two keys, with admission_cap) (and of the
                                          origin class), re-screens left out; new = the wave's non-re-screen strings
                                          whose trial_id is not ledgered yet
   "ledger": PATH,                                the sprint ledger of record (the cells' summ.ledger)
   "expect": {"n_before": N},                     the ledger N the wave was planned on (a stale plan is refused)
   "out_dir": DIR,                                the wave's state dir (receipts, plans, readers, wave-result.json);
                                                  outside the code pathspec (research_tree.CODE_PATHSPEC)
   "record": {"copy_to": DIR?},                   where the record stage copies wave-result.json and the log section
                                                  (outside the code pathspec too)
   "speed": {"reuse_screen_marginal": true, "screen_first": true},
   "marginal": {"ruling": ID, "pool_only": BOOL?, "seconds": N?}}   optional: a PM ruling on the library's marginal
                                                 phase (e.g. PM8-15: pool only, PM6-8 (i), on a theme-erc parent
                                                 with more themes than the verb takes; the phase cap), applied
                                                 to the screen library at register and to a b library that runs
                                                 its own marginal; seconds above the bounded runner's maximum
                                                 (research_tree.RUNNER_MAX_SECONDS, 600) is refused at load
                                                 the wave's own speed rules (wave_stages.py; both default true; they
                                                 change no input of a decision): the cell after a sign-rule drop
                                                 carries the screen's per-row marginal fields (report only) instead of
                                                 a second marginal pass when the screen ran its marginal mode, and a
                                                 b library runs --screen before its cell

CANDIDATE (also one file of the queue, scripts/specs/v8/candidates/<id>.json, with status and wave):
  {"id", "dsl", "dsl_sha256" (SHA-256 of the DSL's UTF-8 bytes), "theme", "tier", "prior_sign" (+1 | -1), "citation",
   "origin" (prior | grid | mined), "hypothesis" (the hypothesis id: one variant per hypothesis),
   "kind": "add" | "replace", "replaces": [ID, ...] (kind replace), "rescreen": bool (one replaced member),
   "removes": [ID, ...] (kind add), "fields": [NAME, ...]?, "prior_sign_source"?, "form"?, "formula"?, "domain"?,
   "deviation"?, "exception": {"limits": {LIMIT: N}, "basis": TEXT}?, "ruling"? (a second variant of a hypothesis)}

``load`` validates and returns (manifest, its file SHA-256); every problem is listed at once (WaveError).
"""
from __future__ import annotations

import hashlib
import json
from pathlib import Path
import re

import research_tree
import wave_rules

SCHEMA = "atx.research-wave/v1"
ID_RE = re.compile(r"[A-Za-z0-9][A-Za-z0-9_-]{0,63}")
CAND_ID_RE = re.compile(r"[a-z][a-z0-9_]{0,63}")
SHA_RE = re.compile(r"[0-9a-f]{64}")
ORIGINS = ("prior", "grid", "mined")
KINDS = (wave_rules.ADD, wave_rules.REPLACE)
CANDIDATE_REQUIRED = ("id", "dsl", "dsl_sha256", "theme", "tier", "prior_sign", "citation", "origin", "hypothesis")
CANDIDATE_OPTIONAL = ("kind", "replaces", "rescreen", "removes", "fields", "prior_sign_source", "form", "formula",
                      "domain", "deviation", "exception", "ruling", "notes", "lane", "report")
TOP_REQUIRED = ("schema", "wave", "parent", "fields", "acceptance", "gross_match", "budget", "ledger", "expect",
                "out_dir")
TOP_OPTIONAL = ("description", "library", "candidates", "rule_cell", "sign_rule", "record", "b_suffix", "speed",
                "marginal")
SPEED_KEYS = ("reuse_screen_marginal", "screen_first")
MARGINAL_KEYS = ("pool_only", "seconds", "ruling")   # "marginal": a PM ruling on the library's marginal phase
BUDGET_LIMITS = ("max_extra_fields", "max_slots", "max_prior_bars")   # generate_library.BUDGET
PREFIX_KEYS = ("admission_cycle_prefix", "admission_cycle_prefixes")  # one TEXT, or a list (P9 OR §4, DEC-2)
BUDGET_KEYS = ("id", "admission_cap") + PREFIX_KEYS + ("admission_origin", "construction_cap")


class WaveError(ValueError):
    pass


def dsl_sha256(dsl: str) -> str:
    return hashlib.sha256(dsl.encode("utf-8")).hexdigest()


def file_sha256(path: Path) -> str:
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def _rel(value) -> bool:
    return isinstance(value, str) and bool(value) and not Path(value).is_absolute() and ".." not in Path(value).parts


def _text(value) -> bool:
    return isinstance(value, str) and bool(value.strip())


def in_code_pathspec(value: str) -> bool:
    """Whether a root-relative path lies inside research_tree.CODE_PATHSPEC (what the bounded runner requires clean and
    what a wave stage commits), case-insensitively (a Windows checkout)."""
    parts = [p for p in Path(value).parts if p not in ("", ".")]
    return bool(parts) and parts[0].lower() in {s.lower() for s in research_tree.CODE_PATHSPEC}


def candidate_problems(c, where: str, extra_keys: tuple = ()) -> list[str]:
    """Problems of one candidate registration (empty: valid). ``extra_keys``: keys the caller allows besides the
    registration's (the queue's status and wave)."""
    if not isinstance(c, dict):
        return [f"{where}: not an object"]
    out = []
    missing = [k for k in CANDIDATE_REQUIRED if k not in c]
    unknown = sorted(set(c) - set(CANDIDATE_REQUIRED) - set(CANDIDATE_OPTIONAL) - set(extra_keys))
    if missing or unknown:
        out.append(f"{where}: missing {missing}, unknown {unknown}")
    cid = c.get("id")
    if not (isinstance(cid, str) and CAND_ID_RE.fullmatch(cid)):
        out.append(f"{where}: id {cid!r} must match {CAND_ID_RE.pattern}")
    if not _text(c.get("dsl")):
        out.append(f"{where}: dsl must be a non-empty string")
    elif c.get("dsl_sha256") != dsl_sha256(c["dsl"]):
        out.append(f"{where}: dsl_sha256 {c.get('dsl_sha256')!r} is not the SHA-256 of the frozen DSL "
                   f"({dsl_sha256(c['dsl'])})")
    for key in ("theme", "tier", "citation", "hypothesis"):
        if key in c and not _text(c[key]):
            out.append(f"{where}: {key} must be a non-empty string")
    if c.get("prior_sign") not in (1, -1) or isinstance(c.get("prior_sign"), bool):
        out.append(f"{where}: prior_sign must be 1 or -1")
    if c.get("origin") not in ORIGINS:
        out.append(f"{where}: origin must be one of {', '.join(ORIGINS)} (contract K5)")
    kind = c.get("kind", wave_rules.ADD)
    if kind not in KINDS:
        out.append(f"{where}: kind must be one of {', '.join(KINDS)}")
    reps, rems = c.get("replaces", []), c.get("removes", [])
    for key, ids in (("replaces", reps), ("removes", rems)):
        if not isinstance(ids, list) or not all(isinstance(i, str) and CAND_ID_RE.fullmatch(i) for i in ids) or \
                len(set(ids)) != len(ids):
            out.append(f"{where}: {key} must list distinct member ids")
    if kind == wave_rules.REPLACE and not reps:
        out.append(f"{where}: kind replace needs replaces")
    if kind == wave_rules.ADD and reps:
        out.append(f"{where}: replaces needs kind replace")
    if kind == wave_rules.REPLACE and rems:
        out.append(f"{where}: removes is for kind add (add-alpha: --replaces and --removes exclude each other)")
    if "rescreen" in c and (type(c["rescreen"]) is not bool or (c["rescreen"] and len(reps) != 1)):
        out.append(f"{where}: rescreen is true or false, and true needs exactly one replaced member")
    if "fields" in c and (not isinstance(c["fields"], list) or not all(_text(f) for f in c["fields"])):
        out.append(f"{where}: fields must list field names")
    for key in ("prior_sign_source", "form", "formula", "domain", "deviation", "ruling", "notes", "lane", "report"):
        if key in c and not _text(c[key]):
            out.append(f"{where}: {key} must be a non-empty string when present")
    ex = c.get("exception")
    if ex is not None and (not isinstance(ex, dict) or set(ex) != {"limits", "basis"} or not _text(ex["basis"]) or
                           not isinstance(ex["limits"], dict) or not ex["limits"] or
                           not all(k in BUDGET_LIMITS and type(v) is int and v > 0 for k, v in ex["limits"].items())):
        out.append(f"{where}: exception must be {{limits: {{{'|'.join(BUDGET_LIMITS)}: N}}, basis: TEXT}}")
    return out


def registration(c: dict) -> dict:
    """The registration fields of a candidate (the queue's status and wave dropped): what a manifest carries."""
    return {k: c[k] for k in CANDIDATE_REQUIRED + CANDIDATE_OPTIONAL if k in c}


def validate(m) -> list[str]:
    """Every problem of a wave manifest (empty: valid)."""
    if not isinstance(m, dict) or m.get("schema") != SCHEMA:
        return [f"schema must be {SCHEMA}"]
    out = []
    missing = [k for k in TOP_REQUIRED if k not in m]
    unknown = sorted(set(m) - set(TOP_REQUIRED) - set(TOP_OPTIONAL))
    if missing or unknown:
        out.append(f"manifest: missing {missing}, unknown {unknown}")
    if not (isinstance(m.get("wave"), str) and ID_RE.fullmatch(m["wave"])):
        out.append(f"wave must match {ID_RE.pattern}")
    p = m.get("parent")
    if not (isinstance(p, dict) and set(p) == {"spec", "library"} and _rel(p["spec"]) and _text(p["library"])):
        out.append("parent must be {spec: a root-relative spec path, library: the parent library name}")
    f = m.get("fields")
    if not (isinstance(f, dict) and set(f) == {"dir", "manifest_sha256"} and _rel(f["dir"]) and
            isinstance(f["manifest_sha256"], str) and SHA_RE.fullmatch(f["manifest_sha256"])):
        out.append("fields must be {dir: a root-relative fields dir, manifest_sha256: its manifest's SHA-256}")
    for key in ("ledger", "out_dir"):
        if key in m and not _rel(m[key]):
            out.append(f"{key} must be a root-relative path")
    if _rel(m.get("out_dir")) and in_code_pathspec(m["out_dir"]):
        out.append(f"out_dir {m['out_dir']!r} lies inside the code pathspec ({', '.join(research_tree.CODE_PATHSPEC)}): "
                   "receipts, readers and results would dirty it (choose a dir outside, e.g. under the build dir)")
    has_c, has_r = "candidates" in m, "rule_cell" in m
    if has_c == has_r:
        out.append("a wave has exactly one of candidates (a library wave) and rule_cell (a rule wave)")
    if has_c:
        out += candidates_problems(m)
    if has_r:
        out += rule_cell_problems(m["rule_cell"])
    acc = m.get("acceptance")
    if not (isinstance(acc, dict) and set(acc) <= {"rule", "printed"} and isinstance(acc.get("rule"), str) and
            isinstance(acc.get("printed", []), list)):
        out.append("acceptance must be {rule: NAME, printed: [criterion, ...]}")
        acc = {"rule": "pm7-34"}
    if has_c and not isinstance(m.get("sign_rule"), str):
        out.append("a library wave names its sign_rule")
    out += wave_rules.validate_names(m.get("sign_rule") if has_c else None, acc["rule"], acc.get("printed", []),
                                     m.get("gross_match"))
    b = m.get("budget")
    if not (isinstance(b, dict) and _text(b.get("id")) and set(b) <= set(BUDGET_KEYS) and
            all(type(b[k]) is int and b[k] >= 0 for k in ("admission_cap", "construction_cap") if k in b) and
            sum(k in b for k in PREFIX_KEYS) == (1 if "admission_cap" in b else 0) and
            ("admission_cycle_prefix" not in b or _text(b["admission_cycle_prefix"])) and
            ("admission_cycle_prefixes" not in b or (isinstance(b["admission_cycle_prefixes"], list) and
                                                     b["admission_cycle_prefixes"] and
                                                     all(_text(p) for p in b["admission_cycle_prefixes"]) and
                                                     len(set(b["admission_cycle_prefixes"])) ==
                                                     len(b["admission_cycle_prefixes"]))) and
            b.get("admission_origin", ORIGINS[0]) in ORIGINS and ("admission_origin" not in b or "admission_cap" in b)):
        out.append("budget must be {id, admission_cap + one of admission_cycle_prefix (TEXT) or "
                   "admission_cycle_prefixes ([TEXT, ...], distinct) (together), admission_origin? (prior | grid | "
                   "mined), construction_cap}")
    e = m.get("expect")
    if not (isinstance(e, dict) and set(e) == {"n_before"} and type(e["n_before"]) is int and e["n_before"] >= 0):
        out.append("expect must be {n_before: the ledger N the wave was planned on}")
    r = m.get("record", {})
    if not (isinstance(r, dict) and set(r) <= {"copy_to"} and ("copy_to" not in r or _rel(r["copy_to"]))):
        out.append("record must be {copy_to: a root-relative dir}")
    elif "copy_to" in r and in_code_pathspec(r["copy_to"]):
        out.append(f"record.copy_to {r['copy_to']!r} lies inside the code pathspec: the copies would stay dirty and "
                   "the next wave's preflight would refuse (choose a dir outside, e.g. the sprint dir)")
    sp = m.get("speed", {})
    if not (isinstance(sp, dict) and set(sp) <= set(SPEED_KEYS) and all(type(v) is bool for v in sp.values())):
        out.append(f"speed must map {', '.join(SPEED_KEYS)} to true or false")
    mg = m.get("marginal", {"ruling": "-"})
    if not (isinstance(mg, dict) and set(mg) <= set(MARGINAL_KEYS) and isinstance(mg.get("ruling"), str) and
            mg["ruling"].strip() and type(mg.get("pool_only", False)) is bool and
            ("seconds" not in mg or (type(mg["seconds"]) is int and mg["seconds"] > 0))):
        out.append("marginal must be {ruling: the PM ruling, pool_only?: true | false, seconds?: a positive integer}")
    elif research_tree.seconds_cap_refusal("marginal.seconds", mg.get("seconds")):
        out.append(research_tree.seconds_cap_refusal("marginal.seconds", mg["seconds"]))
    if "b_suffix" in m and not (isinstance(m["b_suffix"], str) and re.fullmatch(r"[a-z0-9]{1,8}", m["b_suffix"])):
        out.append("b_suffix must be 1-8 lower-case letters or digits")
    return out


def candidates_problems(m: dict) -> list[str]:
    cands = m["candidates"]
    if not isinstance(cands, list) or not cands:
        return ["candidates must be a non-empty list"]
    out = []
    if not (isinstance(m.get("library"), str) and ID_RE.fullmatch(m["library"])):
        out.append("a library wave names its library (add-alpha --name)")
    for k, c in enumerate(cands):
        out += candidate_problems(c, f"candidates[{k}]")
    good = [c for c in cands if isinstance(c, dict)]
    for key in ("id", "dsl_sha256", "hypothesis"):
        seen = [c.get(key) for c in good]
        dup = sorted({x for x in seen if seen.count(x) > 1 and x is not None})
        if dup:
            out.append(f"candidates: duplicate {key} {dup} (one string per hypothesis per wave)")
    return out


def rule_cell_problems(r) -> list[str]:
    if not (isinstance(r, dict) and {"template", "template_sha256"} <= set(r) and
            set(r) <= {"template", "template_sha256", "name", "constants"} and _rel(r["template"]) and
            isinstance(r["template_sha256"], str) and SHA_RE.fullmatch(r["template_sha256"])):
        return ["rule_cell must be {template: PATH, template_sha256: PIN, name?, constants?}"]
    c = r.get("constants", {})
    if not (isinstance(c, dict) and set(c) <= {"set", "flags"} and isinstance(c.get("set", {}), dict) and
            isinstance(c.get("flags", {}), dict) and all(isinstance(v, dict) for v in c.get("flags", {}).values())):
        return ["rule_cell.constants must be {set: {dotted key: value}, flags: {section: {--opt: value}}}"]
    if "name" in r and not (isinstance(r["name"], str) and ID_RE.fullmatch(r["name"])):
        return [f"rule_cell.name must match {ID_RE.pattern}"]
    return []


def load(path: Path) -> tuple[dict, str]:
    """(the manifest, the SHA-256 of its file); WaveError listing every problem."""
    p = Path(path)
    try:
        data = p.read_bytes()
        m = json.loads(data.decode("utf-8"))
    except (OSError, ValueError) as exc:
        raise WaveError(f"wave manifest {p}: {exc}") from exc
    problems = validate(m)
    if problems:
        raise WaveError(f"wave manifest {p}: " + "; ".join(problems))
    return m, hashlib.sha256(data).hexdigest()


def b_library(m: dict) -> str:
    return m["library"] + m.get("b_suffix", "b")


def budget_prefixes(b: dict) -> list[str]:
    """The cycle-name prefixes whose admission lines a budget counts: admission_cycle_prefixes, else the one
    admission_cycle_prefix (the older form, still valid), else none."""
    if "admission_cycle_prefixes" in b:
        return list(b["admission_cycle_prefixes"])
    return [b["admission_cycle_prefix"]] if "admission_cycle_prefix" in b else []


def speed(m: dict, key: str) -> bool:
    return bool((m.get("speed") or {}).get(key, True))
