#include "database_duckdb_internal.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <string_view>
#include <utility>

namespace atx::server::duckdb_detail {
std::string type_name(duckdb_logical_type type) {
    const auto id = duckdb_get_type_id(type);
    if (id == DUCKDB_TYPE_DECIMAL) {
        return "DECIMAL(" + std::to_string(duckdb_decimal_width(type)) + "," +
            std::to_string(duckdb_decimal_scale(type)) + ")";
    }
    constexpr std::array<std::string_view, 40> names{
        "INVALID", "BOOLEAN", "TINYINT", "SMALLINT", "INTEGER", "BIGINT", "UTINYINT",
        "USMALLINT", "UINTEGER", "UBIGINT", "FLOAT", "DOUBLE", "TIMESTAMP", "DATE", "TIME",
        "INTERVAL", "HUGEINT", "VARCHAR", "BLOB", "DECIMAL", "TIMESTAMP_S", "TIMESTAMP_MS",
        "TIMESTAMP_NS", "ENUM", "LIST", "STRUCT", "MAP", "UUID", "UNION", "BIT",
        "TIME WITH TIME ZONE", "TIMESTAMP WITH TIME ZONE", "UHUGEINT", "ARRAY", "ANY",
        "BIGNUM", "NULL", "STRING_LITERAL", "INTEGER_LITERAL", "TIME_NS"};
    const auto index = static_cast<std::uint32_t>(id);
    return index < names.size() ? std::string(names[index]) : "UNKNOWN";
}
std::string truncate_utf8(std::string_view text, std::size_t limit) {
    auto bytes = std::min(limit, text.size());
    // DuckDB strings are valid UTF-8. A byte cap must not split a continuation sequence.
    if (bytes < text.size()) {
        for (; bytes > 0 && (static_cast<unsigned char>(text[bytes]) & 0xc0U) == 0x80U; --bytes) {}
    }
    return std::string(text.substr(0, bytes));
}
namespace {
std::vector<DataColumn> columns(Prepared& prepared) {
    const auto count = duckdb_prepared_statement_column_count(prepared.get());
    if (count > 256) throw DataError(DataErrorCode::capacity, "query exceeds 256 columns");
    std::vector<DataColumn> result;
    result.reserve(static_cast<std::size_t>(count));
    for (idx_t i = 0; i < count; ++i) {
        const auto* raw_name = duckdb_prepared_statement_column_name(prepared.get(), i);
        // SAFETY: this API returns an allocated string as const; freeing never mutates content.
        const Text owned_name(const_cast<char*>(raw_name));
        if (raw_name == nullptr || std::strlen(raw_name) > 1024) {
            throw DataError(DataErrorCode::capacity, "query column name exceeds 1024 bytes");
        }
        LogicalType logical;
        logical.get() = duckdb_prepared_statement_column_logical_type(prepared.get(), i);
        if (logical.get() == nullptr) throw DataError(DataErrorCode::invalid, "unknown column type");
        result.push_back({raw_name, type_name(logical.get())});
    }
    return result;
}
std::string rendering_sql(std::string_view sql, std::size_t count, std::uint32_t max_rows,
    std::size_t character_limit) {
    std::string aliases, expressions;
    for (std::size_t i = 0; i < count; ++i) {
        if (i != 0) { aliases += ','; expressions += ','; }
        const auto alias = "\"__atx_c" + std::to_string(i) + "\"";
        aliases += alias;
        // Bound text inside DuckDB too, including converted nested/BLOB/decimal values.
        expressions += "LEFT(CAST(" + alias + " AS VARCHAR)," +
            std::to_string(character_limit + 1) + ")";
    }
    // query() parses an opaque SQL string; terminators/comments and quoted semicolons survive.
    // The original statement has already passed native extraction and SELECT type validation.
    return "WITH \"__atx_rows\"(" + aliases + ") AS (SELECT * FROM query(" + quote(sql, '\'') +
        ") LIMIT " + std::to_string(max_rows + 1) + ") SELECT " + expressions +
        " FROM \"__atx_rows\"";
}
bool add_row(duckdb_data_chunk chunk, idx_t row, DataResult& result, std::size_t& budget,
    std::size_t character_limit) {
    DataRow output;
    output.cells.reserve(result.columns.size());
    auto used = std::size_t{32};
    for (idx_t col = 0; col < result.columns.size(); ++col) {
        DataCell cell;
        const auto vector = duckdb_data_chunk_get_vector(chunk, col);
        auto* const validity = duckdb_vector_get_validity(vector);
        cell.is_null = validity != nullptr && !duckdb_validity_row_is_valid(validity, row);
        if (!cell.is_null) {
            // The bounded projection casts every output to VARCHAR. Borrow vector storage
            // until this chunk is destroyed; explicit length preserves embedded NUL bytes.
            auto* strings = static_cast<duckdb_string_t*>(duckdb_vector_get_data(vector));
            const auto* data = duckdb_string_t_data(&strings[row]);
            const std::string_view text(data, duckdb_string_t_length(strings[row]));
            auto end = text.size();
            auto characters = std::size_t{};
            for (std::size_t i = 0; i < text.size(); ++i) {
                if ((static_cast<unsigned char>(text[i]) & 0xc0U) != 0x80U &&
                    ++characters > character_limit) {
                    end = i;
                    break;
                }
            }
            cell.text = truncate_utf8(text.substr(0, end), 16384);
            cell.truncated = cell.text.size() < text.size();
            result.truncated = result.truncated || cell.truncated;
        }
        used += cell.text.size() + 32;
        if (used > budget) return false;
        output.cells.push_back(std::move(cell));
    }
    budget -= used;
    result.rows.push_back(std::move(output));
    return true;
}
} // namespace
DataResult run_query(Session& session, std::string_view sql, std::uint32_t max_rows) {
    Prepared original;
    session.prepare(sql, original);
    DataResult result;
    result.columns = columns(original);
    if (result.columns.empty()) throw DataError(DataErrorCode::invalid, "query has no result columns");
    // At most 64 MiB of rendered UTF-8 payload, even for a wide result of long strings.
    // A sentinel character makes adaptive shortening observable. DuckDB expression evaluation
    // and original source data remain subject to its separate buffer budget and deadline.
    const auto cells = (static_cast<std::size_t>(max_rows) + 1) * result.columns.size();
    const auto character_limit = std::min<std::size_t>(16384,
        (std::size_t{64} * 1024 * 1024 / cells / 4) - 1);
    Prepared bounded;
    session.prepare(rendering_sql(sql, result.columns.size(), max_rows, character_limit), bounded);
    Result raw;
    session.execute(bounded, raw);
    auto budget = std::size_t{4} * 1024 * 1024;
    for (const auto& column : result.columns) budget -= column.name.size() + column.type.size() + 64;
    auto finished = false;
    // Each nonempty chunk contains at least one row; the validated SQL emits at most
    // max_rows+1 rows. A bounded chunk count also guards an unexpected empty-chunk loop.
    for (std::uint32_t batch = 0; batch < max_rows + 2 && !finished; ++batch) {
        session.check();
        Handle<duckdb_data_chunk, duckdb_destroy_data_chunk> chunk;
        chunk.get() = duckdb_fetch_chunk(raw.get());
        session.check();
        if (chunk.get() == nullptr) break;
        for (idx_t row = 0; row < duckdb_data_chunk_get_size(chunk.get()); ++row) {
            if (result.rows.size() == max_rows) {
                result.has_more = true;
                finished = true;
                break;
            }
            if (!add_row(chunk.get(), row, result, budget, character_limit)) {
                result.truncated = result.has_more = finished = true;
                break;
            }
        }
        if (duckdb_result_error(&raw.get()) != nullptr) {
            throw DataError(DataErrorCode::invalid, "database result fetch failed");
        }
        if (batch == max_rows + 1 && !finished) {
            throw DataError(DataErrorCode::capacity, "database result chunk budget exceeded");
        }
        if (finished) {
            break;
        }
    }
    if (result.truncated) result.warnings.push_back("Result text or byte budget was truncated.");
    return result;
}
} // namespace atx::server::duckdb_detail
