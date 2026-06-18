#include <filesystem>
#include <string>

#include <gtest/gtest.h>

#include "atx/core/error.hpp"
#include "config.hpp"
#include "stages.hpp"

#include "orats_fixture.hpp"

namespace {
namespace fs = std::filesystem;

// Helper: build the canonical tiny fixture zip and return its path.
// Must be called from a void-returning context (write_orats_zip uses ASSERT_TRUE).
// Since test bodies are void, calling this directly from TEST() is correct.
// Returns the path on success; if write_orats_zip fails ASSERT_TRUE aborts the test.
std::string make_orats_zip() {
    const std::string path = atx_impl_test::make_orats_zip_path();
    const std::string body = atx_impl_test::make_orats_zip_body();
    atx_impl_test::write_orats_zip(body, path);
    return path;
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// Test 1: happy path — tiny zip loads correctly, kvs and output files present
// ---------------------------------------------------------------------------
TEST(AtxImplLoad, LoadsTinyZip) {
    const std::string zip = make_orats_zip();
    const fs::path out = fs::temp_directory_path() / "atx_impl_load_tiny_out";
    fs::remove_all(out);

    atx::impl::RunConfig cfg;
    cfg.zip      = zip;
    cfg.out      = out.string();
    cfg.min_date = "2020-01-01";

    auto r = atx::impl::run_load(cfg);
    ASSERT_TRUE(r.has_value()) << r.error().message();

    // Check kvs by scanning the vector.
    auto find_kv = [&](const std::string& key) -> std::string {
        for (const auto& [k, v] : r->kvs) {
            if (k == key) return v;
        }
        return "";
    };

    EXPECT_EQ(find_kv("rows_kept"),    "3");
    EXPECT_EQ(find_kv("dates_written"), "2");
    EXPECT_EQ(find_kv("distinct"),     "2");

    // Side-car files must exist.
    EXPECT_TRUE(fs::exists(out / "_symbology.parquet"));
    EXPECT_TRUE(fs::exists(out / "_manifest.json"));
    EXPECT_TRUE(fs::exists(out / "2020-01-02.seg"));

    // Clean up.
    fs::remove_all(out);
}

// ---------------------------------------------------------------------------
// Test 2: digest is stable across two independent runs with the same inputs
// ---------------------------------------------------------------------------
TEST(AtxImplLoad, DigestStableAcrossRuns) {
    const std::string zip = make_orats_zip();

    auto run_once = [&](const char* tag) {
        const fs::path out =
            fs::temp_directory_path() / (std::string("atx_impl_load_det_") + tag);
        fs::remove_all(out);
        atx::impl::RunConfig cfg;
        cfg.zip      = zip;
        cfg.out      = out.string();
        cfg.min_date = "2020-01-01";
        auto r = atx::impl::run_load(cfg);
        EXPECT_TRUE(r.has_value()) << r.error().message();
        atx::u64 d = r.has_value() ? r->digest : 0u;
        fs::remove_all(out);
        return d;
    };

    const atx::u64 d1 = run_once("a");
    const atx::u64 d2 = run_once("b");

    EXPECT_NE(d1, 0u)  << "digest must be non-zero";
    EXPECT_EQ(d1, d2)  << "digest must be identical across runs";
}

// ---------------------------------------------------------------------------
// Test 3: malformed min_date => Err(InvalidArgument)
// ---------------------------------------------------------------------------
TEST(AtxImplLoad, MalformedMinDateFails) {
    const std::string zip = make_orats_zip();
    const fs::path out = fs::temp_directory_path() / "atx_impl_load_baddate_out";

    atx::impl::RunConfig cfg;
    cfg.zip      = zip;
    cfg.out      = out.string();
    cfg.min_date = "not-a-date";

    auto r = atx::impl::run_load(cfg);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code(), atx::core::ErrorCode::InvalidArgument);
}

// ---------------------------------------------------------------------------
// Test 4: missing --zip => Err(InvalidArgument)
// ---------------------------------------------------------------------------
TEST(AtxImplLoad, MissingArgsFails) {
    atx::impl::RunConfig cfg;
    // cfg.zip intentionally empty
    cfg.out      = "some/out";
    cfg.min_date = "2020-01-01";

    auto r = atx::impl::run_load(cfg);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code(), atx::core::ErrorCode::InvalidArgument);
}
