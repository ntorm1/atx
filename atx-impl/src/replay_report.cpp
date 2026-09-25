#include "replay_report.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <optional>
#include <ostream>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "artifacts.hpp"
#include "atx/core/sha256.hpp"
#include "atx/engine/book/replay.hpp"
#include "panel_pipeline.hpp"
#include "replay_diagnostics.hpp"
#include "stage_data_provenance.hpp"

namespace atx::impl {
namespace {
namespace fs = std::filesystem;
namespace book = atx::engine::book;
using Json = nlohmann::json;
using atx::core::Err;
using atx::core::ErrorCode;
using atx::core::Ok;
using atx::core::Result;
using atx::core::Status;
constexpr std::string_view kReportDomain = "atx-replay-report-v1\n";
constexpr atx::usize kMaxMetadataBytes = 1024U * 1024U;
constexpr atx::usize kMaxManifestBytes = 16U * 1024U * 1024U;

struct FitBoundary {
    std::optional<atx::usize> observation;
    std::string companion_sha256;
};

struct ValidatedPolicy {
    book::ReplayAllocationPolicy callback;
    Json recipe;
    std::vector<PanelParent> parents;
    book::ReplayIntentPolicy intent_callback;
};

Result<std::optional<ValidatedPolicy>> validate_policy(const ReplayReportPolicy *policy) {
    if (policy == nullptr) return Ok(std::optional<ValidatedPolicy>{});
    if (static_cast<bool>(policy->callback) == static_cast<bool>(policy->intent_callback) ||
        policy->recipe.empty() ||
        policy->recipe.size() > kMaxMetadataBytes || policy->parents.size() > 256) {
        return Err(ErrorCode::InvalidArgument,
                   "replay report: invalid allocation policy descriptor");
    }
    Json recipe;
    try {
        recipe = Json::parse(policy->recipe,
            [](int depth, Json::parse_event_t, Json &) {
                if (depth > 64) throw std::invalid_argument("allocation recipe depth exceeds 64");
                return true;
            });
    } catch (const Json::exception &) {
        return Err(ErrorCode::ParseError, "replay report: invalid allocation policy JSON");
    } catch (const std::invalid_argument &error) {
        return Err(ErrorCode::InvalidArgument, "replay report: " + std::string(error.what()));
    }
    if (!recipe.is_object() || recipe.dump() != policy->recipe) {
        return Err(ErrorCode::InvalidArgument,
                   "replay report: allocation policy recipe must be a canonical JSON object");
    }
    std::set<std::string> roles{"research", "books", "book-schedule", "combo", "fit-boundary"};
    for (const auto &parent : policy->parents) {
        const bool printable = std::all_of(parent.role.begin(), parent.role.end(),
            [](char value) { return value >= ' ' && value <= '~'; });
        const bool hash = parent.sha256.size() == 64 &&
            std::all_of(parent.sha256.begin(), parent.sha256.end(), [](char value) {
                return (value >= '0' && value <= '9') || (value >= 'a' && value <= 'f');
            });
        if (parent.role.empty() || parent.role.size() > 256 || !printable || !hash ||
            !roles.insert(parent.role).second) {
            return Err(ErrorCode::InvalidArgument,
                       "replay report: invalid or colliding allocation policy parent");
        }
    }
    auto parents = policy->parents;
    std::sort(parents.begin(), parents.end(), [](const PanelParent &a, const PanelParent &b) {
        return a.role < b.role;
    });
    return Ok(std::optional<ValidatedPolicy>{ValidatedPolicy{
        policy->callback, std::move(recipe), std::move(parents), policy->intent_callback}});
}

const char *intent_name(book::ReplayTargetAction action) {
    switch (action) {
    case book::ReplayTargetAction::TargetWeight: return "target-weight";
    case book::ReplayTargetAction::HoldCurrent: return "hold-current";
    case book::ReplayTargetAction::Close: return "close";
    }
    throw std::runtime_error("replay report: unknown accepted target action");
}

template <class Number> std::string number(Number value) {
    std::array<char, 64> buffer{};
    const auto parsed = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
    if (parsed.ec != std::errc{}) throw std::runtime_error("replay report numeric formatting failed");
    return {buffer.data(), parsed.ptr};
}

std::string optional_index(const std::optional<atx::usize> &index) {
    return index ? number(*index) : "";
}

std::string optional_number(const std::optional<atx::f64> &value) {
    return value ? number(*value) : "";
}

Result<atx::usize> parse_index(std::string_view text) {
    atx::usize value{};
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size()) {
        return Err(ErrorCode::ParseError, "replay report: invalid fit-boundary index");
    }
    return Ok(value);
}

Result<FitBoundary> read_fit_boundary(const RunConfig &cfg, const PipelinePanel *combo,
                                     atx::usize observations) {
    if (combo == nullptr) return Ok(FitBoundary{});
    const fs::path path = cfg.combo + ".meta";
    ATX_TRY(auto before, atx::core::sha256_file(path.string()));
    ATX_TRY_VOID(require_panel_parent(*combo->identity, "fit-boundary", before));
    std::error_code ec;
    const auto size = fs::file_size(path, ec);
    if (ec || size == 0 || size > kMaxMetadataBytes) {
        return Err(ErrorCode::ParseError, "replay report: invalid fit metadata byte size");
    }
    std::ifstream file(path, std::ios::binary);
    std::string text(static_cast<atx::usize>(size), '\0');
    if (!file.read(text.data(), static_cast<std::streamsize>(text.size())) ||
        file.peek() != std::char_traits<char>::eof()) {
        return Err(ErrorCode::IoError, "replay report: cannot read stable fit metadata");
    }
    ATX_TRY(auto read_hash, atx::core::sha256_hex(text));
    ATX_TRY(auto after, atx::core::sha256_file(path.string()));
    if (before != read_hash || before != after) {
        return Err(ErrorCode::IoError, "replay report: fit metadata changed while reading");
    }
    std::optional<atx::usize> count;
    std::optional<atx::usize> boundary;
    std::istringstream lines(text);
    std::string line;
    while (std::getline(lines, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.starts_with("n_periods=")) {
            if (count) return Err(ErrorCode::ParseError, "replay report: duplicate n_periods");
            ATX_TRY(auto value, parse_index(std::string_view(line).substr(10)));
            count = value;
        } else if (line.starts_with("holdout_begin=")) {
            if (boundary) return Err(ErrorCode::ParseError, "replay report: duplicate holdout_begin");
            ATX_TRY(auto value, parse_index(std::string_view(line).substr(14)));
            boundary = value;
        }
    }
    if (!count || !boundary || *count != observations || *boundary > observations) {
        return Err(ErrorCode::InvalidArgument, "replay report: inconsistent/missing fit boundary");
    }
    return Ok(FitBoundary{boundary, before});
}

Status reserve_directory(const fs::path &directory) {
    std::error_code ec;
    if (!directory.parent_path().empty()) {
        fs::create_directories(directory.parent_path(), ec);
        if (ec) return Err(ErrorCode::IoError, "replay report: cannot create output parent");
    }
    const bool created = fs::create_directory(directory, ec);
    if (ec || (!created && (!fs::is_directory(directory, ec) || ec ||
                           !fs::is_empty(directory, ec) || ec))) {
        return Err(ErrorCode::AlreadyExists, "replay report: output directory must be fresh");
    }
    if (!fs::create_directory(directory / ".pending", ec) || ec) {
        return Err(ErrorCode::IoError, "replay report: cannot reserve pending publication");
    }
    // Recheck after the exclusive claim, including when a caller supplied an empty
    // directory. Competing runs cannot both reserve .pending; unrelated files fail.
    for (const auto &entry : fs::directory_iterator(directory)) {
        if (entry.path().filename() != ".pending") {
            return Err(ErrorCode::AlreadyExists, "replay report: output changed during reservation");
        }
    }
    return Ok();
}

Status publish_fresh(const fs::path &partial, const fs::path &final) {
    std::error_code ec;
    fs::create_hard_link(partial, final, ec); // Atomic no-replace publication in one directory.
    if (ec) return Err(ErrorCode::IoError, "replay report: cannot publish " + final.string());
    if (!fs::remove(partial, ec) || ec) {
        return Err(ErrorCode::IoError, "replay report: cannot remove published partial");
    }
    return Ok();
}

template <class Render>
Result<Json> write_companion(const fs::path &directory, const std::string &name,
                             const Render &render) {
    const auto partial = directory / (name + ".partial");
    const auto final = directory / name;
    std::ofstream stream(partial, std::ios::binary);
    if (!stream) return Err(ErrorCode::IoError, "replay report: cannot create " + name);
    ATX_TRY_VOID(render(stream));
    stream.close();
    if (!stream) return Err(ErrorCode::IoError, "replay report: cannot finish " + name);
    ATX_TRY(auto hash, atx::core::sha256_file(partial.string()));
    std::error_code ec;
    const auto bytes = fs::file_size(partial, ec);
    if (ec) return Err(ErrorCode::IoError, "replay report: cannot stat " + name);
    ATX_TRY_VOID(publish_fresh(partial, final));
    return Ok(Json{{"filename", name}, {"sha256", hash}, {"size_bytes", number(bytes)}});
}

Status write_ledger(std::ostream &out, const book::ReplayResult &replay,
                    const PanelIdentity &identity) {
    out << "start_observation,end_observation,start_session_key_ns,end_session_key_ns,"
           "decision_observation,decision_session_key_ns,pretrade_nav,end_cash,end_assets,"
           "end_nav,gross_pnl_dollars,trade_cost_dollars,borrow_cost_dollars,net_return,"
           "absolute_trade_dollars,start_gross_dollars,end_gross_dollars\n";
    for (const auto &row : replay.intervals) {
        out << number(row.start_period) << ',' << number(row.end_period) << ','
            << number(identity.session_keys[row.start_period]) << ','
            << number(identity.session_keys[row.end_period]) << ','
            << optional_index(row.decision_period) << ',';
        if (row.decision_period) out << number(identity.session_keys[*row.decision_period]);
        out << ',' << number(row.pretrade_nav) << ',' << number(row.cash) << ','
            << number(row.assets) << ',' << number(row.nav) << ',' << number(row.gross_pnl)
            << ',' << number(row.trade_cost) << ',' << number(row.borrow_cost) << ','
            << number(row.net_return) << ',' << number(row.traded_dollars) << ','
            << number(row.start_gross) << ',' << number(row.end_gross) << '\n';
        if (!out) return Err(ErrorCode::IoError, "replay report: ledger write failed");
    }
    return Ok();
}

Status write_trades(std::ostream &out, const book::ReplayResult &replay,
                    const PanelIdentity &identity, const ReplayDiagnostics &diagnostics) {
    if (diagnostics.trade_participation.size() != replay.trades.size()) {
        return Err(ErrorCode::Internal, "replay report: trade diagnostics shape mismatch");
    }
    out << "observation,session_key_ns,decision_observation,decision_session_key_ns,"
           "instrument_index,security_id,dollar_delta,prior_dollar_adv,participation,liquidity_status\n";
    for (atx::usize index = 0; index < replay.trades.size(); ++index) {
        const auto &trade = replay.trades[index];
        const auto &liquidity = diagnostics.trade_participation[index];
        if (liquidity.trade_index != index) {
            return Err(ErrorCode::Internal, "replay report: trade diagnostics order mismatch");
        }
        out << number(trade.period) << ',' << number(identity.session_keys[trade.period]) << ','
            << number(trade.decision_period) << ','
            << number(identity.session_keys[trade.decision_period]) << ','
            << number(trade.instrument) << ',' << identity.instrument_ids[trade.instrument] << ','
            << number(trade.dollar_delta) << ',' << optional_number(liquidity.prior_dollar_adv)
            << ',' << optional_number(liquidity.participation) << ','
            << replay_liquidity_status_name(liquidity.status) << '\n';
        if (!out) return Err(ErrorCode::IoError, "replay report: trades write failed");
    }
    return Ok();
}

Status write_allocations(std::ostream &out, std::span<const book::ReplayAllocation> allocations,
                         const PanelIdentity &identity, bool intents) {
    out << "schedule_index,decision_observation,execution_observation,decision_session_key_ns,"
           "execution_session_key_ns,instrument_index,security_id,";
    out << (intents ? "resolved_intent_pretrade_nav_weight," : "requested_pretrade_nav_weight,");
    out << "pretrade_marked_dollars,posttrade_marked_dollars,pretrade_nav,posttrade_nav,"
           "pretrade_cash,posttrade_cash,allocation_absolute_trade_dollars,"
           "allocation_trade_cost_dollars";
    if (intents) out << ",target_action,intent_weight_payload";
    out << '\n';
    for (const auto &row : allocations) {
        if (row.target_weights.size() != identity.instrument_ids.size() ||
            row.pretrade_marked_dollars.size() != row.target_weights.size() ||
            row.posttrade_marked_dollars.size() != row.target_weights.size() ||
            (intents && row.target_intents.size() != row.target_weights.size()) ||
            (!intents && !row.target_intents.empty()) ||
            row.decision_period >= identity.session_keys.size() ||
            row.execution_period >= identity.session_keys.size() ||
            row.decision_session_key != identity.session_keys[row.decision_period] ||
            row.execution_session_key != identity.session_keys[row.execution_period]) {
            return Err(ErrorCode::Internal, "replay report: accepted allocation axes mismatch");
        }
        for (atx::usize i = 0; i < row.target_weights.size(); ++i) {
            out << number(row.schedule_index) << ',' << number(row.decision_period) << ','
                << number(row.execution_period) << ',' << number(row.decision_session_key) << ','
                << number(row.execution_session_key) << ',' << number(i) << ','
                << identity.instrument_ids[i] << ',' << number(row.target_weights[i]) << ','
                << number(row.pretrade_marked_dollars[i]) << ','
                << number(row.posttrade_marked_dollars[i]) << ','
                << number(row.pretrade_nav) << ',' << number(row.posttrade_nav) << ','
                << number(row.pretrade_cash) << ',' << number(row.posttrade_cash) << ','
                << number(row.traded_dollars) << ',' << number(row.trade_cost);
            if (intents) out << ',' << intent_name(row.target_intents[i].action) << ','
                             << number(row.target_intents[i].weight);
            out << '\n';
            if (!out) return Err(ErrorCode::IoError, "replay report: allocations write failed");
        }
    }
    return Ok();
}

Json performance_json(const ReplayPerformance &performance) {
    Json out{{"observed_intervals", performance.observed_intervals},
              {"initial_nav", performance.initial_nav}, {"final_nav", performance.final_nav},
              {"gross_pnl_dollars", performance.gross_pnl_dollars},
              {"net_pnl_dollars", performance.net_pnl_dollars},
              {"trade_cost_dollars", performance.trade_cost_dollars},
              {"borrow_cost_dollars", performance.borrow_cost_dollars},
              {"absolute_trade_dollars", performance.abs_trade_dollars},
              {"total_return", performance.total_return},
              {"max_drawdown", performance.max_drawdown},
              {"sharpe_252", performance.sharpe_252 ? Json(*performance.sharpe_252) : Json(nullptr)}};
    return out;
}

Json identity_json(const PanelIdentity &identity) {
    Json sessions = Json::array();
    Json original_indices = Json::array();
    for (const auto value : identity.session_keys) sessions.push_back(number(value));
    for (const auto value : identity.original_instrument_indices) original_indices.push_back(number(value));
    return Json{{"session_encoding", kPanelSessionEncoding},
                {"session_semantics", kPanelSessionSemantics},
                {"session_keys", std::move(sessions)},
                {"instrument_namespace", identity.instrument_namespace},
                {"instrument_ids", identity.instrument_ids},
                {"original_instrument_indices", std::move(original_indices)}};
}

// The replay kernel documents a stable zero-based diagnostic suffix. Enrich it
// with the exact bound date/security labels without interpreting arbitrary prose.
atx::core::Error identified_error(const atx::core::Error &error,
                                 const PanelIdentity &identity) {
    std::string message = error.message();
    const auto marker = message.rfind(" at period=");
    if (marker == std::string::npos) return error;
    std::string_view suffix(message.data() + marker + 11, message.size() - marker - 11);
    const auto split = suffix.find(" instrument=");
    const auto period = parse_index(suffix.substr(0, split));
    if (!period || *period >= identity.session_keys.size()) return error;
    std::string context = " session_key_ns=" + number(identity.session_keys[*period]);
    if (split != std::string_view::npos) {
        const auto instrument = parse_index(suffix.substr(split + 12));
        if (!instrument || *instrument >= identity.instrument_ids.size()) return error;
        context += " security_id=" + identity.instrument_ids[*instrument];
    }
    return atx::core::Error(error.code(), message + context);
}

// W0-B0 adds `ReplayConfig::allow_same_close` to the engine (reject delay 0 unless
// set). This lane runs concurrently, so the flag is forwarded whenever the engine
// field exists and the call compiles either way (no merge-order dependency).
template <class Config>
void set_allow_same_close(Config &config, bool allow) {
    if constexpr (requires { config.allow_same_close = allow; }) {
        config.allow_same_close = allow;
    } else {
        (void)config;
        (void)allow;
    }
}

Result<book::ReplayConfig> replay_config(const RunConfig &cfg,
                                        std::span<const atx::f64> planning_costs) {
    if (cfg.cost_bps != 0.0 || cfg.borrow_bps != 0.0) {
        return Err(ErrorCode::InvalidArgument,
                   "identified report: legacy --cost-bps/--borrow-bps are unsupported; use "
                   "--replay-trade-bps and --replay-annual-borrow-bps explicitly");
    }
    if (!std::isfinite(cfg.report_aum) || cfg.report_aum <= 0.0 ||
        !std::isfinite(cfg.replay_trade_bps) || cfg.replay_trade_bps < 0.0 ||
        !std::isfinite(cfg.replay_annual_borrow_bps) || cfg.replay_annual_borrow_bps < 0.0 ||
        (cfg.replay_day_basis != 360 && cfg.replay_day_basis != 365)) {
        return Err(ErrorCode::InvalidArgument, "identified report: invalid NAV/rates/day basis");
    }
    bool nonzero_planning_cost = false;
    for (const auto cost : planning_costs) {
        if (!std::isfinite(cost) || cost < 0.0) {
            return Err(ErrorCode::InvalidArgument, "identified report: invalid planning cost");
        }
        nonzero_planning_cost = nonzero_planning_cost || cost != 0.0;
    }
    if (nonzero_planning_cost && cfg.replay_trade_bps == 0.0 &&
        cfg.set_flags.count("replay-trade-bps") == 0) {
        return Err(ErrorCode::InvalidArgument,
                   "identified report: book has nonzero planning costs; choose "
                   "--replay-trade-bps explicitly (including an intentional zero)");
    }
    // W0-I0b / I-11: report costs are mandatory. A headline replayed at the silent
    // 0/0 defaults is frictionless without anyone having chosen that; both rates must
    // be chosen (a flag or config key — an intentional 0 included — or a nonzero value).
    const bool trade_chosen =
        cfg.replay_trade_bps != 0.0 || cfg.set_flags.count("replay-trade-bps") != 0;
    const bool borrow_chosen = cfg.replay_annual_borrow_bps != 0.0 ||
                               cfg.set_flags.count("replay-annual-borrow-bps") != 0;
    if (!trade_chosen || !borrow_chosen) {
        return Err(ErrorCode::InvalidArgument,
                   "identified report: report costs are mandatory; choose --replay-trade-bps "
                   "and --replay-annual-borrow-bps explicitly (0 only as a deliberate choice)");
    }
    // W0-I0b / B-02: a zero delay fills at the decision's own close.
    if (cfg.replay_execution_delay < 1 && !cfg.allow_same_close) {
        return Err(ErrorCode::InvalidArgument,
                   "identified report: --replay-execution-delay 0 fills at the signal close; "
                   "pass --allow-same-close to request it explicitly");
    }
    book::ReplayConfig result;
    result.initial_nav = cfg.report_aum;
    result.execution_delay_periods = cfg.replay_execution_delay;
    set_allow_same_close(result, cfg.allow_same_close);
    result.trade_bps = cfg.replay_trade_bps;
    result.annual_borrow_bps = cfg.replay_annual_borrow_bps;
    result.borrow_day_basis = cfg.replay_day_basis == 360
        ? book::ReplayDayBasis::D360 : book::ReplayDayBasis::D365;
    return Ok(result);
}

} // namespace

Result<StageResult> run_identified_replay_report(
    const RunConfig &cfg, const PipelinePanel &research, const PipelinePanel &books,
    std::span<const atx::usize> decisions, std::span<const atx::f64> planning_costs,
    const PipelinePanel *combo, const ReplayReportPolicy *policy) {
    try {
        ATX_TRY(auto allocation_policy, validate_policy(policy));
        if (!research.identity || !books.identity || cfg.report_out.empty() ||
            planning_costs.size() != decisions.size() || (combo && !combo->identity)) {
            return Err(ErrorCode::InvalidArgument, "replay report: identified inputs required");
        }
        ATX_TRY_VOID(require_book_schedule(books, research, decisions));
        ATX_TRY(auto schedule_sha, atx::core::sha256_file(cfg.books + ".meta.txt"));
        ATX_TRY_VOID(require_panel_parent(*books.identity, "book-schedule", schedule_sha));
        if (combo != nullptr) {
            ATX_TRY_VOID(require_pipeline_parent(*combo, research, "research"));
            ATX_TRY_VOID(require_panel_parent(*books.identity, "combo", combo->artifact_id));
        }
        ATX_TRY(auto config, replay_config(cfg, planning_costs));
        ATX_TRY(auto boundary, read_fit_boundary(cfg, combo, research.panel.dates()));
        ATX_TRY(auto executable_sha, current_executable_sha256());
        ATX_TRY(auto weight_field, books.panel.field_id("weight"));
        const fs::path directory(cfg.report_out);
        ATX_TRY_VOID(reserve_directory(directory));
        book::ReplayResult replay;
        std::vector<book::ReplayAllocation> allocations;
        if (allocation_policy) {
            auto replay_result = allocation_policy->intent_callback
                ? book::replay_scheduled_intents(research.panel, research.identity->session_keys,
                    decisions, books.panel.field_all(weight_field),
                    allocation_policy->intent_callback, config)
                : book::replay_scheduled_targets(research.panel, research.identity->session_keys,
                    decisions, books.panel.field_all(weight_field), allocation_policy->callback, config);
            if (!replay_result) {
                return Err(identified_error(replay_result.error(), *research.identity));
            }
            replay = std::move(replay_result->replay);
            allocations = std::move(replay_result->allocations);
        } else {
            auto replay_result = book::replay_scheduled_targets(
                research.panel, research.identity->session_keys, decisions,
                books.panel.field_all(weight_field), config);
            if (!replay_result) {
                return Err(identified_error(replay_result.error(), *research.identity));
            }
            replay = std::move(*replay_result);
        }
        ATX_TRY(auto diagnostics, diagnose_replay(replay, research.panel, boundary.observation));

        Json files = Json::array();
        ATX_TRY(auto ledger_file, write_companion(directory, "ledger.csv", [&](std::ostream &out) {
            return write_ledger(out, replay, *research.identity);
        }));
        files.push_back(std::move(ledger_file));
        ATX_TRY(auto trade_file, write_companion(directory, "trades.csv", [&](std::ostream &out) {
            return write_trades(out, replay, *research.identity, diagnostics);
        }));
        files.push_back(std::move(trade_file));
        ATX_TRY(auto units_file, write_companion(directory, "final_tri_units.csv",
            [&](std::ostream &out) -> Status {
                out << "instrument_index,security_id,tri_units\n";
                for (atx::usize i = 0; i < replay.final_tri_units.size(); ++i) {
                    out << number(i) << ',' << research.identity->instrument_ids[i] << ','
                        << number(replay.final_tri_units[i]) << '\n';
                }
                return out ? Ok() : Err(ErrorCode::IoError, "replay report: final units write failed");
            }));
        files.push_back(std::move(units_file));
        if (allocation_policy) {
            ATX_TRY(auto allocation_file, write_companion(directory, "allocations.csv",
                [&](std::ostream &out) {
                    return write_allocations(out, allocations, *research.identity,
                        static_cast<bool>(allocation_policy->intent_callback));
                }));
            files.push_back(std::move(allocation_file));
        }

        // The remaining summary and manifest are assembled below from the same ledger.
        Json summary{{"schema", "atx-replay-summary-v1"},
                      {"full", performance_json(diagnostics.full)},
                      {"post_fit", diagnostics.post_fit
                          ? performance_json(*diagnostics.post_fit) : Json(nullptr)},
                      {"effective_rebalances", replay.effective_rebalances},
                      {"unexecuted_decisions", replay.unexecuted_decisions},
                      {"actual_trade_count", replay.trades.size()},
                      {"final_cash", replay.final_cash}, {"final_assets", replay.final_assets},
                      {"first_post_fit_effective_observation",
                          diagnostics.first_post_fit_effective_observation
                              ? Json(*diagnostics.first_post_fit_effective_observation) : Json(nullptr)},
                      {"trade_liquidity", {
                          {"known_count", diagnostics.known_participation_count},
                          {"unknown_count", diagnostics.unknown_participation_count},
                          {"max_participation", diagnostics.max_participation
                              ? Json(*diagnostics.max_participation) : Json(nullptr)},
                          {"max_known_participation", diagnostics.max_known_participation
                              ? Json(*diagnostics.max_known_participation) : Json(nullptr)},
                          {"convention", kReplayLiquidityConvention}}},
                      {"sharpe_convention", kReplaySharpeConvention},
                      {"strategy_capacity", {{"status", "unavailable"},
                          {"reason", kReplayCapacityStatus}}},
                      {"investment_validity", "unverified-research-diagnostic"}};
        if (allocation_policy) {
            summary["allocation_model"] = allocation_policy->intent_callback
                ? "explicit-intents-against-marked-holdings-v2"
                : "callback-against-marked-holdings-v1";
            summary["books_role"] = "frozen-decision-preferences";
            summary["accepted_allocation_count"] = allocations.size();
            summary["allocation_constraints"] = "policy-defined-not-certified-by-generic-report";
        }
        ATX_TRY(auto summary_file, write_companion(directory, "summary.json",
            [&](std::ostream &out) -> Status {
                out << summary.dump(2) << '\n';
                return out ? Ok() : Err(ErrorCode::IoError, "replay report: summary write failed");
            }));
        files.push_back(std::move(summary_file));
        ATX_TRY(auto summary_text_file, write_companion(directory, "summary.txt",
            [&](std::ostream &out) -> Status {
                out << "Identified daily holdings replay (unverified research diagnostic)\n"
                    << "Initial NAV: " << number(replay.initial_nav) << '\n'
                    << "Final NAV: " << number(replay.final_nav) << '\n'
                    << "Net PnL dollars: " << number(diagnostics.full.net_pnl_dollars) << '\n'
                    << "Trade cost dollars: " << number(diagnostics.full.trade_cost_dollars) << '\n'
                    << "Borrow cost dollars: " << number(diagnostics.full.borrow_cost_dollars) << '\n'
                    << "Covered intervals: " << number(replay.intervals.size()) << '\n'
                    << "Effective rebalances: " << number(replay.effective_rebalances) << '\n'
                    << "Unexecuted decisions: " << number(replay.unexecuted_decisions) << '\n'
                    << "Strategy capacity: unavailable\n"
                    << "Historical availability, instrument types and executable fills: unverified\n";
                if (allocation_policy) {
                    out << "Input books: frozen decision preferences\n"
                        << "Applied holdings: accepted allocations against marked holdings\n"
                        << "Allocation constraints: policy-defined; "
                           "generic report does not certify\n";
                }
                return out ? Ok() : Err(ErrorCode::IoError, "replay report: text summary write failed");
            }));
        files.push_back(std::move(summary_text_file));

        const bool planning_nonzero = std::any_of(planning_costs.begin(), planning_costs.end(),
                                                  [](atx::f64 cost) { return cost != 0.0; });
        Json recipe{{"model", "daily-marked-total-return-index-holdings-v1"},
                     {"initial_nav", cfg.report_aum}, {"initial_holdings", "all-cash"},
                     {"execution_delay_observations", number(cfg.replay_execution_delay)},
                     {"execution_timing", "hypothetical-observation-close"},
                     {"trade_bps_per_absolute_dollar", cfg.replay_trade_bps},
                     {"annual_borrow_bps", cfg.replay_annual_borrow_bps},
                     {"borrow_day_basis", cfg.replay_day_basis},
                     {"borrow_accrual", "simple-actual-calendar-time-post-trade-short-dollars"},
                     {"target_basis", "pretrade-NAV"},
                     {"intermediate_policy", "hold-TRI-units-no-implicit-rebalance"},
                     {"cash_interest", 0.0}, {"dividends", "already-in-total-return-prices"},
                     {"terminal_policy", "valuation-only-no-trade-no-liquidation"},
                     {"held_missing_price_policy", "reject"},
                     {"eligibility_policy", "decision-target-gate-never-mask-held-PnL"},
                     {"stored_cost_bps_nonzero", planning_nonzero},
                     {"stored_cost_bps_role", "planning-telemetry-not-realized-debits"},
                     {"replay_trade_bps_explicit", cfg.replay_trade_bps != 0.0 ||
                         cfg.set_flags.count("replay-trade-bps") != 0},
                     {"fit_boundary_observation", boundary.observation
                         ? Json(number(*boundary.observation)) : Json(nullptr)},
                     {"post_fit_policy", "first-effective-decision-at-or-after-fit-boundary"},
                     {"sharpe_annualization", kReplaySharpeConvention},
                     {"trade_ADV", kReplayLiquidityConvention},
                     {"historical_availability", "unknown-archive-snapshot"},
                     {"historical_vintages_verified", false},
                     {"instrument_type_eligibility", "unknown"},
                     {"physical_share_execution", false},
                     {"market_impact_locates_settlement", "not-modeled"},
                     {"investment_validity", "unverified-research-diagnostic"},
                     {"producer_executable_sha256", executable_sha.empty()
                         ? "unknown" : executable_sha}};
        if (allocation_policy) {
            recipe["allocation_model"] = allocation_policy->intent_callback
                ? "explicit-intents-against-marked-holdings-v2"
                : "callback-against-marked-holdings-v1";
            recipe["allocation_policy"] = allocation_policy->recipe;
            recipe["books_role"] = "frozen-decision-preferences";
            recipe["accepted_allocation_basis"] = allocation_policy->intent_callback
                ? "explicit-intent-payloads-resolved-pretrade-NAV-weights-actual-TRI-dollars"
                : "requested-pretrade-NAV-weights-actual-TRI-dollars";
            if (allocation_policy->intent_callback) {
                recipe["hold_instruction"] = "copy-held-TRI-units-and-marked-dollars-exactly";
                recipe["close_instruction"] = "zero-units-actual-closing-dollar-delta-and-fees";
            }
            recipe["allocation_constraints"] = "policy-defined-not-certified-by-generic-report";
            recipe["allocation_csv_portfolio_columns"] =
                "cash-NAV-traded-dollars-and-fees-repeat-per-instrument-do-not-sum-rows";
        }
        Json parents = Json::array({Json{{"role", "research"}, {"sha256", research.artifact_id}},
                                    Json{{"role", "books"}, {"sha256", books.artifact_id}},
                                    Json{{"role", "book-schedule"}, {"sha256", schedule_sha}}});
        for (const auto &parent : books.identity->parents) {
            if (parent.role == "combo") {
                parents.push_back(Json{{"role", "combo"}, {"sha256", parent.sha256}});
            }
        }
        if (!boundary.companion_sha256.empty()) {
            parents.push_back(Json{{"role", "fit-boundary"},
                                   {"sha256", boundary.companion_sha256}});
        }
        if (allocation_policy) {
            for (const auto &parent : allocation_policy->parents) {
                parents.push_back(Json{{"role", parent.role}, {"sha256", parent.sha256}});
            }
        }
        recipe["combo_supplied_and_validated"] = combo != nullptr;
        Json manifest{{"schema", "atx-replay-report-v1"}, {"status", "complete"},
                       {"parents", std::move(parents)}, {"recipe", std::move(recipe)},
                       {"axes", identity_json(*research.identity)}, {"files", std::move(files)}};
        ATX_TRY(auto report_id, atx::core::sha256_hex(std::string(kReportDomain) + manifest.dump()));
        manifest["report_id"] = report_id;
        const std::string manifest_bytes = manifest.dump(2) + "\n";
        if (manifest_bytes.size() > kMaxManifestBytes) {
            return Err(ErrorCode::OutOfRange, "replay report: manifest exceeds 16 MiB");
        }
        const auto partial = directory / "manifest.json.partial";
        std::ofstream out(partial, std::ios::binary);
        out.write(manifest_bytes.data(), static_cast<std::streamsize>(manifest_bytes.size()));
        out.close();
        if (!out) return Err(ErrorCode::IoError, "replay report: manifest write failed");
        ATX_TRY_VOID(require_pipeline_file(books, "book-schedule", cfg.books + ".meta.txt"));
        if (combo) {
            ATX_TRY_VOID(require_pipeline_file(*combo, "fit-boundary", cfg.combo + ".meta"));
        }
        std::error_code ec;
        if (!fs::remove(directory / ".pending", ec) || ec) {
            return Err(ErrorCode::IoError, "replay report: cannot release pending marker");
        }
        ATX_TRY_VOID(publish_fresh(partial, directory / "manifest.json"));
        StageResult result;
        result.digest = fnv1a64(report_id.data(), report_id.size());
        const auto mode = !allocation_policy ? "identified-holdings-replay-v1"
            : allocation_policy->intent_callback ? "identified-intent-holdings-replay-v2"
                                                 : "identified-policy-holdings-replay-v1";
        result.kvs = {{"report_mode", mode},
                       {"report_id", report_id},
                       {"research_artifact_id", research.artifact_id},
                       {"books_artifact_id", books.artifact_id},
                       {"initial_nav", number(replay.initial_nav)},
                       {"final_nav", number(replay.final_nav)},
                       {"total_pnl_dollars", number(replay.final_nav - replay.initial_nav)},
                       {"trade_cost_dollars", number(diagnostics.full.trade_cost_dollars)},
                       {"borrow_cost_dollars", number(diagnostics.full.borrow_cost_dollars)},
                       {"covered_intervals", number(replay.intervals.size())},
                       {"effective_rebalances", number(replay.effective_rebalances)},
                       {"unexecuted_decisions", number(replay.unexecuted_decisions)},
                       {"strategy_capacity", "unavailable"}};
        return Ok(std::move(result));
    } catch (const std::exception &error) {
        return Err(ErrorCode::IoError, "replay report: " + std::string(error.what()));
    }
}

} // namespace atx::impl
