"""wave-result.json (``atx.wave-result/v1``) and the ready-to-paste log section of one research wave.

The record stage (wave_stages.record) builds both from the stage receipts' outputs; the book scoreboard
(wave_scoreboard.py) reads only these files and the ledger. Layout:

  {schema, wave, kind (library | rule), manifest{path, sha256, commit}, library | null, template | null,
   budget{id, admission_used, admission_new, admission_new_ids, admission_cap, admission_cycle_prefix |
   admission_cycle_prefixes, admission_origin, construction_cap} (preflight's count), parent{spec, spec_digest,
   library, nav, leverage}, screen{spec, gate_exit, rows[], kept[], dropped[], sign_rule} | null, cell{spec,
   spec_digest, library, kind, nav, leverage, gross, gross_parent, corrected} | null, marginal{source, mode, rows[]} |
   null (report only), mechanics{rule, pass, rows[]} | null, nav_exe{parent, cell, equal, ref} | null (verify's NAV
   exe SHA-256s, P9 OR-2),
   stats{cell, parent} (the book reader's rows) | null, paired{...} | null, bundle{...} | null, dsr{...} | null,
   pbo, verdict{rule, text, accepted, checks, decided_by, criteria[]} | {accepted: false, reason},
   ledger{path, lines_before, lines_after, head, n_before, n_after, trial_id, admission_lines[]},
   next_parent{spec, library}, timings[{phase, run_dir, outcome, exit_code, seconds, peak_mib, executable_sha256}],
   receipts{stage: sha256},
   exes_sha256{parent, cell, differ}   only when a spec pins its exes (lock --exes; verify's record, P9 OR-2)}
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
    doc = {"schema": SCHEMA, "wave": m["wave"], "kind": "library" if library else "rule",
           "description": m.get("description", ""), "manifest": pre["manifest"],
           "library": m.get("library"), "template": (m.get("rule_cell") or {}).get("template"),
           "budget": pre.get("budget"),
           "parent": {k: parent.get(k) for k in ("spec", "spec_digest", "library", "nav", "leverage")},
           "screen": ({"spec": sc["spec"], "gate_exit": sc["gate_exit"], "rows": sc["decision"]["rows"],
                       "kept": sc["decision"]["kept"], "dropped": sc["decision"]["dropped"],
                       "sign_rule": sc["decision"]["rule"]} if sc.get("decision") else None),
           "cell": cell, "marginal": marginal_rows(sc, sp, jd), "mechanics": vf.get("mechanics"),
           "nav_exe": vf.get("nav_exe"),
           "seal_scan": seal if seal is not None else vf.get("seal_scan"),
           "stats": jd.get("book"), "paired": jd.get("paired"), "bundle": jd.get("bundle"), "dsr": jd.get("dsr"),
           "pbo": jd.get("pbo"), "verdict": verdict, "ledger": ledger,
           "next_parent": ({"spec": cell["spec"], "library": cell["library"]} if accepted and cell else
                           {"spec": parent["spec"], "library": parent["library"]}),
           "timings": rows, "receipts": receipt_digests(w)}
    if vf.get("exes_sha256") is not None:   # P9 OR-2: only when a spec pins its exes (the layout of before otherwise)
        doc["exes_sha256"] = vf["exes_sha256"]
    return doc


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


def what_ran(doc: dict) -> str:
    """The hand heading's parenthesis after the wave kind: "library v8x2 screen, then v8x2b on p0", "library v8x3 on
    p0", "template `x-theme-erc.json` on p0"."""
    parent, cell = doc["parent"]["library"], doc.get("cell") or {}
    if doc["kind"] == "rule":
        return f"template `{str(doc.get('template') or '').rsplit('/', 1)[-1]}` on {parent}"
    lib = doc.get("library") or (doc.get("screen") or {}).get("spec", "")
    if cell.get("kind") == "b-library":
        return f"library {lib} screen, then {cell['library']} on {parent}"
    return f"library {lib} on {parent}" + ("" if cell else " (screen; no cell)")


def budget_line(b: dict, led: dict) -> str:
    """The budget block (preflight's count), in the hand log's words: admission trials used + new of the cap
    (re-screens left out), construction N of its cap."""
    out = [f"Budget {b.get('id')}:"]
    if "admission_cap" in b:
        used, new = b.get("admission_used", 0), b.get("admission_new", 0)
        prefixes = b.get("admission_cycle_prefixes") or [b.get("admission_cycle_prefix")]   # P9 DEC-2: a list
        out.append(f"admission trials {used} + {new} new = {used + new} of {b['admission_cap']} (cycles "
                   + ", ".join(f"{p}*" for p in prefixes) + (f", origin {b['admission_origin']}"
                                                             if b.get("admission_origin") else "") +
                   "; re-screens left out);")
    if "construction_cap" in b:
        out.append(f"construction N {led['n_before']} -> {led['n_after']} of {b['construction_cap']}.")
    return " ".join(out)


def seal_tail(seal: dict) -> str:
    """The seal scan's guard references and allow-listed tokens (wave_seal.scan), for the hidden-data line."""
    out = []
    refs = seal.get("seal_references") or {}
    if refs.get("tokens"):
        out.append(f"{refs['tokens']} seal reference(s) of a guard")
    out += [f"{a['token']} x{a['tokens']} allowed: {a['ruling']}" for a in seal.get("allowed") or []]
    return "; ".join(out)


def verdict_word(doc: dict) -> str:
    if not doc.get("cell"):
        return "NO CELL"
    return "ACCEPTED" if doc["verdict"].get("accepted") else "NOT ACCEPTED"


def log_section(doc: dict) -> str:
    """The integration-log section of the wave (markdown), in the log's order: registration, screen, cell, gross
    match, mechanics (read before any return), statistics of record, verdict, ledger, returns, timings."""
    led, par, cell = doc["ledger"], doc["parent"], doc.get("cell")
    out = [f"### Cell {doc['wave']} ({doc['kind']} wave; {what_ran(doc)}): N {led['n_after']}", "",
           f"**{verdict_word(doc)}.** Manifest `{doc['manifest']['path']}` sha256 `{doc['manifest']['sha256'][:16]}` "
           f"(commit `{(doc['manifest'].get('commit') or '')[:12]}`); parent `{par['spec']}` (library "
           f"{par['library']}, L {par['leverage']}); driver `research_cycle.py wave run`.", ""]
    if doc.get("budget"):
        out += [budget_line(doc["budget"], led), ""]
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
    tail = seal_tail(seal)
    out += [f"Hidden-data record: seal scan of {seal.get('files', 0)} log(s) (every run dir, reader and console of the "
            f"wave; forms {', '.join(seal.get('forms') or ['iso'])}): "
            f"{seal.get('tokens_at_or_after_seal', 0)} date token(s) at or after {seal.get('seal', 'the seal')}" +
            (f" ({tail})" if tail else "") + ".",
            f"**Next parent: `{doc['next_parent']['spec']}`, library {doc['next_parent']['library']}.**", ""]
    return "\n".join(out)
