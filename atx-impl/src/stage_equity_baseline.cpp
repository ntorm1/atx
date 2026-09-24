#include "stage_equity_baseline.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "artifacts.hpp"
#include "atx/core/sha256.hpp"
#include "atx/engine/alpha/streams.hpp"
#include "atx/engine/combine/combiner.hpp"
#include "atx/engine/data/orats_history.hpp"
#include "equity_baseline_views.hpp"
#include "panel_artifact.hpp"
#include "research_sim.hpp"
#include "stage_data_provenance.hpp"

namespace atx::impl {
namespace {
namespace fs = std::filesystem;
namespace alpha = atx::engine::alpha;
using Json = nlohmann::json;
using atx::core::Err;
using atx::core::ErrorCode;
using atx::core::Ok;
using atx::core::Result;
using atx::core::Status;
constexpr atx::u64 kOverheadReserve = 512'000'000;
constexpr std::string_view kDomain = "atx-equity-baseline-v1\n";

template <class Number> std::string number(Number value) {
    std::array<char, 64> bytes{};
    const auto result = std::to_chars(bytes.data(), bytes.data() + bytes.size(), value);
    if (result.ec != std::errc{}) throw std::runtime_error("baseline: numeric formatting failed");
    return {bytes.data(), result.ptr};
}

Result<atx::u64> add(atx::u64 a, atx::u64 b) {
    if (b > std::numeric_limits<atx::u64>::max() - a) {
        return Err(ErrorCode::OutOfRange, "equity baseline: memory estimate overflow");
    }
    return Ok(a + b);
}

Result<atx::u64> multiply(atx::u64 a, atx::u64 b) {
    if (a != 0 && b > std::numeric_limits<atx::u64>::max() / a) {
        return Err(ErrorCode::OutOfRange, "equity baseline: memory estimate overflow");
    }
    return Ok(a * b);
}

struct Profile {
    EquityBaselineConfig views;
    RunConfig replay;
    Json recipe;
};

Result<Profile> resolve(const RunConfig &cfg) {
    const std::set<std::string> allowed{"panel", "out", "evaluation-start", "evaluation-end",
        "max-working-bytes", "report-aum", "replay-execution-delay", "replay-trade-bps",
        "replay-annual-borrow-bps", "replay-day-basis", "quiet", "digest-only", "config",
        "min-dollar-adv", "dollar-adv-window"};
    for (const auto &flag : cfg.set_flags) {
        if (!allowed.contains(flag)) {
            return Err(ErrorCode::InvalidArgument, "equity baseline: unsupported flag --" + flag);
        }
    }
    // Programmatic callers also must not silently request a different book model.
    if (cfg.allow_unidentified_panels || cfg.cost_bps != 0 || cfg.borrow_bps != 0 ||
        cfg.gross != 0 || cfg.name_cap != 0 || !cfg.rebalance.empty() || cfg.trade_rate != 1 ||
        cfg.risk_aversion != 0 || cfg.turnover_penalty != 0 || !cfg.method.empty() ||
        cfg.conviction || cfg.kelly_fraction != 0 || cfg.sector_neutral || cfg.industry_neutral ||
        cfg.group_neutralize || cfg.risk_model != "diagonal" || cfg.dead_alpha_factors ||
        cfg.metabook || cfg.gp_trading || cfg.participation_cap != 0 || cfg.book_turnover_gate != 0 ||
        cfg.fit_begin != 0 || cfg.fit_end != 0 || cfg.combine_holdout_frac != 0 ||
        cfg.weight_transform != "rank" || cfg.winsorize_limit != 0.025 || cfg.gross_leverage != 1 ||
        !cfg.seed_exprs.empty() || !cfg.library_dir.empty() || cfg.gated || cfg.walk_forward != 0 ||
        cfg.corr_penalty != 0 || cfg.capacity_floor != 0 || !cfg.combo.empty() || !cfg.books.empty()) {
        return Err(ErrorCode::InvalidArgument,
                   "equity baseline: unsupported override of the fixed unfit weekly shaping recipe");
    }
    if (cfg.panel.empty() || cfg.out.empty() || cfg.equity_evaluation_start.empty() ||
        cfg.equity_evaluation_end.empty() || cfg.equity_max_working_bytes <= kOverheadReserve) {
        return Err(ErrorCode::InvalidArgument,
                   "equity baseline: panel, fresh out, explicit evaluation dates and adequate budget required");
    }
    using atx::engine::data::detail::date_to_nanos;
    const auto begin = date_to_nanos(cfg.equity_evaluation_start);
    const auto end = date_to_nanos(cfg.equity_evaluation_end);
    if (!begin || !end || *begin >= *end || *begin < *date_to_nanos("2013-04-01") ||
        *end > *date_to_nanos("2020-01-01")) {
        return Err(ErrorCode::InvalidArgument,
                   "equity baseline: evaluation must be within training [2013-04-01,2020-01-01)");
    }
    Profile profile;
    profile.views.evaluation = {*begin, *end};
    profile.views.observation_basis =
        EquityBaselineObservationBasis::ArchiveRawVolumeAndPointwiseAdjustedCloseV1;
    // Checkpoint 19 liquidity floor (R18-5): an admission predicate, never a trial.
    if (!std::isfinite(cfg.equity_min_dollar_adv) || cfg.equity_min_dollar_adv < 0.0) {
        return Err(ErrorCode::InvalidArgument, "equity baseline: --min-dollar-adv must be >= 0");
    }
    if (cfg.equity_dollar_adv_window < 1) {
        return Err(ErrorCode::InvalidArgument, "equity baseline: --dollar-adv-window must be >= 1");
    }
    profile.views.min_dollar_adv = cfg.equity_min_dollar_adv;
    profile.views.dollar_adv_window = cfg.equity_dollar_adv_window;
    profile.replay.report_aum = cfg.set_flags.contains("report-aum") || cfg.report_aum != 1e9
        ? cfg.report_aum : 100e6;
    profile.replay.replay_execution_delay = cfg.replay_execution_delay;
    profile.replay.replay_trade_bps = cfg.set_flags.contains("replay-trade-bps") ||
        cfg.replay_trade_bps != 0 ? cfg.replay_trade_bps : 5;
    profile.replay.replay_annual_borrow_bps = cfg.set_flags.contains("replay-annual-borrow-bps") ||
        cfg.replay_annual_borrow_bps != 0 ? cfg.replay_annual_borrow_bps : 365;
    profile.replay.replay_day_basis = cfg.replay_day_basis;
    // Deliberately supplied zeros and profile defaults are both explicit at the report seam.
    profile.replay.set_flags = {"replay-trade-bps", "replay-annual-borrow-bps"};
    if (!std::isfinite(profile.replay.report_aum) || profile.replay.report_aum <= 0 ||
        !std::isfinite(profile.replay.replay_trade_bps) || profile.replay.replay_trade_bps < 0 ||
        !std::isfinite(profile.replay.replay_annual_borrow_bps) ||
        profile.replay.replay_annual_borrow_bps < 0 ||
        (cfg.replay_day_basis != 360 && cfg.replay_day_basis != 365)) {
        return Err(ErrorCode::InvalidArgument, "equity baseline: invalid replay NAV/rate/day basis");
    }
    ATX_TRY(auto executable, current_executable_sha256());
    Json signals = Json::array();
    for (atx::usize a = 0; a < kEquityBaselineDsl.size(); ++a) {
        ATX_TRY(auto sha, atx::core::sha256_hex(kEquityBaselineDsl[a]));
        signals.push_back(Json{{"name", kEquityBaselineSignalNames[a]}, {"dsl", kEquityBaselineDsl[a]},
                               {"dsl_sha256", sha}, {"constant_weight", 0.5}});
    }
    profile.recipe = Json{{"profile", "slow-momentum-equal-weekly-shaping-v1"},
        {"trial_purpose", "training-only-software-and-book-diagnostic"},
        {"signals", signals}, {"fit_kind", "unfit-constant-weights"}, {"fitted_observations", 0},
        {"evaluation_start", cfg.equity_evaluation_start}, {"evaluation_end_exclusive", cfg.equity_evaluation_end},
        {"warmup_observations", kEquityBaselineWarmup}, {"signal_readiness", "both-finite-before-ranking"},
        {"feature_observation_basis", "finite-positive-adjusted-close-and-raw-close-nonnegative-raw-volume"},
        {"decision_eligibility", "inherited-context-mask-intersect-common-readiness"},
        {"inherited_universe_caveat", "context may require raw-close-times-shares validity and retain warmup-union columns"},
        {"context_profile", {{"top_n_by_adv", 1000}, {"min_adv_usd", 20e6},
            {"min_raw_price_exclusive", 5.0}, {"adv_window_bars", 21}, {"min_mktcap_usd", 0.0},
            {"require_sector", false}, {"current_session_data_included", true}, {"compact_to_universe", true}}},
        {"weight_policy", {{"transform", "rank"}, {"winsorize_limit", 0.025},
            {"industry_neutral", false}, {"gross_leverage", 1.0}}},
        {"combination", "equal-weighted-constituent-target-position-streams"},
        {"allocator", "existing-position-mode-shape_book"}, {"target_gross", 1.0}, {"name_cap", 0.01},
        {"target_net_qualification_tolerance", 1e-10}, {"constraint_tolerance", 1e-10},
        {"decision_schedule", "every-five-evaluation-observations-starting-at-zero"},
        {"trade_rate", 1.0}, {"hysteresis", "none"}, {"planning_cost_bps", 0},
        {"initial_nav", profile.replay.report_aum}, {"initial_holdings", "all-cash-at-evaluation-start"},
        {"execution_delay_observations", number(profile.replay.replay_execution_delay)},
        {"trade_bps_per_absolute_dollar", profile.replay.replay_trade_bps},
        {"annual_borrow_bps", profile.replay.replay_annual_borrow_bps},
        {"borrow_day_basis", profile.replay.replay_day_basis},
        {"execution_timing", "hypothetical-observation-close-not-publication-certification"},
        {"held_missing_price_policy", "reject-entire-run-no-window-shortening"},
        {"terminal_policy", "valuation-only-no-trade-no-liquidation"},
        {"legacy_post_fit_boundary", "zero-is-unfit-constant-weights-never-heldout-selection"},
        {"replay_liquidity_context", "evaluation-only-prior-21-observations-first-21-trade-ADV-may-be-unknown"},
        {"raw_data_economics", "unverified"}, {"historical_availability", "unknown-archive-snapshot"},
        {"instrument_type_eligibility", "unknown"}, {"sector_beta_risk", "unqualified"},
        {"locates_impact_settlement", "not-modeled"}, {"strategy_capacity", "unavailable"},
        {"investment_qualification", "unverified-no-promotion"}, {"live_orders_authorized", false},
        {"max_working_bytes", number(cfg.equity_max_working_bytes)},
        {"producer_executable_sha256", executable.empty() ? "unknown" : executable}};
    if (profile.views.min_dollar_adv > 0.0) {
        // Added post hoc so a run without the floor keeps a byte-identical recipe.
        profile.recipe["liquidity_floor"] =
            Json{{"min_dollar_adv", profile.views.min_dollar_adv},
                 {"dollar_adv_window", number(profile.views.dollar_adv_window)},
                 {"rule", "admitted only if mean(raw_close * volume) over the trailing "
                          "dollar_adv_window sessions ending at t (all finite, > 0) >= "
                          "min_dollar_adv; checkpoint 19, ruling R18-5"}};
    }
    return Ok(std::move(profile));
}

Status reserve_directory(const fs::path &directory) {
    std::error_code ec;
    if (!directory.parent_path().empty()) fs::create_directories(directory.parent_path(), ec);
    if (ec || !fs::create_directory(directory, ec) || ec) {
        return Err(ErrorCode::AlreadyExists, "equity baseline: output root must not exist");
    }
    if (!fs::create_directory(directory / ".pending", ec) || ec) {
        return Err(ErrorCode::IoError, "equity baseline: cannot reserve publication");
    }
    return Ok();
}

Result<Json> write_text(const fs::path &directory, const std::string &name, std::string_view text) {
    const auto partial = directory / (name + ".partial");
    const auto final = directory / name;
    std::ofstream out(partial, std::ios::binary);
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    out.close();
    if (!out) return Err(ErrorCode::IoError, "equity baseline: cannot write " + name);
    ATX_TRY(auto sha, atx::core::sha256_hex(text));
    std::error_code ec;
    fs::create_hard_link(partial, final, ec);
    if (ec) return Err(ErrorCode::IoError, "equity baseline: cannot publish " + name);
    fs::remove(partial, ec);
    if (ec) return Err(ErrorCode::IoError, "equity baseline: cannot remove published partial");
    return Ok(Json{{"filename", name}, {"sha256", sha}, {"size_bytes", number(text.size())}});
}

Result<std::string> read_small(const fs::path &path) {
    std::error_code ec;
    const auto length = fs::file_size(path, ec);
    if (ec || length == 0 || length > 16U * 1024U * 1024U) {
        return Err(ErrorCode::IoError, "equity baseline: invalid companion size");
    }
    std::ifstream in(path, std::ios::binary);
    std::string text(static_cast<atx::usize>(length), '\0');
    if (!in.read(text.data(), static_cast<std::streamsize>(length)) ||
        in.peek() != std::char_traits<char>::eof()) {
        return Err(ErrorCode::IoError, "equity baseline: cannot read companion snapshot");
    }
    return Ok(std::move(text));
}

Json strict_json(const std::string &text) {
    std::vector<std::set<std::string>> keys;
    auto callback = [&](int depth, Json::parse_event_t event, Json &parsed) {
        if (depth > 64) throw std::invalid_argument("baseline: JSON nesting exceeds 64");
        if (event == Json::parse_event_t::object_start) keys.emplace_back();
        if (event == Json::parse_event_t::key && !keys.back().insert(parsed.get<std::string>()).second) {
            throw std::invalid_argument("baseline: duplicate JSON key");
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
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || number(result) != text) {
        return Err(ErrorCode::ParseError, "equity baseline: noncanonical context window");
    }
    return Ok(result);
}

// Checkpoint 14 screen: the daily ADV-21 top-1000 above $20M ADV and $5. Checkpoint 16
// (design 2026-09-20-iteration16-alpha-scorecard-design.md, R16-5) replaces the screen
// with a point-in-time membership allow-list and the PIT floors; the membership rule
// string is the recipe's own declaration of that mode and nothing in between is accepted.
bool universe_screen_is_cp14(const Json &universe) {
    return universe.at("top_n_by_adv") == 1000 && universe.at("min_adv_usd") == 20e6 &&
           universe.at("min_raw_price_exclusive") == 5.0;
}

bool universe_screen_is_cp16_membership(const Json &recipe, const Json &universe) {
    return recipe.contains("membership_rule") &&
           recipe.at("membership_rule") == "year-union-plus-last-prior-rebalance;not-as-of" &&
           recipe.contains("universe_membership_sha256") && recipe.contains("universe_cut") &&
           universe.at("top_n_by_adv") == 0 && universe.at("min_adv_usd") == 0.0 &&
           universe.at("min_raw_price_exclusive") == 1.0;
}

Status require_context_recipe(const std::string &text, const Profile &profile) {
    const auto recipe = strict_json(text);
    const auto &universe = recipe.at("universe");
    if (recipe.at("version") != "tickerhistory-panel-v2-identified" ||
        recipe.at("research_ohlc") != "raw-OHLC*cumulReturnFactor-pointwise" ||
        recipe.at("raw_close") != "unadjusted-as-traded" ||
        recipe.at("volume") != "raw-reported-volume-no-total-return-factor-rescaling" ||
        recipe.at("universe").at("adv_basis") != "raw_close*raw_volume" ||
        recipe.at("universe").at("top_n_tie_break") != "original-instrument-index" ||
        !(universe_screen_is_cp14(universe) || universe_screen_is_cp16_membership(recipe, universe)) ||
        recipe.at("universe").at("adv_window_bars") != 21 ||
        recipe.at("universe").at("min_mktcap_usd") != 0.0 ||
        recipe.at("universe").at("require_sector") != false ||
        recipe.at("universe").at("current_session_data_included") != true ||
        recipe.at("universe").at("adv_missing") != "full-trailing-window-any-NaN-invalid" ||
        recipe.at("compact_to_universe") != true ||
        recipe.at("compaction") != "keep-original-order-if-ever-in-universe-in-selected-window" ||
        recipe.at("augmentation").at("enabled") != false ||
        recipe.at("augmentation").at("adv_windows") != Json::array()) {
        return Err(ErrorCode::InvalidArgument, "equity baseline: unsupported context field/universe recipe");
    }
    ATX_TRY(auto start, session_number(recipe.at("start_inclusive_nanos")));
    ATX_TRY(auto end, session_number(recipe.at("end_exclusive_nanos")));
    const auto training_end = *atx::engine::data::detail::date_to_nanos("2020-01-01");
    if (start >= end || start > profile.views.evaluation.begin_session_key ||
        end < profile.views.evaluation.end_exclusive_session_key || end > training_end) {
        return Err(ErrorCode::InvalidArgument,
                   "equity baseline: context must prove requested end coverage and exclude sealed dates");
    }
    return Ok();
}

struct Inputs { PanelArtifactReceipt evaluation; PanelArtifactReceipt combo; Json readiness; Json memory;
                bool membership_mode = false; };

Result<Inputs> make_inputs(const RunConfig &cfg, Profile &profile, Json &attempt,
                           Json &files, const fs::path &directory) {
    std::error_code ec;
    const auto source_bytes = fs::file_size(cfg.panel, ec);
    if (ec) return Err(ErrorCode::IoError, "equity baseline: cannot stat context");
    ATX_TRY(auto read_arrays, multiply(source_bytes, 2));
    ATX_TRY(auto read_peak, add(read_arrays, kOverheadReserve));
    if (read_peak > cfg.equity_max_working_bytes) {
        return Err(ErrorCode::OutOfRange, "equity baseline: context snapshot reader exceeds memory budget");
    }
    // Metadata preflight rejects a declared future/sealed context before loading its
    // numeric payload. The identified reader then verifies the same exact recipe.
    ATX_TRY(auto metadata, read_small(cfg.panel + ".manifest.json"));
    const auto declared = strict_json(metadata);
    const auto declared_recipe = declared.at("recipe").get<std::string>();
    ATX_TRY_VOID(require_context_recipe(declared_recipe, profile));
    const auto declared_json = strict_json(declared_recipe);
    const bool membership_mode =
        universe_screen_is_cp16_membership(declared_json, declared_json.at("universe"));
    ATX_TRY(auto declared_size, session_number(declared.at("payload").at("size_bytes")));
    if (declared_size < 0 || static_cast<atx::u64>(declared_size) != source_bytes) {
        return Err(ErrorCode::InvalidArgument, "equity baseline: preflight payload extent changed");
    }
    const atx::u64 max_payload = std::min<atx::u64>(source_bytes,
        (cfg.equity_max_working_bytes - kOverheadReserve) / 2);
    ATX_TRY(auto context, read_panel_artifact(cfg.panel, max_payload));
    attempt["source_context_artifact_id"] = context.artifact_id;
    attempt["source_context_payload_sha256"] = context.payload_sha256;
    if (context.identity.recipe != declared_recipe ||
        context.artifact_id != declared.at("artifact_id").get<std::string>() ||
        context.payload_sha256 != declared.at("payload").at("sha256").get<std::string>() ||
        context.identity.instrument_namespace != kSpiderRockSecurityIdNamespace) {
        return Err(ErrorCode::InvalidArgument, "equity baseline: changed/unsupported identified context");
    }
    const auto context_recipe = strict_json(context.identity.recipe);
    ATX_TRY(auto source_begin, session_number(context_recipe.at("start_inclusive_nanos")));
    ATX_TRY(auto source_end, session_number(context_recipe.at("end_exclusive_nanos")));
    if (context.identity.session_keys.empty() || context.identity.session_keys.front() < source_begin ||
        context.identity.session_keys.back() >= source_end) {
        return Err(ErrorCode::InvalidArgument, "equity baseline: context axes violate declared window");
    }
    profile.recipe["inherited_context_recipe"] = context.identity.recipe;
    // Checkpoint 16 membership contexts admit thin names with missing closes; the book
    // replay's held_missing_price_policy would reject the whole run, and the downstream
    // equity-ic stage consumes only evaluation.bin / combo.bin. The replay is therefore
    // not attempted and the manifest says so; nothing here is a book result.
    profile.recipe["membership_mode"] = membership_mode;
    profile.recipe["replay"] = membership_mode
        ? "skipped-membership-context-signals-only-no-book-result" : "full-book-replay";
    attempt["recipe"] = profile.recipe;
    ATX_TRY(auto request, write_text(directory, "request.json", attempt.dump(2) + "\n"));
    files.push_back(std::move(request));
    profile.views.max_additional_bytes = cfg.equity_max_working_bytes - kOverheadReserve;
    ATX_TRY(auto plan, plan_equity_baseline(context, profile.views));
    ATX_TRY(auto blend_extra, multiply(plan.evaluation_cells, 64));
    ATX_TRY(auto cross_section_extra, multiply(context.panel.instruments(), 256));
    ATX_TRY(auto feature_arrays, add(source_bytes, plan.additional_array_bytes));
    ATX_TRY(feature_arrays, add(feature_arrays, blend_extra));
    ATX_TRY(feature_arrays, add(feature_arrays, cross_section_extra));
    ATX_TRY(auto feature_peak, add(feature_arrays, kOverheadReserve));
    ATX_TRY(auto downstream_arrays, multiply(plan.evaluation_cells, 128));
    ATX_TRY(downstream_arrays, add(downstream_arrays, cross_section_extra));
    ATX_TRY(auto downstream_peak, add(downstream_arrays, kOverheadReserve));
    const auto estimated_peak = std::max({read_peak, feature_peak, downstream_peak});
    if (estimated_peak > cfg.equity_max_working_bytes) {
        return Err(ErrorCode::OutOfRange,
                   "equity baseline: full warmup/feature/stream/replay memory estimate exceeds budget");
    }
    ATX_TRY(auto view, evaluate_equity_baseline(context, profile.views));
    const auto dates = view.panel.dates();
    const auto instruments = view.panel.instruments();
    PanelIdentity identity = context.identity;
    identity.session_keys = view.session_keys;
    identity.parents = {{"source-context", context.artifact_id}};
    for (const auto &signal : profile.recipe["signals"]) {
        identity.parents.push_back({signal.at("name").get<std::string>() + "-dsl",
                                    signal.at("dsl_sha256").get<std::string>()});
    }
    identity.recipe = profile.recipe.dump();
    ATX_TRY(auto evaluation, write_panel_artifact(view.panel, (directory / "evaluation.bin").string(), identity));
    const atx::engine::combine::Combination constant{{0.5, 0.5}, 0, 0};
    ATX_TRY(auto streams, alpha::extract_streams(view.signals, atx::engine::WeightPolicy{},
                                                view.panel, frictionless_sim()));
    std::vector<atx::f64> combined(plan.evaluation_cells, std::numeric_limits<double>::quiet_NaN());
    std::vector<std::uint8_t> mask(plan.evaluation_cells);
    std::ostringstream readiness;
    readiness << "evaluation_observation,session_key_ns,context_observation,ready_names,admitted_names\n";
    atx::usize empty_rows = 0;
    atx::usize minimum_admitted = instruments;
    for (atx::usize d = 0; d < dates; ++d) {
        atx::usize admitted = 0;
        const auto first = streams.positions(0, d);
        const auto second = streams.positions(1, d);
        for (atx::usize i = 0; i < instruments; ++i) {
            if (!view.panel.in_universe(d, i)) continue;
            mask[d * instruments + i] = 1;
            combined[d * instruments + i] = constant.weights[0] * first[i] + constant.weights[1] * second[i];
            ++admitted;
        }
        minimum_admitted = std::min(minimum_admitted, admitted);
        if (admitted == 0) ++empty_rows;
        readiness << d << ',' << view.session_keys[d] << ',' << view.context_rows[d] << ','
                  << view.ready_by_observation[d] << ',' << admitted << '\n';
    }
    ATX_TRY(auto readiness_file, write_text(directory, "readiness.csv", readiness.str()));
    files.push_back(std::move(readiness_file));
    std::string weights = "method=equal\nfit_kind=unfit-constant-weights\nfit_begin=0\nfit_end=0\n";
    for (atx::usize a = 0; a < 2; ++a) {
        weights += "w[" + number(a) + "]=0.5 " + std::string(kEquityBaselineSignalNames[a]) + "\n";
    }
    ATX_TRY(auto weights_file, write_text(directory, "combo.bin.weights.txt", weights));
    ATX_TRY(auto fit_file, write_text(directory, "combo.bin.meta", "n_periods=" + number(dates) +
        "\nfit_begin=0\nfit_end=0\nholdout_begin=0\nholdout_frac=0\n"
        "fit_kind=unfit-constant-weights\nholdout_interpretation=not-heldout-selection\n"));
    identity.parents.push_back({"research", evaluation.artifact_id});
    identity.parents.push_back({"weights", weights_file.at("sha256").get<std::string>()});
    identity.parents.push_back({"fit-boundary", fit_file.at("sha256").get<std::string>()});
    files.push_back(std::move(weights_file));
    files.push_back(std::move(fit_file));
    std::vector<std::vector<atx::f64>> columns;
    columns.push_back(std::move(combined));
    ATX_TRY(auto combo_panel, alpha::Panel::create(dates, instruments, {"alpha"},
                                                 std::move(columns), std::move(mask)));
    ATX_TRY(auto combo, write_panel_artifact(combo_panel, (directory / "combo.bin").string(), identity));
    Inputs inputs{std::move(evaluation), std::move(combo), Json{}, Json{}, membership_mode};
    inputs.readiness =
        Json{{"observations", dates}, {"instruments", instruments}, {"feature_begin_context_row", plan.feature_begin},
             {"evaluation_begin_context_row", plan.evaluation_begin}, {"evaluation_end_context_row", plan.evaluation_end},
             {"first_evaluation_session_key_ns", number(plan.first_evaluation_session_key)},
             {"last_evaluation_session_key_ns", number(plan.last_evaluation_session_key)},
             {"observed_feature_cells", view.observed_feature_cells},
             {"eligible_evaluation_cells", view.eligible_evaluation_cells},
             {"ready_evaluation_cells", view.ready_evaluation_cells},
             {"admitted_evaluation_cells", view.admitted_evaluation_cells},
             {"liquidity_floor_rejected_cells", view.liquidity_floor_rejected_cells},
             {"empty_evaluation_rows", empty_rows}, {"minimum_admitted_names", minimum_admitted}};
    inputs.memory =
        Json{{"estimated_peak_bytes", number(estimated_peak)}, {"context_reader_peak_bytes", number(read_peak)},
             {"features_and_blend_peak_bytes", number(feature_peak)}, {"downstream_peak_bytes", number(downstream_peak)},
             {"overhead_reserve_bytes", number(kOverheadReserve)}, {"vm_slots", plan.vm_slots},
             {"scope", "known-array-bound-plus-overhead-reserve-not-an-OS-RSS-limit"}};
    return Ok(std::move(inputs));
}

// Checkpoint 16 signals-only commit: evaluation.bin + combo.bin and their manifests are
// bound; no books, no report, no replay figures. status is deliberately NOT "complete".
Result<StageResult> commit_signals_only(const Inputs &inputs, const Profile &profile, const Json &attempt,
                                        const fs::path &directory, Json files) {
    Json summary{{"schema", "atx-equity-baseline-summary-v1"},
        {"purpose", "training-only-signal-panel-for-cross-section-ic-no-book-result"},
        {"readiness", inputs.readiness}, {"memory_preflight", inputs.memory},
        {"replay", "skipped-membership-context"}, {"strategy_capacity", "unavailable"},
        {"qualification", "not-attempted"}};
    ATX_TRY(auto summary_file, write_text(directory, "summary.json", summary.dump(2) + "\n"));
    files.push_back(std::move(summary_file));
    for (const std::string name : {"evaluation.bin", "evaluation.bin.manifest.json", "combo.bin",
             "combo.bin.manifest.json"}) {
        ATX_TRY(auto hash, atx::core::sha256_file((directory / name).string()));
        files.push_back(Json{{"filename", name}, {"sha256", hash},
                              {"size_bytes", number(fs::file_size(directory / name))}});
    }
    Json manifest{{"schema", "atx-equity-baseline-v1"}, {"status", "complete-signals-only"},
        {"qualification", "not-attempted"}, {"recipe", profile.recipe},
        {"parents", Json::array({Json{{"role", "source-context"}, {"sha256", attempt.at("source_context_artifact_id")}},
            Json{{"role", "evaluation"}, {"sha256", inputs.evaluation.artifact_id}},
            Json{{"role", "combo"}, {"sha256", inputs.combo.artifact_id}}})}, {"files", files}};
    ATX_TRY(auto baseline_id, atx::core::sha256_hex(std::string(kDomain) + manifest.dump()));
    manifest["baseline_id"] = baseline_id;
    std::error_code ec;
    if (!fs::remove(directory / ".pending", ec) || ec) {
        return Err(ErrorCode::IoError, "equity baseline: cannot release pending marker");
    }
    ATX_TRY(auto final_file, write_text(directory, "manifest.json", manifest.dump(2) + "\n"));
    (void)final_file;
    StageResult result;
    result.digest = fnv1a64(baseline_id.data(), baseline_id.size());
    result.kvs.emplace_back("baseline_id", baseline_id);
    result.kvs.emplace_back("qualification", "not-attempted");
    result.kvs.emplace_back("replay", "skipped-membership-context");
    result.kvs.emplace_back("admitted_evaluation_cells", inputs.readiness.at("admitted_evaluation_cells").dump());
    return Ok(std::move(result));
}

Result<Json> target_diagnostics(const PanelArtifact &books, const fs::path &directory, Json &files) {
    ATX_TRY(auto weight_id, books.panel.field_id("weight"));
    const auto values = books.panel.field_all(weight_id);
    std::vector<double> previous(books.panel.instruments(), 0);
    double max_net = 0, max_gross = 0, min_gross = std::numeric_limits<double>::infinity();
    double max_name = 0, total_turnover = 0;
    atx::usize violations = 0;
    std::ostringstream out;
    out << "decision_index,session_key_ns,target_net,target_gross,max_name_abs,target_l1_change,qualified_shape\n";
    for (atx::usize d = 0; d < books.panel.dates(); ++d) {
        double net = 0, gross = 0, cap = 0, turnover = 0;
        for (atx::usize i = 0; i < books.panel.instruments(); ++i) {
            const double w = values[d * books.panel.instruments() + i];
            if (!std::isfinite(w)) return Err(ErrorCode::InvalidArgument, "equity baseline: nonfinite target weight");
            net += w; gross += std::abs(w); cap = std::max(cap, std::abs(w));
            turnover += std::abs(w - previous[i]); previous[i] = w;
        }
        const bool qualified = std::abs(net) <= 1e-10 && gross <= 1 + 1e-10 && cap <= 0.01 + 1e-10;
        if (!qualified) ++violations;
        max_net = std::max(max_net, std::abs(net)); max_gross = std::max(max_gross, gross);
        min_gross = std::min(min_gross, gross); max_name = std::max(max_name, cap);
        total_turnover += turnover;
        out << d << ',' << books.identity.session_keys[d] << ',' << number(net) << ',' << number(gross)
            << ',' << number(cap) << ',' << number(turnover) << ',' << (qualified ? "true" : "false") << '\n';
    }
    ATX_TRY(auto file, write_text(directory, "target_exposures.csv", out.str()));
    files.push_back(std::move(file));
    return Ok(Json{{"decisions", books.panel.dates()}, {"max_abs_target_net", max_net},
        {"max_target_gross", max_gross}, {"min_target_gross", min_gross}, {"max_name_abs", max_name},
        {"total_target_l1_change", total_turnover}, {"shape_violation_decisions", violations},
        {"neutrality", violations == 0 ? "measured-target-tolerance-only" : "failed-target-tolerance"},
        {"turnover_basis", "successive-target-L1-not-actual-dollar-turnover"},
        {"post_cost_and_between_decision_risk", "unqualified-not-enforced"}});
}

Result<StageResult> execute(const RunConfig &cfg, Profile &profile, Json &attempt,
                            const fs::path &directory) {
    Json files = Json::array();
    // Entire context, VM/signals and stream arrays die before downstream readers run.
    ATX_TRY(auto inputs, make_inputs(cfg, profile, attempt, files, directory));
    if (inputs.membership_mode) return commit_signals_only(inputs, profile, attempt, directory, std::move(files));
    RunConfig optimize;
    optimize.panel = (directory / "evaluation.bin").string();
    optimize.combo = (directory / "combo.bin").string();
    optimize.books_out = (directory / "books.bin").string();
    optimize.position_mode = true; optimize.gross = 1; optimize.name_cap = 0.01; optimize.rebalance = "weekly";
    ATX_TRY(auto optimized, run_optimize(optimize));
    (void)optimized;
    ATX_TRY(auto books, read_panel_artifact(optimize.books_out));
    ATX_TRY_VOID(require_panel_parent(books.identity, "research", inputs.evaluation.artifact_id));
    ATX_TRY_VOID(require_panel_parent(books.identity, "combo", inputs.combo.artifact_id));
    ATX_TRY(auto exposures, target_diagnostics(books, directory, files));
    profile.replay.panel = optimize.panel; profile.replay.combo = optimize.combo;
    profile.replay.books = optimize.books_out; profile.replay.report_out = (directory / "report").string();
    ATX_TRY(auto reported, run_report(profile.replay));
    std::string report_id;
    for (const auto &[key, value] : reported.kvs) if (key == "report_id") report_id = value;
    ATX_TRY(auto report_text, read_small(directory / "report/manifest.json"));
    Json report_manifest = strict_json(report_text);
    if (report_manifest.at("report_id") != report_id || report_manifest.at("status") != "complete") {
        return Err(ErrorCode::ParseError, "equity baseline: replay manifest identity mismatch");
    }
    report_manifest.erase("report_id");
    ATX_TRY(auto actual_report_id, atx::core::sha256_hex("atx-replay-report-v1\n" + report_manifest.dump()));
    if (actual_report_id != report_id) return Err(ErrorCode::ParseError, "equity baseline: invalid replay manifest hash");
    ATX_TRY(auto summary_text, read_small(directory / "report/summary.json"));
    ATX_TRY(auto summary_sha, atx::core::sha256_hex(summary_text));
    bool summary_bound = false;
    for (const auto &file : report_manifest.at("files")) {
        if (file.at("filename") == "summary.json" && file.at("sha256") == summary_sha) summary_bound = true;
    }
    if (!summary_bound) return Err(ErrorCode::ParseError, "equity baseline: replay summary is not bound");
    const Json replay_summary = strict_json(summary_text);
    const bool failed_shape = exposures.at("shape_violation_decisions").get<atx::usize>() != 0;
    Json summary{{"schema", "atx-equity-baseline-summary-v1"}, {"purpose", "training-only-software-and-book-diagnostic"},
        {"readiness", inputs.readiness}, {"target_exposures", exposures}, {"memory_preflight", inputs.memory},
        {"replay", replay_summary.at("full")}, {"trade_liquidity", replay_summary.at("trade_liquidity")},
        {"strategy_capacity", "unavailable"}, {"qualification", failed_shape ? "failed" : "unknown"},
        {"qualification_reasons", Json::array({"source-economics-and-publication-vintages-unverified",
            "instrument-types-and-locates-unknown", "post-cost-and-between-decision-risk-not-enforced",
            "no-heldout-selection-evidence"})},
        {"post_fit_interpretation", "unfit-constant-weights-boundary-zero-is-not-out-of-sample-selection"}};
    if (failed_shape) summary["qualification_reasons"].push_back("target-shaping-violates-declared-neutrality-or-cap-tolerance");
    ATX_TRY(auto summary_file, write_text(directory, "summary.json", summary.dump(2) + "\n"));
    files.push_back(std::move(summary_file));
    for (const std::string name : {"evaluation.bin", "evaluation.bin.manifest.json", "combo.bin",
             "combo.bin.manifest.json", "books.bin", "books.bin.manifest.json", "books.bin.meta.txt",
             "report/manifest.json"}) {
        ATX_TRY(auto hash, atx::core::sha256_file((directory / name).string()));
        files.push_back(Json{{"filename", name}, {"sha256", hash},
                              {"size_bytes", number(fs::file_size(directory / name))}});
    }
    Json manifest{{"schema", "atx-equity-baseline-v1"}, {"status", "complete"},
        {"qualification", failed_shape ? "failed" : "unknown"}, {"recipe", profile.recipe},
        {"parents", Json::array({Json{{"role", "source-context"}, {"sha256", attempt.at("source_context_artifact_id")}},
            Json{{"role", "evaluation"}, {"sha256", inputs.evaluation.artifact_id}},
            Json{{"role", "combo"}, {"sha256", inputs.combo.artifact_id}},
            Json{{"role", "books"}, {"sha256", books.artifact_id}},
            Json{{"role", "report"}, {"sha256", report_id}}})}, {"files", files}};
    ATX_TRY(auto baseline_id, atx::core::sha256_hex(std::string(kDomain) + manifest.dump()));
    manifest["baseline_id"] = baseline_id;
    std::error_code ec;
    if (!fs::remove(directory / ".pending", ec) || ec) {
        return Err(ErrorCode::IoError, "equity baseline: cannot release pending marker");
    }
    ATX_TRY(auto final_file, write_text(directory, "manifest.json", manifest.dump(2) + "\n"));
    (void)final_file;
    StageResult result = std::move(reported);
    result.digest = fnv1a64(baseline_id.data(), baseline_id.size());
    result.kvs.emplace_back("baseline_id", baseline_id);
    result.kvs.emplace_back("qualification", failed_shape ? "failed" : "unknown");
    result.kvs.emplace_back("target_shape_violations", exposures.at("shape_violation_decisions").dump());
    result.kvs.emplace_back("actual_absolute_trade_dollars", replay_summary.at("full").at("absolute_trade_dollars").dump());
    result.kvs.emplace_back("admitted_evaluation_cells", inputs.readiness.at("admitted_evaluation_cells").dump());
    return Ok(std::move(result));
}

} // namespace

Result<StageResult> run_equity_baseline(const RunConfig &config) {
    bool reserved = false;
    const fs::path directory(config.out);
    Json attempt{{"schema", "atx-equity-baseline-attempt-v1"}, {"status", "started"}};
    auto work = [&]() -> Result<StageResult> {
        ATX_TRY(auto profile, resolve(config));
        attempt["recipe"] = profile.recipe;
        ATX_TRY_VOID(reserve_directory(directory));
        reserved = true;
        return execute(config, profile, attempt, directory);
    };
    Result<StageResult> result = [&]() -> Result<StageResult> {
        try { return work(); }
        catch (const std::exception &error) {
            return Err(ErrorCode::IoError, "equity baseline: " + std::string(error.what()));
        }
    }();
    if (!result && reserved) {
        attempt["status"] = "failed";
        attempt["error"] = result.error().to_string();
        try { (void)write_text(directory, "failure.json", attempt.dump(2) + "\n"); }
        catch (const std::exception &) { /* Original failure and .pending remain authoritative. */ }
    }
    return result;
}

} // namespace atx::impl
