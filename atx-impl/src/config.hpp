#pragma once

#include <array>
#include <limits>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "atx/core/error.hpp"

namespace atx::impl {

// The single source of truth for valid subcommand names. parse_args validates
// against this; dispatch's routing if-chain consumes the same names.
inline constexpr std::array<std::string_view, 9> kSubcommands = {
    "load", "panel", "discover", "combine", "optimize", "report", "run", "regime", "sweep"};

// ----------------------------------------------------------------------------
// RunConfig â€” all CLI flags / config-file keys for every subcommand.
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
    bool        exclude_no_sector = false; // --exclude-no-sector (drop rows w/ no GICS at load: ETF/fund prune)

    // -- panel --
    std::string segs;                  // --segs
    std::string panel_out;             // --panel-out
    std::string start;                 // --start
    std::string end;                   // --end
    double      min_adv_usd   = 0.0;  // --min-adv-usd
    long        top_n_by_adv  = 0;    // --top-n-by-adv
    double      min_price     = 0.0;  // --min-price       (raw-close floor; membership iff raw_close > min_price; 0 = off)
    bool        require_sector   = false; // --require-sector   (single-stock screen: require a GICS/SIC sector)
    bool        compact_universe = false; // --compact-universe (drop instrument cols never in-universe -> tighter panel)

    // -- discover --
    std::string panel;                 // --panel
    std::string alpha_out;             // --alpha-out
    unsigned long long seed = 0ULL;    // --seed
    long        population   = 0;     // --population
    long        generations  = 0;     // --generations
    std::vector<std::string> seed_exprs; // --seed-expr (repeatable)
    double      min_dsr      = 0.5;   // --min-dsr

    // -- discover quality gate (opt-in; default path is the ungated top-N search) --
    // When --gated is set, discover routes through factory::Factory::mine_into:
    // every distinct candidate is ranked by deflated Sharpe and admitted into a
    // persistent library only if it clears the AlphaGate floors below AND
    // dsr >= min_dsr. This yields a robust, low-turnover, low-correlation,
    // high-fitness alpha database. Absent --gated, behavior is unchanged.
    bool        gated         = false; // --gated
    double      min_sharpe    = 0.25;  // --min-sharpe    (AlphaGate standalone-Sharpe sanity floor)
    double      min_fitness   = 1.0;   // --min-fitness   (AlphaGate WorldQuant-fitness floor)
    double      max_turnover  = 0.70;  // --max-turnover  (AlphaGate per-alpha turnover cap)
    double      max_pool_corr = 0.70;  // --max-pool-corr (AlphaGate max |corr| to any admitted alpha)
    double      target_aum    = 0.0;   // --target-aum    (capacity cost objective; 0 = off)
    long        workers       = 0;     // --workers       (search DetPool fan-out; 0 = auto = cores-1).
                                       // Digest-invariant (F1): affects speed/memory, never bits.
    double      oos_fraction  = 0.0;   // --oos-fraction  (0 = off; fraction of the time axis held out for OOS admission)
    // CAVEAT (combining --oos-fraction with --robust-holdout-frac): the robust ratio
    // geometry is MISALIGNED. The OOS search runs on the TRAIN sub-window, but the weak
    // panel is masked over the FULL-date panel, so the robust factor compares a full-
    // date weak universe against a train-window WQ. Treat the robust signal as
    // approximate when both knobs are set (re-deriving the weak mask over the train
    // panel is deferred / out of scope).
    double      oos_embargo   = 0.0;   // --oos-embargo   (embargo fraction at the train|holdout cut; 0 = engine default)
    // --min-split-sharpe (W4a): OPTIONAL split-sample stability admission floor. A
    // candidate is admitted only if BOTH halves of its OOS PnL stream have a per-
    // period Sharpe >= this floor AND both share the full-sample Sharpe sign (a
    // single-regime artifact — strong H1, dead/negative H2 — is rejected). DISABLING
    // DEFAULT = -infinity (NOT 0.0): the gate is evaluated only when the value is
    // FINITE, so the no-flag path is byte-identical to today (the factory determinism
    // golden + the discover slice are unchanged). Threaded into FactoryConfig::min_split_sharpe.
    double      min_split_sharpe = -std::numeric_limits<double>::infinity(); // --min-split-sharpe
    // --max-pbo (W4b): OPTIONAL run-level CSCV-PBO batch gate. The PROBABILITY OF
    // BACKTEST OVERFITTING (Bailey-López de Prado CSCV) computed POST-HOC over the SET
    // of alphas a run admitted; PBO ∈ [0, 1] (→0 a persistent edge, →0.5 the in-sample
    // winner is OOS noise). DISABLING DEFAULT = 1.0: at 1.0 the factory SKIPS the whole
    // computation, so the no-flag path is byte-identical to today (the factory
    // determinism golden + the discover slice are unchanged). ACTIVE when < 1.0: the
    // admitted SET passes iff its run-level PBO <= max_pbo. The verdict is ADVISORY-but-
    // RECORDED — it is surfaced in the gated manifest and a breach emits a loud warning,
    // but it never un-persists an alpha or changes the exit code. Threaded into
    // FactoryConfig::max_pbo.
    double      max_pbo = 1.0; // --max-pbo (run-level CSCV-PBO batch gate; 1.0 = off, active when < 1.0)
    // --robust-holdout-frac (W4a): OPTIONAL. When > 0, discover builds a weak/holdout
    // sub-universe Panel = the main panel with its universe restricted to a
    // DETERMINISTIC seeded instrument sub-sample (~this fraction of in-universe
    // instruments, drawn with sc.master_seed — NEVER thread/time), and threads it into
    // the search so the robust factor (robust = clamp(wq_on(weak)/wq, 0, 1)) ACTIVATES.
    // 0.0 (default) -> no weak panel built, robust stays the constant 1.0 and the
    // discover digest is byte-identical to today. Clamped to (0, 1); out-of-range -> off.
    // CAVEAT (combining with --oos-fraction > 0): the robust ratio geometry is
    // MISALIGNED — the weak panel is masked over the FULL-date panel while the OOS
    // search runs on the TRAIN sub-window, so robust compares a full-date weak universe
    // against a train-window WQ. The robust signal is approximate when both knobs are
    // set (re-deriving the weak mask over the train panel is deferred / out of scope).
    double      robust_holdout_frac = 0.0; // --robust-holdout-frac
    long        oos_windows   = 0;     // --oos-windows   (0 = legacy terminal holdout; >=1 = walk-forward windows)
    long        oos_window    = 0;     // --oos-window    (which window [0,oos_windows); sweep sets this per run)
    std::string run_db;                // --run-db  (SQLite progress DB path; "" = off, no store I/O)
    bool        resume        = false; // --resume  (requires --run-db; continue an incomplete matching run)
    // -- discover weight policy (W1a): the book's signal->weight knobs, exposed
    // so a discover run can use a Raw (passthrough) transform / a custom
    // winsorize band instead of the fixed Rank@2.5% book. DEFAULTS EXACTLY
    // REPRODUCE engine::WeightPolicy{} (transform=Rank, winsorize_limit=0.025,
    // industry_neutral=false, gross_leverage=1.0), so a discover run with NONE of
    // these flags is byte-identical to today. See stage_discover.cpp for the
    // string->Transform mapping and the industry_neutral discovery-wiring caveat.
    std::string weight_transform = "rank"; // --weight-transform: rank|zscore|raw
    double      winsorize_limit  = 0.025;  // --winsorize-limit  (0 disables; band == full range)
    bool        industry_neutral = false;  // --industry-neutral (needs a group_map; see caveat)
    double      gross_leverage   = 1.0;    // --gross-leverage   (target Sigma|w|, Alpha101 `scale`)
    // --enable-wrap-in-op (W1b): turn ON the wrap_in_op genetic mutation so the
    // search can CREATE in-expression conditioning (signedpower(zscore(raw), p)).
    // DEFAULT FALSE: absent this flag the SearchConfig knob defaults false and the
    // factory's mutate_one path is byte-identical to today (kGoldenDigest pin).
    bool        enable_wrap_in_op = false; // --enable-wrap-in-op

    // W2 — capacity universe screen for discovery (opt-in via --min-adv / --min-price).
    // Screen is ACTIVE iff min_adv_usd > 0 || min_price > 0 (reusing the panel-stage
    // fields); when INACTIVE the original panel object is passed through UNCHANGED.
    // adv_window: trailing window for ADV computation (ts_mean(dollar_volume, W)).
    // Default 20 (matching the manual capacity-universe validation). --min-adv is an
    // alias parse arm for cfg.min_adv_usd so "--min-adv 50e6" works as documented.
    long        adv_window = 20;           // --adv-window (used only when screen active)

    // --library-dir (8.A): a STABLE on-disk library::Library directory that
    // ACCUMULATES admitted alphas across discover runs/seeds (the library is
    // re-opened, not wiped, so re-admitting an identical alpha is deduped). UNSET
    // ("") preserves today's behavior exactly: a fresh per-run library under
    // <alpha_out>/_library wiped each run, so single-run determinism/resume goldens
    // stay byte-identical. Only an explicit --library-dir opts into accumulation.
    std::string library_dir;           // --library-dir  ("" = per-run <alpha_out>/_library, no accumulation)

    // -- sweep --
    long        sweep_runs   = 0;    // --sweep-runs (number of ResearchDriver runs; >=1 required for sweep)
    long        patience     = 0;    // --patience (early-stop after this many consecutive zero-admit runs; 0 = full budget)

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
    // --holdout-frac (A2a): fraction of the combine time axis held OUT of the
    // combiner fit so Task A2b's report can score the mega-alpha out-of-sample.
    // 0.0 (default) = off -> fit_end resolves exactly as today (np or explicit
    // --fit-end), so the no-flag combo.bin / digest is byte-identical. When > 0
    // (and --fit-end not explicitly set), the weights fit on [fit_begin, np-oos_n)
    // and [np-oos_n, np) is the OOS window. run_all defaults this to 0.25.
    double      combine_holdout_frac = 0.0; // --holdout-frac  (0 = off; fraction of the time axis held OUT of the
                                            //                  combiner fit so report can score it out-of-sample)
    // --corr-penalty / --capacity-floor (9.2, opt-in crowding de-correlation): a
    // post-fit transform of the combiner weights via crowding::decorrelate_weights.
    // BOTH default 0.0, which is the engine's EXACT-passthrough rail (corr_penalty==0
    // AND capacity_floor<=0 => weights returned bit-for-bit), so the no-flag combine
    // output stays byte-identical to today. Only an explicit --corr-penalty > 0 (or a
    // positive --capacity-floor) opts into de-correlation / capacity scaling.
    double      corr_penalty   = 0.0; // --corr-penalty   (0 = disabled = passthrough)
    double      capacity_floor = 0.0; // --capacity-floor (<=0 = capacity scaling off)

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

// Read a seed-file in `<id>: <dsl>` format (same as alpha101.txt).
// Lines whose first non-whitespace char is '#' are comments; blank lines and
// lines with no ':' or an empty DSL remainder are skipped. Returns the
// collected DSL strings in file order. Returns Err(IoError) if the file
// cannot be opened, Err(InvalidArgument) if the file yields zero templates.
[[nodiscard]] atx::core::Result<std::vector<std::string>>
read_seed_file(const std::string& path);

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
