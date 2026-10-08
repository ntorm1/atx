#include "library_validation.hpp"
#include "workspace_validation.hpp"
#include "governance_store.hpp"
#include <atx/core/sha256.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <set>

namespace atx::server::library_detail {
namespace {
using Json = nlohmann::json;
void string_field(const Json& json, const char* key, std::size_t max, bool required = false) {
    if (!json.contains(key)) {
        if (required) throw Error(ErrorCode::invalid, std::string(key) + " is required");
        return;
    }
    if (!json[key].is_string()) throw Error(ErrorCode::invalid, "recipe field must be a string");
    const auto& text = json[key].get_ref<const std::string&>();
    workspace_detail::validate_text(text, max);
    if (text.empty()) throw Error(ErrorCode::invalid, "recipe string must not be empty");
}
void number_field(const Json& json, const char* key, double low, double high,
    bool integral = false) {
    if (!json.contains(key)) return;
    if (!json[key].is_number()) throw Error(ErrorCode::invalid, "recipe field must be numeric");
    const auto value = json[key].get<double>();
    if (!std::isfinite(value) || value < low || value > high ||
        (integral && std::floor(value) != value)) {
        throw Error(ErrorCode::invalid, std::string(key) + " is outside recipe bounds");
    }
}
void date_fields(const Json& json) {
    for (const auto* key : {"start_date", "end_date"}) {
        string_field(json, key, 10);
        if (!json.contains(key)) continue;
        const auto& text = json[key].get_ref<const std::string&>();
        if (text.size() != 10 || text[4] != '-' || text[7] != '-') {
            throw Error(ErrorCode::invalid, "recipe dates must use YYYY-MM-DD");
        }
        for (std::size_t i = 0; i < text.size(); ++i) {
            if (i != 4 && i != 7 && (text[i] < '0' || text[i] > '9')) {
                throw Error(ErrorCode::invalid, "recipe date contains non-digits");
            }
        }
        const auto year = std::stoi(text.substr(0, 4));
        const auto month = static_cast<unsigned>(std::stoi(text.substr(5, 2)));
        const auto day = static_cast<unsigned>(std::stoi(text.substr(8, 2)));
        if (year == 0 || !std::chrono::year_month_day{std::chrono::year{year},
            std::chrono::month{month}, std::chrono::day{day}}.ok()) {
            throw Error(ErrorCode::invalid, "recipe date is not a calendar date");
        }
    }
    if (json.contains("start_date") && json.contains("end_date") &&
        json["start_date"].get<std::string>() > json["end_date"].get<std::string>()) {
        throw Error(ErrorCode::invalid, "recipe start date exceeds end date");
    }
}
void digest_field(const Json& json, const char* key) {
    string_field(json, key, 64, true);
    const auto& text = json[key].get_ref<const std::string&>();
    if (text.size() != 64 || !std::all_of(text.begin(), text.end(), [](char ch) {
        return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f');
    })) throw Error(ErrorCode::invalid, "recipe SHA256 must contain 64 lowercase hex digits");
}
void portfolio_fields(const Json& json) {
    if (!json.contains("components") || !json["components"].is_array() ||
        json["components"].size() < 2 || json["components"].size() > 12) {
        throw Error(ErrorCode::invalid, "portfolio requires 2..12 frozen components");
    }
    const std::set<std::string> keys{"strategy_dsl", "expression_sha256", "weight"};
    double gross{};
    for (const auto& item : json["components"]) {
        if (!item.is_object()) throw Error(ErrorCode::invalid, "component must be an object");
        for (const auto& field : item.items()) {
            if (!keys.contains(field.key())) throw Error(ErrorCode::invalid, "unknown component field");
        }
        string_field(item, "strategy_dsl", 1024, true);
        digest_field(item, "expression_sha256");
        if (!item.contains("weight")) throw Error(ErrorCode::invalid, "component weight required");
        number_field(item, "weight", -1e6, 1e6);
        const auto digest = atx::core::sha256_hex(item["strategy_dsl"].get<std::string>());
        if (!digest) throw Error(ErrorCode::unavailable, "expression hashing failed");
        if (item["expression_sha256"] != *digest) {
            throw Error(ErrorCode::conflict, "component expression SHA256 does not match");
        }
        gross += std::abs(item["weight"].get<double>());
    }
    if (gross <= 1e-12) throw Error(ErrorCode::invalid, "portfolio component weights are zero");
    number_field(json, "gross_limit", 0, 2);
    number_field(json, "name_cap", 0.001, 1);
    number_field(json, "max_turnover", 0, 2);
    if (json.contains("signal_decay_sessions") && !json["signal_decay_sessions"].is_number_integer()) {
        throw Error(ErrorCode::invalid, "signal decay sessions must be an integer");
    }
    number_field(json, "signal_decay_sessions", 1, 21, true);
}
} // namespace
std::string canonical_recipe(std::string_view text) {
    const auto json = workspace_detail::parse_json(text, 65536);
    if (!json.is_object()) throw Error(ErrorCode::invalid, "recipe must be a JSON object");
    string_field(json, "mode", 32);
    const auto mode = json.value("mode", std::string("synthetic_demo"));
    const std::set<std::string> demo{"mode", "strategy_id", "sessions", "initial_capital",
        "trade_bps", "annual_borrow_bps", "strategy_dsl"};
    const std::set<std::string> imported{"mode", "strategy_id", "artifact_path", "summary_path",
        "start_date", "end_date"};
    const std::set<std::string> real{"mode", "strategy_id", "combined_path", "combined_sha256",
        "role_path", "role_sha256", "rule", "cadence", "trade_fraction", "monthly_budget",
        "start_date", "end_date"};
    const std::set<std::string> dsl{"mode", "strategy_id", "dataset_id", "dataset_sha256",
        "strategy_dsl", "start_date", "end_date", "instrument_limit", "initial_capital",
        "trade_bps", "annual_borrow_bps", "cadence", "neutralization",
        "execution_delay_sessions"};
    auto portfolio = dsl;
    portfolio.erase("strategy_dsl");
    portfolio.insert({"components", "gross_limit", "name_cap", "max_turnover",
        "signal_decay_sessions"});
    if (mode != "synthetic_demo" && mode != "import_nav" && mode != "real_nav" &&
        mode != "real_dsl" && mode != "real_portfolio") {
        throw Error(ErrorCode::invalid, "unsupported recipe research mode");
    }
    const auto& keys = mode == "synthetic_demo" ? demo : mode == "import_nav" ? imported :
        mode == "real_dsl" ? dsl : mode == "real_portfolio" ? portfolio : real;
    for (const auto& item : json.items()) {
        if (!keys.contains(item.key())) throw Error(ErrorCode::invalid, "unknown recipe field");
    }
    string_field(json, "strategy_id", 64);
    const auto expected = mode == "synthetic_demo" ? "synthetic-long-short-v1" :
        mode == "import_nav" ? "imported-nav" : mode == "real_dsl" ?
        "real-data-long-short-v1" : mode == "real_portfolio" ? "real-data-portfolio-v1" :
        "saved-blend-nav";
    if (json.contains("strategy_id") && json["strategy_id"] != expected) {
        throw Error(ErrorCode::invalid, "recipe strategy_id does not match mode");
    }
    if (mode == "synthetic_demo" || mode == "real_dsl" || mode == "real_portfolio") {
        if (mode == "synthetic_demo") number_field(json, "sessions", 10, 756, true);
        number_field(json, "initial_capital", 1, 1e12);
        number_field(json, "trade_bps", 0, 100);
        number_field(json, "annual_borrow_bps", 0, 5000);
        string_field(json, "strategy_dsl", 1024);
        if (mode == "real_dsl" || mode == "real_portfolio") {
            if (mode == "real_portfolio") portfolio_fields(json);
            string_field(json, "dataset_id", 64, true);
            const auto& id = json["dataset_id"].get_ref<const std::string&>();
            if (!std::all_of(id.begin(), id.end(), [](char ch) {
                return (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
                    (ch >= '0' && ch <= '9') || ch == '_' || ch == '-';
            })) throw Error(ErrorCode::invalid, "dataset id must use ASCII identifier characters");
            digest_field(json, "dataset_sha256");
            date_fields(json);
            const auto start = json.value("start_date", std::string("2020-01-01"));
            const auto end = json.value("end_date", std::string(mode == "real_portfolio" ?
                "2020-03-31" : "2022-12-31"));
            if (start < "2018-01-01" || end > "2022-12-31" || start > end) {
                throw Error(ErrorCode::invalid, "real DSL recipe must remain within TRAIN dates");
            }
            number_field(json, "instrument_limit", 2, 512, true);
            number_field(json, "cadence", 1, 252, true);
            number_field(json, "execution_delay_sessions", 1, 5, true);
            string_field(json, "neutralization", 16);
            if (json.contains("neutralization") && json["neutralization"] != "dollar") {
                throw Error(ErrorCode::invalid, "only dollar neutralization is available");
            }
        }
    } else {
        date_fields(json);
        if (mode == "import_nav") {
            string_field(json, "artifact_path", 4096, true);
            string_field(json, "summary_path", 4096);
        } else {
            string_field(json, "combined_path", 4096, true);
            string_field(json, "role_path", 4096, true);
            digest_field(json, "combined_sha256");
            digest_field(json, "role_sha256");
            string_field(json, "rule", 64);
            if (json.contains("rule") && json["rule"] != "baseline-v1" &&
                json["rule"] != "monthly-budget-v2" && json["rule"] != "aim-partial-v5") {
                throw Error(ErrorCode::invalid, "unsupported recipe target rule");
            }
            number_field(json, "cadence", 1, 252, true);
            number_field(json, "trade_fraction", 0.001, 1);
            number_field(json, "monthly_budget", 0.001, 2);
        }
    }
    auto result = json.dump();
    if (result.size() > 65536) throw Error(ErrorCode::invalid, "canonical recipe is too large");
    return result;
}
void validate_project(std::string_view project) {
    if (!project.empty() && project != "local") {
        throw Error(ErrorCode::invalid, "only the shared local project is available");
    }
}
void validate_page(std::uint32_t limit, std::uint32_t offset) {
    if (limit == 0 || limit > 100 || offset > 1000000) {
        throw Error(ErrorCode::invalid, "invalid library pagination");
    }
}
Input validate(const AlphaInput& input) {
    governance_detail::validate_actor(input.actor_id);
    validate_project(input.project_id);
    workspace_detail::validate_text(input.description, 512);
    workspace_detail::validate_text(input.hypothesis, 4096);
    if (!input.source_job_id.empty()) workspace_detail::validate_id(input.source_job_id);
    if (input.tags.size() > 16) {
        throw Error(ErrorCode::invalid, "at most 16 alpha tags are allowed");
    }
    std::set<std::string> tags;
    for (const auto& tag : input.tags) {
        workspace_detail::validate_text(tag, 64);
        if (tag.empty() || tag != workspace_detail::validate_name(tag) ||
            !tags.insert(tag).second) {
            throw Error(ErrorCode::invalid, "alpha tags must be unique, trimmed and nonempty");
        }
    }
    const auto request = canonical_recipe(input.request_json);
    const auto hash = atx::core::sha256_hex(request);
    if (!hash) throw Error(ErrorCode::unavailable, "recipe hashing failed");
    return {workspace_detail::validate_name(input.name), input.description,
        Json(std::vector<std::string>(tags.begin(), tags.end())).dump(), input.hypothesis,
        request, *hash, input.source_job_id, "local", input.actor_id};
}
} // namespace atx::server::library_detail
