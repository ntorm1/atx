"""cycle_verdict.json of one research cycle (platform v8 lane A, task A-2), written by research_cycle.py.

  {schema, cycle, mode, spec_sha256,
   admission[]   the admission.json rows of the gate's listed candidates (every row without a gate),
   marginal[]    the marginal_ic.json rows (contract K6) of the same ids; marginal_note when the phase was skipped,
   paired{dsr, se, cbb_ci, lw_p}, dsr{n, cell_count, effective_n, ...}, pbo
                 only after a full run whose summ wrote nav_summ --json / --pbo-json into the cycle dir (spec
                 "verdict": true); dsr is the pre-registered DSR (v8-prereg item 3, review C-1): nav_summ
                 --dsr-ledger's ``deflated_ledger`` (N and V[SR] from the sprint ledger of record, never from a cell
                 count), the legacy-variance DSR beside it (gates nothing); a summ row without it is refused,
   phases[{name, seconds, peak_mib}]  from each phase's bounded-runner receipt, else the cycle's own wall clock
                 (peak_mib null) for a direct phase run by this invocation,
   ledger{path, head, lines}  the sprint ledger of record's chain head when the cycle has a ledger (review C-6)}

``keep`` (research_cycle --keep-verdicts, P9 OR section 3): every write also leaves the same bytes in
<cycle dir>/verdicts/<mode>-<k>.json (k the first free number; written once, never overwritten), so a receipt that
pins a verdict's SHA-256 never dangles when a later run rewrites cycle_verdict.json.

The cycle is duck-typed (research_cycle.Cycle, or research_roles.RolesCycle): spec, res, screen, steps(), receipt(),
cycle_dir(). A step of one era of a roles: cycle (task H-1) is keyed ``phase:role`` (``step_key``) in the phase rows;
the scoring blocks are read for the last NAV step (a pooled summ's last JSON row is the pooled row).
"""
from __future__ import annotations

import json
from pathlib import Path

VERDICT = "cycle_verdict.json"
VERDICT_SCHEMA = "atx.cycle-verdict/v1"
VERDICTS = "verdicts"                  # <cycle dir>/verdicts/<mode>-<k>.json: the per-run copies (keep)
SUMM_JSON, PBO_JSON = "summ.json", "pbo.json"     # nav_summ --json / --pbo-json targets in the cycle dir


class VerdictError(ValueError):
    """A verdict that cannot be written as pre-registered (research_cycle.py turns it into a hard stop)."""


def step_key(st) -> str:
    """A step's name in plans, logs, timings and phase rows: its phase, or ``phase:role`` for an era's step (H-1)."""
    role = getattr(st, "role", None)
    return f"{st.phase}:{role}" if role else st.phase


def listed_rows(rows: list, ids: list[str] | None) -> list:
    """The rows (dicts with an id) of the listed ids in listed order, or every row when no list is given."""
    rows = [r for r in rows if isinstance(r, dict)]
    if ids is None:
        return rows
    by_id = {r.get("id"): r for r in rows}
    return [by_id[i] for i in ids if i in by_id]


def phase_rows(cycle, steps: dict, timings: dict) -> list[dict]:
    out = []
    for key, st in steps.items():   # the receipt of the attempt run now (an always-run phase moved on to a fresh dir)
        ran = timings.get(key) or {}
        r = cycle.receipt(ran.get("run_dir") or st.run_dir)
        if r is not None:
            out.append({"name": key, "seconds": r.get("wall_seconds"),
                        "peak_mib": (r.get("sampled_peak_tree_rss_bytes") or 0) >> 20})
        elif ran:
            out.append({"name": key, "seconds": ran["seconds"], "peak_mib": None})
    return out


def scoring_blocks(res, cycle_dir: str, nav_out: str) -> dict:
    """paired, dsr and pbo from nav_summ's --json (a list of per-dir results: this cycle's NAV dir, else the last)
    and --pbo-json; {} when the summ wrote no JSON."""
    summ = res.read_json(f"{cycle_dir}/{SUMM_JSON}")
    if not isinstance(summ, list) or not summ:
        return {}
    mine = [r for r in summ if isinstance(r, dict) and Path(str(r.get("dir", ""))).as_posix() == Path(nav_out).as_posix()]
    row = mine[-1] if mine else summ[-1]
    p = row.get("paired") or {}
    pbo = res.read_json(f"{cycle_dir}/{PBO_JSON}")
    return {"paired": {"dsr": p.get("dsr"), "se": p.get("memmel_se"), "cbb_ci": p.get("cbb_ci95"),
                       "lw_p": (p.get("lw") or {}).get("p_value")},
            "dsr": dsr_block(row),
            "pbo": pbo.get("pbo") if isinstance(pbo, dict) else None}


def dsr_block(row: dict) -> dict:
    """The verdict's DSR (review C-1): nav_summ --dsr-ledger's ``deflated_ledger`` of the row, i.e. N = the ledger's
    trial count and V[SR] = the cross-trial variance of the cells ledgered on the research window (v8-prereg item 3).
    ``cell_count`` is that DSR; the legacy-variance DSR is reported beside it and gates nothing. The DSR of
    ``deflated`` (V from the listed dirs, or Lo's single-cell variance) is never read: a row without
    ``deflated_ledger`` raises VerdictError."""
    dl = row.get("deflated_ledger")
    if not isinstance(dl, dict):
        raise VerdictError(f"summ row {row.get('dir')!r} has no deflated_ledger: the verdict DSR is computed only from "
                           "the sprint ledger of record (nav_summ --dsr-ledger; v8-prereg item 3), never from a cell "
                           "count")
    de = row.get("deflated_effective_n") or {}
    return {"n": dl.get("n"), "cell_count": dl.get("dsr"), "effective_n": de.get("dsr"),
            "variance_sr": dl.get("variance_sr"), "variance_cells": dl.get("cells"), "window_id": dl.get("window_id"),
            "legacy": {"dsr": dl.get("legacy_dsr"), "cells": dl.get("legacy_cells"),
                       "note": "legacy variance (ledger lines without a window_id): reported, gates nothing"},
            "source": "nav_summ --dsr-ledger (deflated_ledger)"}


def verdict(cycle, timings: dict, spec_sha256: str | None, ledger: dict | None = None) -> dict:
    """The verdict document; ``ledger`` = {path, head, lines} of the sprint ledger of record when the cycle has one
    (review C-6: the chain head as this cycle left it, so a later edit of the ledger's tail is detected)."""
    s, res = cycle.spec, cycle.res
    steps = {step_key(st): st for st in cycle.steps()}
    ids = s["gate"]["admitted"] if "gate" in s else None
    doc = {"schema": VERDICT_SCHEMA, "cycle": s["name"], "mode": "screen" if cycle.screen else "run",
           "spec_sha256": spec_sha256}
    fit = steps.get("fit")
    adm = res.read_json(f"{fit.output}/admission.json") if fit and fit.output else None
    doc["admission"] = listed_rows((adm or {}).get("candidates", []), ids)
    m = steps.get("marginal")
    mdoc = res.read_json(f"{m.output}/marginal_ic.json") if m and m.output and m.state == "done" else None
    doc["marginal"] = listed_rows(mdoc if isinstance(mdoc, list) else (mdoc or {}).get("candidates", []), ids)
    if m is not None and m.state == "skipped":
        doc["marginal_note"] = m.note
    doc["phases"] = phase_rows(cycle, steps, timings)
    navs = [st for st in steps.values() if st.phase == "nav"]
    if not cycle.screen and s.get("verdict") and navs:
        doc.update(scoring_blocks(res, cycle.cycle_dir(), navs[-1].output))
    if ledger is not None:
        doc["ledger"] = ledger
    return doc


def keep_copy(cycle, mode: str, text: str) -> str:
    """Write ``text`` to <cycle dir>/verdicts/<mode>-<k>.json, k the first free number (exclusive create: a copy is
    never overwritten); returns its root-relative path."""
    d = cycle.res.path(f"{cycle.cycle_dir()}/{VERDICTS}")
    d.mkdir(parents=True, exist_ok=True)
    k = 1
    while True:
        try:
            with (d / f"{mode}-{k}.json").open("x", encoding="utf-8", newline="\n") as f:
                f.write(text)
            return f"{cycle.cycle_dir()}/{VERDICTS}/{mode}-{k}.json"
        except FileExistsError:
            k += 1


def write_verdict(cycle, timings: dict, spec_sha256: str | None, log, ledger: dict | None = None,
                  keep: bool = False) -> dict:
    path = cycle.res.path(f"{cycle.cycle_dir()}/{VERDICT}")
    path.parent.mkdir(parents=True, exist_ok=True)
    doc = verdict(cycle, timings, spec_sha256, ledger)
    text = json.dumps(doc, indent=2) + "\n"
    path.write_text(text, encoding="utf-8", newline="\n")
    copy = keep_copy(cycle, doc["mode"], text) if keep else None
    for r in doc["marginal"]:
        log(f"marginal {r.get('id')}: ic21 {r.get('ic21')} (HAC t {r.get('ic21_hac_t')}); marginal ic21 "
            f"{r.get('marginal_ic21')} (HAC t {r.get('marginal_hac_t')}); max |rho| {r.get('max_abs_rho')} with "
            f"{r.get('max_rho_member')}")
    if doc.get("marginal_note"):
        log(f"marginal: {doc['marginal_note']}")
    log(f"== verdict {cycle.cycle_dir()}/{VERDICT}" + (f" (kept as {copy})" if copy else ""))
    return doc
