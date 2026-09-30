"""cycle_verdict.json of one research cycle (platform v8 lane A, task A-2), written by research_cycle.py.

  {schema, cycle, mode, spec_sha256,
   admission[]   the admission.json rows of the gate's listed candidates (every row without a gate),
   marginal[]    the marginal_ic.json rows (contract K6) of the same ids; marginal_note when the phase was skipped,
   paired{dsr, se, cbb_ci, lw_p}, dsr{n, cell_count, effective_n}, pbo
                 only after a full run whose summ wrote nav_summ --json / --pbo-json into the cycle dir (spec
                 "verdict": true),
   phases[{name, seconds, peak_mib}]  from each phase's bounded-runner receipt, else the cycle's own wall clock
                 (peak_mib null) for a direct phase run by this invocation}

The cycle is duck-typed (research_cycle.Cycle): spec, res, screen, steps(), receipt(), cycle_dir().
"""
from __future__ import annotations

import json
from pathlib import Path

VERDICT = "cycle_verdict.json"
VERDICT_SCHEMA = "atx.cycle-verdict/v1"
SUMM_JSON, PBO_JSON = "summ.json", "pbo.json"     # nav_summ --json / --pbo-json targets in the cycle dir


def listed_rows(rows: list, ids: list[str] | None) -> list:
    """The rows (dicts with an id) of the listed ids in listed order, or every row when no list is given."""
    rows = [r for r in rows if isinstance(r, dict)]
    if ids is None:
        return rows
    by_id = {r.get("id"): r for r in rows}
    return [by_id[i] for i in ids if i in by_id]


def phase_rows(cycle, steps: dict, timings: dict) -> list[dict]:
    out = []
    for st in steps.values():   # the receipt of the attempt run now (an always-run phase moved on to a fresh dir)
        ran = timings.get(st.phase) or {}
        r = cycle.receipt(ran.get("run_dir") or st.run_dir)
        if r is not None:
            out.append({"name": st.phase, "seconds": r.get("wall_seconds"),
                        "peak_mib": (r.get("sampled_peak_tree_rss_bytes") or 0) >> 20})
        elif ran:
            out.append({"name": st.phase, "seconds": ran["seconds"], "peak_mib": None})
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
    dq, de = row.get("deflated") or {}, row.get("deflated_effective_n") or {}
    pbo = res.read_json(f"{cycle_dir}/{PBO_JSON}")
    return {"paired": {"dsr": p.get("dsr"), "se": p.get("memmel_se"), "cbb_ci": p.get("cbb_ci95"),
                       "lw_p": (p.get("lw") or {}).get("p_value")},
            "dsr": {"n": dq.get("n"), "cell_count": dq.get("dsr"), "effective_n": de.get("dsr")},
            "pbo": pbo.get("pbo") if isinstance(pbo, dict) else None}


def verdict(cycle, timings: dict, spec_sha256: str | None) -> dict:
    s, res = cycle.spec, cycle.res
    steps = {st.phase: st for st in cycle.steps()}
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
    if not cycle.screen and s.get("verdict") and "nav" in steps:
        doc.update(scoring_blocks(res, cycle.cycle_dir(), steps["nav"].output))
    return doc


def write_verdict(cycle, timings: dict, spec_sha256: str | None, log) -> dict:
    path = cycle.res.path(f"{cycle.cycle_dir()}/{VERDICT}")
    path.parent.mkdir(parents=True, exist_ok=True)
    doc = verdict(cycle, timings, spec_sha256)
    path.write_text(json.dumps(doc, indent=2) + "\n", encoding="utf-8", newline="\n")
    for r in doc["marginal"]:
        log(f"marginal {r.get('id')}: ic21 {r.get('ic21')} (HAC t {r.get('ic21_hac_t')}); marginal ic21 "
            f"{r.get('marginal_ic21')} (HAC t {r.get('marginal_hac_t')}); max |rho| {r.get('max_abs_rho')} with "
            f"{r.get('max_rho_member')}")
    if doc.get("marginal_note"):
        log(f"marginal: {doc['marginal_note']}")
    log(f"== verdict {cycle.cycle_dir()}/{VERDICT}")
    return doc
