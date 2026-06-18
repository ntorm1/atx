#include "dispatch.hpp"

#include <ostream>
#include <string>
#include <string_view>
#include <vector>

#include "artifacts.hpp"
#include "config.hpp"
#include "stages.hpp"

namespace atx::impl {

// ---------------------------------------------------------------------------
// emit_digest_line (initializer_list overload — used by tests)
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
// emit_digest_line (vector overload — used by dispatch with StageResult::kvs)
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

    // 3. "run" + config file => merge file into the CLI-parsed cfg. A flag
    //    explicitly supplied on the CLI always wins (presence-tracked via
    //    cfg.set_flags); the file only fills gaps the CLI left unset.
    if (cfg.subcommand == "run" && !cfg.config_file.empty()) {
        auto merge_result = merge_config_file(cfg, cfg.config_file);
        if (!merge_result) {
            err << merge_result.error().message() << '\n';
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
        emit_digest_line(out, sub, stage_result->digest, stage_result->kvs);
    }
    return 0;
}

} // namespace atx::impl
