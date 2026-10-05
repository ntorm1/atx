"""D2R flag-absent identity (scratch): D2's three synthetic cases (test_composition_engine_fit.CASES on its World),
run through the fitter's real argv path (fcw.main(argv), so the parser is exercised), flag absent; SHA-256 of every
output file and the stdout summary without script_sha256 (CM-5: the receipt, not the output bytes).
Usage: python identity.py <repo> <world dir (recreated)> <out.json>"""
import contextlib
import hashlib
import io
import json
import shutil
import sys
import unittest.mock
from pathlib import Path

REPO, WORLD, OUT = Path(sys.argv[1]), Path(sys.argv[2]), Path(sys.argv[3])
sys.path.insert(0, str(REPO / "atx-impl" / "tools"))
if len(sys.argv) > 4:  # a scratch dir holding another fit_composition_weights.py (the pre-fix blob)
    sys.path.insert(0, sys.argv[4])
import composition_rules as cr  # noqa: E402
import fit_composition_weights as fcw  # noqa: E402
import test_composition_engine_fit as tce  # noqa: E402  (D2's CASES and World)

FLAG = {"orientation": "--orientation", "composition": "--composition", "theme_erc": "--theme-erc",
        "theme_tsmom": "--theme-tsmom", "theme_resid": "--theme-resid"}
if WORLD.exists():
    shutil.rmtree(WORLD)
WORLD.mkdir(parents=True)
fx = tce.World.build(WORLD)
res = {"script_sha256": fcw.SCRIPT_SHA256, "fcw_file": fcw.__file__, "cases": {}}
with unittest.mock.patch.object(cr, "REGISTRY_PATH", WORLD / "no-registry.json"):
    for name, case in tce.CASES.items():
        extra = [tok for k, v in case.items() if k != "screen" for tok in (FLAG[k], v)]
        out = WORLD / f"{name}-plain"
        argv = fx.argv(out, case["screen"], extra)
        so, se = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(so), contextlib.redirect_stderr(se):
            code = fcw.main(argv)
        summary = json.loads(so.getvalue())
        summary.pop("script_sha256", None)
        seconds = summary.pop("seconds", None)
        files = {p.name: hashlib.sha256(p.read_bytes()).hexdigest() for p in sorted(out.iterdir()) if p.is_file()}
        res["cases"][name] = {"code": code, "argv": argv[-(len(extra) + 4):], "files": files,
                              "summary_sha256": hashlib.sha256(json.dumps(summary, sort_keys=True).encode()).hexdigest(),
                              "summary": summary, "seconds_present": seconds is not None}
OUT.write_text(json.dumps(res, indent=1), encoding="utf-8")
for name, r in res["cases"].items():
    print(name, r["code"], len(r["files"]), "files", r["summary_sha256"][:16])
