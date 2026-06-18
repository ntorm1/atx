#pragma once

#include <array>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "atx/core/error.hpp"

namespace atx::impl {

// The single source of truth for valid subcommand names. parse_args validates
// against this; dispatch's routing if-chain consumes the same names.
inline constexpr std::array<std::string_view, 7> kSubcommands = {
    "load", "panel", "discover", "combine", "optimize", "report", "run"};

// ----------------------------------------------------------------------------
// RunConfig — all CLI flags / config-file keys for every subcommand.
//
// Field consolidation note: 'out' is a shared "primary output path" field used
// by load, panel (via panel_out), and other stages. Each stage has its own
// distinct named field where names differ (panel_out, alpha_out, combo_out,
// books_out, report_out) so --config round-trips are lossless.
// ----------------------------------------------------------------------------
struct RunConfig {
    std::string subcommand;            // "load"|"panel"|...|"run"|""

    // -- load --
    std::string zip;                   // --zip
    std::string out;                   // --out  (shared output field)
    std::string min_date;              // --min-date

    // -- panel --
    std::string segs;                  // --segs
    std::string panel_out;             // --panel-out
    std::string start;                 // --start
    std::string end;                   // --end
    double      min_adv_usd   = 0.0;  // --min-adv-usd
    long        top_n_by_adv  = 0;    // --top-n-by-adv

    // -- discover --
    std::string panel;                 // --panel
    std::string alpha_out;             // --alpha-out
    unsigned long long seed = 0ULL;    // --seed
    long        population   = 0;     // --population
    long        generations  = 0;     // --generations
    std::vector<std::string> seed_exprs; // --seed-expr (repeatable)
    double      min_dsr      = 0.0;   // --min-dsr

    // -- combine --
    std::string alphas;                // --alphas
    std::string combo_out;             // --combo-out
    std::string method;                // --method
    long        fit_begin    = 0;     // --fit-begin
    long        fit_end      = 0;     // --fit-end

    // -- optimize --
    std::string combo;                 // --combo
    std::string books_out;             // --books-out
    double      risk_aversion    = 0.0; // --risk-aversion
    double      turnover_penalty = 0.0; // --turnover-penalty
    double      gross            = 0.0; // --gross
    double      name_cap         = 0.0; // --name-cap
    std::string rebalance;             // --rebalance  "daily"|"weekly"

    // -- report --
    std::string books;                 // --books
    std::string report_out;            // --report-out

    // -- run --
    std::string config_file;           // --config

    // -- global --
    bool help        = false;
    bool quiet       = false;
    bool digest_only = false;

    // Canonical names of flags explicitly supplied by the parsed source (CLI
    // args or config-file keys). Used by the run-mode merge so a CLI-present
    // flag always wins over a file value, regardless of its value (e.g. an
    // explicit --gross 0.0 must not be treated as "unset"). Names are the same
    // keys apply_flag matches on (e.g. "gross", "seed-expr", "config").
    std::set<std::string> set_flags;
};

// Parse CLI arguments.
// argv[1] is the subcommand (or --help/-h).
// Returns Err(InvalidArgument) on unknown flag/subcommand.
[[nodiscard]] atx::core::Result<RunConfig> parse_args(int argc, char** argv);

// Parse a config file (newline-separated flag=value, # comments).
// Returns Err(IoError/ParseError) on failure.
[[nodiscard]] atx::core::Result<RunConfig> parse_config_file(
        const std::string& path,
        const std::string& subcommand);

// Merge a config file into an existing (CLI-parsed) config. Flags already
// present in base.set_flags (i.e. explicitly supplied on the CLI) are NOT
// overridden; the file only fills gaps the CLI left unset. This makes a CLI
// flag win regardless of its value (including an explicit 0.0).
[[nodiscard]] atx::core::Status merge_config_file(RunConfig& base,
                                                  const std::string& path);

} // namespace atx::impl
