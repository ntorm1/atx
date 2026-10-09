"""Teeth of the D3W C++ refusals: apply (python teeth_cpp.py apply) or restore (restore) the mutation set; the
originals are kept beside this script as *.orig (byte copies) and restored byte for byte."""
import shutil
import sys
from pathlib import Path

REPO = Path(r"C:/atx-wt/pool-23")
HERE = Path(__file__).resolve().parent / "teeth-orig"
FIT = "atx-engine/src/research/composition/fit.cpp"
RULES = "atx-impl/src/strategy_ic_rules.cpp"
COMP = "atx-impl/src/strategy_ic_composition.cpp"
ALL_MUTATIONS = [
    ("E1 window", FIT, "  if (wf.window!=walk_forward_expanding)", "  if (false && wf.window!=walk_forward_expanding)"),
    ("E2 tsmom/resid", FIT, "  if (spec.theme_tsmom || spec.theme_resid)", "  if (false && (spec.theme_tsmom || spec.theme_resid))"),
    ("E3 rows bound", FIT, "  if (rows>walk_forward_max_rows)", "  if (false && rows>walk_forward_max_rows)"),
    ("E4 volman parent", FIT, "  if (spec.mom_volman && (aim || spec.theme_tsmom || spec.theme_resid))",
     "  if (false && spec.mom_volman && (aim || spec.theme_tsmom || spec.theme_resid))"),
    ("E5 volman decisions", FIT, "  if (train.size()<mom_volman_min_decisions || train.size()>mom_volman_max_decisions)",
     "  if (false && (train.size()<mom_volman_min_decisions || train.size()>mom_volman_max_decisions))"),
    ("R1 grouping", RULES, "  if (!pinned.themes.empty() || pinned.residualise || pinned.schedule==theme_tsmom_rule)",
     "  if (false && (!pinned.themes.empty() || pinned.residualise || pinned.schedule==theme_tsmom_rule))"),
    ("R2 increasing", RULES, "    if (!from.empty() && !(session>from.back()))",
     "    if (false && !from.empty() && !(session>from.back()))"),
    ("R3 empty row", RULES, "    if (std::none_of(w.begin(),w.end(),[](f64 x) { return x>0; }))",
     "    if (false && std::none_of(w.begin(),w.end(),[](f64 x) { return x>0; }))"),
    ("C1 redistribute", COMP, "  if (!p.theme.empty())\r\n    return co::Err(co::ErrorCode::InvalidArgument,\r\n"
     "                   \"IC composition: weight rows are refused under the redistribute grouping\");",
     "  if (false && !p.theme.empty())\r\n    return co::Err(co::ErrorCode::InvalidArgument,\r\n"
     "                   \"IC composition: weight rows are refused under the redistribute grouping\");"),
]
MUTATIONS = [m for m in ALL_MUTATIONS if len(sys.argv) < 3 or m[0].split()[0] in sys.argv[2].split(",")]
files = sorted({m[1] for m in MUTATIONS})
if sys.argv[1] == "apply":
    HERE.mkdir(exist_ok=True)
    texts = {}
    for rel in files:
        shutil.copyfile(REPO / rel, HERE / Path(rel).name)
        texts[rel] = (REPO / rel).read_bytes().decode("utf-8")
    for name, rel, old, new in MUTATIONS:
        if "\r\n" not in texts[rel]:
            old, new = old.replace("\r\n", "\n"), new.replace("\r\n", "\n")
        assert texts[rel].count(old) == 1, (name, texts[rel].count(old))
        texts[rel] = texts[rel].replace(old, new)
        print("applied", name)
    for rel in files:
        (REPO / rel).write_bytes(texts[rel].encode("utf-8"))
else:
    for rel in files:
        shutil.copyfile(HERE / Path(rel).name, REPO / rel)
        print("restored", rel)
