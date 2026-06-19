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
inline constexpr std::array<std::string_view, 8> kSubcommands = {
    "load", "panel", "discover", "combine", "optimize", "report", "run", "regime"};

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

    // -- discover quality gate (opt-in; default path is the ungated top-N search) --
    // When --gated is set, discover routes through factory::Factory::mine_into:
    // every distinct candidate is ranked by deflated Sharpe and admitted into a
    // persistent library only if it clears the AlphaGate floors below AND
    // dsr >= min_dsr. This yields a robust, low-turnover, low-correlation,
    // high-fitness alpha database. Absent --gated, behavior is unchanged.
    bool        gated         = false; // --gated
    double      min_sharpe    = 1.0;   // --min-sharpe    (AlphaGate standalone-Sharpe floor)
    double      min_fitness   = 1.0;   // --min-fitness   (AlphaGate WorldQuant-fitness floor)
    double      max_turnover  = 0.70;  // --max-turnover  (AlphaGate per-alpha turnover cap)
    double      max_pool_corr = 0.70;  // --max-pool-corr (AlphaGate max |corr| to any admitted alpha)
    double      target_aum    = 0.0;   // --target-aum    (capacity cost objective; 0 = off)
    long        workers       = 0;     // --workers       (search DetPool fan-out; 0 = auto = cores-1).
                                       // Digest-invariant (F1): affects speed/memory, never bits.
    double      oos_fraction  = 0.0;   // --oos-fraction  (0 = off; fraction of the time axis held out for OOS admission)
    double      oos_embargo   = 0.0;   // --oos-embargo   (embargo fraction at the train|holdout cut; 0 = engine default)
    std::string run_db;                // --run-db  (SQLite progress DB path; "" = off, no store I/O)
    bool        resume        = false; // --resume  (requires --run-db; continue an incomplete matching run)

    // -- combine --
    std::string alphas;                // --alphas
    std::string combo_out;             // --combo-out
    std::string method;                // --method
    // --sector-neutral (opt-in): when set AND the panel has a "sector" field, each
    // alpha's book is sector-demeaned before blending so the mega-alpha expresses
    // idiosyncratic views, not sector bets. Default false: no-flag path byte-identical.
    bool        sector_neutral = false; // --sector-neutral
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
    bool        position_mode    = false; // --position-mode (signal-as-position deploy; skip mean-variance optimize)
    double      trade_rate       = 1.0;  // --trade-rate (position-mode partial-step toward prior book; 1.0 = full step = legacy)

    // -- report --
    std::string books;                 // --books
    std::string report_out;            // --report-out
    double      report_aum = 1e9;     // --report-aum (deployment AUM for capacity-footprint metrics in summary.txt)

    // -- regime (macro/regime data) --
    std::string staging_dir;   // --staging-dir  (regime subcommand: dir of staged CSVs)
    std::string regime_out;    // --regime-out   (regime subcommand: output .seg)
    std::string regime_segs;   // --regime-segs  (panel stage: regime .seg to broadcast)
    std::string regime_fields; // --regime-fields(panel stage: comma-separated series)

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
