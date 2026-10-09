// NAV replay: run_nav_replay, calibration and the grid run.
// P9 C3 (task 1, move-only): moved unchanged from strategy_nav_replay.cpp.

#include "book/nav_replay_internal.hpp"
#include "book/nav_spec.hpp" // parse_unique_keys (Ruling C3-7)

namespace atx::impl::strategy {
using namespace nav_book;
co::Status run_nav_replay(const TargetReplayRunConfig& cfg, std::ostream& progress) {
  return run_nav_replay(cfg, NavTurnoverLimits{}, progress);
}
co::Status run_nav_replay(const TargetReplayRunConfig& cfg, const NavTurnoverLimits& limits,
                          std::ostream& progress) {
  return run_nav_replay(cfg, limits, NavFieldsPin{}, progress);
}
co::Status run_nav_replay(const TargetReplayRunConfig& cfg, const NavTurnoverLimits& limits,
                          const NavFieldsPin& fields, std::ostream& progress) {
  return run_nav_replay(cfg, limits, fields, NavRateOptions{}, progress);
}
co::Status run_nav_replay(const TargetReplayRunConfig& cfg, const NavTurnoverLimits& limits,
                          const NavFieldsPin& fields, const NavRateOptions& rate,
                          std::ostream& progress) {
  return run_nav_replay(cfg, limits, fields, rate, NavExecutionOptions{}, progress);
}
co::Status run_nav_replay(const TargetReplayRunConfig& cfg, const NavTurnoverLimits& limits,
                          const NavFieldsPin& fields, const NavRateOptions& rate,
                          const NavExecutionOptions& execution, std::ostream& progress) {
  return run_nav_replay(cfg, limits, fields, rate, execution, NavEmitOptions{}, progress);
}
namespace {
// The run-level contract of the nav verb (run_nav_replay and run_nav_grid).
co::Status check_run(const TargetReplayRunConfig& cfg, const NavTurnoverLimits& limits,
                     const NavFieldsPin& fields, const NavLabelRolePin& label) {
  if (cfg.role_path.empty() || cfg.role_sha256.empty() || cfg.output_directory.empty())
    return co::Err(co::ErrorCode::InvalidArgument, "nav replay: pinned role and output required");
  if (cfg.target.one_way_bps != 0 || cfg.target.annual_borrow_bps != 0)
    return co::Err(co::ErrorCode::InvalidArgument,
                   "nav replay: costs and borrow are fixed scenarios, not flags");
  if (!std::isfinite(limits.daily_mean_max) || !(limits.daily_mean_max > 0) ||
      !std::isfinite(limits.daily_p95_max) || !(limits.daily_p95_max > 0))
    return co::Err(co::ErrorCode::InvalidArgument, "nav replay: daily turnover ceilings");
  if (fields.manifest_path.empty() != fields.manifest_sha256.empty())
    return co::Err(co::ErrorCode::InvalidArgument,
                   "nav replay: --fields and --fields-sha256 go together");
  if (neutralize_by_industry(cfg.target.neutralize) && fields.manifest_path.empty())
    return co::Err(co::ErrorCode::InvalidArgument,
                   "nav replay: the industry ids need --fields (grp_ff12)");
  if (label.manifest_path.empty() != label.manifest_sha256.empty())
    return co::Err(co::ErrorCode::InvalidArgument,
                   "nav replay: --label-role and --label-role-sha256 go together");
  return co::Ok();
}
// The book config every scenario of a nav run starts from: the target and the options.
NavReplayConfig run_base(const TargetReplayConfig& target, const NavRateOptions& rate,
                         const NavExecutionOptions& execution) {
  NavReplayConfig base; base.target = target;
  base.rate = rate.rate; base.rate_rra = rate.rate_rra; base.rate_lambda = rate.rate_lambda;
  base.rate_min = rate.rate_min; base.rate_max = rate.rate_max;
  base.order_basis = execution.order_basis; base.locate_in_aim = execution.locate_in_aim;
  base.liquidity_cache = execution.liquidity_cache;
  base.warm_start_sessions = execution.warm_start_sessions;
  base.book_workers = execution.book_workers;
  return base;
}
// P9 AL-CLOCK: the info_clock block's field record (null without a loaded clock).
Json info_clock_field(const LoadedFields& loaded) {
  if (loaded.info_clock.empty()) return Json(nullptr);
  return Json{{"name", nav_info_clock_field}, {"sha256", loaded.info_clock_sha256}};
}
} // namespace
namespace nav_book {
// Command-line numbers: the whole value, else std::invalid_argument.
f64 parse_real(const std::string& value) {
  usize used = 0; const f64 x = std::stod(value, &used);
  if (used != value.size()) throw std::invalid_argument("invalid number");
  return x;
}
u64 parse_integer(const std::string& value) {
  u64 x = 0;
  const auto parsed = std::from_chars(value.data(), value.data() + value.size(), x);
  if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size())
    throw std::invalid_argument("invalid integer");
  return x;
}
// The construction flags (nav_grid_variant_flags), parsed exactly as the command line
// parses them (the command line calls this); false for any other key. A bad value throws
// std::invalid_argument (std::out_of_range from std::stod).
bool apply_construction_flag(const std::string& key, const std::string& value,
                             TargetReplayConfig& target) {
  if (key == "--rule") {
    if (value == "baseline-v1") target.rule = TargetReplayRule::BaselineTargetV1;
    else if (value == "monthly-budget-v2") target.rule = TargetReplayRule::MonthlyTargetBudgetV2;
    else if (value == "aim-partial-v5") target.rule = TargetReplayRule::AimPartialV5;
    else throw std::invalid_argument("unknown target rule");
  } else if (key == "--cadence") {
    const auto x = parse_integer(value);
    if (x > max_dates) throw std::invalid_argument("cadence exceeds bound");
    target.cadence = static_cast<usize>(x);
  } else if (key == "--trade-fraction") {
    target.trade_fraction = parse_real(value);
  } else if (key == "--monthly-budget") {
    target.monthly_budget = parse_real(value);
  } else if (key == "--band-multiple") {
    target.band_multiple = parse_real(value);
  } else if (key == "--dust-multiple") {
    target.dust_multiple = parse_real(value);
  } else if (key == "--aim-leverage") {
    target.aim_leverage = parse_real(value);
  } else if (key == "--exit-rate") {
    target.exit_rate = parse_real(value);
  } else {
    return false;
  }
  return true;
}
} // namespace nav_book
namespace {
constexpr const char* grid_file_schema = "atx.nav-construction-grid/v1";
} // namespace
namespace nav_book {
co::Result<GridSpec> read_grid(const std::string& path) {
  std::error_code ec;
  const auto size = std::filesystem::file_size(path, ec);
  if (ec || size > max_grid_file_bytes)
    return co::Err(co::ErrorCode::InvalidArgument,
                   "nav grid: the grid file is missing or above 1 MiB");
  std::string text(static_cast<usize>(size), '\0');
  std::ifstream file(path, std::ios::binary);
  if (!file || !file.read(text.data(), static_cast<std::streamsize>(size)))
    return co::Err(co::ErrorCode::IoError, "nav grid: grid file read");
  GridSpec spec;
  ATX_TRY(spec.sha256, co::sha256_hex(text));
  // Ruling C3-7 (review C3 S-9): R-1's refusal on a file a NavSpec can name (construction_grid):
  // the parser would run a repeated key's last value while a reader of the file sees the first.
  std::string duplicate;
  const auto doc = nav_spec::parse_unique_keys(text, duplicate);
  if (!doc.is_discarded() && !duplicate.empty())
    return co::Err(co::ErrorCode::InvalidArgument,
                   "nav grid: duplicate key \"" + duplicate + "\" in " + path +
                       " (a key appears once per object: JSON keeps the last value, a reader the "
                       "first)");
  const auto is_string = [](const Json& j, const char* key) {
    return j.contains(key) && j.at(key).is_string();
  };
  if (doc.is_discarded() || !doc.is_object() || doc.size() != 2 || !is_string(doc, "schema") ||
      doc.at("schema").get<std::string>() != grid_file_schema || !doc.contains("variants") ||
      !doc.at("variants").is_array())
    return co::Err(co::ErrorCode::InvalidArgument,
                   "nav grid: a grid file is {\"schema\": \"atx.nav-construction-grid/v1\", "
                   "\"variants\": [...]}");
  for (const auto& v : doc.at("variants")) {
    if (!v.is_object() || v.size() != 2 || !is_string(v, "id") || !v.contains("flags") ||
        !v.at("flags").is_object())
      return co::Err(co::ErrorCode::InvalidArgument,
                     "nav grid: a variant is {\"id\": ..., \"flags\": {...}}");
    par::GridVariant variant{v.at("id").get<std::string>(), {}};
    const auto& flags = v.at("flags");
    for (auto it = flags.begin(); it != flags.end(); ++it) {
      if (!it.value().is_string())
        return co::Err(co::ErrorCode::InvalidArgument,
                       "nav grid: flag values are strings, as on the command line");
      variant.overrides.emplace_back(it.key(), it.value().get<std::string>());
    }
    spec.variants.push_back(std::move(variant));
  }
  ATX_TRY_VOID(par::validate_grid(spec.variants,
                                  par::GridRules{nav_max_grid_variants, nav_grid_variant_flags}));
  return co::Ok(std::move(spec));
}
} // namespace nav_book
namespace {
// The target of each variant: the base's with the variant's flags applied.
co::Result<std::vector<TargetReplayConfig>> variant_targets(const GridSpec& grid,
                                                            const TargetReplayConfig& base) {
  std::vector<TargetReplayConfig> out;
  out.reserve(grid.variants.size());
  for (const auto& variant : grid.variants) {
    auto& target = out.emplace_back(base);
    try {
      for (const auto& [key, value] : variant.overrides)
        if (!apply_construction_flag(key, value, target))
          return co::Err(co::ErrorCode::InvalidArgument, "nav grid: may not set " + key);
    } catch (const std::exception& e) {
      return co::Err(co::ErrorCode::InvalidArgument,
                     "nav grid: variant " + variant.id + ": " + e.what());
    }
  }
  return co::Ok(std::move(out));
}
constexpr const char* grid_declaration =
    "v8 D-1 construction grid: one pinned load; one shared construction per decision "
    "(borrow tiers, price exposures from one session ring, desired target, neutralization) "
    "for every variant, formed on the union of the variants' cadence days (a variant whose "
    "cadence misses a decision plans it idle, as it does alone); each variant's scenario "
    "books run their own MARK, EXECUTE, DECIDE and close in lockstep. <id>/ is byte for "
    "byte the directory of the standalone nav run with the base flags and the variant's "
    "flags; this manifest is written last";
// Only with --adv-hold-q and several aim leverages (review A-1).
constexpr const char* leverage_groups_declaration =
    "v8 adv-hold-v1 caps the shared desired target at Q ADV / (aim_leverage NAV) and "
    "--aim-leverage is a variant flag: the variants run in one lockstep per distinct aim "
    "leverage (these groups, in grid order), each group's shared construction capped at its "
    "own leverage, so every <id>/ stays byte for byte its standalone run";
// Only with --two-speed and several aim leverages (v8 Y-5, review YCOMB #2).
constexpr const char* two_speed_groups_declaration =
    "v8 two-speed-v1 forms the shared desired target from the fast sleeve F, which moves "
    "toward L m_f d_f, and --aim-leverage is a variant flag: the variants run in one lockstep "
    "per distinct aim leverage (these groups, in grid order), each group's shared "
    "construction carrying its own F at its own leverage on the grid's one cadence, so every "
    "<id>/ stays byte for byte its standalone run";
} // namespace
co::Status run_nav_replay(const TargetReplayRunConfig& cfg, const NavTurnoverLimits& limits,
                          const NavFieldsPin& fields, const NavRateOptions& rate,
                          const NavExecutionOptions& execution, const NavEmitOptions& emit,
                          std::ostream& progress) {
  return run_nav_replay(cfg, limits, fields, rate, execution, emit, NavCalibrateOptions{},
                        progress);
}
namespace {
// P9 C2: the options, the rule they need (aim_leverage is aim-partial-v5's key) and no spo rule
// (its engines keep the rows of every replay of the process, pass 1's included).
co::Status check_calibrate(const TargetReplayConfig& target, const NavCalibrateOptions& options) {
  ATX_TRY_VOID(validate_calibrate(options));
  if (!options.on()) return co::Ok();
  if (target.rule != TargetReplayRule::AimPartialV5)
    return co::Err(co::ErrorCode::InvalidArgument,
                   "nav calibrate: --calibrate-gross needs --rule aim-partial-v5 (its aim "
                   "leverage is what PM6-6 corrects)");
  if (v7::shared_plan_state())
    return co::Err(co::ErrorCode::InvalidArgument,
                   "nav calibrate: --calibrate-gross is refused with an spo rule (its engines "
                   "would keep the calibration pass's rows)");
  return co::Ok();
}
// The S2 book's all-rows mean gross leverage: numpy's mean of its daily gross_leverage column
// over every row (nav_summ mean_gross_leverage_all_rows).
f64 all_rows_gross(const NavReplayResult& book) {
  std::vector<f64> gross;
  gross.reserve(book.days.size());
  for (const auto& day : book.days) gross.push_back(day.gross_leverage);
  return numpy_mean(gross);
}
template <class T> u64 bytes_of(const std::vector<T>& v) { return u64{v.size()} * sizeof(T); }
// What the admitted run holds after its load (blend, fields, label marks).
u64 payload_bytes(const AdmittedRun& a) {
  const auto& b = a.blend;
  return bytes_of(b.signal) + bytes_of(b.close) + bytes_of(b.raw) + bytes_of(b.volume) +
         bytes_of(b.member) + bytes_of(b.present) + bytes_of(b.sessions) + bytes_of(b.ids) +
         bytes_of(b.sleeve_fast) + bytes_of(b.sleeve_slow) + bytes_of(b.sleeve_fast_share) +
         bytes_of(a.fields.shares_out) + bytes_of(a.fields.si_shares) +
         bytes_of(a.fields.industry) + bytes_of(a.label.close) + bytes_of(a.label.raw) +
         bytes_of(a.label.present);
}
// The decisions a construction record may hold: every session of the load plus the warm-up.
usize construction_decisions(const NavReplayConfig& base, const AdmittedRun& admitted) {
  return admitted.blend.end - admitted.blend.begin + base.warm_start_sessions;
}
// Whether pass 2 may replay pass 1's construction: the construction reads nothing that differs
// between the passes (adv-hold-v1's cap reads L, two-speed-v1's F moves toward L and DECIDE reads
// it, and the holdings export of a declared hold band reads the band state, which a restored
// pass does not advance), and its record fits what --max-bytes leaves after the workspace
// reserve of every book and the load (max_working_bytes is a recipe key: it is never raised).
bool construction_reusable(const TargetReplayRunConfig& cfg, const NavReplayConfig& base,
                           const AdmittedRun& admitted, usize books, bool tiered,
                           bool holdings) {
  const auto& target = base.target;
  if (target.adv_hold_q > 0 || two_speed_on(target) || (holdings && hold_band_declared(target)))
    return false;
  const auto& blend = admitted.blend;
  const u64 reserve = nav_workspace_reserve_bytes(base, books, tiered, blend.names,
                                                  blend.end - blend.begin, holdings);
  const u64 held = reserve + payload_bytes(admitted);
  if (cfg.target.max_working_bytes <= held) return false;
  return construction_tape_bytes(construction_decisions(base, admitted), blend.names, tiered) <=
         cfg.target.max_working_bytes - held;
}
// P9 C2 pass 1: the primary S2 book alone at the requested leverage (no other main book, no
// capacity book, no observer; the v7 seam's records untouched), recording the shared construction
// into `tape` when given; then PM6-6's step (strategy_nav_calibrate.hpp).
co::Result<NavCalibration> calibration_pass(const NavReplayInput& input,
                                            const NavReplayConfig& requested,
                                            std::span<const NavScenario> scenarios,
                                            const NavCalibrateOptions& options,
                                            ConstructionTape* tape, StageClock* stages) {
  const auto primary = scenarios.subspan(nav_primary_scenario_index, 1);
  ATX_TRY(const auto pass, replay_books(input, std::span<const NavReplayConfig>(&requested, 1),
                                        primary, Observer{}, stages, tape, false));
  NavCalibration c;
  c.options = options;
  c.leverage_requested = requested.target.aim_leverage;
  c.gross_requested = all_rows_gross(pass.front().front());
  ATX_TRY(c.step, calibrate_step(c.leverage_requested, options.target_gross, c.gross_requested,
                                 options.tolerance));
  c.construction_reused = tape != nullptr;
  return co::Ok(std::move(c));
}
} // namespace
co::Status run_nav_replay(const TargetReplayRunConfig& cfg, const NavTurnoverLimits& limits,
                          const NavFieldsPin& fields, const NavRateOptions& rate,
                          const NavExecutionOptions& execution, const NavEmitOptions& emit,
                          const NavCalibrateOptions& calibrate, std::ostream& progress) {
  try {
    StageTimes times; // --stage-timers: the clock starts at entry
    StageTimes* const timed = emit.stage_timers ? &times : nullptr;
    ATX_TRY_VOID(check_run(cfg, limits, fields, execution.label_role));
    ATX_TRY_VOID(check_calibrate(cfg.target, calibrate)); // P9 C2
    const bool tiered = !fields.manifest_path.empty();
    // L4 hook: the books; P9 C1: the capacity books of --capacity-curve in the same lockstep,
    // from lockstep.capacity_begin on (lockstep.books.size() without them).
    const auto lockstep = v7::lockstep_scenarios(nav_scenario_matrix(tiered));
    const auto& scenarios = lockstep.books;
    const usize main_books = lockstep.capacity_begin;
    // P9 C2: the run as requested (admission, the calibration pass); without --calibrate-gross
    // the published run is exactly this one.
    const auto requested = run_base(cfg.target, rate, execution);
    const bool holdings = !emit.holdings_directory.empty();
    if (holdings) {
      const auto nav_dir = std::filesystem::path(cfg.output_directory);
      const auto holdings_dir = std::filesystem::path(emit.holdings_directory);
      if (nav_dir.lexically_normal() == holdings_dir.lexically_normal() ||
          std::filesystem::exists(nav_dir) || std::filesystem::exists(holdings_dir))
        return co::Err(co::ErrorCode::AlreadyExists,
                       "nav replay: --output and --emit-holdings must differ and not exist");
    }
    // Admission (the reserve at the pinned role's own geometry, then the fields and the
    // loader against the rest) and the pinned load (v8 E-25: with the label role's marks).
    ATX_TRY(auto admitted, admit_and_load(cfg, requested, fields, scenarios.size(), holdings,
                                          execution.label_role, emit.info_clock_print));
    times.loaded = StageTimes::Clock::now();
    log_fields_seal(admitted.fields, progress); // P9 C3 (P13): a legacy manifest, logged
    const auto& blend = admitted.blend; const auto& loaded = admitted.fields;
    const auto& marks = admitted.label; // empty without --label-role: MARK reads the role
    auto view = blend.view();
    view.industry = loaded.industry; // empty unless an industry id
    const NavReplayInput input{view, blend.volume,
                               NavFinancingFields{loaded.shares_out, loaded.si_shares},
                               NavMarkPrices{marks.close, marks.raw, marks.present},
                               loaded.info_clock}; // P9 AL-CLOCK: empty unless rule or print
    StageClock* const stages = timed ? &times.replay : nullptr;
    // P9 C2 (--calibrate-gross): pass 1 on this load, then the published run at the final
    // leverage; `published` is the plain run's config at that leverage (cfg without the flag).
    TargetReplayRunConfig published = cfg;
    NavCalibration calibration;
    ConstructionTape tape;
    if (calibrate.on()) {
      const bool reuse = construction_reusable(cfg, requested, admitted, scenarios.size(), tiered,
                                               holdings);
      if (reuse)
        reserve_construction(tape, construction_decisions(requested, admitted), blend.names,
                             tiered);
      ATX_TRY(calibration, calibration_pass(input, requested, scenarios, calibrate,
                                            reuse ? &tape : nullptr, stages));
      published.target.aim_leverage = calibration.step.leverage;
      tape.replay = true;
      const auto precision = progress.precision(17); // console only
      progress << "nav calibrate: pass 1 (S2 alone) at L " << calibration.leverage_requested
               << ": G " << calibration.gross_requested << " vs target "
               << calibrate.target_gross << " (tolerance " << calibrate.tolerance << ") -> L "
               << calibration.step.leverage
               << (calibration.step.corrected ? " (one correction)" : " (kept)")
               << "; construction " << (reuse ? "reused" : "formed again") << '\n';
      progress.precision(precision);
    }
    const auto base = run_base(published.target, rate, execution);
    ConstructionTape* const restore =
        calibrate.on() && calibration.construction_reused ? &tape : nullptr;
    // All scenarios in lockstep: the desired target (and its price exposures) and
    // the borrow tiers are formed once per decision for every scenario book. With
    // --emit-holdings the primary book is observed (read only) while it runs.
    const auto replay = [&](const Observer& observer) -> co::Result<std::vector<NavReplayResult>> {
      ATX_TRY(auto grid, replay_books(input, std::span<const NavReplayConfig>(&base, 1),
                                      scenarios, observer, stages, restore));
      return co::Ok(std::move(grid.front()));
    };
    std::vector<NavReplayResult> results;
    HoldingsEmitter csv;
    if (holdings) {
      ATX_TRY_VOID(csv.open(emit.holdings_directory, emit.format, view.instrument_ids,
                            hold_band_declared(base.target)));
      ATX_TRY(results, replay(Observer{&csv, nav_primary_scenario_index}));
      ATX_TRY_VOID(csv.close());
    } else {
      ATX_TRY(results, replay(Observer{}));
    }
    times.replayed = StageTimes::Clock::now();
    Json calibration_record(nullptr);
    if (calibrate.on()) {
      calibration.gross = all_rows_gross(results[nav_primary_scenario_index]);
      calibration_record = Json::parse(calibration_summary_json(calibration));
      const auto precision = progress.precision(17); // console only
      progress << "nav calibrate: published at L " << calibration.step.leverage << ": G "
               << calibration.gross << " ("
               << (calibration_matched(calibration) ? "matched" : "NOT matched: a stop") << ")\n";
      progress.precision(precision);
    }
    std::vector<NavSummary> summaries; summaries.reserve(scenarios.size());
    for (const auto& result : results) {
      ATX_TRY(auto summary, summarize_nav(result, limits));
      summaries.push_back(std::move(summary));
    }
    // L4 hook: records the books; spo's specific-ceiling tripwire voids the run here, before
    // publish_nav creates the output directory (Ok without an extension).
    ATX_TRY_VOID(v7::capture(scenarios, results, summaries));
    const Json warm = warm_start_record(base, view);
    const Json label = label_record(execution.label_role, marks);
    // P9 C1: the main books, the capacity books and the run's v7 records (its own books').
    const std::span<const NavScenario> all_scenarios(scenarios);
    const std::span<const NavReplayResult> all_results(results);
    const std::span<const NavSummary> all_summaries(summaries);
    const v7::RunRecords records = v7::run_records(all_scenarios, all_results, all_summaries);
    const Json clock = info_clock_field(loaded); // P9 AL-CLOCK: printed only under the flag
    const NavRun run{published,
                     limits,
                     base,
                     all_scenarios.first(main_books),
                     all_results.first(main_books),
                     all_summaries.first(main_books),
                     blend.manifest_json,
                     loaded.binding,
                     warm,
                     timed,
                     true,
                     label,
                     all_scenarios.subspan(main_books),
                     all_results.subspan(main_books),
                     all_summaries.subspan(main_books),
                     v7::installed() ? &records : nullptr,
                     false,
                     calibration_record.is_null() ? nullptr : &calibration_record,
                     emit.ann_mean_gross,
                     emit.info_clock_print ? &clock : nullptr};
    // Console provenance only: the cache changes no published byte, so no file records it.
    if (base.liquidity_cache) progress << "nav replay: shared execution liquidity cache on\n";
    if (!label.is_null())
      progress << "nav replay: label role " << execution.label_role.manifest_sha256
               << " marks the books (" << marks.label_only_cells
               << " label-only present cells)\n";
    if (base.book_workers > 1)
      progress << "nav replay: scenario books on " << base.book_workers << " workers\n";
    if (!warm.is_null())
      progress << "nav replay: warm start of " << base.warm_start_sessions
               << " sessions, first decision session_ns "
               << warm.at("first_decision_session_ns").get<i64>() << '\n';
    ATX_TRY_VOID(publish_nav(run, progress));
    return holdings ? publish_holdings(run, csv, progress) : co::Ok();
  } catch (const std::exception& e) {
    return co::Err(co::ErrorCode::InvalidArgument, std::string("nav replay: ") + e.what());
  }
}
co::Status run_nav_grid(const TargetReplayRunConfig& cfg, const NavTurnoverLimits& limits,
                        const NavFieldsPin& fields, const NavRateOptions& rate,
                        const NavExecutionOptions& execution, const NavEmitOptions& emit,
                        const std::string& grid_path, std::ostream& progress) {
  try {
    StageTimes times;
    StageTimes* const timed = emit.stage_timers ? &times : nullptr;
    ATX_TRY_VOID(check_run(cfg, limits, fields, execution.label_role));
    if (!emit.holdings_directory.empty())
      return co::Err(co::ErrorCode::InvalidArgument,
                     "nav grid: --emit-holdings is a single-run option");
    // P9 C1: every v7 rule but the spo engines (which hold every book's state) keeps its state
    // per book, so a grid runs it; each <id>/ publishes its own v7 files.
    if (v7::shared_plan_state())
      return co::Err(co::ErrorCode::InvalidArgument,
                     "nav grid: not with an spo rule (its engines hold every book's state)");
    const auto root = std::filesystem::path(cfg.output_directory);
    if (std::filesystem::exists(root))
      return co::Err(co::ErrorCode::AlreadyExists, "nav grid: output must not exist");
    ATX_TRY(const auto grid, read_grid(grid_path));
    const bool tiered = !fields.manifest_path.empty();
    const auto lockstep = v7::lockstep_scenarios(nav_scenario_matrix(tiered)); // L4 hook
    const auto& scenarios = lockstep.books;
    const usize main_books = lockstep.capacity_begin;
    const auto base = run_base(cfg.target, rate, execution);
    ATX_TRY(const auto targets, variant_targets(grid, base.target));
    std::vector<NavReplayConfig> configs(targets.size(), base);
    for (usize v = 0; v < targets.size(); ++v) configs[v].target = targets[v];
    // One admission and load for the grid, at the first variant's config (the variants
    // share every setting the reserve and the loader read); every book is charged.
    auto first = cfg;
    first.target = targets.front();
    const usize books = configs.size() * scenarios.size();
    ATX_TRY(auto admitted, admit_and_load(first, configs.front(), fields, books, false,
                                          execution.label_role, emit.info_clock_print));
    times.loaded = StageTimes::Clock::now();
    log_fields_seal(admitted.fields, progress); // P9 C3 (P13): a legacy manifest, logged
    const auto& blend = admitted.blend; const auto& loaded = admitted.fields;
    const auto& marks = admitted.label; // v8 E-25: empty without --label-role
    auto view = blend.view();
    view.industry = loaded.industry;
    const NavReplayInput input{view, blend.volume,
                               NavFinancingFields{loaded.shares_out, loaded.si_shares},
                               NavMarkPrices{marks.close, marks.raw, marks.present},
                               loaded.info_clock}; // P9 AL-CLOCK: empty unless rule or print
    const Json clock = info_clock_field(loaded);
    ATX_TRY(const auto results, replay_books(input, configs, scenarios, Observer{},
                                             timed ? &times.replay : nullptr));
    times.replayed = StageTimes::Clock::now();
    std::vector<std::vector<NavSummary>> summaries(configs.size());
    for (usize v = 0; v < configs.size(); ++v)
      for (const auto& result : results[v]) {
        ATX_TRY(auto summary, summarize_nav(result, limits));
        summaries[v].push_back(std::move(summary));
      }
    const Json warm = warm_start_record(base, view);
    const Json label = label_record(execution.label_role, marks);
    if (!std::filesystem::create_directory(root))
      return co::Err(co::ErrorCode::AlreadyExists, "nav grid: output must not exist");
    Json published = Json::array();
    for (usize v = 0; v < configs.size(); ++v) {
      const auto& variant = grid.variants[v];
      auto own = cfg;
      own.target = targets[v];
      own.output_directory = (root / variant.id).string();
      // The variant summaries carry no stage_seconds: they stay the standalone bytes. P9 C1:
      // the variant's capacity books and v7 records are its own books'.
      const std::span<const NavScenario> all_scenarios(scenarios);
      const std::span<const NavReplayResult> all_results(results[v]);
      const std::span<const NavSummary> all_summaries(summaries[v]);
      const v7::RunRecords records =
          v7::run_records(all_scenarios, all_results, all_summaries);
      const NavRun run{own,
                       limits,
                       configs[v],
                       all_scenarios.first(main_books),
                       all_results.first(main_books),
                       all_summaries.first(main_books),
                       blend.manifest_json,
                       loaded.binding,
                       warm,
                       timed,
                       false,
                       label,
                       all_scenarios.subspan(main_books),
                       all_results.subspan(main_books),
                       all_summaries.subspan(main_books),
                       v7::installed() ? &records : nullptr,
                       false,
                       nullptr,
                       emit.ann_mean_gross,
                       emit.info_clock_print ? &clock : nullptr};
      ATX_TRY_VOID(publish_nav(run, progress));
      const auto dir = root / variant.id;
      ATX_TRY(auto recipe_sha, timed_digest(timed, [&] {
        return co::sha256_file((dir / "recipe.json").string());
      }));
      ATX_TRY(auto summary_sha, timed_digest(timed, [&] {
        return co::sha256_file((dir / "summary.json").string());
      }));
      Json flags = Json::object();
      for (const auto& [key, value] : variant.overrides) flags[key] = value;
      published.push_back(Json{{"id", variant.id}, {"directory", variant.id},
                               {"flags", std::move(flags)},
                               {"recipe_file_sha256", std::move(recipe_sha)},
                               {"summary_file_sha256", std::move(summary_sha)}});
    }
    Json manifest{{"schema", "atx.nav-grid-run/v1"}, {"status", "complete"},
                  {"grid_sha256", grid.sha256}, {"combined_sha256", cfg.combined_sha256},
                  {"role_sha256", cfg.role_sha256}, {"books", books},
                  {"book_workers", base.book_workers}, {"variants", std::move(published)},
                  {"semantics", grid_declaration}};
    if (const auto groups = leverage_groups(configs); groups.size() > 1) { // review A-1
      Json ids = Json::array();
      for (const auto& group : groups) {
        Json members = Json::array();
        for (const usize v : group) members.push_back(grid.variants[v].id);
        ids.push_back(std::move(members));
      }
      const char* rule = two_speed_on(base.target) ? two_speed_groups_declaration
                                                   : leverage_groups_declaration;
      manifest["leverage_groups"] = Json{{"rule", rule}, {"groups", std::move(ids)}};
    }
    if (timed) manifest["stage_seconds"] = times.json();
    if (!label.is_null()) manifest["label_role_sha256"] = execution.label_role.manifest_sha256;
    progress << "nav grid: " << configs.size() << " variants x " << scenarios.size()
             << " scenario books published under " << root.string() << '\n';
    return write_json(root / "grid_manifest.json", manifest);
  } catch (const std::exception& e) {
    return co::Err(co::ErrorCode::InvalidArgument, std::string("nav grid: ") + e.what());
  }
}
} // namespace atx::impl::strategy
