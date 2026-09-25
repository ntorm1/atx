#include "dispatch.hpp"

#include <ostream>
#include <string>
#include <string_view>
#include <vector>

#include "artifacts.hpp"
#include "config.hpp"
#include "stages.hpp"
#include "stage_equity_baseline.hpp"
#include "stage_equity_book.hpp"
#include "stage_equity_ic.hpp"
#include "stage_equity_mine.hpp"
#include "stage_equity_universe.hpp"

namespace atx::impl {

// ---------------------------------------------------------------------------
// emit_digest_line (initializer_list overload â€” used by tests)
// Exactly: "[atx-impl] stage=<stage> digest=<hex16> k=v k=v\n"
// ---------------------------------------------------------------------------
void emit_digest_line(std::ostream& out,
                      std::string_view stage,
                      atx::u64 digest,
                      std::initializer_list<std::pair<std::string_view, std::string>> kvs) {
    out << "[atx-impl] stage=" << stage
        << " digest=" << to_hex16(digest);
    for (const auto& [k, v] : kvs) {
        out << ' ' << k << '=' << v;
    }
    out << '\n';
}

// ---------------------------------------------------------------------------
// emit_digest_line (vector overload â€” used by dispatch with StageResult::kvs)
// ---------------------------------------------------------------------------
void emit_digest_line(std::ostream& out,
                      std::string_view stage,
                      atx::u64 digest,
                      const std::vector<std::pair<std::string, std::string>>& kvs) {
    out << "[atx-impl] stage=" << stage
        << " digest=" << to_hex16(digest);
    for (const auto& [k, v] : kvs) {
        out << ' ' << k << '=' << v;
    }
    out << '\n';
}

// ---------------------------------------------------------------------------
// print_usage
// ---------------------------------------------------------------------------
static void print_usage(std::ostream& out) {
    out << "Usage: atx-impl <subcommand> [--flag value ...]\n"
           "\n"
           "Subcommands:\n"
           "  load       Convert daily ticker-history zip -> .seg files\n"
           "  panel      Build a filtered universe panel\n"
           "  discover   Search for alpha expressions\n"
           "  combine    Combine alpha expressions into a blend\n"
           "  optimize   Optimize portfolio book weights\n"
           "  report     Generate performance report\n"
           "  equity-baseline  Fixed slow-momentum training book and replay\n"
           "  equity-book  Constrained allocation of fixed baseline preferences against marked holdings\n"
           "  equity-ic  Pre-registered cross-sectional forecast evaluation of the fixed baseline\n"
           "  equity-universe  Point-in-time dollar-volume universe over sealed archive segments\n"
           "  run        Run the full pipeline from a config file\n"
           "  regime     Build a regime/macro .seg from staged CSVs\n"
           "  sweep      Sweep K seeds into one --library-dir accumulating library\n"
           "  metabook   Sleeve-aware meta-book (S2 fund::MetaBook), standalone\n"
           "\n"
           "Global flags: --help, --quiet, --digest-only, --config <file> (every stage\n"
           "  except equity-ic and equity-universe, which reject it).\n"
           "Boolean flags take an optional true|false|1|0 (--metabook false; key=false in a file).\n"
           "Double-valued flags must be finite (nan/inf are rejected).\n"
           "Panel inputs require matching .manifest.json identity files.\n"
           "Legacy diagnostics: --allow-unidentified-panels true (unknown identity).\n"
           "Load provenance: --preparation-manifest <completed preparation manifest>.\n"
           "Identified reports replay holdings/cash with --report-aum as initial NAV.\n"
           "Replay: --replay-execution-delay <observations, default 1;\n"
           "        0 needs --allow-same-close>,\n"
           "        --replay-trade-bps <per traded dollar, REQUIRED (0 must be explicit)>,\n"
           "        --replay-annual-borrow-bps <annual bps, REQUIRED (0 must be explicit)>,\n"
           "        --replay-day-basis <360|365, default 365>.\n"
           "Replay timing is hypothetical; historical availability remains unverified.\n";
    out << "Equity baseline: --panel <identified context> --out <fresh directory>,\n"
           "  --evaluation-start <YYYY-MM-DD> --evaluation-end <exclusive YYYY-MM-DD>,\n"
           "  --max-working-bytes <bytes, default 3000000000>.\n"
           "Requires 256 earlier observations; uses two fixed signals and weekly equal blend.\n"
           "Baseline defaults: initial NAV 100m, trade fee 5 bps, annual borrow 365 bps.\n";
    out << "Equity book: --panel <identified source context> --baseline-dir <baseline directory>\n"
           "  --out <fresh directory> [--max-working-bytes <bytes>].\n"
           "Uses the same frozen preferences and inherits the original full evaluation window.\n"
           "Financial assumptions inherit the baseline unless explicitly overridden with replay/report flags.\n";
    out << "Equity IC: --panel <identified context> --baseline-dir <baseline directory>\n"
           "  --out <fresh directory> --evaluation-start <YYYY-MM-DD>\n"
           "  --evaluation-end <exclusive YYYY-MM-DD> [--max-working-bytes <bytes>]\n"
           "  [--trial-ledger <path, default atx-engine/reviews/trial-ledger.jsonl>].\n"
           "Both evaluation dates are required; --config is not accepted for this subcommand.\n"
           "Pre-registered: 3 signals x 5 horizons x 2 forward-return variants, decile spreads\n"
           "and block-bootstrap intervals, appended to the trial ledger before and after the run.\n"
           "Information coefficients are model-skill statistics, never accepted\n"
           "alpha or a Sharpe.\n";
    out << "Equity universe: --segments-dirs <dir;dir;...> --preparation-manifests <m;m;...>\n"
           "  --out <fresh directory> --rank-start <YYYY-MM-DD> --rank-end <YYYY-MM-DD>\n"
           "  [--max-working-bytes <bytes>] [--trial-ledger <path>].\n"
           "Frozen recipe: ADV63 median dollar volume, top_n {1000,2000,3000} x band {0.00,0.10},\n"
           "monthly rank sessions effective the next session; refuses any segment >= 2020-01-01;\n"
           "--config is not accepted. Membership lists only: no forecast and no alpha claim.\n";
}

// ---------------------------------------------------------------------------
// dispatch
// ---------------------------------------------------------------------------
int dispatch(int argc, char** argv, std::ostream& out, std::ostream& err) {
    if (argc > 1 && std::string_view{argv[1]} == "equity-mine") // lane 9: owns its flags
        return dispatch_equity_mine(argc, argv, out, err);
    // 1. Parse args.
    auto cfg_result = parse_args(argc, argv);
    if (!cfg_result) {
        err << cfg_result.error().message() << '\n';
        return 2;
    }
    RunConfig cfg = std::move(*cfg_result);

    // 2. Help / no subcommand => usage.
    if (cfg.help || cfg.subcommand.empty()) {
        print_usage(out);
        return 0;
    }

    // 3. W0-I0b / I-10: --config works in EVERY stage or is rejected. Before W0 only
    //    run / equity-baseline / equity-book merged the file and every other stage
    //    silently ignored it. Now every stage merges it, except the two frozen-recipe
    //    stages (subcommand_rejects_config), which refuse it here and in the stage.
    //    A flag explicitly supplied on the CLI always wins (presence-tracked via
    //    cfg.set_flags); the file only fills gaps the CLI left unset, and the merged
    //    result passes the same cross-flag rules as a pure CLI invocation.
    if (!cfg.config_file.empty()) {
        if (subcommand_rejects_config(cfg.subcommand)) {
            err << cfg.subcommand << ": --config is not accepted for this subcommand\n";
            return 2;
        }
        auto merge_result = merge_config_file(cfg, cfg.config_file);
        if (!merge_result) {
            err << merge_result.error().message() << '\n';
            return 2;
        }
        auto validated = validate_cross_flags(cfg);
        if (!validated) {
            err << validated.error().message() << '\n';
            return 2;
        }
    }

    // 4. Route to stage function.
    const std::string& sub = cfg.subcommand;

    atx::core::Result<StageResult> stage_result = [&]() -> atx::core::Result<StageResult> {
        if (sub == "load")     return run_load(cfg);
        if (sub == "panel")    return run_panel(cfg);
        if (sub == "discover") return run_discover(cfg);
        if (sub == "combine")  return run_combine(cfg);
        if (sub == "optimize") return run_optimize(cfg);
        if (sub == "report")   return run_report(cfg);
        if (sub == "equity-baseline") return run_equity_baseline(cfg);
        if (sub == "equity-book") return run_equity_book(cfg);
        if (sub == "equity-ic") return run_equity_ic(cfg);
        if (sub == "equity-universe") return run_equity_universe(cfg);
        if (sub == "run")      return run_all(cfg);
        if (sub == "regime")   return run_regime(cfg);
        if (sub == "sweep")    return run_sweep(cfg);
        if (sub == "metabook") return run_metabook(cfg);
        // Unreachable: parse_args already validated the subcommand.
        return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
            "unknown subcommand: '" + sub + "'");
    }();

    if (!stage_result) {
        err << stage_result.error().message() << '\n';
        return 1;
    }

    if (!cfg.quiet) {
        emit_digest_line(out, sub, stage_result->digest, stage_result->kvs);
    }
    return 0;
}

} // namespace atx::impl
