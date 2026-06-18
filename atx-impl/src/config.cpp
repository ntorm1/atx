#include "config.hpp"

#include <charconv>
#include <fstream>
#include <string>
#include <string_view>

#include "atx/core/error.hpp"

namespace atx::impl {

// ---------------------------------------------------------------------------
// Worker: apply one recognized (flag, value) pair to a RunConfig.
// 'flag' must NOT have a leading "--". Returns Err(InvalidArgument) for unknown
// flags. Does NOT touch cfg.set_flags — that bookkeeping lives in apply_flag.
// ---------------------------------------------------------------------------
static atx::core::Result<void> apply_flag_value(RunConfig& cfg,
                                                std::string_view flag,
                                                std::string_view value) {
    using EC = atx::core::ErrorCode;

    // Boolean flags (value is ignored / empty for valueless booleans).
    if (flag == "help")         { cfg.help        = true; return atx::core::Ok(); }
    if (flag == "quiet")        { cfg.quiet       = true; return atx::core::Ok(); }
    if (flag == "digest-only")  { cfg.digest_only = true; return atx::core::Ok(); }

    // String flags
    if (flag == "zip")          { cfg.zip          = value; return atx::core::Ok(); }
    if (flag == "out")          { cfg.out           = value; return atx::core::Ok(); }
    if (flag == "min-date")     { cfg.min_date      = value; return atx::core::Ok(); }
    if (flag == "segs")         { cfg.segs          = value; return atx::core::Ok(); }
    if (flag == "panel-out")    { cfg.panel_out     = value; return atx::core::Ok(); }
    if (flag == "start")        { cfg.start         = value; return atx::core::Ok(); }
    if (flag == "end")          { cfg.end           = value; return atx::core::Ok(); }
    if (flag == "panel")        { cfg.panel         = value; return atx::core::Ok(); }
    if (flag == "alpha-out")    { cfg.alpha_out     = value; return atx::core::Ok(); }
    if (flag == "alphas")       { cfg.alphas        = value; return atx::core::Ok(); }
    if (flag == "combo-out")    { cfg.combo_out     = value; return atx::core::Ok(); }
    if (flag == "method")       { cfg.method        = value; return atx::core::Ok(); }
    if (flag == "combo")        { cfg.combo         = value; return atx::core::Ok(); }
    if (flag == "books-out")    { cfg.books_out     = value; return atx::core::Ok(); }
    if (flag == "rebalance")    { cfg.rebalance     = value; return atx::core::Ok(); }
    if (flag == "books")        { cfg.books         = value; return atx::core::Ok(); }
    if (flag == "report-out")   { cfg.report_out    = value; return atx::core::Ok(); }
    if (flag == "config")       { cfg.config_file   = value; return atx::core::Ok(); }

    // Repeatable string flag
    if (flag == "seed-expr")    { cfg.seed_exprs.emplace_back(value); return atx::core::Ok(); }

    // Numeric flags
    auto parse_double = [&](double& dest) -> atx::core::Result<void> {
        // std::from_chars for double requires C++17; available on MSVC 19.24+.
        double tmp = 0.0;
        auto [ptr, ec] = std::from_chars(value.data(),
                                         value.data() + value.size(), tmp);
        if (ec != std::errc{} || ptr != value.data() + value.size()) {
            return atx::core::Err(EC::InvalidArgument,
                std::string("invalid double value for --") + std::string(flag)
                + ": '" + std::string(value) + "'");
        }
        dest = tmp;
        return atx::core::Ok();
    };

    auto parse_long = [&](long& dest) -> atx::core::Result<void> {
        long tmp = 0;
        auto [ptr, ec] = std::from_chars(value.data(),
                                         value.data() + value.size(), tmp);
        if (ec != std::errc{} || ptr != value.data() + value.size()) {
            return atx::core::Err(EC::InvalidArgument,
                std::string("invalid integer value for --") + std::string(flag)
                + ": '" + std::string(value) + "'");
        }
        dest = tmp;
        return atx::core::Ok();
    };

    auto parse_ull = [&](unsigned long long& dest) -> atx::core::Result<void> {
        unsigned long long tmp = 0ULL;
        auto [ptr, ec] = std::from_chars(value.data(),
                                         value.data() + value.size(), tmp);
        if (ec != std::errc{} || ptr != value.data() + value.size()) {
            return atx::core::Err(EC::InvalidArgument,
                std::string("invalid unsigned integer value for --")
                + std::string(flag) + ": '" + std::string(value) + "'");
        }
        dest = tmp;
        return atx::core::Ok();
    };

    if (flag == "min-adv-usd")       return parse_double(cfg.min_adv_usd);
    if (flag == "top-n-by-adv")      return parse_long(cfg.top_n_by_adv);
    if (flag == "seed")              return parse_ull(cfg.seed);
    if (flag == "population")        return parse_long(cfg.population);
    if (flag == "generations")       return parse_long(cfg.generations);
    if (flag == "min-dsr")           return parse_double(cfg.min_dsr);
    if (flag == "fit-begin")         return parse_long(cfg.fit_begin);
    if (flag == "fit-end")           return parse_long(cfg.fit_end);
    if (flag == "risk-aversion")     return parse_double(cfg.risk_aversion);
    if (flag == "turnover-penalty")  return parse_double(cfg.turnover_penalty);
    if (flag == "gross")             return parse_double(cfg.gross);
    if (flag == "name-cap")          return parse_double(cfg.name_cap);

    return atx::core::Err(EC::InvalidArgument,
        std::string("unknown flag: --") + std::string(flag));
}

// ---------------------------------------------------------------------------
// Shared helper: apply one (flag, value) pair, recording the canonical flag
// name into cfg.set_flags on success. Used uniformly by CLI and config-file
// parsing so the "was this flag explicitly supplied?" signal is symmetric.
// ---------------------------------------------------------------------------
static atx::core::Result<void> apply_flag(RunConfig& cfg,
                                          std::string_view flag,
                                          std::string_view value) {
    auto r = apply_flag_value(cfg, flag, value);
    if (!r) return r;
    cfg.set_flags.emplace(flag);
    return atx::core::Ok();
}

// ---------------------------------------------------------------------------
// parse_args
// ---------------------------------------------------------------------------
atx::core::Result<RunConfig> parse_args(int argc, char** argv) {
    using EC = atx::core::ErrorCode;
    RunConfig cfg{};

    int i = 1;

    // First positional arg: subcommand or --help/-h.
    if (i < argc) {
        std::string_view a{argv[i]};
        if (a == "--help" || a == "-h") {
            cfg.help = true;
            return atx::core::Ok(cfg);
        }
        // Subcommand must not start with '-'.
        if (!a.empty() && a[0] != '-') {
            bool found = false;
            for (auto sc : kSubcommands) {
                if (a == sc) { found = true; break; }
            }
            if (!found) {
                return atx::core::Err(EC::InvalidArgument,
                    std::string("unknown subcommand: '") + std::string(a) + "'");
            }
            cfg.subcommand = std::string(a);
            ++i;
        }
    }

    // Remaining args: --flag [value] pairs.
    while (i < argc) {
        std::string_view tok{argv[i]};
        if (tok.size() < 2 || tok[0] != '-' || tok[1] != '-') {
            return atx::core::Err(EC::InvalidArgument,
                std::string("unexpected argument: '") + std::string(tok) + "'");
        }
        std::string_view flag = tok.substr(2); // strip leading "--"

        // Valueless boolean flags.
        if (flag == "help" || flag == "quiet" || flag == "digest-only") {
            auto r = apply_flag(cfg, flag, "");
            if (!r) return atx::core::Err(std::move(r).error());
            ++i;
            continue;
        }

        // Flags that require a value.
        ++i;
        if (i >= argc) {
            return atx::core::Err(EC::InvalidArgument,
                std::string("flag --") + std::string(flag) + " requires a value");
        }
        std::string_view val{argv[i]};
        auto r = apply_flag(cfg, flag, val);
        if (!r) return atx::core::Err(std::move(r).error());
        ++i;
    }

    return atx::core::Ok(cfg);
}

// ---------------------------------------------------------------------------
// Core config-file reader: apply each `flag=value` line to `cfg`. Lines whose
// flag name is already in `skip` are ignored (used by the run-mode merge so a
// flag explicitly supplied on the CLI is never overridden by the file). When
// `skip` is empty, every recognized flag is applied.
// ---------------------------------------------------------------------------
static atx::core::Status read_config_file_into(
        RunConfig& cfg,
        const std::string& path,
        const std::set<std::string>& skip) {
    using EC = atx::core::ErrorCode;

    std::ifstream file(path);
    if (!file.is_open()) {
        return atx::core::Err(EC::IoError,
            "cannot open config file: '" + path + "'");
    }

    std::string line;
    int lineno = 0;
    while (std::getline(file, line)) {
        ++lineno;
        // Strip CR if present (Windows CRLF).
        if (!line.empty() && line.back() == '\r') line.pop_back();

        // Skip blank lines and comments.
        if (line.empty() || line[0] == '#') continue;

        const auto eq = line.find('=');
        if (eq == std::string::npos) {
            return atx::core::Err(EC::ParseError,
                path + ":" + std::to_string(lineno)
                + ": malformed line (expected flag=value): '" + line + "'");
        }

        std::string flag = line.substr(0, eq);
        std::string_view value{line.data() + eq + 1, line.size() - eq - 1};

        // A flag already supplied on the CLI wins: do not let the file override
        // it (regardless of value, including an explicit 0.0).
        if (skip.find(flag) != skip.end()) continue;

        auto r = apply_flag(cfg, flag, value);
        if (!r) {
            return atx::core::Err(EC::ParseError,
                path + ":" + std::to_string(lineno) + ": "
                + r.error().message());
        }
    }

    return atx::core::Ok();
}

// ---------------------------------------------------------------------------
// parse_config_file
// ---------------------------------------------------------------------------
atx::core::Result<RunConfig> parse_config_file(const std::string& path,
                                                const std::string& subcommand) {
    RunConfig cfg{};
    cfg.subcommand = subcommand;
    auto r = read_config_file_into(cfg, path, /*skip=*/{});
    if (!r) return atx::core::Err(std::move(r).error());
    return atx::core::Ok(cfg);
}

// ---------------------------------------------------------------------------
// merge_config_file
// ---------------------------------------------------------------------------
atx::core::Status merge_config_file(RunConfig& base, const std::string& path) {
    // CLI-present flags (already in base.set_flags) are skipped, so the file
    // only fills gaps the CLI left unset. Capture the skip-set by copy because
    // applying file flags mutates base.set_flags as we go.
    const std::set<std::string> skip = base.set_flags;
    return read_config_file_into(base, path, skip);
}

} // namespace atx::impl
