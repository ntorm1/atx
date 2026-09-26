#include "config.hpp"

#include <array>
#include <cctype>
#include <charconv>
#include <cmath>
#include <fstream>
#include <limits>
#include <string>
#include <string_view>
#include <utility>

#include "atx/core/error.hpp"

namespace atx::impl {

const char *replay_delisting_policy_name(ReplayDelistingPolicy policy) noexcept {
    switch (policy) {
    case ReplayDelistingPolicy::AbortV1: return "abort";
    case ReplayDelistingPolicy::TerminalReturnV2: return "terminal-return";
    }
    return "invalid";
}

atx::core::Result<ReplayDelistingPolicy>
parse_replay_delisting_policy(std::string_view value) {
    if (value == "terminal-return") return atx::core::Ok(ReplayDelistingPolicy::TerminalReturnV2);
    if (value == "abort") return atx::core::Ok(ReplayDelistingPolicy::AbortV1);
    return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
        "--replay-delisting-policy requires terminal-return or abort");
}

namespace {

// W0-I0b / I-10: every boolean flag in one table. A boolean's value is PARSED
// (parse_bool_flag_value) rather than ignored, so `metabook=false` in a config file
// turns the stage off instead of on. On the CLI a boolean is valueless, optionally
// followed by one boolean literal token (parse_args). The literals are `true` /
// `false` and their numeric spellings `1` / `0`, which committed runbooks pass
// (`--require-sector 1 --compact-universe 1`, fix pass 1).
[[nodiscard]] constexpr bool is_bool_literal(std::string_view token) noexcept {
    return token == "true" || token == "false" || token == "1" || token == "0";
}

struct BoolFlag {
    std::string_view name;
    bool RunConfig::*field;
};

constexpr std::array<BoolFlag, 35> kBoolFlags{{
    {"help", &RunConfig::help},
    {"quiet", &RunConfig::quiet},
    {"digest-only", &RunConfig::digest_only},
    {"gated", &RunConfig::gated},
    {"sector-neutral", &RunConfig::sector_neutral},
    {"conviction", &RunConfig::conviction},
    {"position-mode", &RunConfig::position_mode},
    {"resume", &RunConfig::resume},
    {"exclude-no-sector", &RunConfig::exclude_no_sector},
    {"require-sector", &RunConfig::require_sector},
    {"compact-universe", &RunConfig::compact_universe},
    {"industry-neutral", &RunConfig::industry_neutral},
    {"enable-wrap-in-op", &RunConfig::enable_wrap_in_op},
    {"typed-fields", &RunConfig::typed_fields},             // R1
    {"pbo-hard-block", &RunConfig::pbo_hard_block},         // R3
    {"deflate-selection", &RunConfig::deflate_selection},   // R4
    {"protect-seed-elites", &RunConfig::protect_seed_elites}, // S7-1
    {"mutate-seed-copies", &RunConfig::mutate_seed_copies},   // S7-1
    {"augment-panel", &RunConfig::augment_panel},             // S7-3
    {"dead-alpha-factors", &RunConfig::dead_alpha_factors},   // S5-0 (S1)
    {"group-neutralize", &RunConfig::group_neutralize},       // S5-0 (S1)
    {"metabook", &RunConfig::metabook},                       // S5-0 (S2)
    {"impact-in-selection", &RunConfig::impact_in_selection}, // S5-0 (S4)
    {"capacity-curve", &RunConfig::capacity_curve},           // S5-0 (S4)
    {"require-split-stable", &RunConfig::require_split_stable}, // S5-0 (deflation)
    {"blocking-pbo", &RunConfig::blocking_pbo},                 // S5-2
    {"incremental-panel", &RunConfig::incremental_panel},       // S5-0 (p7 carry-forward)
    {"robustness-battery", &RunConfig::robustness_battery},     // p8 final-wave (Item 3)
    {"robustness-sub-universe", &RunConfig::robustness_sub_universe},             // S5-3
    {"robustness-alt-neutralization", &RunConfig::robustness_alt_neutralization}, // S5-3
    {"robustness-param-perturb", &RunConfig::robustness_param_perturb},           // S5-3
    {"gp-trading", &RunConfig::gp_trading},                   // p9 S3
    {"capacity-objective", &RunConfig::capacity_objective},   // p9 S4
    {"turnover-objective", &RunConfig::turnover_objective},   // p9 S4
    {"allow-same-close", &RunConfig::allow_same_close},       // W0-I0b / B-02
}};

[[nodiscard]] const BoolFlag *find_bool_flag(std::string_view flag) noexcept {
    for (const auto &entry : kBoolFlags) {
        if (entry.name == flag) return &entry;
    }
    return nullptr;
}

// A size_t-valued flag: a canonical nonnegative integer (from_chars, whole value).
[[nodiscard]] atx::core::Result<atx::usize> parse_count(std::string_view flag,
                                                        std::string_view value) {
    atx::usize parsed = 0;
    const auto [ptr, ec] = std::from_chars(value.data(), value.data() + value.size(), parsed);
    if (value.empty() || ec != std::errc{} || ptr != value.data() + value.size()) {
        return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
            "--" + std::string(flag) + " requires a nonnegative integer: got '" +
                std::string(value) + "'");
    }
    return atx::core::Ok(parsed);
}

} // namespace

atx::core::Result<bool> apply_ic_screen_option(
    atx::engine::factory::IcScreenConfig& config, std::string_view flag,
    std::string_view value) {
    using atx::core::Err;
    using atx::core::Ok;
    using atx::core::ErrorCode;
    using atx::engine::factory::IcScreenRule;
    const auto invalid = [&] {
        return Err(ErrorCode::InvalidArgument,
            "--" + std::string(flag) + ": invalid value '" + std::string(value) + "'");
    };
    if (flag == "ic-screen-rule") {
        if (value == "disabled-v1") config.rule = IcScreenRule::DisabledV1;
        else if (value == "conservative-v2") config.rule = IcScreenRule::ConservativeV2;
        else if (value == "equivalence-v3") config.rule = IcScreenRule::EquivalenceV3;
        else return invalid();
        return Ok(true);
    }
    if (flag == "ic-screen-horizons") {
        auto horizons = config.horizons;
        auto rest = value;
        for (atx::usize i = 0; i < horizons.size(); ++i) {
            const auto separator = rest.find_first_of(",;");
            const auto token = rest.substr(0, separator);
            auto parsed = parse_count(flag, token);
            if (!parsed || *parsed == 0 || *parsed > 65535) return invalid();
            if (i > 0 && *parsed <= horizons[i - 1]) return invalid();
            horizons[i] = *parsed;
            if (i + 1 == horizons.size()) {
                if (separator != std::string_view::npos) return invalid();
            } else {
                if (separator == std::string_view::npos) return invalid();
                rest.remove_prefix(separator + 1);
            }
        }
        config.horizons = horizons;
        return Ok(true);
    }
    if (flag == "ic-screen-min-abs-ic" || flag == "ic-screen-confidence") {
        double number{};
        const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), number);
        if (error != std::errc{} || end != value.data() + value.size() ||
            !std::isfinite(number)) return invalid();
        if (flag == "ic-screen-min-abs-ic") {
            if (!(number > 0.0) || number >= 1.0) return invalid();
            config.practical_abs_ic = number;
        } else {
            if (number < 3.0 || number > 20.0) return invalid();
            config.confidence_multiplier = number;
        }
        return Ok(true);
    }
    if (flag != "ic-screen-min-names" && flag != "ic-screen-min-dates" &&
        flag != "ic-screen-max-cache-mib") return Ok(false);
    auto count = parse_count(flag, value);
    if (!count) return Err(count.error());
    if (flag == "ic-screen-min-names") {
        if (*count < 3 || *count > 262144) return invalid();
        config.min_names = *count;
    } else if (flag == "ic-screen-min-dates") {
        if (*count < 8 || *count > 65536) return invalid();
        config.min_dates = *count;
    } else {
        if (*count == 0 || *count > 65536) return invalid();
        config.max_cache_bytes = static_cast<atx::u64>(*count) * 1024U * 1024U;
    }
    return Ok(true);
}

atx::core::Result<bool> parse_bool_flag_value(std::string_view flag, std::string_view value) {
    if (value.empty() || value == "true" || value == "1") return atx::core::Ok(true);
    if (value == "false" || value == "0") return atx::core::Ok(false);
    return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
        "--" + std::string(flag) + " takes true or false (or 1 / 0): got '" +
            std::string(value) + "'");
}

bool subcommand_rejects_config(std::string_view subcommand) noexcept {
    // Frozen pre-registered recipes: their allowed-flag sets deliberately exclude
    // `config` (rulings AR-9 / DR15-7), so they reject rather than merge.
    return subcommand == "equity-ic" || subcommand == "equity-universe";
}

// ---------------------------------------------------------------------------
// Worker: apply one recognized (flag, value) pair to a RunConfig.
// 'flag' must NOT have a leading "--". Returns Err(InvalidArgument) for unknown
// flags. Does NOT touch cfg.set_flags â€” that bookkeeping lives in apply_flag.
// ---------------------------------------------------------------------------
static atx::core::Result<void> apply_flag_value(RunConfig& cfg,
                                                std::string_view flag,
                                                std::string_view value) {
    using EC = atx::core::ErrorCode;

    ATX_TRY(const bool ic_handled, apply_ic_screen_option(cfg.ic_screen, flag, value));
    if (ic_handled) return atx::core::Ok();

    if (flag == "allow-unidentified-panels") {
        if (value != "true" && value != "false") {
            return atx::core::Err(EC::InvalidArgument,
                "--allow-unidentified-panels requires true or false");
        }
        cfg.allow_unidentified_panels = value == "true";
        return atx::core::Ok();
    }

    // Boolean flags (W0-I0b / I-10): the value is parsed, never ignored.
    if (const BoolFlag *entry = find_bool_flag(flag); entry != nullptr) {
        ATX_TRY(const bool parsed, parse_bool_flag_value(flag, value));
        cfg.*(entry->field) = parsed;
        return atx::core::Ok();
    }
    // String flags
    if (flag == "preparation-manifest") {
        cfg.preparation_manifest = value;
        return atx::core::Ok();
    }
    if (flag == "zip")          { cfg.zip          = value; return atx::core::Ok(); }
    if (flag == "out")          { cfg.out           = value; return atx::core::Ok(); }
    if (flag == "min-date")     { cfg.min_date      = value; return atx::core::Ok(); }
    if (flag == "segs")         { cfg.segs          = value; return atx::core::Ok(); }
    if (flag == "panel-out")    { cfg.panel_out     = value; return atx::core::Ok(); }
    if (flag == "panel-storage-rule") {
        if (value != "legacy-f64-v1" && value != "mmap-f32-v2")
            return atx::core::Err(EC::InvalidArgument,
                "--panel-storage-rule requires legacy-f64-v1 or mmap-f32-v2");
        cfg.panel_storage_rule = value;
        return atx::core::Ok();
    }
    if (flag == "ic-prereg-file" || flag == "ic-prereg-sha256") {
        if (value.empty() || value.starts_with("--"))
            return atx::core::Err(EC::InvalidArgument,
                "--" + std::string(flag) + " requires a nonempty value");
        if (flag == "ic-prereg-file") cfg.equity_ic_prereg_file = value;
        else cfg.equity_ic_prereg_sha256 = value;
        return atx::core::Ok();
    }
    if (flag == "start")        { cfg.start         = value; return atx::core::Ok(); }
    if (flag == "end")          { cfg.end           = value; return atx::core::Ok(); }
    if (flag == "allocation-rule") {
        if (value != "legacy-dense-absolute-v1" && value != "sparse-relative-v2")
            return atx::core::Err(EC::InvalidArgument,
                "--allocation-rule requires legacy-dense-absolute-v1 or sparse-relative-v2");
        cfg.equity_allocation_rule = value;
        return atx::core::Ok();
    }
    if (flag == "baseline-dir") {
        if (value.empty() || value.starts_with("--")) {
            return atx::core::Err(EC::InvalidArgument,
                "--baseline-dir requires a directory value");
        }
        cfg.equity_baseline_dir = value;
        return atx::core::Ok();
    }
    if (flag == "trial-ledger") {
        if (value.empty() || value.starts_with("--")) {
            return atx::core::Err(EC::InvalidArgument,
                "--trial-ledger requires a ledger path value");
        }
        cfg.equity_trial_ledger = value;
        return atx::core::Ok();
    }
    // W0-I0b equity-stage flags. Values are shape-checked here; membership image
    // contents, cut agreement and the terminal table are the stages' boundary checks.
    if (flag == "membership" || flag == "terminal-returns") {
        if (value.empty() || value.starts_with("--")) {
            return atx::core::Err(EC::InvalidArgument,
                "--" + std::string(flag) + " requires a path value");
        }
        if (flag == "membership") cfg.equity_membership = value;
        else cfg.equity_terminal_returns = value;
        return atx::core::Ok();
    }
    if (flag == "membership-rule") {
        if (value != "as-of-v2" && value != "year-union-v1") {
            return atx::core::Err(EC::InvalidArgument,
                "--membership-rule must be as-of-v2 or year-union-v1: got '" +
                    std::string(value) + "'");
        }
        cfg.equity_membership_rule = value;
        return atx::core::Ok();
    }
    if (flag == "ic-block-len-rule") {
        if (value != "two-horizon-v2" && value != "half-horizon-v1" &&
            value != "politis-white-v2") {
            return atx::core::Err(EC::InvalidArgument,
                "--ic-block-len-rule must be two-horizon-v2|half-horizon-v1|politis-white-v2: "
                "got '" + std::string(value) + "'");
        }
        cfg.equity_ic_block_len_rule = value;
        return atx::core::Ok();
    }
    if (flag == "min-names-per-date") {
        ATX_TRY(const auto parsed, parse_count(flag, value));
        if (parsed < 2) {
            return atx::core::Err(EC::InvalidArgument,
                "--min-names-per-date must be >= 2: got " + std::string(value));
        }
        cfg.equity_ic_min_names_per_date = parsed;
        return atx::core::Ok();
    }
    if (flag == "ic-execution-delay") {
        ATX_TRY(cfg.equity_ic_execution_delay, parse_count(flag, value));
        return atx::core::Ok();
    }
    if (flag == "si-publication-lag") {
        long parsed = 0;
        const auto [ptr, ec] =
            std::from_chars(value.data(), value.data() + value.size(), parsed);
        if (value.empty() || ec != std::errc{} || ptr != value.data() + value.size() ||
            parsed < 0) {
            return atx::core::Err(EC::InvalidArgument,
                "--si-publication-lag requires a nonnegative integer: got '" +
                    std::string(value) + "'");
        }
        cfg.si_publication_lag = parsed;
        return atx::core::Ok();
    }
    if (flag == "si-publication-lag-rule") {
        if (value != "nyse-sessions-v2" && value != "calendar-days-v1") {
            return atx::core::Err(EC::InvalidArgument,
                "--si-publication-lag-rule must be nyse-sessions-v2 or calendar-days-v1: got '" +
                    std::string(value) + "'");
        }
        cfg.si_publication_lag_rule = value;
        return atx::core::Ok();
    }
    if (flag == "universe-rule") {
        if (value != "legacy-v1" && value != "common-stock-v2")
            return atx::core::Err(EC::InvalidArgument, "--universe-rule must be legacy-v1 or common-stock-v2");
        cfg.equity_universe_rule = value;
        return atx::core::Ok();
    }
    if (flag == "instrument-types") {
        if (value.empty() || value.starts_with("--"))
            return atx::core::Err(EC::InvalidArgument, "--instrument-types requires a path");
        cfg.equity_instrument_types = value;
        return atx::core::Ok();
    }
    // Checkpoint 15 `equity-universe` flags (design §5.2): each rejects an empty or
    // `--` value exactly as --trial-ledger does; list splitting and date validation
    // belong to the stage.
    if (flag == "segments-dirs" || flag == "preparation-manifests" || flag == "rank-start" ||
        flag == "rank-end") {
        if (value.empty() || value.starts_with("--")) {
            return atx::core::Err(EC::InvalidArgument,
                "--" + std::string(flag) + " requires a value");
        }
        if (flag == "segments-dirs") cfg.equity_segments_dirs = value;
        else if (flag == "preparation-manifests") cfg.equity_preparation_manifests = value;
        else if (flag == "rank-start") cfg.equity_rank_start = value;
        else cfg.equity_rank_end = value;
        return atx::core::Ok();
    }
    // Checkpoint 16 `panel` membership-restriction flags. Only shape-free rejection
    // here (empty / a stray `--`); cut spelling, date validity and the membership
    // image itself are the panel stage's boundary checks.
    if (flag == "universe-membership" || flag == "universe-cut" ||
        flag == "universe-eval-start") {
        if (value.empty() || value.starts_with("--")) {
            return atx::core::Err(EC::InvalidArgument,
                "--" + std::string(flag) + " requires a value");
        }
        if (flag == "universe-membership") cfg.panel_universe_membership = value;
        else if (flag == "universe-cut") cfg.panel_universe_cut = value;
        else cfg.panel_universe_eval_start = value;
        return atx::core::Ok();
    }
    // R21-3 `panel --asof-field <name>=<csv_path>` (repeatable, order preserved).
    // Shape-only here: a non-empty name and path split on the FIRST '='. Identifier
    // validity, name collisions and the CSV itself are the panel stage's checks.
    if (flag == "asof-field") {
        const auto eq = value.find('=');
        if (value.starts_with("--") || eq == std::string_view::npos || eq == 0 ||
            eq + 1 >= value.size()) {
            return atx::core::Err(EC::InvalidArgument,
                "--asof-field requires <name>=<csv_path>: got '" + std::string(value) + "'");
        }
        cfg.panel_asof_fields.emplace_back(std::string(value.substr(0, eq)),
                                           std::string(value.substr(eq + 1)));
        return atx::core::Ok();
    }
    if (flag == "asof-max-stale-days") {
        atx::i64 parsed = 0;
        const auto [ptr, ec] =
            std::from_chars(value.data(), value.data() + value.size(), parsed);
        if (ec != std::errc{} || ptr != value.data() + value.size() || parsed < 0) {
            return atx::core::Err(EC::InvalidArgument,
                "--asof-max-stale-days requires a nonnegative integer day count (0 = no cap)");
        }
        cfg.panel_asof_max_stale_days = parsed;
        return atx::core::Ok();
    }
    if (flag == "evaluation-start" || flag == "evaluation-end") {
        if (value.empty() || value.starts_with("--")) {
            return atx::core::Err(EC::InvalidArgument,
                "--" + std::string(flag) + " requires a date value");
        }
        if (flag == "evaluation-start") cfg.equity_evaluation_start = value;
        else cfg.equity_evaluation_end = value;
        return atx::core::Ok();
    }
    if (flag == "max-working-bytes") {
        atx::u64 parsed = 0;
        const auto [ptr, ec] =
            std::from_chars(value.data(), value.data() + value.size(), parsed);
        if (ec != std::errc{} || ptr != value.data() + value.size() || parsed == 0) {
            return atx::core::Err(EC::InvalidArgument,
                "--max-working-bytes requires a positive integer byte count");
        }
        cfg.equity_max_working_bytes = parsed;
        return atx::core::Ok();
    }
    if (flag == "panel")        { cfg.panel         = value; return atx::core::Ok(); }
    if (flag == "alpha-out")    { cfg.alpha_out     = value; return atx::core::Ok(); }
    if (flag == "run-db")       { cfg.run_db        = value; return atx::core::Ok(); }
    if (flag == "library-dir")  { cfg.library_dir   = value; return atx::core::Ok(); }
    if (flag == "dead-alpha-lib-dir") { cfg.dead_alpha_lib_dir = value; return atx::core::Ok(); } // S1 (p9)
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
    if (flag == "short-interest") { cfg.short_interest = value; return atx::core::Ok(); } // S5-0
    if (flag == "augment-out")    { cfg.augment_out    = value; return atx::core::Ok(); } // S5-0

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

    // --risk-model (S5-0/S1): closed taxonomy {diagonal, factor}. "diagonal" is the
    // inert default (risk::RiskModelConfig{}'s own default kind).
    if (flag == "risk-model") {
        if (value != "diagonal" && value != "factor") {
            return atx::core::Err(EC::InvalidArgument,
                "--risk-model must be 'diagonal' or 'factor': got '" + std::string(value) + "'");
        }
        cfg.risk_model = value;
        return atx::core::Ok();
    }
    // --sleeve-method (S5-0/S2): closed taxonomy {erc, hrp, invvol}; ignored unless
    // --metabook is set. Validated eagerly (like --weight-transform) so a typo
    // fails fast at parse time rather than silently falling back downstream.
    if (flag == "sleeve-method") {
        if (value != "erc" && value != "hrp" && value != "invvol") {
            return atx::core::Err(EC::InvalidArgument,
                "--sleeve-method must be one of erc|hrp|invvol: got '" + std::string(value) + "'");
        }
        cfg.sleeve_method = value;
        return atx::core::Ok();
    }

    if (flag == "vwap-rule") {
        const auto rule = atx::engine::alpha::parse_vwap_rule(value);
        if (!rule) return atx::core::Err(EC::InvalidArgument,
            "--vwap-rule must be raw-daily-close-v2 or adjusted-typical-v1");
        cfg.vwap_rule = *rule;
        return atx::core::Ok();
    }

    // --adv-windows (S7-3): comma-separated list of u16 ADV windows (e.g. "5,10,20,60").
    if (flag == "adv-windows") {
        cfg.adv_windows.clear();
        std::string_view rest = value;
        if (rest.empty())
            return atx::core::Err(EC::InvalidArgument, "--adv-windows: empty value");
        while (!rest.empty()) {
            const auto comma = rest.find(',');
            const std::string_view tok = rest.substr(0, comma);
            if (tok.empty())
                return atx::core::Err(EC::InvalidArgument, "--adv-windows: empty window in list");
            unsigned long w = 0;
            auto [ptr, ec] = std::from_chars(tok.data(), tok.data() + tok.size(), w);
            if (ec != std::errc{} || ptr != tok.data() + tok.size() || w == 0 || w > 65535)
                return atx::core::Err(EC::InvalidArgument,
                    std::string("--adv-windows: invalid window '") + std::string(tok) + "' (need 1..65535)");
            cfg.adv_windows.push_back(static_cast<atx::u16>(w));
            if (comma == std::string_view::npos) break;
            rest.remove_prefix(comma + 1);
        }
        return atx::core::Ok();
    }

    // Numeric flags
    // W0-I0b / I-12: every double flag must be FINITE. from_chars accepts "nan" and
    // "inf", and a NaN silently fails every `x > 0` guard downstream (--holdout-frac nan
    // disabled the holdout). Flags whose default is +/-inf mean "off" by omission only.
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
        if (!std::isfinite(tmp)) {
            return atx::core::Err(EC::InvalidArgument,
                std::string("--") + std::string(flag) + " requires a finite value: got '"
                + std::string(value) + "'");
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
    if (flag == "cpcv-rule") {
        if (value == "observation-v1") cfg.cpcv_rule = atx::engine::eval::CpcvRule::ObservationV1;
        else if (value == "date-v2") cfg.cpcv_rule = atx::engine::eval::CpcvRule::DateV2;
        else return atx::core::Err(EC::InvalidArgument,
            "--cpcv-rule must be observation-v1 or date-v2");
        return atx::core::Ok();
    }
    if (flag == "cpcv-embargo-dates" || flag == "cpcv-max-working-bytes") {
        unsigned long long parsed = 0;
        ATX_TRY_VOID(parse_ull(parsed));
        if (parsed > static_cast<unsigned long long>(std::numeric_limits<long long>::max()) ||
            (flag == "cpcv-max-working-bytes" && parsed == 0U))
            return atx::core::Err(EC::InvalidArgument, "CPCV integer setting out of range");
        if (flag == "cpcv-embargo-dates") cfg.cpcv_embargo_dates = static_cast<atx::usize>(parsed);
        else cfg.cpcv_max_working_bytes = static_cast<atx::u64>(parsed);
        return atx::core::Ok();
    }
    if (flag == "pbo-rule") {
        if (value == "legacy-gather-v1") cfg.pbo_rule = atx::engine::eval::PboRule::LegacyGatherV1;
        else if (value == "cached-moments-v2") cfg.pbo_rule = atx::engine::eval::PboRule::CachedMomentsV2;
        else return atx::core::Err(EC::InvalidArgument,
            "--pbo-rule must be legacy-gather-v1 or cached-moments-v2");
        return atx::core::Ok();
    }
    if (flag == "robust-holdout-frac") return parse_double(cfg.robust_holdout_frac); // W4a robust-factor weak sub-universe
    if (flag == "reject-price-scale") {                                               // R2 price-scale admission gate
        ATX_TRY_VOID(parse_double(cfg.max_price_scale_corr));
        if (cfg.max_price_scale_corr <= 0.0 || cfg.max_price_scale_corr > 1.0) {
            return atx::core::Err(EC::InvalidArgument,
                "--reject-price-scale must be in (0, 1]: got " + std::string(value));
        }
        return atx::core::Ok();
    }
    if (flag == "dsr-subwindows") {                                                    // R3 intra-holdout DSR sub-windows
        long tmp = 0;
        ATX_TRY_VOID(parse_long(tmp));
        if (tmp == 1) {
            return atx::core::Err(EC::InvalidArgument,
                "--dsr-subwindows must be 0 or >= 2: value 1 is meaningless (single window == aggregate gate)");
        }
        if (tmp < 0) {
            return atx::core::Err(EC::InvalidArgument,
                "--dsr-subwindows must be 0 or >= 2: got " + std::string(value));
        }
        cfg.dsr_subwindows = static_cast<int>(tmp);
        return atx::core::Ok();
    }
    if (flag == "field-cardinality-max") {                                           // R1 typed-fields cardinality threshold
        long tmp = 0;
        ATX_TRY_VOID(parse_long(tmp));
        if (tmp < 1) {
            return atx::core::Err(EC::InvalidArgument,
                "--field-cardinality-max must be >= 1: got " + std::string(value));
        }
        cfg.field_cardinality_max = static_cast<int>(tmp);
        return atx::core::Ok();
    }
    if (flag == "winsorize-limit")   return parse_double(cfg.winsorize_limit);
    if (flag == "gross-leverage")    return parse_double(cfg.gross_leverage);
    if (flag == "min-sharpe")        return parse_double(cfg.min_sharpe);
    if (flag == "min-fitness")       return parse_double(cfg.min_fitness);
    if (flag == "max-turnover")      return parse_double(cfg.max_turnover);
    if (flag == "max-pool-corr")     return parse_double(cfg.max_pool_corr);
    if (flag == "cost-bps-admit")    return parse_double(cfg.cost_bps_admit);   // S7-2
    if (flag == "min-holding-days")  return parse_double(cfg.min_holding_days); // S7-2
    if (flag == "cost-max-turnover") return parse_double(cfg.cost_max_turnover); // S7-2
    if (flag == "turnover-penalty-slope") return parse_double(cfg.turnover_penalty_slope); // S7-1
    if (flag == "max-turnover-target")    return parse_double(cfg.max_turnover_target);    // S7-1
    if (flag == "min-viable-raw")         return parse_double(cfg.min_viable_raw);         // S7-1
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
    if (flag == "gp-risk-aversion") {
        ATX_TRY_VOID(parse_double(cfg.gp_risk_aversion));
        if (cfg.gp_risk_aversion < 0.0) {
            return atx::core::Err(EC::InvalidArgument,
                "--gp-risk-aversion must be >= 0: got " + std::string(value));
        }
        return atx::core::Ok();
    } // p9 S3
    if (flag == "gp-trade-cost-scale") {
        ATX_TRY_VOID(parse_double(cfg.gp_trade_cost_scale));
        if (cfg.gp_trade_cost_scale < 0.0) {
            return atx::core::Err(EC::InvalidArgument,
                "--gp-trade-cost-scale must be >= 0: got " + std::string(value));
        }
        return atx::core::Ok();
    } // p9 S3
    if (flag == "selection-aum")      return parse_double(cfg.selection_aum);      // S5-0 (S4)
    if (flag == "kelly-fraction")     return parse_double(cfg.kelly_fraction);     // S5-0 (p7 carry-forward)
    if (flag == "kelly-max-gross")    return parse_double(cfg.kelly_max_gross);    // S5-0 (p7 carry-forward)
    if (flag == "report-aum") {
        ATX_TRY_VOID(parse_double(cfg.report_aum));
        if (!std::isfinite(cfg.report_aum) || cfg.report_aum <= 0.0) {
            return atx::core::Err(EC::InvalidArgument,
                "--report-aum must be finite and > 0: got " + std::string(value));
        }
        return atx::core::Ok();
    }
    if (flag == "replay-execution-delay") {
        // A 0 parses here; validate_execution_delay (after every flag is seen, so
        // --allow-same-close may come later on the line) refuses it without the opt-in.
        atx::usize parsed = 0;
        const auto [ptr, ec] =
            std::from_chars(value.data(), value.data() + value.size(), parsed);
        if (ec != std::errc{} || ptr != value.data() + value.size()) {
            return atx::core::Err(EC::InvalidArgument,
                "--replay-execution-delay requires a nonnegative observation count");
        }
        cfg.replay_execution_delay = parsed;
        return atx::core::Ok();
    }
    if (flag == "replay-delisting-policy") {
        ATX_TRY(auto parsed, parse_replay_delisting_policy(value));
        cfg.replay_delisting_policy = parsed;
        return atx::core::Ok();
    }
    if (flag == "replay-day-basis") {
        int parsed = 0;
        const auto [ptr, ec] =
            std::from_chars(value.data(), value.data() + value.size(), parsed);
        if (ec != std::errc{} || ptr != value.data() + value.size() ||
            (parsed != 360 && parsed != 365)) {
            return atx::core::Err(EC::InvalidArgument,
                "--replay-day-basis requires 360 or 365 calendar days");
        }
        cfg.replay_day_basis = parsed;
        return atx::core::Ok();
    }
    if (flag == "min-dollar-adv") {
        double parsed = 0.0;
        ATX_TRY_VOID(parse_double(parsed));
        if (!std::isfinite(parsed) || parsed < 0.0) {
            return atx::core::Err(EC::InvalidArgument,
                "--min-dollar-adv requires a finite nonnegative dollar amount");
        }
        cfg.equity_min_dollar_adv = parsed;
        return atx::core::Ok();
    }
    if (flag == "dollar-adv-window") {
        atx::u64 parsed = 0;
        const auto [ptr, ec] =
            std::from_chars(value.data(), value.data() + value.size(), parsed);
        if (ec != std::errc{} || ptr != value.data() + value.size() || parsed == 0) {
            return atx::core::Err(EC::InvalidArgument,
                "--dollar-adv-window requires a positive session count");
        }
        cfg.equity_dollar_adv_window = static_cast<atx::usize>(parsed);
        return atx::core::Ok();
    }
    if (flag == "replay-trade-bps" || flag == "replay-annual-borrow-bps") {
        double parsed = 0.0;
        ATX_TRY_VOID(parse_double(parsed));
        if (!std::isfinite(parsed) || parsed < 0.0) {
            return atx::core::Err(EC::InvalidArgument,
                "--" + std::string(flag) + " requires a finite nonnegative rate");
        }
        if (flag == "replay-trade-bps") {
            cfg.replay_trade_bps = parsed;
        } else {
            cfg.replay_annual_borrow_bps = parsed;
        }
        return atx::core::Ok();
    }
    if (flag == "book-turnover-gate") {                                              // S5-1
        ATX_TRY_VOID(parse_double(cfg.book_turnover_gate));
        if (cfg.book_turnover_gate < 0.0) {
            return atx::core::Err(EC::InvalidArgument,
                "--book-turnover-gate must be >= 0: got " + std::string(value));
        }
        return atx::core::Ok();
    }
    if (flag == "participation-cap") {                                               // S5-2
        ATX_TRY_VOID(parse_double(cfg.participation_cap));
        if (cfg.participation_cap < 0.0) {
            return atx::core::Err(EC::InvalidArgument,
                "--participation-cap must be >= 0: got " + std::string(value));
        }
        return atx::core::Ok();
    }
    if (flag == "borrow-bps") {                                                      // S5-4
        ATX_TRY_VOID(parse_double(cfg.borrow_bps));
        if (cfg.borrow_bps < 0.0) {
            return atx::core::Err(EC::InvalidArgument,
                "--borrow-bps must be >= 0: got " + std::string(value));
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
// validate_membership_flags
// ---------------------------------------------------------------------------
atx::core::Status validate_membership_flags(const RunConfig& cfg) {
    const int supplied = (cfg.panel_universe_membership.empty() ? 0 : 1)
                       + (cfg.panel_universe_cut.empty() ? 0 : 1)
                       + (cfg.panel_universe_eval_start.empty() ? 0 : 1);
    if (supplied != 0 && supplied != 3) {
        return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
            "--universe-membership, --universe-cut and --universe-eval-start must be "
            "supplied together");
    }
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

        // Boolean flags (kBoolFlags): valueless means true; an immediately following
        // boolean literal (true / false / 1 / 0) is consumed as the value (I-10), so
        // `--metabook false` is off and `--compact-universe 1` keeps working.
        if (find_bool_flag(flag) != nullptr) {
            std::string_view bool_value;
            if (i + 1 < argc) {
                const std::string_view next{argv[i + 1]};
                if (is_bool_literal(next)) {
                    bool_value = next;
                    ++i;
                }
            }
            auto r = apply_flag(cfg, flag, bool_value);
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

    // With --config the cross-flag pass runs after the file merge instead (dispatch),
    // so an opt-in the file supplies (allow-same-close=true) counts for a CLI
    // --replay-execution-delay 0. The merged result meets the same rules.
    if (cfg.config_file.empty()) ATX_TRY_VOID(validate_cross_flags(cfg));
    return atx::core::Ok(cfg);
}

// ---------------------------------------------------------------------------
// validate_execution_delay / validate_cross_flags
// ---------------------------------------------------------------------------
atx::core::Status validate_execution_delay(const RunConfig& cfg) {
    if (cfg.allow_same_close) return atx::core::Ok();
    if (cfg.replay_execution_delay < 1) {
        return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
            "--replay-execution-delay 0 fills at the signal's own close; pass "
            "--allow-same-close to request that explicitly");
    }
    if (cfg.equity_ic_execution_delay < 1) {
        return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
            "--ic-execution-delay 0 evaluates from the signal's own close; pass "
            "--allow-same-close to request that explicitly");
    }
    return atx::core::Ok();
}

atx::core::Status validate_ic_prereg_flags(const RunConfig& cfg) {
    using EC = atx::core::ErrorCode;
    const bool supplied = !cfg.equity_ic_prereg_file.empty();
    if (supplied != !cfg.equity_ic_prereg_sha256.empty())
        return atx::core::Err(EC::InvalidArgument,
            "--ic-prereg-file and --ic-prereg-sha256 must be supplied together");
    if (!supplied) return atx::core::Ok();
    if (cfg.subcommand != "equity-ic")
        return atx::core::Err(EC::InvalidArgument, "IC pre-registration is only valid for equity-ic");
    if (cfg.equity_ic_prereg_sha256.size() != 64 ||
        !std::all_of(cfg.equity_ic_prereg_sha256.begin(), cfg.equity_ic_prereg_sha256.end(),
            [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); }))
        return atx::core::Err(EC::InvalidArgument, "IC pre-registration SHA256 must be 64 lowercase hex digits");
    return atx::core::Ok();
}

atx::core::Status validate_cross_flags(const RunConfig& cfg) {
    using EC = atx::core::ErrorCode;
    // --resume requires --run-db.
    if (cfg.resume && cfg.run_db.empty()) {
        return atx::core::Err(EC::InvalidArgument, "--resume requires --run-db");
    }
    // The panel membership restriction is all-three-or-none. A partial set would
    // silently pick a cut or a window the operator never named.
    ATX_TRY_VOID(validate_membership_flags(cfg));
    ATX_TRY_VOID(validate_ic_prereg_flags(cfg));
    ATX_TRY_VOID(validate_execution_delay(cfg));
    if (cfg.si_publication_lag < 0) {
        return atx::core::Err(EC::InvalidArgument, "--si-publication-lag must be >= 0");
    }
    return atx::core::Ok();
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

        // dispatch merges exactly one file; a nested reference would be ignored.
        if (flag == "config") {
            return atx::core::Err(EC::ParseError,
                path + ":" + std::to_string(lineno) + ": a config file cannot name another "
                "config file");
        }

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
