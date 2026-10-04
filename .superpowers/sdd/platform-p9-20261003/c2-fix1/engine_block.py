def cal_path(spec_path: str) -> str:
    """The engine mode's run recipe beside the cell spec: <stem>-cal.json (the -gm path is the plain cell of record)."""
    p = Path(spec_path)
    return (p.parent / f"{p.stem}-cal{p.suffix}").as_posix()


def calibrated_doc(doc: dict, resolved: dict, g_parent: float, tolerance: float, note: str) -> dict:
    """The run recipe of engine mode (P9 C2): the cell spec file ``doc`` (``resolved``: its resolved spec) named -gm,
    its NAV output <output>-gm, nav.flags + ``--calibrate-gross repr(G_parent) --calibrate-tolerance repr(T)`` (repr
    round-trips: the exe parses the reader's G_parent exactly). nav.leverage stays the cell's L: the exe applies
    PM6-6's correction itself and records it in summary.json calibration. The reference construction stays the
    uncalibrated one (review R1: the ref phase inherits nav.flags, research_cycle.ref_step): ref.flags is pinned to
    the cell's nav.flags and ref.leverage to its L unless the ref sets them (gm_doc pins ref.leverage likewise)."""
    out = copy.deepcopy(doc)
    out["name"] = f"{doc['name']}-gm"
    out["description"] = (doc.get("description", "") + " " + note).strip()
    nav_out = f"{resolved['nav']['output']}-gm"
    ops = {"--calibrate-gross": repr(float(g_parent)), "--calibrate-tolerance": repr(float(tolerance))}
    if research_spec.is_template(doc):
        change = out.setdefault("change", {})
        sets = change.setdefault("set", {})
        sets["nav.output"] = nav_out
        change.setdefault("flags", {}).setdefault("nav", {}).update(ops)
        ref = resolved.get("ref")
        if isinstance(ref, dict):              # a template that sets its own ref (resolve drops the parent's)
            if "flags" not in ref:
                sets["ref.flags"] = list(resolved["nav"].get("flags", []))
            if "leverage" not in ref:
                sets["ref.leverage"] = resolved["nav"]["leverage"]
        return out
    flags = research_spec.apply_flags(list(out["nav"].get("flags", [])), ops, "calibrated nav")
    out["nav"] = dict(out["nav"], output=nav_out, flags=flags)
    if isinstance(out.get("ref"), dict):
        ref = dict(out["ref"])
        ref.setdefault("flags", list(doc["nav"].get("flags", [])))
        ref.setdefault("leverage", doc["nav"]["leverage"])
        out["ref"] = ref
    return out


def record_doc(doc: dict, resolved: dict, leverage: str, nav_out: str, note: str) -> dict:
    """The engine mode's cell of record (Ruling C2-R2): the PLAIN recipe of the published book, gm_doc's copy at the
    final L (no --calibrate-* flag; ref pinned at the cell's L as gm_doc pins it) with its NAV output the engine NAV
    (``nav_out``, the run recipe's; the same name, so the same cycle dir). The exe published exactly the plain run at
    that L (summary.json adds calibration), so a child copying it reproduces the book without calibrating again."""
    out = WS.gm_doc(doc, resolved, leverage, note)
    if research_spec.is_template(doc):
        out["change"]["set"]["nav.output"] = nav_out
    else:
        out["nav"] = dict(out["nav"], output=nav_out)
    return out


def match_engine(w: Wave, done: dict, log) -> dict:
    """PM6-6 in one process (P9 C2, P15; Rulings C2-1..3, C2-R2): G_parent from the parent's mechanics; the run recipe
    <cell>-cal.json (the cell at its own L with ``nav --calibrate-gross G_parent``) runs ONE NAV, whose summary.json
    calibration record says what the exe did (pass 1 at L, at most one correction, published at the final L). matched
    false is a stop (the NAV stays, as the Python mode's matched run does); the mechanics reader of the published NAV
    must give the exe's own G bit for bit (both are numpy's mean of the S2 daily gross). Then the cell of record
    <cell>-gm.json: the plain recipe at the final L bound to that NAV (record_doc), which wave_result records as the
    cell and next_parent; verify and judge run the recipe that ran (cell_spec = the run recipe)."""
    m, s = w.manifest, done["spec"]
    mode = m["gross_match"]
    rule = WR.GROSS_MATCH[mode]
    par = read_once(w, "mechanics", "mech-parent", {"parent": s["reference_nav"]})
    gp = par["navs"]["parent"]["mean_gross_leverage_all_rows"]
    if not (WR._num(gp) and gp > 0):
        raise StageError(f"gross match (engine): the parent's all-rows gross {gp!r} must be finite and > 0")
    log(f"   gross (engine): G_parent {gp} (mode {mode}, tolerance {rule['tolerance']}); nav --calibrate-gross")
    cell = s["cell_spec"]
    gm, run_spec = WS.gm_path(cell), cal_path(cell)
    w.require_clean("match", {gm, run_spec})
    doc, resolved = w.read_json(cell), w.load_spec(cell)
    note = (f"Wave {m['wave']} at matched gross in one process (Ruling PM6-6, nav --calibrate-gross {gp!r}, P9 C2): "
            f"the exe calibrates at L {s['leverage']} and publishes at the final L (summary.json calibration).")
    write_spec_file(w, run_spec, calibrated_doc(doc, resolved, gp, rule["tolerance"], note))
    nav = w.outputs(run_spec)["nav"]
    w.run(cyc(w, "lock", run_spec), "lock (dry: every pin of the run recipe verified)")
    commit = w.commit_paths(f"wave {m['wave']}: {Path(run_spec).name} calibrated in one process (PM6-6, P9 C2)",
                            {run_spec}) or w.committed(run_spec)
    w.run(cyc(w, "run", run_spec, "--stop-after", "nav"), "calibrated run (--stop-after nav, nav --calibrate-gross)")
    cal = (w.read_json(f"{nav}/summary.json") or {}).get("calibration")
    if not isinstance(cal, dict) or any(k not in cal for k in CALIBRATION_KEYS):
        raise StageError(f"gross match (engine): {nav}/summary.json has no complete calibration record (an exe "
                         "without nav --calibrate-gross?)", EXIT_PIN)
    cal = {k: cal[k] for k in CALIBRATION_KEYS}
    if cal["leverage_requested"] != float(s["leverage"]) or cal["target"] != gp:
        raise StageError(f"gross match (engine): the calibration ran at L {cal['leverage_requested']} toward "
                         f"{cal['target']}, not the cell's L {s['leverage']} toward G_parent {gp}", EXIT_PIN)
    got = read_once(w, "mechanics", "mech-matched", {"cell": nav})
    g2 = got["navs"]["cell"]["mean_gross_leverage_all_rows"]
    if g2 != cal["gross"]:
        raise StageError(f"gross match (engine): the mechanics reader's G {g2!r} is not the exe's {cal['gross']!r}")
    corrected = bool(cal["corrected"])
    lev = f"{cal['leverage']:.4f}" if corrected else s["leverage"]
    log(f"   calibrated: G {cal['gross_requested']} at L {s['leverage']} -> L {lev}, G {g2} vs G_parent {gp}")
    if not cal["matched"]:
        raise StageError(f"gross match (engine): the published run's G {g2} is {abs(g2 - gp):.5f} from G_parent {gp} "
                         f"after {rule['corrections']} correction: stop for the PM's ruling (no ledger line)")
    rec_note = (f"Wave {m['wave']} at matched gross (Ruling PM6-6 in one process, P9 C2): the plain recipe of the NAV "
                f"{nav}, which {Path(run_spec).name} published calibrating at L {s['leverage']} toward {gp!r} "
                f"(summary.json calibration); L = {lev}.")
    write_spec_file(w, gm, record_doc(doc, resolved, lev, nav, rec_note))
    ran, rec = w.outputs(run_spec), w.outputs(gm)
    if (rec["nav"], rec["cycle_dir"]) != (ran["nav"], ran["cycle_dir"]):
        raise StageError(f"gross match (engine): {gm} names NAV {rec['nav']} / {rec['cycle_dir']}, not the run "
                         f"recipe's {ran['nav']} / {ran['cycle_dir']}")
    w.run(cyc(w, "lock", gm), "lock (dry: every pin of the cell of record verified)")
    record_commit = w.commit_paths(f"wave {m['wave']}: {Path(gm).name} at L {lev}, the plain recipe of the "
                                   "calibrated NAV (PM6-6, P9 C2)", {gm}) or w.committed(gm)
    return {"mode": mode, "tolerance": rule["tolerance"], "g_parent": gp, "g_calibration": cal["gross_requested"],
            "leverage_calibration": s["leverage"], "corrected": corrected, "cell_spec": run_spec, "commit": commit,
            "nav": nav, "leverage": lev, "g_cell": g2, "mechanics": got["navs"]["cell"],
            "spec_digest": w.spec_digest(run_spec), "phases": phase_rows(w, run_spec),
            "readers": reader_digests(w, ["mech-parent", "mech-matched"]), "engine": True, "calibration": cal,
            "record_spec": gm, "record_spec_digest": w.spec_digest(gm), "record_commit": record_commit}


