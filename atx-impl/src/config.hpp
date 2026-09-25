#pragma once

#include <array>
#include <limits>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "atx/core/error.hpp"
#include "atx/core/types.hpp"  // atx::u16 (needed for adv_windows)

namespace atx::impl {

// B-04: keep the pre-W0 rejection available only as an explicit legacy policy.
enum class ReplayDelistingPolicy : atx::u8 { AbortV1 = 1, TerminalReturnV2 = 2 };

// Canonical CLI/recipe spelling; an invalid programmatic enum returns "invalid".
[[nodiscard]] const char *replay_delisting_policy_name(ReplayDelistingPolicy policy) noexcept;
// Accept only terminal-return or abort; invalid text returns InvalidArgument.
[[nodiscard]] atx::core::Result<ReplayDelistingPolicy>
parse_replay_delisting_policy(std::string_view value);

// The single source of truth for valid subcommand names. parse_args validates
// against this; dispatch's routing if-chain consumes the same names.
inline constexpr std::array<std::string_view, 14> kSubcommands = {
    "load", "panel", "discover", "combine", "optimize", "report", "run", "regime", "sweep",
    "metabook", "equity-baseline", "equity-book", "equity-ic", "equity-universe"};

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
    // Explicit legacy diagnostics only. Identified/unknown panels cannot be mixed.
    bool allow_unidentified_panels = false; // --allow-unidentified-panels true|false
    std::string preparation_manifest;      // --preparation-manifest (load provenance)

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
    // -- S7-2: S4 cost-aware admission-gate knobs (inert defaults -> byte-identical) --
    double cost_bps_admit    = 0.0; // --cost-bps-admit   -> GateConfig.rt_cost_bps
    double min_holding_days  = 0.0; // --min-holding-days -> GateConfig.min_holding_days
    double cost_max_turnover = 0.0; // --cost-max-turnover (0 = off; else overrides GateConfig.max_turnover)
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
    // -- S7-1: S3 search net-cost / seed-handling knobs (inert defaults -> byte-identical) --
    double turnover_penalty_slope = 0.0;                                  // --turnover-penalty-slope -> FitnessCfg
    double max_turnover_target    = std::numeric_limits<double>::infinity(); // --max-turnover-target -> FitnessCfg
    bool   protect_seed_elites    = false;                               // --protect-seed-elites -> SearchConfig
    bool   mutate_seed_copies     = false;                               // --mutate-seed-copies  -> SearchConfig
    double min_viable_raw         = 0.0;                                 // --min-viable-raw      -> SearchConfig

    // --reject-price-scale (R2): opt-in admission gate. Rejects a candidate whose
    // holdout book is a trivial 1/price (price-scale) tilt. The threshold is the
    // MAX allowed |time-averaged cross-sectional Pearson(w, 1/raw_close)|. Default
    // 1.0 = OFF (any correlation passes). Active when < 1.0. Range: (0, 1].
    // When absent (the default), the OOS digest and version_id are BYTE-IDENTICAL.
    double      max_price_scale_corr = 1.0; // --reject-price-scale (R2); 1.0 == OFF

    // --dsr-subwindows <K> (R3 Q1): opt-in intra-holdout DSR sub-windows gate. Splits the sealed
    // holdout PnL into K contiguous sub-segments and requires min_dsr on EACH. 0 (default) == OFF.
    // Active when >= 2; value 1 is rejected (meaningless: single window == aggregate gate).
    int         dsr_subwindows = 0;  // --dsr-subwindows (R3); 0 == OFF

    // --pbo-hard-block (R3 Q2): escalate advisory PBO breach to FAIL verdict + non-zero exit.
    // The manifest + library are already persisted when the Err is returned; the run exits non-zero.
    // Requires --max-pbo < 1.0 to have any effect (the advisory gate must be active to breach).
    bool        pbo_hard_block = false; // --pbo-hard-block (R3); escalate advisory PBO breach to non-zero exit

    // --deflate-selection (R4): opt-in deflated-Sharpe search selection pressure.
    // When set, the deflated Sharpe (DSR) enters the genetic search's SELECTION
    // signal: objectives[kObjDeflation]=dsr (new NSGA column) and raw is multiplied
    // by dsr (elitism/ScalarRaw haircut). The per-generation deflation N =
    // max(1, canon.size()) is captured serially before the parallel_for, so the
    // seq==parallel invariant holds. DEFAULT FALSE: absent this flag both the F1
    // search digest and every existing golden are BYTE-IDENTICAL to today.
    bool        deflate_selection = false;  // --deflate-selection (R4)

    // --capacity-objective / --turnover-objective (S4): opt-in NSGA objectives.
    // capacity_objective REQUIRES --target-aum > 0 (its AUM anchor) -- validated in
    // stage_discover.cpp (fail-loud, not a silent no-op). Both default false ->
    // byte-identical to today.
    bool        capacity_objective = false; // --capacity-objective -> SearchConfig
    bool        turnover_objective = false; // --turnover-objective -> SearchConfig

    // --typed-fields (R1): opt-in field-type discipline. When set, before running
    // the search a one-pass cardinality scan classifies each numeric panel field;
    // binary / low-cardinality / categorical fields are excluded from the grammar's
    // NUMERIC leaf pool and the GICS classifier (raw "gics" column if present) is
    // routed to the GROUP pool instead. DEFAULT FALSE: absent this flag both
    // exclusion lists stay EMPTY and the search is BYTE-IDENTICAL to today.
    bool        typed_fields = false;       // --typed-fields (valueless bool)
    // --field-cardinality-max <K> (R1): the distinct-value threshold for the R1
    // cardinality scan. A numeric field with <= K distinct finite values is excluded
    // from the numeric leaf pool. DEFAULT 12. Ignored when --typed-fields is absent.
    int         field_cardinality_max = 12; // --field-cardinality-max <K>

    // W2 — capacity universe screen for discovery (opt-in via --min-adv / --min-price).
    // Screen is ACTIVE iff min_adv_usd > 0 || min_price > 0 (reusing the panel-stage
    // fields); when INACTIVE the original panel object is passed through UNCHANGED.
    // adv_window: trailing window for ADV computation (ts_mean(dollar_volume, W)).
    // Default 20 (matching the manual capacity-universe validation). --min-adv is an
    // alias parse arm for cfg.min_adv_usd so "--min-adv 50e6" works as documented.
    long        adv_window = 20;           // --adv-window (used only when screen active)
    // -- S7-3: S5 panel-augment knobs. adv_windows MUST be std::vector<atx::u16> — stage_panel.cpp
    // already assigns cfg.adv_windows directly into a std::vector<atx::u16>. empty => {adv_window}.
    std::vector<atx::u16> adv_windows;      // --adv-windows (comma-separated list)
    bool                  augment_panel = false; // --augment-panel (valueless bool)

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
    // --executor (C2.1): OPTIONAL parallel substrate selector for the sweep's per-run
    // mine. "" (default) and "inprocess" keep the SERIAL path (byte-identical to
    // today); "process" runs each per-run mine on the proven bit-identical
    // ProcessExecutor (mine_into_oos_parallel). The digest is invariant across the
    // substrate + worker count (F1), so --executor never shifts a result bit.
    std::string executor;            // --executor <inprocess|process>; "" = serial

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
    // --conviction (D1.2, opt-in): after the combiner fits, scale each alpha's weight by a
    // per-alpha conviction score computed AT COMBINE TIME from that alpha's own PnL stream
    // (deflated-Sharpe probability + first/second-half Sharpe stability), then renormalize
    // Σ|w|=1. Default false => the gated block is skipped and combo.bin is byte-identical.
    bool        conviction = false;  // --conviction

    // --kelly-fraction / --kelly-max-gross (S5-2, opt-in): fractional-Kelly,
    // conviction-scaled, covariance-aware sizing of the combined book. When
    // kelly_fraction > 0, the combiner's renormalized weights are REPLACED by the
    // Kelly target f = kelly_fraction * V^{-1}mu, scaled per-alpha by conviction
    // (S5-1 scores when --conviction is on, else an all-1.0 vector) and gross-clamped
    // to kelly_max_gross. V is a diagonal FactorModel built from per-alpha realized
    // PnL variance over the fit window (the minimal-scope covariance; a full factor
    // model is out of S5 scope). Default 0.0 = OFF => the Kelly block is skipped
    // entirely and combo.bin / the digest are byte-identical to today. See
    // risk/kelly_sizing.hpp. (CLI arg parsing in config.cpp is deferred to S7; these
    // fields are set directly on RunConfig until then.)
    double      kelly_fraction  = 0.0; // --kelly-fraction  (0.0 = off = byte-identical)
    double      kelly_max_gross = 1.0; // --kelly-max-gross (cap on Sum|w|; <=0 disables clamp)

    // --walk-forward <k> (D3b, opt-in): when k>=1, run an expanding-window k-fold walk-forward re-fit of
    // the combiner over [fit_begin, np) and RECORD each fold's OOS Sharpe + their mean as telemetry. The
    // shipped combo.bin is unchanged (WF re-fits are scratch). Default 0 = off => byte-identical, no extra work.
    long        walk_forward = 0;    // --walk-forward

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
    double      cost_bps         = 0.0;  // --cost-bps   (flat round-trip transaction cost in basis points; 0 = off = byte-identical)

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

    // =========================================================================
    // -- S5 (p8 hub): risk-model / meta-book / combine-method / cost-in-selection /
    //    deflation knobs the S1-S4 feature sprints exposed on their own engine
    //    config structs. Each field below defaults to TODAY's behavior, so a
    //    discover/combine/optimize/report/run invocation with NONE of these flags
    //    asserted is byte-identical to pre-S5 (the AtxImplDiscover / FactoryOos /
    //    NsgaSearch goldens are the gate). Appended at struct END per the p8
    //    convention (aggregate-init order is load-bearing).
    // =========================================================================
    // --risk-model=diagonal|factor (S1): selects risk::RiskModelConfig::kind at the
    // optimize call site. "diagonal" (default) reproduces today's per-name variance
    // model exactly (risk::RiskModelConfig{} default); "factor" activates the S1
    // factor-covariance spine (stage_optimize.cpp threads this into the 2-arg
    // run_optimize(cfg, risk_cfg) overload the CLI zero-arg path now calls).
    std::string risk_model = "diagonal";
    bool dead_alpha_factors = false; // --dead-alpha-factors (S1; false = no crowding augmentation)
    bool group_neutralize   = false; // --group-neutralize   (S1; false = no factor/industry neutralize)

    // --dead-alpha-lib-dir (p9 S1): the on-disk library::Library directory whose ADMITTED alpha
    // pool is threaded into build_risk_model's dead_lib/dead_ids as the Kakushadze-Yu crowding-
    // factor source (stage_optimize.cpp's build_risk_model call sites — the Potemkin-book gap).
    // "" (default) FALLS BACK to cfg.library_dir (the discover accumulating library, if
    // --library-dir was set); if THAT is also "" -- or the resolved directory does not exist on
    // disk -- the wire is a documented fail-open no-op (matches build_risk_model's own
    // dead_lib==nullptr contract, stage_riskmodel.hpp:120-126).
    std::string dead_alpha_lib_dir; // --dead-alpha-lib-dir ("" = fall back to --library-dir, else off)

    // --metabook (S2): enable the meta-book stage (assign_sleeves + run_metabook)
    // in run_all / the standalone "metabook" subcommand. false (default) = the
    // pipeline's optimize stage runs exactly as today (no metabook stage inserted).
    bool metabook = false;
    // --sleeve-method=erc|hrp|invvol (S2): fund::RiskBudgetMethod for
    // MetaAllocatorConfig; ignored unless --metabook is set. "invvol" is the
    // simplest/most-conservative default among the three (not necessarily the
    // engine's own MetaAllocatorConfig{} default, which is EqualRiskContribution) —
    // the mapping is applied only when --metabook activates the stage at all.
    std::string sleeve_method = "invvol";

    // --combine-method: NOT a new field. `--method stack|regime-stack` (S3;
    // stage_combine.cpp's existing method_from_string) ALREADY routes this end to
    // end from the CLI — adding a second `combine_method` flag would be a
    // duplicate, confusing knob. See the S5 ledger for the reconciliation note.

    // --impact-in-selection / --selection-aum (S4, wired end-to-end in the p8
    // final-wave): threads the FLAG + AUM into FitnessCfg.cost_selection
    // (fitness.hpp), which fitness.cpp's fitness_core/finish_report now actually
    // consume (book_cost_bps at selection_aum nets into the ScalarRaw selection
    // scalar via factory::apply_selection_cost). false (default) is inert
    // either way — see fitness_cost_selection_test.cpp for the RED->GREEN proof.
    bool   impact_in_selection = false; // --impact-in-selection
    double selection_aum       = 0.0;   // --selection-aum (0 = off; AUM cost is priced at)

    // --capacity-curve (S4): documented pass-through marker. stage_report.cpp
    // ALREADY emits the book-level capacity curve unconditionally whenever
    // cfg.report_aum > 0.0 (the existing default) — there is no gate in the S4
    // emit body for a separate flag (S4-owned file; not edited by S5). This field
    // lets an operator/harness assert intent explicitly; the V1 scorecard (S5-5)
    // is the actual CONSUMER of the emitted capacity_point_aum / book_gross_edge_bps
    // KVs regardless of this flag's value.
    bool capacity_curve = false;

    // --require-split-stable (deflation, S1 GateConfig plumbing): wired into
    // stage_discover.cpp's `gc.require_split_stable` (the AlphaGate the library
    // path consults). false (default) = GateConfig inert (byte-identical).
    bool require_split_stable = false;
    // --blocking-pbo (S5-2): opt-in escalation of the advisory run-level PBO gate
    // to UN-ADMIT the run's marginal admits (or fail closed) instead of merely
    // warning. Distinct from --pbo-hard-block (exit-code-only escalation). false
    // (default) = advisory-only, exactly as today.
    bool blocking_pbo = false;

    // --robustness-battery (p8 final-wave, Item 3): opt-in eval::RobustnessBattery
    // (S5-3) noise-control check at admission (factory::FactoryConfig::
    // robustness_battery). Threaded into fcfg.robustness_battery in
    // stage_discover.cpp. A candidate whose edge SURVIVES a seeded permutation of
    // the panel's "close" field (a dimensional-artifact negative control) is
    // rejected with library::AdmitKind::RejectRobustness. SCOPED PARTIAL this
    // wave: only the noise_control check is wired (see FactoryConfig's own doc
    // for the sub_universe/alt_neutralization/param_perturbation deferral).
    // false (default) never builds a BatteryConfig/Reevaluator/alternate Panel at
    // all -> byte-identical to today.
    bool robustness_battery = false;

    // -- p7-S7 carry-forwards (subsumed here; augment/incremental-panel wiring) --
    // --short-interest / --augment-out (FINRA short-interest augment stage): the
    // RunConfig fields the CLI would need are threaded here, but the "augment"
    // CLI subcommand itself is NOT implemented in this sprint — building it needs
    // NEW infrastructure (reconstructing the panel date axis + a FINRA-ticker ->
    // instrument map from the ORATS seg partition + symbology, per
    // stage_augment.hpp's own CLI-deferral note) well beyond CLI flag threading.
    // The pure engine core (augment_panel_with_finra) is fully built/tested
    // (S2/p7); only the CLI stage is deferred. Recorded honestly in the S5 ledger
    // (never a hard block) rather than fabricated.
    std::string short_interest;             // --short-interest <csv> (deferred stage; see ledger)
    std::string augment_out;                // --augment-out <bin>   (deferred stage; see ledger)
    // W0-I0b / D-02: FINRA short interest becomes usable on the 8th NYSE session after
    // settlement (7 sessions plus the after-close +1, FinraPublicationLag{} in
    // data/finra_short.hpp). The count is in NYSE SESSIONS under the default rule
    // "nyse-sessions-v2"; "calendar-days-v1" keeps the pre-W0 calendar-day meaning
    // (the old default was 2 calendar days, which leaked 1-2 sessions on ~40% of rows).
    long        si_publication_lag = 7;     // --si-publication-lag <sessions|days>
    std::string si_publication_lag_rule = "nyse-sessions-v2"; // --si-publication-lag-rule
    // --incremental-panel (S6/p7 carry-forward): runtime opt-in for
    // stage_panel.cpp's acquire_history_panel incremental append path (previously
    // gated behind the ATX_PANEL_INCREMENTAL compile macro, unreachable from any
    // build). false (default) = full rebuild, byte-identical to today.
    bool incremental_panel = false;

    // --gp-trading / --gp-risk-aversion / --gp-trade-cost-scale (p9 S3): opt-in
    // Gârleanu-Pedersen aim-portfolio trade for the position-mode partial-step
    // (stage_optimize.cpp's "5a" branch). false (default) leaves the existing
    // linear trade_rate blend (w := prev + trade_rate*(target-prev)) untouched --
    // the no-flag path is byte-identical. When true, the partial-trade step
    // instead trades toward the GP AIM (risk::gp_turnover_native_step), computed
    // from risk::gp_aim_and_value(alpha_bar, V, gp_risk_aversion) over a single
    // whole-panel Diagonal risk model V built once for the run (see
    // stage_optimize.cpp's gp_trading block; independent of --risk-model, which
    // remains an MVO-branch-only concern owned by S1/S2). gp_risk_aversion=0.0
    // (inert default) is gp_aim_and_value's own documented lambda==0 convention
    // (aim_pos == alpha_bar, no risk-curvature fold -- a valid, if aggressive, GP
    // setting, NOT an error; unlike the nearby risk_aversion field, this one is
    // read RAW, with no set_flags-gated substitution). gp_trade_cost_scale=0.0
    // (inert default) leaves the effective GP trade rate kappa == cfg.trade_rate
    // unchanged; kappa = trade_rate / (1 + gp_trade_cost_scale) for
    // gp_trade_cost_scale > 0 -- a scoped, caller-side scalar approximation (the
    // shipped GP reduction has no matrix trade-cost knob to bind this to; see
    // garleanu_pedersen.hpp's own "matrix Riccati is the recorded lift, not
    // shipped" note).
    bool   gp_trading          = false; // --gp-trading
    double gp_risk_aversion    = 0.0;   // --gp-risk-aversion (0 = lambda=0 GP convention)
    double gp_trade_cost_scale = 0.0;   // --gp-trade-cost-scale (0 = kappa == trade_rate)

    // -- p9 S5: book-level gates / capacity-in-optimizer / full robustness battery / borrow --
    // All fields inert-default; a run asserting none of them is byte-identical to pre-S5.
    // --book-turnover-gate (S5-1): cross-sleeve-netted book turnover, expressed as a PER-DAY
    // L1 rate (mean per-period turnover / average rebalance-day spacing). 0.0 (default) = the
    // rate is measured and surfaced (stage_optimize/stage_metabook kvs: "book_turnover_per_day")
    // but never rejects. Active when > 0.0: a measured rate exceeding this threshold fails the
    // stage CLOSED (mirrors --blocking-pbo's escalation; the books/sidecar are already written
    // by the time the check runs, so a rejected run's own diagnostics stay inspectable).
    double book_turnover_gate = 0.0;
    // --participation-cap (S5-2): ADV participation fraction rho bounding |w_i| INSIDE the
    // optimizer's QP construction (risk::ParticipationCap.adv_frac), not just the post-hoc
    // stage_report.cpp capacity curve. 0.0 (default) = mc.constraints stays unset -> the fast
    // (non-augmented) PortfolioOptimizer path runs, byte-identical to today.
    double participation_cap = 0.0;
    // --borrow-bps (S5-4): flat per-period financing charge (bps) on the book's SHORT weight
    // (mirrors --cost-bps's own bps-per-period convention). 0.0 (default) = pnl_borrow is
    // exactly 0.0 every period -> report digest byte-identical to today.
    double borrow_bps = 0.0;
    // Identified report replay: hypothetical close execution after this many
    // stored observations. This delay does not certify historical availability.
    // W0-I0b / B-02: 0 (same-close fills) is rejected unless --allow-same-close.
    atx::usize replay_execution_delay = 1; // --replay-execution-delay
    // W0-I0b / I-11: the identified report refuses to run unless BOTH rates were
    // chosen explicitly (a flag, a config key, or a nonzero programmatic value); an
    // intentional frictionless replay must say --replay-trade-bps 0 etc. out loud.
    double replay_trade_bps = 0.0;         // --replay-trade-bps per actual dollar traded
    double replay_annual_borrow_bps = 0.0; // --replay-annual-borrow-bps annual simple rate
    // W0-I0b / B-02: explicit opt-in to a zero execution delay (same-close fills) in
    // the replay, equity IC and equity-mine delays. Default false: delay < 1 is refused.
    bool allow_same_close = false;         // --allow-same-close [true|false]
    int replay_day_basis = 365;            // --replay-day-basis 360|365 calendar days
    ReplayDelistingPolicy replay_delisting_policy = ReplayDelistingPolicy::TerminalReturnV2;
                                          // --replay-delisting-policy terminal-return|abort
    // Fixed equity-baseline recipe: explicit scored window, separate from the
    // source panel's feature warmup. Dates are validated by the baseline stage.
    std::string equity_evaluation_start; // --evaluation-start, inclusive
    std::string equity_evaluation_end;   // --evaluation-end, exclusive
    atx::u64 equity_max_working_bytes = 3'000'000'000ULL; // --max-working-bytes
    double equity_min_dollar_adv = 0.0;                  // --min-dollar-adv (0 = no liquidity floor)
    atx::usize equity_dollar_adv_window = 21;            // --dollar-adv-window (sessions)
    // Identified fixed baseline artifacts supplying equity-book preferences.
    // The stage inherits baseline dates/financial assumptions unless overridden;
    // set_flags distinguishes an explicit zero fee from an omitted override.
    std::string equity_baseline_dir; // --baseline-dir
    // Append-only hash-chained pre-registration ledger consumed by equity-ic.
    // Empty = the stage's own frozen default, atx-engine/reviews/trial-ledger.jsonl.
    std::string equity_trial_ledger; // --trial-ledger
    // W0-I0b / D-12: point-in-time membership for equity-baseline / equity-ic. A
    // context built with --universe-membership carries a YEAR-UNION allow-list; under
    // the default rule "as-of-v2" the stage re-applies the membership AS OF each
    // session (a name is admitted only once the last rebalance effective on or before
    // that session lists it), which needs the same membership.bin the context names.
    // "year-union-v1" reproduces the pre-W0 (look-ahead) mask for frozen artifacts.
    std::string equity_membership;                  // --membership <membership.bin>
    std::string equity_membership_rule = "as-of-v2"; // --membership-rule as-of-v2|year-union-v1
    // W0-I0b / E-18: equity-ic minimum admitted names for a date to emit (was 2).
    atx::usize equity_ic_min_names_per_date = 50; // --min-names-per-date
    // W0-I0b / E-09 wiring: equity-ic entry delay (sessions from the signal close to
    // the entry close). 0 requires --allow-same-close.
    atx::usize equity_ic_execution_delay = 1;     // --ic-execution-delay
    // W0-I0b / E-02 wiring: two-horizon-v2 (default) | half-horizon-v1 | politis-white-v2.
    std::string equity_ic_block_len_rule = "two-horizon-v2"; // --ic-block-len-rule
    // W0-I0b / I-15: optional terminal-return table (CSV; W2-D2 delivers the data).
    // Empty = the frozen checkpoint-14 2013 table built into equity-ic.
    std::string equity_terminal_returns;          // --terminal-returns <csv>
    // Checkpoint 15 `equity-universe` (design 2026-09-20-iteration15 §5.1-§5.2): the
    // `;`-separated segment directories and their positionally paired preparation
    // manifests, plus the inclusive rank-date window. Parsed and validated by the stage.
    std::string equity_segments_dirs;         // --segments-dirs a;b;...
    std::string equity_preparation_manifests; // --preparation-manifests a;b;...
    std::string equity_rank_start;            // --rank-start, inclusive YYYY-MM-DD
    std::string equity_rank_end;              // --rank-end, inclusive YYYY-MM-DD
    // --robustness-sub-universe / --robustness-alt-neutralization / --robustness-param-perturb
    // (S5-3): expose the 3 currently-unreachable eval::BatteryConfig checks (noise_control is
    // already wired via --robustness-battery alone, p8 final-wave). Each requires BOTH its own
    // flag AND --robustness-battery; all false (default) = battery_cfg construction is exactly
    // today's noise-control-only shape -> byte-identical admitted set/digest.
    bool robustness_sub_universe = false;
    bool robustness_alt_neutralization = false;
    bool robustness_param_perturb = false;

    // Checkpoint 16 `panel` point-in-time membership restriction. All three are
    // REQUIRED TOGETHER (parse_args and the stage both reject a partial set): a
    // membership.bin, the (top_n, band) cut inside it, and the evaluation start that
    // anchors the union rule. None supplied (the default) leaves every panel code
    // path — recipe bytes included — byte-identical to today.
    std::string panel_universe_membership; // --universe-membership <membership.bin>
    std::string panel_universe_cut;        // --universe-cut <top_n>:<band>, e.g. "3000:0.00"
    std::string panel_universe_eval_start; // --universe-eval-start YYYY-MM-DD
    // R21-3 `panel` point-in-time scalar field ingest. Each --asof-field entry is
    // `<name>=<csv_path>` (repeatable, command-line order preserved); the panel stage
    // appends one f64 column per entry after build_history_panel (see asof_field.hpp
    // for the CSV contract and the strict available_at < session-date join rule).
    // --asof-max-stale-days caps a value's calendar age (0 = no cap). Empty list (the
    // default) leaves every panel code path and the recipe bytes unchanged.
    std::vector<std::pair<std::string, std::string>> panel_asof_fields; // --asof-field
    atx::i64 panel_asof_max_stale_days = 0;                            // --asof-max-stale-days

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

// Checkpoint 16: the `panel` membership restriction is all-three-or-none.
// Ok when none of --universe-membership / --universe-cut / --universe-eval-start is
// set, and when all three are; Err(InvalidArgument) for any partial set. parse_args
// calls this, and the panel stage calls it again so a config-file-only invocation
// (which never runs parse_args' cross-flag pass) fails the same way.
[[nodiscard]] atx::core::Status validate_membership_flags(const RunConfig& cfg);

// W0-I0b / B-02: an execution delay below one session means the book fills at the
// same close that produced its signal. Refused (InvalidArgument) for
// replay_execution_delay and equity_ic_execution_delay unless allow_same_close.
[[nodiscard]] atx::core::Status validate_execution_delay(const RunConfig& cfg);

// Every cross-flag rule (--resume needs --run-db, membership all-three-or-none, the
// execution-delay guard, the si-lag rule). parse_args applies it when no --config is
// given; with --config it is deferred and the caller (dispatch) MUST run it on the
// merged result, so a file cannot bypass what the CLI enforces and an opt-in named in
// the file (allow-same-close=true) counts for a CLI value.
[[nodiscard]] atx::core::Status validate_cross_flags(const RunConfig& cfg);

// Parse a boolean flag value (W0-I0b / I-10): "" (a valueless CLI flag or a bare
// `flag=` config line), "true" and "1" mean true, "false" and "0" mean false; anything
// else is Err(InvalidArgument). Exposed for the stages that parse their own flags.
[[nodiscard]] atx::core::Result<bool> parse_bool_flag_value(std::string_view flag,
                                                            std::string_view value);

// The subcommands that refuse --config outright (their allowed-flag sets are
// frozen); every other subcommand merges the file (W0-I0b / I-10).
[[nodiscard]] bool subcommand_rejects_config(std::string_view subcommand) noexcept;

// Parse CLI arguments.
// argv[1] is the subcommand (or --help/-h). A boolean flag consumes one following
// literal true / false / 1 / 0 token as its value. Runs validate_cross_flags unless
// --config is present (see validate_cross_flags).
// Returns Err(InvalidArgument) on unknown flag/subcommand.
[[nodiscard]] atx::core::Result<RunConfig> parse_args(int argc, char** argv);

// Parse a config file (newline-separated flag=value, # comments). A boolean key takes
// true|false|1|0 (or an empty value, meaning true). A nested `config=` key is refused.
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
