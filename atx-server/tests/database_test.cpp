#include <atx/server/database.hpp>

#include <duckdb.h>
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <future>
#include <mutex>
#include <string>
#include <system_error>
#include <utility>

namespace atx::server {
namespace {
class DatabaseFixture : public ::testing::Test {
protected:
    std::filesystem::path directory, database;
    void SetUp() override {
        static std::atomic<std::uint64_t> sequence{};
        directory = std::filesystem::temp_directory_path() / ("atx-database-test-" +
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" +
            std::to_string(sequence.fetch_add(1)));
        std::filesystem::create_directories(directory);
        database = directory / "fixture.duckdb";
        duckdb_database handle{};
        ASSERT_EQ(duckdb_open(database.string().c_str(), &handle), DuckDBSuccess);
        struct Database { duckdb_database& value; ~Database() { duckdb_close(&value); } } db{handle};
        duckdb_connection connection{};
        ASSERT_EQ(duckdb_connect(handle, &connection), DuckDBSuccess);
        struct Connection {
            duckdb_connection& value;
            ~Connection() { duckdb_disconnect(&value); }
        } conn{connection};
        duckdb_result result{};
        struct Result { duckdb_result& value; ~Result() { duckdb_destroy_result(&value); } } r{result};
        ASSERT_EQ(duckdb_query(connection,
            "CREATE TABLE \"quoted\"\"table\" AS SELECT 9223372036854775807::BIGINT AS exact_int,"
            " 12345678901234567890.123456789::DECIMAL(38,9) AS exact_decimal, NULL::VARCHAR AS missing;"
            " CREATE TABLE numbers AS SELECT range AS n FROM range(1000);", &result), DuckDBSuccess)
            << duckdb_result_error(&result);
    }
    void TearDown() override {
        std::error_code ignored;
        std::filesystem::remove_all(directory, ignored);
    }
    [[nodiscard]] DatabaseRegistry registry() const {
        return DatabaseRegistry({{"fixture", "Fixture", make_duckdb_provider(database)}});
    }
};
} // namespace

TEST_F(DatabaseFixture, RegistryRejectsUnknownAndConflictingIds) {
    auto viewer = registry();
    EXPECT_THROW(static_cast<void>(viewer.catalog("unknown")), DataError);
    EXPECT_THROW(static_cast<void>(DatabaseRegistry({
        {"same", "One", make_duckdb_provider(database)},
        {"same", "Two", make_duckdb_provider(database)}})), DataError);
    EXPECT_THROW(static_cast<void>(DatabaseRegistry({
        {"../path", "Bad", make_duckdb_provider(database)}})), DataError);
    EXPECT_EQ(viewer.sources().front().provider->name(), "duckdb");
    EXPECT_EQ(viewer.sources().front().provider->capabilities().max_rows, 500U);
}
TEST_F(DatabaseFixture, CatalogAndQuotedPreviewPreservePrecisionAndNull) {
    auto viewer = registry();
    const auto catalog = viewer.catalog("fixture");
    ASSERT_FALSE(catalog.tables.empty());
    for (const auto& table : catalog.tables) {
        if (table.name != "quoted\"table") continue;
        const auto result = viewer.preview("fixture", table);
        ASSERT_EQ(result.rows.size(), 1U);
        ASSERT_EQ(result.columns.size(), 3U);
        EXPECT_EQ(result.columns[0].type, "BIGINT");
        EXPECT_EQ(result.columns[1].type, "DECIMAL(38,9)");
        EXPECT_EQ(result.rows[0].cells[0].text, "9223372036854775807");
        EXPECT_EQ(result.rows[0].cells[1].text, "12345678901234567890.123456789");
        EXPECT_TRUE(result.rows[0].cells[2].is_null);
        EXPECT_FALSE(result.rows[0].cells[0].is_null);
        return;
    }
    FAIL() << "Quoted fixture table missing from catalog";
}
TEST_F(DatabaseFixture, QueryBoundsRowsAndAcceptsTerminalCommentsAndQuotedSemicolons) {
    auto viewer = registry();
    const auto result = viewer.query("fixture", "SELECT n FROM numbers ORDER BY n; -- final", 7);
    ASSERT_EQ(result.rows.size(), 7U);
    EXPECT_EQ(result.rows.front().cells.front().text, "0");
    EXPECT_TRUE(result.has_more);
    EXPECT_EQ(viewer.query("fixture", "SELECT 'a;b' AS literal").rows[0].cells[0].text, "a;b");
    EXPECT_THROW(static_cast<void>(viewer.query("fixture", "SELECT 1", 501)), DataError);
    EXPECT_THROW(static_cast<void>(viewer.query("fixture", std::string(65537, ' '))), DataError);
}
TEST_F(DatabaseFixture, RejectsWritesMultipleStatementsAndConfigurationChanges) {
    auto viewer = registry();
    for (const auto* sql : {"DELETE FROM numbers", "CREATE TABLE forbidden(x INT)",
            "SELECT 1; SELECT 2", "EXPLAIN DELETE FROM numbers", "INSTALL httpfs",
            "LOAD httpfs", "ATTACH ':memory:' AS other", "SET enable_external_access=true",
            "COPY numbers TO 'escaped.csv'", "PRAGMA database_list"}) {
        EXPECT_THROW(static_cast<void>(viewer.query("fixture", sql)), DataError) << sql;
    }
    EXPECT_EQ(viewer.query("fixture", "SELECT count(*) FROM numbers").rows[0].cells[0].text, "1000");
    EXPECT_FALSE(std::filesystem::exists(directory / "escaped.csv"));
}
TEST_F(DatabaseFixture, ExternalFilesAndNetworkAreDisabled) {
    const auto secret = directory / "secret.txt";
    { std::ofstream output(secret); output << "private"; }
    auto viewer = registry();
    EXPECT_THROW(static_cast<void>(viewer.query("fixture",
        "SELECT * FROM read_text('" + secret.generic_string() + "')")), DataError);
    EXPECT_THROW(static_cast<void>(viewer.query("fixture",
        "SELECT * FROM read_csv('https://127.0.0.1:1/not-allowed')")), DataError);
}
TEST_F(DatabaseFixture, CellAndAggregateBudgetsSignalTruncation) {
    auto viewer = registry();
    const auto long_cell = viewer.query("fixture", "SELECT repeat('x',20000) AS big");
    ASSERT_EQ(long_cell.rows.size(), 1U);
    EXPECT_EQ(long_cell.rows[0].cells[0].text.size(), 16384U);
    EXPECT_TRUE(long_cell.rows[0].cells[0].truncated);
    EXPECT_TRUE(long_cell.truncated);
    const auto aggregate = viewer.query("fixture", "SELECT repeat('x',16384) FROM range(500)", 500);
    EXPECT_LT(aggregate.rows.size(), 500U);
    EXPECT_TRUE(aggregate.truncated);
    EXPECT_TRUE(aggregate.has_more);
}
TEST_F(DatabaseFixture, DeadlineAndCancellationInterruptExpensiveOperations) {
    auto viewer = registry();
    DataContext expired;
    expired.deadline = std::chrono::steady_clock::now() - std::chrono::milliseconds(1);
    EXPECT_THROW(static_cast<void>(viewer.query("fixture", "SELECT 1", 1, expired)), DataError);
    DataContext timed;
    timed.deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(25);
    const auto started = std::chrono::steady_clock::now();
    EXPECT_THROW(static_cast<void>(viewer.query("fixture",
        "SELECT sum(sin(i::DOUBLE)) FROM range(10000000000) t(i)", 1, timed)), DataError);
    EXPECT_LT(std::chrono::steady_clock::now() - started, std::chrono::seconds(2));
    std::stop_source source;
    source.request_stop();
    DataContext stopped;
    stopped.stop = source.get_token();
    EXPECT_THROW(static_cast<void>(viewer.query("fixture", "SELECT 1", 1, stopped)), DataError);
    EXPECT_EQ(viewer.query("fixture", "SELECT 1").rows.size(), 1U);
}
TEST_F(DatabaseFixture, MissingConfiguredDatabaseNeverCreatesAFile) {
    const auto missing = directory / "does-not-exist.duckdb";
    DatabaseRegistry viewer({{"missing", "Missing", make_duckdb_provider(missing)}});
    EXPECT_THROW(static_cast<void>(viewer.catalog("missing")), DataError);
    EXPECT_FALSE(std::filesystem::exists(missing));
}
TEST_F(DatabaseFixture, PinnedRuntimeAndSafetySettingsAreActive) {
    EXPECT_STREQ(duckdb_library_version(), "v1.5.1");
    auto viewer = registry();
    const auto settings = viewer.query("fixture",
        "SELECT current_setting('access_mode'),current_setting('enable_external_access'),"
        "current_setting('lock_configuration'),current_setting('autoload_known_extensions'),"
        "current_setting('autoinstall_known_extensions'),current_setting('allow_unsigned_extensions'),"
        "current_setting('temp_directory'),current_setting('threads')");
    ASSERT_EQ(settings.rows.size(), 1U);
    const auto& cells = settings.rows[0].cells;
    EXPECT_EQ(cells[0].text, "read_only");
    EXPECT_EQ(cells[1].text, "false");
    EXPECT_EQ(cells[2].text, "true");
    EXPECT_EQ(cells[3].text, "false");
    EXPECT_EQ(cells[4].text, "false");
    EXPECT_EQ(cells[5].text, "false");
    EXPECT_TRUE(cells[6].text.empty());
    EXPECT_EQ(cells[7].text, "1");
}
TEST_F(DatabaseFixture, UnicodeCellsRemainValidAndEmbeddedNullIsPreserved) {
    auto viewer = registry();
    const auto unicode = viewer.query("fixture", "SELECT repeat(chr(8364),6000)");
    const auto& cell = unicode.rows[0].cells[0];
    EXPECT_EQ(cell.text.size(), 16383U);
    EXPECT_TRUE(cell.truncated);
    EXPECT_EQ(cell.text.substr(cell.text.size() - 3), "\xE2\x82\xAC");
    const auto nul = viewer.query("fixture", "SELECT 'a'||chr(0)||'b'");
    EXPECT_EQ(nul.rows[0].cells[0].text, std::string("a\0b", 3));
    EXPECT_FALSE(nul.rows[0].cells[0].is_null);
}
TEST_F(DatabaseFixture, UnicodeConfiguredDatabasePathsWork) {
    const auto unicode_path = directory / std::filesystem::path(u8"fixture-é-数据.duckdb");
    std::filesystem::copy_file(database, unicode_path);
    DatabaseRegistry viewer({{"unicode", "Unicode", make_duckdb_provider(unicode_path)}});
    EXPECT_EQ(viewer.query("unicode", "SELECT count(*) FROM numbers").rows[0].cells[0].text, "1000");
}
TEST_F(DatabaseFixture, PreviewValidatesIdentifiersAndPagination) {
    auto viewer = registry();
    for (const auto& table : viewer.catalog("fixture").tables) {
        if (table.name != "numbers") continue;
        const auto page = viewer.preview("fixture", table, 3, 10);
        ASSERT_EQ(page.rows.size(), 3U);
        EXPECT_EQ(page.rows[0].cells[0].text, "10");
        EXPECT_TRUE(page.has_more);
        EXPECT_THROW(static_cast<void>(viewer.preview("fixture", table, 0)), DataError);
        EXPECT_THROW(static_cast<void>(viewer.preview("fixture", table, 1, 1000001)), DataError);
        auto injected = table;
        injected.name += "\"; DELETE FROM numbers;--";
        EXPECT_THROW(static_cast<void>(viewer.preview("fixture", injected)), DataError);
        EXPECT_EQ(viewer.query("fixture", "SELECT count(*) FROM numbers").rows[0].cells[0].text,
                  "1000");
        return;
    }
    FAIL() << "Numbers fixture missing";
}
TEST_F(DatabaseFixture, ExcessColumnsAndOversizedMetadataNamesAreRejected) {
    auto viewer = registry();
    std::string sql = "SELECT ";
    for (int i = 0; i < 257; ++i) {
        if (i != 0) sql += ',';
        sql += "1 AS c" + std::to_string(i);
    }
    EXPECT_THROW(static_cast<void>(viewer.query("fixture", sql)), DataError);
    EXPECT_THROW(static_cast<void>(viewer.query("fixture",
        "SELECT 1 AS \"" + std::string(1025, 'a') + "\"")), DataError);
}
TEST_F(DatabaseFixture, WideLongCellsUseObservableAdaptiveRenderingBudget) {
    auto viewer = registry();
    std::string sql = "SELECT ";
    for (int i = 0; i < 128; ++i) {
        if (i != 0) sql += ',';
        sql += "repeat('x',1000) AS c" + std::to_string(i);
    }
    sql += " FROM range(500)";
    const auto result = viewer.query("fixture", sql, 500);
    EXPECT_TRUE(result.truncated);
    ASSERT_FALSE(result.rows.empty());
    EXPECT_TRUE(result.rows[0].cells[0].truncated);
    EXPECT_LT(result.rows[0].cells[0].text.size(), 1000U);
    auto wire_bytes = std::size_t{};
    for (const auto& row : result.rows) {
        for (const auto& cell : row.cells) wire_bytes += cell.text.size() + 32;
    }
    EXPECT_LT(wire_bytes, std::size_t{4} * 1024 * 1024);
}
namespace {
struct ProviderState {
    std::mutex mutex;
    std::condition_variable wake;
    unsigned entered{};
    bool released{};
};
class BlockingProvider final : public DatabaseProvider {
public:
    explicit BlockingProvider(std::shared_ptr<ProviderState> state, bool enabled = true)
        : state_(std::move(state)), enabled_(enabled) {}
    std::string_view name() const noexcept override { return "test-provider"; }
    DataCapabilities capabilities() const noexcept override {
        DataCapabilities result;
        result.browse = result.query = enabled_;
        return result;
    }
    DataCatalog catalog(const DataContext&) const override {
        std::unique_lock lock(state_->mutex);
        ++state_->entered;
        state_->wake.notify_all();
        state_->wake.wait_for(lock, std::chrono::seconds(2), [&] { return state_->released; });
        return {};
    }
    DataResult preview(const DataTable&, std::uint32_t, std::uint32_t,
        const DataContext&) const override { return {}; }
    DataResult query(std::string_view, std::uint32_t, const DataContext&) const override { return {}; }
private:
    const std::shared_ptr<ProviderState> state_;
    const bool enabled_;
};
} // namespace
TEST(DatabaseRegistry, GlobalAdmissionBoundsDifferentSourcesAndReleasesAfterCompletion) {
    auto state = std::make_shared<ProviderState>();
    auto provider = std::make_shared<BlockingProvider>(state);
    DatabaseRegistry viewer({{"first", "First", provider}, {"second", "Second", provider}});
    std::future<DataCatalog> first, second;
    struct Release {
        ProviderState& state;
        ~Release() {
            { std::lock_guard lock(state.mutex); state.released = true; }
            state.wake.notify_all();
        }
    } release{*state}; // Released before futures join, including assertion failures.
    first = std::async(std::launch::async, [&] { return viewer.catalog("first"); });
    second = std::async(std::launch::async, [&] { return viewer.catalog("second"); });
    {
        std::unique_lock lock(state->mutex);
        ASSERT_TRUE(state->wake.wait_for(lock, std::chrono::seconds(1), [&] {
            return state->entered == 2;
        }));
    }
    try {
        static_cast<void>(viewer.catalog("second"));
        FAIL() << "Third active database operation admitted";
    } catch (const DataError& error) { EXPECT_EQ(error.code(), DataErrorCode::capacity); }
    { std::lock_guard lock(state->mutex); state->released = true; }
    state->wake.notify_all();
    static_cast<void>(first.get());
    static_cast<void>(second.get());
    EXPECT_NO_THROW(static_cast<void>(viewer.catalog("first")));
}
TEST(DatabaseRegistry, UnsupportedProviderCapabilitiesAreHonored) {
    auto state = std::make_shared<ProviderState>();
    DatabaseRegistry viewer({{"disabled", "Disabled",
        std::make_shared<BlockingProvider>(state, false)}});
    EXPECT_THROW(static_cast<void>(viewer.catalog("disabled")), DataError);
    EXPECT_THROW(static_cast<void>(viewer.query("disabled", "SELECT 1")), DataError);
    EXPECT_THROW(static_cast<void>(viewer.preview("disabled", {"db", "main", "table", ""})),
                 DataError);
    EXPECT_EQ(state->entered, 0U);
}
TEST(DatabaseRegistry, NamespaceLessProvidersCanPreviewUnqualifiedTables) {
    auto state = std::make_shared<ProviderState>();
    DatabaseRegistry viewer({{"simple", "Simple", std::make_shared<BlockingProvider>(state)}});
    EXPECT_NO_THROW(static_cast<void>(viewer.preview("simple", {"", "", "table", ""})));
}
} // namespace atx::server
