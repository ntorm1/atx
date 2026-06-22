#include "config.hpp"

#include <cctype>
#include <charconv>
#include <fstream>
#include <string>
#include <string_view>
#include <utility>

#include "atx/core/error.hpp"

namespace atx::impl {

// ---------------------------------------------------------------------------
// Worker: apply one recognized (flag, value) pair to a RunConfig.
// 'flag' must NOT have a leading "--". Returns Err(InvalidArgument) for unknown
// flags. Does NOT touch cfg.set_flags â€” that bookkeeping lives in apply_flag.
// ---------------------------------------------------------------------------
static atx::core::Result<void> apply_flag_value(RunConfig& cfg,
                                                std::string_view flag,
                                                std::string_view value) {
    using EC = atx::core::ErrorCode;

    // Boolean flags (value is ignored / empty for valueless booleans).
    if (flag == "help")           { cfg.help          = true; return atx::core::Ok(); }
    if (flag == "quiet")          { cfg.quiet         = true; return atx::core::Ok(); }
    if (flag == "digest-only")    { cfg.digest_only   = true; return atx::core::Ok(); }
    if (flag == "gated")          { cfg.gated         = true; return atx::core::Ok(); }
    if (flag == "sector-neutral") { cfg.sector_neutral = true; return atx::core::Ok(); }
    if (flag == "conviction")     { cfg.conviction     = true; return atx::core::Ok(); }
    if (flag == "position-mode")  { cfg.position_mode  = true; return atx::core::Ok(); }
    if (flag == "resume")         { cfg.resume         = true; return atx::core::Ok(); }
    if (flag == "exclude-no-sector") { cfg.exclude_no_sector = true; return atx::core::Ok(); }
    if (flag == "require-sector")    { cfg.require_sector    = true; return atx::core::Ok(); }
    if (flag == "compact-universe")  { cfg.compact_universe  = true; return atx::core::Ok(); }
    if (flag == "industry-neutral")  { cfg.industry_neutral  = true; return atx::core::Ok(); }
    if (flag == "enable-wrap-in-op")  { cfg.enable_wrap_in_op = true; return atx::core::Ok(); }

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
    if (flag == "run-db")       { cfg.run_db        = value; return atx::core::Ok(); }
    if (flag == "library-dir")  { cfg.library_dir   = value; return atx::core::Ok(); }
    if (flag == "alphas")       { cfg.alphas        = value; return atx::core::Ok(); }
    if (flag == "combo-out")    { cfg.combo_out     = value; return atx::core::Ok(); }
    if (flag == "method")       { cfg.method        = value; return atx::core::Ok(); }
    if (flag == "combo")        { cfg.combo         = value; return atx::core::Ok(); }
    if (flag == "books-out")    { cfg.books_out     = value; return atx::core::Ok(); }
    if (flag == "rebalance")    { cfg.rebalance     = value; return atx::core::Ok(); }
    if (flag == "books")        { cfg.books         = value; return atx::core::Ok(); }
    if (flag == "report-out")   { cfg.report_out    = value; return atx::core::Ok(); }
    if (flag == "config")       { cfg.config_file   = value; return atx::core::Ok(); }
    if (flag == "staging-dir")   { cfg.staging_dir   = value; return atx::core::Ok(); }
    if (flag == "regime-out")    { cfg.regime_out    = value; return atx::core::Ok(); }
    if (flag == "regime-segs")   { cfg.regime_segs   = value; return atx::core::Ok(); }
    if (flag == "regime-fields") { cfg.regime_fields = value; return atx::core::Ok(); }

    // Repeatable string flag
    if (flag == "seed-expr")    { cfg.seed_exprs.emplace_back(value); return atx::core::Ok(); }

    // --seed-file <path>: read a `<id>: <dsl>` template library and append all
    // valid DSL strings (in file order, after any existing --seed-expr entries).
    // Fail-closed: unreadable path -> Err; zero valid templates -> Err.
    if (flag == "seed-file") {
        auto dsls_r = read_seed_file(std::string(value));
        if (!dsls_r) return atx::core::Err(std::move(dsls_r).error());
        // read_seed_file already returns Err(InvalidArgument) when it collects
        // zero templates, so a successful result is guaranteed non-empty.
        for (auto& dsl : *dsls_r) {
            cfg.seed_exprs.emplace_back(std::move(dsl));
        }
        return atx::core::Ok();
    }

    // --executor (C2.1): the sweep's OPTIONAL parallel substrate selector. Validated
    // against the closed {"", inprocess, process} taxonomy. "" / "inprocess" keep the
    // serial path; "process" runs each per-run mine on the ProcessExecutor. The digest
    // is invariant across the substrate, so this never shifts a result bit (F1).
    if (flag == "executor") {
        if (value != "" && value != "inprocess" && value != "process") {
            return atx::core::Err(EC::InvalidArgument,
                "--executor must be 'inprocess' or 'process'");
        }
        cfg.executor = value;
        return atx::core::Ok();
    }

    // --weight-transform (W1a): the book's cross-sectional transform. Lowercased,
    // then validated against the closed {rank,zscore,raw} taxonomy (reject anything
    // else with a clear error). Default "rank" reproduces engine::WeightPolicy{}.
    if (flag == "weight-transform") {
        std::string lowered{value};
        for (char& c : lowered) {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        if (lowered != "rank" && lowered != "zscore" && lowered != "raw") {
            return atx::core::Err(EC::InvalidArgument,
                "--weight-transform must be one of rank|zscore|raw: got '"
                + std::string(value) + "'");
        }
        cfg.weight_transform = std::move(lowered);
        return atx::core::Ok();
    }

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
    if (flag == "min-adv")           return parse_double(cfg.min_adv_usd); // W2 alias
    if (flag == "adv-window")        return parse_long(cfg.adv_window);    // W2
    if (flag == "top-n-by-adv")      return parse_long(cfg.top_n_by_adv);
    if (flag == "min-price")         return parse_double(cfg.min_price);
    if (flag == "seed")              return parse_ull(cfg.seed);
    if (flag == "population")        return parse_long(cfg.population);
    if (flag == "generations")       return parse_long(cfg.generations);
    if (flag == "min-dsr")           return parse_double(cfg.min_dsr);
    if (flag == "min-split-sharpe")  return parse_double(cfg.min_split_sharpe);   // W4a split-sample stability floor
    if (flag == "max-pbo")           return parse_double(cfg.max_pbo);            // W4b run-level CSCV-PBO batch gate
    if (flag == "robust-holdout-frac") return parse_double(cfg.robust_holdout_frac); // W4a robust-factor weak sub-universe
    if (flag == "winsorize-limit")   return parse_double(cfg.winsorize_limit);
    if (flag == "gross-leverage")    return parse_double(cfg.gross_leverage);
    if (flag == "min-sharpe")        return parse_double(cfg.min_sharpe);
    if (flag == "min-fitness")       return parse_double(cfg.min_fitness);
    if (flag == "max-turnover")      return parse_double(cfg.max_turnover);
    if (flag == "max-pool-corr")     return parse_double(cfg.max_pool_corr);
    if (flag == "target-aum")        return parse_double(cfg.target_aum);
    if (flag == "workers")           return parse_long(cfg.workers);
    if (flag == "oos-fraction")      return parse_double(cfg.oos_fraction);
    if (flag == "oos-embargo")       return parse_double(cfg.oos_embargo);
    if (flag == "oos-windows")       return parse_long(cfg.oos_windows);
    if (flag == "oos-window")        return parse_long(cfg.oos_window);
    if (flag == "sweep-runs")      return parse_long(cfg.sweep_runs);
    if (flag == "patience")        return parse_long(cfg.patience);
    if (flag == "fit-begin")         return parse_long(cfg.fit_begin);
    if (flag == "fit-end")           return parse_long(cfg.fit_end);
    if (flag == "walk-forward") {
        ATX_TRY_VOID(parse_long(cfg.walk_forward));
        if (cfg.walk_forward < 0) cfg.walk_forward = 0;  // negative -> clamp to off
        return atx::core::Ok();
    }
    if (flag == "holdout-frac") {
        ATX_TRY_VOID(parse_double(cfg.combine_holdout_frac));
        if (cfg.combine_holdout_frac < 0.0 || cfg.combine_holdout_frac >= 1.0) {
            return atx::core::Err(EC::InvalidArgument,
                "--holdout-frac must be in [0, 1): got " + std::string(value));
        }
        return atx::core::Ok();
    } // A2a combine holdout-fit
    if (flag == "corr-penalty")      return parse_double(cfg.corr_penalty);
    if (flag == "capacity-floor")    return parse_double(cfg.capacity_floor);
    if (flag == "risk-aversion")     return parse_double(cfg.risk_aversion);
    if (flag == "turnover-penalty")  return parse_double(cfg.turnover_penalty);
    if (flag == "gross")             return parse_double(cfg.gross);
    if (flag == "name-cap")          return parse_double(cfg.name_cap);
    if (flag == "trade-rate") {
        ATX_TRY_VOID(parse_double(cfg.trade_rate));
        if (cfg.trade_rate <= 0.0 || cfg.trade_rate > 1.0) {
            return atx::core::Err(EC::InvalidArgument,
                "--trade-rate must be in (0, 1]: got " + std::string(value));
        }
        return atx::core::Ok();
    }
    if (flag == "cost-bps") {
        ATX_TRY_VOID(parse_double(cfg.cost_bps));
        if (cfg.cost_bps < 0.0) {
            return atx::core::Err(EC::InvalidArgument,
                "--cost-bps must be >= 0: got " + std::string(value));
        }
        return atx::core::Ok();
    }
    if (flag == "report-aum") {
        ATX_TRY_VOID(parse_double(cfg.report_aum));
        if (cfg.report_aum <= 0.0) {
            return atx::core::Err(EC::InvalidArgument,
                "--report-aum must be > 0: got " + std::string(value));
        }
        return atx::core::Ok();
    }

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
        if (flag == "help" || flag == "quiet" || flag == "digest-only" || flag == "gated" || flag == "sector-neutral" || flag == "conviction" || flag == "position-mode" || flag == "resume" || flag == "industry-neutral" || flag == "enable-wrap-in-op") {
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

    // Cross-flag validation: --resume requires --run-db.
    if (cfg.resume && cfg.run_db.empty()) {
        return atx::core::Err(EC::InvalidArgument,
            "--resume requires --run-db");
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

// ---------------------------------------------------------------------------
// read_seed_file
// ---------------------------------------------------------------------------
// Format mirrors read_alpha_fixture (atx-impl/tests/alpha101_support.hpp:66-96):
//   - Lines whose first non-whitespace char is '#' are comments (skip).
//   - Blank lines are skipped.
//   - Each remaining line is split on the FIRST ':'; the DSL is the trimmed
//     remainder.  Lines with no ':' or an empty DSL after trim are skipped.
//   - The <id> prefix is informational only; it is discarded.
// Returns Err(IoError) if the file cannot be opened.
// Returns Err(InvalidArgument) if the file yields zero valid template lines.
atx::core::Result<std::vector<std::string>>
read_seed_file(const std::string& path) {
    using EC = atx::core::ErrorCode;

    std::ifstream in(path);
    if (!in.is_open()) {
        return atx::core::Err(EC::IoError,
            "read_seed_file: cannot open '" + path + "'");
    }

    std::vector<std::string> out;
    std::string line;
    while (std::getline(in, line)) {
        // Strip trailing CR (Windows CRLF).
        if (!line.empty() && line.back() == '\r') line.pop_back();

        // Find first non-whitespace character.
        std::size_t b = 0;
        while (b < line.size() &&
               (line[b] == ' ' || line[b] == '\t')) {
            ++b;
        }

        // Skip blank lines and comment lines.
        if (b == line.size() || line[b] == '#') continue;

        // Split on the first ':'.
        const std::size_t colon = line.find(':', b);
        if (colon == std::string::npos) continue;   // no colon — skip

        // Trim leading whitespace from DSL.
        std::size_t s = colon + 1;
        while (s < line.size() && (line[s] == ' ' || line[s] == '\t')) ++s;

        // Trim trailing whitespace from DSL.
        std::size_t e = line.size();
        while (e > s && (line[e-1] == ' ' || line[e-1] == '\t' ||
                         line[e-1] == '\r' || line[e-1] == '\n')) {
            --e;
        }

        if (s >= e) continue;  // empty DSL — skip

        out.emplace_back(line.substr(s, e - s));
    }

    if (out.empty()) {
        return atx::core::Err(EC::InvalidArgument,
            "read_seed_file: '" + path + "' contains no valid template lines");
    }
    return atx::core::Ok(std::move(out));
}

} // namespace atx::impl
