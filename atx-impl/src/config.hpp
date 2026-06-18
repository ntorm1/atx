#pragma once

#include <string>
#include <vector>

#include "atx/core/error.hpp"

namespace atx::impl {

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

} // namespace atx::impl
