"""Flag-absent identity: python ident.py TOOLS_DIR WORK_DIR OUT_DIR -- the fitter of TOOLS_DIR on the synthetic world
(test_composition_engine_fit.World) at WORK_DIR, outputs copied to OUT_DIR: plain fits (no --engine-fit) of four cases
and --engine-fit fits (stand-ins) of three, every file's bytes."""
import shutil
import sys
import unittest.mock
from pathlib import Path

tools, work, out = (Path(a) for a in sys.argv[1:4])
sys.path.insert(0, str(tools))
import composition_ic_shrink as cis  # noqa: E402
import composition_resid as cres  # noqa: E402
import composition_rules as cr  # noqa: E402
import composition_theme_erc as cte  # noqa: E402
import composition_theme_tsmom as ct  # noqa: E402
import fit_composition_weights as fcw  # noqa: E402
import test_composition_engine_fit as tcef  # noqa: E402

STD = dict(screen="v4-prior-v1", orientation="prior", composition="ew-theme-std-v1")
PLAIN = {"std": dict(STD), "std-erc-tsmom": dict(STD, theme_erc=cte.RULE_ID, theme_tsmom=ct.RULE_ID),
         "shrink-tsmom": dict(STD, composition=cis.RULE_ID, theme_tsmom=ct.RULE_ID),
         "std-aim-resid": dict(STD, composition=cr.STD_AIM_RULE_ID, theme_resid=cres.RULE_ID)}
shutil.rmtree(work, ignore_errors=True)
work.mkdir(parents=True)
out.mkdir(parents=True, exist_ok=True)
fx = tcef.World.build(work)
with unittest.mock.patch.object(cr, "REGISTRY_PATH", work / "no-registry.json"):
    for name, case in PLAIN.items():
        code, _ = fcw.fit(fx.args(work / f"plain-{name}", **case))
        print(name, "plain", code)
        shutil.copytree(work / f"plain-{name}", out / f"plain-{name}")
        if name == "std":
            continue
        with tcef.StandIns(work / f"{name}-factors"):
            code, _ = fcw.fit(fx.args(work / f"engine-{name}", **case, **tcef.placeholders(work / f"{name}-in")))
        print(name, "engine", code)
        shutil.copytree(work / f"engine-{name}", out / f"engine-{name}")
shutil.rmtree(work, ignore_errors=True)
