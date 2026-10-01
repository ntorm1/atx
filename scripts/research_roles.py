"""The ``roles:`` loop of research_cycle.py (platform v8, task H-1): one research cycle over era shards.

A spec (``atx.research-cycle-spec/v1``) may list ``roles`` instead of ``inputs.role``, in date order:

  "roles": [{"id": "E1", "dir": "build-equity/era-2014-2016-lo1", "manifest_sha256": null,
             "begin": "2014-01-01", "end": "2017-01-01", "universe": "linked-operating-v1",
             "fields_dir": "build-equity/era-2014-2016-lo1-fields-v9", "fields_manifest_sha256": null}, ...]

  id              [A-Za-z0-9_]+ (it keys file names); dir holds the role manifest.json (pinned by manifest_sha256)
  begin, end      the era window [begin, end) of its scored sessions (the warm-up before begin is not part of it);
                  era_pool.check_windows at load time with TRAIN and the seal of research_window.current() (the JSON's
                  values): no overlap, date order, nothing past the seal, no window straddling the TRAIN begin. When
                  the manifest is readable its score_start_ns and score_end_ns must lie inside [begin, end].
  universe        optional, as inputs.role.universe (checked against the manifest)
  fields_dir      optional: an as-built fields dir for this era (pinned by fields_manifest_sha256, as a single-role
                  fields.manifest_sha256); without it the era's fields are built by the spec's fields builder
  lock            fills every null input, role and fields pin from the files (research_cycle.py lock)

One role: exactly the single-role Cycle of the derived spec (inputs.role from the entry; fields as built when
fields_dir is given): every plan, status and run line of a spec with inputs.role. A history role (end on or before the
TRAIN begin) adds the fitter's ``--era-id`` (the explicit pooled mask) and nav_summ's ``--pool`` (the ERA ledger rule);
when its summ ledgers, summ.extra must name the TRAIN cell it re-reads (``--era-of TRIAL_ID``, review P-1).

Two or more roles (RolesCycle): per era (keyed ``-<id>`` outputs, ``--role-id`` receipts) fields, u, w and nav; shared,
on the anchor (the last role, E3 when present): the static check (on the anchor's fields), the pooled fit (``--era``
per other era, ``--era-id`` anchor; one composition_weights.<id>.json per other era feeds its w pass), the gate and the
pooled summ (nav_summ ``--pool``, one ``--weights`` per era file, the ledger N by the pooled trial_id). Steps run
phase-major: fields, check, u, fit, gate, w, nav, summ; a step is named ``phase:<id>`` (step_key) and --stop-after
PHASE stops after every era's step of that phase. The candidate cache and the fit work store stay content-keyed by
role SHA. Refused with two or more roles (single-role phases or inputs): ref, card, marginal, monitor, compare,
fields.check, summ.cells, an as-built fields.manifest_sha256, inputs.reuse_fields, --reuse-fields, --keep-fields, and
an ic section without fit (the fit pools the eras).

research_cycle.py imports this module lazily (it imports research_cycle); era_pool and research_window come from
atx-engine/tools (standard library at import).
"""
from __future__ import annotations

import json
from pathlib import Path
import sys

HERE = Path(__file__).resolve().parent
ENGINE_TOOLS = HERE.parent / "atx-engine" / "tools"
for _p in (str(HERE), str(ENGINE_TOOLS)):
    if _p not in sys.path:
        sys.path.insert(0, _p)
import era_pool as EP  # noqa: E402
import research_cycle as RC  # noqa: E402
import research_window as rw  # noqa: E402

ROLE_REQUIRED = ("id", "dir", "begin", "end")
ROLE_OPTIONAL = ("manifest_sha256", "universe", "fields_dir", "fields_manifest_sha256")
PIN_KEYS = ("manifest_sha256", "fields_manifest_sha256")
PER_ERA = RC.ERA_PHASES                              # fields, u, w, nav: every era runs its own
ORDER = ("fields", "check", "u", "fit", "gate", "w", "nav", "summ")
ANCHOR_ONLY = ("static_check", "gate", "summ")       # sections the non-anchor eras' derived specs drop
MULTI_REFUSED = ("ref", "card", "marginal", "monitor", "compare")
UNLOCKED = "UNLOCKED (computed now; run `lock --write`)"


def usage(message: str) -> RC.CycleError:
    return RC.CycleError(f"spec roles: {message}", RC.EXIT_USAGE)


def window() -> dict:
    """TRAIN and the seal of the repository window, recomputed from its JSON (a harness bind cannot move them)."""
    return rw.current()


def era_ns(role: dict) -> tuple[int, int]:
    return rw.date_ns(role["begin"]), rw.date_ns(role["end"])


def is_history(role: dict) -> bool:
    """An era before TRAIN (its window ends on or before the TRAIN begin)."""
    return era_ns(role)[1] <= window()["TRAIN_BEGIN_NS"]


def validate_roles(spec: dict, keep_fields: bool = False, reuse_fields: str | None = None) -> list[dict]:
    """The roles of SPEC, checked (exit 2 on a refusal): shape, pins, windows and, with two or more roles, the
    single-role sections and options."""
    roles = spec.get("roles")
    if not isinstance(roles, list) or not roles:
        raise usage("must be a non-empty list of roles")
    if "role" in spec.get("inputs", {}):
        raise usage("inputs.role is absent when roles are given")
    for r in roles:
        if not isinstance(r, dict) or not all(k in r for k in ROLE_REQUIRED):
            raise usage(f"each role needs {', '.join(ROLE_REQUIRED)}")
        unknown = sorted(set(r) - set(ROLE_REQUIRED) - set(ROLE_OPTIONAL))
        if unknown:
            raise usage(f"role {r.get('id')}: unknown key(s) {', '.join(unknown)}")
        for key in ("dir", "universe", "fields_dir"):
            if key in r and not (isinstance(r[key], str) and r[key]):
                raise usage(f"role {r.get('id')}: {key} must be a non-empty string")
        for key in PIN_KEYS:
            if r.get(key) is not None and not (isinstance(r[key], str) and len(r[key]) == 64):
                raise usage(f"role {r.get('id')}: {key} must be a SHA-256 hex digest or null (until `lock`)")
        try:
            era_ns(r)
        except (TypeError, ValueError) as exc:
            raise usage(f"role {r.get('id')}: begin and end must be YYYY-MM-DD dates") from exc
    w = window()
    try:
        EP.check_windows([(r["id"], *era_ns(r)) for r in roles], w["TRAIN_BEGIN_NS"], w["TRAIN_END_NS"],
                         w["SEAL_NS"])
    except EP.PoolError as exc:
        raise usage(f"{exc} ({w['WINDOW_ID']}: TRAIN [{w['TRAIN_BEGIN_DATE']}, {w['TRAIN_END_DATE']}), seal "
                    f"{w['SEAL_DATE']})") from exc
    if len(roles) > 1:
        refused = [k for k in MULTI_REFUSED if k in spec]
        f, sm = spec.get("fields", {}), spec.get("summ", {})
        refused += [k for k, bad in (("fields.check", "check" in f), ("summ.cells", "cells" in sm),
                                     ("fields.manifest_sha256", bool(f.get("manifest_sha256"))),
                                     ("inputs.reuse_fields", "reuse_fields" in spec.get("inputs", {})),
                                     ("--reuse-fields", bool(reuse_fields)), ("--keep-fields", keep_fields)) if bad]
        if refused:
            raise usage(f"{', '.join(refused)} are single-role; refused with {len(roles)} roles")
        if "ic" in spec and "fit" not in spec:
            raise usage("a cycle of two or more roles pools the eras in the fit: the spec needs fit")
    return roles


def fields_pin(res: RC.Resolver, role: dict) -> str:
    """The pin of an era's as-built fields dir: fields_manifest_sha256, else the file's (exit 3 when missing)."""
    rel = f"{role['fields_dir'].rstrip('/')}/manifest.json"
    pin = role.get("fields_manifest_sha256") or res.sha(rel)
    if pin is None:
        raise RC.CycleError(f"role {role['id']}: fields manifest missing: {rel}", RC.EXIT_PIN)
    return pin


def derived_spec(spec: dict, role: dict, res: RC.Resolver, anchor: bool = True) -> dict:
    """The single-role spec of one era: inputs.role from the entry, the fields as built when fields_dir is given, and
    (a non-anchor era of several) without the static check, gate and summ, which run on the anchor."""
    out = json.loads(json.dumps({k: v for k, v in spec.items() if k != "roles"}))
    item = {"dir": role["dir"], "path": f"{role['dir'].rstrip('/')}/manifest.json", "sha256": role.get("manifest_sha256")}
    if role.get("universe"):
        item["universe"] = role["universe"]
    out["inputs"]["role"] = item
    if role.get("fields_dir"):
        fields = {"output": role["fields_dir"], "manifest_sha256": fields_pin(res, role)}
        if "list" in spec.get("fields", {}):
            fields["list"] = list(spec["fields"]["list"])
        out["fields"] = fields
    if not anchor:
        for key in ANCHOR_ONLY:
            out.pop(key, None)
    RC.validate_spec(out)
    return out


def check_scored_window(cycle: RC.Cycle, role: dict) -> None:
    """The role manifest's scored window [score_start_ns, score_end_ns] lies inside the era's [begin, end]."""
    doc = cycle.res.read_json(cycle.ipath("role"))
    if not isinstance(doc, dict) or type(doc.get("score_start_ns")) is not int or type(doc.get("score_end_ns")) is not int:
        return                                   # hash-only (a fixture) or no score window: nothing to check
    begin, end = era_ns(role)
    if not begin <= doc["score_start_ns"] < doc["score_end_ns"] <= end:
        raise usage(f"role {role['id']}: its manifest scores [{doc['score_start_ns']}, {doc['score_end_ns']}) ns, "
                    f"outside the era window [{role['begin']}, {role['end']})")


def fit_weights_dir(cycle: RC.Cycle) -> str:
    return cycle.out(cycle.spec["fit"]["output"], keyed=False) if "fit" in cycle.spec else ""


def check_history_ledger(cycle: RC.Cycle, role: dict) -> None:
    """Review P-1: a history read on one role that ledgers its summ names the TRAIN cell it re-reads (nav_summ
    ``--era-of TRIAL_ID`` in summ.extra, written as the line's era_of); refused at plan time, before any step runs."""
    sm = cycle.spec["summ"]
    if (cycle.ledger or sm.get("ledger")) and RC.option_value(sm.get("extra", []), "--era-of") is None:
        raise usage(f"role {role['id']}: a history read on one role re-reads a TRAIN cell and its ledger line names it: "
                    "add --era-of <that cell's trial_id> to summ.extra (review P-1)")


def roles_cycle(spec: dict, res: RC.Resolver, **kw):
    """The cycle of a roles: spec: one role is its single-role Cycle; two or more a RolesCycle."""
    roles = validate_roles(spec, kw.get("keep_fields", False), kw.get("reuse_fields"))
    if RC.option_value(spec.get("summ", {}).get("extra", []), "--era-of") is not None and (
            len(roles) > 1 or not is_history(roles[0])):
        raise usage("--era-of in summ.extra names the TRAIN cell of a history read on one role (review P-1); a pool "
                    "of two or more roles is its own trial and a role inside TRAIN is the cell itself")
    if len(roles) > 1:
        return RolesCycle(spec, roles, res, **kw)
    role = roles[0]
    cycle = RC.Cycle(derived_spec(spec, role, res), res, **kw)
    check_scored_window(cycle, role)
    if is_history(role):    # the fitter's explicit pooled mask and the ERA ledger rule (a one-era pool)
        if "fit" in cycle.spec:
            cycle.fit_pool = (["--era-id", role["id"]], [])
        if "summ" in cycle.spec:
            check_history_ledger(cycle, role)
            cycle.summ_pool = [(role["id"], cycle.out(cycle.spec["nav"]["output"]),
                                f"{fit_weights_dir(cycle)}/{cycle.weights_name}")]
    return cycle


class RolesCycle:
    """Two or more era roles run as one cycle (see the module doc). Duck-typed as research_cycle.Cycle for plan_lines,
    status_lines, run_cycle and cycle_verdict: every attribute it does not define is the anchor era's."""

    def __init__(self, spec: dict, roles: list[dict], res: RC.Resolver, **kw):
        self.roles, self.eras = roles, []
        for k, role in enumerate(roles):
            anchor = k == len(roles) - 1
            cycle = RC.Cycle(derived_spec(spec, role, res, anchor), res, role_key=role["id"], **kw)
            check_scored_window(cycle, role)
            if not anchor:
                cycle.weights_name = EP.era_weights_name(role["id"])
            self.eras.append(cycle)
        self.anchor = self.eras[-1]
        self.spec = self.anchor.spec
        self.pins = {key: pin for key, pin in self.anchor.pins.items() if key != "role"}
        for cycle, role in zip(self.eras, roles):
            self.pins[f"role:{role['id']}"] = cycle.pins["role"]
            if role.get("fields_dir"):
                rel = f"{role['fields_dir'].rstrip('/')}/manifest.json"
                self.pins[f"fields:{role['id']}"] = (rel, fields_pin(res, role), "locked (fields_manifest_sha256)"
                                                     if role.get("fields_manifest_sha256") else UNLOCKED)

    def __getattr__(self, name: str):
        if name in ("anchor", "eras"):           # not yet set (during __init__): no recursion
            raise AttributeError(name)
        return getattr(self.anchor, name)

    def fit_pool(self, per: dict) -> tuple[list[str], list[str]]:
        """The anchor fit's ``--era ID ROLE ROLE_SHA ORIENT ORIENT_SHA SUMMARY SUMMARY_SHA`` per other era (from its
        u pass) and ``--era-id``, and the files the bounded runner binds."""
        argv, binds = [], []
        for cycle in self.eras[:-1]:
            u = next(st for st in per[cycle.role_key] if st.phase == "u")
            orient, summary, role_m = f"{u.output}/orientations.json", f"{u.output}/summary.json", cycle.ipath("role")
            argv += ["--era", cycle.role_key, role_m, cycle.pin("role"), orient, cycle.rt_sha(orient), summary,
                     cycle.rt_sha(summary)]
            binds += [role_m, orient, summary]
        return argv + ["--era-id", self.anchor.role_key], binds

    def steps(self) -> list[RC.Step]:
        anchor = self.anchor
        per = {cycle.role_key: cycle.steps() for cycle in self.eras[:-1]}
        if "fit" in anchor.spec:
            anchor.fit_pool = self.fit_pool(per)
        if "summ" in anchor.spec:
            w_dir = fit_weights_dir(anchor)
            anchor.summ_pool = [(c.role_key, c.out(c.spec["nav"]["output"]), f"{w_dir}/{c.weights_name}")
                                for c in self.eras]
        per[anchor.role_key] = anchor.steps()
        out = []
        for phase in ORDER:
            owners = self.eras if phase in PER_ERA else [anchor]
            for cycle in owners:
                for st in per[cycle.role_key]:
                    if st.phase == phase:
                        st.cycle, st.role = cycle, cycle.role_key if phase in PER_ERA else None
                        out.append(st)
        return out


def lock(spec: dict, root: Path, relock: bool = False) -> tuple[dict, list[str]]:
    """research_cycle.py lock for a roles: spec: every input pin, then each role's manifest and fields pins."""
    roles = validate_roles(spec)
    res = RC.Resolver(root)
    notes: list[str] = []
    for key, item in spec["inputs"].items():
        RC.lock_pin(res, item, "sha256", item["path"], key, relock, notes)
    for role in roles:
        RC.lock_pin(res, role, "manifest_sha256", f"{role['dir'].rstrip('/')}/manifest.json", f"role {role['id']}",
                    relock, notes)
        if role.get("fields_dir"):
            RC.lock_pin(res, role, "fields_manifest_sha256", f"{role['fields_dir'].rstrip('/')}/manifest.json",
                        f"fields {role['id']}", relock, notes)
    for k, role in enumerate(roles):
        derived_spec(spec, role, res, k == len(roles) - 1)
    return spec, notes
