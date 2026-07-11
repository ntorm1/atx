#include <gtest/gtest.h>

#include <atomic>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "atx/vol/calib.hpp"
#include "atx/vol/detail/archive_util.hpp" // crc32c (test-side CRC repair)
#include "atx/vol/surface_db.hpp"

// ATXVDB v1 manifest suite: on-disk record layout pinning, writer/parser
// round-trip (every SymbolFitConfig field preserved bit-for-bit), duplicate /
// malformed-input rejection, and corruption detection (magic, header CRC,
// payload CRC, truncation, out-of-range enum wire values). Pure in-memory
// (no file IO — that's Task 3).

namespace atx::vol {
namespace {

// A config where EVERY field differs from its default, so a round-trip that
// drops or transposes any field fails the equality sweep below.
SymbolFitConfig make_full_config() {
  SymbolFitConfig c;
  c.enabled = false;
  c.preset = FitPreset::Hft;
  c.pin_curve = true;
  c.curve.kind = VolCurveKind::ConvexDense;
  c.curve.convex.lambda = 7.5e-4;
  c.curve.convex.bound_slope_below = true;
  c.curve.convex.node_cap = 56;
  c.curve.convex.max_iter = 123;
  c.curve.convex.loss = CalibLossKind::Interval;
  auto& p = c.curve.parametric;
  p.max_outer_iter = 5; p.max_inner_iter = 13;
  p.tol_param = 2e-9; p.tol_residual = 3e-10;
  p.huber_k = 1.75;
  p.min_vega_weight = 2e-6; p.max_spread_vol = 0.07; p.max_weight = 500.0;
  p.max_obs_per_slice = 96; p.max_otm_shortcut_premium_spread_frac = 0.25;
  p.prior_strength = 0.5;
  p.essvi_rho_mode = EssviRhoMode::Shared;
  p.optimization_level = OptimizationLevel::Risk;
  p.essvi_fallback_rmse_threshold = 0.02; p.n_butterfly_grid = 128;
  p.max_iter_quick_mark = 9; p.max_iter_trading = 36; p.max_iter_risk = 101;
  p.max_iter_reference = 251; p.max_iter_cold_fast = 11;
  p.wing_floor_alpha = 0.05;
  p.lee_bound_project = false;
  p.morozov_stop = true; p.morozov_tau = 1.3;
  p.validate_no_arb = false;
  p.residual_disable = false;
  p.residual_basis_kind = ResidualBasisKind::C2Bspline;
  p.residual_n_basis_terms = 8; p.residual_ridge_factor = 2e-3;
  p.loss_kind = CalibLossKind::Interval;
  p.anchor_kind = CalibAnchorKind::Ask;
  p.essvi_asymmetric_rho = true;
  p.min_obs_per_slice = 6; p.max_post_fit_sigma = 3.0;
  p.max_spread_to_mid_pct = 0.4;
  c.al_override = true;
  c.al = AlOpts{9, 20, 6, 1e-9};
  c.band_k = 1.25;
  c.calendar_repair = CalendarRepair::Project;
  c.use_correction_cache = false;
  c.score_parity = false;
  c.enforce_calendar_floor = false;
  c.use_deam_cache_for_fit = true;
  return c;
}

void expect_config_eq(const SymbolFitConfig& a, const SymbolFitConfig& b) {
  EXPECT_EQ(a.enabled, b.enabled);
  EXPECT_EQ(a.preset, b.preset);
  EXPECT_EQ(a.pin_curve, b.pin_curve);
  EXPECT_EQ(a.curve.kind, b.curve.kind);
  EXPECT_EQ(a.curve.convex.lambda, b.curve.convex.lambda);
  EXPECT_EQ(a.curve.convex.bound_slope_below, b.curve.convex.bound_slope_below);
  EXPECT_EQ(a.curve.convex.node_cap, b.curve.convex.node_cap);
  EXPECT_EQ(a.curve.convex.max_iter, b.curve.convex.max_iter);
  EXPECT_EQ(a.curve.convex.loss, b.curve.convex.loss);
  const auto& x = a.curve.parametric; const auto& y = b.curve.parametric;
  EXPECT_EQ(x.max_outer_iter, y.max_outer_iter);
  EXPECT_EQ(x.max_inner_iter, y.max_inner_iter);
  EXPECT_EQ(x.tol_param, y.tol_param);
  EXPECT_EQ(x.tol_residual, y.tol_residual);
  EXPECT_EQ(x.huber_k, y.huber_k);
  EXPECT_EQ(x.min_vega_weight, y.min_vega_weight);
  EXPECT_EQ(x.max_spread_vol, y.max_spread_vol);
  EXPECT_EQ(x.max_weight, y.max_weight);
  EXPECT_EQ(x.max_obs_per_slice, y.max_obs_per_slice);
  EXPECT_EQ(x.max_otm_shortcut_premium_spread_frac, y.max_otm_shortcut_premium_spread_frac);
  EXPECT_EQ(x.prior_strength, y.prior_strength);
  EXPECT_EQ(x.essvi_rho_mode, y.essvi_rho_mode);
  EXPECT_EQ(x.optimization_level, y.optimization_level);
  EXPECT_EQ(x.essvi_fallback_rmse_threshold, y.essvi_fallback_rmse_threshold);
  EXPECT_EQ(x.n_butterfly_grid, y.n_butterfly_grid);
  EXPECT_EQ(x.max_iter_quick_mark, y.max_iter_quick_mark);
  EXPECT_EQ(x.max_iter_trading, y.max_iter_trading);
  EXPECT_EQ(x.max_iter_risk, y.max_iter_risk);
  EXPECT_EQ(x.max_iter_reference, y.max_iter_reference);
  EXPECT_EQ(x.max_iter_cold_fast, y.max_iter_cold_fast);
  EXPECT_EQ(x.wing_floor_alpha, y.wing_floor_alpha);
  EXPECT_EQ(x.lee_bound_project, y.lee_bound_project);
  EXPECT_EQ(x.morozov_stop, y.morozov_stop);
  EXPECT_EQ(x.morozov_tau, y.morozov_tau);
  EXPECT_EQ(x.validate_no_arb, y.validate_no_arb);
  EXPECT_EQ(x.residual_disable, y.residual_disable);
  EXPECT_EQ(x.residual_basis_kind, y.residual_basis_kind);
  EXPECT_EQ(x.residual_n_basis_terms, y.residual_n_basis_terms);
  EXPECT_EQ(x.residual_ridge_factor, y.residual_ridge_factor);
  EXPECT_EQ(x.loss_kind, y.loss_kind);
  EXPECT_EQ(x.anchor_kind, y.anchor_kind);
  EXPECT_EQ(x.essvi_asymmetric_rho, y.essvi_asymmetric_rho);
  EXPECT_EQ(x.min_obs_per_slice, y.min_obs_per_slice);
  EXPECT_EQ(x.max_post_fit_sigma, y.max_post_fit_sigma);
  EXPECT_EQ(x.max_spread_to_mid_pct, y.max_spread_to_mid_pct);
  EXPECT_EQ(a.al_override, b.al_override);
  EXPECT_EQ(a.al.n_collocation, b.al.n_collocation);
  EXPECT_EQ(a.al.n_quadrature, b.al.n_quadrature);
  EXPECT_EQ(a.al.max_newton_iter, b.al.max_newton_iter);
  EXPECT_EQ(a.al.tol, b.al.tol);
  EXPECT_EQ(a.band_k, b.band_k);
  EXPECT_EQ(a.calendar_repair, b.calendar_repair);
  EXPECT_EQ(a.use_correction_cache, b.use_correction_cache);
  EXPECT_EQ(a.score_parity, b.score_parity);
  EXPECT_EQ(a.enforce_calendar_floor, b.enforce_calendar_floor);
  EXPECT_EQ(a.use_deam_cache_for_fit, b.use_deam_cache_for_fit);
}

// Fresh per-test temp dir under the system temp root, self-cleaning at start
// so a prior crashed run doesn't leak stale manifest/partition files into
// this run. Each SurfaceDb.* test also removes it again at the end.
std::filesystem::path test_root(std::string_view name) {
  auto p = std::filesystem::temp_directory_path() / ("atx_surface_db_" + std::string(name));
  std::filesystem::remove_all(p);
  return p;
}

TEST(SurfaceDbManifest, RoundTrip_FullConfig_EveryFieldPreserved) {
  const auto cfg = make_full_config();
  const std::vector<DbSymbolEntry> syms{{"aapl", cfg}, {"SPY", SymbolFitConfig{}}};
  const std::vector<DbPartitionInfo> parts{
      {"2026-07-10", 123, 456789, 1720569600000000000LL}};
  auto bytes = write_db_manifest(syms, parts, {.generation = 7});
  ASSERT_TRUE(bytes.has_value());
  auto m = DbManifest::open(std::move(*bytes));
  ASSERT_TRUE(m.has_value());
  EXPECT_EQ(m->generation(), 7u);
  ASSERT_EQ(m->symbols().size(), 2u);
  ASSERT_EQ(m->partitions().size(), 1u);
  // canonical sort: AAPL < SPY
  auto got = m->find_symbol("AaPl");   // case-insensitive
  ASSERT_TRUE(got.has_value());
  expect_config_eq(*got, cfg);
  auto dflt = m->find_symbol("spy");
  ASSERT_TRUE(dflt.has_value());
  expect_config_eq(*dflt, SymbolFitConfig{});
  const auto* p = m->find_partition("2026-07-10");
  ASSERT_NE(p, nullptr);
  EXPECT_EQ(p->surface_count, 123u);
  EXPECT_EQ(p->file_size, 456789u);
  EXPECT_EQ(m->find_partition("2026-07-11"), nullptr);
  EXPECT_EQ(m->find_symbol("MSFT").error().code(), ErrorCode::NotFound);
}

TEST(SurfaceDbManifest, RoundTrip_Empty) {
  auto bytes = write_db_manifest({}, {});
  ASSERT_TRUE(bytes.has_value());
  auto m = DbManifest::open(std::move(*bytes));
  ASSERT_TRUE(m.has_value());
  EXPECT_EQ(m->symbols().size(), 0u);
  EXPECT_EQ(m->partitions().size(), 0u);
  EXPECT_EQ(m->header().symbol_count, 0u);
  EXPECT_EQ(m->header().partition_count, 0u);
}

TEST(SurfaceDbManifest, Write_RejectsDuplicateAndInvalid) {
  const std::vector<DbSymbolEntry> dup{{"AAPL", {}}, {"aapl", {}}};
  EXPECT_EQ(write_db_manifest(dup, {}).error().code(), ErrorCode::AlreadyExists);
  const std::vector<DbSymbolEntry> empty_sym{{"", {}}};
  EXPECT_EQ(write_db_manifest(empty_sym, {}).error().code(), ErrorCode::InvalidArgument);
  const std::vector<DbPartitionInfo> bad_key{{"bad/key", 0, 0, 0}};
  EXPECT_EQ(write_db_manifest({}, bad_key).error().code(), ErrorCode::InvalidArgument);
  const std::vector<DbPartitionInfo> dotdot{{"..", 0, 0, 0}};
  EXPECT_EQ(write_db_manifest({}, dotdot).error().code(), ErrorCode::InvalidArgument);
}

TEST(SurfaceDbManifest, Open_RejectsCorruption) {
  auto bytes = write_db_manifest({{DbSymbolEntry{"AAPL", {}}}}, {});
  ASSERT_TRUE(bytes.has_value());
  {
    auto bad = *bytes; bad[0] ^= std::byte{0xFF};  // magic
    EXPECT_EQ(DbManifest::open(std::move(bad)).error().code(), ErrorCode::ParseError);
  }
  {
    auto bad = *bytes; bad[100] ^= std::byte{0x01};  // header reserved => header CRC
    EXPECT_EQ(DbManifest::open(std::move(bad)).error().code(), ErrorCode::ParseError);
  }
  {
    auto bad = *bytes; bad[200] ^= std::byte{0x01};  // symbol record => payload CRC
    EXPECT_EQ(DbManifest::open(std::move(bad)).error().code(), ErrorCode::ParseError);
  }
  {
    auto bad = *bytes; bad.resize(bad.size() - 1);   // truncation
    EXPECT_EQ(DbManifest::open(std::move(bad)).error().code(), ErrorCode::ParseError);
  }
}

// Re-stamp both CRCs after a deliberate payload mutation so DbManifest::open
// gets PAST the checksum gates — the record-level (enum wire-range) validation
// must then be what rejects. Mirrors the writer's discipline: payload CRC over
// [symbols_offset, end), then header CRC over the header with its own field
// zeroed, computed last (so it covers the fresh payload_crc32c).
void restamp_crcs(std::vector<std::byte>& bytes) {
  DbManifestHeader h{};
  std::memcpy(&h, bytes.data(), sizeof h);
  const auto symbols_offset = static_cast<std::size_t>(h.symbols_offset);
  const std::uint32_t payload =
      detail::crc32c(bytes.data() + symbols_offset, bytes.size() - symbols_offset);
  std::memcpy(bytes.data() + offsetof(DbManifestHeader, payload_crc32c), &payload,
              sizeof payload);
  const std::uint32_t zero = 0;
  std::memcpy(bytes.data() + offsetof(DbManifestHeader, header_crc32c), &zero, sizeof zero);
  const std::uint32_t hcrc = detail::crc32c(bytes.data(), sizeof(DbManifestHeader));
  std::memcpy(bytes.data() + offsetof(DbManifestHeader, header_crc32c), &hcrc, sizeof hcrc);
}

TEST(SurfaceDbManifest, Open_RejectsOutOfRangeEnum) {
  auto bytes = write_db_manifest({{DbSymbolEntry{"AAPL", {}}}}, {});
  ASSERT_TRUE(bytes.has_value());

  // Sanity: a restamp with NO mutation must still open — proves the helper
  // reproduces the writer's CRCs, so the rejections below are the enum check.
  {
    auto same = *bytes;
    restamp_crcs(same);
    EXPECT_TRUE(DbManifest::open(std::move(same)).has_value());
  }

  DbManifestHeader h{};
  std::memcpy(&h, bytes->data(), sizeof h);
  // DbSymbolRecord layout: symbol[32], symbol_len (u16 @32), flags (u16 @34),
  // then the uint8 enum run — preset @ +36, curve_kind @ +37.
  const auto preset_off = static_cast<std::size_t>(h.symbols_offset) + 36;
  for (const std::size_t off : {preset_off, preset_off + 1}) {
    auto bad = *bytes;
    bad[off] = std::byte{0xFF};  // outside every enum's wire range
    restamp_crcs(bad);
    EXPECT_EQ(DbManifest::open(std::move(bad)).error().code(), ErrorCode::ParseError)
        << "enum byte at offset " << off;
  }
}

// ── SurfaceDb: create/open, atomic manifest persistence, symbol CRUD,
// refresh() ─────────────────────────────────────────────────────────────

TEST(SurfaceDb, CreateOpenUpsertReopen_ConfigPersists) {
  const auto root = test_root("create_open");     // helper: fresh temp dir
  auto db = SurfaceDb::create(root.string());
  ASSERT_TRUE(db.has_value());
  EXPECT_EQ(db->generation(), 1u);
  EXPECT_TRUE(db->symbols().empty());

  const auto cfg = make_full_config();
  ASSERT_TRUE(db->upsert_symbol("aapl", cfg).has_value());
  EXPECT_EQ(db->generation(), 2u);
  ASSERT_TRUE(db->upsert_symbol("SPY", SymbolFitConfig{}).has_value());
  EXPECT_EQ(db->generation(), 3u);

  auto db2 = SurfaceDb::open(root.string());      // fresh process simulation
  ASSERT_TRUE(db2.has_value());
  EXPECT_EQ(db2->generation(), 3u);
  EXPECT_EQ(db2->symbols(), (std::vector<std::string>{"AAPL", "SPY"}));
  auto got = db2->symbol_config("AAPL");
  ASSERT_TRUE(got.has_value());
  expect_config_eq(*got, cfg);

  ASSERT_TRUE(db2->remove_symbol("aapl").has_value());
  EXPECT_EQ(db2->symbol_config("AAPL").error().code(), ErrorCode::NotFound);
  EXPECT_EQ(db2->remove_symbol("AAPL").error().code(), ErrorCode::NotFound);
  std::filesystem::remove_all(root);
}

TEST(SurfaceDb, Create_RejectsExisting_Open_RejectsMissing) {
  const auto root = test_root("create_guard");
  ASSERT_TRUE(SurfaceDb::create(root.string()).has_value());
  EXPECT_EQ(SurfaceDb::create(root.string()).error().code(), ErrorCode::AlreadyExists);
  const auto missing = test_root("no_such_db");
  EXPECT_EQ(SurfaceDb::open(missing.string()).error().code(), ErrorCode::NotFound);
  std::filesystem::remove_all(root);
}

TEST(SurfaceDb, Refresh_SeesExternalWriterUpdate) {
  const auto root = test_root("refresh");
  auto writer = SurfaceDb::create(root.string());
  ASSERT_TRUE(writer.has_value());
  auto reader = SurfaceDb::open(root.string());
  ASSERT_TRUE(reader.has_value());
  EXPECT_EQ(reader->generation(), 1u);

  ASSERT_TRUE(writer->upsert_symbol("QQQ", SymbolFitConfig{}).has_value());
  // Reader still on its old snapshot until refresh:
  EXPECT_EQ(reader->generation(), 1u);
  ASSERT_TRUE(reader->refresh().has_value());
  EXPECT_EQ(reader->generation(), 2u);
  EXPECT_TRUE(reader->symbol_config("QQQ").has_value());
  // Idempotent when current:
  ASSERT_TRUE(reader->refresh().has_value());
  EXPECT_EQ(reader->generation(), 2u);
  std::filesystem::remove_all(root);
}

TEST(SurfaceDb, ConcurrentReaders_DuringUpserts_AreSafe) {
  const auto root = test_root("concurrent");
  auto db = SurfaceDb::create(root.string());
  ASSERT_TRUE(db.has_value());
  ASSERT_TRUE(db->upsert_symbol("SPY", SymbolFitConfig{}).has_value());
  std::atomic<bool> stop{false};
  std::vector<std::thread> readers;
  for (int t = 0; t < 4; ++t) {
    readers.emplace_back([&] {
      while (!stop.load(std::memory_order_relaxed)) {
        auto snap = db->manifest();
        auto cfg = db->symbol_config("SPY");
        EXPECT_TRUE(cfg.has_value());
        (void)snap;
      }
    });
  }
  for (int i = 0; i < 50; ++i) {
    SymbolFitConfig c; c.band_k = 1.0 + 0.01 * i;
    ASSERT_TRUE(db->upsert_symbol("SPY", c).has_value());
  }
  stop.store(true);
  for (auto& th : readers) th.join();
  auto final_cfg = db->symbol_config("SPY");
  ASSERT_TRUE(final_cfg.has_value());
  EXPECT_DOUBLE_EQ(final_cfg->band_k, 1.0 + 0.01 * 49);
  std::filesystem::remove_all(root);
}

}  // namespace
}  // namespace atx::vol
