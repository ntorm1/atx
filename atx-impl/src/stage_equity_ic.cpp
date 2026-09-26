#include "stage_equity_ic.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <limits>
#include <optional>
#include <set>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "artifacts.hpp"
#include "atx/core/sha256.hpp"
#include "atx/engine/data/orats_history.hpp"
#include "atx/engine/eval/cross_section_ic.hpp"
#include "atx/engine/eval/trial_epoch.hpp"
#include "equity_allocation.hpp"
#include "equity_baseline_views.hpp"
#include "panel_artifact.hpp"
#include "prereg.hpp"
#include "stage_data_provenance.hpp"
#include "trial_ledger.hpp"

namespace atx::impl {
namespace {
namespace fs = std::filesystem;
namespace eval = atx::engine::eval;
using Json = nlohmann::json;
using atx::core::Err;
using atx::core::ErrorCode;
using atx::core::Ok;
using atx::core::Result;
using atx::core::Status;

constexpr atx::u64 kOverheadReserve = 512'000'000;
constexpr std::string_view kDomain = "atx-equity-ic-v1\n";
constexpr std::string_view kDefaultTrialLedger = "atx-engine/reviews/trial-ledger.jsonl";

// §4.5's `design-note` parent, and §11.9 ruling I-2: the digest is EMBEDDED, not
// read at runtime. The executable cannot reliably locate the repository, §7.1's
// allowed-flag set is frozen, and a runtime read would make the synthetic stage
// tests depend on the design note. `iteration14_run_equity_ic.py` closes the loop
// by refusing the run unless the on-disk note hashes to exactly this value, so a
// later edit to the design breaks that preflight BY DESIGN: a changed design is a
// new freeze and must be re-embedded deliberately.
constexpr std::string_view kDesignNoteRelativePath =
    "atx-engine/reviews/2026-09-20-iteration14-cross-section-ic-design.md";
constexpr std::string_view kDesignNoteSha256 =
    "888c726b123c02b39636876a667cfab890b4304794f6be42d46d35befbedbe25";
// §11.9 I-1/I-3: no in-process peak-working-set helper exists anywhere in this
// repository, so the producer measures wall time only and says who measures the
// peak instead of publishing a bare null.
constexpr std::string_view kPeakWorkingSetSource =
    "measured by build-equity/audits/iteration14_run_equity_ic.py at 100 ms sampling; the "
    "producer measures wall time only";

// The source-reconciliation audit sits beside the baseline directory in the data
// root (handoff, "Source reconciliation"). The subcommand takes no flag for it
// (ruling AR-9 froze the allowed set), so the directory NAME is the frozen part
// and the location is derived from --baseline-dir's parent.
constexpr std::string_view kRequiredMarkAuditDir = "equity_source_reconciliation_2013_20260919";

// §4.2 / §4.3 — frozen before any run. Changing any of these is a NEW trial.
// W0-I0b / E-18: the minimum names per date is a parameter (RunConfig, default 50);
// checkpoint 14 froze 2, which admitted two-name "cross-sections".
constexpr std::array<atx::usize, 5> kHorizons{1, 5, 10, 21, 63};
constexpr atx::usize kQuantiles = 10;
constexpr atx::usize kBootstrapDraws = 2000;
constexpr atx::usize kBlockLenFloor = 5;
constexpr atx::u64 kBootstrapSeed = 20260920;
constexpr atx::i64 kDayBasis = 365;
constexpr atx::f64 kShortLegGross = 1.0;
// Ruling AR-10: a COMPILE-TIME reference to atx-impl's own allocator header.
// equity_allocation.hpp is included, never modified.
constexpr atx::f64 kTradeBps = EquityAllocationConfig{}.trade_bps;
// Ruling AR-10: the deployed equity-book run configuration (handoff, "Current
// working book"), NOT replay.hpp — whose annual_borrow_bps default is 0.0.
constexpr atx::f64 kAnnualBorrowBps = 365.0;
constexpr atx::i64 kCheckpoint = 22;
constexpr atx::usize kRequiredMarkIdCount = 34; // §2.3 partition 3 + 2 + 29
constexpr atx::i64 kPcsSecurityId = 146189;     // §2.3, flagged terminal WITHOUT evidence

constexpr std::array<std::string_view, 2> kVariantNames{"DropMissingForward",
                                                        "IncludeAuditedTerminalV1"};
constexpr std::array<std::string_view, 2> kRestrictionNames{"full", "ex34"};
// Since checkpoint 19 the run declares the REAL count of NEW configurations:
// (families - retained) x horizons x variants x restrictions. Retained families
// (the 26 declared at checkpoints 17-21) and the checkpoint-14 signals
// (momentum_252, momentum_126, blend_equal) are re-measurements, not new trials.
constexpr atx::i64 kTrialCountDeclared =
    static_cast<atx::i64>((kEquityFamilyDsl.size() - kEquityFamilyRetainedCount) * kHorizons.size() *
                          kVariantNames.size() * kRestrictionNames.size());
constexpr std::string_view kTrialIdPrefix = "iteration22-cross-section-ic-";
constexpr std::string_view kTrialCountRule =
    "(families - retained) x horizons x variants x restrictions = (29 - 26) x 5 x 2 x 2; the 26 "
    "retained families (checkpoints 17-21) and momentum_252 / momentum_126 / blend_equal "
    "(checkpoint 14) are RE-MEASUREMENTS (on the qa-v2 remediated 2016-2018 sessions per R21-4), "
    "not new trials; the 3 new configurations are short_interest_ratio, days_to_cover_21 and "
    "d_sir_63 (FINRA short interest, R22-1); year x cut cells are AR-7 restrictions (R16-2)";
static_assert(kEquityFamilyRetainedCount < kEquityFamilyDsl.size());

// W0-I0b / E-09: the alignment label follows the execution delay actually used.
// Delay 0 is the pre-W0 label verbatim (the return was indexed from the signal close
// while the deployed book executes one session later).
constexpr std::string_view kAlignmentDelay0 =
    "signal-at-t-return-from-t-deployed-book-executes-at-t-plus-1";
std::string alignment_label(atx::usize delay) {
    if (delay == 0) return std::string(kAlignmentDelay0);
    return "signal-at-t-return-from-entry-close-t-plus-" + std::to_string(delay) +
           "-deployed-book-executes-at-t-plus-1";
}
constexpr std::string_view kNaiveTValidity = "invalid-under-overlapping-horizons";
constexpr std::string_view kBorrowDayConvention = "calendar_days_from_session_keys";
constexpr std::string_view kTurnoverSource = "rho_rank";
constexpr std::string_view kTurnoverInterpretation =
    "practitioner-rule-of-thumb-not-an-accounting-identity-not-comparable-to-realized-turnover";
// §3.12, mandatory in every schema that carries a net spread.
constexpr std::string_view kCostModelProvenance =
    "trade_bps=constexpr EquityAllocationConfig{}.trade_bps (equity_allocation.hpp:40); "
    "annual_borrow_bps=365 literal from the deployed equity-book run config (handoff), "
    "NOT replay.hpp whose default is 0.0; no call into replay.cpp borrow_charge, which is "
    "file-local";
// §7.3 / ruling NEW-4 — carried verbatim as quantile_spread.csv's comment row.
constexpr std::string_view kTurnoverScaleNote =
    "decile_one_way_turnover is measured at the gross-2.0 book's weights (+/-1.0/n, ruling "
    "AR-11), so it ranges over [0, 2] and a complete decile turnover reads 2.0, not 1.0. It is "
    "not comparable to signal_autocorr.csv's implied_one_way_turnover.";
// §10.5 — verbatim, in ic_summary.json and manifest.json.
constexpr std::string_view kSignAndShapeStatement =
    "A 189-observation result is sign-and-shape evidence only. It is not accepted alpha, not a "
    "Sharpe, not evidence of trading capacity, and not grounds for selecting a signal, a "
    "horizon, a cadence or a threshold. Research D-1: \"189 dates x roughly 200-400 admitted "
    "names is enough for a decay *shape* and a coverage table, and enough to expose a zero or "
    "negative IC - but it is a thin sample for an ICIR. Report the bootstrap interval, never a "
    "bare point estimate, and select nothing on it.\"";

// §2.3 evidence table. The audit names exactly three events with consideration
// amounts and calls them a terminal-cash-event HYPOTHESIS; PCS is never here.
// Since W0-I0b (I-15) this is the FROZEN DEFAULT of the terminal-return table
// (equity_ic_frozen_terminal_table); a --terminal-returns table replaces it.
struct TerminalEvent {
    atx::i64 security_id;
    std::string_view ticker;
    atx::f64 consideration;
    atx::f64 special_dividend; // 0.0 unless record_date is non-empty (ruling AR-2)
    std::string_view record_date;
    std::string_view last_observation;
    atx::f64 last_raw_close;
};
constexpr std::array<TerminalEvent, 3> kTerminalEvents{
    {{37648, "HNZ", 72.50, 0.0, "", "2013-06-07", 72.49},
     {35715, "DELL", 13.75, 0.13, "2013-10-28", "2013-10-29", 13.86},
     {39970, "MOLX", 38.68, 0.0, "", "2013-12-06", 38.68}}};
constexpr std::array<atx::i64, 2> kEvidencedNonTerminalIds{150340, 351548};
constexpr std::string_view kAuditSource =
    "atx-engine/reviews/2026-09-20-equity-required-marks-audit.md:13-15";
constexpr std::string_view kFrozenTerminalTableSource = "frozen-checkpoint14-2013-audit-table";

// ---------------------------------------------------------------------------
//  Formatting and small IO helpers, restated TU-locally exactly as
//  stage_equity_baseline.cpp:41-45 / :186-210 do (both are file-local statics
//  there; the design copies the idiom rather than sharing a helper).
// ---------------------------------------------------------------------------
template <class Number> std::string number(Number value) {
    if constexpr (std::is_floating_point_v<Number>) {
        // §7.4: a published cell is "never `0`, never `NaN`". `std::to_chars`
        // reports success for a non-finite double and writes `inf` / `nan`, so
        // an engine regression would publish a token a reader would parse as a
        // measurement. An empty cell is the honest rendering of "no value".
        if (!std::isfinite(value)) return std::string();
    }
    std::array<char, 64> bytes{};
    const auto result = std::to_chars(bytes.data(), bytes.data() + bytes.size(), value);
    if (result.ec != std::errc{}) throw std::runtime_error("equity ic: numeric formatting failed");
    return {bytes.data(), result.ptr};
}

Result<atx::u64> add(atx::u64 a, atx::u64 b) {
    if (b > std::numeric_limits<atx::u64>::max() - a) {
        return Err(ErrorCode::OutOfRange, "equity ic: memory estimate overflow");
    }
    return Ok(a + b);
}

Result<atx::u64> multiply(atx::u64 a, atx::u64 b) {
    if (a != 0 && b > std::numeric_limits<atx::u64>::max() / a) {
        return Err(ErrorCode::OutOfRange, "equity ic: memory estimate overflow");
    }
    return Ok(a * b);
}

// A CSV cell carrying a comma or a quote is double-quoted with doubled quotes.
std::string csv(std::string_view text) {
    if (text.find_first_of(",\"\n") == std::string_view::npos) return std::string(text);
    std::string out = "\"";
    for (const char c : text) {
        if (c == '"') out += '"';
        out += c;
    }
    out += '"';
    return out;
}

Status reserve_directory(const fs::path &directory) {
    std::error_code ec;
    if (!directory.parent_path().empty()) fs::create_directories(directory.parent_path(), ec);
    if (ec || !fs::create_directory(directory, ec) || ec) {
        return Err(ErrorCode::AlreadyExists, "equity ic: output root must not exist");
    }
    if (!fs::create_directory(directory / ".pending", ec) || ec) {
        return Err(ErrorCode::IoError, "equity ic: cannot reserve publication");
    }
    return Ok();
}

Result<Json> write_text(const fs::path &directory, const std::string &name, std::string_view text) {
    const auto partial = directory / (name + ".partial");
    const auto final_path = directory / name;
    std::ofstream out(partial, std::ios::binary);
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    out.close();
    if (!out) return Err(ErrorCode::IoError, "equity ic: cannot write " + name);
    ATX_TRY(auto sha, atx::core::sha256_hex(text));
    std::error_code ec;
    fs::create_hard_link(partial, final_path, ec);
    if (ec) return Err(ErrorCode::IoError, "equity ic: cannot publish " + name);
    fs::remove(partial, ec);
    if (ec) return Err(ErrorCode::IoError, "equity ic: cannot remove published partial");
    return Ok(Json{{"filename", name}, {"sha256", sha}, {"size_bytes", number(text.size())}});
}

Result<std::string> read_file(const fs::path &path, atx::u64 max_bytes) {
    std::error_code ec;
    const auto length = fs::file_size(path, ec);
    if (ec || length == 0 || length > max_bytes) {
        return Err(ErrorCode::IoError, "equity ic: invalid companion size: " + path.string());
    }
    std::ifstream in(path, std::ios::binary);
    std::string text(static_cast<atx::usize>(length), '\0');
    if (!in.read(text.data(), static_cast<std::streamsize>(length)) ||
        in.peek() != std::char_traits<char>::eof()) {
        return Err(ErrorCode::IoError, "equity ic: cannot read " + path.string());
    }
    return Ok(std::move(text));
}

Json strict_json(const std::string &text) {
    std::vector<std::set<std::string>> keys;
    auto callback = [&](int depth, Json::parse_event_t event, Json &parsed) {
        if (depth > 64) throw std::invalid_argument("equity ic: JSON nesting exceeds 64");
        if (event == Json::parse_event_t::object_start) keys.emplace_back();
        if (event == Json::parse_event_t::key &&
            !keys.back().insert(parsed.get<std::string>()).second) {
            throw std::invalid_argument("equity ic: duplicate JSON key");
        }
        if (event == Json::parse_event_t::object_end) keys.pop_back();
        return true;
    };
    return Json::parse(text, callback);
}

Result<atx::i64> session_number(const Json &value) {
    const auto text = value.get<std::string>();
    atx::i64 result{};
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), result);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() ||
        number(result) != text) {
        return Err(ErrorCode::ParseError, "equity ic: noncanonical integer");
    }
    return Ok(result);
}

atx::f64 elapsed_seconds(std::chrono::steady_clock::time_point started) {
    const std::chrono::duration<atx::f64> span = std::chrono::steady_clock::now() - started;
    return span.count();
}

std::string utc_now() {
    const auto now = std::chrono::system_clock::now();
    const std::time_t tt = std::chrono::system_clock::to_time_t(now);
    std::tm gm{};
#if defined(_WIN32)
    gmtime_s(&gm, &tt);
#else
    gmtime_r(&tt, &gm);
#endif
    std::array<char, 32> buffer{};
    std::strftime(buffer.data(), buffer.size(), "%Y-%m-%dT%H:%M:%SZ", &gm);
    return std::string(buffer.data());
}

// §3.10's frozen five-code enum (ruling A-3). One spelling, used by every writer.
std::string_view reason_text(atx::u8 code) {
    switch (code) {
    case 0: return "";
    case 1: return "series-shorter-than-twenty";
    case 2: return "common-prefix-gap";
    case 3: return "series-too-short-for-block-length";
    case 4: return "bootstrap-draws-zero";
    case 5: return "zero-or-nonfinite-long-run-variance"; // HAC-only (W0-E0a)
    default: return "unknown";
    }
}

// ---------------------------------------------------------------------------
//  Profile — validated arguments plus the frozen §4 recipe.
// ---------------------------------------------------------------------------
// W0-I0b: the evaluation knobs a run actually used (E-18, E-09, E-02), carried
// together so the recipe, the engine config and the ledger never disagree.
struct IcRules {
    atx::usize min_names_per_date{50};
    atx::usize execution_delay{1};
    eval::BlockLenRule block_len_rule{eval::BlockLenRule::TwoHorizonV2};
    std::string block_len_rule_name{"two-horizon-v2"};
    std::string block_len_rule_text{"max(5, 2h)"};
    std::vector<atx::i64> block_lens; // on the frozen horizons; empty for PolitisWhiteV2
};

Result<IcRules> ic_rules(const RunConfig &cfg) {
    IcRules rules;
    rules.min_names_per_date = cfg.equity_ic_min_names_per_date;
    if (rules.min_names_per_date < 2) {
        return Err(ErrorCode::InvalidArgument, "equity ic: --min-names-per-date must be >= 2");
    }
    rules.execution_delay = cfg.equity_ic_execution_delay;
    if (rules.execution_delay < 1 && !cfg.allow_same_close) {
        return Err(ErrorCode::InvalidArgument,
                   "equity ic: --ic-execution-delay 0 evaluates from the signal's own close; "
                   "pass --allow-same-close to request it explicitly");
    }
    if (rules.execution_delay > eval::kMaxIcExecutionDelay) {
        return Err(ErrorCode::InvalidArgument, "equity ic: --ic-execution-delay is too large");
    }
    rules.block_len_rule_name = cfg.equity_ic_block_len_rule;
    if (rules.block_len_rule_name == "two-horizon-v2") {
        rules.block_len_rule = eval::BlockLenRule::TwoHorizonV2;
        rules.block_len_rule_text = "max(5, 2h)";
    } else if (rules.block_len_rule_name == "half-horizon-v1") {
        rules.block_len_rule = eval::BlockLenRule::HalfHorizonV1;
        rules.block_len_rule_text = "max(5, ceil(h/2))";
    } else if (rules.block_len_rule_name == "politis-white-v2") {
        rules.block_len_rule = eval::BlockLenRule::PolitisWhiteV2;
        rules.block_len_rule_text = "max(5, 2h, ceil(b_CB)) per horizon IC series";
    } else {
        return Err(ErrorCode::InvalidArgument,
                   "equity ic: unknown --ic-block-len-rule " + rules.block_len_rule_name);
    }
    if (rules.block_len_rule != eval::BlockLenRule::PolitisWhiteV2) {
        for (const auto h : kHorizons) {
            rules.block_lens.push_back(static_cast<atx::i64>(
                eval::detail::block_len_for_rule(rules.block_len_rule, h, kBlockLenFloor)));
        }
    }
    return Ok(std::move(rules));
}

struct Profile {
    EquityBaselineConfig views;
    std::string ledger_path;
    IcRules rules;
    EquityTerminalReturnTable terminal;
    Json recipe;
    std::optional<EquityIcPrereg> prereg;
    bool epoch_accounting{};
    std::vector<std::string_view> family_dsl;
    std::vector<std::string_view> family_names;
    std::vector<atx::usize> horizons{kHorizons.begin(), kHorizons.end()};

    atx::i64 checkpoint() const { return prereg ? prereg->checkpoint : kCheckpoint; }
    atx::i64 ledger_count() const { return prereg ? prereg->measured_n : kTrialCountDeclared; }
    atx::usize retained_count() const {
        return prereg ? prereg->retained_count : kEquityFamilyRetainedCount;
    }
    std::string_view count_rule() const {
        return prereg ? "runtime-v1: every measured family/horizon times 2 variants times 2 "
                        "restrictions; unverified retained declarations do not reduce ledger N"
                      : kTrialCountRule;
    }
};

Json frozen_recipe(const RunConfig &cfg, const IcRules &rules,
                   const EquityTerminalReturnTable &terminal, const std::string &executable) {
    Json signals = Json::array();
    for (atx::usize a = 0; a < kEquityBaselineDsl.size(); ++a) {
        signals.push_back(Json{{"name", kEquityBaselineSignalNames[a]},
                               {"dsl", kEquityBaselineDsl[a]}});
    }
    signals.push_back(Json{{"name", "blend_equal"},
                           {"dsl", "published combo.bin alpha column (rank-space blend)"}});
    for (atx::usize a = 0; a < kEquityFamilyDsl.size(); ++a) {
        signals.push_back(Json{{"name", kEquityFamilySignalNames[a]},
                               {"dsl", kEquityFamilyDsl[a]}, {"family", true},
                               {"retained", a < kEquityFamilyRetainedCount}});
    }
    return Json{{"profile", "cross-section-ic-training-2013-v1"},
                {"checkpoint", kCheckpoint},
                {"trial_count_rule", kTrialCountRule},
                {"trial_purpose", "training-only-forecast-evaluation"},
                {"signals", signals},
                {"horizons", Json::array({1, 5, 10, 21, 63})},
                {"quantiles", kQuantiles},
                {"min_names_per_date", rules.min_names_per_date},
                {"execution_delay", rules.execution_delay},
                {"bootstrap", {{"draws", kBootstrapDraws}, {"seed", kBootstrapSeed},
                               {"block_len_rule", rules.block_len_rule_text},
                               {"block_len_rule_id", rules.block_len_rule_name},
                               {"percentiles", Json::array({2.5, 97.5})},
                               {"reportable_rule", "draws>=1 && n>=20 && floor(n/L)>=10"}}},
                {"hac", {{"rule", "HansenHodrickV1"},
                         {"lag", "max(h - 1, floor(4 (n/100)^(2/9)))"},
                         {"kernel", "uniform; Bartlett fallback when S <= 0 (flagged)"},
                         {"reportable_rule", "n>=20 && n/(lag+1)>=10 && S finite > 0"}}},
                {"autocorr_lags", Json::array({1})},
                {"turnover_source", kTurnoverSource},
                {"forward_variants",
                 Json::array({kVariantNames[0], kVariantNames[1]})},
                {"restrictions", Json::array({"full", "_ex34"})},
                {"samples", Json::array({"full", "_common"})},
                {"common_sample_rule",
                 "prefix t < (T - label_embargo(max(H), delay)) = T - (max(H) + delay); "
                 "incomplete prefix => _common unreportable"},
                {"alignment", alignment_label(rules.execution_delay)},
                {"membership_rule", cfg.equity_membership_rule},
                {"terminal_table", {{"source", terminal.source}, {"sha256", terminal.sha256},
                                    {"events", terminal.events.size()}}},
                {"naive_t_validity", kNaiveTValidity},
                {"net_spread_rule",
                 "per-date net_h(t) = gross_h(t) - cost_drag_h(t); no rebalance grid"},
                {"cost", {{"trade_bps", kTradeBps}, {"annual_borrow_bps", kAnnualBorrowBps},
                          {"short_leg_gross", kShortLegGross}, {"priced_book_gross", 2.0},
                          {"decile_weights", "+1.0/n_top, -1.0/n_bottom"},
                          {"borrow_day_convention", kBorrowDayConvention},
                          {"provenance", kCostModelProvenance}}},
                {"seal", {{"policy", "RejectSealedV1"}, {"validation_begin", "2020-01-01"},
                          {"sealed_begin", "2023-01-01"}}},
                {"fit_kind", "unfit-no-fitting-performed"},
                {"fitted_observations", 0},
                {"trial_count_declared", kTrialCountDeclared},
                {"evaluation_start", cfg.equity_evaluation_start},
                {"evaluation_end_exclusive", cfg.equity_evaluation_end},
                {"warmup_observations", kEquityBaselineWarmup},
                {"session_semantics", std::string(kPanelSessionSemantics)},
                {"raw_data_economics", "unverified"},
                {"historical_availability", "unknown-archive-snapshot"},
                {"instrument_type_eligibility", "unknown"},
                {"investment_qualification", "unverified-no-promotion"},
                {"live_orders_authorized", false},
                {"max_working_bytes", number(cfg.equity_max_working_bytes)},
                {"producer_executable_sha256", executable.empty() ? "unknown" : executable}};
}

Result<Profile> resolve(const RunConfig &cfg) {
    ATX_TRY_VOID(validate_ic_prereg_flags(cfg));
    ATX_TRY_VOID(validate_ic_epoch_flags(cfg));
    // Ruling AR-9: `config` is deliberately ABSENT — equity-ic accepts no config file.
    // W0-I0b adds the evaluation knobs (E-18, E-09, E-02), membership (D-12) and the
    // terminal-return table (I-15); each is recorded in the recipe.
    const std::set<std::string> allowed{"panel", "baseline-dir", "out", "evaluation-start",
        "evaluation-end", "max-working-bytes", "trial-ledger", "quiet", "digest-only",
        "min-names-per-date", "ic-execution-delay", "ic-block-len-rule", "allow-same-close",
        "membership", "membership-rule", "terminal-returns", "ic-prereg-file",
        "ic-prereg-sha256", "ic-trial-accounting-rule", "ic-epoch-catalog", "ic-epoch-anchor"};
    for (const auto &flag : cfg.set_flags) {
        if (!allowed.contains(flag)) {
            return Err(ErrorCode::InvalidArgument, "equity ic: unsupported flag --" + flag);
        }
    }
    // A programmatic caller must not silently request a different model either.
    if (cfg.allow_unidentified_panels || !cfg.config_file.empty() || cfg.cost_bps != 0 ||
        cfg.borrow_bps != 0 || cfg.gross != 0 || cfg.name_cap != 0 || !cfg.rebalance.empty() ||
        cfg.trade_rate != 1 || cfg.risk_aversion != 0 || cfg.turnover_penalty != 0 ||
        !cfg.method.empty() || cfg.conviction || cfg.kelly_fraction != 0 || cfg.sector_neutral ||
        cfg.industry_neutral || cfg.group_neutralize || cfg.risk_model != "diagonal" ||
        cfg.dead_alpha_factors || cfg.metabook || cfg.gp_trading || cfg.participation_cap != 0 ||
        cfg.book_turnover_gate != 0 || cfg.fit_begin != 0 || cfg.fit_end != 0 ||
        cfg.combine_holdout_frac != 0 || cfg.weight_transform != "rank" ||
        cfg.winsorize_limit != 0.025 || cfg.gross_leverage != 1 || !cfg.seed_exprs.empty() ||
        !cfg.library_dir.empty() || cfg.gated || cfg.walk_forward != 0 || cfg.corr_penalty != 0 ||
        cfg.capacity_floor != 0 || !cfg.combo.empty() || !cfg.books.empty()) {
        return Err(ErrorCode::InvalidArgument,
                   "equity ic: unsupported override of the frozen pre-registered recipe");
    }
    // Ruling AR-8: both evaluation dates are REQUIRED, exactly as equity-baseline
    // requires them (stage_equity_baseline.cpp:91-94).
    if (cfg.panel.empty() || cfg.equity_baseline_dir.empty() || cfg.out.empty() ||
        cfg.equity_evaluation_start.empty() || cfg.equity_evaluation_end.empty() ||
        cfg.equity_max_working_bytes <= kOverheadReserve) {
        return Err(ErrorCode::InvalidArgument,
                   "equity ic: panel, baseline-dir, fresh out, explicit evaluation dates and "
                   "adequate budget required");
    }
    using atx::engine::data::detail::date_to_nanos;
    const auto begin = date_to_nanos(cfg.equity_evaluation_start);
    const auto end = date_to_nanos(cfg.equity_evaluation_end);
    // §5.3: the same window guard stage_equity_baseline.cpp:99-100 applies.
    if (!begin || !end || *begin >= *end || *begin < *date_to_nanos("2013-04-01") ||
        *end > *date_to_nanos("2020-01-01")) {
        return Err(ErrorCode::InvalidArgument,
                   "equity ic: evaluation must be within training [2013-04-01,2020-01-01)");
    }
    Profile profile;
    profile.epoch_accounting = cfg.equity_ic_trial_accounting_rule == "epoch-e2-v1";
    profile.views.evaluation = {*begin, *end};
    profile.views.observation_basis =
        EquityBaselineObservationBasis::ArchiveRawVolumeAndPointwiseAdjustedCloseV1;
    profile.views.max_additional_bytes = cfg.equity_max_working_bytes - kOverheadReserve;
    ATX_TRY(profile.views.membership_rule,
            parse_equity_membership_rule(cfg.equity_membership_rule));
    ATX_TRY(profile.rules, ic_rules(cfg));
    if (cfg.equity_terminal_returns.empty()) {
        profile.terminal = equity_ic_frozen_terminal_table();
    } else {
        ATX_TRY(profile.terminal, load_equity_terminal_table(cfg.equity_terminal_returns));
    }
    profile.ledger_path = cfg.equity_trial_ledger.empty() ? std::string(kDefaultTrialLedger)
                                                          : cfg.equity_trial_ledger;
    if (profile.epoch_accounting) {
        const auto catalog_path = fs::weakly_canonical(cfg.equity_ic_epoch_catalog);
        if (catalog_path == fs::weakly_canonical(profile.ledger_path))
            return Err(ErrorCode::InvalidArgument, "equity ic: epoch catalog and legacy ledger must be distinct");
    }
    ATX_TRY(auto executable, current_executable_sha256());
    profile.recipe = frozen_recipe(cfg, profile.rules, profile.terminal, executable);
    if (!cfg.equity_ic_prereg_file.empty()) {
        ATX_TRY(auto declaration, load_equity_ic_prereg(cfg.equity_ic_prereg_file,
            cfg.equity_ic_prereg_sha256, profile.epoch_accounting
                ? PreregLineageRule::CatalogVerifiedCellsE2 : PreregLineageRule::LegacySameFamilyV1));
        profile.prereg = std::move(declaration);
        profile.horizons = profile.prereg->horizons;
        profile.rules.block_lens.clear();
        for (const auto h : profile.horizons) {
            if (profile.rules.block_len_rule != eval::BlockLenRule::PolitisWhiteV2) {
                profile.rules.block_lens.push_back(static_cast<atx::i64>(
                    eval::detail::block_len_for_rule(profile.rules.block_len_rule, h,
                                                     kBlockLenFloor)));
            }
        }
        auto &recipe = profile.recipe;
        recipe["profile"] = "runtime-preregistered-cross-section-ic-v1";
        recipe["checkpoint"] = profile.checkpoint();
        recipe["trial_count_declared"] = profile.ledger_count();
        recipe["trial_count_rule"] = profile.count_rule();
        recipe["horizons"] = profile.horizons;
        recipe["bootstrap"]["block_lens"] = profile.rules.block_lens;
        recipe["bootstrap"]["predicted_null_horizons"] = Json::array();
        recipe["bootstrap"]["horizon_index_scope"] =
            "index within each signal registered horizon list; reference signals use union";
        recipe["signals"].erase(recipe["signals"].begin() + 3, recipe["signals"].end());
        for (const auto &family : profile.prereg->families) {
            recipe["signals"].push_back(Json{{"name", family.name}, {"dsl", family.dsl},
                {"id", family.id}, {"sign", family.sign}, {"theme", family.theme},
                {"horizons", family.horizons}, {"retained", family.retained},
                {"configuration_sha256", family.configuration_sha256}});
        }
        recipe["preregistration"] = Json{
            {"canonical_sha256", profile.prereg->canonical_sha256},
            {"file_sha256", profile.prereg->file_sha256},
            {"declared_new_n", profile.prereg->declared_n},
            {"measured_n_charged_to_ledger", profile.prereg->measured_n},
            {"retained_lineage_status", "declared-unverified-pending-E2-reconciliation"},
            {"document", strict_json(profile.prereg->canonical_json)}};
        if (profile.epoch_accounting) {
            recipe["trial_accounting_rule"] = "epoch-e2-v1";
            recipe["preregistration"]["retained_lineage_status"] =
                "requires-exact-prior-catalog-cell-proof-before-family-VM";
        }
    }
    return Ok(std::move(profile));
}

// Restated from stage_equity_baseline.cpp:235-267 (TU-local there).
Status require_context_recipe(const std::string &text, const Profile &profile) {
    const auto recipe = strict_json(text);
    if (recipe.at("version") != "tickerhistory-panel-v2-identified" ||
        recipe.at("research_ohlc") != "raw-OHLC*cumulReturnFactor-pointwise" ||
        recipe.at("raw_close") != "unadjusted-as-traded" ||
        recipe.at("volume") != "raw-reported-volume-no-total-return-factor-rescaling" ||
        recipe.at("compact_to_universe") != true) {
        return Err(ErrorCode::InvalidArgument, "equity ic: unsupported context field recipe");
    }
    ATX_TRY(auto start, session_number(recipe.at("start_inclusive_nanos")));
    ATX_TRY(auto end, session_number(recipe.at("end_exclusive_nanos")));
    const auto training_end = *atx::engine::data::detail::date_to_nanos("2020-01-01");
    if (start >= end || start > profile.views.evaluation.begin_session_key ||
        end < profile.views.evaluation.end_exclusive_session_key || end > training_end) {
        return Err(ErrorCode::InvalidArgument,
                   "equity ic: context must prove requested end coverage and exclude sealed dates");
    }
    return Ok();
}

// ---------------------------------------------------------------------------
//  Required-mark audit (§2.3, §7.2). The audit enumerates its 110 required
//  date/ID cells but not its 34 IDs in prose, so PCS membership is INFERRED —
//  ruling "informational item" (§11.4) makes the run check that inference.
// ---------------------------------------------------------------------------
struct RequiredMarks {
    bool present{};
    std::string path;
    std::string audit_id;
    std::string manifest_sha256;
    std::set<atx::i64> ids;
    atx::usize cells{};
};

// W0-I0b / I-15: the 2013 audit is OPTIONAL. When its manifest is absent the run
// proceeds with an EMPTY ex34 restriction (declared in request/manifest); when it is
// present it is loaded and, for the frozen 2013 terminal table, checked exactly as
// before (the partition the frozen table rests on is still verified at runtime).
Result<RequiredMarks> load_required_marks(const std::string &baseline_dir,
                                          bool frozen_terminal_table) {
    fs::path base(baseline_dir);
    if (base.filename().empty()) base = base.parent_path();
    RequiredMarks marks;
    marks.path = (base.parent_path() / kRequiredMarkAuditDir / "manifest.json").string();
    std::error_code exists_ec;
    if (!fs::exists(marks.path, exists_ec) && !exists_ec) return Ok(std::move(marks));
    marks.present = true;
    ATX_TRY(auto text, read_file(marks.path, 256U * 1024U * 1024U));
    ATX_TRY(marks.manifest_sha256, atx::core::sha256_hex(text));
    const auto manifest = strict_json(text);
    marks.audit_id = manifest.at("audit_id").get<std::string>();
    for (const auto &gap : manifest.at("gaps")) {
        const auto id_text = gap.at("required_gap").at("security_id").get<std::string>();
        atx::i64 id{};
        const auto parsed = std::from_chars(id_text.data(), id_text.data() + id_text.size(), id);
        if (parsed.ec != std::errc{} || parsed.ptr != id_text.data() + id_text.size() || id <= 0) {
            return Err(ErrorCode::ParseError, "equity ic: noncanonical required-mark security id");
        }
        marks.ids.insert(id);
        ++marks.cells;
    }
    if (!frozen_terminal_table) return Ok(std::move(marks));
    // The run depends on the inference, so the run checks it (§11.4, §8 T5).
    if (marks.ids.size() != kRequiredMarkIdCount) {
        return Err(ErrorCode::InvalidArgument,
                   "equity ic: required-mark audit names " + number(marks.ids.size()) +
                       " ids, expected " + number(kRequiredMarkIdCount));
    }
    if (!marks.ids.contains(kPcsSecurityId)) {
        return Err(ErrorCode::InvalidArgument,
                   "equity ic: PCS 146189 is not in the loaded required-mark set; the "
                   "terminal-unevidenced partition rests on that membership");
    }
    for (const auto &event : kTerminalEvents) {
        if (!marks.ids.contains(event.security_id)) {
            return Err(ErrorCode::InvalidArgument,
                       "equity ic: evidenced terminal id " + number(event.security_id) +
                           " is not in the loaded required-mark set");
        }
    }
    return Ok(std::move(marks));
}

// ---------------------------------------------------------------------------
//  Per-cell engine inputs (§3.1, §3.8, §6). Every array is dates*instruments.
// ---------------------------------------------------------------------------
struct CellArrays {
    std::vector<atx::u8> mask;
    std::vector<atx::u8> terminal;
    std::vector<atx::u8> terminal_evidenced;
    std::vector<atx::f64> terminal_value;
    std::vector<atx::u8> excluded_audited;
    atx::usize terminal_columns{};
    atx::usize excluded_columns{};
};

Result<CellArrays> build_cells(const EquityBaselineEvaluation &view,
                               const std::vector<std::string> &instrument_ids,
                               const RequiredMarks &marks,
                               const EquityTerminalReturnTable &table) {
    const auto dates = view.panel.dates();
    const auto names = view.panel.instruments();
    if (instrument_ids.size() != names) {
        return Err(ErrorCode::InvalidArgument, "equity ic: instrument axis size mismatch");
    }
    ATX_TRY(auto cells, multiply(dates, names));
    ATX_TRY(auto raw_id, view.panel.field_id("raw_close"));
    const auto raw = view.panel.field_all(raw_id);
    CellArrays out;
    const auto count = static_cast<atx::usize>(cells);
    out.mask.assign(count, 0);
    out.terminal.assign(count, 0);
    out.terminal_evidenced.assign(count, 0);
    out.terminal_value.assign(count, 0.0);
    out.excluded_audited.assign(count, 0);
    for (atx::usize i = 0; i < names; ++i) {
        atx::i64 id{};
        const auto &text = instrument_ids[i];
        const auto parsed = std::from_chars(text.data(), text.data() + text.size(), id);
        if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size()) {
            return Err(ErrorCode::ParseError, "equity ic: noncanonical instrument id");
        }
        const bool audited = marks.ids.contains(id);
        const EquityTerminalEvent *event = table.find(id);
        const bool evidenced = event != nullptr && event->evidenced;
        // A return row prices off the security's LAST finite raw close in the view.
        // Precondition (checked): that close is the delisting observation, i.e. the
        // security's closes have no interior gap. The engine applies the terminal leg
        // to ANY forward gap, so after a halt that later resumes, a cell whose
        // horizon ends inside the halt would be priced off a close that comes after
        // its horizon (a look-ahead). Such a view is refused, never priced.
        atx::f64 return_base = 0.0;
        if (evidenced && event->terminal_return) {
            const auto priced = [&](atx::usize d) {
                const auto value = raw[d * names + i];
                return std::isfinite(value) && value > 0.0;
            };
            atx::usize last = dates;
            for (atx::usize d = dates; d-- > 0;) {
                if (priced(d)) {
                    last = d;
                    break;
                }
            }
            if (last == dates) {
                return Err(ErrorCode::InvalidArgument,
                           "equity ic: terminal-return row for security " + number(id) +
                               " has no finite raw close in the evaluated window");
            }
            return_base = raw[last * names + i];
            bool in_gap = false;
            for (atx::usize d = last; d-- > 0;) {
                if (!priced(d)) {
                    in_gap = true;
                } else if (in_gap) {
                    return Err(ErrorCode::InvalidArgument,
                               "equity ic: terminal-return row for security " + number(id) +
                                   " has a finite raw close after an interior gap; the last "
                                   "close must be the delisting observation (a resumed halt "
                                   "would price earlier horizons off a later close)");
                }
            }
        }
        if (audited) ++out.excluded_columns;
        if (event != nullptr) ++out.terminal_columns;
        for (atx::usize d = 0; d < dates; ++d) {
            const auto cell = d * names + i;
            out.mask[cell] = view.panel.in_universe(d, i) ? atx::u8{1} : atx::u8{0};
            out.excluded_audited[cell] = audited ? atx::u8{1} : atx::u8{0};
            if (evidenced) {
                out.terminal[cell] = 1;
                out.terminal_evidenced[cell] = 1;
                // Ruling AR-2 lives in one place: equity_terminal_cash.
                out.terminal_value[cell] =
                    equity_terminal_cash(*event, view.session_keys[d]) +
                    (event->terminal_return ? return_base * (1.0 + *event->terminal_return)
                                            : 0.0);
            } else if (event != nullptr) {
                // Ruling AR-1: flagged terminal, never evidenced, never applied.
                out.terminal[cell] = 1;
            }
        }
    }
    return Ok(std::move(out));
}

// R-A partition: every audited id that is neither an evidenced terminal event nor an
// evidenced non-terminal id (PCS, unevidenced, is inside this count). Published in
// request.json, manifest.json and the ledger entry (pre-W0 a constant 29).
[[nodiscard]] atx::i64 unclassified_mark_count(const RequiredMarks &marks,
                                               const EquityTerminalReturnTable &table) {
    atx::i64 unclassified = 0;
    for (const auto id : marks.ids) {
        const auto *event = table.find(id);
        const bool terminal = event != nullptr && event->evidenced;
        const bool non_terminal =
            std::find(kEvidencedNonTerminalIds.begin(), kEvidencedNonTerminalIds.end(), id) !=
            kEvidencedNonTerminalIds.end();
        if (!terminal && !non_terminal) ++unclassified;
    }
    return unclassified;
}

Json terminal_ids(const EquityTerminalReturnTable &table, bool evidenced) {
    Json ids = Json::array();
    for (const auto &event : table.events) {
        if (event.evidenced == evidenced) ids.push_back(event.security_id);
    }
    return ids;
}

// The evidenced rows as published evidence. Frozen rows keep the audit's ticker and
// last-observation detail (§2.3); table rows carry their own source text.
Json terminal_evidence_json(const EquityTerminalReturnTable &table) {
    Json evidence = Json::array();
    for (const auto &event : table.events) {
        if (!event.evidenced) continue;
        Json row{{"security_id", event.security_id},
                 {"consideration", event.terminal_value ? Json(*event.terminal_value)
                                                        : Json(nullptr)},
                 {"terminal_return", event.terminal_return ? Json(*event.terminal_return)
                                                           : Json(nullptr)},
                 {"special_dividend", event.special_dividend},
                 {"record_date", event.record_date}, {"source", event.source},
                 {"classification",
                  "terminal-cash-event hypothesis, not a settled classification"}};
        for (const auto &frozen : kTerminalEvents) {
            if (frozen.security_id != event.security_id) continue;
            row["ticker"] = frozen.ticker;
            row["last_observation"] = frozen.last_observation;
            row["last_raw_close"] = frozen.last_raw_close;
        }
        evidence.push_back(std::move(row));
    }
    return evidence;
}

// ---------------------------------------------------------------------------
//  JSON serializers for the engine result (§7.3, §7.4 null encoding).
// ---------------------------------------------------------------------------
// `block_reason` is the owning block's reason. An interval that is itself fine
// but whose block is closed must not publish `reportable: false` beside
// `unreportable_reason: 0` — "false with no reason" is unreadable.
Json interval_json(const eval::BootstrapInterval &iv, bool block_reportable,
                   atx::u8 block_reason) {
    const bool ok = block_reportable && iv.reportable != 0;
    const atx::u8 reason =
        (!ok && iv.unreportable_reason == 0U) ? block_reason : iv.unreportable_reason;
    Json out{{"draws", iv.draws}, {"block_len", iv.block_len}, {"blocks", iv.blocks},
             {"series_len", iv.series_len}, {"modulo_fallbacks", iv.modulo_fallbacks},
             {"reportable", ok}, {"unreportable_reason", reason},
             {"unreportable_reason_text", reason_text(reason)}};
    out["point"] = ok ? Json(iv.point) : Json(nullptr);
    out["lo"] = ok ? Json(iv.lo) : Json(nullptr);
    out["hi"] = ok ? Json(iv.hi) : Json(nullptr);
    return out;
}

// W0-E0a / E-03 HAC interval, published beside naive_t (which stays "invalid under
// overlapping horizons"). A closed block nulls it like every other statistic.
std::string_view kernel_name(eval::hac::Kernel kernel) {
    switch (kernel) {
    case eval::hac::Kernel::BartlettV1: return "bartlett";
    case eval::hac::Kernel::UniformV1: return "uniform";
    case eval::hac::Kernel::Unknown: return "unknown";
    }
    return "unknown";
}

Json hac_json(const eval::HacInterval &iv, bool block_reportable, atx::u8 block_reason) {
    const bool ok = block_reportable && iv.reportable != 0;
    const atx::u8 reason =
        (!ok && iv.unreportable_reason == 0U) ? block_reason : iv.unreportable_reason;
    const auto real = [ok](atx::f64 v) { return ok ? Json(v) : Json(nullptr); };
    return Json{{"n", iv.n}, {"lag", iv.lag}, {"kernel", kernel_name(iv.kernel)},
                {"fell_back", iv.fell_back != 0}, {"reportable", ok},
                {"unreportable_reason", reason},
                {"unreportable_reason_text", reason_text(reason)}, {"point", real(iv.point)},
                {"se", real(iv.se)}, {"t", real(iv.t)}, {"lo", real(iv.lo)}, {"hi", real(iv.hi)}};
}

// `spread_ok` is IcHorizonSummary::spread_reportable (§11.8): the spread family
// carries its own reportability, so a block whose IC series reports while its
// spread series does not nulls the spread half only.
Json sample_json(const eval::IcSampleStats &s, bool spread_ok, atx::u8 spread_reason) {
    // Ruling NEW-1: when the block is unreportable EVERY field serializes as
    // null, means included — a point estimate over an incomplete prefix is
    // exactly the number a reader would mistake for a common-sample result.
    const bool ok = s.summary_reportable != 0;
    const bool sok = ok && spread_ok;
    const auto real = [ok](atx::f64 v) { return ok ? Json(v) : Json(nullptr); };
    const auto sreal = [sok](atx::f64 v) { return sok ? Json(v) : Json(nullptr); };
    Json out{{"dates_emitted", s.dates_emitted}, {"summary_reportable", ok},
             {"unreportable_reason", s.unreportable_reason},
             {"unreportable_reason_text", reason_text(s.unreportable_reason)},
             {"common_prefix_gaps", s.common_prefix_gaps},
             {"ic_mean", real(s.ic_mean)}, {"ic_sd", real(s.ic_sd)}, {"icir", real(s.icir)},
             {"naive_t", real(s.naive_t)}, {"naive_t_validity", kNaiveTValidity},
             {"rank_ic_mean", real(s.rank_ic_mean)}, {"rank_ic_sd", real(s.rank_ic_sd)},
             {"rank_icir", real(s.rank_icir)}, {"rank_naive_t", real(s.rank_naive_t)},
             {"spread_gross_mean", sreal(s.spread_gross_mean)},
             {"spread_gross_sd", sreal(s.spread_gross_sd)},
             {"spread_net_mean", sreal(s.spread_net_mean)},
             {"spread_net_sd", sreal(s.spread_net_sd)}};
    // A closed spread gate on an otherwise reportable block carries §11.8's own
    // reason; a closed block carries the block's.
    const atx::u8 spread_block_reason = ok ? spread_reason : s.unreportable_reason;
    out["ic_mean_ci"] = interval_json(s.ic_mean_ci, ok, s.unreportable_reason);
    out["icir_ci"] = interval_json(s.icir_ci, ok, s.unreportable_reason);
    out["rank_ic_mean_ci"] = interval_json(s.rank_ic_mean_ci, ok, s.unreportable_reason);
    out["rank_icir_ci"] = interval_json(s.rank_icir_ci, ok, s.unreportable_reason);
    out["ic_mean_hac"] = hac_json(s.ic_mean_hac, ok, s.unreportable_reason);
    out["rank_ic_mean_hac"] = hac_json(s.rank_ic_mean_hac, ok, s.unreportable_reason);
    out["spread_gross_ci"] = interval_json(s.spread_gross_ci, sok, spread_block_reason);
    out["spread_net_ci"] = interval_json(s.spread_net_ci, sok, spread_block_reason);
    out["spread_reportable"] = sok;
    out["spread_unreportable_reason"] = sok ? atx::u8{0} : spread_block_reason;
    out["spread_unreportable_reason_text"] = reason_text(sok ? atx::u8{0} : spread_block_reason);
    return out;
}

// One emitted CSV real, or "" when the flag says the value must be ignored.
std::string csv_real(atx::f64 value, bool emitted) {
    return emitted ? number(value) : std::string();
}

// ---------------------------------------------------------------------------
//  Writers — one ostringstream per output, filled in the fixed §7.4 row order
//  (signal outer, then horizon, variant, restriction, ascending date).
// ---------------------------------------------------------------------------
struct Writers {
    std::ostringstream coverage;
    std::ostringstream ic;
    std::ostringstream decay;
    std::ostringstream autocorr;
    std::ostringstream spread;
    Json summary_rows = Json::array();
};

// quantile_spread.csv carries bucket rows and SPREAD rows in ONE table, so the
// column list is spelled once and every row is emitted as exactly this many
// fields — a hand-counted run of commas is how a column silently shifts.
constexpr std::array<std::string_view, 28> kSpreadColumns{
    "signal", "horizon", "variant", "restriction", "quantile", "n_dates", "mean_names",
    "mean_forward_return", "gross_mean", "gross_sd", "gross_lo", "gross_hi", "net_mean",
    "net_sd", "net_lo", "net_hi", "decile_one_way_turnover", "trade_drag", "mean_borrow_drag",
    "mean_cost_drag", "mean_days_forward", "gross_mean_common", "net_mean_common",
    "spread_reportable", "spread_unreportable_reason", "cost_model_provenance",
    "borrow_day_convention", "mean_dollar_adv"};

void write_headers(Writers &w) {
    w.coverage << "date_index,session_key_ns,signal,horizon,variant,restriction,n_eligible,"
                  "n_signal_finite,n_with_forward,n_dropped_missing_forward,n_terminal_applied,"
                  "n_terminal_unevidenced,n_excluded_audited,n_used,in_common_sample\n";
    w.ic << "date_index,session_key_ns,signal,horizon,variant,restriction,n_used,pearson_ic,"
            "rank_ic,spread_gross,spread_net,days_forward,borrow_drag,cost_drag,emitted,"
            "spread_emitted,in_common_sample\n";
    w.decay << "signal,variant,restriction,horizon,block_len,ic_mean,ic_lo,ic_hi,rank_ic_mean,"
               "rank_lo,rank_hi,reportable,ic_mean_common,ic_lo_common,ic_hi_common,"
               "rank_ic_mean_common,rank_lo_common,rank_hi_common,reportable_common\n";
    w.autocorr << "signal,lag,pairs_emitted,rho_pearson,rho_rank,implied_one_way_turnover,"
                  "turnover_source,interpretation\n";
    w.spread << "# " << kTurnoverScaleNote << '\n';
    for (atx::usize c = 0; c < kSpreadColumns.size(); ++c) {
        w.spread << (c == 0 ? "" : ",") << kSpreadColumns[c];
    }
    w.spread << '\n';
}

struct BlockKey {
    std::string_view signal;
    std::string_view variant;
    std::string_view restriction;
};

void emit_row(std::ostringstream &out, const std::array<std::string, 28> &fields) {
    for (atx::usize c = 0; c < fields.size(); ++c) out << (c == 0 ? "" : ",") << fields[c];
    out << '\n';
}

void emit_date_rows(Writers &w, const BlockKey &key, const eval::IcHorizonSummary &sum) {
    const auto horizon = number(sum.horizon);
    for (const auto &p : sum.series) {
        w.coverage << p.date << ',' << p.session_key << ',' << key.signal << ',' << horizon << ','
                   << key.variant << ',' << key.restriction << ',' << p.n_eligible << ','
                   << p.n_signal_finite << ',' << p.n_with_forward << ','
                   << p.n_dropped_missing_forward << ',' << p.n_terminal_applied << ','
                   << p.n_terminal_unevidenced << ',' << p.n_excluded_audited << ',' << p.n_used
                   << ',' << static_cast<int>(p.in_common_sample) << '\n';
        w.ic << p.date << ',' << p.session_key << ',' << key.signal << ',' << horizon << ','
             << key.variant << ',' << key.restriction << ',' << p.n_used << ','
             << csv_real(p.pearson_ic, p.emitted != 0) << ','
             << csv_real(p.rank_ic, p.emitted != 0) << ','
             << csv_real(p.spread_gross, p.spread_emitted != 0) << ','
             << csv_real(p.spread_net, p.spread_emitted != 0) << ',' << p.days_forward << ','
             << number(p.borrow_drag) << ',' << number(p.cost_drag) << ','
             << static_cast<int>(p.emitted) << ',' << static_cast<int>(p.spread_emitted) << ','
             << static_cast<int>(p.in_common_sample) << '\n';
    }
}

void emit_decay_row(Writers &w, const BlockKey &key, const eval::IcHorizonSummary &sum) {
    const auto block = [](const eval::IcSampleStats &s, std::ostringstream &out) {
        const bool ok = s.summary_reportable != 0;
        const bool ic_ok = ok && s.ic_mean_ci.reportable != 0;
        const bool rank_ok = ok && s.rank_ic_mean_ci.reportable != 0;
        out << csv_real(s.ic_mean, ok) << ',' << csv_real(s.ic_mean_ci.lo, ic_ok) << ','
            << csv_real(s.ic_mean_ci.hi, ic_ok) << ',' << csv_real(s.rank_ic_mean, ok) << ','
            << csv_real(s.rank_ic_mean_ci.lo, rank_ok) << ','
            << csv_real(s.rank_ic_mean_ci.hi, rank_ok) << ',' << (ic_ok ? 1 : 0);
    };
    w.decay << key.signal << ',' << key.variant << ',' << key.restriction << ',' << sum.horizon
            << ',' << sum.block_len << ',';
    block(sum.full, w.decay);
    w.decay << ',';
    block(sum.common, w.decay);
    w.decay << '\n';
}

void emit_spread_rows(Writers &w, const BlockKey &key, const eval::IcHorizonSummary &sum) {
    std::array<std::string, 28> fields{};
    fields[0] = std::string(key.signal);
    fields[1] = number(sum.horizon);
    fields[2] = std::string(key.variant);
    fields[3] = std::string(key.restriction);
    for (const auto &bucket : sum.buckets) {
        auto row = fields;
        row[4] = number(bucket.quantile);
        row[5] = number(bucket.n_dates);
        row[6] = number(bucket.mean_names);
        row[7] = number(bucket.mean_forward_return);
        row[27] = bucket.n_aux_dates > 0U ? number(bucket.mean_aux) : std::string{};
        emit_row(w.spread, row);
    }
    // §11.8: the spread family carries its OWN reportability, so a horizon whose
    // IC series reports while its spread series does not is visible as such
    // instead of publishing a spread mean a reader would trust.
    const bool spread_ok = sum.spread_reportable != 0;
    const auto &full = sum.full;
    const auto &common = sum.common;
    const bool ok = full.summary_reportable != 0 && spread_ok;
    const bool gross_ok = ok && full.spread_gross_ci.reportable != 0;
    const bool net_ok = ok && full.spread_net_ci.reportable != 0;
    const bool common_ok = common.summary_reportable != 0 && spread_ok;
    auto row = fields;
    row[4] = "SPREAD";
    row[8] = csv_real(full.spread_gross_mean, ok);
    row[9] = csv_real(full.spread_gross_sd, ok);
    row[10] = csv_real(full.spread_gross_ci.lo, gross_ok);
    row[11] = csv_real(full.spread_gross_ci.hi, gross_ok);
    row[12] = csv_real(full.spread_net_mean, ok);
    row[13] = csv_real(full.spread_net_sd, ok);
    row[14] = csv_real(full.spread_net_ci.lo, net_ok);
    row[15] = csv_real(full.spread_net_ci.hi, net_ok);
    row[16] = number(sum.decile_one_way_turnover);
    row[17] = number(sum.trade_drag);
    row[18] = number(sum.mean_borrow_drag);
    row[19] = number(sum.mean_cost_drag);
    row[20] = number(sum.mean_days_forward);
    row[21] = csv_real(common.spread_gross_mean, common_ok);
    row[22] = csv_real(common.spread_net_mean, common_ok);
    row[23] = spread_ok ? "1" : "0";
    row[24] = number(static_cast<unsigned>(sum.spread_unreportable_reason));
    row[25] = csv(kCostModelProvenance);
    row[26] = std::string(kBorrowDayConvention);
    emit_row(w.spread, row);
}

void emit_summary_row(Writers &w, const BlockKey &key, const eval::IcHorizonSummary &sum) {
    w.summary_rows.push_back(
        Json{{"signal", key.signal}, {"horizon", sum.horizon}, {"variant", key.variant},
             {"restriction", key.restriction}, {"block_len", sum.block_len},
             {"block_len_rule_id", static_cast<unsigned>(sum.block_len_rule)},
             {"execution_delay", sum.execution_delay}, {"embargo", sum.embargo},
             {"dates_below_min_names", sum.dates_below_min_names},
             {"dates_below_quantile_count", sum.dates_below_quantile_count},
             {"spread_reportable", sum.spread_reportable != 0},
             {"spread_unreportable_reason", sum.spread_unreportable_reason},
             {"spread_unreportable_reason_text", reason_text(sum.spread_unreportable_reason)},
             {"decile_one_way_turnover", sum.decile_one_way_turnover},
             {"decile_one_way_turnover_scale", kTurnoverScaleNote},
             {"trade_drag", sum.trade_drag}, {"mean_borrow_drag", sum.mean_borrow_drag},
             {"mean_cost_drag", sum.mean_cost_drag}, {"mean_days_forward", sum.mean_days_forward},
             {"full", sample_json(sum.full, sum.spread_reportable != 0,
                                  sum.spread_unreportable_reason)},
             {"common", sample_json(sum.common, sum.spread_reportable != 0,
                                    sum.spread_unreportable_reason)}});
}

void emit_autocorr_row(Writers &w, std::string_view signal, const eval::AutocorrSummary &a) {
    w.autocorr << signal << ',' << a.lag << ',' << a.pairs_emitted << ',' << number(a.rho_pearson)
               << ',' << number(a.rho_rank) << ',' << number(a.implied_one_way_turnover) << ','
               << kTurnoverSource << ',' << kTurnoverInterpretation << '\n';
}

// ---------------------------------------------------------------------------
//  Trial ledger (§4.5). Two lines per run, never one.
// ---------------------------------------------------------------------------
TrialLedgerEntry base_entry(const RunConfig &cfg, const Profile &profile,
                            const RequiredMarks &marks, const PanelArtifact &context,
                            const PanelArtifact &evaluation, const PanelArtifact &combo,
                            const std::string &trial_id, atx::usize observations) {
    TrialLedgerEntry entry;
    entry.trial_id = trial_id;
    entry.appended_utc = utc_now();
    entry.checkpoint = profile.checkpoint();
    entry.purpose = "training-only-forecast-evaluation";
    entry.trial_count_declared = profile.ledger_count();
    // §4.5's four parents, plus the required-mark audit the §7.2 assertions bind
    // to. `design-note` carries the digest, never an artifact id (§11.9 I-2).
    entry.parents = {{"source-context", context.artifact_id, ""},
                     {"baseline-evaluation", evaluation.artifact_id, ""},
                     {"baseline-combo", combo.artifact_id, ""},
                     {"design-note", "", std::string(kDesignNoteSha256)}};
    // I-15: the audit is optional; an absent audit is not a parent.
    if (marks.present) entry.parents.push_back({"required-mark-audit", marks.audit_id, ""});
    if (!profile.terminal.sha256.empty()) {
        entry.parents.push_back({"terminal-return-table", "", profile.terminal.sha256});
    }
    for (atx::usize a = 0; a < kEquityBaselineDsl.size(); ++a) {
        auto sha = atx::core::sha256_hex(kEquityBaselineDsl[a]);
        entry.recipe.signals.push_back({std::string(kEquityBaselineSignalNames[a]),
                                        std::string(kEquityBaselineDsl[a]),
                                        sha ? *sha : std::string()});
    }
    entry.recipe.signals.push_back({"blend_equal", "published combo.bin alpha column", ""});
    for (atx::usize a = 0; a < profile.family_dsl.size(); ++a) {
        const auto dsl = profile.prereg && profile.prereg->families[a].sign < 0
            ? "-1 * (" + std::string(profile.family_dsl[a]) + ")"
            : std::string(profile.family_dsl[a]);
        auto sha = atx::core::sha256_hex(dsl);
        entry.recipe.signals.push_back({std::string(profile.family_names[a]), dsl,
                                        sha ? *sha : std::string()});
    }
    if (profile.prereg) {
        entry.parents.erase(entry.parents.begin() + 3); // design note is legacy only
        entry.parents.push_back({"preregistration-file", "", profile.prereg->file_sha256});
        entry.parents.push_back({"preregistration-canonical", "",
                                  profile.prereg->canonical_sha256});
        entry.recipe.horizons.assign(profile.horizons.begin(), profile.horizons.end());
        entry.recipe.bootstrap.predicted_null_horizons.clear();
    }
    entry.window = {cfg.equity_evaluation_start, cfg.equity_evaluation_end,
                    static_cast<atx::i64>(observations)};
    // W0-I0b (E-02 / E-09 wiring, grant trial_ledger.hpp:89-90): the ledger records the
    // block rule, the block lengths and the alignment ACTUALLY used, not the
    // checkpoint-14 defaults the struct carries.
    const auto &rules = profile.rules;
    entry.recipe.bootstrap.block_len_rule = rules.block_len_rule_text;
    entry.recipe.bootstrap.block_lens = rules.block_lens;
    entry.recipe.alignment = alignment_label(rules.execution_delay);
    entry.recipe.common_sample_rule =
        "prefix t < (T - (max(H) + delay)), delay " + number(rules.execution_delay) +
        "; incomplete prefix => _common unreportable";
    for (const auto id : marks.ids) {
        entry.source_exclusions.ex34_restriction_ids.push_back(number(id));
    }
    entry.source_exclusions.required_mark_id_count = static_cast<atx::i64>(marks.ids.size());
    entry.source_exclusions.terminal_hypothesis_ids.clear();
    entry.source_exclusions.terminal_evidenced_record_dates.clear();
    entry.source_exclusions.terminal_unevidenced_ids.clear();
    for (const auto &event : profile.terminal.events) {
        if (!event.evidenced) {
            entry.source_exclusions.terminal_unevidenced_ids.push_back(event.security_id);
            continue;
        }
        entry.source_exclusions.terminal_hypothesis_ids.push_back(event.security_id);
        if (event.record_session_key) {
            entry.source_exclusions.terminal_evidenced_record_dates.emplace_back(
                number(event.security_id), event.record_date);
        }
    }
    if (!marks.present) entry.source_exclusions.evidenced_non_terminal_ids.clear();
    entry.source_exclusions.unclassified_id_count =
        unclassified_mark_count(marks, profile.terminal);
    entry.producer_executable_sha256 =
        profile.recipe.at("producer_executable_sha256").get<std::string>();
    entry.notes = "Checkpoint 17 Stage 3 families; sign-and-shape only; not accepted alpha. " +
                  std::string(kTrialCountRule) + ".";
    if (profile.prereg) {
        entry.notes = std::string(profile.count_rule()) + "; declaration=" +
            profile.prereg->canonical_json;
    }
    return entry;
}

Result<std::string> next_trial_id(const std::string &ledger_path) {
    atx::u64 declared = 0;
    const auto prior = declared_trials_for_checkpoint(ledger_path, kCheckpoint);
    if (prior) {
        declared = *prior;
    } else if (prior.error().code() != ErrorCode::NotFound) {
        return Err(prior.error().code(), prior.error().message());
    }
    // Every checkpoint-17 pre-registration line declares the same count, so the
    // quotient is the number of prior runs; the next ordinal follows it.
    const atx::u64 ordinal = declared / static_cast<atx::u64>(kTrialCountDeclared) + 1;
    std::string digits = number(ordinal);
    while (digits.size() < 4U) digits.insert(digits.begin(), '0');
    return Ok(std::string(kTrialIdPrefix) + digits);
}

Result<std::string> runtime_trial_id(const Profile &profile, const fs::path &directory) {
    auto count = pre_registered_lines_for_checkpoint(profile.ledger_path, profile.checkpoint());
    if (!count && count.error().code() != ErrorCode::NotFound) {
        return Err(count.error().code(), count.error().message());
    }
    const auto ordinal = count ? *count : 0;
    if (ordinal == std::numeric_limits<atx::u64>::max()) {
        return Err(ErrorCode::OutOfRange, "equity ic: attempt ordinal overflow");
    }
    // The exclusively created output directory distinguishes simultaneous attempts
    // which observed the same ledger count before either acquired its append lock.
    ATX_TRY(auto location, atx::core::sha256_hex(fs::absolute(directory).generic_string()));
    return Ok("prereg-" + profile.prereg->canonical_sha256 + "-" + number(ordinal + 1) +
              "-" + location.substr(0, 16));
}

// Parallel cells share one sidecar ledger. append_trial never waits (its lock is
// a stop sign for a DEAD writer), so the stage retries a LIVE writer's brief
// exclusive window here and gives up after a bounded wait.
Result<TrialLedgerEntry> append_trial_with_retry(const std::string &ledger_path,
                                                 const TrialLedgerEntry &entry) {
    constexpr int kAttempts = 120;
    for (int attempt = 1;; ++attempt) {
        auto appended = append_trial(ledger_path, entry);
        if (appended || appended.error().code() != ErrorCode::Unavailable || attempt >= kAttempts) {
            return appended;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
}

// ---------------------------------------------------------------------------
//  execute — the §7.2 reuse chain, the §5 seal, the 12 engine calls and the
//  §7.3 publication.
// ---------------------------------------------------------------------------

struct Bound {
    PanelArtifact context;
    PanelArtifact evaluation;
    PanelArtifact combo;
    atx::u64 estimated_peak{};
};

Result<Bound> load_bound_inputs(const RunConfig &cfg, Profile &profile, Json &attempt) {
    std::error_code ec;
    const auto source_bytes = fs::file_size(cfg.panel, ec);
    if (ec) return Err(ErrorCode::IoError, "equity ic: cannot stat context");
    ATX_TRY(auto read_arrays, multiply(source_bytes, 2));
    ATX_TRY(auto read_peak, add(read_arrays, kOverheadReserve));
    if (read_peak > cfg.equity_max_working_bytes) {
        return Err(ErrorCode::OutOfRange, "equity ic: context snapshot reader exceeds budget");
    }
    ATX_TRY(auto metadata, read_file(cfg.panel + ".manifest.json", 16U * 1024U * 1024U));
    const auto declared = strict_json(metadata);
    const auto declared_recipe = declared.at("recipe").get<std::string>();
    ATX_TRY_VOID(require_context_recipe(declared_recipe, profile));
    const atx::u64 max_payload =
        std::min<atx::u64>(source_bytes, (cfg.equity_max_working_bytes - kOverheadReserve) / 2);
    ATX_TRY(auto context, read_panel_artifact(cfg.panel, max_payload));
    if (context.identity.recipe != declared_recipe ||
        context.artifact_id != declared.at("artifact_id").get<std::string>() ||
        context.identity.instrument_namespace != kSpiderRockSecurityIdNamespace) {
        return Err(ErrorCode::InvalidArgument, "equity ic: changed/unsupported identified context");
    }
    attempt["source_context_artifact_id"] = context.artifact_id;
    attempt["source_context_payload_sha256"] = context.payload_sha256;
    // D-12: re-derive admission AS OF each session from the membership.bin the
    // context's year-union allow-list was built from (the default rule).
    const auto context_recipe = strict_json(declared_recipe);
    const bool context_has_membership = context_recipe.contains("universe_membership_sha256");
    ATX_TRY(auto membership, resolve_equity_membership(context_has_membership,
        context_has_membership ? context_recipe.at("universe_membership_sha256").get<std::string>()
                               : std::string(),
        context_has_membership ? context_recipe.at("universe_cut").get<std::string>()
                               : std::string(),
        profile.views.membership_rule, cfg.equity_membership));
    attempt["membership"] = Json{
        {"context_restricted", context_has_membership},
        {"rule", equity_membership_rule_label(profile.views.membership_rule)},
        {"applied", membership.has_value()},
        {"dsl_cross_section_mask", membership ? "as-of-per-feature-date" : "observed-context"},
        {"membership_sha256", membership ? Json(membership->sha256) : Json(nullptr)},
        {"rebalances", membership ? membership->rebalances : 0U}};
    if (membership) profile.views.membership = std::move(membership->asof);
    const fs::path baseline(cfg.equity_baseline_dir);
    ATX_TRY(auto evaluation, read_panel_artifact((baseline / "evaluation.bin").string()));
    ATX_TRY(auto combo, read_panel_artifact((baseline / "combo.bin").string()));
    // Mirrors stage_equity_book.cpp:187-194 — the blend is the PUBLISHED one.
    ATX_TRY_VOID(require_panel_parent(evaluation.identity, "source-context", context.artifact_id));
    ATX_TRY_VOID(require_panel_parent(combo.identity, "research", evaluation.artifact_id));
    ATX_TRY_VOID(require_same_panel_axes(evaluation.identity, combo.identity));
    const auto baseline_recipe = strict_json(evaluation.identity.recipe);
    if (baseline_recipe.at("profile") != "slow-momentum-equal-weekly-shaping-v1" ||
        baseline_recipe.at("fit_kind") != "unfit-constant-weights" ||
        baseline_recipe.at("evaluation_start") != cfg.equity_evaluation_start ||
        baseline_recipe.at("evaluation_end_exclusive") != cfg.equity_evaluation_end) {
        return Err(ErrorCode::InvalidArgument,
                   "equity ic: baseline recipe or window differs from the requested window");
    }
    // A W0 baseline declares its membership rule; the IC must evaluate under the same one
    // (bind_fresh_view then proves the admission cells agree one for one).
    if (baseline_recipe.contains("membership_mask") &&
        baseline_recipe.at("membership_mask").at("rule").get<std::string>() !=
            equity_membership_rule_label(profile.views.membership_rule)) {
        return Err(ErrorCode::InvalidArgument,
                   "equity ic: the baseline was built under a different membership rule");
    }
    attempt["universe"] = Json{{"liquidity_floor", baseline_recipe.contains("liquidity_floor")
                                                       ? baseline_recipe.at("liquidity_floor")
                                                       : Json(nullptr)},
        {"source", "baseline recipe decision_eligibility + liquidity_floor"}};
    // Checkpoint 19: the baseline recipe defines the universe. The fresh view
    // re-evaluated below must admit exactly what the published baseline admitted,
    // so the floor is taken from the recipe, never from this stage's own flags.
    // (The first cp19 sweep ran without this and silently measured the unfloored
    // universe; bind_fresh_view now also compares the admission masks.)
    profile.views.min_dollar_adv = 0.0;
    profile.views.dollar_adv_window = 21;
    if (baseline_recipe.contains("liquidity_floor")) {
        const auto &floor = baseline_recipe.at("liquidity_floor");
        profile.views.min_dollar_adv = floor.at("min_dollar_adv").get<atx::f64>();
        const auto window = floor.at("dollar_adv_window");
        profile.views.dollar_adv_window = window.is_string()
            ? static_cast<atx::usize>(std::stoull(window.get<std::string>()))
            : window.get<atx::usize>();
    }
    Bound bound{std::move(context), std::move(evaluation), std::move(combo), read_peak};
    return Ok(std::move(bound));
}

Json manifest_parents(const Bound &bound, const RequiredMarks &marks,
                      const EquityTerminalReturnTable &terminal) {
    Json parents = Json::array({
        Json{{"role", "source-context"}, {"sha256", bound.context.artifact_id}},
        Json{{"role", "baseline-evaluation"}, {"sha256", bound.evaluation.artifact_id}},
        Json{{"role", "baseline-combo"}, {"sha256", bound.combo.artifact_id}}});
    if (marks.present) {
        parents.push_back(Json{{"role", "required-mark-audit"}, {"sha256", marks.audit_id}});
    }
    if (!terminal.sha256.empty()) {
        parents.push_back(Json{{"role", "terminal-return-table"}, {"sha256", terminal.sha256}});
    }
    return parents;
}

Status bind_fresh_view(const EquityBaselineEvaluation &view, const PanelArtifact &evaluation,
                       const PanelArtifact &context) {
    PanelIdentity axes = context.identity;
    axes.session_keys = view.session_keys;
    ATX_TRY_VOID(require_same_panel_axes(axes, evaluation.identity));
    if (view.panel.dates() != evaluation.panel.dates() ||
        view.panel.instruments() != evaluation.panel.instruments()) {
        return Err(ErrorCode::InvalidArgument, "equity ic: fresh view shape differs from baseline");
    }
    for (const std::string field : {"close", "raw_close"}) {
        ATX_TRY(auto fresh_id, view.panel.field_id(field));
        ATX_TRY(auto published_id, evaluation.panel.field_id(field));
        const auto fresh = view.panel.field_all(fresh_id);
        const auto published = evaluation.panel.field_all(published_id);
        if (fresh.size() != published.size()) {
            return Err(ErrorCode::InvalidArgument, "equity ic: field extent differs from baseline");
        }
        for (atx::usize c = 0; c < fresh.size(); ++c) {
            const bool masked = view.panel.in_universe(c / view.panel.instruments(),
                                                       c % view.panel.instruments());
            if (masked && fresh[c] != published[c]) {
                return Err(ErrorCode::InvalidArgument,
                           "equity ic: fresh view values differ from the published baseline");
            }
        }
    }
    // Checkpoint 19: admission itself must match cell for cell, so a universe
    // predicate carried by the baseline recipe (the liquidity floor) can never be
    // silently dropped by this stage.
    for (atx::usize row = 0; row < view.panel.dates(); ++row) {
        for (atx::usize inst = 0; inst < view.panel.instruments(); ++inst) {
            if (view.panel.in_universe(row, inst) != evaluation.panel.in_universe(row, inst)) {
                return Err(ErrorCode::InvalidArgument,
                           "equity ic: fresh view admission differs from the published baseline");
            }
        }
    }
    return Ok();
}

Json epoch_counts(const eval::TrialEpochCounts &c) {
    return Json{{"known_unique_cells", c.unique_cells}, {"measured_declarations", c.declared_cells},
        {"verified_retained_cells", c.verified_retained_cells}, {"attempts", c.attempts},
        {"completed", c.completed}, {"failed", c.failed}, {"incomplete", c.incomplete}};
}

// Hash ordered axes without materializing a second full JSON axis array. The
// textual length prefixes make the encoding unambiguous across platforms.
Result<std::string> epoch_axes_sha(const PanelIdentity &axes) {
    atx::core::Sha256 hash;
    const auto feed = [&](std::string_view s) -> Status {
        const auto prefix = number(s.size()) + ":";
        ATX_TRY_VOID(hash.update(std::as_bytes(std::span(prefix.data(), prefix.size()))));
        return hash.update(std::as_bytes(std::span(s.data(), s.size())));
    };
    ATX_TRY_VOID(feed("atx-e2-panel-axes-v1"));
    ATX_TRY_VOID(feed(axes.instrument_namespace));
    ATX_TRY_VOID(feed(number(axes.session_keys.size())));
    for (const auto key : axes.session_keys) ATX_TRY_VOID(feed(number(key)));
    ATX_TRY_VOID(feed(number(axes.instrument_ids.size())));
    for (const auto &id : axes.instrument_ids) ATX_TRY_VOID(feed(id));
    ATX_TRY_VOID(feed(number(axes.original_instrument_indices.size())));
    for (const auto i : axes.original_instrument_indices) ATX_TRY_VOID(feed(number(i)));
    ATX_TRY(auto bytes, hash.finalize());
    constexpr char digits[] = "0123456789abcdef";
    std::string result;
    result.reserve(64);
    for (const auto byte : bytes) {
        const auto value = std::to_integer<unsigned>(byte);
        result.push_back(digits[value >> 4U]);
        result.push_back(digits[value & 15U]);
    }
    return result;
}

Result<eval::TrialEpochAttempt> epoch_attempt(const Profile &profile, const Bound &bound,
    const RequiredMarks &marks, const Json &attempt, const fs::path &directory) {
    if (!profile.prereg) return Err(ErrorCode::InvalidArgument, "equity ic: E2 requires preregistration");
    ATX_TRY(auto context_axes, epoch_axes_sha(bound.context.identity));
    ATX_TRY(auto evaluation_axes, epoch_axes_sha(bound.evaluation.identity));
    // Both evidenced prices and unevidenced terminal flags affect the labels.
    // Source prose/order is provenance, not a different numerical model.
    std::vector<const EquityTerminalEvent *> ordered_events;
    // This bounded declaration format intentionally refuses oversized recipes;
    // callers must not silently omit terminal support from the cell identity.
    if (profile.terminal.events.size() > 128)
        return Err(ErrorCode::OutOfRange, "equity ic: E2 inline terminal recipe exceeds 128-event bound");
    ordered_events.reserve(profile.terminal.events.size());
    for (const auto &event : profile.terminal.events) ordered_events.push_back(&event);
    std::sort(ordered_events.begin(), ordered_events.end(), [](const auto *a, const auto *b) {
        return a->security_id < b->security_id;
    });
    Json terminal = Json::array();
    for (const auto *e : ordered_events) terminal.push_back(Json{{"security_id", e->security_id},
        {"terminal_value", e->terminal_value ? Json(*e->terminal_value) : Json(nullptr)},
        {"terminal_return", e->terminal_return ? Json(*e->terminal_return) : Json(nullptr)},
        {"special_dividend", e->special_dividend}, {"record_session_key",
            e->record_session_key ? Json(*e->record_session_key) : Json(nullptr)},
        {"evidenced", e->evidenced}});
    // Explicit whitelist, never the full run recipe: display labels, report paths,
    // prereg/checkpoint identity and producer executable are not numerical choices.
    Json recipe{{"schema", "atx-equity-ic-numerical-cells-e2-v1"},
        {"engine_recipe", "cross-section-ic-HansenHodrickV1-AverageRanksV1"},
        {"context_payload_sha256", bound.context.payload_sha256},
        {"context_axes_sha256", context_axes},
        {"evaluation_payload_sha256", bound.evaluation.payload_sha256},
        {"evaluation_axes_sha256", evaluation_axes},
        {"evaluation_start", profile.views.evaluation.begin_session_key},
        {"evaluation_end_exclusive", profile.views.evaluation.end_exclusive_session_key},
        {"observation_basis", "ArchiveRawVolumeAndPointwiseAdjustedCloseV1"},
        {"membership", attempt.at("membership")},
        {"min_dollar_adv", profile.views.min_dollar_adv},
        {"dollar_adv_window", profile.views.dollar_adv_window},
        {"common_max_horizon", profile.horizons.back()},
        {"quantiles", kQuantiles}, {"min_names", profile.rules.min_names_per_date},
        {"delay", profile.rules.execution_delay}, {"bootstrap_draws", kBootstrapDraws},
        {"bootstrap_seed", kBootstrapSeed}, {"block_len_floor", kBlockLenFloor},
        {"block_len_rule", profile.rules.block_len_rule_name},
        {"autocorr_lags", Json::array({1})},
        {"trade_bps", kTradeBps}, {"annual_borrow_bps", kAnnualBorrowBps},
        {"short_leg_gross", kShortLegGross}, {"day_basis", kDayBasis},
        {"terminal_evidence", terminal},
        {"excluded_mark_ids", marks.ids}};
    eval::TrialEpochAttempt declaration;
    declaration.numerical_recipe_json = recipe.dump();
    declaration.prereg_sha256 = profile.prereg->file_sha256;
    const Json execution_identity{{"schema", "atx-equity-ic-epoch-attempt-v1"},
        {"epoch", profile.prereg->epoch}, {"prereg_sha256", declaration.prereg_sha256},
        {"directory", fs::absolute(directory).lexically_normal().generic_string()},
        {"recipe", recipe}};
    ATX_TRY(declaration.token, atx::core::sha256_hex(execution_identity.dump()));
    declaration.trial_id = "e2-ic-" + declaration.token;
    for (atx::usize a = 0; a < profile.prereg->families.size(); ++a) {
        const auto &family = profile.prereg->families[a];
        for (atx::usize hi = 0; hi < family.horizons.size(); ++hi) {
            for (const auto variant : kVariantNames) for (const auto restriction : kRestrictionNames) {
                eval::TrialEpochCell cell;
                cell.canonical_dsl = family.dsl;
                cell.sign = family.sign;
                cell.horizon = family.horizons[hi];
                cell.stream_signal_index = a + kEquityBaselineDsl.size() + 1;
                cell.stream_horizon_index = hi;
                cell.forward_variant = variant;
                cell.restriction = restriction == "ex34" ? "_ex34" : "full";
                cell.family_sha256 = family.configuration_sha256;
                cell.display_alias = family.name;
                if (family.lineage) cell.retained = eval::TrialEpochLineage{
                    family.lineage->prereg_sha256, family.lineage->configuration_sha256,
                    family.lineage->trial_id};
                declaration.cells.push_back(std::move(cell));
            }
        }
    }
    return declaration;
}

Result<StageResult> execute(const RunConfig &cfg, Profile &profile, Json &attempt,
                            const fs::path &directory, bool &registered) {
    const auto started = std::chrono::steady_clock::now();
    Json files = Json::array();
    // Form borrowed views only after the owning Profile has reached its final address.
    if (profile.prereg) {
        for (const auto &family : profile.prereg->families) {
            profile.family_dsl.push_back(family.dsl);
            profile.family_names.push_back(family.name);
        }
    } else {
        profile.family_dsl.assign(kEquityFamilyDsl.begin(), kEquityFamilyDsl.end());
        profile.family_names.assign(kEquityFamilySignalNames.begin(), kEquityFamilySignalNames.end());
    }
    const bool frozen_terminal_table = profile.terminal.source == kFrozenTerminalTableSource;
    ATX_TRY(auto marks, load_required_marks(cfg.equity_baseline_dir, frozen_terminal_table));
    ATX_TRY(auto bound, load_bound_inputs(cfg, profile, attempt));
    ATX_TRY(auto plan, plan_equity_baseline(bound.context, profile.views));
    ATX_TRY(auto view_arrays, multiply(plan.evaluation_cells, 64));
    ATX_TRY(auto estimate, add(bound.estimated_peak, plan.additional_array_bytes));
    ATX_TRY(estimate, add(estimate, view_arrays));
    if (estimate > cfg.equity_max_working_bytes) {
        return Err(ErrorCode::OutOfRange, "equity ic: memory estimate exceeds budget");
    }
    ATX_TRY(auto view, evaluate_equity_baseline(bound.context, profile.views));
    ATX_TRY_VOID(bind_fresh_view(view, bound.evaluation, bound.context));
    const auto dates = view.panel.dates();
    const auto names = view.panel.instruments();
    ATX_TRY(auto close_id, view.panel.field_id("close"));
    ATX_TRY(auto raw_id, view.panel.field_id("raw_close"));
    ATX_TRY(auto alpha_id, bound.combo.panel.field_id("alpha"));
    const auto price = view.panel.field_all(close_id);
    const auto raw_price = view.panel.field_all(raw_id);
    const auto blend = bound.combo.panel.field_all(alpha_id);
    struct SignalColumn {
        std::string_view name;
        std::span<const atx::f64> values;
        std::span<const atx::usize> horizons;
    };
    std::vector<SignalColumn> columns;
    EquityFamilyEvaluation families, dollar_adv;
    CellArrays cells;
    auto prepare = [&]() -> Status {
        // Checkpoint 17: every family is evaluated in ONE pass over the same feature
        // window, on the baseline's admission mask; its warmup is derived.
        auto family_views = profile.views;
        if (profile.prereg) family_views.max_additional_bytes = cfg.equity_max_working_bytes - estimate;
        ATX_TRY(families, evaluate_equity_families(bound.context, family_views, view,
                    std::span<const std::string_view>(profile.family_dsl),
                    std::span<const std::string_view>(profile.family_names)));
        ATX_TRY(estimate, add(estimate, families.additional_array_bytes));
        if (estimate > cfg.equity_max_working_bytes) {
            return Err(ErrorCode::OutOfRange, "equity ic: memory estimate with families exceeds budget");
        }
        // R17-8 cost/capacity view: dollar ADV per admitted cell, an ancillary series
        // (never a signal, never a trial) averaged over each decile's members.
        const std::array<std::string_view, 1> adv_dsl{kEquityDollarAdvDsl};
        const std::array<std::string_view, 1> adv_name{kEquityDollarAdvName};
        if (profile.prereg) family_views.max_additional_bytes = cfg.equity_max_working_bytes - estimate;
        ATX_TRY(dollar_adv, evaluate_equity_families(bound.context, family_views, view,
                    std::span<const std::string_view>(adv_dsl),
                    std::span<const std::string_view>(adv_name)));
        ATX_TRY(estimate, add(estimate, dollar_adv.additional_array_bytes));
        if (estimate > cfg.equity_max_working_bytes) {
            return Err(ErrorCode::OutOfRange, "equity ic: memory estimate with dollar adv exceeds budget");
        }

        // §5.2 — a live gate that a 2013-only panel does not trip.
        eval::CalendarSeal seal{};
        seal.policy = eval::SealPolicy::RejectSealedV1;
        ATX_TRY(auto seal_report,
                eval::apply_calendar_seal(std::span<const atx::i64>(view.session_keys), seal));
        const Json seal_json{
            {"policy", "RejectSealedV1"}, {"dates_total", seal_report.dates_total},
            {"dates_visible", seal_report.dates_visible},
            {"dates_at_or_after_validation", seal_report.dates_at_or_after_validation},
            {"dates_at_or_after_sealed", seal_report.dates_at_or_after_sealed},
            {"embargo_len", seal_report.embargo_len},
            {"content_address", number(seal_report.content_address)},
            {"used_reserve_window", seal_report.used_reserve_window != 0},
            {"statement", "non-vacuous by code; vacuous by data for a 2013-only panel"},
            {"untested_paths", Json::array({"MaskSealedV1", "reserve_window routing"})}};
        ATX_TRY(auto seal_file, write_text(directory, "seal.json", seal_json.dump(2) + "\n"));
        files.push_back(std::move(seal_file));

        ATX_TRY(cells, build_cells(view, bound.evaluation.identity.instrument_ids, marks,
                                        profile.terminal));
        if (view.signals.alphas.size() != kEquityBaselineDsl.size() ||
            blend.size() != price.size() ||
            families.signals.alphas.size() != profile.family_dsl.size() ||
            dollar_adv.signals.alphas.size() != 1 ||
            dollar_adv.signals.alphas[0].values.size() != price.size()) {
            return Err(ErrorCode::InvalidArgument, "equity ic: unexpected signal or combo extent");
        }
        // Signal table: baseline DSLs, the published blend, then the families. Index 0
        // stays momentum_252 so the single-block measurement scopes below are unchanged.
        for (atx::usize a = 0; a < kEquityBaselineDsl.size(); ++a) {
            columns.push_back({kEquityBaselineSignalNames[a], view.signals.alphas[a].values,
                               profile.horizons});
        }
        columns.push_back({"blend_equal", blend, profile.horizons});
        for (atx::usize a = 0; a < profile.family_dsl.size(); ++a) {
            if (families.signals.alphas[a].values.size() != price.size()) {
                return Err(ErrorCode::InvalidArgument, "equity ic: family signal extent");
            }
            if (profile.prereg && profile.prereg->families[a].sign < 0) {
                for (auto &value : families.signals.alphas[a].values) value = -value;
            }
            const std::span<const atx::usize> requested = profile.prereg
                ? std::span<const atx::usize>(profile.prereg->families[a].horizons)
                : std::span<const atx::usize>(profile.horizons);
            columns.push_back({profile.family_names[a], families.signals.alphas[a].values, requested});
        }

        return Ok();
    };
    if (!profile.prereg) ATX_TRY_VOID(prepare());

    std::optional<eval::TrialEpochCatalog> epoch;
    std::optional<eval::TrialEpochReservation> reservation;
    std::string epoch_token, epoch_trial_id;
    if (profile.epoch_accounting) {
        ATX_TRY(auto catalog, eval::TrialEpochCatalog::open(cfg.equity_ic_epoch_catalog,
            profile.prereg->epoch, cfg.equity_ic_epoch_anchor));
        epoch = std::move(catalog);
        ATX_TRY(auto declaration, epoch_attempt(profile, bound, marks, attempt, directory));
        epoch_token = declaration.token;
        epoch_trial_id = declaration.trial_id;
        ATX_TRY(auto reserved, epoch->reserve_attempt(declaration));
        reservation = std::move(reserved);
        if (!reservation->inserted) return Err(ErrorCode::AlreadyExists,
            "equity ic: E2 token already reserved; no VM rerun, use a new attempt directory");
    }
    // Every exit after a durable E2 reservation (including legacy pre-line and
    // publication failures) passes through catalog finalization below.
    std::string manifest_sha;
    auto registered_run = [&]() -> Result<StageResult> {
      try {
    if (reservation) {
        registered = true;
        attempt["epoch_catalog"] = Json{{"rule", "epoch-e2-v1"},
            {"epoch", profile.prereg->epoch}, {"trial_id", epoch_trial_id}, {"token", epoch_token},
            {"reservation_head_sha256", reservation->reservation_head_sha256},
            {"numerical_recipe_sha256", reservation->numerical_recipe_sha256},
            {"new_unique_cells", reservation->new_unique_cells},
            {"verified_retained_cells", reservation->verified_retained_cells},
            {"counts_at_reservation", epoch_counts(reservation->counts)},
            {"coverage", "known-catalog-declarations-only; historical-imports-unreconciled"},
            {"pnl_cluster_coverage", "unavailable-no-calendar-PnL-attached"}};
        ATX_TRY(auto reserved_file, write_text(directory, "epoch-reservation.json",
            attempt.at("epoch_catalog").dump(2) + "\n"));
        (void)reserved_file;
    }

    // §4.5 — the pre-registration line goes in BEFORE the numbers exist.
    ATX_TRY(auto trial_id, epoch ? Ok(epoch_trial_id) : (profile.prereg
        ? runtime_trial_id(profile, directory) : next_trial_id(profile.ledger_path)));
    auto pre = base_entry(cfg, profile, marks, bound.context, bound.evaluation, bound.combo,
                          trial_id, dates);
    pre.status = "pre-registered";
    pre.result.outcome = "pending";
    ATX_TRY(auto pre_line, append_trial_with_retry(profile.ledger_path, pre));
    registered = true;
    std::string pre_sha;

    // ---- POINT OF NO RETURN (§11.9 ruling C-1) -----------------------------
    // The ledger now carries a "pre-registered" line for this trial_id. §4.5:
    // "A failed run keeps both lines." Everything below therefore runs inside
    // ONE guarded phase whose every exit — Err, write failure, or exception —
    // falls through to the terminal append at the bottom of this function. No
    // `return` may appear between here and it.
    std::optional<atx::f64> published_seconds;
    Result<StageResult> outcome = [&]() -> Result<StageResult> {
      try {
        ATX_TRY(auto pre_text, serialize_trial_entry(pre_line));
        ATX_TRY(pre_sha, atx::core::sha256_hex(pre_text + "\n"));
        if (profile.prereg) {
            attempt["preregistration"] = profile.recipe.at("preregistration");
            ATX_TRY_VOID(prepare());
        }
        attempt["required_mark_audit"] = Json{
            {"status", marks.present ? "loaded" : "absent-optional-ex34-restriction-empty"},
            {"path", marks.path}, {"audit_id", marks.audit_id},
            {"manifest_sha256", marks.manifest_sha256},
            {"required_mark_id_count", marks.ids.size()},
            {"required_mark_cells", marks.cells},
            {"terminal_hypothesis_ids", terminal_ids(profile.terminal, true)},
            {"evidenced_non_terminal_ids",
             Json::array({kEvidencedNonTerminalIds[0], kEvidencedNonTerminalIds[1]})},
            {"unclassified_id_count", unclassified_mark_count(marks, profile.terminal)},
            {"terminal_unevidenced_ids", terminal_ids(profile.terminal, false)},
            {"pcs_statement", "PCS is never applied; admission remains rejected"},
            {"membership_checked_at_runtime", marks.present && frozen_terminal_table},
            {"panel_columns_excluded_by_ex34", cells.excluded_columns},
            {"panel_columns_flagged_terminal", cells.terminal_columns}};
        attempt["terminal_evidence"] = terminal_evidence_json(profile.terminal);
        attempt["terminal_table"] = Json{{"source", profile.terminal.source},
            {"sha256", profile.terminal.sha256}, {"events", profile.terminal.events.size()},
            {"interface", "W0-I0b terminal-return table; data from W2-D2"}};
        attempt["trial_ledger"] = Json{{"path", profile.ledger_path}, {"trial_id", trial_id},
            {"pre_registration_line_sha256", pre_sha},
            {"trial_count_declared", profile.ledger_count()}};
        {
            Json finite = Json::object();
            for (atx::usize a = 0; a < profile.family_dsl.size(); ++a) {
                finite[std::string(profile.family_names[a])] = families.finite_admitted_cells[a];
            }
            attempt["families"] = Json{{"checkpoint", profile.checkpoint()},
                {"count", profile.family_dsl.size()},
                {"warmup_observations_derived", families.warmup}, {"vm_slots", families.vm_slots},
                {"additional_array_bytes", number(families.additional_array_bytes)},
                {"admission", "baseline mask (eligible AND both momentum signals ready); a family "
                              "cell is NaN where its own value is not finite and the engine "
                              "excludes and counts it"},
                {"finite_admitted_cells", finite}, {"trial_count_rule", profile.count_rule()},
                {"retained_count", profile.retained_count()},
                {"dollar_adv", Json{{"dsl", kEquityDollarAdvDsl}, {"name", kEquityDollarAdvName},
                    {"finite_admitted_cells", number(dollar_adv.finite_admitted_cells[0])},
                    {"role", "ancillary; mean_dollar_adv column of quantile_spread.csv; not a "
                             "signal, not a trial"}}}};
        }
        attempt["cost_model_provenance"] = kCostModelProvenance;
        attempt["borrow_day_convention"] = kBorrowDayConvention;
        attempt["alignment"] = alignment_label(profile.rules.execution_delay);
        if (!profile.prereg) attempt["design_note"] =
            Json{{"path", kDesignNoteRelativePath}, {"sha256", kDesignNoteSha256},
                 {"binding", "embedded constant; the runner refuses the run unless the on-disk "
                             "design note hashes to exactly this value"}};
        ATX_TRY(auto request, write_text(directory, "request.json", attempt.dump(2) + "\n"));
        files.push_back(std::move(request));

        // --- the 12 engine calls ------------------------------------------------
        eval::CrossSectionIcInput input{};
        input.dates = dates;
        input.instruments = names;
        input.price = price;
        input.raw_price = raw_price;
        input.mask = cells.mask;
        input.terminal = cells.terminal;
        input.terminal_evidenced = cells.terminal_evidenced;
        input.terminal_value = cells.terminal_value;
        input.excluded_audited = cells.excluded_audited;
        input.session_keys = view.session_keys;
        input.aux = dollar_adv.signals.alphas[0].values;

        eval::CrossSectionIcConfig base{};
        base.horizons = profile.horizons;
        base.quantiles = kQuantiles;
        base.min_names_per_date = profile.rules.min_names_per_date; // E-18
        base.bootstrap_draws = kBootstrapDraws;
        base.block_len_floor = kBlockLenFloor;
        base.block_len_rule = profile.rules.block_len_rule;         // E-02
        base.execution_delay = profile.rules.execution_delay;       // E-09
        base.hac_rule = eval::IcHacRule::HansenHodrickV1;           // E-03
        base.bootstrap_seed = kBootstrapSeed;
        base.trade_bps = kTradeBps;
        base.annual_borrow_bps = kAnnualBorrowBps;
        base.short_leg_gross = kShortLegGross;
        base.day_basis = kDayBasis;
        // E-09: the _common prefix must leave room for the LONGEST label, which ends
        // delay + max(H) rows after its signal row; the pre-W0 T - max(H) put a row with
        // no exit into the h = 63 prefix under delay 1 and voided that block.
        const auto embargo = eval::label_embargo(profile.horizons.back(), base.execution_delay);
        base.common_sample_dates = dates > embargo ? dates - embargo : 0;
        base.ties = eval::IcTieHandling::AverageRanksV1;
        base.forward_variant = eval::ForwardReturnVariant::DropMissingForward;

        input.signal = std::span<const atx::f64>(view.signals.alphas[0].values);
        ATX_TRY(auto scratch, eval::plan_cross_section_ic(input, base));

        Writers writers;
        write_headers(writers);
        // modulo_fallbacks counts distinct bootstrap draws, so a sum over every
        // interval is the right total. dates_below_min_names is a per-block property
        // and is NOT summed (§11.9 minors): the named block carries the reportable
        // value and the maximum over all sixty blocks proves none of the others
        // differ.
        atx::usize modulo_fallbacks = 0;
        atx::usize dates_below_min_names = 0;
        atx::usize dates_below_min_names_max = 0;
        std::set<atx::usize> null_interval_horizons;
        atx::usize terminal_applied = 0;
        atx::usize terminal_unevidenced = 0;

        for (atx::usize s = 0; s < columns.size(); ++s) {
            input.signal = columns[s].values;
            std::array<std::array<eval::CrossSectionIcResult, 2>, 2> block{};
            for (atx::usize v = 0; v < kVariantNames.size(); ++v) {
                for (atx::usize r = 0; r < kRestrictionNames.size(); ++r) {
                    eval::CrossSectionIcConfig c = base;
                    c.horizons = columns[s].horizons;
                    c.stream_signal_index = s;
                    c.stream_variant_id = v;
                    c.stream_restriction_id = r;
                    c.forward_variant =
                        v == 0 ? eval::ForwardReturnVariant::DropMissingForward
                               : eval::ForwardReturnVariant::IncludeAuditedTerminalV1;
                    ATX_TRY(auto computed, eval::compute_cross_section_ic(input, c, scratch));
                    block[v][r] = std::move(computed);
                }
            }
            for (atx::usize h = 0; h < columns[s].horizons.size(); ++h) {
                for (atx::usize v = 0; v < kVariantNames.size(); ++v) {
                    for (atx::usize r = 0; r < kRestrictionNames.size(); ++r) {
                        const auto &sum = block[v][r].horizons[h];
                        const BlockKey key{columns[s].name, kVariantNames[v], kRestrictionNames[r]};
                        emit_date_rows(writers, key, sum);
                        emit_decay_row(writers, key, sum);
                        emit_spread_rows(writers, key, sum);
                        emit_summary_row(writers, key, sum);
                        dates_below_min_names_max =
                            std::max(dates_below_min_names_max, sum.dates_below_min_names);
                        for (const auto *sample : {&sum.full, &sum.common}) {
                            for (const auto *iv : {&sample->ic_mean_ci, &sample->icir_ci,
                                     &sample->rank_ic_mean_ci, &sample->rank_icir_ci,
                                     &sample->spread_gross_ci, &sample->spread_net_ci}) {
                                modulo_fallbacks += iv->modulo_fallbacks;
                            }
                        }
                        if (sum.full.ic_mean_ci.reportable == 0) {
                            null_interval_horizons.insert(sum.horizon);
                        }
                        // Counted ONCE, on one named block: the same cell recurs in
                        // all 12 blocks and every horizon, so a sum over them would
                        // publish a multiple of the real cell count as "measured".
                        if (s == 0 && v == 1 && r == 0) {
                            dates_below_min_names += sum.dates_below_min_names;
                            for (const auto &p : sum.series) {
                                terminal_applied += p.n_terminal_applied;
                                terminal_unevidenced += p.n_terminal_unevidenced;
                            }
                        }
                    }
                }
            }
            emit_autocorr_row(writers, columns[s].name, block[0][0].autocorr);
        }

        Json summary{{"schema", "atx-equity-ic-summary-v1"},
                     {"purpose", "training-only-forecast-evaluation"},
                     {"observations", dates}, {"instruments", names},
                     {"common_sample_dates", base.common_sample_dates},
                     {"checkpoint", profile.checkpoint()}, {"signal_count", columns.size()},
                     {"family_warmup_observations_derived", families.warmup},
                     {"trial_count_declared", profile.ledger_count()},
                     {"trial_count_rule", profile.count_rule()},
                     {"family_retained_count", profile.retained_count()},
                     {"liquidity_floor", attempt.at("universe").at("liquidity_floor")},
                     {"alignment", alignment_label(profile.rules.execution_delay)},
                     {"execution_delay", profile.rules.execution_delay},
                     {"min_names_per_date", profile.rules.min_names_per_date},
                     {"block_len_rule", profile.rules.block_len_rule_name},
                     {"membership", attempt.at("membership")},
                     {"membership_rejected_cells", view.membership_rejected_cells},
                     {"required_mark_audit", attempt.at("required_mark_audit").at("status")},
                     {"terminal_table", attempt.at("terminal_table")},
                     {"naive_t_validity", kNaiveTValidity},
                     {"cost_model_provenance", kCostModelProvenance},
                     {"borrow_day_convention", kBorrowDayConvention},
                     {"turnover_source", kTurnoverSource},
                     {"sign_and_shape_statement", kSignAndShapeStatement},
                     {"acceptance", "sign-and-shape evidence only; not accepted alpha"},
                     {"series", writers.summary_rows}};
        if (reservation) summary["epoch_catalog"] = attempt.at("epoch_catalog");
        for (const auto &[name, text] : {std::pair<std::string, std::string>{
                 "coverage.csv", writers.coverage.str()},
             std::pair<std::string, std::string>{"ic.csv", writers.ic.str()},
             std::pair<std::string, std::string>{"ic_decay.csv", writers.decay.str()},
             std::pair<std::string, std::string>{"signal_autocorr.csv", writers.autocorr.str()},
             std::pair<std::string, std::string>{"quantile_spread.csv", writers.spread.str()},
             std::pair<std::string, std::string>{"ic_summary.json", summary.dump(2) + "\n"}}) {
            ATX_TRY(auto file, write_text(directory, name, text));
            files.push_back(std::move(file));
        }

        Json null_horizons = Json::array();
        for (const auto h : null_interval_horizons) null_horizons.push_back(h);
        // One measurement, published in manifest.json and carried to the ledger's
        // terminal line, so the two numbers can be cross-checked.
        const atx::f64 publish_seconds = elapsed_seconds(started);
        Json manifest{{"schema", "atx-equity-ic-v1"}, {"status", "complete"},
            {"recipe", profile.recipe},
            {"parents", manifest_parents(bound, marks, profile.terminal)},
            {"alignment", alignment_label(profile.rules.execution_delay)},
            {"cost_model_provenance", kCostModelProvenance},
            {"borrow_day_convention", kBorrowDayConvention},
            {"membership", attempt.at("membership")},
            {"trial_ledger", Json{{"path", profile.ledger_path}, {"trial_id", trial_id},
                                  {"pre_registration_line_sha256", pre_sha}}},
            {"runtime", Json{{"runtime_seconds", publish_seconds},
                             {"peak_working_set_bytes", nullptr},
                             {"peak_working_set_source", kPeakWorkingSetSource}}},
            {"predictions_confirmed",
             Json{{"null_interval_horizons", null_horizons},
                  {"dates_below_min_names", dates_below_min_names},
                  {"dates_below_min_names_scope",
                   "ONE block: signal momentum_252, variant IncludeAuditedTerminalV1, "
                   "restriction full, summed over its five horizons"},
                  {"dates_below_min_names_max_over_blocks", dates_below_min_names_max},
                  {"modulo_fallbacks", modulo_fallbacks}}},
            {"terminal_evidence", Json{{"required_mark_id_count", marks.ids.size()},
                {"required_mark_audit", attempt.at("required_mark_audit").at("status")},
                {"terminal_table_source", profile.terminal.source},
                {"terminal_hypothesis_ids", terminal_ids(profile.terminal, true)},
                {"evidenced_non_terminal_ids",
             Json::array({kEvidencedNonTerminalIds[0], kEvidencedNonTerminalIds[1]})},
                {"unclassified_id_count", unclassified_mark_count(marks, profile.terminal)},
                {"terminal_unevidenced_ids", terminal_ids(profile.terminal, false)},
                {"n_terminal_applied_cells_measured", terminal_applied},
                {"n_terminal_unevidenced_cells_measured", terminal_unevidenced},
                {"terminal_cell_measurement_scope",
                 "summed over every horizon of ONE block: signal momentum_252, variant "
                 "IncludeAuditedTerminalV1, restriction full"},
                {"pcs_applied", false},
                {"pcs_statement", "PCS admission remains rejected; never applied"}}},
            {"sign_and_shape_statement", kSignAndShapeStatement},
            {"qualifications", Json::array({
                "An IC is a model-skill statistic, not realized long/short returns after costs.",
                "The price series is not a verified total-return series across all securities.",
                "Dropping missing forward returns is a selection and biases the IC upward.",
                "Overlapping horizons make the naive t-statistic invalid; read the interval.",
                "The decay curve confounds horizon with calendar period; read the _common column.",
                "blend_equal is a rank-space blend; its Pearson IC is not "
                "comparable to signals 0/1.",
                "The return is indexed from t while the deployed book executes at t+1.",
                "The net decile spread restates two constants; it is not the replay cost model.",
                "1 - rho_rank is a rule of thumb on a different scale from decile turnover.",
                "No sanitizer, static analyser, include-clean build or CI covers this work.",
                "Nothing here completes a replay, prices a corporate action "
                "or changes PCS admission.",
                "No live trading and no broker action is performed or authorized."})},
            {"acceptance", "sign-and-shape evidence only; not accepted alpha"}, {"files", files}};
        if (profile.prereg) {
            manifest["parents"].push_back(Json{{"role", "preregistration-file"},
                {"sha256", profile.prereg->file_sha256}});
            manifest["parents"].push_back(Json{{"role", "preregistration-canonical"},
                {"sha256", profile.prereg->canonical_sha256}});
            manifest["predictions_confirmed"]["dates_below_min_names_scope"] =
                "ONE reference block: momentum_252, IncludeAuditedTerminalV1, full, "
                "summed over the registered horizon union";
        }
        if (reservation) manifest["epoch_catalog"] = attempt.at("epoch_catalog");
        ATX_TRY(auto ic_id, atx::core::sha256_hex(std::string(kDomain) + manifest.dump()));
        manifest["ic_id"] = ic_id;
        // Manifest FIRST, `.pending` last: a failure on the manifest write must
        // leave a directory that still advertises itself as incomplete.
        const auto manifest_text = manifest.dump(2) + "\n";
        ATX_TRY(auto manifest_file, write_text(directory, "manifest.json", manifest_text));
        std::error_code ec;
        if (!epoch && (!fs::remove(directory / ".pending", ec) || ec)) {
            return Err(ErrorCode::IoError, "equity ic: cannot release pending marker");
        }
        manifest_sha = manifest_file.at("sha256").get<std::string>();
        published_seconds = publish_seconds;

        StageResult result{};
        result.digest = fnv1a64(ic_id.data(), ic_id.size());
        result.kvs.emplace_back("ic_id", ic_id);
        result.kvs.emplace_back("trial_id", trial_id);
        result.kvs.emplace_back("observations", number(dates));
        result.kvs.emplace_back("dates_below_min_names", number(dates_below_min_names));
        result.kvs.emplace_back("modulo_fallbacks", number(modulo_fallbacks));
        result.kvs.emplace_back("acceptance", "sign-and-shape-only-not-accepted-alpha");
        return Ok(std::move(result));
      } catch (const std::exception &error) {
        // An exception here would otherwise unwind past the terminal append.
        return Err(ErrorCode::IoError, "equity ic: " + std::string(error.what()));
      }
    }();
    // ---- the terminal line, on EVERY path (§4.5, §11.9 C-1) ----------------
    // Building the entry allocates strings; an exception here would unwind past
    // the append and leave the pre-registration dangling, so it is confined and
    // reported as its own error (final branch review, residual C-1 shape).
    using TerminalEntry = decltype(base_entry(cfg, profile, marks, bound.context,
                                              bound.evaluation, bound.combo, trial_id, dates));
    auto terminal = [&]() -> Result<TerminalEntry> {
      try {
        auto done = base_entry(cfg, profile, marks, bound.context, bound.evaluation,
                               bound.combo, trial_id, dates);
        done.runtime.wall_seconds = published_seconds.value_or(elapsed_seconds(started));
        if (outcome) {
            done.status = "completed";
            done.result.outcome = "completed";
            done.result.manifest_sha256 = manifest_sha;
            return Ok(std::move(done));
        }
        done.status = "failed";
        done.result.outcome = "failed";
        // §4.5 requires a digest on a terminal line, so failure.json is
        // published HERE, before the append, rather than by the caller.
        std::string failure_text;
        try {
            attempt["status"] = "failed";
            attempt["error"] = outcome.error().to_string();
            attempt["runtime_seconds"] = done.runtime.wall_seconds.value_or(0.0);
            failure_text = attempt.dump(2) + "\n";
        } catch (const std::exception &) {
            failure_text = "{\"schema\":\"atx-equity-ic-attempt-v1\",\"status\":\"failed\"}\n";
        }
        const auto failure_sha = atx::core::sha256_hex(failure_text);
        if (failure_sha) {
            done.result.failure_sha256 = *failure_sha;
        } else {
            done.result.failure_sha256 = std::string(kTrialLedgerGenesisSha256);
            done.notes += " Failure digest unavailable; the zero digest is a placeholder.";
        }
        // A write failure must not stop the terminal line: the ledger is the
        // artifact that cannot be repaired afterwards.
        try { (void)write_text(directory, "failure.json", failure_text); }
        catch (const std::exception &) { /* the ledger line is the obligation */ }
        return Ok(std::move(done));
      } catch (const std::exception &error) {
        return Err(ErrorCode::Internal,
                   "equity ic: terminal ledger line could not be built: " +
                       std::string(error.what()));
      }
    }();
    if (!terminal) {
        std::string message = terminal.error().to_string() +
                              "; the pre-registration line is left without a terminal line";
        if (!outcome) message += "; original failure: " + outcome.error().to_string();
        try {
            attempt["status"] = "failed";
            attempt["ledger_terminal_error"] = message;
            (void)write_text(directory, "failure.json", attempt.dump(2) + "\n");
        } catch (const std::exception &) { /* the returned error still names it */ }
        return Err(terminal.error().code(), message);
    }
    auto &done = *terminal;
    auto appended = append_trial_with_retry(profile.ledger_path, done);
    if (!appended) {
        // Nothing further can be done; this error replaces the returned one.
        std::string message =
            "equity ic: completion ledger append failed: " + appended.error().to_string();
        if (!outcome) message += "; original failure: " + outcome.error().to_string();
        try {
            attempt["status"] = "failed";
            attempt["ledger_completion_error"] = appended.error().to_string();
            (void)write_text(directory, "failure.json", attempt.dump(2) + "\n");
        } catch (const std::exception &) { /* the returned error still names it */ }
        return Err(appended.error().code(), message);
    }
    return outcome;
      } catch (const std::exception &error) {
        return Err(ErrorCode::IoError, "equity ic: registered attempt failed: " + std::string(error.what()));
      }
    }();
    if (epoch && reservation) {
        // The immutable manifest binds the reservation head. The terminal binds
        // that manifest (or a precise failure receipt); the new head is separate.
        std::string result_sha = manifest_sha;
        if (!registered_run) {
            const Json failure{{"schema", "atx-equity-ic-e2-failure-v1"},
                {"token", epoch_token}, {"reservation_head_sha256", reservation->reservation_head_sha256},
                {"error", registered_run.error().to_string()}, {"manifest_sha256", manifest_sha}};
            ATX_TRY(result_sha, atx::core::sha256_hex(failure.dump()));
            // Failure publication is secondary to recording the terminal event.
            const auto written = write_text(directory, "epoch-failure.json", failure.dump(2) + "\n");
            if (!written) attempt["epoch_failure_publication_error"] = written.error().to_string();
        }
        const auto finalized = epoch->finish_attempt(epoch_token, registered_run
            ? eval::TrialEpochTerminal::Completed : eval::TrialEpochTerminal::Failed, result_sha);
        if (!finalized) return Err(finalized.error().code(),
            "equity ic: E2 terminal append failed; reservation remains counted: " +
            finalized.error().to_string() + (registered_run ? "" : "; original: " + registered_run.error().to_string()));
        const Json anchor{{"schema", "atx-trial-epoch-external-anchor-v1"},
            {"epoch", profile.prereg->epoch}, {"head_sha256", epoch->head_sha256()},
            {"counts", epoch_counts(epoch->counts())}, {"token", epoch_token},
            {"terminal_result_sha256", result_sha},
            {"trust", "export separately; no automatic trust in a mutable adjacent sidecar"}};
        ATX_TRY(auto anchor_file, write_text(directory, "epoch-anchor.json", anchor.dump(2) + "\n"));
        (void)anchor_file;
        if (registered_run) {
            std::error_code ec;
            if (!fs::remove(directory / ".pending", ec) || ec)
                return Err(ErrorCode::IoError, "equity ic: E2 completed but pending marker removal failed");
            registered_run->kvs.emplace_back("epoch_head_sha256", epoch->head_sha256());
            registered_run->kvs.emplace_back("epoch_known_unique_cells", number(epoch->counts().unique_cells));
            registered_run->kvs.emplace_back("epoch_new_unique_cells", number(reservation->new_unique_cells));
            registered_run->kvs.emplace_back("epoch_attempts", number(epoch->counts().attempts));
        }
    }
    return registered_run;
}

} // namespace

const EquityTerminalEvent *EquityTerminalReturnTable::find(atx::i64 security_id) const noexcept {
    for (const auto &event : events) {
        if (event.security_id == security_id) return &event;
    }
    return nullptr;
}

EquityTerminalReturnTable equity_ic_frozen_terminal_table() {
    EquityTerminalReturnTable table;
    table.source = std::string(kFrozenTerminalTableSource);
    for (const auto &frozen : kTerminalEvents) {
        EquityTerminalEvent event;
        event.security_id = frozen.security_id;
        event.terminal_value = frozen.consideration;
        event.special_dividend = frozen.special_dividend;
        if (!frozen.record_date.empty()) {
            // A frozen literal date that cannot be parsed is a defect in this file, not
            // caller input; entitlement is then refused rather than assumed.
            const auto record_ns = atx::engine::data::detail::date_to_nanos(frozen.record_date);
            if (record_ns) event.record_session_key = *record_ns;
            event.record_date = std::string(frozen.record_date);
        }
        event.evidenced = true;
        event.source = std::string(kAuditSource);
        table.events.push_back(std::move(event));
    }
    EquityTerminalEvent pcs;
    pcs.security_id = kPcsSecurityId;
    pcs.evidenced = false; // Ruling AR-1: flagged terminal WITHOUT evidence, never priced.
    pcs.source = "atx-engine/reviews/2026-09-20-equity-required-marks-audit.md (PCS, AR-1)";
    table.events.push_back(std::move(pcs));
    return table;
}

atx::f64 equity_terminal_cash(const EquityTerminalEvent &event, atx::i64 session_key_ns) noexcept {
    if (!event.evidenced) return 0.0;
    const atx::f64 base = event.terminal_value.value_or(0.0);
    // Ruling AR-2: the special dividend only for a cell observed at or before the
    // record date; no record date means no entitlement to infer.
    if (event.special_dividend > 0.0 && event.record_session_key &&
        session_key_ns <= *event.record_session_key) {
        return base + event.special_dividend;
    }
    return base;
}

atx::f64 equity_ic_terminal_value(atx::i64 security_id, atx::i64 session_key_ns) {
    static const EquityTerminalReturnTable frozen = equity_ic_frozen_terminal_table();
    const auto *event = frozen.find(security_id);
    return event == nullptr ? 0.0 : equity_terminal_cash(*event, session_key_ns);
}

namespace {

std::vector<std::string_view> split_fields(std::string_view line) {
    std::vector<std::string_view> fields;
    for (std::size_t pos = 0;;) {
        const auto comma = line.find(',', pos);
        fields.push_back(line.substr(pos, comma == std::string_view::npos ? line.npos
                                                                          : comma - pos));
        if (comma == std::string_view::npos) break;
        pos = comma + 1;
    }
    return fields;
}

Result<std::optional<atx::f64>> optional_real(std::string_view text, std::string_view column) {
    if (text.empty()) return Ok(std::optional<atx::f64>{});
    atx::f64 value = 0.0;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() ||
        !std::isfinite(value)) {
        return Err(ErrorCode::ParseError, "terminal table: non-finite or malformed " +
                                              std::string(column) + " '" + std::string(text) + "'");
    }
    return Ok(std::optional<atx::f64>{value});
}

} // namespace

Result<EquityTerminalReturnTable> parse_equity_terminal_table(std::string_view csv_text) {
    constexpr std::string_view kHeader =
        "security_id,terminal_value,terminal_return,special_dividend,record_date,evidenced,source";
    EquityTerminalReturnTable table;
    std::set<atx::i64> seen;
    atx::usize line_number = 0;
    for (std::size_t pos = 0; pos < csv_text.size();) {
        auto end = csv_text.find('\n', pos);
        if (end == std::string_view::npos) end = csv_text.size();
        auto line = csv_text.substr(pos, end - pos);
        pos = end + 1;
        ++line_number;
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        const auto where = "terminal table line " + number(line_number) + ": ";
        if (line_number == 1) {
            if (line != kHeader) {
                return Err(ErrorCode::ParseError, where + "header must be exactly " +
                                                      std::string(kHeader));
            }
            continue;
        }
        if (line.empty()) continue;
        if (line.find('"') != std::string_view::npos) {
            return Err(ErrorCode::ParseError, where + "quoted fields are not supported");
        }
        const auto fields = split_fields(line);
        if (fields.size() != 7) {
            return Err(ErrorCode::ParseError, where + "expected 7 fields");
        }
        EquityTerminalEvent event;
        const auto id_text = fields[0];
        const auto id_parsed =
            std::from_chars(id_text.data(), id_text.data() + id_text.size(), event.security_id);
        if (id_parsed.ec != std::errc{} || id_parsed.ptr != id_text.data() + id_text.size() ||
            event.security_id <= 0 || number(event.security_id) != id_text) {
            return Err(ErrorCode::ParseError, where + "noncanonical security_id");
        }
        if (!seen.insert(event.security_id).second) {
            return Err(ErrorCode::ParseError, where + "duplicate security_id");
        }
        ATX_TRY(event.terminal_value, optional_real(fields[1], "terminal_value"));
        ATX_TRY(event.terminal_return, optional_real(fields[2], "terminal_return"));
        ATX_TRY(const auto dividend, optional_real(fields[3], "special_dividend"));
        event.special_dividend = dividend.value_or(0.0);
        if (fields[5] == "true") {
            event.evidenced = true;
        } else if (fields[5] != "false") {
            return Err(ErrorCode::ParseError, where + "evidenced must be true or false");
        }
        event.source = std::string(fields[6]);
        const bool has_value = event.terminal_value.has_value();
        const bool has_return = event.terminal_return.has_value();
        if (event.evidenced ? (has_value == has_return) : (has_value || has_return)) {
            return Err(ErrorCode::ParseError,
                       where + "an evidenced row needs exactly one of terminal_value / "
                               "terminal_return; an unevidenced row neither");
        }
        if ((has_value && !(*event.terminal_value > 0.0)) ||
            (has_return && !(*event.terminal_return > -1.0)) || event.special_dividend < 0.0) {
            return Err(ErrorCode::ParseError,
                       where + "terminal_value must be > 0, terminal_return > -1, dividend >= 0");
        }
        if (!fields[4].empty()) {
            const auto record = atx::engine::data::detail::date_to_nanos(fields[4]);
            if (!record) return Err(ErrorCode::ParseError, where + "record_date is not a date");
            event.record_session_key = *record;
            event.record_date = std::string(fields[4]);
        }
        if (event.special_dividend > 0.0 && !event.record_session_key) {
            return Err(ErrorCode::ParseError,
                       where + "a special dividend needs its record_date (ruling AR-2)");
        }
        table.events.push_back(std::move(event));
    }
    if (line_number == 0) return Err(ErrorCode::ParseError, "terminal table: empty file");
    return Ok(std::move(table));
}

Result<EquityTerminalReturnTable> load_equity_terminal_table(const std::string &path) {
    ATX_TRY(auto text, read_file(path, 64U * 1024U * 1024U));
    ATX_TRY(auto table, parse_equity_terminal_table(text));
    ATX_TRY(table.sha256, atx::core::sha256_hex(text));
    table.source = "terminal-return-table:" + fs::path(path).filename().string();
    return Ok(std::move(table));
}

Result<StageResult> run_equity_ic(const RunConfig &config) {
    bool reserved = false;
    // Set the instant the pre-registration line lands. Past that point `execute`
    // owns both `failure.json` and the terminal ledger line (§11.9 C-1), so this
    // function must not write a second, digest-less failure.json over it.
    bool registered = false;
    const fs::path directory(config.out);
    Json attempt{{"schema", "atx-equity-ic-attempt-v1"}, {"status", "started"}};
    auto work = [&]() -> Result<StageResult> {
        ATX_TRY(auto profile, resolve(config));
        attempt["recipe"] = profile.recipe;
        ATX_TRY_VOID(reserve_directory(directory));
        reserved = true;
        return execute(config, profile, attempt, directory, registered);
    };
    Result<StageResult> result = [&]() -> Result<StageResult> {
        try { return work(); }
        catch (const std::exception &error) {
            return Err(ErrorCode::IoError, "equity ic: " + std::string(error.what()));
        }
    }();
    if (!result && reserved && !registered) {
        attempt["status"] = "failed";
        attempt["error"] = result.error().to_string();
        try { (void)write_text(directory, "failure.json", attempt.dump(2) + "\n"); }
        catch (const std::exception &) { /* Original failure and .pending remain authoritative. */ }
    }
    return result;
}

} // namespace atx::impl
