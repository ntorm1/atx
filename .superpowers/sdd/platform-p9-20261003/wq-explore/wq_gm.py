"""WQ-explore: the house matched-gross rule (Ruling PM6-6): L = L_cal x G_parent / G_cell, G = the primary scenario's
all-rows mean gross leverage at the calibration run (L_cal 1.1828, the parent's aim leverage). Runs the NAV replay
again at L into NAME/nav-gm.   usage: wq_gm.py NAME [NAME ...]"""
import subprocess, sys
from pathlib import Path
sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[4]
sys.path.insert(0, str(ROOT / "atx-impl/tools"))
import nav_summ as NS


def gross(d):
    s = NS.load_summary(d)
    sc = next(x for x in s["scenarios"] if x["primary"])
    return NS.construction_stats(NS.load_daily(d, sc["scenario"]), sc)["mean_gross_leverage_all_rows"]


X = ROOT / "build-equity/wq-explore"
G = gross(X / "primary-rel/nav")
for v in sys.argv[1:]:
    g = gross(X / v / "nav-L1.1828")
    L = 1.1828 * G / g
    print(v, f"G_parent {G:.6f} G_cell {g:.6f} -> L {L:.4f}", flush=True)
    subprocess.run([sys.executable, str(Path(__file__).with_name("wq_chain.py")), "--nav-only", v,
                    f"build-equity/wq-explore/{v}/w/train_combined.json", "--aim-leverage", f"{L:.4f}",
                    "--nav-name", "nav-gm"], check=True)
