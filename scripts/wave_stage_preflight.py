"""The preflight stage of a research wave (wave_stages.py): pins, seal, ledger, the admission budget (the hand
count) and the code pathspec, before anything runs.
"""
from __future__ import annotations

import re

import cycle_admission
import research_ledger
import research_spec
import research_tree
import wave_queue
from wave_context import Wave
from wave_stage_util import EXIT_PIN, StageError, library_wave

import research_window as RW   # atx-engine/tools (wave_context puts it on the path): the one source of the seal

YEAR = re.compile(r"(?<![0-9])((?:19|20)[0-9]{2})(?![0-9])")
LIBRARIES = "atx-impl/strategies/libraries"     # add-alpha's library definitions (a cycle's name is its library's)


# ------------------------------------------------------------------ preflight
def preflight_inputs(w: Wave, done: dict) -> dict:
    m = w.manifest
    parent = m["parent"]["spec"]
    out = {"manifest_sha256": w.manifest_sha,
           "parent_spec_digest": w.spec_digest(parent) if w.exists(parent) else None,
           "fields_manifest_sha256": w.sha(f"{m['fields']['dir']}/manifest.json")}
    if not library_wave(w):
        out["template_sha256"] = w.sha(m["rule_cell"]["template"])
    return out


def sealed_years(text: str) -> list[str]:
    return sorted({y for y in YEAR.findall(text) if int(y) >= RW.FIRST_SEALED_YEAR})


def admission_trial_id(cid: str, dsl_sha: str, role_sha: str | None) -> str:
    """The trial_id the gate ledgers for one screened string, by the gate's own rule (cycle_admission.trial_id:
    [candidate, its DSL SHA-256, the role pin, the research window id]), so a string ledgered already is no new
    trial."""
    return cycle_admission.trial_id(cid, dsl_sha, role_sha, research_ledger.backtest_integrity().window_id())


def role_pin(w: Wave) -> str | None:
    """The role pin of the wave's cells: the parent's inputs.role (add-alpha keeps it), else the file's SHA-256."""
    role = w.load_spec(w.manifest["parent"]["spec"])["inputs"].get("role") or {}
    return role.get("sha256") or (w.sha(role["path"]) if role.get("path") else None)


def rescreens_of(w: Wave, cycle, cache: dict) -> set:
    """The re-screen strings of a cycle's library: libraries/<cycle>.json "rescreens" (a cycle is named after its
    library; a -gm copy, e.g. v8x3b-gm, after its library plus -gm); none without the file."""
    if cycle not in cache:
        names = [str(cycle)] + ([str(cycle)[:-3]] if str(cycle).endswith("-gm") else []) if cycle else []
        doc = next((d for d in (w.read_json(f"{LIBRARIES}/{n}.json") for n in names) if isinstance(d, dict)), None)
        cache[cycle] = set((doc or {}).get("rescreens") or [])
    return cache[cycle]


def budget_selects(b: dict, origin) -> bool:
    return "admission_origin" not in b or origin == b["admission_origin"]


def admission_used(w: Wave, records: list[dict], b: dict) -> int:
    """The admission trials the budget has spent (the hand count): the ledger's admission lines of cycles with the
    budget's prefix and, when set, its origin class; a re-screen line (its candidate in the cycle library's
    ``rescreens``: Ruling R2-e, 0 admission trials, e.g. X-4's nine) is left out, as the hand count leaves it out."""
    bi = research_ledger.backtest_integrity()
    cache: dict = {}
    return sum(c for r, c in zip(records, bi.trial_counts(records))
               if r.get("kind") == "admission" and str(r.get("cycle", "")).startswith(b["admission_cycle_prefix"])
               and budget_selects(b, r.get("origin")) and r.get("candidate") not in rescreens_of(w, r.get("cycle"), cache))


def admission_new(w: Wave, records: list[dict], b: dict) -> list[str]:
    """The wave's strings that are new admission trials: not a re-screen, of the budget's origin class, and whose
    trial_id is not in the ledger yet (a fresh state dir of a screened wave adds nothing)."""
    have = {r.get("trial_id") for r in records}
    role = role_pin(w)
    return [c["id"] for c in w.manifest.get("candidates") or []
            if not c.get("rescreen") and budget_selects(b, c["origin"]) and
            admission_trial_id(c["id"], c["dsl_sha256"], role) not in have]


def ledger_state(w: Wave) -> tuple[list[dict], dict]:
    bi = research_ledger.backtest_integrity()
    p = w.path(w.manifest["ledger"])
    try:
        records = bi.ledger_read(p)
        head = bi.ledger_head(p)
    except (OSError, ValueError) as exc:
        raise StageError(f"ledger {w.manifest['ledger']}: {exc}", EXIT_PIN) from exc
    return records, {"path": w.manifest["ledger"], "lines": len(records), "head": head,
                     "n": bi.ledger_n(records, True)}


def budget_check(w: Wave, records: list[dict], n_before: int) -> tuple[dict, list[str]]:
    b = w.manifest["budget"]
    out, problems = {"id": b["id"]}, []
    if "admission_cap" in b:
        used, new = admission_used(w, records, b), admission_new(w, records, b) if library_wave(w) else []
        out.update(admission_used=used, admission_new=len(new), admission_new_ids=new, admission_cap=b["admission_cap"],
                   admission_cycle_prefix=b["admission_cycle_prefix"], admission_origin=b.get("admission_origin"))
        if used + len(new) > b["admission_cap"]:
            problems.append(f"budget {b['id']}: {used} admission trials used + {len(new)} new > cap "
                            f"{b['admission_cap']} (a ruling raises the cap; the wave does not)")
    if "construction_cap" in b:
        out["construction_cap"] = b["construction_cap"]
        if n_before + 1 > b["construction_cap"]:
            problems.append(f"budget {b['id']}: N {n_before} + this cell > construction cap {b['construction_cap']}")
    return out, problems


def fields_check(w: Wave) -> tuple[dict, list[str]]:
    f = w.manifest["fields"]
    rel = f"{f['dir']}/manifest.json"
    got, problems = w.sha(rel), []
    if got != f["manifest_sha256"]:
        return {}, [f"fields {rel} is {got}, the manifest pins {f['manifest_sha256']}"]
    doc = w.read_json(rel) or {}
    end = (doc.get("seal") or {}).get("exclusive_end")
    if doc.get("status") != "complete":
        problems.append(f"fields {rel}: status {doc.get('status')!r}, not complete")
    if not isinstance(end, str) or end > RW.SEAL_DATE:
        problems.append(f"fields {rel}: seal.exclusive_end {end!r} is not on or before the research seal "
                        f"{RW.SEAL_DATE} ({RW.WINDOW_ID})")
    names = [row.get("name") for row in doc.get("fields", []) if isinstance(row, dict)]
    for c in w.manifest.get("candidates") or []:
        miss = [x for x in c.get("fields", []) if x not in names]
        if miss:
            problems.append(f"candidate {c['id']}: fields {miss} are not in {rel}")
    return {"dir": f["dir"], "manifest_sha256": got, "rows": len(names), "seal_exclusive_end": end}, problems


def preflight(w: Wave, done: dict, log) -> dict:
    m, problems = w.manifest, []
    commit = w.committed(w.manifest_rel)
    if commit is None:
        problems.append(f"manifest {w.manifest_rel} is not committed as it is: commit it first (its commit is the "
                        "wave's pre-registration)")
    dirty = w.dirty_code()
    if dirty:
        problems.append(f"the code pathspec is dirty ({', '.join(dirty[:6])}): the bounded runner refuses; commit first")
    parent = m["parent"]["spec"]
    if not w.exists(parent):
        raise StageError(f"parent spec {parent} not found", EXIT_PIN)
    spec = w.load_spec(parent)
    refusal = research_spec.run_refusal(spec, w.path(parent), research_tree.REPO)
    if refusal:
        problems.append(f"parent {parent} is not a cell that ran: {refusal}")
    outs = w.outputs(parent)
    if not w.exists(f"{outs['nav']}/summary.json"):
        problems.append(f"parent cell {parent} has no NAV output {outs['nav']}/summary.json")
    if outs["ledger"] != m["ledger"]:
        problems.append(f"the parent's summ.ledger {outs['ledger']!r} is not the wave's ledger {m['ledger']!r}")
    fields, more = fields_check(w)
    problems += more
    for where in (m["fields"]["dir"], parent, m["out_dir"], m.get("library") or ""):
        years = sealed_years(where)
        if years:
            problems.append(f"{where} names sealed year(s) {years} (seal {RW.SEAL_DATE})")
    records, led = ledger_state(w)
    if led["n"] != m["expect"]["n_before"]:
        problems.append(f"ledger N is {led['n']}, the wave was planned on N {m['expect']['n_before']} (another cell "
                        "was ledgered first: re-pin expect.n_before)")
    budget, more = budget_check(w, records, led["n"])
    problems += more
    problems += wave_queue.queue_check(w.root, m)
    if not library_wave(w):
        rc = m["rule_cell"]
        doc = w.read_json(rc["template"]) if w.exists(rc["template"]) else None
        if w.sha(rc["template"]) != rc["template_sha256"] or not research_spec.is_template(doc):
            problems.append(f"rule_cell.template {rc['template']} is not the pinned cell template")
    if problems:
        raise StageError("preflight refused: " + "; ".join(problems), EXIT_PIN)
    log(f"   preflight: manifest commit {(commit or '')[:12]}, parent {outs['name']} (L {outs['leverage']}), fields "
        f"{fields['manifest_sha256'][:12]}, ledger {led['lines']} lines N {led['n']}, budget {budget}")
    return {"manifest": {"path": w.manifest_rel, "sha256": w.manifest_sha, "commit": commit},
            "parent": dict(outs, library=m["parent"]["library"], spec_digest=w.spec_digest(parent),
                           python=w.python),
            "fields": fields, "ledger": {"path": led["path"], "lines": led["lines"], "head": led["head"],
                                         "n_before": led["n"]},
            "budget": budget, "window": {"id": RW.WINDOW_ID, "seal": RW.SEAL_DATE}}


def preflight_plan(w: Wave, done: dict) -> list[str]:
    return [f"#   checks: manifest committed; parent {w.manifest['parent']['spec']} loads and has its NAV; fields pin; "
            f"seal {RW.SEAL_DATE}; ledger chain, N == {w.manifest['expect']['n_before']}; budget; code pathspec clean"]
