#include <atx/server/database.hpp>

#include <algorithm>
#include <atomic>
#include <utility>

namespace atx::server {
DataError::DataError(DataErrorCode code, std::string message)
    : std::runtime_error(std::move(message)), code_(code) {}
DataErrorCode DataError::code() const noexcept { return code_; }
void DataContext::check() const {
    if (std::chrono::steady_clock::now() >= deadline) {
        throw DataError(DataErrorCode::deadline, "database operation deadline exceeded");
    }
    if (stop.stop_requested() || (cancelled && cancelled())) {
        throw DataError(DataErrorCode::cancelled, "database operation cancelled");
    }
}
namespace {
void validate_text(std::string_view value, std::size_t limit, std::string_view label,
    bool allow_empty = false) {
    if ((!allow_empty && value.empty()) || value.size() > limit ||
        value.find('\0') != std::string_view::npos) {
        throw DataError(DataErrorCode::invalid, "invalid " + std::string(label));
    }
}
struct Admission {
    std::atomic<std::uint32_t>& active;
    explicit Admission(std::atomic<std::uint32_t>& count) : active(count) {
        if (active.fetch_add(1) >= 2) {
            active.fetch_sub(1);
            throw DataError(DataErrorCode::capacity, "database operation capacity exhausted");
        }
    }
    ~Admission() { active.fetch_sub(1); }
    Admission(const Admission&) = delete;
    Admission& operator=(const Admission&) = delete;
};
} // namespace
struct DatabaseRegistry::Impl {
    const std::vector<DataSource> sources;
    mutable std::atomic<std::uint32_t> active{};
    explicit Impl(std::vector<DataSource> configured) : sources(std::move(configured)) {
        if (sources.size() > 32) throw DataError(DataErrorCode::capacity, "too many data sources");
        for (std::size_t i = 0; i < sources.size(); ++i) {
            const auto& source = sources[i];
            validate_text(source.id, 64, "source id");
            validate_text(source.name, 128, "source name");
            if (!source.provider || !std::all_of(source.id.begin(), source.id.end(), [](char ch) {
                return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                    (ch >= '0' && ch <= '9') || ch == '_' || ch == '-';
            })) throw DataError(DataErrorCode::invalid, "invalid data source registration");
            for (std::size_t j = 0; j < i; ++j) {
                if (sources[j].id == source.id) {
                    throw DataError(DataErrorCode::invalid, "duplicate data source id");
                }
            }
        }
    }
    const DatabaseProvider& provider(std::string_view id) const {
        validate_text(id, 64, "source id");
        const auto found = std::find_if(sources.begin(), sources.end(),
            [&](const auto& source) { return source.id == id; });
        if (found == sources.end()) throw DataError(DataErrorCode::missing, "unknown data source");
        return *found->provider;
    }
};
DatabaseRegistry::DatabaseRegistry(std::vector<DataSource> sources)
    : impl_(std::make_unique<Impl>(std::move(sources))) {}
DatabaseRegistry::~DatabaseRegistry() = default;
const std::vector<DataSource>& DatabaseRegistry::sources() const noexcept { return impl_->sources; }
DataCatalog DatabaseRegistry::catalog(std::string_view source, const DataContext& context) const {
    const auto& provider = impl_->provider(source);
    if (!provider.capabilities().browse) {
        throw DataError(DataErrorCode::invalid, "provider does not support catalog browsing");
    }
    context.check();
    Admission operation(impl_->active);
    return provider.catalog(context);
}
DataResult DatabaseRegistry::preview(std::string_view source, const DataTable& table,
    std::uint32_t limit, std::uint32_t offset, const DataContext& context) const {
    const auto& provider = impl_->provider(source);
    const auto limits = provider.capabilities();
    if (!limits.browse) throw DataError(DataErrorCode::invalid, "provider does not support previews");
    if (limit == 0 || limit > limits.max_rows || offset > limits.max_offset) {
        throw DataError(DataErrorCode::invalid, "invalid database preview pagination");
    }
    validate_text(table.catalog, 1024, "catalog", true);
    validate_text(table.schema, 1024, "schema", true);
    validate_text(table.name, 1024, "table");
    context.check();
    Admission operation(impl_->active);
    return provider.preview(table, limit, offset, context);
}
DataResult DatabaseRegistry::query(std::string_view source, std::string_view sql,
    std::uint32_t max_rows, const DataContext& context) const {
    const auto& provider = impl_->provider(source);
    const auto limits = provider.capabilities();
    if (!limits.query) throw DataError(DataErrorCode::invalid, "provider does not support queries");
    validate_text(sql, limits.max_query_bytes, "SQL query");
    if (max_rows == 0 || max_rows > limits.max_rows) {
        throw DataError(DataErrorCode::invalid, "invalid database query row limit");
    }
    context.check();
    Admission operation(impl_->active);
    return provider.query(sql, max_rows, context);
}
} // namespace atx::server
