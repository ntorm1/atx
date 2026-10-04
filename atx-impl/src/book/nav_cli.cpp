// NAV replay: the nav verb command line.
// P9 C3 (task 1, move-only): moved unchanged from strategy_nav_replay.cpp.

#include "book/nav_replay_internal.hpp"

namespace atx::impl::strategy {
using namespace nav_book;
int dispatch_nav_replay(int argc, char** argv, std::ostream& out, std::ostream& err) {
  // P9 C1 (K-P9-7): `nav --list-rules [--json]`, before any other flag is read; P9 C3: the one
  // table of every registry (book/nav_rules.hpp: leverage, trade-rate, cost-law, target).
  for (int i = 1; i < argc; ++i) {
    if (std::string_view(argv[i]) != "--list-rules") continue;
    bool json = false;
    for (int j = 1; j < argc; ++j) json = json || std::string_view(argv[j]) == "--json";
    const Json rules = nav_rules::table_json();
    if (json) {
      out << rules.dump(2) << '\n';
    } else {
      for (const auto& row : rules.at("rules"))
        out << row.at("kind").get<std::string>() << ' ' << row.at("id").get<std::string>()
            << '\n';
    }
    return 0;
  }
  // P9 C3: nav --spec NAVSPEC.json [--output NEWDIR] runs the spec's command line through this
  // dispatch; nav <flags> --print-spec prints the NavSpec of <flags> (book/nav_spec.hpp).
  if (const int code = nav_spec::dispatch(argc, argv, out, err); code != nav_spec::not_spec)
    return code;
  if (v7::claims_nav_args(argc, argv)) return v7::dispatch_nav_v7(argc, argv, out, err); // L4
  try {
    TargetReplayRunConfig cfg; NavTurnoverLimits limits; NavFieldsPin fields;
    NavRateOptions rate; NavExecutionOptions execution; NavEmitOptions emit;
    NavCalibrateOptions calibrate; // P9 C2 --calibrate-gross / --calibrate-tolerance
    std::string grid_path; // v8 --construction-grid
    std::set<std::string> seen;
    for (int i = 1; i < argc; ++i) {
      const std::string key = argv[i];
      // v6/v8 switches take no value; repeated, they are a usage error like any flag. P9 C3
      // (PRE-7a): --ann-mean-gross, print-only like --stage-timers.
      if (key == "--locate-in-aim" || key == "--liquidity-cache" || key == "--stage-timers" ||
          key == "--ann-mean-gross") {
        if (!seen.insert(key).second) throw std::invalid_argument("duplicate/missing flag");
        bool& on = key == "--stage-timers"     ? emit.stage_timers
                   : key == "--ann-mean-gross" ? emit.ann_mean_gross
                   : key == "--locate-in-aim"  ? execution.locate_in_aim
                                               : execution.liquidity_cache;
        on = true;
        continue;
      }
      if (key == "--help") {
        out << "nav --combined PATH --combined-sha256 SHA --role PATH --role-sha256 SHA "
               "--output NEWDIR [--rule baseline-v1|monthly-budget-v2|aim-partial-v5] "
               "[--cadence 5] [--trade-fraction .25 (aim-partial-v5: theta)] "
               "[--monthly-budget .30] "
               "[--neutralize none|price-risk-v1|price-risk-ind-v1|price-risk-ind-v2 "
               "(ind: FF12 demeaning, needs --fields; ind-v2: vol 126 / log-ADV 252 windows)] "
               "[--band-multiple 0] "
               "[--dust-multiple 0 --aim-leverage 1 (aim-partial-v5 only)] "
               "[--rate fixed|per-name-v1 (aim-partial-v5 only) --rate-rra 10 "
               "--rate-lambda .2 --rate-min .01 --rate-max .15 (per-name-v1 only)] "
               "[--daily-turnover-mean-max .20] [--daily-turnover-p95-max .30] "
               "[--fields FIELDS/manifest.json --fields-sha256 SHA] [--max-bytes 536870912] "
               "[--order-basis target|delta] [--exit-rate 1 (below 1: aim-partial-v5, "
               "dust > 0)] [--locate-in-aim (needs --fields and --neutralize)] "
               "[--liquidity-cache] [--emit-holdings NEWDIR (primary book per-name "
               "holdings, streamed; manifest.json last) [--holdings-format f64|csv (f64: "
               "holdings.f64 + holdings_index.json, the default; csv: the v1 holdings.csv; "
               "ignored without --emit-holdings)]] "
               "[--warm-start-sessions 0 (K > 0: decide and trade from score_begin - K, "
               "score from score_begin; K <= the role's score_begin)] "
               "[--hold-band B (aim-partial-v5; v8 hold-band-v1 rank hysteresis, B in [0, 1])] "
               "[--adv-hold-q Q (aim-partial-v5; v8 adv-hold-v1 holding cap Q x ADV, 0 = off; "
               "the cap uses each book's NAV: the run's initial NAV, m x that NAV for the "
               "--capacity-curve book at multiple m)] "
               "[--vol-scale inv-vol-v1 (aim-partial-v5, not with --hold-band; v8 X: each "
               "member's tied rank x median sigma / max(sigma, .25 median sigma) before the "
               "demean, sigma the execution cost model's for the decision's fills)] "
               "[--rank-shape norm-score-v1 (aim-partial-v5, not with --hold-band or "
               "--vol-scale; v8 Y: each member's tied rank replaced by its van der Waerden "
               "normal score before the demean)] "
               "[--two-speed two-speed-v1 (aim-partial-v5 at the fixed rate and --trade-fraction "
               ".05, not with --hold-band, --vol-scale or --adv-hold-q; v8 Y-5: the saved fast "
               "and slow sleeves, a virtual fast sleeve at theta_f = 1 - 2^(-C/5) at cadence C, "
               "the slow remainder at theta_s = .05, netted)] "
               "[--book-workers 1 (1..64: every book's phases on a deterministic pool, "
               "bit-identical; fixed rate only, not with an spo rule)] [--list-rules [--json] "
               "(the rule contract atx.nav-rules/v1, then exit)] [--stage-timers (summary.json "
               "stage_seconds: load, exposures, construction, books, hash, write)] "
               "[--construction-grid GRID.json (atx.nav-construction-grid/v1 variants over "
               "--rule --cadence --trade-fraction --monthly-budget --band-multiple "
               "--dust-multiple --aim-leverage --exit-rate: one lockstep run; "
               "<output>/<id>/ = the standalone run, grid_manifest.json last)] "
               "[--label-role ROLE/manifest.json --label-role-sha256 SHA (v8 E-25: the books "
               "are MARKED - P&L, NAV, stale carry, write-off - by this role's close, raw_close "
               "and present, e.g. a --delisting-returns build; signals, fields, membership, "
               "construction, tiers, liquidity and execution stay on --role, and no order fills "
               "on a cell only the label role prices. Refused unless it shares --role's dates, "
               "instruments, membership and base: every manifest key but files and universe "
               "equal, the same files and extents with equal SHA-256 except close, raw_close, "
               "volume and present, the same universe id and base-role, identity-bridge, "
               "SIC-events and delisting-stage pins; member.u8 (and score_member_counts) may "
               "differ only by the members it declares cleared on termination sessions "
               "(universe.delisting.applied.members_cleared_on_termination_session N), exactly "
               "N cells --role has absent and keeps a member; it must not reach the research "
               "seal and may only add presence to --role's, at its prices; recipe and summary "
               "record label_role)] "
               "[--calibrate-gross G_P [--calibrate-tolerance .005] (P9 C2, Ruling PM6-6 in "
               "one process: pass 1 = the S2 book alone at --aim-leverage L; G = its all-rows "
               "mean gross; |G - G_P| > tolerance: ONE correction L' = L x G_P / G to 4 "
               "decimals, refused outside [1, 2]; the published run is the plain run at the "
               "final L plus summary.json calibration {..., matched}; aim-partial-v5, not with "
               "--construction-grid or an spo rule)] "
               "[--spec NAVSPEC.json [--output NEWDIR] (P9 C3: the run of one atx.nav-spec/v1 "
               "file, typed options and constants; it holds every other flag) | --print-spec "
               "(the NavSpec of the given flags, then exit)] "
               "[--ann-mean-gross (P9 C3, print-only: every scenario of summary.json gains "
               "ann_mean_gross = 252 x the mean daily gross_return over ann_mean's return rows, "
               "before trade costs and financing; nothing else changes)]\n"
               "Runs every fixed scenario (S1 linear-6bps-stale5-v1, S2 modeled-1bn-stale5-v1 "
               "PRIMARY, S3 modeled-1bn-terminal-adverse-v1); costs/borrow are not flags.\n"
               "Without --fields: flat-300-v0 financing only. With the pinned role fields "
               "(atx.research-role-fields/v1: shares_out, si_shares): S1/S2/S3 x swap-fin-v1 "
               "(PRIMARY S2), S2 x flat-300-v0, S2 x engine-tiers-v1.\n";
        v7::append_help(out); // L4 hook
        return 0;
      }
      if (key == "--one-way-bps" || key == "--annual-borrow-bps")
        throw std::invalid_argument(key + " is not accepted in nav mode (fixed cost scenarios)");
      if (!seen.insert(key).second || i + 1 >= argc)
        throw std::invalid_argument("duplicate/missing flag");
      const std::string value = argv[++i];
      const auto real = [&]() { return parse_real(value); };
      const auto integer = [&]() { return parse_integer(value); };
      // --rule, --cadence and the construction parameters (the grid variants' flags).
      if (apply_construction_flag(key, value, cfg.target)) continue;
      if (key == "--combined") cfg.combined_path = value;
      else if (key == "--combined-sha256") cfg.combined_sha256 = value;
      else if (key == "--output") cfg.output_directory = value;
      else if (key == "--role") cfg.role_path = value;
      else if (key == "--role-sha256") cfg.role_sha256 = value;
      else if (key == "--fields") fields.manifest_path = value;
      else if (key == "--fields-sha256") fields.manifest_sha256 = value;
      else if (key == "--emit-holdings") emit.holdings_directory = value;
      else if (key == "--construction-grid") grid_path = value;
      else if (key == "--label-role") execution.label_role.manifest_path = value; // v8 E-25
      else if (key == "--label-role-sha256") execution.label_role.manifest_sha256 = value;
      else if (key == "--holdings-format") {
        // Accepted (and ignored) without --emit-holdings, as before P9 C1 (when the v7
        // capacity pass dropped --emit-holdings and forwarded every other flag).
        if (value == "f64") emit.format = NavHoldingsFormat::F64;
        else if (value == "csv") emit.format = NavHoldingsFormat::Csv;
        else throw std::invalid_argument("unknown --holdings-format (f64|csv)");
      } else if (key == "--max-bytes") cfg.target.max_working_bytes = integer();
      else if (key == "--book-workers") {
        const auto x = integer();
        if (x < 1 || x > max_book_workers) throw std::invalid_argument("--book-workers 1..64");
        execution.book_workers = static_cast<usize>(x);
      } else if (key == "--hold-band") cfg.target.hold_band = real(); // v8 R-4 hold-band-v1
      else if (key == "--adv-hold-q") cfg.target.adv_hold_q = real(); // v8 R-5 adv-hold-v1
      else if (key == "--vol-scale") { // v8 X (lane XCOMB) inv-vol-v1
        if (value != "inv-vol-v1") throw std::invalid_argument("unknown --vol-scale (inv-vol-v1)");
        cfg.target.inv_vol = true;
      } else if (key == "--rank-shape") { // v8 Y (lane YCOMB) norm-score-v1
        if (value != "norm-score-v1")
          throw std::invalid_argument("unknown --rank-shape (norm-score-v1)");
        cfg.target.norm_score = true;
      } else if (key == "--two-speed") { // v8 Y-5 (lane YCOMB) two-speed-v1
        if (value != "two-speed-v1")
          throw std::invalid_argument("unknown --two-speed (two-speed-v1)");
        cfg.target.two_speed = true;
      } else if (key == "--warm-start-sessions") {
        const auto x = integer();
        if (x > max_dates) throw std::invalid_argument("warm start exceeds bound");
        execution.warm_start_sessions = static_cast<usize>(x);
      } else if (key == "--order-basis") {
        if (value == "target") execution.order_basis = NavOrderBasis::Target;
        else if (value == "delta") execution.order_basis = NavOrderBasis::Delta;
        else throw std::invalid_argument("unknown --order-basis (target|delta)");
      } else if (key == "--rate-rra") rate.rate_rra = real();
      else if (key == "--rate-lambda") rate.rate_lambda = real();
      else if (key == "--rate-min") rate.rate_min = real();
      else if (key == "--rate-max") rate.rate_max = real();
      else if (key == "--daily-turnover-mean-max") limits.daily_mean_max = real();
      else if (key == "--daily-turnover-p95-max") limits.daily_p95_max = real();
      else if (key == "--calibrate-gross") calibrate.target_gross = real(); // P9 C2
      else if (key == "--calibrate-tolerance") calibrate.tolerance = real();
      else if (key == "--neutralize") {
        if (!detail::parse_neutralize(value, cfg.target))
          throw std::invalid_argument(
              "unknown --neutralize (none|price-risk-v1|price-risk-ind-v1|price-risk-ind-v2)");
      } else if (key == "--rate") {
        if (value == "fixed") rate.rate = NavRateRule::Fixed;
        else if (value == "per-name-v1") rate.rate = NavRateRule::PerNameV1;
        else throw std::invalid_argument("unknown --rate (fixed|per-name-v1)");
      } else throw std::invalid_argument("unknown flag: " + key);
    }
    if (cfg.role_path.empty() || cfg.role_sha256.empty())
      throw std::invalid_argument("--role and --role-sha256 are required in nav mode");
    if (fields.manifest_path.empty() != fields.manifest_sha256.empty())
      throw std::invalid_argument("--fields and --fields-sha256 go together");
    if (execution.label_role.manifest_path.empty() !=
        execution.label_role.manifest_sha256.empty())
      throw std::invalid_argument("--label-role and --label-role-sha256 go together");
    // P9 C2: the tolerance belongs to --calibrate-gross, whose value is a finite gross (a parsed
    // "nan" would otherwise read as the flag being absent); one run, never a grid.
    if (seen.count("--calibrate-tolerance") && !seen.count("--calibrate-gross"))
      throw std::invalid_argument("--calibrate-tolerance needs --calibrate-gross");
    if (seen.count("--calibrate-gross") && !std::isfinite(calibrate.target_gross))
      throw std::invalid_argument("--calibrate-gross must be finite and > 0");
    if (calibrate.on() && !grid_path.empty())
      throw std::invalid_argument("--calibrate-gross does not take --construction-grid");
    // The rate belongs to aim-partial-v5, its parameters to --rate per-name-v1. A grid
    // checks every variant's own rule (its --rule overrides the base's), as its
    // standalone command line would be checked.
    std::vector<TargetReplayRule> rules{cfg.target.rule};
    if (!grid_path.empty()) {
      if (!emit.holdings_directory.empty())
        throw std::invalid_argument("--construction-grid does not take --emit-holdings");
      auto grid = read_grid(grid_path);
      if (!grid) { err << grid.error().to_string() << '\n'; return 1; }
      rules.clear();
      for (const auto& variant : grid->variants) {
        auto target = cfg.target;
        for (const auto& [flag, setting] : variant.overrides)
          if (!apply_construction_flag(flag, setting, target))
            throw std::invalid_argument("grid flag " + flag);
        rules.push_back(target.rule);
      }
    }
    for (const auto rule : rules)
      if (seen.count("--rate") && rule != TargetReplayRule::AimPartialV5)
        throw std::invalid_argument("--rate is aim-partial-v5 only");
    for (const char* flag : {"--rate-rra", "--rate-lambda", "--rate-min", "--rate-max"})
      if (seen.count(flag) && rate.rate != NavRateRule::PerNameV1)
        throw std::invalid_argument(std::string(flag) + " needs --rate per-name-v1");
    const auto status = grid_path.empty()
        ? run_nav_replay(cfg, limits, fields, rate, execution, emit, calibrate, out)
        : run_nav_grid(cfg, limits, fields, rate, execution, emit, grid_path, out);
    if (!status) { err << status.error().to_string() << '\n'; return 1; }
    return 0;
  } catch (const std::exception& e) { err << "nav replay: " << e.what() << '\n'; return 2; }
}
} // namespace atx::impl::strategy
