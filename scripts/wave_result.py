"""wave-result.json (``atx.wave-result/v1``) and the ready-to-paste log section of one research wave.

The record stage (wave_stages.record) builds both from the stage receipts' outputs; the book scoreboard
(wave_scoreboard.py) reads only these files and the ledger. Layout:

  {schema, wave, kind (library | rule), manifest{path, sha256, commit}, parent{spec, spec_digest, library, nav,
   leverage}, screen{spec, gate_exit, rows[], kept[], dropped[], sign_rule} | null, cell{spec, spec_digest, library,
   kind, nav, leverage, gross, gross_parent, corrected} | null, marginal{source, mode, rows[]} | null (report only),
   mechanics{rule, pass, rows[]} | null,
   stats{cell, parent} (the book reader's rows) | null, paired{...} | null, bundle{...} | null, dsr{...} | null,
   pbo, verdict{rule, text, accepted, checks, decided_by, criteria[]} | {accepted: false, reason},
   ledger{path, lines_before, lines_after, head, n_before, n_after, trial_id, admission_lines[]},
   next_parent{spec, library}, timings[{phase, run_dir, outcome, exit_code, seconds, peak_mib}],
   receipts{stage: sha256}}
"""
from __future__ import annotations

SCHEMA = "atx.wave-result/v1"
RESULT, LOG = "wave-result.json", "wave-log.md"


def _f(x, spec: str = ".4f") -> str:
    return format(x, spec) if isinstance(x, (int, float)) and not isinstance(x, bool) else "na"


def build(w, done: dict, ledger: dict, seal: dict | None = None) -> dict:
    """``seal``: the record stage's scan of every log the wave produced (the hidden-data line)."""
    m, pre = w.manifest, done["preflight"]
    sc, sp, mt = done.get("screen") or {}, done.get("spec") or {}, done.get("match") or {}
    jd, vf = done.get("judge") or {}, done.get("verify") or {}
    library = "candidates" in m
    cell = None
    if sp.get("cell_spec"):
        cell = {"spec": mt["cell_spec"], "spec_digest": mt.get("spec_digest") or sp["spec_digest"],
                "library": sp["library"], "kind": sp["kind"], "nav": mt["nav"], "leverage": mt["leverage"],
                "gross": mt["g_cell"], "gross_parent": mt["g_parent"], "gross_calibration": mt["g_calibration"],
                "leverage_calibration": mt["leverage_calibration"], "corrected": mt["corrected"],
                "gross_match": mt["mode"]}
    verdict = jd.get("verdict") or {"accepted": False, "reason": sp.get("reason") or "no cell"}
    parent = pre["parent"]
    accepted = bool(verdict.get("accepted"))
    timings = []
    for key in ("run", "match", "judge"):
        timings += (done.get(key) or {}).get("phases") or []
    seen, rows = set(), []
    for r in timings:                      # each run dir once (the match and judge stages re-list the run's phases)
        if r["run_dir"] not in seen:
            seen.add(r["run_dir"])
            rows.append(r)
    return {"schema": SCHEMA, "wave": m["wave"], "kind": "library" if library else "rule",
            "description": m.get("description", ""), "manifest": pre["manifest"],
            "parent": {k: parent.get(k) for k in ("spec", "spec_digest", "library", "nav", "leverage")},
            "screen": ({"spec": sc["spec"], "gate_exit": sc["gate_exit"], "rows": sc["decision"]["rows"],
                        "kept": sc["decision"]["kept"], "dropped": sc["decision"]["dropped"],
                        "sign_rule": sc["decision"]["rule"]} if sc.get("decision") else None),
            "cell": cell, "marginal": marginal_rows(sc, sp, jd), "mechanics": vf.get("mechanics"),
            "seal_scan": seal if seal is not None else vf.get("seal_scan"),
            "stats": jd.get("book"), "paired": jd.get("paired"), "bundle": jd.get("bundle"), "dsr": jd.get("dsr"),
            "pbo": jd.get("pbo"), "verdict": verdict, "ledger": ledger,
            "next_parent": ({"spec": cell["spec"], "library": cell["library"]} if accepted and cell else
                            {"spec": parent["spec"], "library": parent["library"]}),
            "timings": rows, "receipts": receipt_digests(w)}


PER_ROW = ("id", "ic21", "ic21_hac_t", "marginal_ic21", "marginal_hac_t")   # a row's own: pool / themes, not library
LIBRARY_WIDE = ("max_abs_rho", "max_rho_member")                           # over the whole library's members


def marginal_rows(sc: dict, sp: dict, jd: dict | None = None) -> dict | None:
    """The marginal IC rows (contract K6, report only) of the cell's strings. A screen-library cell's (and a no-cell
    wave's) are its screen's. A b library that carried the screen's (spec.marginal.reuse: the same mode) gets only the
    per-row fields, max_abs_rho / max_rho_member null (the screen's are over a library that held dropped strings); a b
    library that ran its own marginal (another mode, or reuse off) reports its cell's rows."""
    if not sc.get("decision"):
        return None
    mg = sp.get("marginal") or {}
    if sp.get("kind") == "b-library" and not mg.get("reuse"):
        return {"source": f"the cell's own marginal ({sp['cell_spec']}, {mg.get('mode')})", "mode": mg.get("mode"),
                "rows": list((jd or {}).get("marginal") or [])}
    kept = set(sc["decision"]["kept"]) if sp.get("cell_spec") else set(r["id"] for r in sc["decision"]["rows"])
    rows = [r for r in sc.get("marginal") or [] if r.get("id") in kept]
    if sp.get("kind") != "b-library":
        return {"source": f"the screen ({sc['spec']})", "mode": mg.get("screen_mode"), "rows": rows}
    return {"source": f"the screen ({sc['spec']}): per-row fields carried, {', '.join(LIBRARY_WIDE)} left null (over "
                      "the screen library)", "mode": mg["mode"],
            "rows": [dict({k: r.get(k) for k in PER_ROW}, **{k: None for k in LIBRARY_WIDE}) for r in rows]}


def receipt_digests(w) -> dict:
    """{receipt file stem: SHA-256} of the wave's ok receipts so far (the record stage's own is written after)."""
    from wave_context import stage_chain  # noqa: PLC0415  (the scoreboard imports this module without the context)
    d = w.path(w.wave_path("receipts"))
    return {p.stem: stage_chain.sha256_file(p) for p in sorted(d.glob("*.json"))} if d.is_dir() else {}


def verdict_word(doc: dict) -> str:
    if not doc.get("cell"):
        return "NO CELL"
    return "ACCEPTED" if doc["verdict"].get("accepted") else "NOT ACCEPTED"


def log_section(doc: dict) -> str:
    """The integration-log section of the wave (markdown), in the log's order: registration, screen, cell, gross
    match, mechanics (read before any return), statistics of record, verdict, ledger, returns, timings."""
    led, par, cell = doc["ledger"], doc["parent"], doc.get("cell")
    out = [f"### Wave {doc['wave']} ({doc['kind']} wave): {verdict_word(doc)}, N {led['n_after']}", "",
           f"Manifest `{doc['manifest']['path']}` sha256 `{doc['manifest']['sha256'][:16]}` (commit "
           f"`{(doc['manifest'].get('commit') or '')[:12]}`); parent `{par['spec']}` (library {par['library']}, "
           f"L {par['leverage']}); driver `research_cycle.py wave run`.", ""]
    sc = doc.get("screen")
    if sc:
        out += [f"**Screen** (`{sc['spec']}`, gate exit {sc['gate_exit']}, sign rule {sc['sign_rule']}): kept "
                f"{', '.join(sc['kept']) or 'none'}; dropped {', '.join(sc['dropped']) or 'none'}.", "",
                "| id | kind | prior | status | runner sign | decision | reason |", "|---|---|---|---|---|---|---|"]
        out += [f"| {r['id']} | {r['kind']} | {r['prior_sign']:+d} | {r['status']} | {r['runner_sign']} | "
                f"{r['decision']} | {r['reason']} |" for r in sc["rows"]]
        out.append("")
    if cell:
        out += [f"**Cell** `{cell['spec']}` ({cell['kind']}, library {cell['library']}): gross match {cell['gross_match']}"
                f": calibration L {cell['leverage_calibration']} G {_f(cell['gross_calibration'], '.10f')} vs "
                f"G_parent {_f(cell['gross_parent'], '.10f')}" +
                (f" -> corrected to L {cell['leverage']}, G {_f(cell['gross'], '.10f')}" if cell["corrected"] else
                 " (stands)") + ".", ""]
        mech = doc.get("mechanics") or {}
        out += [f"**Mechanics (S2, read before any return): {'PASS' if mech.get('pass') else 'FAIL'}** (" +
                "; ".join(f"{r['check']} {_f(r['value'], '.5g')} {r['limit']}" for r in mech.get("rows", [])) + ").", ""]
        p, b, d = doc.get("paired") or {}, doc.get("bundle") or {}, doc.get("dsr") or {}
        st = doc.get("stats") or {}
        c, q = st.get("cell") or {}, st.get("parent") or {}
        out += [f"**Statistics of record** (S2): net Sharpe {_f(c.get('net_sharpe'))} vs parent "
                f"{_f(q.get('net_sharpe'))}: dSR {_f(p.get('dsr'), '+.4f')}, Memmel SE {_f(p.get('se'))}, CBB 95% "
                f"{p.get('cbb_ci')}, LW p {_f(p.get('lw_p'))}; bundle p one-sided {_f(b.get('p_one_sided'))}, "
                f"two-sided {_f(b.get('p_two_sided'))}. DSR (N {d.get('n')}): ledger {_f(d.get('cell_count'))}; "
                f"PBO {_f(doc.get('pbo'))}.", ""]
        v = doc["verdict"]
        out += [f"**Verdict ({v['rule']}: {v['text']}): {verdict_word(doc)}** {v['checks']}."]
        out += [f"- criterion {r['criterion']} (printed{'' if 'criteria' not in v['decided_by'] else ', deciding'}): "
                f"{r['printed']}" for r in v.get("criteria", [])]
        out.append("")
        out += [f"Returns (S2, annual): net {_f(c.get('net_annual'), '.2%')} (CAGR {_f(c.get('cagr'), '.2%')}) vs "
                f"{_f(q.get('net_annual'), '.2%')}; gross of cost {_f(c.get('gross_annual'), '.2%')} vs "
                f"{_f(q.get('gross_annual'), '.2%')}; vol {_f(c.get('ann_vol'), '.2%')}; max drawdown "
                f"{_f(c.get('max_drawdown'), '.2%')}; 4x net Sharpe {_f(c.get('x4_net_sharpe'))} vs "
                f"{_f(q.get('x4_net_sharpe'))}; tau {_f(c.get('tau_gmv_mean'), '.5f')} (per unit gross "
                f"{_f(c.get('tau_per_gross'), '.5f')}).", ""]
    else:
        out += [f"No cell: {doc['verdict'].get('reason')}.", ""]
    out += [f"Ledger `{led['path']}`: lines {led['lines_before']} -> {led['lines_after']}, head `{led['head'][:16]}`, "
            f"N {led['n_before']} -> {led['n_after']}" + (f", cell trial `{led['trial_id']}`" if led.get("trial_id")
                                                          else "") +
            f"; admission lines appended {len(led['admission_lines'])}.", ""]
    if doc.get("timings"):
        out += ["| phase | run dir | s | peak MiB | outcome |", "|---|---|---|---|---|"]
        out += [f"| {r['phase']} | `{r['run_dir']}` | {_f(r['seconds'], '.1f')} | {r['peak_mib']} | {r['outcome']} |"
                for r in doc["timings"]]
        out.append("")
    seal = doc.get("seal_scan") or {}
    out += [f"Hidden-data record: seal scan of {seal.get('files', 0)} log(s) (every run dir, reader and console of the "
            f"wave): "
            f"{seal.get('tokens_at_or_after_seal', 0)} date token(s) at or after {seal.get('seal', 'the seal')}.",
            f"**Next parent: `{doc['next_parent']['spec']}`, library {doc['next_parent']['library']}.**", ""]
    return "\n".join(out)
