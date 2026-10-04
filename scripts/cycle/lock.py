"""``research_cycle.py lock`` (split move-only into scripts/cycle/ by P9 lane E2): every input pin computed from its
file, a template's in its change (research_spec.lock_template), and with --exes the exes the cell runs (P9 OR-2)."""
from __future__ import annotations

import json
from pathlib import Path

import research_spec
from cycle.resolver import Resolver
from cycle.spec import EXES_PIN, EXIT_PIN, CycleError, as_built, effective_exes, load_spec, validate_spec


# ------------------------------------------------------------------ lock
def lock(spec_path: Path, root: Path, relock: bool = False, exes: bool = False) -> tuple[dict, list[str]]:
    """The spec with every input pin (and a null as-built fields.manifest_sha256) computed from its file; a template
    gets the pins of the inputs it adds or derives (research_spec.lock_template), its parent keeps its own. ``exes``
    (``lock --exes``, P9 OR-2) also pins the exes the cell runs (lock_exes)."""
    spec, notes = lock_inputs(spec_path, root, relock)
    if exes:
        lock_exes(spec, spec_path, Resolver(root), notes)
    return spec, notes


def lock_exes(spec: dict, spec_path: Path, res: Resolver, notes: list[str]) -> None:
    """P9 OR-2: exes_sha256 = {exe key: SHA-256} of every exe the cell runs (effective_exes of the resolved spec), always
    the files as they are now (a pin that moves is noted RELOCKED: re-pinning the build is the point of the call); a
    template's goes into change.set, over the pins its parent's spec carries. A missing exe is exit 3."""
    template = research_spec.is_template(spec)
    exes = effective_exes(load_spec(spec_path) if template else spec)
    if not exes:
        raise CycleError("lock --exes: the spec names no exes", EXIT_PIN)
    target = spec.setdefault("change", {}).setdefault("set", {}) if template else spec
    have, pins = target.get(EXES_PIN) or {}, {}
    for key, rel in sorted(exes.items()):
        got = res.sha(rel)
        if got is None:
            raise CycleError(f"lock --exes: exe {key} missing: {rel}", EXIT_PIN)
        old = have.get(key)
        if old != got:
            notes.append(f"RELOCKED exe {key}: {rel} {old} -> {got}" if old else f"locked exe {key}: {rel} {got}")
        pins[key] = got
    target[EXES_PIN] = pins


def lock_inputs(spec_path: Path, root: Path, relock: bool = False) -> tuple[dict, list[str]]:
    """``lock`` without --exes: the input pins (see lock)."""
    spec = json.loads(Path(spec_path).read_text(encoding="utf-8"))
    if "roles" in spec:                    # H-1: the inputs and every role's role and fields pins
        import research_roles  # noqa: PLC0415  (imports this module)
        return research_roles.lock(spec, root, relock)
    res = Resolver(root)
    if research_spec.is_template(spec):
        try:
            return research_spec.lock_template(spec, load_spec(spec_path), res.sha, relock)
        except research_spec.TemplateError as exc:
            raise CycleError(str(exc), EXIT_PIN) from exc
    validate_spec(spec)
    notes: list[str] = []
    f = spec.get("fields")
    if as_built(f) and f["manifest_sha256"] is None:
        got = res.sha(f"{f['output']}/manifest.json")
        if got is None:
            raise CycleError(f"lock: fields manifest missing: {f['output']}/manifest.json", EXIT_PIN)
        f["manifest_sha256"] = got
        notes.append(f"locked fields: {f['output']}/manifest.json {got}")
    for key, item in spec["inputs"].items():
        lock_pin(res, item, "sha256", item["path"], key, relock, notes)
    return spec, notes


def lock_pin(res: Resolver, item: dict, field: str, rel: str, key: str, relock: bool, notes: list[str]) -> None:
    """item[field] = the SHA-256 of the file ``rel``: a null pin is filled, an equal one kept, a different one
    replaced only with ``relock`` (else exit 3); a missing file is exit 3."""
    got = res.sha(rel)
    if got is None:
        raise CycleError(f"lock: input {key} missing: {rel}", EXIT_PIN)
    if item.get(field) in (None, got):
        if item.get(field) is None:
            notes.append(f"locked {key}: {rel} {got}")
        item[field] = got
    elif relock:
        notes.append(f"RELOCKED {key}: {rel} {item[field]} -> {got}")
        item[field] = got
    else:
        raise CycleError(f"lock: {key} pin {item[field]} differs from the file ({got}); --relock to replace",
                         EXIT_PIN)
