// atx::impl — discover-run provenance tests (p7 S6-4).
//
// Proves PipelineRunRow.config_json and .engine_git_sha are populated for a real
// gated discover run (no longer the empty strings of pre-S6) by running a tiny
// gated discover with a temp --run-db and reading the pipeline_run row back:
//   - config_json is non-empty,
//   - config_json round-trips the key gate/seed fields to their original values,
//   - engine_git_sha is non-empty and is either "unknown" (no-git fallback) or a
//     40-char lowercase hex SHA (optionally with a "-dirty" suffix).
//
// We assert against the persisted DB values rather than the ATX_ENGINE_GIT_SHA
// macro directly: the macro is a PRIVATE compile definition of atx-impl-core
// (baked by atx-impl/CMakeLists.txt), so the value reaches this test only through
// the run it produced — which is exactly the provenance path under test.

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "atx/core/types.hpp"

#include "atx/engine/alpha/panel.hpp"
#include "atx/engine/store/db.hpp" // store::StoreDb

#include "config.hpp"
#include "serialize_panel.hpp"
#include "stages.hpp"

namespace atxtest_provenance {

namespace fs = std::filesystem;
using atx::f64;
using atx::usize;
using atx::engine::alpha::Panel;
namespace store = atx::engine::store;

// Deterministic noisy-momentum panel (mirrors store_discover_test.cpp's fixture).
struct Lcg {
    std::uint64_t s;
    [[nodiscard]] f64 next() noexcept {
        s = s * 6364136223846793005ULL + 1442695040888963407ULL;
        const std::uint64_t hi = s >> 11U;
        const f64 u = static_cast<f64>(hi) / static_cast<f64>(1ULL << 53U);
        return 2.0 * u - 1.0;
    }
};

std::vector<f64> noisy_close(usize dates, usize insts, std::uint64_t seed) {
    std::vector<f64> drift(insts);
    for (usize j = 0; j < insts; ++j) {
        drift[j] = 0.006 - 0.0024 * static_cast<f64>(j);
    }
    std::vector<f64> close(dates * insts);
    std::vector<f64> px(insts, 100.0);
    Lcg rng{seed};
    for (usize t = 0; t < dates; ++t) {
        for (usize j = 0; j < insts; ++j) {
            px[j] *= (1.0 + drift[j] + 0.010 * rng.next());
            close[t * insts + j] = px[j];
        }
    }
    return close;
}

std::optional<Panel> make_panel(usize dates = 96, usize insts = 6) {
    const std::vector<f64> close = noisy_close(dates, insts, 0xBEEFCAFEULL);
    auto r = Panel::create(dates, insts, {"close"}, {close}, {});
    if (!r.has_value()) {
        ADD_FAILURE() << "panel fixture must build: " << r.error().to_string();
        return std::nullopt;
    }
    return std::move(r.value());
}

std::string write_panel_tmp(const Panel& panel, const std::string& stem) {
    const std::string path =
        (fs::temp_directory_path() / ("atx_impl_prov_" + stem + ".bin")).string();
    auto r = atx::impl::write_panel(panel, path);
    EXPECT_TRUE(r.has_value()) << "write_panel must succeed";
    return path;
}

atx::impl::RunConfig gated_cfg(const std::string& panel_path, const std::string& alpha_out) {
    atx::impl::RunConfig cfg;
    cfg.subcommand    = "discover";
    cfg.allow_unidentified_panels = true; // Legacy numeric fixture: explicit diagnostic mode.
    cfg.panel         = panel_path;
    cfg.alpha_out     = alpha_out;
    cfg.seed          = 4242ULL;
    cfg.population    = 16;
    cfg.generations   = 4;
    cfg.seed_exprs    = {"rank(close)", "ts_mean(close,10)", "delta(close,2)"};
    cfg.gated         = true;
    cfg.min_sharpe    = 0.0;
    cfg.min_fitness   = 0.0;
    cfg.max_turnover  = 10.0;
    cfg.max_pool_corr = 1.0;
    cfg.min_dsr       = 0.0;
    return cfg;
}

// Read a single TEXT column from the pipeline_run row.
std::string read_run_text(store::StoreDb& db, const std::string& column) {
    auto stmt_r = db.db().prepare_cached("SELECT " + column + " FROM pipeline_run LIMIT 1");
    EXPECT_TRUE(stmt_r.has_value());
    if (!stmt_r.has_value()) return "";
    auto* stmt = *stmt_r;
    auto step = stmt->step();
    EXPECT_TRUE(step.has_value());
    if (!step.has_value() || *step != atx::core::db::Statement::Step::Row) return "";
    return std::string{stmt->column_text(0)};
}

// Minimal value extractor for the flat compact JSON object build_config_json emits
// (no nesting, no arrays). Returns the raw token after "key": (a number literal, a
// "quoted" string with surrounding quotes stripped, or a bool literal), or empty
// if absent. Sufficient for asserting the round-trip of the keys we check.
std::string json_value_of(const std::string& json, const std::string& key) {
    const std::string needle = "\"" + key + "\":";
    const auto k = json.find(needle);
    if (k == std::string::npos) return "";
    usize p = k + needle.size();
    if (p >= json.size()) return "";
    if (json[p] == '"') {
        const auto end = json.find('"', p + 1);
        if (end == std::string::npos) return "";
        return json.substr(p + 1, end - (p + 1));
    }
    const auto end = json.find_first_of(",}", p);
    return json.substr(p, (end == std::string::npos ? json.size() : end) - p);
}

// Run a gated discover with a temp run-db and return the persisted provenance.
struct Provenance {
    std::string config_json;
    std::string engine_git_sha;
    std::string manifest;
};

Provenance run_and_read(const std::string& tag, bool capacity = false,
    atx::engine::eval::PboRule pbo_rule = atx::engine::eval::PboRule::CachedMomentsV2,
    atx::engine::eval::CpcvRule cpcv_rule = atx::engine::eval::CpcvRule::ObservationV1) {
    Provenance out;
    auto panel = make_panel();
    if (!panel.has_value()) return out;
    if (capacity) {
        const auto close_span = panel->field_all(panel->field_id("close").value());
        const std::vector<f64> close(close_span.begin(), close_span.end());
        panel = Panel::create(panel->dates(), panel->instruments(),
            {"close", "raw_close", "volume"},
            {close, close, std::vector<f64>(close.size(), 1e6)}, {}).value();
    }
    const std::string panel_path = write_panel_tmp(*panel, tag);
    const std::string alpha_out = (fs::temp_directory_path() / ("atx_prov_out_" + tag)).string();
    const std::string db_path =
        (fs::temp_directory_path() / ("atx_prov_" + tag + ".db")).string();

    std::error_code ec0;
    fs::remove(db_path, ec0);

    auto cfg = gated_cfg(panel_path, alpha_out);
    cfg.pbo_rule = pbo_rule;
    cfg.cpcv_rule = cpcv_rule;
    if (cpcv_rule == atx::engine::eval::CpcvRule::DateV2) {
        cfg.cpcv_embargo_dates = 2; cfg.cpcv_max_working_bytes = 1048576;
        cfg.population = 2; cfg.generations = 1;
    }
    if (capacity) {
        cfg.min_price = 1.25;
        cfg.min_adv_usd = 1e6;
        cfg.adv_window = 3;
    }
    cfg.run_db = db_path;
    auto r = atx::impl::run_discover(cfg);
    EXPECT_TRUE(r.has_value()) << (r ? "" : r.error().message());

    auto db_r = store::StoreDb::open(db_path);
    EXPECT_TRUE(db_r.has_value());
    if (db_r.has_value()) {
        store::StoreDb db = std::move(*db_r);
        out.config_json    = read_run_text(db, "config_json");
        out.engine_git_sha = read_run_text(db, "engine_git_sha");
    }

    if (cpcv_rule == atx::engine::eval::CpcvRule::DateV2) {
        std::ifstream input(fs::path(alpha_out) / "_manifest.txt");
        std::ostringstream text; text << input.rdbuf(); out.manifest = text.str();
    }
    std::error_code ec;
    fs::remove(panel_path, ec);
    fs::remove_all(alpha_out, ec);
    fs::remove(db_path, ec);
    return out;
}

TEST(AtxImplProvenance, ActiveCapacityRecipeIsPersisted) {
    const auto active = run_and_read("capacity_vwap_v2", true);
    EXPECT_EQ(json_value_of(active.config_json, "vwap_rule"), "raw-daily-close-v2");
    EXPECT_EQ(json_value_of(active.config_json, "min_price"), "1.25");
    EXPECT_EQ(json_value_of(active.config_json, "min_adv_usd"), "1000000");
    EXPECT_EQ(json_value_of(active.config_json, "adv_window"), "3");
    const auto off = run_and_read("capacity_disabled");
    for (const auto *key : {"vwap_rule", "min_price", "min_adv_usd", "adv_window"}) {
        EXPECT_TRUE(json_value_of(off.config_json, key).empty());
    }
}

TEST(AtxImplProvenance, PboRuleIsPersistedAndExplicitV1KeepsLegacySchema) {
    const auto cached = run_and_read("pbo_recipe_v2");
    EXPECT_EQ(json_value_of(cached.config_json, "v"), "3");
    EXPECT_EQ(json_value_of(cached.config_json, "pbo_rule"), "cached-moments-v2");
    const auto legacy = run_and_read("pbo_recipe_v1", false,
        atx::engine::eval::PboRule::LegacyGatherV1);
    EXPECT_EQ(json_value_of(legacy.config_json, "v"), "2"); // IC screen remains active
    EXPECT_TRUE(json_value_of(legacy.config_json, "pbo_rule").empty());
}

// ---------------------------------------------------------------------------
// Provenance_ConfigJsonNonEmpty
// ---------------------------------------------------------------------------
TEST(AtxImplProvenance, ConfigJsonNonEmpty) {
    const Provenance p = run_and_read("nonempty");
    EXPECT_FALSE(p.config_json.empty()) << "config_json must be populated";
    EXPECT_EQ(p.config_json.front(), '{') << "config_json must be a JSON object";
    EXPECT_EQ(p.config_json.back(), '}');
}

// ---------------------------------------------------------------------------
// Provenance_ConfigJsonRoundTrips — key gate/seed fields round-trip.
// ---------------------------------------------------------------------------
TEST(AtxImplProvenance, ConfigJsonRoundTrips) {
    const Provenance p = run_and_read("roundtrip");
    ASSERT_FALSE(p.config_json.empty());

    // Seed / search environment.
    EXPECT_EQ(json_value_of(p.config_json, "seed"), "4242");
    EXPECT_EQ(json_value_of(p.config_json, "population"), "16");
    EXPECT_EQ(json_value_of(p.config_json, "generations"), "4");
    EXPECT_EQ(json_value_of(p.config_json, "gated"), "true");
    // Gate floors (doubles serialized via setprecision(17); compare numerically).
    EXPECT_DOUBLE_EQ(std::stod(json_value_of(p.config_json, "min_sharpe")), 0.0);
    EXPECT_DOUBLE_EQ(std::stod(json_value_of(p.config_json, "min_fitness")), 0.0);
    EXPECT_DOUBLE_EQ(std::stod(json_value_of(p.config_json, "max_turnover")), 10.0);
    EXPECT_DOUBLE_EQ(std::stod(json_value_of(p.config_json, "max_pool_corr")), 1.0);
    EXPECT_DOUBLE_EQ(std::stod(json_value_of(p.config_json, "min_dsr")), 0.0);
    // Weight policy.
    EXPECT_EQ(json_value_of(p.config_json, "weight_transform"), "rank");
    // Non-finite sentinel default round-trips as a JSON string token.
    EXPECT_EQ(json_value_of(p.config_json, "min_split_sharpe"), "-inf");
    EXPECT_EQ(json_value_of(p.config_json, "max_turnover_target"), "inf");
}

// ---------------------------------------------------------------------------
// Provenance_ConfigJsonDeterministic — SAME config (incl. panel path) -> identical
// config_json. Both runs share one panel file so the only difference is the (not-
// in-config) run-db / alpha-out paths; the JSON must be byte-identical, proving it
// carries no wall-clock timestamp or other run-varying state.
// ---------------------------------------------------------------------------
TEST(AtxImplProvenance, ConfigJsonDeterministic) {
    auto panel = make_panel();
    ASSERT_TRUE(panel.has_value());
    const std::string panel_path = write_panel_tmp(*panel, "det_shared");

    auto run_once = [&](const char* tag) -> std::string {
        const std::string alpha_out =
            (fs::temp_directory_path() / (std::string("atx_prov_det_out_") + tag)).string();
        const std::string db_path =
            (fs::temp_directory_path() / (std::string("atx_prov_det_") + tag + ".db")).string();
        std::error_code ec0;
        fs::remove(db_path, ec0);
        auto cfg = gated_cfg(panel_path, alpha_out); // identical config, same panel path
        cfg.run_db = db_path;
        auto r = atx::impl::run_discover(cfg);
        EXPECT_TRUE(r.has_value()) << (r ? "" : r.error().message());
        std::string json;
        auto db_r = store::StoreDb::open(db_path);
        EXPECT_TRUE(db_r.has_value());
        if (db_r.has_value()) {
            store::StoreDb db = std::move(*db_r);
            json = read_run_text(db, "config_json");
        }
        fs::remove_all(alpha_out, ec0);
        fs::remove(db_path, ec0);
        return json;
    };

    const std::string a = run_once("a");
    const std::string b = run_once("b");
    ASSERT_FALSE(a.empty());
    EXPECT_EQ(a, b)
        << "config_json must be deterministic for the same config (no timestamps)";

    std::error_code ec;
    fs::remove(panel_path, ec);
}

// ---------------------------------------------------------------------------
// Provenance_EngineGitShaNonEmpty
// ---------------------------------------------------------------------------
TEST(AtxImplProvenance, EngineGitShaNonEmpty) {
    const Provenance p = run_and_read("sha_nonempty");
    EXPECT_FALSE(p.engine_git_sha.empty()) << "engine_git_sha must be baked at build time";
}

// ---------------------------------------------------------------------------
// Provenance_EngineGitShaFormat — "unknown" OR 40-hex (optionally "-dirty").
// ---------------------------------------------------------------------------
TEST(AtxImplProvenance, EngineGitShaFormat) {
    const Provenance p = run_and_read("sha_format");
    ASSERT_FALSE(p.engine_git_sha.empty());
    if (p.engine_git_sha == "unknown") {
        SUCCEED() << "no-git fallback is a valid provenance value";
        return;
    }
    // Strip an optional "-dirty" suffix, then require a 40-char lowercase hex SHA.
    std::string sha = p.engine_git_sha;
    const std::string dirty = "-dirty";
    if (sha.size() > dirty.size() && sha.compare(sha.size() - dirty.size(), dirty.size(), dirty) == 0) {
        sha = sha.substr(0, sha.size() - dirty.size());
    }
    ASSERT_EQ(sha.size(), 40u) << "git SHA core must be 40 hex chars, got: " << p.engine_git_sha;
    for (const char c : sha) {
        const bool hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        EXPECT_TRUE(hex) << "non-hex char in SHA: '" << c << "'";
    }
}

} // namespace atxtest_provenance

namespace atxtest_provenance {
TEST(AtxImplProvenance, DateCpcvPersistsActiveRecipeAndActualPathMetadata) {
    const auto date=run_and_read("date_cpcv_v2",false,
        atx::engine::eval::PboRule::LegacyGatherV1,atx::engine::eval::CpcvRule::DateV2);
    EXPECT_EQ(json_value_of(date.config_json,"v"),"4");
    EXPECT_EQ(json_value_of(date.config_json,"cpcv_rule"),"date-v2");
    EXPECT_EQ(json_value_of(date.config_json,"cpcv_embargo_dates"),"2");
    EXPECT_EQ(json_value_of(date.config_json,"cpcv_max_working_bytes"),"1048576");
    EXPECT_NE(date.manifest.find("cpcv_fold_count=15"),std::string::npos);
    EXPECT_NE(date.manifest.find("cpcv_path_count=5"),std::string::npos);
    EXPECT_NE(date.manifest.find("cpcv_path_4="),std::string::npos);
    const auto legacy=run_and_read("date_cpcv_legacy",false,
        atx::engine::eval::PboRule::LegacyGatherV1);
    EXPECT_TRUE(json_value_of(legacy.config_json,"cpcv_rule").empty());
}
}

// P9 Ruling M1d-RED-1b: the discover config JSON is written to the sidecar
// <alpha_out>/_config.json, not into _manifest.txt. With a non-legacy recipe (the
// defaults: pbo cached-moments-v2, IC screen on) the sidecar is written and holds
// exactly the config persisted in the run-DB, and the manifest carries no
// `config_json=` line. With every rule at its legacy value no sidecar is due, and a
// stale one left in the output directory by an earlier run is removed.
namespace atxtest_provenance {
namespace {
std::string read_binary(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream text; text << in.rdbuf(); return text.str();
}
} // namespace

TEST(AtxImplProvenance, ConfigSidecarHoldsTheRunConfigAndStaysOutOfTheManifest) {
    auto panel = make_panel();
    ASSERT_TRUE(panel.has_value());
    const std::string panel_path = write_panel_tmp(*panel, "config_sidecar");
    const fs::path active_out = fs::temp_directory_path() / "atx_prov_sidecar_active";
    const fs::path legacy_out = fs::temp_directory_path() / "atx_prov_sidecar_legacy";
    const std::string db_path = (fs::temp_directory_path() / "atx_prov_sidecar.db").string();
    std::error_code ec;
    fs::remove_all(active_out, ec); fs::remove_all(legacy_out, ec); fs::remove(db_path, ec);

    // Default recipe: the sidecar is due.
    auto active_cfg = gated_cfg(panel_path, active_out.string());
    active_cfg.run_db = db_path;
    auto active = atx::impl::run_discover(active_cfg);
    ASSERT_TRUE(active.has_value()) << active.error().message();
    std::string persisted;
    {
        auto db = store::StoreDb::open(db_path);
        ASSERT_TRUE(db.has_value());
        persisted = read_run_text(*db, "config_json");
    }
    ASSERT_FALSE(persisted.empty());
    ASSERT_TRUE(fs::exists(active_out / "_config.json"));
    const std::string sidecar = read_binary(active_out / "_config.json");
    EXPECT_EQ(sidecar, persisted + "\n") << "the sidecar holds the run's config_json, newline-terminated";
    EXPECT_EQ(json_value_of(sidecar, "seed"), "4242");
    EXPECT_EQ(json_value_of(sidecar, "pbo_rule"), "cached-moments-v2");
    const std::string active_manifest = read_binary(active_out / "_manifest.txt");
    ASSERT_FALSE(active_manifest.empty());
    EXPECT_EQ(active_manifest.find("config_json="), std::string::npos)
        << "the config JSON must not be written into _manifest.txt";

    // Every rule at its legacy value: no sidecar is due; a stale one is removed.
    fs::create_directories(legacy_out);
    { std::ofstream stale(legacy_out / "_config.json", std::ios::binary); stale << "{\"stale\":true}\n"; }
    auto legacy_cfg = gated_cfg(panel_path, legacy_out.string());
    legacy_cfg.pbo_rule = atx::engine::eval::PboRule::LegacyGatherV1;
    legacy_cfg.ic_screen.rule = atx::engine::factory::IcScreenRule::DisabledV1;
    legacy_cfg.cpcv_rule = atx::engine::eval::CpcvRule::ObservationV1;
    auto legacy = atx::impl::run_discover(legacy_cfg);
    ASSERT_TRUE(legacy.has_value()) << legacy.error().message();
    EXPECT_FALSE(fs::exists(legacy_out / "_config.json"))
        << "a stale sidecar must not survive a run that writes none";
    const std::string legacy_manifest = read_binary(legacy_out / "_manifest.txt");
    ASSERT_FALSE(legacy_manifest.empty());
    EXPECT_EQ(legacy_manifest.find("config_json="), std::string::npos);

    fs::remove_all(active_out, ec); fs::remove_all(legacy_out, ec);
    fs::remove(db_path, ec); fs::remove(panel_path, ec);
}
} // namespace atxtest_provenance
