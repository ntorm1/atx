// P9 lane COV (brief COV items 4, 5; K-P9-12): the `risk` verb's container wiring (CovRecipe:
// flags absent = every byte as before, the container equals the per-session files, the manifest
// names its root, the atx-cov-v1 recipe recorded), the RiskStore container read path (CovStore:
// the same RiskSlice bit for bit, C1's vol-target / risk-target replays unchanged, refusals) and
// the cov-diff tool (CovDiff). Synthetic roles only.

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include "../src/strategy_nav_replay.hpp"
#include "../src/strategy_nav_v7.hpp"
#include "../src/strategy_risk_target.hpp"
#include "../src/strategy_spo.hpp"
#include "strategy_cov_fixture.hpp"
#include "strategy_spo_fixture.hpp"

namespace {
using namespace atx_cov_test;
namespace st = atx::impl::strategy;
namespace sp = atx::impl::strategy::spo;
namespace fx = atx::impl::strategy::spo::fixture;
namespace rt = atx::impl::strategy::risk_target;
namespace v7 = atx::impl::strategy::v7;

const CovPanel& verb_panel() {
  static const CovPanel p(300, 200, 23);
  return p;
}
// One pinned role + fields per test directory.
struct VerbCase {
  Directory dir;
  Pinned pinned;
  VerbCase() : pinned(write_role_and_fields(dir.path / "in", verb_panel())) {}
  fs::path run(const char* name, std::initializer_list<const char*> extra) {
    const auto out = dir.path / name;
    std::string error;
    const int code = run_risk_verb(pinned, out, error, extra);
    if (code != 0) throw std::runtime_error(std::string(name) + ": " + error);
    return out;
  }
};
std::string sha_of(const fs::path& file) { return co::sha256_file(file.string()).value(); }

// ---- CovRecipe -------------------------------------------------------------------------------
TEST(CovRecipe, LegacyRecipeContainerEqualsDirectoryStore) {
  VerbCase c;
  const auto out = c.run("out", {"--emit-exposures", "all", "--emit-container"});
  const auto r = rk::compare_container_to_legacy(out.string());
  ASSERT_TRUE(r) << r.error().to_string();
  EXPECT_TRUE(r->equal) << r->what << " at " << r->session;
  EXPECT_EQ(r->sessions, verb_panel().d);
  EXPECT_EQ(r->compared.size(), 4U);
}

TEST(CovRecipe, FlagsAbsentOutputsUnchanged) {
  VerbCase c;
  const auto plain = c.run("plain", {"--emit-exposures", "all"});
  const auto spelled = c.run("spelled", {"--emit-exposures", "all", "--recipe", "atx-risk-v1.1",
                                         "--bias-families", "v1"});
  const auto packed = c.run("packed", {"--emit-exposures", "all", "--emit-container"});
  usize files = 0;
  for (const auto& entry : fs::directory_iterator(plain)) {
    const auto name = entry.path().filename();
    ++files;
    const auto bytes = file_bytes(entry.path());
    EXPECT_TRUE(bytes == file_bytes(spelled / name)) << name;
    if (name != "manifest.json") EXPECT_TRUE(bytes == file_bytes(packed / name)) << name;
  }
  EXPECT_GT(files, 8U);
  EXPECT_FALSE(fs::exists(plain / "model.atxcov"));
  EXPECT_TRUE(fs::exists(packed / "model.atxcov"));
  const Json a = read_json(plain / "manifest.json");
  Json b = read_json(packed / "manifest.json");
  EXPECT_FALSE(a.contains("container"));
  EXPECT_FALSE(a.contains("eigen_adjustment"));
  EXPECT_FALSE(a.at("recipe").contains("eigen_adjustment"));
  EXPECT_EQ(a.at("model"), rk::risk_model_id);
  ASSERT_TRUE(b.contains("container"));
  ASSERT_TRUE(b.at("files").contains("model.atxcov"));
  b.erase("container");
  b.at("files").erase("model.atxcov");
  EXPECT_TRUE(a == b) << Json::diff(a, b).dump();
}

TEST(CovRecipe, ManifestNamesTheContainerRoot) {
  VerbCase c;
  const auto out = c.run("out", {"--emit-container"});
  const Json m = read_json(out / "manifest.json");
  const auto& block = m.at("container");
  EXPECT_EQ(block.at("file"), "model.atxcov");
  EXPECT_EQ(block.at("format"), "atx.cov-container/1.0");
  EXPECT_EQ(block.at("bytes").get<u64>(), fs::file_size(out / "model.atxcov"));
  EXPECT_EQ(block.at("file_sha256").get<std::string>(), sha_of(out / "model.atxcov"));
  EXPECT_EQ(m.at("files").at("model.atxcov").at("sha256"), block.at("file_sha256"));
  const auto root = er::cov_parse_hex(block.at("root_sha256").get<std::string>());
  ASSERT_TRUE(root.has_value());
  const auto file = er::CovModelFile::open((out / "model.atxcov").string(), *root,
                                           er::VerifyMode::Full, 1ULL << 32);
  ASSERT_TRUE(file) << file.error().to_string();
  EXPECT_EQ(er::cov_hex(file->root_sha256()), block.at("root_sha256").get<std::string>());
  EXPECT_EQ(er::cov_hex(file->role_sha256()), c.pinned.role_sha);
  ASSERT_EQ(file->date_count(), verb_panel().d);
  for (usize t = 0; t < verb_panel().d; ++t) {
    EXPECT_EQ(file->index()[t].as_of_session_ns, verb_panel().sessions[t]);
    EXPECT_EQ(file->index()[t].data_cutoff_ns, verb_panel().sessions[t]);
    EXPECT_LE(file->index()[t].history_first_ns, file->index()[t].data_cutoff_ns);
  }
  EXPECT_EQ(file->index()[1].history_first_ns, verb_panel().sessions[1]); // the first returns
  // The header's recipe hash is the manifest recipe's canonical text.
  const std::string recipe = m.at("recipe").dump();
  const auto digest = co::sha256_hex(recipe);
  ASSERT_TRUE(digest);
  EXPECT_EQ(er::cov_hex(file->recipe_sha256()), *digest);
  const Json meta = Json::parse(file->meta_json());
  EXPECT_EQ(meta.at("model"), rk::risk_model_id);
  EXPECT_EQ(meta.at("recipe"), m.at("recipe"));
  EXPECT_EQ(meta.at("diag_names").size(), er::cov::diag_count);
  // A different root pin is refused.
  auto wrong = *root;
  wrong[0] ^= 1U;
  EXPECT_FALSE(er::CovModelFile::open((out / "model.atxcov").string(), wrong, er::VerifyMode::Full,
                                      1ULL << 32));
}

TEST(CovRecipe, CovV1RecipeRecorded) {
  VerbCase c;
  const auto out = c.run("out", {"--recipe", "atx-cov-v1", "--emit-container"});
  const Json m = read_json(out / "manifest.json");
  EXPECT_EQ(m.at("model"), "atx-cov-v1");
  EXPECT_EQ(m.at("recipe").at("model"), "atx-cov-v1");
  const Json expected{{"method", "menchero-wang-orr simulated (USE4 B7)"}, {"amplification", 1.0},
                      {"simulations", 64}, {"seed", 7},
                      {"simulated_length", "kish effective count of the HL-504 weights"},
                      {"seed_rule", "per session"}};
  EXPECT_EQ(m.at("recipe").at("eigen_adjustment"), expected);
  const auto& sessions = m.at("eigen_adjustment").at("sessions");
  usize total = 0;
  for (const auto& item : sessions.items()) total += item.value().get<usize>();
  EXPECT_EQ(total, verb_panel().d);
  EXPECT_GT(sessions.at("adjusted").get<usize>(), 100U);
  EXPECT_EQ(m.at("eigen_adjustment").at("max_working_bytes_source"), "kernel default");
  EXPECT_EQ(m.at("eigen_adjustment").at("max_working_bytes").get<u64>(), 268'435'456U);
  const auto root = er::cov_parse_hex(m.at("container").at("root_sha256").get<std::string>());
  const auto file = er::CovModelFile::open((out / "model.atxcov").string(), root,
                                           er::VerifyMode::Full, 1ULL << 32);
  ASSERT_TRUE(file) << file.error().to_string();
  EXPECT_EQ(Json::parse(file->meta_json()).at("model"), "atx-cov-v1");
  usize flagged = 0;
  for (const auto& e : file->index()) flagged += (e.flags & er::cov::date_eigen_adjusted) ? 1U : 0U;
  EXPECT_EQ(flagged, sessions.at("adjusted").get<usize>());
  // --max-bytes reaches the kernel under atx-cov-v1 (ruling COV-2).
  const auto bounded = c.run("bounded", {"--recipe", "atx-cov-v1", "--max-bytes", "1500000000"});
  const Json mb = read_json(bounded / "manifest.json");
  EXPECT_EQ(mb.at("eigen_adjustment").at("max_working_bytes").get<u64>(), 1'500'000'000U);
  EXPECT_EQ(mb.at("eigen_adjustment").at("max_working_bytes_source"), "--max-bytes");
}

// ---- CovStore --------------------------------------------------------------------------------
TEST(CovStore, RiskSliceFromContainerEqualsDirectory) {
  VerbCase c;
  const auto legacy_dir = c.run("legacy", {"--emit-exposures", "all"});
  const auto packed_dir = c.run("packed", {"--emit-exposures", "all", "--emit-container"});
  const auto legacy = sp::RiskStore::open(legacy_dir.string(), sha_of(legacy_dir / "manifest.json"),
                                          c.pinned.role_sha);
  const auto packed = sp::RiskStore::open(packed_dir.string(), sha_of(packed_dir / "manifest.json"),
                                          c.pinned.role_sha);
  ASSERT_TRUE(legacy) << legacy.error().to_string();
  ASSERT_TRUE(packed) << packed.error().to_string();
  EXPECT_FALSE(legacy->container());
  EXPECT_TRUE(packed->container());
  ASSERT_EQ(legacy->dates(), packed->dates());
  EXPECT_TRUE(std::equal(legacy->sessions().begin(), legacy->sessions().end(),
                         packed->sessions().begin(), packed->sessions().end()));
  usize forecast = 0;
  for (usize d = 0; d < legacy->dates(); ++d) {
    sp::RiskSlice a, b;
    const auto ra = legacy->read(d, a);
    const auto rb = packed->read(d, b);
    ASSERT_EQ(static_cast<bool>(ra), static_cast<bool>(rb)) << d;
    if (!ra) {
      EXPECT_EQ(ra.error().code(), rb.error().code()) << d;
      continue;
    }
    ++forecast;
    EXPECT_EQ(a.date, b.date);
    EXPECT_EQ(a.session, b.session);
    EXPECT_EQ(a.slot, b.slot) << d;
    EXPECT_TRUE(same_bits(std::span<const f64>(a.styles), std::span<const f64>(b.styles))) << d;
    EXPECT_TRUE(same_bits(std::span<const f64>(a.specific), std::span<const f64>(b.specific))) << d;
    EXPECT_TRUE(same_bits(std::span<const f64>(a.covariance), std::span<const f64>(b.covariance))) << d;
    EXPECT_EQ(a.nan_covariance_entries, b.nan_covariance_entries) << d;
  }
  EXPECT_GT(forecast, 20U);
}

// C1's replay fixture (strategy_spo_fixture.hpp Role, write_risk_model) on a legacy store and on
// the same store carrying a container: every leverage record and NAV day bit for bit, both laws.
TEST(CovStore, VolAndRiskTargetRecordsUnchanged) {
  const fx::Directory dir;
  const fx::Role role(70, 12, 53);
  const std::string pin(64, 'a');
  const std::vector<u8> forecast(role.d, u8{1});
  fs::create_directories(dir.path / "legacy");
  const auto legacy_sha =
      fx::write_risk_model(dir.path / "legacy", role.sessions, role.n, forecast, pin, 3);
  fs::copy(dir.path / "legacy", dir.path / "packed", fs::copy_options::recursive);
  const auto packed_sha = add_container(dir.path / "packed");
  auto legacy = sp::RiskStore::open((dir.path / "legacy").string(), legacy_sha, pin);
  auto packed = sp::RiskStore::open((dir.path / "packed").string(), packed_sha, pin);
  ASSERT_TRUE(legacy) << legacy.error().to_string();
  ASSERT_TRUE(packed) << packed.error().to_string();
  ASSERT_TRUE(packed->container());
  const auto shared_legacy = std::make_shared<const sp::RiskStore>(std::move(*legacy));
  const auto shared_packed = std::make_shared<const sp::RiskStore>(std::move(*packed));
  for (const auto law : {rt::Law::risk_target_v1, rt::Law::vol_target_v1}) {
    struct Replay {
      std::string records;
      std::vector<std::vector<u64>> days;
    };
    const auto replay = [&](const std::shared_ptr<const sp::RiskStore>& store) {
      v7::NavV7Options o;
      o.risk_target.on = true;
      o.risk_target.law = law;
      o.risk_target.params.sigma_star = 0.05;
      o.spo_risk = store;
      const v7::ScopedNavExtension extension(o);
      const auto result = st::replay_nav(role.nav(), fx::nav_config());
      if (!result) throw std::runtime_error(result.error().to_string());
      Replay r;
      const auto* scaler = extension.risk_target_scaler();
      if (!scaler) throw std::runtime_error("no scaler");
      r.records = rt::records_csv(scaler->records(), law);
      for (const auto& day : result->days)
        r.days.push_back({std::bit_cast<u64>(day.pretrade_nav), std::bit_cast<u64>(day.posttrade_nav),
                          std::bit_cast<u64>(day.net_return), std::bit_cast<u64>(day.traded_dollars),
                          std::bit_cast<u64>(day.trade_cost_dollars),
                          std::bit_cast<u64>(day.planned_gross), std::bit_cast<u64>(day.planned_net),
                          std::bit_cast<u64>(day.long_dollars), std::bit_cast<u64>(day.short_dollars),
                          static_cast<u64>(day.fills)});
      return r;
    };
    const Replay a = replay(shared_legacy), b = replay(shared_packed);
    EXPECT_EQ(a.records, b.records) << static_cast<int>(law);
    EXPECT_EQ(a.days, b.days) << static_cast<int>(law);
    EXPECT_GT(a.days.size(), 60U);
    EXPECT_NE(a.records.find('\n'), std::string::npos);
  }
}

TEST(CovStore, RefusesRoleGeometryOrRootMismatch) {
  const fx::Directory dir;
  const fx::Role role(40, 8, 5);
  const std::string pin(64, 'b');
  const std::vector<u8> forecast(role.d, u8{1});
  fs::create_directories(dir.path / "store");
  (void)fx::write_risk_model(dir.path / "store", role.sessions, role.n, forecast, pin, 3);
  const auto sha = add_container(dir.path / "store");
  const auto store_dir = (dir.path / "store").string();
  ASSERT_TRUE(sp::RiskStore::open(store_dir, sha, pin));
  // Another role.
  EXPECT_FALSE(sp::RiskStore::open(store_dir, sha, std::string(64, 'c')));
  // The manifest names another root.
  Json m = read_json(dir.path / "store" / "manifest.json");
  const Json original = m;
  std::string root = m.at("container").at("root_sha256").get<std::string>();
  root[0] = root[0] == '0' ? '1' : '0';
  m["container"]["root_sha256"] = root;
  const auto bad_root = write_json(dir.path / "store" / "manifest.json", m);
  const auto refused_root = sp::RiskStore::open(store_dir, bad_root, pin);
  ASSERT_FALSE(refused_root);
  EXPECT_EQ(refused_root.error().code(), co::ErrorCode::InvalidArgument);
  // A container whose own role pin differs from the manifest's role.
  m = original;
  m["role"]["manifest_sha256"] = std::string(64, 'd');
  const auto other_role = write_json(dir.path / "store" / "manifest.json", m);
  EXPECT_FALSE(sp::RiskStore::open(store_dir, other_role, std::string(64, 'd')));
  // Another geometry (a container of 4 groups / 3 dense columns) under the same manifest.
  {
    er::CovContainerSpec spec;
    spec.groups = 4; spec.dense = 3;
    spec.role_sha256 = er::cov_parse_hex(pin).value();
    spec.recipe_json = spec.inputs_json = spec.meta_json = "{}";
    for (u32 k = 0; k < spec.factors(); ++k) spec.factor_names.push_back("f" + std::to_string(k));
    auto w = er::CovContainerWriter::create((dir.path / "store" / "small.atxcov").string(), spec);
    ASSERT_TRUE(w);
    const std::vector<f64> cov(64, 0.0);
    for (usize t = 0; t < role.d; ++t) {
      er::CovBlockInput b;
      b.as_of_session_ns = b.data_cutoff_ns = b.history_first_ns = role.sessions[t];
      b.role_date_index = static_cast<u32>(t);
      b.flags = er::cov::date_forecast;
      b.factor_cov = cov;
      ASSERT_TRUE(w->append(b));
    }
    const auto small_root = w->finalize();
    ASSERT_TRUE(small_root);
    m = original;
    const auto bytes = fs::file_size(dir.path / "store" / "small.atxcov");
    m["files"]["small.atxcov"] = Json{{"sha256", sha_of(dir.path / "store" / "small.atxcov")}, {"bytes", bytes}};
    m["container"] = Json{{"file", "small.atxcov"}, {"format", "atx.cov-container/1.0"}, {"bytes", bytes},
                          {"file_sha256", m["files"]["small.atxcov"]["sha256"]},
                          {"root_sha256", er::cov_hex(*small_root)}};
    const auto small_sha = write_json(dir.path / "store" / "manifest.json", m);
    const auto refused = sp::RiskStore::open(store_dir, small_sha, pin);
    ASSERT_FALSE(refused);
    EXPECT_NE(refused.error().to_string().find("geometry"), std::string::npos)
        << refused.error().to_string();
  }
}

TEST(CovStore, LegacyManifestTakesTheLegacyPath) {
  const fx::Directory dir;
  const fx::Role role(40, 8, 7);
  const std::string pin(64, 'e');
  const std::vector<u8> forecast(role.d, u8{1});
  const auto sha = fx::write_risk_model(dir.path, role.sessions, role.n, forecast, pin, 3);
  const auto store = sp::RiskStore::open(dir.path.string(), sha, pin);
  ASSERT_TRUE(store) << store.error().to_string();
  EXPECT_FALSE(store->container());
  sp::RiskSlice slice;
  ASSERT_TRUE(store->read(5, slice));
  EXPECT_EQ(slice.slot.size(), role.n);
  // The legacy path still hashes its payloads: a changed covariance file is refused.
  {
    std::fstream f(dir.path / "factor_covariance.f64", std::ios::binary | std::ios::in | std::ios::out);
    f.seekp(8);
    const char flip = 0x5A;
    f.write(&flip, 1);
  }
  EXPECT_FALSE(sp::RiskStore::open(dir.path.string(), sha, pin));
}

// ---- CovDiff ---------------------------------------------------------------------------------
TEST(CovDiff, ThroughSessionReportsTheFirstDifferingBlock) {
  constexpr usize cut = 270;
  const Directory dir;
  const auto base = write_role_and_fields(dir.path / "base", verb_panel());
  const auto perturbed = write_role_and_fields(dir.path / "perturbed", verb_panel().perturb_after(cut, 9));
  std::string error;
  ASSERT_EQ(run_risk_verb(base, dir.path / "a", error, {"--emit-container"}), 0) << error;
  ASSERT_EQ(run_risk_verb(perturbed, dir.path / "b", error, {"--emit-container"}), 0) << error;
  const auto a = (dir.path / "a" / "model.atxcov").string(), b = (dir.path / "b" / "model.atxcov").string();
  const std::string through = std::to_string(verb_panel().sessions[cut]);
  std::string out;
  EXPECT_EQ(run_tool(rk::dispatch_cov_diff, {"cov-diff", a, b, "--through", through}, out), 0) << out;
  EXPECT_EQ(run_tool(rk::dispatch_cov_diff, {"cov-diff", a, a}, out), 0) << out;
  ASSERT_EQ(run_tool(rk::dispatch_cov_diff, {"cov-diff", a, b}, out), 1) << out;
  const std::string next = std::to_string(verb_panel().sessions[cut + 1]);
  EXPECT_NE(out.find("block " + std::to_string(cut + 1) + " session " + next), std::string::npos) << out;
  // cov-info opens both and lists every block.
  EXPECT_EQ(run_tool(rk::dispatch_cov_info, {"cov-info", a, "--verify", "lazy"}, out), 0) << out;
  EXPECT_NE(out.find("sessions 300"), std::string::npos) << out;
  EXPECT_EQ(run_tool(rk::dispatch_cov_diff, {"cov-diff", a}, out), 2) << out;
}

TEST(CovDiff, LegacyModeComparesBitForBit) {
  VerbCase c;
  const auto out_dir = c.run("out", {"--emit-exposures", "all", "--emit-container"});
  std::string out;
  ASSERT_EQ(run_tool(rk::dispatch_cov_diff, {"cov-diff", "--legacy", out_dir.string()}, out), 0) << out;
  EXPECT_NE(out.find("legacy equal"), std::string::npos) << out;
  // One forecast specific variance changed in its last bit: the first difference is named.
  const usize names = verb_panel().n, last = verb_panel().d - 1;
  {
    std::fstream f(out_dir / "specific_variance.f64", std::ios::binary | std::ios::in | std::ios::out);
    std::vector<f64> row(names);
    f.seekg(static_cast<std::streamoff>(last * names * sizeof(f64)));
    f.read(reinterpret_cast<char*>(row.data()), static_cast<std::streamsize>(names * sizeof(f64)));
    const auto it = std::find_if(row.begin(), row.end(), [](f64 v) { return std::isfinite(v); });
    ASSERT_NE(it, row.end());
    *it = std::bit_cast<f64>(std::bit_cast<u64>(*it) ^ 1ULL);
    f.seekp(static_cast<std::streamoff>(last * names * sizeof(f64)));
    // SAFETY: char writes the object representation of the f64 row.
    f.write(reinterpret_cast<const char*>(row.data()), static_cast<std::streamsize>(names * sizeof(f64)));
  }
  ASSERT_EQ(run_tool(rk::dispatch_cov_diff, {"cov-diff", "--legacy", out_dir.string()}, out), 1) << out;
  EXPECT_NE(out.find("specific_variance.f64 at session " + std::to_string(verb_panel().sessions[last])),
            std::string::npos) << out;
}
} // namespace
