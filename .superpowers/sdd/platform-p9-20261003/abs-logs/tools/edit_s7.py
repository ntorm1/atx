"""ABS S-7 (ruling D3W-S): reflow D3W's 19 lines over 100 columns in strategy_ic_runner_test.cpp
(874 and 4855-5097 at f0726acb). Whitespace / line breaks only."""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from abs_edit import File  # noqa: E402

t = File("C:/atx-wt/pool-28/atx-impl/tests/strategy_ic_runner_test.cpp")
pairs = [
    ("""      {themed_text("atx.dsl-composition-weights/v4",cfg,equal,theme_block(both)),"schema/library identity"},
""", """      {themed_text("atx.dsl-composition-weights/v4",cfg,equal,theme_block(both)),
       "schema/library identity"},
"""),
    ("""  return ",\\"theme_schedule\\":"+Json{{"rule","mom-volman-v1"},{"theme","price_momentum"},{"lookback",126},
      {"lag",2},{"step",21},{"sessions",sessions},{"sleeve",sleeve}}.dump();
""", """  return ",\\"theme_schedule\\":"+Json{{"rule","mom-volman-v1"},{"theme","price_momentum"},
      {"lookback",126},{"lag",2},{"step",21},{"sessions",sessions},{"sleeve",sleeve}}.dump();
"""),
    ("""void expect_single_row_identity(const Directory& dir,const std::string& parent,const std::string& v3) {
""", """void expect_single_row_identity(const Directory& dir,const std::string& parent,
                                const std::string& v3) {
"""),
    ("""    for (const auto* suffix:{"_daily_ic.csv","_planned_targets.csv","_combined.f64","_combined_member.u8",
                             "_combined_finite.u8"}) {
""", """    for (const auto* suffix:{"_daily_ic.csv","_planned_targets.csv","_combined.f64",
                             "_combined_member.u8","_combined_finite.u8"}) {
"""),
    ("""    for (const auto* key:{"composition_walk_forward","composition_weights_sha256"}) recipe->erase(key);
""", """    for (const auto* key:{"composition_walk_forward","composition_weights_sha256"})
      recipe->erase(key);
"""),
    ("""      for (const auto* key:{"composition_walk_forward","composition_weights_sha256","run_recipe_sha256",
                            "orientations_artifact_sha256"})
""", """      for (const auto* key:{"composition_walk_forward","composition_weights_sha256",
                            "run_recipe_sha256","orientations_artifact_sha256"})
"""),
    ("""  ASSERT_TRUE(run("wf",themed_text(weights_v3,cfg,halves,
      signs+wf_block("["+wf_row(train_session(200),first)+","+wf_row(train_session(300),second)+"]"))));
  // ew-theme-std-v1: the top-level weights leave volume_rank at 0; its row weights it in theme value.
""", """  ASSERT_TRUE(run("wf",themed_text(weights_v3,cfg,halves,
      signs+wf_block("["+wf_row(train_session(200),first)+","+wf_row(train_session(300),second)+
                     "]"))));
  // ew-theme-std-v1: the top-level weights leave volume_rank at 0; its row weights it in theme
  // value.
"""),
    ("""      for (const auto& reference:after) want.push_back(combined_payload(dir.path/reference.second/combined));
""", """      for (const auto& reference:after)
        want.push_back(combined_payload(dir.path/reference.second/combined));
"""),
    ("""  const std::string shape="walk_forward must be {rule: walk-forward-v1, window: expanding, lag: >= 2";
  const std::string row_shape="walk_forward row must be {from_session: integer, weights: {id: weight}}";
  const std::string refused_with="walk_forward (walk-forward-v1) is refused with theme_redistribution, "
                                 "theme_residualise and theme-tsmom-v1";
""", """  const std::string shape=
      "walk_forward must be {rule: walk-forward-v1, window: expanding, lag: >= 2";
  const std::string row_shape=
      "walk_forward row must be {from_session: integer, weights: {id: weight}}";
  const std::string refused_with=
      "walk_forward (walk-forward-v1) is refused with theme_redistribution, "
      "theme_residualise and theme-tsmom-v1";
"""),
    ("""      {v3(""),"composition weights schema atx.dsl-composition-weights/v3 requires a walk_forward block"},
""", """      {v3(""),
       "composition weights schema atx.dsl-composition-weights/v3 requires a walk_forward block"},
"""),
    ("""      {v3(one(R"({"volume_level":0.5,"volume_vee":0.25})")),"walk_forward weight missing: volume_rank"},
""", """      {v3(one(R"({"volume_level":0.5,"volume_vee":0.25})")),
       "walk_forward weight missing: volume_rank"},
"""),
    ("""      {v3(std_block(themes,true)+resid_block(R"(["value","price_momentum"])")+wf_block(rows)),refused_with},
""", """      {v3(std_block(themes,true)+resid_block(R"(["value","price_momentum"])")+wf_block(rows)),
       refused_with},
"""),
    ("""        if (status) { // every case reports (a mutation's teeth show per refusal), none aborts the loop
""", """        // Every case reports (a mutation's teeth show per refusal); none aborts the loop.
        if (status) {
"""),
    ("""        EXPECT_TRUE(attempt.str().empty()); EXPECT_FALSE(std::filesystem::exists(dir.path/"output"));
""", """        EXPECT_TRUE(attempt.str().empty());
        EXPECT_FALSE(std::filesystem::exists(dir.path/"output"));
"""),
]
for old, new in pairs:
    t.once(old, new)
# The two identical theme lines (WeightsV3 tests at 4913 and 5002).
themes_old = """  const std::string themes=R"({"volume_level":"value","volume_rank":"price_momentum","volume_vee":"value"})";
"""
themes_new = """  const std::string themes=
      R"({"volume_level":"value","volume_rank":"price_momentum","volume_vee":"value"})";
"""
marker = "// ---- P9 lane D3W: composition weights schema v3"
head, sep, tail = t.text.partition(marker)
assert sep and tail.count(themes_old) == 2, tail.count(themes_old)  # D3W's two; 3252 / 3987 are older
t.text = head + sep + tail.replace(themes_old, themes_new)
t.save()
print("ok")
