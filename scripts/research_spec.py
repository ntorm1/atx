"""Cycle spec templates (platform v8 lane A2): a research cell written as its parent's spec plus the registered change.

research_cycle.py loads a template like any SPEC (plan, run, status, lock). A template file:

  {"schema": "atx.research-cycle-template/v1", "name": ..., "description": ...,
   "parent": null | "<spec path>",       root sets it to the last accepted cell's spec (a spec or a template)
   "nominal_parent": "<spec path>",       planned against while parent is null: plan works, run refuses
   "requires": ["<precondition>", ...],   open preconditions (code not written, a ruling): run refuses while listed
   "change": {"unset": [key, ...],        dotted keys removed from the parent's spec
              "set": {key: value},        dotted keys replaced or added (e.g. "nav.output", "gate")
              "inputs": {key: item},      pinned inputs added or replaced (sha256 null until `lock --write`)
              "flags": {section: {"--opt": "value" | true | false | null | {"parent value": "value"}}}},
                                          in <section>.flags: set the option's value (appended when absent) / add
                                          the bare flag / remove the option and its value / root fills it (null: a
                                          "<fill:...>" placeholder; run refuses while one is left) / the value by
                                          the parent's value (a parent value the map lacks is refused; e.g. R-3's
                                          --composition ew-theme-v1 -> ew-theme-aim-v1, ew-theme-std-v1 ->
                                          ew-theme-std-aim-v1)
   "locked": {key: {"path", "sha256"}}}   `lock --write`: the pins of the inputs the template derives

Resolution, in order: the parent spec (a path is looked up next to the template, then from the repository root, then
from the current directory; a template parent is resolved first, a cycle of parents is refused), deep-copied without
the parent's own identity checks against its parent (sections compare, ref, static_check and their inputs
IDENTITY_INPUTS); then unset, set; then DERIVED inputs from the parent's outputs (the paired reference of every R cell
is its parent, plan section 9: reference_cell = its NAV cell, reference_admission = its fit admission and, when the
resolved spec has a marginal section, reference_combined = its first weighted pass's combined signal and
reference_weights = its fit weights, the pool's composition weights); then change.inputs (over the derived ones); then
flags. Name and description are the template's. nav.output must differ from the parent's (a cell is a new NAV dir).
Every other output the template keeps is the parent's and resumes as done (research_cycle never overwrites an
output): a template renames exactly the outputs downstream of its change (a NAV-only change reuses the parent's u, fit,
card and w passes; a composition change renames fit, card, w, nav and monitor; a library change renames them all).

Digest (review F-9): a template's cell is its resolved spec, a function of the bytes of the template and of every
parent up its chain (`lock_template` leaves the parent's pins in the parent's file). spec_digest is the SHA-256 over
those files' SHA-256s, so editing any parent (a re-pointed label role, a nav flag, a relocked pin) changes the digest
of every template below it; a plain spec's digest stays its file's SHA-256. research_cycle records it as the verdict's
spec_sha256 and the C-13 binding (cycle_resume.py).
"""
from __future__ import annotations

import copy
import hashlib
import json
from pathlib import Path

TEMPLATE_SCHEMA = "atx.research-cycle-template/v1"
CHAIN_DIGEST = "atx.research-spec-chain/v1"   # the domain tag of a template's spec_digest
FILL = "<fill:"                          # a value root fills before `run` (a null flag value in the template)
TEMPLATE_KEYS = {"schema", "name", "description", "parent", "nominal_parent", "requires", "change", "locked"}
CHANGE_KEYS = ("unset", "set", "inputs", "flags")
IDENTITY_SECTIONS = ("compare", "ref", "static_check")       # the parent's identity checks against its own parent
IDENTITY_INPUTS = ("baseline_library", "baseline_fields", "reference_daily", "reference_orientations",
                   "reference_daily_ic")
DERIVED = ("reference_cell", "reference_admission", "reference_combined", "reference_weights")


class TemplateError(ValueError):
    pass


def is_template(doc) -> bool:
    return isinstance(doc, dict) and doc.get("schema") == TEMPLATE_SCHEMA


def read(path: Path) -> dict:
    try:
        return json.loads(Path(path).read_text(encoding="utf-8"))
    except (OSError, ValueError) as exc:
        raise TemplateError(f"spec {path}: {exc}") from exc


def parent_of(doc: dict, path: Path, repo: Path) -> tuple[Path, bool]:
    """(the parent spec file, nominal?): `parent`, else `nominal_parent` while parent is null."""
    ref, nominal = doc.get("parent"), doc.get("parent") is None
    if nominal:
        ref = doc.get("nominal_parent")
    if not isinstance(ref, str) or not ref:
        raise TemplateError(f"template {path}: parent (or, while it is null, nominal_parent) must name a spec file")
    for candidate in (Path(path).parent / ref, Path(repo) / ref, Path(ref)):
        if candidate.is_file():
            return candidate.resolve(), nominal
    raise TemplateError(f"template {path}: parent spec {ref} not found (next to the template, under {repo}, or from "
                        "the current directory)")


def placed(spec: dict, name: str) -> str:
    """An output name as research_cycle places it (Cycle.placed: under out_root when relative)."""
    root = spec.get("out_root")
    return f"{root.rstrip('/')}/{name}" if root and not Path(name).is_absolute() else name


def parent_references(parent: dict, marginal: bool) -> dict:
    """The DERIVED inputs of a child: its paired reference and pool are the parent's outputs."""
    if "nav" not in parent or "fit" not in parent or "w_output" not in parent.get("ic", {}):
        raise TemplateError("a template's parent must be a cell spec (fit, ic.w_output and nav)")
    nav, fit = placed(parent, parent["nav"]["output"]), placed(parent, parent["fit"]["output"])
    out = {"reference_cell": {"dir": nav, "path": f"{nav}/summary.json"},
           "reference_admission": {"path": f"{fit}/admission.json"}}
    if marginal:
        out["reference_combined"] = {"path": f"{placed(parent, parent['ic']['w_output'])}-1/train_combined.json"}
        out["reference_weights"] = {"path": f"{fit}/composition_weights.json"}
    return out


def _walk(spec: dict, key: str, create: bool):
    """(container, last key) of a dotted key; None when a level is missing and not created."""
    parts = key.split(".")
    if not all(parts):
        raise TemplateError(f"template change key {key!r} is not a dotted path")
    node = spec
    for part in parts[:-1]:
        if not isinstance(node.get(part), dict):
            if not create:
                return None, parts[-1]
            node[part] = {}
        node = node[part]
    return node, parts[-1]


def apply_flags(flags: list, ops: dict, where: str) -> list:
    """One section's flag list with the template's option operations applied (see the module doc)."""
    out = list(flags)
    for opt, value in ops.items():
        if not isinstance(opt, str) or not opt.startswith("--"):
            raise TemplateError(f"template flags {where}: {opt!r} is not an option")
        k = out.index(opt) if opt in out else -1
        has_value = 0 <= k < len(out) - 1 and not str(out[k + 1]).startswith("--")
        if isinstance(value, dict):                  # the value by the parent's value
            current = out[k + 1] if has_value else None
            if current not in value or not isinstance(value[current], str):
                raise TemplateError(f"template flags {where}: {opt} maps the parent's value, but the parent has "
                                    f"{current!r} (mapped: {sorted(value)})")
            value = value[current]
        if value is False:
            if k >= 0:
                del out[k:k + (2 if has_value else 1)]
        elif value is True:
            if k < 0:
                out.append(opt)
            elif has_value:
                raise TemplateError(f"template flags {where}: {opt} takes a value in the parent ({out[k + 1]})")
        elif value is None or isinstance(value, str):
            text = f"{FILL}{where} {opt}>" if value is None else value
            if k < 0:
                out += [opt, text]
            elif has_value:
                out[k + 1] = text
            else:
                out.insert(k + 1, text)
        else:
            raise TemplateError(f"template flags {where}: {opt} must map to a string, true, false, null or a map of "
                                "the parent's value to a string")
    return out


def resolve(doc: dict, path: Path, load, repo: Path) -> dict:
    """The full cycle spec of a template (`load(path)` loads a parent spec; the module doc gives the order)."""
    unknown = sorted(set(doc) - TEMPLATE_KEYS)
    change = doc.get("change") or {}
    if unknown or not isinstance(change, dict) or set(change) - set(CHANGE_KEYS):
        raise TemplateError(f"template {path}: unknown keys {unknown or sorted(set(change) - set(CHANGE_KEYS))} "
                            f"(template keys {sorted(TEMPLATE_KEYS)}, change keys {list(CHANGE_KEYS)})")
    if not isinstance(doc.get("name"), str) or not doc["name"]:
        raise TemplateError(f"template {path}: name is required")
    requires = doc.get("requires", [])
    if not isinstance(requires, list) or not all(isinstance(r, str) and r for r in requires):
        raise TemplateError(f"template {path}: requires must list preconditions (text)")
    parent_path, _ = parent_of(doc, path, repo)
    parent = load(parent_path)
    spec = copy.deepcopy(parent)
    for key in IDENTITY_SECTIONS:
        spec.pop(key, None)
    for key in IDENTITY_INPUTS + DERIVED:
        spec["inputs"].pop(key, None)
    spec["name"], spec["description"] = doc["name"], doc.get("description", "")
    for key in change.get("unset", []):
        node, last = _walk(spec, key, create=False)
        if node is not None:
            node.pop(last, None)
    for key, value in (change.get("set") or {}).items():
        node, last = _walk(spec, key, create=True)
        node[last] = copy.deepcopy(value)
    locked = doc.get("locked") or {}
    for key, item in parent_references(parent, "marginal" in spec).items():
        if f"inputs.{key}" in change.get("unset", []):
            continue
        pin = locked.get(key) or {}
        spec["inputs"][key] = dict(item, sha256=pin.get("sha256") if pin.get("path") == item["path"] else None)
    for key, item in (change.get("inputs") or {}).items():
        if not isinstance(item, dict) or "path" not in item or "sha256" not in item:
            raise TemplateError(f"template {path}: change.inputs.{key} needs path and sha256 (null until `lock`)")
        spec["inputs"][key] = dict(item)
    for section, ops in (change.get("flags") or {}).items():
        if not isinstance(spec.get(section), dict) or not isinstance(ops, dict):
            raise TemplateError(f"template {path}: change.flags.{section}: the resolved spec has no {section} section")
        spec[section]["flags"] = apply_flags(spec[section].get("flags", []), ops, f"{section}.flags")
    if "nav" not in spec or placed(spec, spec["nav"]["output"]) == placed(parent, parent["nav"]["output"]):
        raise TemplateError(f"template {path}: nav.output must be a new cell (it is the parent's)")
    return spec


def chain(path: Path, repo: Path) -> list[tuple[Path, dict, Path, bool]]:
    """The templates from `path` up its parent chain: (template file, doc, parent file, nominal?) each."""
    out, seen = [], set()
    p = Path(path).resolve()
    while p not in seen:
        seen.add(p)
        doc = read(p)
        if not is_template(doc):
            break
        parent, nominal = parent_of(doc, p, repo)
        out.append((p, doc, parent, nominal))
        p = parent
    return out


def file_sha256(path: Path) -> str:
    h = hashlib.sha256()
    with Path(path).open("rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def spec_digest(path: Path, repo: Path) -> str:
    """Review F-9: the content digest of the cell a spec file describes (see the module doc). A plain spec: its file's
    SHA-256. A template: SHA-256 of the compact JSON list [CHAIN_DIGEST, SHA-256 of the template, of its parent, ...,
    of the plain spec the chain rests on] (the parent, else the nominal parent while parent is null)."""
    links = chain(path, repo)
    if not links:
        return file_sha256(path)
    files = [p for p, _, _, _ in links] + [links[-1][2]]
    text = json.dumps([CHAIN_DIGEST] + [file_sha256(f) for f in files], separators=(",", ":"))
    return hashlib.sha256(text.encode()).hexdigest()


def fills(node, where: str = "") -> list[str]:
    """Every placeholder root still has to fill in a resolved spec."""
    if isinstance(node, dict):
        return [x for k, v in node.items() for x in fills(v, f"{where}.{k}" if where else str(k))]
    if isinstance(node, list):
        return [x for v in node for x in fills(v, where)]
    return [node] if isinstance(node, str) and node.startswith(FILL) else []


def run_refusal(spec: dict, spec_path: Path | None, repo: Path) -> str | None:
    """Why `run` refuses this spec (None: it may run): a template whose parent is null, an open precondition anywhere
    up its template chain, or a value left to fill."""
    why = []
    if spec_path is not None and Path(spec_path).is_file():
        links = chain(spec_path, repo)
        if links and links[0][3]:
            why.append(f"template parent is null (planned on the nominal parent {links[0][2].name}): set parent to the "
                       "last accepted cell's spec")
        why += [f"{p.name} requires: {r}" for p, doc, _, _ in links for r in doc.get("requires", [])]
    why += [f"unfilled value {x}" for x in fills(spec)]
    return ("run refused: " + "; ".join(why)) if why else None


def header_lines(spec: dict, spec_path: Path | None, repo: Path) -> list[str]:
    """Plan / status annotations of a template: its parent chain, open preconditions and values to fill."""
    if spec_path is None or not Path(spec_path).is_file():
        return []
    lines = []
    for p, doc, parent, nominal in chain(spec_path, repo):
        lines.append(f"# template {p.name}: parent " + (f"null -> planned on the nominal parent {parent.name} (root sets "
                                                        "parent to the last accepted cell's spec before `run`)"
                                                        if nominal else parent.name))
        lines += [f"# requires (run refuses while listed): {r}" for r in doc.get("requires", [])]
    lines += [f"# fill (root fills before `run`): {x}" for x in fills(spec)]
    return lines


def lock_template(doc: dict, spec: dict, sha, relock: bool = False) -> tuple[dict, list[str]]:
    """`lock` of a template: the pins of the inputs it adds (written into change.inputs), of the as-built fields dir it
    sets with a null manifest_sha256 (written into change.set) and of the inputs it derives (written into "locked",
    with the path they belong to); the parent's pins stay in the parent's file (lock the parent there)."""
    doc, notes = copy.deepcopy(doc), []
    fields, sets = spec.get("fields") or {}, (doc.get("change") or {}).get("set") or {}
    if "manifest_sha256" in fields and fields["manifest_sha256"] is None:     # an as-built fields dir not yet pinned
        target = sets["fields"] if isinstance(sets.get("fields"), dict) else None
        if target is None and "fields.manifest_sha256" not in sets:
            notes.append(f"parent pin null: fields ({fields['output']}): lock the parent spec")
        else:
            got = sha(f"{fields['output']}/manifest.json")
            if got is None:
                raise TemplateError(f"lock: fields manifest missing: {fields['output']}/manifest.json")
            if target is None:
                sets["fields.manifest_sha256"] = got
            else:
                target["manifest_sha256"] = got
            notes.append(f"locked fields: {fields['output']}/manifest.json {got}")
    own = (doc.get("change") or {}).get("inputs") or {}
    for key, item in spec["inputs"].items():
        if key in own:
            target = own[key]
        elif key in DERIVED:
            target = doc.setdefault("locked", {}).setdefault(key, {})
            if target.get("path") != item["path"]:
                target.clear()
                target.update(path=item["path"], sha256=None)
        else:
            if item.get("sha256") is None:
                notes.append(f"parent pin null: {key} ({item['path']}): lock the parent spec")
            continue
        got = sha(item["path"])
        if got is None:
            raise TemplateError(f"lock: input {key} missing: {item['path']}")
        want = target.get("sha256")
        if want is None:
            notes.append(f"locked {key}: {item['path']} {got}")
            target["sha256"] = got
        elif want != got:
            if not relock:
                raise TemplateError(f"lock: {key} pin {want} differs from the file ({got}); --relock to replace")
            notes.append(f"RELOCKED {key}: {item['path']} {want} -> {got}")
            target["sha256"] = got
    return doc, notes
