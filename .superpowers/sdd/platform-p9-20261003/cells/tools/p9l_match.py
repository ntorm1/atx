"""Cells root, P9-L: the gross match by hand in the form of scripts/wave_stage_cell.match_engine (PRE-3, PRE-27, C2-6;
P9 C2), after `research_cycle.py run label-terminal-cal.json --stop-after nav` and the mech-matched reader.

  p9l_match.py check   the calibration record of the published NAV against the driver's checks: every CALIBRATION_KEY
                       present; leverage_requested == the cell's L and target == G_parent (mech-parent.json) exactly;
                       the mech-matched reader's mean_gross_leverage_all_rows == calibration.gross bit for bit;
                       matched true. Prints the calibration keys (construction figures only) and each check.
  p9l_match.py record  the cell of record label-terminal-gm.json = wave_stage_cell.record_doc (the plain recipe at
                       L-prime = f"{leverage:.4f}" if corrected else L; nav.output = the run recipe's NAV dir), written
                       once (never overwritten), and the driver's check that both name the same NAV and cycle dir.
cwd = the pool root. Reads summary.json's "calibration" block and the readers' JSON only.
"""
from __future__ import annotations

import json
import sys
from pathlib import Path

ROOT = Path.cwd()
sys.path.insert(0, str(ROOT / "scripts"))
import research_cycle as RC  # noqa: E402
import wave_stage_cell as WSC  # noqa: E402

CELL = "scripts/specs/p9/templates/label-terminal.json"
RUN = WSC.cal_path(CELL)
GM = "scripts/specs/p9/templates/label-terminal-gm.json"
READERS = "build-equity/p9l-label-terminal-readers"


def outputs(rel: str) -> dict:
    spec = RC.load_spec(ROOT / rel)
    c = RC.Cycle(spec, RC.Resolver(ROOT), verify=False)
    return {"name": spec["name"], "nav": c.out(spec["nav"]["output"]), "cycle_dir": c.cycle_dir(),
            "leverage": str(spec["nav"].get("leverage"))}


def read(rel: str):
    return json.loads((ROOT / rel).read_text(encoding="utf-8"))


def calibration() -> tuple[dict, dict, float, str]:
    run = outputs(RUN)
    cal = (read(f"{run['nav']}/summary.json") or {}).get("calibration")
    if not isinstance(cal, dict) or any(k not in cal for k in WSC.CALIBRATION_KEYS):
        raise SystemExit(f"STOP: {run['nav']}/summary.json has no complete calibration record")
    gp = read(f"{READERS}/mech-parent.json")["navs"]["parent"]["mean_gross_leverage_all_rows"]
    return run, {k: cal[k] for k in WSC.CALIBRATION_KEYS}, gp, outputs(CELL)["leverage"]


def check() -> int:
    run, cal, gp, lev = calibration()
    print(f"NAV {run['nav']} (run recipe {RUN}, cycle dir {run['cycle_dir']})")
    print("calibration: " + "; ".join(f"{k} {cal[k]!r}" for k in WSC.CALIBRATION_KEYS))
    ok1 = cal["leverage_requested"] == float(lev) and cal["target"] == gp
    print(f"check 1 leverage_requested == L {lev} and target == G_parent {gp!r}: {'holds' if ok1 else 'FAILS'}")
    g2 = read(f"{READERS}/mech-matched.json")["navs"]["cell"]["mean_gross_leverage_all_rows"]
    ok2 = g2 == cal["gross"]
    print(f"check 2 mech-matched G {g2!r} == calibration.gross {cal['gross']!r} bit for bit: "
          f"{'holds' if ok2 else 'FAILS'}")
    ok3 = cal["matched"] is True
    print(f"check 3 matched: {cal['matched']!r} (|G - G_parent| {abs(g2 - gp):.6g} vs tolerance {cal['tolerance']!r}): "
          f"{'holds' if ok3 else 'FAILS: STOP for the PM (no ledger line)'}")
    lp = f"{cal['leverage']:.4f}" if cal["corrected"] else lev
    print(f"L-prime {lp} (corrected {cal['corrected']!r}; the driver's rule)")
    return 0 if ok1 and ok2 and ok3 else 1


def record() -> int:
    run, cal, gp, lev = calibration()
    if not cal["matched"]:
        raise SystemExit("STOP: matched false: no cell of record")
    lp = f"{cal['leverage']:.4f}" if cal["corrected"] else lev
    doc, resolved = read(CELL), RC.load_spec(ROOT / CELL)
    note = (f"P9-L at matched gross (Ruling PM6-6 in one process, P9 C2; by hand in the wave_stage_cell.match_engine "
            f"form): the plain recipe of the NAV {run['nav']}, which {Path(RUN).name} published calibrating at L {lev} "
            f"toward {gp!r} (summary.json calibration); L = {lp}.")
    out = WSC.record_doc(doc, resolved, lp, WSC.engine_nav_output(resolved), note)
    text = json.dumps(out, indent=2) + "\n"
    p = ROOT / GM
    if p.is_file():
        if p.read_text(encoding="utf-8") != text:
            raise SystemExit(f"STOP: {GM} exists with other content: never overwritten")
    else:
        p.write_text(text, encoding="utf-8", newline="\n")
    rec = outputs(GM)
    same = (rec["nav"], rec["cycle_dir"]) == (run["nav"], run["cycle_dir"])
    print(f"{GM}: name {rec['name']} nav {rec['nav']} cycle dir {rec['cycle_dir']} leverage {rec['leverage']}; "
          f"= the run recipe's NAV / cycle dir: {'holds' if same else 'FAILS'}")
    return 0 if same else 1


if __name__ == "__main__":
    raise SystemExit({"check": check, "record": record}[sys.argv[1]]())
