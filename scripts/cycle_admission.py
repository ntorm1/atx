"""Admission trials in the trial ledger (platform v8 review W1-C, C-7), written by research_cycle.py at the gate.

The v8 Appendix A block prints "admission trials this sprint <k>" from the ledger's admission lines on the research
window (backtest_integrity.appendix_a_v8), and nothing wrote such a line: every result printed 0. Now the gate of a v8
cycle (research_cycle.summ_protocol: a verdict spec, or --protocol v8 in summ.extra) with a ledger of record appends,
before it reads the admission, one chained line per candidate the gate lists (gate.admitted, the new members; the
report-only rows are no trial) that has a row in this cycle's admission.json:

  {schema, kind "admission", count 1, candidate, cycle, status, origin, window_id, window{label TRAIN, first_session,
   last_session}, pins{library_sha256, role_sha256, admission_sha256}, dsl_sha256, trial_id, prev_sha256}

origin is the alpha registry's class (contract K5, atx-impl/strategies/alphas/registry.json under the root), else the
spec's summ.origin. trial_id = (admission, SHA-256 of [candidate, dsl_sha256 or the library pin, role pin, window id]):
the same candidate screened again on the same role (a resumed gate, the full run after --screen, a later cycle) is
the same trial and adds nothing. Lines without a cell are skipped by research_ledger.cells.
"""
from __future__ import annotations

import hashlib
import json

import research_ledger
from cycle_verdict import listed_rows

KIND = "admission"
REGISTRY = "atx-impl/strategies/alphas/registry.json"


def registry_origins(cycle) -> dict:
    """{alpha id: origin} of the alpha registry under the cycle's root ({} when it is absent or unreadable)."""
    try:
        doc = cycle.res.read_json(REGISTRY)
    except ValueError:
        return {}
    alphas = (doc or {}).get("alphas") if isinstance(doc, dict) else None
    return {a["id"]: a.get("origin") for a in alphas or [] if isinstance(a, dict) and "id" in a}


def library_dsl(cycle) -> dict:
    """{candidate id: SHA-256 of its DSL} from the IC library JSON ({} when it holds no candidates)."""
    try:
        doc = cycle.res.read_json(cycle.ipath("library"))
    except ValueError:
        return {}
    rows = (doc or {}).get("candidates") if isinstance(doc, dict) else None
    return {c["id"]: hashlib.sha256(c["dsl"].encode()).hexdigest() for c in rows or []
            if isinstance(c, dict) and isinstance(c.get("id"), str) and isinstance(c.get("dsl"), str)}


def trial_id(cid: str, dsl_or_library_pin: str | None, role_pin: str | None, wid: str) -> str:
    """The admission trial id of one candidate (see the module doc): one rule for the gate and its callers (the wave
    driver's budget count)."""
    ident = json.dumps([cid, dsl_or_library_pin, role_pin, wid], separators=(",", ":"))
    return research_ledger.backtest_integrity().trial_id(KIND, hashlib.sha256(ident.encode()).hexdigest())


def admission_lines(cycle, w_dir: str) -> list[dict]:
    """The admission lines of the gate's listed candidates with a row in ``w_dir``/admission.json."""
    s = cycle.spec
    rel = f"{w_dir}/admission.json"
    doc = cycle.res.read_json(rel)
    rows = listed_rows((doc or {}).get("candidates", []) if isinstance(doc, dict) else [], s["gate"]["admitted"])
    bi = research_ledger.backtest_integrity()
    rw, wid = bi.research_window(), bi.window_id()
    window = {"label": "TRAIN", "first_session": bi.session_date(rw.TRAIN_BEGIN_NS).isoformat(),
              "last_session": bi.session_date(rw.TRAIN_END_NS - bi.DAY_NS).isoformat()}
    origins, dsl = registry_origins(cycle), library_dsl(cycle)
    lib, role = cycle.pin("library"), cycle.pin("role")
    out = []
    for row in rows:
        cid = row.get("id")
        origin = origins.get(cid) or (s.get("summ") or {}).get("origin")
        if origin not in bi.ORIGINS:
            raise ValueError(f"admission line {cid}: no origin class (contract K5) in the alpha registry ({REGISTRY}) "
                             "or summ.origin")
        out.append({"schema": bi.LEDGER_SCHEMA, "kind": KIND, "count": 1, "candidate": cid, "cycle": s["name"],
                    "status": row.get("status"), "origin": origin, "window_id": wid, "window": window,
                    "pins": {"library_sha256": lib, "role_sha256": role, "admission_sha256": cycle.res.sha(rel)},
                    "dsl_sha256": dsl.get(cid), "trial_id": trial_id(cid, dsl.get(cid) or lib, role, wid)})
    return out


def ledger_admissions(cycle, ledger: str, w_dir: str, log) -> int:
    """Append the gate's admission lines to ``ledger`` (chained; an identical line already there is skipped); the
    number appended. Raises ValueError on a broken ledger or a line without an origin class."""
    lines = admission_lines(cycle, w_dir)
    if not lines:
        return 0
    appended, skipped = research_ledger.backtest_integrity().ledger_append(cycle.res.path(ledger), lines, chain=True)
    log(f"   ledger {ledger}: {len(appended)} admission trial line(s) appended, {len(skipped)} already ledgered")
    return len(appended)
