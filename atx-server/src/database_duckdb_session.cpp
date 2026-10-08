#include "database_duckdb_internal.hpp"

#include <algorithm>
#include <array>
#include <system_error>
#include <utility>

namespace atx::server::duckdb_detail {
std::string utf8_path(const std::filesystem::path& path) {
    const auto bytes = path.generic_u8string();
    // SAFETY: char may inspect any object's byte representation; char8_t is one UTF-8 byte.
    return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}
std::string quote(std::string_view text, char delimiter) {
    std::string result(1, delimiter);
    for (const auto ch : text) {
        result += ch;
        if (ch == delimiter) result += ch;
    }
    result += delimiter;
    return result;
}
Session::Session(const std::filesystem::path& path, const DuckDbOptions& options,
    const DataContext& context) : context_(context) {
    context_.deadline = std::min(context_.deadline,
        std::chrono::steady_clock::now() + std::chrono::seconds(5));
    check();
    std::error_code file_error;
    if (!std::filesystem::is_regular_file(path, file_error) || file_error) {
        throw DataError(DataErrorCode::unavailable, "configured database file is unavailable");
    }
    Handle<duckdb_config, duckdb_destroy_config> config;
    if (duckdb_create_config(config.out()) != DuckDBSuccess) {
        throw DataError(DataErrorCode::unavailable, "cannot initialize database configuration");
    }
    const std::array<std::pair<const char*, const char*>, 9> settings{{
        {"access_mode", "READ_ONLY"}, {"memory_limit", "256MiB"}, {"threads", "1"},
        {"temp_directory", ""}, {"max_temp_directory_size", "0B"},
        {"autoload_known_extensions", "false"}, {"autoinstall_known_extensions", "false"},
        {"allow_unsigned_extensions", "false"}, {"allow_community_extensions", "false"}}};
    for (const auto& [key, value] : settings) {
        if (duckdb_set_config(config.get(), key, value) != DuckDBSuccess) {
            throw DataError(DataErrorCode::unavailable, "database safety configuration rejected");
        }
    }
    char* open_error{};
    const auto opened = duckdb_open_ext(utf8_path(path).c_str(), database_.out(), config.get(),
                                      &open_error);
    const Text error(open_error);
    if (opened != DuckDBSuccess) {
        throw DataError(DataErrorCode::unavailable, "database unavailable or leased by a writer");
    }
    if (duckdb_connect(database_.get(), connection_.out()) != DuckDBSuccess) {
        throw DataError(DataErrorCode::unavailable, "cannot establish database connection");
    }
    configure(options);
    check();
    watcher_ = std::jthread([this](std::stop_token stop) {
        for (; !stop.stop_requested();) {
            try { context_.check(); }
            catch (const DataError& error) {
                interrupted_.store(error.code() == DataErrorCode::deadline ? 1 : 2);
                duckdb_interrupt(connection_.get());
                return;
            } catch (...) {
                interrupted_.store(2);
                duckdb_interrupt(connection_.get());
                return;
            }
            std::unique_lock lock(wait_mutex_);
            wake_.wait_for(lock, stop, std::chrono::milliseconds(5), [] { return false; });
        }
    });
}
void Session::configure(const DuckDbOptions& options) {
    std::string directories = "[";
    for (std::size_t i = 0; i < options.allowed_directories.size(); ++i) {
        if (i != 0) directories += ',';
        directories += quote(utf8_path(options.allowed_directories[i]), '\'');
    }
    directories += ']';
    const std::array<std::string, 3> statements{
        "SET allowed_directories=" + directories,
        "SET enable_external_access=false", "SET lock_configuration=true"};
    // Trusted fixed statements only. All security settings precede any request SQL binding.
    for (const auto& sql : statements) {
        Result result;
        if (duckdb_query(connection_.get(), sql.c_str(), result.out()) != DuckDBSuccess) {
            throw DataError(DataErrorCode::unavailable, "database safety configuration unavailable");
        }
    }
}
void Session::check() const {
    if (interrupted_.load() == 1) {
        throw DataError(DataErrorCode::deadline, "database operation deadline exceeded");
    }
    if (interrupted_.load() == 2) {
        throw DataError(DataErrorCode::cancelled, "database operation cancelled");
    }
    context_.check();
}
void Session::prepare(std::string_view sql, Prepared& prepared) {
    check();
    Extracted statements;
    const std::string owned(sql);
    const auto count = duckdb_extract_statements(connection_.get(), owned.c_str(), statements.out());
    check();
    if (count != 1) throw DataError(DataErrorCode::invalid, "exactly one SELECT statement is required");
    const auto state = duckdb_prepare_extracted_statement(connection_.get(), statements.get(), 0,
                                                         prepared.out());
    check();
    if (state != DuckDBSuccess) {
        const auto* error = duckdb_prepare_error(prepared.get());
        if (error != nullptr && std::string_view(error).starts_with("Out of Memory Error:")) {
            throw DataError(DataErrorCode::capacity, "database query binding memory budget exhausted");
        }
        throw DataError(DataErrorCode::invalid,
            error == nullptr ? "invalid SELECT query" : truncate_utf8(error, 2048));
    }
    if (duckdb_prepared_statement_type(prepared.get()) != DUCKDB_STATEMENT_TYPE_SELECT) {
        throw DataError(DataErrorCode::invalid, "only SELECT statements are permitted");
    }
}
void Session::execute(Prepared& prepared, Result& result) {
    check();
    const auto state = duckdb_execute_prepared(prepared.get(), result.out());
    check();
    if (state == DuckDBSuccess) return;
    const auto type = duckdb_result_error_type(&result.get());
    if (type == DUCKDB_ERROR_OUT_OF_MEMORY) {
        throw DataError(DataErrorCode::capacity, "database query memory budget exhausted");
    }
    if (type == DUCKDB_ERROR_IO || type == DUCKDB_ERROR_CONNECTION) {
        throw DataError(DataErrorCode::unavailable, "database source is temporarily unavailable");
    }
    const auto* error = duckdb_result_error(&result.get());
    throw DataError(DataErrorCode::invalid,
        error == nullptr ? "SELECT execution failed" : truncate_utf8(error, 2048));
}
} // namespace atx::server::duckdb_detail
