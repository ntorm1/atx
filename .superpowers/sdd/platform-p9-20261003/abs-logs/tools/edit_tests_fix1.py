"""ABS test fix 1 (after p9-abs-a): (a) three range-for loops over a member of a temporary Json
(read_json(...).at(...)) dangled (C++20 keeps only the reference alive): name the document first;
(b) the gate field becomes a positive, date- and name-varying multiplier (times the scale), so a
gated member is informative under every rule: with gate 1 the vee member's ranks never moved a
re-ranked theme composite nor had a TRAIN orientation, which left three non-triviality checks
vacuous (the equivalences themselves held)."""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from abs_edit import File  # noqa: E402

t = File("C:/atx-wt/pool-28/atx-impl/tests/strategy_ic_runner_test.cpp")
t.once("""// `gate` (P9 lane ABS): the scale on every cell (1 keeps a gated member's data, NaN removes it).
""", """// `gate` (P9 lane ABS): the scale times a positive multiplier varying by date and name (scale 1: a
// gated member's data; NaN: none).
""")
t.once("""        (name=="gate"?scale:si_value(d,i,scale))));
""", """        (name=="gate"?scale*(1.5+std::sin(.37*static_cast<f64>(d)+1.3*static_cast<f64>(i))):
        si_value(d,i,scale))));
""")
t.once("""// The members `listed` read the field gate, which no other candidate reads: each DSL becomes
// (DSL) * gate. gate holds the fields' scale on every cell (field_column): 1 keeps the members'
// data, NaN removes it (contract section 5's I': their evaluated signals non-finite everywhere).
""", """// The members `listed` read the field gate, which no other candidate reads: each DSL becomes
// (DSL) * gate. gate (field_column) is a positive multiplier at scale 1, so the members keep
// informative data, and NaN at scale NaN (contract section 5's I': their evaluated signals
// non-finite everywhere).
""")
t.once("""  for (const auto& row:read_json(dir.path/"u-listed"/"orientations.json").at("candidates"))
    EXPECT_EQ(row.at("sign").get<int>()==0,row.at("id")=="volume_vee") << row.at("id");
""", """  const auto orientations=read_json(dir.path/"u-listed"/"orientations.json");
  for (const auto& row:orientations.at("candidates"))
    EXPECT_EQ(row.at("sign").get<int>()==0,row.at("id")=="volume_vee") << row.at("id");
""")
t.once("""  for (const auto& role_result:read_json(dir.path/"warm_listed"/"summary.json").at("roles")) {
""", """  const auto warm_summary=read_json(dir.path/"warm_listed"/"summary.json");
  for (const auto& role_result:warm_summary.at("roles")) {
""")
t.once("""  for (const auto& role_result:read_json(dir.path/"warm_plain"/"summary.json").at("roles"))
""", """  const auto again_summary=read_json(dir.path/"warm_plain"/"summary.json");
  for (const auto& role_result:again_summary.at("roles"))
""")
t.save()
print("ok")
