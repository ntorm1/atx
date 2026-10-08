#include "database_duckdb_internal.hpp"

#include <algorithm>
#include <utility>

namespace atx::server {
namespace {
using duckdb_detail::Prepared;
using duckdb_detail::Result;
using duckdb_detail::Session;
std::uint64_t elapsed(std::chrono::steady_clock::time_point started) {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started).count());
}
class DuckDbProvider final : public DatabaseProvider {
public:
    DuckDbProvider(std::filesystem::path database, DuckDbOptions options)
        : database_(std::filesystem::weakly_canonical(std::filesystem::absolute(database))),
          options_(std::move(options)) {
        const auto* version = duckdb_library_version();
        if (version == nullptr || std::string_view(version) != "v1.5.1") {
            throw DataError(DataErrorCode::unavailable, "DuckDB adapter requires runtime v1.5.1");
        }
        if (database.empty() || database == ":memory:" || options_.allowed_directories.size() > 32) {
            throw DataError(DataErrorCode::invalid, "invalid DuckDB source configuration");
        }
        for (auto& directory : options_.allowed_directories) {
            directory = std::filesystem::canonical(directory);
            if (!std::filesystem::is_directory(directory)) {
                throw DataError(DataErrorCode::invalid, "data root must be an existing directory");
            }
        }
    }
    std::string_view name() const noexcept override { return "duckdb"; }
    DataCapabilities capabilities() const noexcept override { return {}; }
    DataCatalog catalog(const DataContext& context) const override {
        Session session(database_, options_, context);
        auto rows = duckdb_detail::run_query(session,
            "SELECT table_catalog,table_schema,table_name,table_type FROM information_schema.tables"
            " WHERE table_schema NOT IN ('information_schema','pg_catalog')"
            " ORDER BY table_catalog,table_schema,table_name", 2000);
        DataCatalog result;
        result.truncated = rows.has_more || rows.truncated;
        for (const auto& row : rows.rows) {
            if (std::any_of(row.cells.begin(), row.cells.end(), [](const auto& cell) {
                return cell.truncated || cell.text.size() > 1024;
            })) {
                result.truncated = true;
                continue;
            }
            result.tables.push_back({row.cells[0].text, row.cells[1].text, row.cells[2].text,
                                     row.cells[3].text == "VIEW" ? "view" : "table"});
        }
        return result;
    }
    DataResult preview(const DataTable& table, std::uint32_t limit, std::uint32_t offset,
        const DataContext& context) const override {
        if (table.catalog.empty() || table.schema.empty() || table.name.empty() ||
            limit == 0 || limit > 500 || offset > 1000000) {
            throw DataError(DataErrorCode::invalid, "DuckDB preview requires qualified table and bounds");
        }
        const auto started = std::chrono::steady_clock::now();
        Session session(database_, options_, context);
        Prepared metadata;
        session.prepare("SELECT table_name FROM information_schema.tables WHERE table_catalog=?"
            " AND table_schema=? AND table_name=? LIMIT 1", metadata);
        for (const auto& [position, value] : {
                std::pair<idx_t, const std::string&>{1, table.catalog},
                {2, table.schema}, {3, table.name}}) {
            if (duckdb_bind_varchar_length(metadata.get(), position, value.data(), value.size()) !=
                DuckDBSuccess) throw DataError(DataErrorCode::invalid, "invalid table identifier");
        }
        Result found;
        session.execute(metadata, found);
        if (duckdb_row_count(&found.get()) == 0) {
            throw DataError(DataErrorCode::missing, "table is not present in configured source");
        }
        const auto sql = "SELECT * FROM " + duckdb_detail::quote(table.catalog, '"') + '.' +
            duckdb_detail::quote(table.schema, '"') + '.' + duckdb_detail::quote(table.name, '"') +
            " LIMIT " + std::to_string(limit + 1) + " OFFSET " + std::to_string(offset);
        auto result = duckdb_detail::run_query(session, sql, limit);
        result.elapsed_ms = elapsed(started);
        result.warnings.push_back("Preview order follows the provider; use ORDER BY for stable pages.");
        return result;
    }
    DataResult query(std::string_view sql, std::uint32_t max_rows,
        const DataContext& context) const override {
        if (sql.empty() || sql.size() > 65536 || sql.find('\0') != std::string_view::npos ||
            max_rows == 0 || max_rows > 500) {
            throw DataError(DataErrorCode::invalid, "invalid DuckDB query bounds");
        }
        const auto started = std::chrono::steady_clock::now();
        Session session(database_, options_, context);
        auto result = duckdb_detail::run_query(session, sql, max_rows);
        result.elapsed_ms = elapsed(started);
        return result;
    }
private:
    const std::filesystem::path database_;
    DuckDbOptions options_;
};
} // namespace
std::shared_ptr<const DatabaseProvider> make_duckdb_provider(std::filesystem::path database,
    DuckDbOptions options) {
    return std::make_shared<DuckDbProvider>(std::move(database), std::move(options));
}
} // namespace atx::server
