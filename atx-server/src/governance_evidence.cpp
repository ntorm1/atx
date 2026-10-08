#include "governance_store.hpp"
#include "store_sql.hpp"
#include "library_validation.hpp"
#include "workspace_validation.hpp"
#include <atx/core/sha256.hpp>

namespace atx::server::governance_detail {
namespace {
using Json = nlohmann::json;
std::string digest(std::string_view value) {
    const auto hash = atx::core::sha256_hex(value);
    if (!hash) throw Error(ErrorCode::unavailable, "evidence hashing failed");
    return *hash;
}
const Json& required(const Json& json, std::string_view field) {
    const auto found = json.find(field);
    if (found == json.end()) throw Error(ErrorCode::invalid, "review evidence is incomplete");
    return *found;
}
std::string text(const Json& json, std::size_t max_bytes) {
    if (!json.is_string()) throw Error(ErrorCode::invalid, "invalid evidence text");
    auto value = json.get<std::string>();
    workspace_detail::validate_text(value, max_bytes);
    return value;
}
Json recipe_authors(sqlite3* db, const AlphaSnapshot& alpha) {
    using namespace store_detail;
    auto query = prepare(db, "SELECT DISTINCT actor_id FROM alpha_revisions WHERE alpha_id=? "
        "AND request_sha256=? AND revision<=? ORDER BY actor_id LIMIT 16385");
    bind_value(query, 1, alpha.id); bind_value(query, 2, alpha.request_sha256);
    bind_value(query, 3, static_cast<std::int64_t>(alpha.revision));
    auto actors = Json::array();
    std::size_t bytes{};
    for (unsigned i = 0; i < 16385 && step(query) == SQLITE_ROW; ++i) {
        const auto actor = column(query, 0);
        validate_actor(actor);
        // ASCII identifiers need only two JSON quotes and one separator each.
        bytes += actor.size() + 3;
        if (i == 16384 || bytes > 96 * 1024) {
            throw Error(ErrorCode::invalid, "recipe author evidence exceeds resource limits");
        }
        actors.push_back(actor);
    }
    return actors;
}
Json extract(const Json& result, const AlphaSnapshot& alpha, std::string_view request) {
    if (!result.is_object() || required(result, "schema_version") != "atx.research-result/v1") {
        throw Error(ErrorCode::invalid, "unsupported research evidence schema");
    }
    const auto recipe = workspace_detail::parse_json(alpha.request_json, 65536);
    const auto mode = recipe.value("mode", std::string{"synthetic_demo"});
    if (required(result, "mode") != mode) {
        throw Error(ErrorCode::conflict, "result mode does not match alpha recipe");
    }
    const auto& observations = required(required(result, "metrics"), "observations");
    if (!observations.is_number_integer() || observations <= 0 || observations > 1000000) {
        throw Error(ErrorCode::invalid, "evidence requires bounded positive observations");
    }
    const auto& provenance = required(result, "provenance");
    const auto& synthetic = required(provenance, "synthetic");
    if (!synthetic.is_boolean() || synthetic.get<bool>() != (mode == "synthetic_demo")) {
        throw Error(ErrorCode::invalid, "evidence synthetic provenance is inconsistent");
    }
    const auto evaluation = text(required(provenance, "evaluation"), 64);
    if (evaluation.empty()) throw Error(ErrorCode::invalid, "missing evaluation provenance");
    const auto& warnings = required(provenance, "warnings");
    if (!warnings.is_array() || warnings.size() > 32) {
        throw Error(ErrorCode::invalid, "evidence warnings exceed limits");
    }
    for (const auto& warning : warnings) static_cast<void>(text(warning, 512));
    if (provenance.contains("submitted_request_sha256") &&
        text(provenance["submitted_request_sha256"], 64) != digest(request)) {
        throw Error(ErrorCode::conflict, "result provenance does not match submitted recipe");
    }
    return {{"synthetic", synthetic}, {"evaluation", evaluation},
        {"observations", observations}, {"warnings", warnings}, {"mode", mode}};
}
} // namespace
std::string evidence(sqlite3* db, const AlphaSnapshot& alpha,
    const std::vector<std::string>& jobs, std::string_view author) {
    using namespace store_detail;
    Json result{{"schema_version", 1}, {"revision_actor_id", alpha.actor_id},
        {"recipe_actor_ids", recipe_authors(db, alpha)},
        {"jobs", Json::array()}, {"warnings", Json::array()}};
    if (author == "migration_unknown") {
        result["warnings"].push_back("Original alpha author is unknown after legacy migration");
    }
    std::size_t total_bytes{};
    for (const auto& id : jobs) {
        // Bound durable evidence before copying SQLite blobs or parsing JSON.
        auto sizes = prepare(db, "SELECT length(CAST(result_json AS BLOB)) FROM jobs WHERE id=?");
        bind_value(sizes, 1, id);
        if (step(sizes) != SQLITE_ROW) throw Error(ErrorCode::missing, "evidence job not found");
        const auto bytes = sqlite3_column_int64(sizes.get(), 0);
        if (bytes <= 0 || bytes > 16 * 1024 * 1024 ||
            static_cast<std::uint64_t>(bytes) > 16 * 1024 * 1024 - total_bytes) {
            throw Error(ErrorCode::invalid, "review evidence results exceed 16 MiB");
        }
        total_bytes += static_cast<std::size_t>(bytes);
        auto query = prepare(db, "SELECT status,request_json,result_json,alpha_id,alpha_revision,"
            "actor_id FROM jobs WHERE id=?");
        bind_value(query, 1, id);
        if (step(query) != SQLITE_ROW) throw Error(ErrorCode::missing, "evidence job not found");
        const auto request = column(query, 1);
        if (column(query, 0) != "SUCCEEDED" ||
            library_detail::canonical_recipe(request) != alpha.request_json ||
            (!column(query, 3).empty() && (column(query, 3) != alpha.id ||
                static_cast<std::uint64_t>(sqlite3_column_int64(query.get(), 4)) !=
                alpha.revision))) {
            throw Error(ErrorCode::conflict, "evidence requires successful exact alpha-recipe runs");
        }
        const auto raw = column(query, 2);
        auto item = extract(workspace_detail::parse_json(raw, 16 * 1024 * 1024), alpha, request);
        item["job_id"] = id; item["result_sha256"] = digest(raw);
        item["request_sha256"] = alpha.request_sha256;
        item["actor_id"] = column(query, 5);
        if (column(query, 5) == "migration_unknown") {
            result["warnings"].push_back("Run " + id +
                " actor is unknown after legacy migration");
        }
        result["jobs"].push_back(std::move(item));
    }
    auto encoded = result.dump();
    if (encoded.size() > 96 * 1024) throw Error(ErrorCode::invalid, "review evidence exceeds 96 KiB");
    return encoded;
}
} // namespace atx::server::governance_detail
