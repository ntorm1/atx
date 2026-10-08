#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace atx::server {
enum class DataErrorCode { invalid, missing, unavailable, capacity, deadline, cancelled };
class DataError final : public std::runtime_error {
public:
    DataError(DataErrorCode code, std::string message);
    [[nodiscard]] DataErrorCode code() const noexcept;
private:
    DataErrorCode code_;
};
struct DataCapabilities {
    bool browse{true};
    bool query{true};
    std::uint32_t max_rows{500};
    std::uint32_t max_offset{1000000};
    std::uint32_t max_query_bytes{65536};
    std::uint32_t timeout_ms{5000};
};
// The optional cancellation callback must be thread-safe. It is borrowed only during a call.
struct DataContext {
    std::chrono::steady_clock::time_point deadline{
        std::chrono::steady_clock::now() + std::chrono::seconds(5)};
    std::stop_token stop;
    std::function<bool()> cancelled;
    void check() const;
};
struct DataTable { std::string catalog, schema, name, kind; };
struct DataCatalog { std::vector<DataTable> tables; bool truncated{}; };
struct DataColumn { std::string name, type; };
struct DataCell { std::string text; bool is_null{}; bool truncated{}; };
struct DataRow { std::vector<DataCell> cells; };
struct DataResult {
    std::vector<DataColumn> columns;
    std::vector<DataRow> rows;
    bool has_more{};
    bool truncated{};
    std::uint64_t elapsed_ms{};
    std::vector<std::string> warnings;
};
// Providers are immutable and support simultaneous operations on independent connections.
class DatabaseProvider {
public:
    virtual ~DatabaseProvider() = default;
    [[nodiscard]] virtual std::string_view name() const noexcept = 0;
    [[nodiscard]] virtual DataCapabilities capabilities() const noexcept = 0;
    [[nodiscard]] virtual DataCatalog catalog(const DataContext&) const = 0;
    [[nodiscard]] virtual DataResult preview(const DataTable&, std::uint32_t limit,
                                            std::uint32_t offset, const DataContext&) const = 0;
    [[nodiscard]] virtual DataResult query(std::string_view sql, std::uint32_t max_rows,
                                          const DataContext&) const = 0;
};
struct DuckDbOptions { std::vector<std::filesystem::path> allowed_directories; };
[[nodiscard]] std::shared_ptr<const DatabaseProvider> make_duckdb_provider(
    std::filesystem::path database, DuckDbOptions options = {});
struct DataSource {
    std::string id, name;
    // Shared ownership permits a registry and an operator-owned provider to share immutable config.
    std::shared_ptr<const DatabaseProvider> provider;
};
// Registry configuration is frozen at construction; admission alone is mutable and atomic.
class DatabaseRegistry final {
public:
    explicit DatabaseRegistry(std::vector<DataSource> sources = {});
    ~DatabaseRegistry();
    DatabaseRegistry(const DatabaseRegistry&) = delete;
    DatabaseRegistry& operator=(const DatabaseRegistry&) = delete;
    DatabaseRegistry(DatabaseRegistry&&) = delete;
    DatabaseRegistry& operator=(DatabaseRegistry&&) = delete;
    [[nodiscard]] const std::vector<DataSource>& sources() const noexcept;
    [[nodiscard]] DataCatalog catalog(std::string_view source, const DataContext& = {}) const;
    [[nodiscard]] DataResult preview(std::string_view source, const DataTable& table,
        std::uint32_t limit = 200, std::uint32_t offset = 0, const DataContext& = {}) const;
    [[nodiscard]] DataResult query(std::string_view source, std::string_view sql,
        std::uint32_t max_rows = 200, const DataContext& = {}) const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace atx::server
