#pragma once

#include <atx/server/database.hpp>
#include <duckdb.h>

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace atx::server::duckdb_detail {
// Every C API owning handle has exactly one RAII owner, including failed partial results.
template<class T, void (*Destroy)(T*)> class Handle final {
public:
    Handle() = default;
    ~Handle() { Destroy(&value_); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    [[nodiscard]] T& get() noexcept { return value_; }
    [[nodiscard]] T* out() noexcept { return &value_; }
private:
    T value_{};
};
using Result = Handle<duckdb_result, duckdb_destroy_result>;
using Prepared = Handle<duckdb_prepared_statement, duckdb_destroy_prepare>;
using Extracted = Handle<duckdb_extracted_statements, duckdb_destroy_extracted>;
using LogicalType = Handle<duckdb_logical_type, duckdb_destroy_logical_type>;
struct Free { void operator()(char* value) const noexcept { duckdb_free(value); } };
using Text = std::unique_ptr<char, Free>;
[[nodiscard]] std::string quote(std::string_view text, char delimiter);
[[nodiscard]] std::string utf8_path(const std::filesystem::path&);
[[nodiscard]] std::string type_name(duckdb_logical_type type);
[[nodiscard]] std::string truncate_utf8(std::string_view text, std::size_t limit);
class Session final {
public:
    Session(const std::filesystem::path&, const DuckDbOptions&, const DataContext&);
    ~Session() = default;
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;
    [[nodiscard]] duckdb_connection connection() noexcept { return connection_.get(); }
    void check() const;
    void prepare(std::string_view sql, Prepared&);
    void execute(Prepared&, Result&);
private:
    void configure(const DuckDbOptions&);
    Handle<duckdb_database, duckdb_close> database_;
    Handle<duckdb_connection, duckdb_disconnect> connection_;
    DataContext context_;
    std::mutex wait_mutex_;
    std::condition_variable_any wake_;
    // Only the watcher writes; main thread reads after each interruptible C call.
    std::atomic<int> interrupted_{};
    // Declared last: joins before destroying borrowed context/connection and wait state.
    std::jthread watcher_;
};
[[nodiscard]] DataResult run_query(Session&, std::string_view sql, std::uint32_t max_rows);
} // namespace atx::server::duckdb_detail
