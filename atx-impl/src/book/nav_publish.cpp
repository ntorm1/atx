// NAV replay: publication (NAV files, capacity, holdings).
// P9 C3 (task 1, move-only): moved unchanged from strategy_nav_replay.cpp.

#include "book/nav_replay_internal.hpp"

namespace atx::impl::strategy {
using namespace nav_book;
namespace nav_book {
// The summary's warm_start record; null without a warm start. Called after the replay
// accepted the input (warm_start_sessions <= decision_begin). The role rows of the first
// warm-up decision and of score_begin (review A-3); publish_nav adds each book's gross
// leverage on row score_begin.
Json warm_start_record(const NavReplayConfig& base, const TargetReplayInput& x) {
  const usize k = base.warm_start_sessions;
  if (!k) return Json(nullptr);
  return Json{{"sessions", k},
              {"first_decision_session_ns", x.session_keys[x.decision_begin - k]},
              {"scoring_begins_session_ns", x.session_keys[x.decision_begin]},
              {"first_decision_row", x.decision_begin - k},
              {"score_begin_row", x.decision_begin},
              {"rule", "recipe warm_start_rule"}};
}
} // namespace nav_book
namespace {
// Review A-3: each book's gross leverage on row score_begin (its first reported row: the
// daily CSV's gross_leverage there, after that session's EXECUTE), keyed by book label. The
// replay refused the run if every one is 0.
Json score_begin_gross(const NavRun& run, bool tiered) {
  Json gross = Json::object();
  for (usize k = 0; k < run.scenarios.size(); ++k) {
    const auto& days = run.results[k].days;
    gross[scenario_label(run.scenarios[k], tiered)] =
        days.empty() ? Json(nullptr) : finite_or_null(days.front().gross_leverage);
  }
  return gross;
}
} // namespace
namespace nav_book {
// The summary's label_role record (v8 E-25); null without --label-role. The recipe binds the
// manifest SHA-256 and the rule.
Json label_record(const NavLabelRolePin& pin, const detail::LoadedLabelRole& label) {
  if (pin.manifest_path.empty()) return Json(nullptr);
  return Json{{"manifest_sha256", pin.manifest_sha256},
              {"label_only_present_cells", label.label_only_cells},
              {"label_only_present_cells_scored", label.label_only_scored_cells},
              {"basis", "cells present in the label role and absent in --role, over every role "
                        "row and over rows [score_begin, score_end); the rule is the recipe's "
                        "label_role.rule"}};
}
} // namespace nav_book
namespace {
// construction.v5 of one book over its decision rows: the rule's plan (pre locate
// rule) gross, net and held share of the decision's members.
Json aim_partial_json(const TargetReplayConfig& target, const NavReplayResult& result) {
  std::vector<detail::AimPartialDecision> decisions;
  for (const auto& d : result.days)
    if (d.decision)
      decisions.push_back({d.planned_gross, d.planned_net, d.planned_held_names,
                           d.decision_members});
  return Json::parse(detail::aim_partial_summary_json(target, decisions));
}
co::Result<Json> publish_scenario(const NavRun& run, const std::filesystem::path& dir, usize k,
                                  std::ostream& progress) {
  const bool tiered = !run.fields.is_null();
  const auto& sc = run.scenarios[k]; const auto& result = run.results[k];
  const auto& summary = run.summaries[k];
  const auto label = scenario_label(sc, tiered);
  const auto daily = dir / ("daily_" + label + ".csv");
  const auto events = dir / ("events_" + label + ".csv");
  const bool construction = detail::construction_active(run.cfg.target);
  ATX_TRY_VOID(write_daily(daily, result, construction, tiered));
  ATX_TRY_VOID(write_events(events, result));
  ATX_TRY(auto daily_sha, timed_digest(run.times, [&] { return co::sha256_file(daily.string()); }));
  ATX_TRY(auto events_sha,
          timed_digest(run.times, [&] { return co::sha256_file(events.string()); }));
  auto entry = scenario_summary(sc, k == nav_primary_scenario_index, tiered, result, summary,
                                daily_sha, events_sha);
  // P9 C3 (Rulings PRE-7 / PRE-7a): print-only, under --ann-mean-gross alone.
  if (run.ann_mean_gross) entry["ann_mean_gross"] = summary.ann_mean_gross;
  // P9 AL-CLOCK (Ruling ALCLOCK-1): print-only, under --info-clock-print alone.
  if (run.info_clock)
    entry["info_clock"] =
        info_clock_json(run.base, result.construction.info_clock, *run.info_clock);
  if (construction) {
    std::vector<ConstructionDay> decisions;
    for (const auto& d : result.days)
      if (d.decision) decisions.push_back(d.construction);
    entry.update(Json::parse(detail::construction_summary_json(run.cfg.target, decisions)));
    if (run.cfg.target.rule == TargetReplayRule::AimPartialV5) {
      auto v5 = aim_partial_json(run.cfg.target, result);
      if (run.base.rate == NavRateRule::PerNameV1) {
        v5["rate"] = "per-name-v1";
        v5["rate_stats"] = rate_stats_json(result.construction.rate_stats);
      }
      if (run.base.rate == NavRateRule::InfoClockV1) v5["rate"] = nav_info_clock_rate_id; // P9
      entry["construction"]["v5"] = std::move(v5);
    }
  }
  progress << "nav replay " << label << ": net Sharpe " << std::setprecision(6)
           << summary.net_sharpe << ", mean daily GMV turnover " << summary.daily_turnover_mean
           << " (p95 " << summary.daily_turnover_p95 << "), financing $"
           << summary.long_financing_dollars + summary.borrow_dollars << " (short $"
           << summary.borrow_dollars << "), blocked short $" << summary.blocked_short_dollars
           << ", mean monthly turnover " << summary.mean_monthly_turnover << " (ex deployment "
           << summary.mean_monthly_turnover_ex_deployment << ")\n";
  return co::Ok(std::move(entry));
}
constexpr const char* producer_declaration =
    "P9 C1 (NAV receipt completeness): the executable that wrote this directory: engine_git_sha "
    "= the configure-time commit of its source tree (\"-dirty\": the tree had local changes; "
    "\"unknown\": no git), build_type = release (NDEBUG) or debug; a comparison of two builds' "
    "summaries drops this key (every other byte is the replay's)";
// summary.json's producer record (main directories only).
Json producer_json() {
#ifdef NDEBUG
  constexpr const char* build_type = "release";
#else
  constexpr const char* build_type = "debug";
#endif
  return Json{{"engine_git_sha", std::string(build_engine_git_sha())},
              {"build_type", build_type},
              {"definition", producer_declaration}};
}
co::Result<std::string> publish_capacity(const NavRun& run, std::ostream& progress);
} // namespace
namespace nav_book {
// Exclusive directory: recipe.json, per-scenario CSVs, summary.json LAST. P9 C1: under the v7
// seam the capacity directory, the v7 files and v7_extras.json come before summary.json, which
// binds v7_extras.json and capacity/summary.json (v7.files) and names its producer.
co::Status publish_nav(const NavRun& run, std::ostream& progress) {
  const auto& cfg = run.cfg;
  const bool tiered = !run.fields.is_null();
  const auto dir = std::filesystem::path(cfg.output_directory);
  if (!std::filesystem::create_directory(dir))
    return co::Err(co::ErrorCode::AlreadyExists, "nav replay: output must not exist");
  const auto method = nav_recipe(cfg, run.base, run.scenarios, run.limits, run.fields, run.label);
  const auto method_text = method.dump();
  ATX_TRY(auto method_sha, timed_digest(run.times, [&] { return co::sha256_hex(method_text); }));
  ATX_TRY_VOID(write_json(dir / "recipe.json", method));
  Json list = Json::array();
  for (usize k = 0; k < run.scenarios.size(); ++k) {
    ATX_TRY(auto entry, publish_scenario(run, dir, k, progress));
    list.push_back(std::move(entry));
  }
  auto bindings = Json::parse(run.manifest_json);
  // Equal-weight blends carry no weights key; pinned-weight blends bind their SHA.
  const auto weights = bindings.contains("composition_weights_sha256")
      ? bindings.at("composition_weights_sha256") : Json(nullptr);
  const auto semantics = bindings.at("signal_semantics");
  Json summary{{"schema", "atx.dsl-nav-replay-summary/v1"}, {"status", "complete"},
      {"recipe_sha256", method_sha}, {"combined_sha256", cfg.combined_sha256},
      {"role_sha256", cfg.role_sha256}, {"rule", detail::construction_rule_id(cfg.target)},
      {"primary_scenario", scenario_label(run.scenarios[nav_primary_scenario_index], tiered)},
      {"primary_financing_available", tiered}, {"financing_fields", run.fields},
      {"signal_semantics", semantics}, {"composition_weights_sha256", weights},
      {"scenarios", std::move(list)}, {"source_bindings", std::move(bindings)},
      {"self_financing_nav", true}, {"capacity_qualified", false},
      {"limitations", limitations_declaration}};
  // v6 execution options, only when non-default (the default summary is unchanged).
  if (run.base.order_basis == NavOrderBasis::Delta) summary["order_basis"] = "delta";
  if (run.base.locate_in_aim) {
    usize zeroed = 0; // the construction is shared: every book carries the same records
    for (const auto& d : run.results.front().days) zeroed += d.construction.locate_zeroed;
    summary["locate_in_aim"] = Json{{"zeroed_special_short_aims", zeroed},
        {"basis", "member-decisions whose negative tied-rank aim was set to 0 (special tier)"}};
  }
  if (!run.warm_start.is_null()) { // v8, only when on
    auto warm = run.warm_start;
    warm["score_begin_gross_leverage"] = score_begin_gross(run, tiered); // review A-3
    summary["warm_start"] = std::move(warm);
  }
  if (!run.label.is_null()) summary["label_role"] = run.label; // v8 E-25, only when on
  if (run.records) {
    // P9 C1: the v7 seam's files from this run's own books, before this summary, which binds
    // them; the capacity directory's summary carries the v7 block of its pass, unbound.
    Json binding(nullptr);
    if (!run.capacity_pass) {
      std::string capacity_sha;
      if (!run.capacity_scenarios.empty()) {
        ATX_TRY(capacity_sha, publish_capacity(run, progress));
      }
      ATX_TRY_VOID(v7::publish_extras(dir, *run.records, capacity_sha, binding));
    }
    v7::extend_summary(summary, *run.records, binding);
  } else {
    v7::extend_summary(summary); // L4 hook: identity unless extended
  }
  if (run.times && run.summary_timers) { // v8 --stage-timers: the one key a clock writes
    summary["stage_seconds"] = run.times->json();
    progress << "nav replay stage seconds: " << summary.at("stage_seconds").dump() << '\n';
  }
  if (!run.capacity_pass) summary["producer"] = producer_json(); // P9 C1
  if (run.calibration && !run.capacity_pass) summary["calibration"] = *run.calibration; // P9 C2
  return write_json(dir / "summary.json", summary);
}
} // namespace nav_book
namespace {
// P9 C1: <output>/capacity, the capacity books' directory (recipe.json, daily and events CSVs,
// summary.json), the bytes the capacity pass published before (its declarations read pass
// "capacity"; no stage_seconds, no producer); returns its summary.json's SHA-256.
co::Result<std::string> publish_capacity(const NavRun& run, std::ostream& progress) {
  const v7::CapacityPublication pass;
  auto own = run.cfg;
  const auto dir = std::filesystem::path(run.cfg.output_directory) / "capacity";
  own.output_directory = dir.string();
  const NavRun capacity{own,
                        run.limits,
                        run.base,
                        run.capacity_scenarios,
                        run.capacity_results,
                        run.capacity_summaries,
                        run.manifest_json,
                        run.fields,
                        run.warm_start,
                        run.times,
                        false,
                        run.label,
                        {},
                        {},
                        {},
                        run.records,
                        true};
  ATX_TRY_VOID(publish_nav(capacity, progress));
  return timed_digest(run.times,
                      [&] { return co::sha256_file((dir / "summary.json").string()); });
}
constexpr const char* holdings_declaration =
    "primary book; one holdings.csv row per name with any state at the end of each decision "
    "or execution session t (held, the decision's plan, a working order or an EXECUTE outcome "
    "nonzero; -0 counts; an omitted name has every weight 0): held_dollars after EXECUTE at t "
    "(what DECIDE at t reads), held_weight = held_dollars / nav_post (the plan's current "
    "weight, bit for bit); fill/filled_dollars/fill_cost_dollars/unfilled_dollars = EXECUTE at "
    "the close of t (orders placed at earlier decisions); desired_weight = the shared desired "
    "target on effective rebalances (else nan); rule_weight = the target rule's plan and "
    "target_weight = the planned weight after the locate block; planned_trade_weight = "
    "target_weight - held_weight; planned_trade_dollars = order_dollars - held_dollars when "
    "order_placed (else 0); order_dollars = the working order in decision-NAV dollars after "
    "DECIDE (nan when none); decision fields are nan on the final execution-only session; "
    "holdings_days.csv: one row per session, neutralize_skipped = a cadence rebalance the "
    "neutralization guard skipped (ERROR: the book kept its weights)";
// Only with a declared hold band (v8 E-16); without one the manifest has no such key.
constexpr const char* hold_band_state_declaration =
    "v8 E-16, hold band declared (b > 0): every row carries rank_set and desired_prev, the "
    "hold-band state DECIDE at t read (the state entering session t's construction; on the "
    "final execution-only session the state after the last decision; nan = unset), and a name "
    "holding a set rank is a name with state (a row even when flat); holdings.csv appends the "
    "two columns, holdings.f64 rows are 13 values (holdings_index.json names the columns); "
    "decide --positions reads them, so decide --check-replay at t reproduces the book";
} // namespace
namespace nav_book {
bool neutralize_skipped(NeutralizeOutcome outcome) {
  return outcome != NeutralizeOutcome::NotAttempted && outcome != NeutralizeOutcome::Applied;
}
const char* side_label(f64 held) { return held > 0 ? "long" : held < 0 ? "short" : "flat"; }
// manifest.json LAST (after the NAV directory): file SHAs and the NAV recipe SHA. The csv
// format keeps atx.nav-holdings/v1 byte for byte; f64 is v2 (+ format, the index schema).
co::Status publish_holdings(const NavRun& run, const HoldingsEmitter& csv,
                            std::ostream& progress) {
  const bool tiered = !run.fields.is_null();
  const auto method = nav_recipe(run.cfg, run.base, run.scenarios, run.limits, run.fields,
                                 run.label);
  ATX_TRY(auto method_sha, co::sha256_hex(method.dump()));
  const auto& dir = csv.directory();
  ATX_TRY(auto files, csv.files());
  const bool binary = csv.format() == NavHoldingsFormat::F64;
  const auto book = scenario_label(run.scenarios[nav_primary_scenario_index], tiered);
  Json manifest{{"schema", binary ? holdings::manifest_schema_v2 : "atx.nav-holdings/v1"},
      {"status", "complete"},
      {"book", book}, {"nav_recipe_sha256", method_sha},
      {"combined_sha256", run.cfg.combined_sha256}, {"role_sha256", run.cfg.role_sha256},
      {"rule", detail::construction_rule_id(run.cfg.target)},
      {"sessions", csv.sessions}, {"decision_sessions", csv.decisions}, {"rows", csv.rows},
      {"neutralize_skipped_rebalances", csv.skipped_rebalances},
      {"files", std::move(files)}, {"columns", holdings_declaration}};
  if (binary)
    manifest["format"] = Json{{"id", "f64"}, {"index_schema", holdings::index_schema},
        {"rows", "holdings.f64 (layout in holdings_index.json) carries every holdings.csv "
                 "column of v1: the stored ones exactly, the derived ones by the declared "
                 "expressions"}};
  if (csv.hold_state()) manifest["hold_band_state"] = hold_band_state_declaration; // v8 E-16
  // v8 D-0 warm start only (review A-3): K and the role row of the first reported session.
  if (!run.warm_start.is_null())
    manifest["warm_start"] = Json{{"warm_start_sessions", run.warm_start.at("sessions")},
                                  {"score_begin_row", run.warm_start.at("score_begin_row")}};
  v7::extend_holdings(manifest); // L4 hook: identity unless extended
  progress << "nav replay holdings " << book << ": " << csv.rows << " rows over "
           << csv.sessions << " sessions\n";
  if (csv.skipped_rebalances)
    progress << "nav replay holdings ERROR: " << csv.skipped_rebalances
             << " cadence rebalance(s) skipped by the neutralization guard\n";
  return write_json(dir / "manifest.json", manifest);
}
} // namespace nav_book
} // namespace atx::impl::strategy
