"""P9 C2 fix round 1: R1, R2 and C2-4b edits to scripts/wave_stage_cell.py (exact-match replacements)."""
from pathlib import Path

p = Path("C:/atx-wt/pool-15/scripts/wave_stage_cell.py")
t = p.read_text(encoding="utf-8")


def swap(old: str, new: str) -> None:
    global t
    assert t.count(old) == 1, old[:80]
    t = t.replace(old, new, 1)


swap("import research_spec\nimport wave_manifest as WM", "import research_cycle as RC\nimport research_spec\nimport wave_manifest as WM")

swap('''        out = {"spec": cell, "nav": None, "engine": True, "phases": phase_rows(w, cell)}
''', '''        comb = w_combined(w, cell)
        if comb is None:
            raise StageError(f"run: {cell} has no complete w output after --stop-after {ENGINE_STOP}")
        out = {"spec": cell, "nav": None, "engine": True, "spec_digest": w.spec_digest(cell), "w_combined": comb,
               "w_combined_sha256": w.sha(comb), "phases": phase_rows(w, cell)}
''')

swap('''ENGINE_STOP = "w"
''', '''ENGINE_STOP = "w"


def w_combined(w: Wave, spec_rel: str) -> str | None:
    """The combined signal the cell's NAV reads (P9 C2-4b): <ic.w_output>-<n>/train_combined.json of the w attempt
    research_cycle picks for it (Cycle.ic_attempt: its lowest complete attempt); None while none is complete."""
    spec = w.load_spec(spec_rel)
    c = RC.Cycle(spec, RC.Resolver(w.root), verify=False)
    base = c.out(spec["ic"]["w_output"])
    n, state, _ = c.ic_attempt("w", base)
    return f"{base}-{n}/train_combined.json" if state == "done" else None
''')

start, end = t.index("def calibrated_doc("), t.index("def match_plan(")
t = t[:start] + (Path(__file__).parent / "engine_block.py").read_text(encoding="utf-8") + t[end:]

swap('''                "#   write <cell>-gm.json (nav.flags + --calibrate-gross G_parent --calibrate-tolerance T, at the "
                "cell's L), then",
                "#   research_cycle.py lock <cell>-gm.json ; git commit ; research_cycle.py run <cell>-gm.json --stop-after "
                "nav (one NAV: pass 1, at most one correction, published at the final L) ; mechanics reader "
                "(mech-matched)"]''', '''                "#   write <cell>-cal.json (nav.flags + --calibrate-gross G_parent --calibrate-tolerance T, at the "
                "cell's L; ref pinned uncalibrated), then",
                "#   research_cycle.py lock <cell>-cal.json ; git commit ; research_cycle.py run <cell>-cal.json "
                "--stop-after nav (one NAV: pass 1, at most one correction, published at the final L) ; mechanics "
                "reader (mech-matched)",
                "#   write <cell>-gm.json (the plain recipe at the final L, bound to that NAV: the cell of record) ; "
                "research_cycle.py lock <cell>-gm.json ; git commit"]''')

swap('''def match_inputs(w: Wave, done: dict) -> dict:
    rn = done.get("run") or {}
    return pinned("run", rn, {"summary_sha256": w.sha(f"{rn['nav']}/summary.json")}) if rn.get("nav") else {}''',
     '''def match_inputs(w: Wave, done: dict) -> dict:
    rn = done.get("run") or {}
    if rn.get("engine"):        # P9 C2-4b: the cell spec and the w output its calibrated NAV reads
        comb = w_combined(w, rn["spec"]) if w.exists(rn["spec"]) else None
        return pinned("run", rn, {"spec_digest": w.spec_digest(rn["spec"]) if w.exists(rn["spec"]) else None,
                                  "w_combined": comb, "w_combined_sha256": w.sha(comb) if comb else None})
    return pinned("run", rn, {"summary_sha256": w.sha(f"{rn['nav']}/summary.json")}) if rn.get("nav") else {}''')

p.write_text(t, encoding="utf-8", newline="\n")
print("patched")
