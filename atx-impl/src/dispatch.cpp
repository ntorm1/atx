#include "dispatch.hpp"

#include <ostream>
#include <string>
#include <string_view>

#include "artifacts.hpp"
#include "config.hpp"
#include "stages.hpp"

namespace atx::impl {

// ---------------------------------------------------------------------------
// emit_digest_line
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
// print_usage
// ---------------------------------------------------------------------------
static void print_usage(std::ostream& out) {
    out << "Usage: atx-impl <subcommand> [--flag value ...]\n"
           "\n"
           "Subcommands:\n"
           "  load       Convert raw ORATS zip -> .seg files\n"
           "  panel      Build a filtered universe panel\n"
           "  discover   Search for alpha expressions\n"
           "  combine    Combine alpha expressions into a blend\n"
           "  optimize   Optimize portfolio book weights\n"
           "  report     Generate performance report\n"
           "  run        Run the full pipeline from a config file\n"
           "\n"
           "Global flags: --help, --quiet, --digest-only\n";
}

// ---------------------------------------------------------------------------
// dispatch
// ---------------------------------------------------------------------------
int dispatch(int argc, char** argv, std::ostream& out, std::ostream& err) {
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

    // 3. "run" + config file => merge from file.
    if (cfg.subcommand == "run" && !cfg.config_file.empty()) {
        auto file_result = parse_config_file(cfg.config_file, cfg.subcommand);
        if (!file_result) {
            err << file_result.error().message() << '\n';
            return 2;
        }
        // Merge: CLI flags already in cfg take precedence over file values.
        // Simple policy: file supplies defaults, CLI overrides.
        // We overwrite only fields that are still at their zero/empty value.
        RunConfig& fc = *file_result;
        if (cfg.zip.empty())          cfg.zip          = fc.zip;
        if (cfg.out.empty())          cfg.out           = fc.out;
        if (cfg.min_date.empty())     cfg.min_date      = fc.min_date;
        if (cfg.segs.empty())         cfg.segs          = fc.segs;
        if (cfg.panel_out.empty())    cfg.panel_out     = fc.panel_out;
        if (cfg.start.empty())        cfg.start         = fc.start;
        if (cfg.end.empty())          cfg.end           = fc.end;
        if (cfg.min_adv_usd == 0.0)  cfg.min_adv_usd   = fc.min_adv_usd;
        if (cfg.top_n_by_adv == 0)   cfg.top_n_by_adv  = fc.top_n_by_adv;
        if (cfg.panel.empty())        cfg.panel         = fc.panel;
        if (cfg.alpha_out.empty())    cfg.alpha_out     = fc.alpha_out;
        if (cfg.seed == 0ULL)         cfg.seed          = fc.seed;
        if (cfg.population == 0)      cfg.population    = fc.population;
        if (cfg.generations == 0)     cfg.generations   = fc.generations;
        if (cfg.seed_exprs.empty())   cfg.seed_exprs    = fc.seed_exprs;
        if (cfg.min_dsr == 0.0)       cfg.min_dsr       = fc.min_dsr;
        if (cfg.alphas.empty())       cfg.alphas        = fc.alphas;
        if (cfg.combo_out.empty())    cfg.combo_out     = fc.combo_out;
        if (cfg.method.empty())       cfg.method        = fc.method;
        if (cfg.fit_begin == 0)       cfg.fit_begin     = fc.fit_begin;
        if (cfg.fit_end == 0)         cfg.fit_end       = fc.fit_end;
        if (cfg.combo.empty())        cfg.combo         = fc.combo;
        if (cfg.books_out.empty())    cfg.books_out     = fc.books_out;
        if (cfg.risk_aversion == 0.0) cfg.risk_aversion = fc.risk_aversion;
        if (cfg.turnover_penalty == 0.0) cfg.turnover_penalty = fc.turnover_penalty;
        if (cfg.gross == 0.0)         cfg.gross         = fc.gross;
        if (cfg.name_cap == 0.0)      cfg.name_cap      = fc.name_cap;
        if (cfg.rebalance.empty())    cfg.rebalance     = fc.rebalance;
        if (cfg.books.empty())        cfg.books         = fc.books;
        if (cfg.report_out.empty())   cfg.report_out    = fc.report_out;
    }

    // 4. Route to stage function.
    const std::string& sub = cfg.subcommand;

    atx::core::Result<atx::u64> stage_result = [&]() -> atx::core::Result<atx::u64> {
        if (sub == "load")     return run_load(cfg);
        if (sub == "panel")    return run_panel(cfg);
        if (sub == "discover") return run_discover(cfg);
        if (sub == "combine")  return run_combine(cfg);
        if (sub == "optimize") return run_optimize(cfg);
        if (sub == "report")   return run_report(cfg);
        if (sub == "run")      return run_all(cfg);
        // Unreachable: parse_args already validated the subcommand.
        return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
            "unknown subcommand: '" + sub + "'");
    }();

    if (!stage_result) {
        err << stage_result.error().message() << '\n';
        return 1;
    }

    if (!cfg.quiet) {
        emit_digest_line(out, sub, *stage_result);
    }
    return 0;
}

} // namespace atx::impl
