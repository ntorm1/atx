#include "stage_equity_book.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <set>
#include <stdexcept>
#include <vector>

#include <nlohmann/json.hpp>

#include "artifacts.hpp"
#include "atx/core/sha256.hpp"
#include "atx/engine/data/orats_history.hpp"
#include "equity_allocation.hpp"
#include "equity_baseline_views.hpp"
#include "panel_artifact.hpp"
#include "replay_report.hpp"
#include "stage_data_provenance.hpp"

namespace atx::impl {
namespace {
namespace fs = std::filesystem;
using Json = nlohmann::json;
using atx::core::Err;
using atx::core::ErrorCode;
using atx::core::Ok;
using atx::core::Result;
using atx::core::Status;
constexpr atx::u64 kReserve = 512'000'000;
constexpr atx::u64 kDecisionCellBytes = 1280;
constexpr atx::u64 kCallbackCellBytes = 2048;
constexpr atx::u64 kDecisionMetadataBytes = 16'384;

Json parse(const std::string &text) {
    std::vector<std::set<std::string>> keys;
    return Json::parse(text, [&](int depth, Json::parse_event_t event, Json &value) {
        if (depth > 64) throw std::invalid_argument("equity book: JSON depth limit");
        if (event == Json::parse_event_t::object_start) keys.emplace_back();
        if (event == Json::parse_event_t::key && !keys.back().insert(value.get<std::string>()).second)
            throw std::invalid_argument("equity book: duplicate JSON key");
        if (event == Json::parse_event_t::object_end) keys.pop_back();
        return true;
    });
}

Result<std::string> read_small(const fs::path &path) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    const auto length = in.tellg();
    if (!in || length <= 0 || length > 16 * 1024 * 1024)
        return Err(ErrorCode::IoError, "equity book: invalid metadata extent");
    std::string text(static_cast<atx::usize>(length), '\0');
    in.seekg(0);
    if (!in.read(text.data(), length) || in.peek() != std::char_traits<char>::eof())
        return Err(ErrorCode::IoError, "equity book: metadata changed while reading");
    return Ok(std::move(text));
}

Result<Json> publish(const fs::path &root, const std::string &name, const Json &value) {
    const auto text = value.dump(2) + "\n";
    const auto partial = root / (name + ".partial");
    std::ofstream out(partial, std::ios::binary);
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    out.close();
    if (!out) return Err(ErrorCode::IoError, "equity book: cannot write " + name);
    std::error_code ec;
    fs::create_hard_link(partial, root / name, ec);
    if (ec) return Err(ErrorCode::IoError, "equity book: cannot publish " + name);
    fs::remove(partial, ec);
    if (ec) return Err(ErrorCode::IoError, "equity book: cannot remove partial");
    ATX_TRY(auto sha, atx::core::sha256_hex(text));
    return Ok(Json{{"filename", name}, {"sha256", sha}, {"size_bytes", std::to_string(text.size())}});
}

Result<atx::u64> plus(atx::u64 a, atx::u64 b) {
    if (b > std::numeric_limits<atx::u64>::max() - a)
        return Err(ErrorCode::OutOfRange, "equity book: memory addition overflow");
    return Ok(a + b);
}
Result<atx::u64> times(atx::u64 a, atx::u64 b) {
    if (a && b > std::numeric_limits<atx::u64>::max() / a)
        return Err(ErrorCode::OutOfRange, "equity book: memory multiplication overflow");
    return Ok(a * b);
}
Result<atx::i64> integer(const Json &value) {
    const auto text = value.get<std::string>();
    atx::i64 out{};
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), out);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || std::to_string(out) != text)
        return Err(ErrorCode::ParseError, "equity book: noncanonical integer");
    return Ok(out);
}

Status arguments(const RunConfig &cfg) {
    const std::set<std::string> allowed{"panel", "baseline-dir", "out", "max-working-bytes",
        "report-aum", "replay-execution-delay", "replay-trade-bps", "replay-annual-borrow-bps",
        "replay-day-basis", "quiet", "digest-only", "config", "allow-same-close"};
    for (const auto &flag : cfg.set_flags) if (!allowed.contains(flag))
        return Err(ErrorCode::InvalidArgument, "equity book: unsupported flag --" + flag);
    if (cfg.panel.empty() || cfg.equity_baseline_dir.empty() || cfg.out.empty() ||
        cfg.equity_max_working_bytes <= kReserve || cfg.allow_unidentified_panels ||
        !cfg.equity_evaluation_start.empty() || !cfg.equity_evaluation_end.empty() ||
        !cfg.books.empty() || !cfg.combo.empty() || !cfg.method.empty() || cfg.gross != 0 ||
        cfg.name_cap != 0 || cfg.risk_aversion != 0 || cfg.turnover_penalty != 0 ||
        cfg.cost_bps != 0 || cfg.borrow_bps != 0 || !cfg.rebalance.empty() || cfg.trade_rate != 1 ||
        cfg.conviction || cfg.sector_neutral || cfg.industry_neutral || cfg.group_neutralize ||
        cfg.kelly_fraction != 0 || cfg.risk_model != "diagonal" || cfg.dead_alpha_factors ||
        cfg.metabook || cfg.gp_trading || cfg.participation_cap != 0 || cfg.book_turnover_gate != 0 ||
        cfg.fit_begin != 0 || cfg.fit_end != 0 || cfg.combine_holdout_frac != 0 ||
        cfg.weight_transform != "rank" || cfg.winsorize_limit != .025 || cfg.gross_leverage != 1 ||
        !cfg.seed_exprs.empty() || !cfg.library_dir.empty() || cfg.gated || cfg.walk_forward != 0)
        return Err(ErrorCode::InvalidArgument, "equity book: requires context, baseline-dir, fresh out and fixed profile");
    return Ok();
}

struct Inputs {
    PanelArtifact context, evaluation, combo, books;
    Json baseline_recipe;
    atx::usize first_row{};
    atx::u64 payload_cap{}, resident_bound{};
};

Result<Inputs> load(const RunConfig &cfg) {
    const fs::path baseline(cfg.equity_baseline_dir);
    const std::array<std::string, 4> paths{cfg.panel, (baseline / "evaluation.bin").string(),
        (baseline / "combo.bin").string(), (baseline / "books.bin").string()};
    std::array<atx::u64, 4> sizes{};
    std::array<std::string, 4> declared_ids;
    atx::u64 sum = 0;
    // Reject sealed dates from the metadata before allocating numeric panels.
    const auto training_end = *atx::engine::data::detail::date_to_nanos("2020-01-01");
    const auto training_begin = *atx::engine::data::detail::date_to_nanos("2013-04-01");
    Json baseline_recipe;
    for (atx::usize p = 0; p < paths.size(); ++p) {
        ATX_TRY(auto text, read_small(paths[p] + ".manifest.json"));
        const auto metadata = parse(text);
        declared_ids[p] = metadata.at("artifact_id").get<std::string>();
        const auto recipe = p < 2 ? parse(metadata.at("recipe").get<std::string>()) : Json{};
        for (const auto &key : metadata.at("axes").at("session_keys")) {
            ATX_TRY(auto date, integer(key));
            if (date >= training_end || (p > 0 && date < training_begin))
                return Err(ErrorCode::InvalidArgument, "equity book: source axes extend outside training");
        }
        if (p == 0) {
            ATX_TRY(auto end, integer(recipe.at("end_exclusive_nanos")));
            if (end > training_end)
                return Err(ErrorCode::InvalidArgument, "equity book: context includes sealed dates");
        }
        if (p == 1) baseline_recipe = recipe;
        std::error_code ec;
        sizes[p] = fs::file_size(paths[p], ec);
        if (ec || sizes[p] == 0) return Err(ErrorCode::IoError, "equity book: invalid input extent");
        ATX_TRY(sum, plus(sum, sizes[p]));
    }
    ATX_TRY(auto resident, times(sum, 4));
    ATX_TRY(resident, plus(resident, kReserve));
    if (resident >= cfg.equity_max_working_bytes)
        return Err(ErrorCode::OutOfRange, "equity book: bounded snapshot readers exceed budget");
    ATX_TRY(auto context, read_panel_artifact(paths[0], sizes[0]));
    ATX_TRY(auto evaluation, read_panel_artifact(paths[1], sizes[1]));
    ATX_TRY(auto combo, read_panel_artifact(paths[2], sizes[2]));
    ATX_TRY(auto books, read_panel_artifact(paths[3], sizes[3]));
    const std::array<const PanelArtifact *, 4> panels{&context, &evaluation, &combo, &books};
    for (atx::usize p = 0; p < panels.size(); ++p) if (panels[p]->artifact_id != declared_ids[p])
        return Err(ErrorCode::InvalidArgument, "equity book: input changed after preflight");
    if (baseline_recipe.at("profile") != "slow-momentum-equal-weekly-shaping-v1" ||
        baseline_recipe.at("fit_kind") != "unfit-constant-weights" ||
        baseline_recipe.at("fitted_observations") != 0 ||
        baseline_recipe.at("decision_schedule") != "every-five-evaluation-observations-starting-at-zero" ||
        baseline_recipe.at("inherited_context_recipe") != context.identity.recipe ||
        combo.identity.recipe != evaluation.identity.recipe || books.identity.recipe != "stage=optimize-v1\nstep=5" ||
        context.identity.instrument_namespace != kSpiderRockSecurityIdNamespace ||
        baseline_recipe.at("signals").size() != kEquityBaselineDsl.size())
        return Err(ErrorCode::InvalidArgument, "equity book: unsupported baseline provenance/recipe");
    for (atx::usize i = 0; i < kEquityBaselineDsl.size(); ++i) {
        ATX_TRY(auto sha, atx::core::sha256_hex(kEquityBaselineDsl[i]));
        const auto &signal = baseline_recipe.at("signals").at(i);
        if (signal.at("dsl").get<std::string>() != kEquityBaselineDsl[i] || signal.at("dsl_sha256") != sha ||
            signal.at("constant_weight") != .5)
            return Err(ErrorCode::InvalidArgument, "equity book: unsupported signal recipe");
    }
    ATX_TRY_VOID(require_panel_parent(evaluation.identity, "source-context", context.artifact_id));
    ATX_TRY_VOID(require_panel_parent(combo.identity, "research", evaluation.artifact_id));
    ATX_TRY_VOID(require_panel_parent(books.identity, "research", evaluation.artifact_id));
    ATX_TRY_VOID(require_panel_parent(books.identity, "combo", combo.artifact_id));
    ATX_TRY_VOID(require_same_panel_axes(evaluation.identity, combo.identity));
    auto axes = context.identity;
    axes.session_keys = evaluation.identity.session_keys;
    ATX_TRY_VOID(require_same_panel_axes(axes, evaluation.identity));
    const auto begin = atx::engine::data::detail::date_to_nanos(baseline_recipe.at("evaluation_start").get<std::string>());
    const auto end = atx::engine::data::detail::date_to_nanos(baseline_recipe.at("evaluation_end_exclusive").get<std::string>());
    if (!begin || !end || *begin < training_begin || *end > training_end || *begin >= *end)
        return Err(ErrorCode::InvalidArgument, "equity book: invalid original evaluation window");
    const auto &keys = context.identity.session_keys;
    const auto context_recipe = parse(context.identity.recipe);
    ATX_TRY(auto context_end, integer(context_recipe.at("end_exclusive_nanos")));
    if (context_end < *end)
        return Err(ErrorCode::InvalidArgument, "equity book: context does not cover original evaluation end");
    const auto first = std::lower_bound(keys.begin(), keys.end(), *begin);
    const auto last = std::lower_bound(keys.begin(), keys.end(), *end);
    const auto first_row = static_cast<atx::usize>(first - keys.begin());
    if (first_row < kEquityBaselineWarmup || first >= last ||
        !std::equal(first, last, evaluation.identity.session_keys.begin(), evaluation.identity.session_keys.end()))
        return Err(ErrorCode::InvalidArgument, "equity book: truncated/misaligned original evaluation window");
    const auto n = evaluation.panel.instruments();
    const auto d = evaluation.panel.dates();
    const auto scheduled = d / 5 + (d % 5 != 0 ? 1 : 0);
    if (books.identity.session_keys.size() != scheduled)
        return Err(ErrorCode::InvalidArgument, "equity book: preference schedule is not complete weekly schedule");
    for (atx::usize i = 0; i < scheduled; ++i)
        if (books.identity.session_keys[i] != evaluation.identity.session_keys[i * 5])
            return Err(ErrorCode::InvalidArgument, "equity book: preference decision differs from fixed weekly schedule");
    for (const std::string field : {"close", "raw_close", "volume"}) {
        ATX_TRY(auto src, context.panel.field_id(field));
        ATX_TRY(auto dst, evaluation.panel.field_id(field));
        const auto x = context.panel.field_all(src).subspan(first_row * n, d * n);
        const auto y = evaluation.panel.field_all(dst);
        for (atx::usize i = 0; i < x.size(); ++i)
            if (std::bit_cast<atx::u64>(x[i]) != std::bit_cast<atx::u64>(y[i]))
                return Err(ErrorCode::InvalidArgument, "equity book: evaluation values differ from context");
    }
    for (atx::usize row = 0; row < d; ++row) for (atx::usize i = 0; i < n; ++i)
        if (evaluation.panel.in_universe(row, i) && !context.panel.in_universe(first_row + row, i))
            return Err(ErrorCode::InvalidArgument, "equity book: expanded decision eligibility");
    ATX_TRY(auto cells, times(d, n));
    ATX_TRY(auto replay_arrays, times(cells, 64));
    ATX_TRY(resident, plus(resident, replay_arrays));
    ATX_TRY(auto decisions, times(books.panel.dates(), n));
    // Full reasons, typed invalid-mark mappings, raw/resolved weights, intents
    // and accepted allocation arrays, including JSON nodes/serialization space.
    // Retain one additional callback context and allow two live mark snapshots.
    ATX_TRY(auto decision_arrays, times(decisions, kDecisionCellBytes));
    ATX_TRY(resident, plus(resident, decision_arrays));
    ATX_TRY(auto callback_arrays, times(n, kCallbackCellBytes));
    ATX_TRY(resident, plus(resident, callback_arrays));
    ATX_TRY(auto metadata_bytes, times(books.panel.dates(), kDecisionMetadataBytes));
    ATX_TRY(resident, plus(resident, metadata_bytes));
    atx::u64 identifier_bytes = 0;
    for (const auto &id : context.identity.instrument_ids) {
        ATX_TRY(identifier_bytes, plus(identifier_bytes, id.size()));
    }
    ATX_TRY(auto identifier_copies, plus(books.panel.dates(), 2));
    ATX_TRY(auto escaped_identifiers, times(identifier_bytes, 8));
    ATX_TRY(auto retained_identifiers, times(escaped_identifiers, identifier_copies));
    ATX_TRY(resident, plus(resident, retained_identifiers));
    if (resident >= cfg.equity_max_working_bytes)
        return Err(ErrorCode::OutOfRange, "equity book: resident panels/replay admission exceeds budget");
    return Ok(Inputs{std::move(context), std::move(evaluation), std::move(combo), std::move(books),
        std::move(baseline_recipe), first_row, *std::max_element(sizes.begin(), sizes.end()), resident});
}

Json certificate(const EquityAllocationResult &out) {
    const auto &c = out.certificate;
    return Json{{"solver_used", c.solver_used}, {"primal_residual", c.solver.prim_res},
        {"dual_residual", c.solver.dual_res}, {"polished", c.solver.polished},
        {"effective_solver_feasibility_tolerance", c.effective_solver_feasibility_tolerance},
        {"fee_reserve", c.fee_reserve}, {"requested_prefee_net", c.requested_prefee_net},
        {"requested_prefee_gross", c.requested_prefee_gross}, {"requested_turnover", c.requested_turnover},
        {"actual_turnover", c.actual_turnover}, {"postfee_net", c.postfee_net},
        {"postfee_gross", c.postfee_gross}, {"postfee_max_name", c.postfee_max_name},
        {"traded_dollars", c.traded_dollars}, {"trade_cost", c.trade_cost},
        {"posttrade_cash", c.posttrade_cash}, {"posttrade_nav", c.posttrade_nav},
        {"objective", c.objective}, {"fixed_zero_count", c.fixed_zero_count},
        {"execution_unavailable_count", c.execution_unavailable_count},
        {"execution_fixed_zero_count", c.execution_fixed_zero_count},
        {"required_zero_count", c.required_zero_count},
        {"decision_fixed_zero_l1_change", c.decision_fixed_zero_l1_change},
        {"execution_fixed_zero_l1_change", c.execution_fixed_zero_l1_change},
        {"solver_certificate_scope", "continuous-weights-before-representation"},
        {"solver_constraint_scope",
            "original-canonical-union-with-decision-and-observed-close-fixed-zero-equalities"},
        {"continuous_objective", c.continuous_objective},
        {"represented_objective", c.represented_objective},
        {"representation_objective_gap", c.representation_objective_gap},
        {"fixed_zero_l1_change", c.fixed_zero_l1_change},
        {"representation_l1_change", c.representation_l1_change},
        {"representation_l1_budget", c.representation_l1_budget},
        {"representation_max_eta", c.representation_max_eta},
        {"actual_representation_l1_change", c.actual_representation_l1_change},
        {"close_count", c.close_count}, {"hold_count", c.hold_count},
        {"union_instruments", out.plan.union_instruments},
        {"additional_bytes_bound", std::to_string(out.plan.additional_bytes_bound)}};
}

const char *close_classification(atx::f64 mark) {
    if (std::isnan(mark)) return "nan";
    if (std::isinf(mark)) return mark > 0 ? "positive-infinity" : "negative-infinity";
    if (mark == 0.0) return "zero";
    return mark < 0.0 ? "negative" : "finite-positive";
}

Result<Json> availability_snapshot(const atx::engine::book::ReplayAllocationState &state,
                                   const PanelArtifact &source) {
    Json marks = Json::array();
    Json invalid = Json::array();
    atx::usize held = 0;
    for (atx::usize i = 0; i < state.current_marks.size(); ++i) {
        const auto mark = state.current_marks[i];
        const auto classification = close_classification(mark);
        const auto value = std::isfinite(mark) ? Json(mark) : Json(nullptr);
        marks.push_back(Json::array({classification, value}));
        if (state.tri_units[i] != 0.0) {
            ++held; // Replay validated every held mark before invoking this callback.
        } else if (!std::isfinite(mark) || mark <= 0.0) {
            invalid.push_back(Json{{"instrument_index", i},
                {"security_id", source.identity.instrument_ids[i]},
                {"reason", "execution-close-unavailable"},
                {"mark_classification", classification}, {"observed_close", value}});
        }
    }
    const Json observed{{"source_artifact_id", source.artifact_id},
        {"execution_period", state.execution_period},
        {"execution_session_key_ns", std::to_string(state.execution_session_key)},
        {"marks_canonical_order", std::move(marks)}};
    ATX_TRY(auto sha, atx::core::sha256_hex(
        "atx-equity-observed-closes-v1\n" + observed.dump()));
    return Ok(Json{{"source_artifact_id", source.artifact_id},
        {"execution_period", state.execution_period},
        {"execution_session_key_ns", std::to_string(state.execution_session_key)},
        {"observed_closes_sha256", sha}, {"held_count", held},
        {"held_current_marks_verified", true},
        {"unheld_invalid_current_mark_count", invalid.size()},
        {"invalid_unheld_marks", std::move(invalid)},
        {"available_by_order_submission", "unverified"}});
}

std::string callback_error(const atx::core::Error &error,
    const atx::engine::book::ReplayAllocationState &state, const PanelIdentity &identity) {
    auto message = error.message() + "; decision_period=" + std::to_string(state.decision_period) +
        " execution_period=" + std::to_string(state.execution_period) +
        " decision_session_key_ns=" + std::to_string(state.decision_session_key) +
        " execution_session_key_ns=" + std::to_string(state.execution_session_key);
    const auto token = error.message().rfind("instrument=");
    if (token != std::string::npos) {
        const auto *begin = error.message().data() + token + 11;
        const auto *end = error.message().data() + error.message().size();
        atx::usize instrument{};
        const auto parsed = std::from_chars(begin, end, instrument);
        if (parsed.ec == std::errc{} && instrument < identity.instrument_ids.size()) {
            message += " security_id=" + identity.instrument_ids[instrument];
        }
    }
    return message;
}

Result<StageResult> execute(const RunConfig &cfg, const fs::path &directory, Json &attempt,
                             Json &certificates) {
    ATX_TRY(auto inputs, load(cfg));
    RunConfig report;
    const auto &base = inputs.baseline_recipe;
    report.report_aum = cfg.set_flags.contains("report-aum") || cfg.report_aum != 1e9
        ? cfg.report_aum : base.at("initial_nav").get<double>();
    report.replay_trade_bps = cfg.set_flags.contains("replay-trade-bps") || cfg.replay_trade_bps != 0
        ? cfg.replay_trade_bps : base.at("trade_bps_per_absolute_dollar").get<double>();
    report.replay_annual_borrow_bps = cfg.set_flags.contains("replay-annual-borrow-bps") || cfg.replay_annual_borrow_bps != 0
        ? cfg.replay_annual_borrow_bps : base.at("annual_borrow_bps").get<double>();
    ATX_TRY(auto delay, integer(base.at("execution_delay_observations")));
    if (delay < 0 || static_cast<atx::u64>(delay) > std::numeric_limits<atx::usize>::max())
        return Err(ErrorCode::InvalidArgument, "equity book: invalid inherited delay");
    report.replay_execution_delay = cfg.set_flags.contains("replay-execution-delay") || cfg.replay_execution_delay != 1
        ? cfg.replay_execution_delay : static_cast<atx::usize>(delay);
    // B-02: an inherited or explicit zero delay (same-close fills) needs the opt-in,
    // so a legacy delay-0 baseline cannot silently seed a same-close book.
    report.allow_same_close = cfg.allow_same_close;
    if (report.replay_execution_delay < 1 && !cfg.allow_same_close)
        return Err(ErrorCode::InvalidArgument,
                   "equity book: execution delay 0 fills at the signal close; pass "
                   "--allow-same-close to request it explicitly");
    if (!base.at("borrow_day_basis").is_number_integer() ||
        (base.at("borrow_day_basis") != 360 && base.at("borrow_day_basis") != 365))
        return Err(ErrorCode::InvalidArgument, "equity book: invalid inherited borrow day basis");
    report.replay_day_basis = cfg.set_flags.contains("replay-day-basis") || cfg.replay_day_basis != 365
        ? cfg.replay_day_basis : base.at("borrow_day_basis").get<int>();
    report.set_flags = {"replay-trade-bps", "replay-annual-borrow-bps"};
    report.panel = (fs::path(cfg.equity_baseline_dir) / "evaluation.bin").string();
    report.combo = (fs::path(cfg.equity_baseline_dir) / "combo.bin").string();
    report.books = (fs::path(cfg.equity_baseline_dir) / "books.bin").string();
    report.report_out = (directory / "report").string();
    EquityAllocationConfig allocation;
    allocation.representation = EquityAllocationRepresentation::MachinePrecisionIntentsV1;
    allocation.execution_availability = EquityExecutionAvailability::ObservedCloseEntryConstraintV1;
    allocation.trade_bps = report.replay_trade_bps;
    allocation.max_additional_bytes = cfg.equity_max_working_bytes - inputs.resident_bound;
    ATX_TRY(auto executable, current_executable_sha256());
    Json recipe{{"profile", "constrained-preference-weekly-observed-close-v3"},
        {"purpose", "training-only-software-and-book-diagnostic"},
        {"baseline_recipe", base}, {"initial_nav", report.report_aum},
        {"execution_delay_observations", std::to_string(report.replay_execution_delay)},
        {"trade_bps_per_absolute_dollar", report.replay_trade_bps},
        {"annual_borrow_bps", report.replay_annual_borrow_bps}, {"borrow_day_basis", report.replay_day_basis},
        {"objective", "0.5*sum((weight-preference)^2)+lambda*sum(variance*weight^2)"},
        {"preference_interpretation", "rank-position-preference-not-expected-return"},
        {"risk_return_count", 63}, {"risk_estimator", "population-variance-adjacent-TRI-returns-no-gap-bridging"},
        {"variance_floor", allocation.variance_floor}, {"risk_penalty", allocation.risk_penalty},
        {"risk_information", "64-context-observations-ending-at-original-decision"},
        {"net_limit", 0}, {"gross_limit", allocation.gross_limit}, {"name_limit", allocation.name_limit},
        {"full_l1_turnover_limit", allocation.turnover_limit},
        {"turnover_basis", "representable-trade-dollars/pretrade-NAV"},
        {"risk_limit_timing", "immediate-post-fee-only-between-rebalance-exposure-not-constrained"},
        {"annual_turnover_limit", "not-enforced"}, {"execution", "hypothetical-close-sizing-using-marked-holdings"},
        {"solver", {{"iters", allocation.solver.iters}, {"rho", allocation.solver.rho},
            {"sigma", allocation.solver.sigma}, {"feas_tol", allocation.solver.feas_tol},
            {"ruiz_passes", allocation.solver.ruiz_passes}, {"polish", allocation.solver.polish},
            {"polish_refine", allocation.solver.polish_refine},
            {"max_factor_bytes", std::to_string(allocation.solver.max_factor_bytes)}}},
        {"economic_tolerance", allocation.feasibility_tolerance}, {"residual_tolerance", allocation.residual_tolerance},
        {"execution_availability", {{"model", "ObservedCloseEntryConstraintV1"},
            {"scope", "exact-unheld-invalid-current-observed-close"},
            {"available_by_order_submission", "unverified"},
            {"constraint", "pre-solve-fixed-zero-equality-retain-canonical-solve-union"},
            {"held_missing_marks", "fail-before-policy"},
            {"current_volume_used", false}, {"future_observations_used", false},
            {"permanent_exclusion", false}, {"exchange_halt_inferred", false},
            {"counterfactual_orders", "not-computed"}}},
        {"required_zero_reason_bits", {{"none", 0}, {"decision-ineligible", 1},
            {"risk-unready", 2}, {"execution-close-unavailable", 4}}},
        {"required_zero_count_scope", "deduplicated-solve-union"},
        {"representation", {{"model", "MachinePrecisionIntentsV1"},
            {"eta", "min(64*binary64_epsilon*max(1,abs(raw),abs(previous)),budget/canonical_N)"},
            {"budget", "economic_tolerance*fee_reserve/(128*(1+fee_rate*(1+gross_limit+name_limit)))"},
            {"priority", "mandatory-close,abs(raw)<=eta-close,abs(raw-previous)<=eta-hold,target-weight"},
            {"hold", "exact-current-TRI-units-and-marked-dollars"},
            {"close", "zero-units-actual-closing-delta-and-fees"},
            {"economic_limits", "recertify-entire-represented-book-under-original-limits"},
            {"threshold_retry", "none"}, {"physical_share_rounding", false},
            {"economic_no_trade_band", false}}},
        {"solver_feasibility_rule", "min(configured,economic_tolerance*fee_reserve/(8*(2*union_count+1)*(1+fee_rate*(1+gross_limit+name_limit))))"},
        {"resident_admission_bytes", std::to_string(inputs.resident_bound)},
        {"max_working_bytes", std::to_string(cfg.equity_max_working_bytes)},
        {"memory_scope", "conservative-array-admission-plus-overhead-reserve-not-OS-RSS-limit"},
        {"retained_evidence_admission", {{"bytes_per_decision_instrument", kDecisionCellBytes},
            {"callback_workspace_bytes_per_instrument", kCallbackCellBytes},
            {"metadata_bytes_per_decision", kDecisionMetadataBytes},
            {"identifier_bytes", "8*sum(identifier_lengths)*(decision_count+2)"},
            {"callback_context_history", "last-attempt-only-plus-certified-proposal-contexts"}}},
        {"held_missing_price_policy", "reject-entire-run-no-window-shortening"},
        {"qualification", "unknown"}, {"strategy_capacity", "unavailable"},
        {"sector_beta_constraints", "not-enforced"}, {"live_orders_authorized", false},
        {"producer_executable_sha256", executable.empty() ? "unknown" : executable}};
    const Json parents = Json::array({Json{{"role", "source-context"}, {"sha256", inputs.context.artifact_id}},
        Json{{"role", "evaluation"}, {"sha256", inputs.evaluation.artifact_id}},
        Json{{"role", "combo"}, {"sha256", inputs.combo.artifact_id}},
        Json{{"role", "preferences"}, {"sha256", inputs.books.artifact_id}}});
    attempt["recipe"] = recipe;
    attempt["parents"] = parents;
    ATX_TRY(auto request_file, publish(directory, "request.json", attempt));
    ReplayReportPolicy policy;
    policy.recipe = recipe.dump();
    policy.parents = {{"source-context", inputs.context.artifact_id}};
    policy.max_input_payload_bytes = inputs.payload_cap;
    policy.expected_research_artifact_id = inputs.evaluation.artifact_id;
    policy.expected_books_artifact_id = inputs.books.artifact_id;
    policy.expected_combo_artifact_id = inputs.combo.artifact_id;
    const auto n = inputs.context.panel.instruments();
    ATX_TRY(auto close_id, inputs.context.panel.field_id("close"));
    ATX_TRY(auto raw_id, inputs.context.panel.field_id("raw_close"));
    ATX_TRY(auto volume_id, inputs.context.panel.field_id("volume"));
    const auto closes = inputs.context.panel.field_all(close_id);
    const auto raw = inputs.context.panel.field_all(raw_id);
    const auto volume = inputs.context.panel.field_all(volume_id);
    policy.intent_callback = [&](const atx::engine::book::ReplayAllocationState &state)
        -> Result<std::vector<atx::engine::book::ReplayTargetIntent>> {
        auto &last = certificates["last_callback_attempt"];
        last = Json{{"status", "started"}, {"phase", "execution-availability-snapshot"},
            {"schedule_index", state.schedule_index}, {"decision_period", state.decision_period},
            {"execution_period", state.execution_period},
            {"decision_session_key_ns", std::to_string(state.decision_session_key)},
            {"execution_session_key_ns", std::to_string(state.execution_session_key)},
            {"pretrade_nav", state.pretrade_nav}, {"pretrade_cash", state.cash},
            {"raw_candidate_captured", false}, {"certified_candidate_returned", false}};
        auto candidate = [&]() -> Result<std::vector<atx::engine::book::ReplayTargetIntent>> {
            ATX_TRY(auto availability, availability_snapshot(state, inputs.evaluation));
            last["execution_availability"] = std::move(availability);
            last["phase"] = "freeze-original-decision-risk";
            const auto row = inputs.first_row + state.decision_period;
            if (row >= inputs.context.panel.dates() || row < 63 ||
                inputs.context.identity.session_keys[row] != state.decision_session_key)
                return Err(ErrorCode::InvalidArgument, "equity book: decision/context timing mismatch");
            const auto first = row - 63;
            std::vector<atx::u8> observed(64 * n);
            for (atx::usize j = 0; j < observed.size(); ++j) {
                const auto pos = first * n + j;
                observed[j] = std::isfinite(closes[pos]) && closes[pos] > 0 &&
                    std::isfinite(raw[pos]) && raw[pos] > 0 && std::isfinite(volume[pos]) && volume[pos] >= 0;
            }
            const EquityAllocationRiskWindow window{state.decision_period, first, row, state.decision_session_key,
                std::span<const atx::i64>(inputs.context.identity.session_keys).subspan(first, 64),
                closes.subspan(first * n, 64 * n), observed};
            ATX_TRY(auto frozen, freeze_equity_allocation_decision(window, state.preference_weights,
                state.decision_eligibility, allocation));
            const EquityAllocationExecution execution{state.decision_period, state.execution_period,
                state.decision_session_key, state.execution_session_key, state.cash, state.pretrade_nav,
                state.tri_units, state.current_marks, state.marked_dollars};
            last["phase"] = "allocate-and-certify";
            ATX_TRY(auto chosen, allocate_equity_preference(frozen, execution));
            last["phase"] = "record-certified-proposal";
            Json row_certificate = certificate(chosen);
            row_certificate["decision_period"] = state.decision_period;
            row_certificate["execution_period"] = state.execution_period;
            row_certificate["decision_session_key_ns"] = std::to_string(state.decision_session_key);
            row_certificate["execution_session_key_ns"] = std::to_string(state.execution_session_key);
            row_certificate["first_context_row"] = first;
            row_certificate["decision_context_row"] = row;
            row_certificate["first_risk_session_key_ns"] = std::to_string(frozen.first_risk_session_key);
            row_certificate["pretrade_nav"] = state.pretrade_nav;
            row_certificate["pretrade_cash"] = state.cash;
            row_certificate["proposed_weights_canonical_order"] = chosen.weights;
            row_certificate["continuous_weights_canonical_order"] = chosen.continuous_weights;
            row_certificate["required_zero_reasons_canonical_order"] = chosen.required_zero_reasons;
            ATX_TRY(auto reasons_sha, atx::core::sha256_hex(
                "atx-equity-required-zero-reasons-v1\n" + Json(chosen.required_zero_reasons).dump()));
            row_certificate["required_zero_reasons_sha256"] = reasons_sha;
            row_certificate["execution_availability"] = last["execution_availability"];
            Json intents = Json::array();
            for (const auto &intent : chosen.intents) {
                const char *action = nullptr;
                switch (intent.action) {
                case atx::engine::book::ReplayTargetAction::TargetWeight: action = "target-weight"; break;
                case atx::engine::book::ReplayTargetAction::HoldCurrent: action = "hold-current"; break;
                case atx::engine::book::ReplayTargetAction::Close: action = "close"; break;
                }
                if (action == nullptr)
                    return Err(ErrorCode::Internal, "equity book: invalid certified target intent");
                intents.push_back(Json{{"action", action}, {"weight", intent.weight}});
            }
            row_certificate["proposed_intents_canonical_order"] = std::move(intents);
            // Artifact identity + exact row bounds bind the full risk input; hash the
            // derived finite variance/count vectors separately without publishing them.
            ATX_TRY(auto risk_sha, atx::core::sha256_hex(Json{{"variance", frozen.variance},
                {"valid_return_count", frozen.valid_return_count}}.dump()));
            row_certificate["risk_snapshot_sha256"] = risk_sha;
            certificates["decisions"].push_back(std::move(row_certificate));
            last["raw_candidate_captured"] = true;
            return Ok(std::move(chosen.intents));
        }();
        if (!candidate) {
            last["status"] = "failed";
            last["error"] = candidate.error().to_string();
            last["raw_candidate_status"] = "unavailable-no-successful-helper-result-recorded";
            return Err(candidate.error().code(),
                callback_error(candidate.error(), state, inputs.context.identity));
        }
        last["status"] = "certified-proposal-returned";
        last["phase"] = "awaiting-replay-acceptance";
        last["certified_candidate_returned"] = true;
        return candidate;
    };
    auto reported = run_policy_replay_report(report, policy);
    certificates["status"] = reported ? "complete-replay" : "failed-replay-proposals-only";
    auto &last = certificates["last_callback_attempt"];
    if (!reported && last.is_object() && last["status"] != "certified-proposal-returned") {
        // Also handles callback exceptions caught at the report boundary.
        last["status"] = "failed";
        last["error"] = reported.error().to_string();
        if (!last["raw_candidate_captured"].get<bool>()) {
            last["raw_candidate_status"] = "unavailable-no-successful-helper-result-recorded";
        }
    }
    ATX_TRY(auto certificate_file, publish(directory, "allocation_certificates.json", certificates));
    if (!reported) return Err(reported.error());
    ATX_TRY(auto report_text, read_small(directory / "report/manifest.json"));
    auto report_manifest = parse(report_text);
    const auto report_id = report_manifest.at("report_id").get<std::string>();
    report_manifest.erase("report_id");
    ATX_TRY(auto actual_id, atx::core::sha256_hex("atx-replay-report-v1\n" + report_manifest.dump()));
    if (actual_id != report_id || report_manifest.at("status") != "complete")
        return Err(ErrorCode::ParseError, "equity book: report identity mismatch");
    ATX_TRY(auto report_sha, atx::core::sha256_hex(report_text));
    Json manifest{{"schema", "atx-equity-book-v1"}, {"status", "complete"}, {"qualification", "unknown"},
        {"recipe", recipe}, {"parents", parents}, {"report_id", report_id},
        {"files", Json::array({request_file, certificate_file, Json{{"filename", "report/manifest.json"},
            {"sha256", report_sha}, {"size_bytes", std::to_string(report_text.size())}}})}};
    ATX_TRY(auto book_id, atx::core::sha256_hex("atx-equity-book-v1\n" + manifest.dump()));
    manifest["book_id"] = book_id;
    // I-17: manifest first, `.pending` released last.
    ATX_TRY_VOID(publish_manifest_then_release_pending(directory, [&]() -> Status {
        ATX_TRY(auto completed, publish(directory, "manifest.json", manifest));
        (void)completed;
        return Ok();
    }));
    reported->digest = fnv1a64(book_id.data(), book_id.size());
    reported->kvs.emplace_back("book_id", book_id);
    reported->kvs.emplace_back("qualification", "unknown");
    return Ok(std::move(*reported));
}
} // namespace

Result<StageResult> run_equity_book(const RunConfig &config) {
    bool reserved = false;
    const fs::path root(config.out);
    Json attempt{{"schema", "atx-equity-book-attempt-v1"}, {"status", "started"}};
    Json certificates{{"schema", "atx-equity-allocation-certificates-v1"},
        {"interpretation", "certified-callback-proposals-entire-replay-must-complete-before-performance-use"},
        {"decisions", Json::array()}, {"last_callback_attempt", nullptr}};
    const auto result = [&]() -> Result<StageResult> {
        try {
            ATX_TRY_VOID(arguments(config));
            std::error_code ec;
            if (!root.parent_path().empty()) fs::create_directories(root.parent_path(), ec);
            if (ec || !fs::create_directory(root, ec) || ec)
                return Err(ErrorCode::AlreadyExists, "equity book: output root must not exist");
            reserved = true;
            if (!fs::create_directory(root / ".pending", ec) || ec)
                return Err(ErrorCode::IoError, "equity book: cannot reserve pending marker");
            return execute(config, root, attempt, certificates);
        } catch (const std::exception &error) {
            return Err(ErrorCode::IoError, "equity book: " + std::string(error.what()));
        }
    }();
    if (!result && reserved) {
        attempt["status"] = "failed";
        attempt["error"] = result.error().to_string();
        try {
            attempt["last_callback_attempt"] = certificates["last_callback_attempt"];
            attempt["callback_failure_observed"] =
                certificates["last_callback_attempt"].is_object() &&
                certificates["last_callback_attempt"]["status"] == "failed";
            if (!fs::exists(root / "allocation_certificates.json")) {
                certificates["status"] = "failed-replay-proposals-only";
                (void)publish(root, "allocation_certificates.json", certificates);
            }
            (void)publish(root, "failure.json", attempt);
        } catch (const std::exception &) { /* Preserve original error and pending evidence. */ }
    }
    return result;
}
} // namespace atx::impl
