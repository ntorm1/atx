#include <gtest/gtest.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <nlohmann/json.hpp>
#include "atx/core/sha256.hpp"
#include "atx/engine/combine/marginal_rank_ic.hpp"
#include "atx/engine/eval/trial_registry.hpp"
#include "atx/engine/factory/research_ic_fitness.hpp"
#include "strategy_mine.hpp"
#include "strategy_mine_ledger.hpp"
#include "strategy_mine_rule.hpp"
#include "strategy_research_role.hpp"

// platform v8 H-3: `atx-equity-strategy-mine` on a synthetic role (the sprint plan's fixture
// acceptance) and the mined-v1 arithmetic.
//
// The world: 16 names, one session per calendar day from 2018-12-14 to 2023-12-31 (1,844
// sessions; score_begin 383 is 2020-01-01, row 1,295 is 2022-07-01), every name a decision member
// from session 63. Drivers p1..p4, m1, m2, n1, neg, flip and the noise e are i.i.d. N(0, 1) over
// names and days, so no expression of a driver's past predicts anything. The one-day return is
// r(t) = .01 x(t - 2) + .005 e(t), x = p1 + p2 + p3 + m1 - neg + (p4 - flip before row 1,295;
// flip from it), so the h 21 label of decision t (close[t+22] / close[t+1] - 1) carries each
// loaded driver at t. Discover is [2020-01-01, 2022-07-01) (890 mature label rows), confirm
// [2022-07-01, 2024-01-01) (527).
// The pool (review MINE-9): the marginal term's regressor is the book rank(m1) (its centred
// tied rank) and the pool's one member is m2, independent of every field and of the returns, so
// only the marginal term can stop a copy of the book.
// The mined fields: p1, p2, p3 (planted), copy = m1 + .02 noise (the book again), n1 (noise),
// swap (p4 before the confirm begin, m1 from it: planted in discover, the book in confirm), neg
// (return loading -1 in both windows) and flip (-1 in discover, +1 in confirm).
// A numpy replica of this world (the same generator; Bartlett-21 t's; the IC runner's
// conservative t approximated by Bartlett) reads, on the corrected scale t / 1.55:
//   discover f2 / F  rank(p1) 5.1, rank(p2) 6.4, rank(p3) 6.0, rank(swap) 4.6, rank(neg) 5.6 and
//                    rank(flip) 6.8 (both sign -1), rank(copy) -1.2 (its f1 / F 7.3), rank(n1)
//                    0.3; a delta(f, w) template reads about .7 of rank(f) and correlates with
//                    it at about .66 (under the rho bound);
//   confirm t / F    rank(p1) 4.3, rank(p2) 5.0, rank(p3) 4.3, rank(neg) 6.1, rank(flip) -3.6,
//                    rank(swap) undefined (every confirm row spanned by the book), while its raw
//                    IC t / F is 4.8.
namespace {
using namespace atx;
using Json = nlohmann::json;
namespace fs = std::filesystem;
namespace st = atx::impl::strategy;
namespace cb = atx::engine::combine;
namespace ev = atx::engine::eval;
namespace ex = atx::engine::factory;
constexpr i64 day = 86'400'000'000'000LL;
constexpr i64 first_day = 17879; // 2018-12-14
constexpr usize D = 1844, N = 16, score_begin = 383;
// The fixture's confirm begin, 2022-07-01: swap and flip change their role from this row on.
constexpr usize kConfirmRow = 1295;
constexpr f64 missing = std::numeric_limits<f64>::quiet_NaN();
// The fixture campaigns' --budget: above their capacity (templates + 16 x 2 stage-2 candidates).
constexpr u64 kBudget = 128;

struct Directory {
  fs::path path;
  Directory() {
    static std::atomic<unsigned> sequence{};
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    for (unsigned a = 0; a < 32; ++a) {
      auto candidate = fs::temp_directory_path() / ("atx-mine-" + std::to_string(stamp) + "-" +
                                                    std::to_string(sequence.fetch_add(1)));
      if (fs::create_directory(candidate)) {
        path = std::move(candidate);
        break;
      }
    }
  }
  ~Directory() {
    if (!path.empty()) {
      std::error_code ec;
      fs::remove_all(path, ec);
    }
  }
  Directory(const Directory &) = delete;
  Directory &operator=(const Directory &) = delete;
};
template <class T>
bool payload(const fs::path &dir, Json &files, const std::string &name,
             const std::vector<T> &data) {
  std::ofstream f(dir / name, std::ios::binary);
  const auto bytes = std::as_bytes(std::span(data));
  f.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  f.close();
  if (!f) return false;
  auto sha = core::sha256_file((dir / name).string());
  if (!sha) return false;
  files[name] = {{"bytes", bytes.size()}, {"sha256", *sha}};
  return true;
}
bool json_file(const fs::path &path, const Json &j, std::string &sha) {
  const auto text = j.dump(2) + "\n";
  std::ofstream out(path, std::ios::binary);
  out << text;
  out.close();
  auto digest = core::sha256_hex(text);
  if (!out || !digest) return false;
  sha = *digest;
  return true;
}
Json read_json(const fs::path &path) {
  std::ifstream in(path);
  return Json::parse(in);
}
struct Rng {
  std::uint64_t s;
  f64 uni() {
    s = s * 6364136223846793005ULL + 1442695040888963407ULL;
    return (static_cast<f64>(s >> 11U) + 0.5) / 9007199254740992.0;
  }
  f64 gauss() {
    const f64 u1 = uni();
    const f64 u2 = uni();
    return std::sqrt(-2.0 * std::log(u1)) * std::cos(6.283185307179586 * u2);
  }
};
std::vector<f64> draws(Rng &g, usize n) {
  std::vector<f64> v(n);
  for (auto &x : v) x = g.gauss();
  return v;
}
std::vector<i64> daily_sessions(usize count) {
  std::vector<i64> out(count);
  for (usize t = 0; t < count; ++t) out[t] = (first_day + static_cast<i64>(t)) * day;
  return out;
}
std::vector<u64> instrument_ids() {
  std::vector<u64> out(N);
  for (usize i = 0; i < N; ++i) out[i] = 1001U + i;
  return out;
}

struct World {
  std::vector<f64> p1, p2, p3, p4, m1, m2, n1, neg, flip, copy, swap, close, book;
  World() {
    Rng g{0x6d696e65U};
    p1 = draws(g, D * N);
    p2 = draws(g, D * N);
    p3 = draws(g, D * N);
    p4 = draws(g, D * N);
    m1 = draws(g, D * N);
    m2 = draws(g, D * N);
    n1 = draws(g, D * N);
    neg = draws(g, D * N);
    flip = draws(g, D * N);
    const auto e = draws(g, D * N), jitter = draws(g, D * N);
    copy.resize(D * N);
    swap.resize(D * N);
    for (usize c = 0; c < D * N; ++c) {
      copy[c] = m1[c] + 0.02 * jitter[c];
      swap[c] = c / N < kConfirmRow ? p4[c] : m1[c];
    }
    close.assign(D * N, 100.0);
    for (usize t = 1; t < D; ++t)
      for (usize i = 0; i < N; ++i) {
        f64 driver = 0.0;
        if (t >= 2) {
          const usize s = (t - 2) * N + i; // the driving row t - 2
          driver = p1[s] + p2[s] + p3[s] + m1[s] - neg[s] +
                   (t - 2 < kConfirmRow ? p4[s] - flip[s] : flip[s]);
        }
        close[t * N + i] = close[(t - 1) * N + i] * (1.0 + 0.01 * driver + 0.005 * e[t * N + i]);
      }
    // The book's regressor: m1's centred tied rank over the decision members.
    book.assign(D * N, missing);
    std::vector<u8> member(N);
    std::vector<std::pair<f64, usize>> sorted;
    for (usize t = 0; t < D; ++t) {
      std::fill(member.begin(), member.end(), static_cast<u8>(t >= 63));
      const auto status = cb::centred_tied_ranks(std::span<const f64>(m1.data() + t * N, N),
                                                  member,
                                                  std::span<f64>(book.data() + t * N, N), sorted);
      EXPECT_TRUE(status.has_value());
    }
  }
};

// Writes an atx.recent-research-role/v1 role of `days` daily sessions from 2018-12-14, with the
// manifest block `universe` when it is not null.
bool write_role(const fs::path &dir, usize days, const std::vector<f64> &close, std::string &sha,
                Json &files, const Json &universe = Json()) {
  if (!fs::create_directory(dir)) return false;
  std::vector<u8> member(days * N, 0), present(days * N, 1);
  for (usize t = 63; t < days; ++t)
    std::fill_n(member.begin() + static_cast<std::ptrdiff_t>(t * N), N, u8{1});
  const std::vector<f64> volume(days * N, 1e6);
  const auto keys = daily_sessions(days);
  if (!payload(dir, files, "sessions.i64", keys) ||
      !payload(dir, files, "ids.u64", instrument_ids()) ||
      !payload(dir, files, "member.u8", member) || !payload(dir, files, "present.u8", present) ||
      !payload(dir, files, "close.f64", close) || !payload(dir, files, "raw_close.f64", close) ||
      !payload(dir, files, "volume.f64", volume))
    return false;
  const Json membership{{"rule", "research-prior63-usd-adv-topn-v1"}, {"top_n", N},
                        {"lookback_sessions", 63}, {"lag_sessions", 1},
                        {"min_raw_price_exclusive", 5}, {"min_adv_exclusive", 5000000},
                        {"ties", "securityID-ascending"},
                        {"missing", "complete-prior-calendar-window-required"},
                        {"common_stock_verified", false}};
  Json manifest{{"schema", "atx.recent-research-role/v1"}, {"status", "complete"},
                {"instrument_namespace", "spiderrock.securityID"}, {"dates", days},
                {"instruments", N}, {"score_begin", score_begin}, {"score_end", days},
                {"score_start_ns", keys[score_begin]}, {"score_end_ns", keys.back() + day},
                {"source_sha256", std::string(64, 'a')},
                {"membership_recipe", membership.dump()},
                {"clock_recipe", "modeled-session+22h-mark+23h-decision-v1"},
                {"close_basis", "f64(raw-f32-close)*f64-cumulReturnFactor"},
                {"volume_basis", "raw-share-volume"}, {"common_stock_verified", false},
                {"historical_vintage_verified", false},
                {"declared_output_bytes", days * N * 26 + days * 8 + N * 8}, {"files", files}};
  if (!universe.is_null()) manifest["universe"] = universe;
  return json_file(dir / "manifest.json", manifest, sha);
}

struct Fixture {
  Directory dir;
  std::string role_sha, fields_sha, pool_sha;
  bool ok{};
  Json pool_files = Json::object(); // the pool payloads: name -> {bytes, sha256}
  Fixture() {
    if (dir.path.empty()) return;
    const World world;
    Json role_files;
    ok = write_role(dir.path / "role", D, world.close, role_sha, role_files) &&
         write_fields(world, role_files) && write_pool(world);
  }
  bool write_fields(const World &world, const Json &role_files) {
    const auto fields = dir.path / "fields";
    if (!fs::create_directory(fields)) return false;
    Json rows = Json::array(), files = Json::object();
    const std::vector<std::pair<const char *, const std::vector<f64> *>> columns{
        {"p1", &world.p1},     {"p2", &world.p2},     {"p3", &world.p3},
        {"copy", &world.copy}, {"n1", &world.n1},     {"swap", &world.swap},
        {"neg", &world.neg},   {"flip", &world.flip}};
    for (const auto &[name, values] : columns) {
      const std::string file = std::string(name) + ".f64";
      if (!payload(fields, files, file, *values)) return false;
      rows.push_back({{"name", name}, {"file", file}, {"dtype", "<f8"}, {"layout", "date-major"},
                      {"shape", {D, N}}, {"sha256", files[file]["sha256"]},
                      {"point_in_time", true}, {"non_pit_aspects", Json::array()},
                      {"definition", "synthetic driver"}});
    }
    const Json role{{"manifest_sha256", role_sha},
                    {"sessions_sha256", role_files["sessions.i64"]["sha256"]},
                    {"ids_sha256", role_files["ids.u64"]["sha256"]}, {"dates", D},
                    {"instruments", N}};
    return json_file(fields / "manifest.json",
                     {{"schema", "atx.research-role-fields/v1"}, {"status", "complete"},
                      {"role", role}, {"fields", rows}, {"files", files}},
                     fields_sha);
  }
  bool write_pool(const World &world) {
    const auto pool = dir.path / "pool";
    if (!fs::create_directory(pool)) return false;
    if (!payload(pool, pool_files, "book.f64", world.book) ||
        !payload(pool, pool_files, "m2.f64", world.m2))
      return false;
    return write_pool_manifest("manifest.json", {"book"}, {"m2"}, pool_sha);
  }
  // An atx.mine-pool/v1 manifest `name` in the pool directory over the payloads written there.
  bool write_pool_manifest(const std::string &name, const std::vector<std::string> &regressors,
                           const std::vector<std::string> &members, std::string &sha) const {
    const auto rows = [this](const std::vector<std::string> &names) {
      Json out = Json::array();
      for (const std::string &row : names) {
        const std::string file = row + ".f64";
        out.push_back(Json{{"name", row}, {"file", file},
                           {"sha256", pool_files.at(file).at("sha256")},
                           {"bytes", pool_files.at(file).at("bytes")}});
      }
      return out;
    };
    return json_file(dir.path / "pool" / name,
                     {{"schema", "atx.mine-pool/v1"}, {"status", "complete"},
                      {"role_manifest_sha256", role_sha}, {"dates", D}, {"instruments", N},
                      {"regressors", rows(regressors)}, {"members", rows(members)}},
                     sha);
  }
  st::MineConfig config(const std::string &tag, u64 seed, usize workers) const {
    st::MineConfig cfg;
    cfg.role.manifest = (dir.path / "role" / "manifest.json").string();
    cfg.role.manifest_sha256 = role_sha;
    cfg.role.fields_directory = (dir.path / "fields").string();
    cfg.role.fields_sha256 = fields_sha;
    // The stage-2 campaigns mine the fields whose combinations cannot cancel (every planted
    // loading +1 in both windows); the rule pins (RulePinsOnTheTemplates) mine all eight.
    cfg.role.fields = {"p1", "p2", "p3", "copy", "n1"};
    cfg.pool_path = (dir.path / "pool" / "manifest.json").string();
    cfg.pool_sha256 = pool_sha;
    cfg.discover_begin = "2020-01-01";
    cfg.discover_end = "2022-07-01";
    cfg.confirm_begin = "2022-07-01";
    cfg.confirm_end = "2024-01-01";
    cfg.registry_path = (dir.path / ("registry-" + tag + ".atxtrg")).string();
    cfg.campaign_id = "fixture";
    cfg.output_directory = (dir.path / ("out-" + tag)).string();
    cfg.budget = kBudget;
    cfg.seed = seed;
    cfg.workers = workers;
    cfg.stage2_seeds = 8;
    cfg.stage2_population = 16;
    cfg.stage2_generations = 2;
    // The rung keeps half: on every second name the numpy replica of this world ranks rank(copy)
    // 4th of the 55 templates, rank(p1) 9th and rank(p3) 14th; the 28th |t| is under 2.
    cfg.race_strides = {2};
    cfg.race_keep = 0.5;
    cfg.min_names = 10;
    cfg.min_dates = 128;
    return cfg;
  }
};

std::string text_of(const fs::path &path) {
  std::ifstream in(path, std::ios::binary);
  std::ostringstream out;
  out << in.rdbuf();
  return out.str();
}

// trials.csv: eleven unquoted cells, then the quoted DSL.
struct TrialRow {
  std::string canon_hash, stage, status, reason, dsl;
  f64 f1{missing}, f2{missing};
};
std::vector<TrialRow> read_trials(const fs::path &path) {
  std::ifstream in(path);
  std::string line;
  std::getline(in, line);
  std::vector<TrialRow> out;
  while (std::getline(in, line)) {
    std::vector<std::string> cells;
    usize from = 0;
    for (int k = 0; k < 11; ++k) {
      const auto comma = line.find(',', from);
      cells.push_back(line.substr(from, comma - from));
      from = comma + 1U;
    }
    const auto number = [](const std::string &cell) {
      return cell.empty() ? missing : std::stod(cell);
    };
    TrialRow row;
    row.canon_hash = cells[0];
    row.stage = cells[1];
    row.status = cells[2];
    row.reason = cells[3];
    row.f1 = number(cells[6]);
    row.f2 = number(cells[9]);
    row.dsl = line.substr(from + 1U, line.size() - from - 2U);
    out.push_back(std::move(row));
  }
  return out;
}

// The SHA-256 of a file's first `bytes` bytes (empty when the file is shorter).
std::string prefix_sha256(const fs::path &path, u64 bytes) {
  const std::string text = text_of(path);
  if (text.size() < bytes) return {};
  const auto sha = core::sha256_hex(std::string_view(text).substr(0, static_cast<usize>(bytes)));
  return sha ? *sha : std::string{};
}

// Review MINE-1 / Ruling E-33: ledger_line.json is the line backtest_integrity.campaign_line
// writes for campaign.json -- by the verb's own check (mine_ledger_line_problem) and by an
// independent mirror here -- and its chain head is the SHA-256 of the registry log's first
// `bytes` bytes. The pre-fix verb's 16-hex head is a line that check refuses. Ruling E-33a:
// registry.count is the campaign's new records, registry.total the registry's size.
void expect_campaign_line(const st::MineConfig &cfg, const Json &campaign, const fs::path &out,
                          const std::string &context) {
  const std::string text = text_of(out / "ledger_line.json");
  EXPECT_EQ(st::mine_ledger_line_problem(text), "") << context;
  const Json &registry = campaign.at("registry");
  const auto head_hex = registry.at("head").get<std::string>();
  const auto bytes = registry.at("bytes").get<u64>();
  ASSERT_EQ(head_hex.size(), 64U) << context;
  EXPECT_EQ(head_hex, prefix_sha256(cfg.registry_path, bytes)) << context;
  std::string registry_path = cfg.registry_path;
  std::replace(registry_path.begin(), registry_path.end(), '\\', '/');
  // Review MINE-3: the recipe in campaign.json hashes to the identity and binds the confirm window.
  const auto recipe_sha = campaign.at("recipe_sha256").get<std::string>();
  EXPECT_EQ(core::sha256_hex(campaign.at("recipe").dump()).value_or(""), recipe_sha) << context;
  const Json &confirm = campaign.at("recipe").at("confirm");
  EXPECT_EQ(confirm.at("begin").get<std::string>(), cfg.confirm_begin) << context;
  EXPECT_EQ(confirm.at("end").get<std::string>(), cfg.confirm_end) << context;
  const auto ident =
      core::sha256_hex(Json::array({"mining-campaign", recipe_sha, head_hex}).dump());
  ASSERT_TRUE(ident.has_value());
  const Json expected{{"schema", "atx.trial-ledger/v1"},
                      {"kind", "mining-campaign"},
                      {"count", 0},
                      {"campaign", campaign.at("campaign_id")},
                      {"origin", "mined"},
                      {"rule", "mined-v1"},
                      {"budget", campaign.at("budget")},
                      {"recipe_sha256", recipe_sha},
                      {"confirm", {{"begin", cfg.confirm_begin}, {"end", cfg.confirm_end}}},
                      {"window_id", campaign.at("research_window").at("id")},
                      {"registry",
                       {{"path", registry_path},
                        {"chain_head", head_hex},
                        {"bytes", bytes},
                        {"count", registry.at("new_records")},
                        {"total", registry.at("n_raw")}}},
                      {"trial_id", ident->substr(0, 16)}};
  EXPECT_EQ(Json::parse(text), expected) << context;
  std::string old_form = text;
  old_form.replace(old_form.find(head_hex), head_hex.size(), head_hex.substr(0, 16));
  EXPECT_NE(st::mine_ledger_line_problem(old_form), "") << context;
  const auto head = ev::read_chain_head(out / "registry_head.txt");
  ASSERT_TRUE(head.has_value()) << context;
  EXPECT_EQ(head->records, registry.at("records").get<u64>()) << context;
  EXPECT_EQ(head->head, std::stoull(registry.at("chain").get<std::string>(), nullptr, 16))
      << context;
}

// ---- mined-v1 arithmetic ---------------------------------------------------------------------
TEST(StrategyMineRule, HurdleIsTheBonferroniValueOfThePlan) {
  EXPECT_NEAR(st::mined_hurdle(100), 3.4808, 1e-3);
  EXPECT_NEAR(st::mined_hurdle(1000), 4.0556, 1e-3);
  EXPECT_NEAR(st::mined_hurdle(10000), 4.5648, 1e-3);
  // The plan's rounded values: 3.5 at 100, 4.1 at 1,000, 4.6 at 10,000.
  EXPECT_EQ(std::round(st::mined_hurdle(100) * 10.0), 35.0);
  EXPECT_EQ(std::round(st::mined_hurdle(1000) * 10.0), 41.0);
  EXPECT_EQ(std::round(st::mined_hurdle(10000) * 10.0), 46.0);
  EXPECT_TRUE(std::isnan(st::mined_hurdle(0)));
}

// Review MINE-6: the hurdle is read on f2 / F, so with F 1.55 at a hurdle of 3 an f2 of 4.6 is
// out and one of 4.7 is in; a NaN factor (a budget the table does not cover) shortlists nothing.
TEST(StrategyMineRule, ShortlistIsByF2ThenHashAndCapped) {
  const std::vector<st::MinedRead> reads{{9, 5.0}, {3, missing}, {2, 7.0}, {1, 5.0},
                                         {4, 3.0}, {5, 4.6},     {6, 4.7}};
  EXPECT_EQ(st::mined_shortlist(reads, 3.0, 1.55, 8), (std::vector<usize>{2, 3, 0, 6}));
  EXPECT_EQ(st::mined_shortlist(reads, 3.0, 1.55, 2), (std::vector<usize>{2, 3}));
  EXPECT_TRUE(st::mined_shortlist(reads, missing, 1.55, 8).empty());
  EXPECT_TRUE(st::mined_shortlist(reads, 3.0, missing, 8).empty());
}

// Review MINE-6 (Ruling E-32a): the label-overlap factors and the window floors they were derived
// on are registered with the rule, and the derivation's estimator (mine_overlap_factor.py
// summarize_t) is the verb's: summarize_rank_ic at Bartlett lag 21 over the defined days. The
// pinned t's are the script's on the same series (test_mine_overlap_factor.py pins them too, and
// pins both factor tables against this header).
TEST(StrategyMineRule, OverlapFactorIsRegisteredAndItsEstimatorIsTheVerbs) {
  EXPECT_EQ(st::kMinedOverlapBands[1].top, 1000U);
  EXPECT_EQ(st::kMinedOverlapBands[1].factor, 1.54);
  EXPECT_EQ(st::kMinedMinDiscoverRows, 504U);
  EXPECT_EQ(st::kMinedMinConfirmRows, 200U);
  EXPECT_EQ(ex::kResearchIcHacLag, 21U);
  EXPECT_DOUBLE_EQ(st::mined_overlap_corrected(3.1, 1.55), 2.0);
  EXPECT_TRUE(std::isnan(st::mined_overlap_corrected(missing, 1.54)));
  std::vector<f64> daily(60);
  for (usize k = 0; k < daily.size(); ++k)
    daily[k] = static_cast<f64>(static_cast<int>((k * 37U) % 23U) - 11) / 100.0 + 0.004;
  daily[7] = missing;
  daily[30] = missing;
  std::vector<f64> compact;
  const auto full = cb::summarize_rank_ic(daily, ex::kResearchIcHacLag, compact);
  EXPECT_EQ(full.dates, 58U);
  EXPECT_NEAR(full.hac_t, 2.096947998340487, 1e-12);
  // Eleven defined days: the lag clamps to 10.
  const auto clamped = cb::summarize_rank_ic(std::span<const f64>(daily).first(12U),
                                             ex::kResearchIcHacLag, compact);
  EXPECT_EQ(clamped.dates, 11U);
  EXPECT_NEAR(clamped.hac_t, -0.3360738764789554, 1e-12);
}

TEST(StrategyMineRule, RhoIsGreedyAgainstMembersAndKeptCandidates) {
  // Row 0 a pool member m; rows 1..3 candidates: c (orthogonal to m), c again, m again.
  const std::vector<f64> m{1, -1, 1, -1, 1, -1, 1, -1}, c{1, 1, -1, -1, 1, 1, -1, -1};
  cb::PairwiseRowCorrelation rho(4, 3);
  const std::vector<std::span<const f64>> rows{m, c, c, m};
  for (int d = 0; d < 3; ++d) ASSERT_TRUE(rho.add_date(rows));
  const auto out = st::mined_rho_select(rho, 1, 3);
  ASSERT_EQ(out.size(), 3U);
  EXPECT_TRUE(out[0].pass);
  EXPECT_NEAR(out[0].max_abs, 0.0, 1e-12);
  EXPECT_FALSE(out[1].pass); // blocked by candidate 0 (row 1), which passed
  EXPECT_EQ(out[1].against, 1U);
  EXPECT_NEAR(out[1].max_abs, 1.0, 1e-12);
  EXPECT_FALSE(out[2].pass); // blocked by the pool member (row 0)
  EXPECT_EQ(out[2].against, 0U);
}

// Review MINE-6 (lane MINE-STAT): four confirm reads are taken as t / Fc(4) = t / 1.77 -- the raw
// t's 10.62, 4.425 and 3.363 are 6.0, 2.5 and 1.9 on the corrected scale.
TEST(StrategyMineRule, ConfirmIsOneSidedWithBenjaminiYekutieli) {
  const std::vector<f64> t{10.62, 4.425, 3.363, missing};
  const auto out = st::mined_confirm(t);
  ASSERT_EQ(out.size(), 4U);
  EXPECT_EQ(out[1].t, 4.425);
  for (const auto &read : out) EXPECT_EQ(read.factor, 1.77);
  EXPECT_NEAR(out[0].t_corrected, 6.0, 1e-12);
  EXPECT_NEAR(out[1].t_corrected, 2.5, 1e-12);
  EXPECT_NEAR(out[2].t_corrected, 1.9, 1e-12);
  EXPECT_NEAR(out[1].p, 0.0062097, 1e-6); // Phi(-2.5)
  EXPECT_NEAR(out[1].p_by, 0.0258736, 1e-6); // 4 x 2.0833 / 2 x p
  EXPECT_NEAR(out[2].p_by, 0.0797682, 1e-6);
  EXPECT_EQ(out[3].p, 1.0);
  EXPECT_TRUE(out[0].confirmed);
  EXPECT_TRUE(out[1].confirmed);
  EXPECT_FALSE(out[2].confirmed); // p_BY passes and the raw t clears 2; t / F does not
  EXPECT_FALSE(out[3].confirmed);
}

// Review MINE-2: a confirm read counts only on its full window of at least 200 label rows.
TEST(StrategyMineRule, ConfirmReadNeedsItsFullWindow) {
  EXPECT_TRUE(st::mined_confirm_defined(true, 228, 228, 228));
  EXPECT_FALSE(st::mined_confirm_defined(false, 228, 228, 228)); // the h 21 IC undefined
  EXPECT_FALSE(st::mined_confirm_defined(true, 199, 228, 228));  // too few IC rows
  EXPECT_FALSE(st::mined_confirm_defined(true, 228, 227, 228));  // a marginal day missing
  EXPECT_FALSE(st::mined_confirm_defined(true, 3, 3, 3));        // three overlapping rows
  EXPECT_EQ(st::kMinedMinConfirmRows, 200U);
}

// Lane MINE-STAT (lifts Ruling PM4-13): the discover factor is read from the --budget band and the
// confirm factor from the band of m, the reads that reach the confirm; a band's top belongs to it,
// and a count the table does not cover (0, or above the last top) reads NaN, so no hurdle passes.
// Both tables grow with their count (deeper levels, heavier tails), and the ceiling is the
// overlap table's last top.
TEST(StrategyMineRule, FactorsAreReadFromTheirBands) {
  EXPECT_EQ(st::kMinedMaxBudget, 10000U);
  EXPECT_EQ(st::kMinedMaxBudget, st::kMinedOverlapBands.back().top);
  const std::vector<std::pair<u64, f64>> budgets{
      {1U, 1.47},    {100U, 1.47},   {101U, 1.54},  {128U, 1.54},
      {1000U, 1.54}, {1001U, 1.63}, {10000U, 1.63}};
  for (const auto &[budget, factor] : budgets)
    EXPECT_EQ(st::mined_overlap_factor(budget), factor) << budget;
  EXPECT_TRUE(std::isnan(st::mined_overlap_factor(0U)));
  EXPECT_TRUE(std::isnan(st::mined_overlap_factor(st::kMinedMaxBudget + 1U)));
  const std::vector<std::pair<usize, f64>> reads{
      {1U, 1.77}, {16U, 1.77}, {17U, 1.96}, {64U, 1.96}, {65U, 2.15}, {256U, 2.15}};
  for (const auto &[m, factor] : reads) EXPECT_EQ(st::mined_confirm_factor(m), factor) << m;
  EXPECT_TRUE(std::isnan(st::mined_confirm_factor(0U)));
  EXPECT_TRUE(std::isnan(st::mined_confirm_factor(257U)));
  for (usize k = 1; k < st::kMinedOverlapBands.size(); ++k) {
    EXPECT_GT(st::kMinedOverlapBands[k].top, st::kMinedOverlapBands[k - 1].top);
    EXPECT_GT(st::kMinedOverlapBands[k].factor, st::kMinedOverlapBands[k - 1].factor);
  }
  for (usize k = 1; k < st::kMinedConfirmBands.size(); ++k) {
    EXPECT_GT(st::kMinedConfirmBands[k].top, st::kMinedConfirmBands[k - 1].top);
    EXPECT_GT(st::kMinedConfirmBands[k].factor, st::kMinedConfirmBands[k - 1].factor);
  }
  // The configuration's shortlist bound (--max-promotions 1..256) is the confirm table's last top.
  EXPECT_EQ(st::kMinedConfirmBands.back().top, 256U);
}

// Lane MINE-STAT: the confirm factor follows m. The same t 4.425 reads 2.5 among 16 reads (Fc
// 1.77) and about 2.26 among 17 (Fc 1.96); above the table (257 reads) Fc is NaN and nothing
// confirms however large t is.
TEST(StrategyMineRule, ConfirmFactorFollowsTheReadsThatReachIt) {
  std::vector<f64> t(16U, missing);
  t[0] = 4.425;
  const auto sixteen = st::mined_confirm(t);
  EXPECT_EQ(sixteen[0].factor, 1.77);
  EXPECT_NEAR(sixteen[0].t_corrected, 2.5, 1e-12);
  t.push_back(missing);
  const auto seventeen = st::mined_confirm(t);
  EXPECT_EQ(seventeen[0].factor, 1.96);
  EXPECT_NEAR(seventeen[0].t_corrected, 4.425 / 1.96, 1e-12);
  EXPECT_LT(seventeen[0].t_corrected, sixteen[0].t_corrected);
  std::vector<f64> many(257U, 50.0);
  const auto over = st::mined_confirm(many);
  ASSERT_EQ(over.size(), 257U);
  for (const auto &read : over) {
    EXPECT_TRUE(std::isnan(read.factor));
    EXPECT_FALSE(read.confirmed);
  }
}

TEST(StrategyMine, TemplatesAreTheHouseSet) {
  const std::vector<std::string> fields{"a", "b"};
  const auto t = st::mine_templates(fields);
  ASSERT_EQ(t.size(), 22U);
  EXPECT_EQ(t[0], "rank(a)");
  EXPECT_EQ(t[1], "rank(ts_mean(a, 5))");
  EXPECT_EQ(t[5], "rank(ts_mean(a, 252))");
  EXPECT_EQ(t[6], "rank(delta(a, 5))");
  EXPECT_EQ(t[21], "rank(delta(b, 252))");
}

// Review MINE-10: the memory admission is the derived sum documented in strategy_mine.hpp. The
// pinned totals come from the MINE-FIX report's Python mirror of that formula: a small footprint
// and the report's worked example (the 4-year role, 1,405 x 6,100, 16 fields, 4 workers, one
// rung, the default search's 272 trials). One more trial costs its daily IC, its allowance and
// its registry row; one more prior record its registry row. The configuration bounds need far
// more than the 64 GiB --max-memory-mib ceiling, so such a campaign is refused before payload.
TEST(StrategyMine, WorkingBytesAreTheDerivedSum) {
  EXPECT_EQ(st::kMineMaxProgramSlots, 8U);
  st::MineFootprint small;
  small.dates = 100;
  small.names = 10;
  small.extras = 2;
  small.regressors = 1;
  small.members = 1;
  small.workers = 2;
  small.rungs = 1;
  small.shortlist = 3;
  small.trials = 40;
  small.prior_records = 7;
  const auto base = st::mine_working_bytes(small);
  ASSERT_TRUE(base.has_value()) << base.error().to_string();
  EXPECT_EQ(*base, u64{85'689'444});
  auto more_trials = small;
  more_trials.trials += 1U;
  EXPECT_EQ(st::mine_working_bytes(more_trials).value() - *base,
            u64{100} * 8U + (u64{16} << 10) + 128U);
  auto more_prior = small;
  more_prior.prior_records += 1U;
  EXPECT_EQ(st::mine_working_bytes(more_prior).value() - *base, u64{128});
  st::MineFootprint role;
  role.dates = 1405;
  role.names = 6100;
  role.extras = 16;
  role.regressors = 3;
  role.members = 32;
  role.workers = 4;
  role.rungs = 1;
  role.shortlist = 16;
  role.trials = 16U * 11U + 24U * 4U;
  EXPECT_EQ(st::mine_working_bytes(role).value(), u64{11'651'171'382});
  st::MineFootprint bounds;
  bounds.dates = 4096;
  bounds.names = 20000;
  bounds.extras = 64;
  bounds.regressors = 11;
  bounds.members = 64;
  bounds.workers = 64;
  bounds.rungs = 2;
  bounds.shortlist = 256;
  bounds.trials = 64U * 11U + 4096U * 256U;
  EXPECT_EQ(st::mine_working_bytes(bounds).value(), u64{1'184'945'709'312});
  EXPECT_GT(st::mine_working_bytes(bounds).value(), u64{64} << 30);
  bounds.trials = (u64{1} << 32) + 1U;
  EXPECT_FALSE(st::mine_working_bytes(bounds).has_value());
}

// ---- the fixture acceptance --------------------------------------------------------------------
// The shortlist row of `dsl` in campaign.json's promotions (null when it is not shortlisted).
const Json *promotion_of(const Json &campaign, const std::string &dsl) {
  for (const Json &row : campaign.at("promotions"))
    if (row.at("dsl").get<std::string>() == dsl) return &row;
  return nullptr;
}

// Lane MINE-STAT: a factor table as the recipe carries it, [[top, factor], ...].
Json bands_of(std::span<const st::MinedFactorBand> bands) {
  Json out = Json::array();
  for (const st::MinedFactorBand &band : bands)
    out.push_back(Json::array({band.top, band.factor}));
  return out;
}

// 3 planted signals promoted, the planted copy stopped by the marginal term alone (the pool's
// member m2 is independent of it, so no rho step could), no noise expression promoted, in 5
// seeds; registry count = evaluated + racing-rejected + screen-rejected. Review MINE-9: stage 1
// (the templates, one generation, no mutation) does not depend on the seed, so the seeds differ
// in stage 2 only; the test pins that stage 1 is the same in every seed rather than counting it
// five times.
TEST(StrategyMineCampaign, PromotesThePlantedSignalsOnlyInFiveSeeds) {
  Fixture f;
  ASSERT_TRUE(f.ok);
  std::string first_stage1;
  Json first_stage1_search;
  for (const u64 seed : {1U, 2U, 3U, 4U, 5U}) {
    const auto cfg = f.config("seed" + std::to_string(seed), seed, 1);
    std::ostringstream progress;
    const auto status = st::run_mine(cfg, progress);
    ASSERT_TRUE(status) << status.error().to_string() << "\n" << progress.str();
    const fs::path out(cfg.output_directory);
    const Json campaign = read_json(out / "campaign.json");
    const Json mined = read_json(out / "mined_members.json");
    const auto rows = read_trials(out / "trials.csv");
    const f64 hurdle = campaign.at("hurdle").at("t").get<f64>();
    const Json &trials = campaign.at("trials");
    const Json &registry = campaign.at("registry");
    EXPECT_EQ(registry.at("n_raw").get<u64>(),
              trials.at("evaluated").get<u64>() + trials.at("screen_rejected").get<u64>() +
                  trials.at("racing_rejected").get<u64>())
        << "seed " << seed;
    EXPECT_EQ(trials.at("failed").get<u64>(), 0U) << "seed " << seed; // none past the slot bound
    EXPECT_EQ(campaign.at("search").at("max_program_slots").get<u32>(), st::kMineMaxProgramSlots);
    EXPECT_GT(trials.at("racing_rejected").get<u64>(), 0U) << "seed " << seed;
    EXPECT_EQ(rows.size(), trials.at("distinct").get<usize>()) << "seed " << seed;
    EXPECT_EQ(registry.at("new_records"), registry.at("n_raw")) << "seed " << seed;
    // Ruling E-32a: the hurdle is the Bonferroni value of the declared budget, not of n_raw.
    EXPECT_EQ(campaign.at("budget").get<u64>(), kBudget);
    EXPECT_EQ(campaign.at("hurdle").at("budget").get<u64>(), kBudget);
    EXPECT_LE(registry.at("n_raw").get<u64>(), kBudget);
    EXPECT_LE(campaign.at("search").at("capacity").get<u64>(), kBudget);
    EXPECT_EQ(hurdle, st::mined_hurdle(kBudget)) << "seed " << seed;
    // Review MINE-6 (lane MINE-STAT): the hurdle is read on f2 / F, F the factor of the budget's
    // band; the recipe carries the tables F is read from.
    const f64 factor = st::mined_overlap_factor(kBudget);
    EXPECT_EQ(campaign.at("hurdle").at("overlap_factor").get<f64>(), factor);
    EXPECT_EQ(campaign.at("recipe").at("overlap_bands"), bands_of(st::kMinedOverlapBands));
    EXPECT_EQ(campaign.at("recipe").at("confirm_bands"), bands_of(st::kMinedConfirmBands));
    std::array<bool, 3> planted_read{};
    for (const Json &member : mined.at("members")) {
      const auto dsl = member.at("dsl").get<std::string>();
      bool planted = false;
      for (usize k = 0; k < 3U; ++k)
        if (dsl.find("p" + std::to_string(k + 1U)) != std::string::npos) {
          planted_read[k] = true;
          planted = true;
        }
      EXPECT_TRUE(planted) << "seed " << seed << ": promoted without a planted field: " << dsl;
      EXPECT_EQ(member.at("theme"), "mined");
    }
    EXPECT_TRUE(planted_read[0] && planted_read[1] && planted_read[2]) << "seed " << seed;
    // The copy predicts the label (f1 above the value) but adds nothing to the book (f2 below).
    const auto copy = std::find_if(rows.begin(), rows.end(),
                                   [](const TrialRow &row) { return row.dsl == "rank(copy)"; });
    ASSERT_NE(copy, rows.end()) << "seed " << seed;
    EXPECT_EQ(copy->status, "evaluated") << "seed " << seed;
    EXPECT_GE(st::mined_overlap_corrected(copy->f1, factor), hurdle) << "seed " << seed;
    EXPECT_LT(st::mined_overlap_corrected(copy->f2, factor), hurdle) << "seed " << seed;
    EXPECT_TRUE(promotion_of(campaign, "rank(copy)") == nullptr) << "seed " << seed;
    // Stage 1 in the same order with the same racing in every seed.
    std::string stage1;
    for (const TrialRow &row : rows)
      if (row.stage == "1") stage1 += row.canon_hash + " " + row.dsl + "\n";
    Json stage1_search = campaign.at("search").at("stage1");
    stage1_search.erase("seconds");
    EXPECT_EQ(stage1_search.at("trials").get<usize>(), 55U) << "seed " << seed;
    if (seed == 1U) {
      first_stage1 = stage1;
      first_stage1_search = stage1_search;
    } else {
      EXPECT_EQ(stage1, first_stage1) << "seed " << seed;
      EXPECT_EQ(stage1_search, first_stage1_search) << "seed " << seed;
    }
    expect_campaign_line(cfg, campaign, out, "seed " + std::to_string(seed));
  }
}

// Review MINE-9 (T-1 standard): the mined-v1 rule pinned on the 88 templates of all eight fields
// (stage 1 only, no racing, so every template is read in full; --budget 1000), each pin a case a
// wrong rule passes:
//   E-32      rank(swap) is shortlisted and passes the rho check, and its raw IC t / Fc on the
//             confirm window clears 2, but its confirm marginal t is undefined (the book spans
//             every confirm row): a raw-IC confirm would admit it;
//   sign      rank(neg) is admitted with sign -1; rank(flip) (sign -1 frozen from discover) reads
//             a confirm t / Fc below -2 (about -5.6 raw) and is rejected: a confirm on |t| would
//             admit it;
//   N         the hurdle is mined_hurdle(--budget), not that of the 88 trials recorded, and the
//             shortlist is exactly the evaluated rows with f2 / F at or above it, by f2;
//   copy      rank(copy) clears the hurdle on f1 and not on f2, so it never reaches the rho step
//             (where the independent member m2 could not stop it).
TEST(StrategyMineCampaign, RulePinsOnTheTemplates) {
  Fixture f;
  ASSERT_TRUE(f.ok);
  auto cfg = f.config("pins", 1, 1);
  cfg.role.fields = {"p1", "p2", "p3", "copy", "n1", "swap", "neg", "flip"};
  cfg.stage2_generations = 0;
  cfg.race_strides.clear();
  cfg.max_promotions = 64; // above the 27 rows the replica shortlists: no cap
  // The 1,000 band (F 1.54). At the 10,000 ceiling the hurdle is 4.56 x 1.63, which the replica's
  // rank(swap) does not clear.
  cfg.budget = 1000U;
  std::ostringstream progress;
  const auto status = st::run_mine(cfg, progress);
  ASSERT_TRUE(status) << status.error().to_string() << "\n" << progress.str();
  const fs::path out(cfg.output_directory);
  const Json campaign = read_json(out / "campaign.json");
  const auto rows = read_trials(out / "trials.csv");
  // Ruling PM4-13 lifted by the table (lane MINE-STAT): the ceiling and both factor tables are in
  // the recipe (so its identity), the ceiling and the factor of the budget's band in the hurdle.
  EXPECT_EQ(campaign.at("budget").get<u64>(), 1000U);
  EXPECT_EQ(campaign.at("recipe").at("max_budget").get<u64>(), st::kMinedMaxBudget);
  EXPECT_EQ(campaign.at("hurdle").at("max_budget").get<u64>(), st::kMinedMaxBudget);
  EXPECT_EQ(campaign.at("recipe").at("overlap_bands"), bands_of(st::kMinedOverlapBands));
  EXPECT_EQ(campaign.at("recipe").at("confirm_bands"), bands_of(st::kMinedConfirmBands));
  const f64 factor = st::mined_overlap_factor(1000U);
  EXPECT_EQ(campaign.at("hurdle").at("overlap_factor").get<f64>(), factor);
  ASSERT_EQ(rows.size(), 88U);
  ASSERT_EQ(campaign.at("registry").at("n_raw").get<u64>(), 88U);
  // N: the declared budget's Bonferroni value.
  const f64 hurdle = campaign.at("hurdle").at("t").get<f64>();
  EXPECT_EQ(hurdle, st::mined_hurdle(1000));
  EXPECT_NE(hurdle, st::mined_hurdle(88));
  // The shortlist: every evaluated row with f2 / F >= the hurdle, f2 descending, hash ascending.
  std::vector<const TrialRow *> expected;
  for (const TrialRow &row : rows)
    if (row.status == "evaluated" && std::isfinite(row.f2) &&
        st::mined_overlap_corrected(row.f2, factor) >= hurdle)
      expected.push_back(&row);
  std::sort(expected.begin(), expected.end(), [](const TrialRow *a, const TrialRow *b) {
    return a->f2 != b->f2 ? a->f2 > b->f2 : a->canon_hash < b->canon_hash;
  });
  const Json &promotions = campaign.at("promotions");
  ASSERT_EQ(promotions.size(), expected.size());
  ASSERT_LT(expected.size(), cfg.max_promotions);
  for (usize k = 0; k < expected.size(); ++k)
    EXPECT_EQ(promotions[k].at("canon_hash").get<std::string>(), expected[k]->canon_hash) << k;
  // E-32: the swapped field.
  const Json *swapped = promotion_of(campaign, "rank(swap)");
  ASSERT_TRUE(swapped != nullptr);
  EXPECT_TRUE(swapped->at("rho_pass").get<bool>());
  EXPECT_TRUE(swapped->at("confirm_read").get<bool>());
  EXPECT_TRUE(swapped->at("confirm_defined").get<bool>());
  EXPECT_TRUE(swapped->at("confirm_marginal_t").is_null());
  EXPECT_GE(st::mined_overlap_corrected(swapped->at("confirm_ic_t").get<f64>(),
                                        swapped->at("confirm_factor").get<f64>()),
            st::kMinedConfirmT);
  EXPECT_FALSE(swapped->at("admitted").get<bool>());
  // Sign: frozen from discover.
  const Json *negative = promotion_of(campaign, "rank(neg)");
  ASSERT_TRUE(negative != nullptr);
  EXPECT_EQ(negative->at("sign").get<int>(), -1);
  EXPECT_GE(negative->at("confirm_t_corrected").get<f64>(), st::kMinedConfirmT);
  EXPECT_TRUE(negative->at("admitted").get<bool>());
  const Json *flipped = promotion_of(campaign, "rank(flip)");
  ASSERT_TRUE(flipped != nullptr);
  EXPECT_EQ(flipped->at("sign").get<int>(), -1);
  EXPECT_TRUE(flipped->at("rho_pass").get<bool>());
  EXPECT_TRUE(flipped->at("confirm_defined").get<bool>());
  EXPECT_LE(flipped->at("confirm_t_corrected").get<f64>(), -st::kMinedConfirmT);
  EXPECT_FALSE(flipped->at("admitted").get<bool>());
  // The copy: f1 clears the hurdle, f2 does not, so it is not shortlisted.
  const auto copy = std::find_if(rows.begin(), rows.end(),
                                 [](const TrialRow &row) { return row.dsl == "rank(copy)"; });
  ASSERT_NE(copy, rows.end());
  EXPECT_EQ(copy->status, "evaluated");
  EXPECT_GE(st::mined_overlap_corrected(copy->f1, factor), hurdle);
  EXPECT_LT(st::mined_overlap_corrected(copy->f2, factor), hurdle);
  EXPECT_TRUE(promotion_of(campaign, "rank(copy)") == nullptr);
  // Admitted: only expressions of p1, p2, p3 or neg, and each of them.
  const Json mined = read_json(out / "mined_members.json");
  std::array<bool, 4> read_field{};
  const std::array<std::string, 4> planted{"p1", "p2", "p3", "neg"};
  for (const Json &member : mined.at("members")) {
    const auto dsl = member.at("dsl").get<std::string>();
    bool any = false;
    for (usize k = 0; k < planted.size(); ++k) {
      if (dsl.find(planted[k]) == std::string::npos) continue;
      read_field[k] = true;
      any = true;
    }
    EXPECT_TRUE(any) << dsl;
    for (const char *field : {"swap", "flip", "copy", "n1"})
      EXPECT_EQ(dsl.find(field), std::string::npos) << dsl;
  }
  EXPECT_TRUE(read_field[0] && read_field[1] && read_field[2] && read_field[3]);
}

// Same seed twice and at 1 and 4 workers: the same registry chain head, trial log and members.
// An existing registry reopens only against its exported head, and a re-run of the recorded
// campaign's recipe (a second confirm read on its identity, review MINE-3) is refused, writing
// nothing.
TEST(StrategyMineCampaign, SameSeedSameChainHeadAtOneAndFourWorkers) {
  Fixture f;
  ASSERT_TRUE(f.ok);
  std::vector<std::string> heads, logs, members;
  for (const auto &[tag, workers] :
       std::vector<std::pair<std::string, usize>>{{"w1a", 1U}, {"w1b", 1U}, {"w4", 4U}}) {
    const auto cfg = f.config(tag, 7, workers);
    std::ostringstream progress;
    const auto status = st::run_mine(cfg, progress);
    ASSERT_TRUE(status) << status.error().to_string();
    const fs::path out(cfg.output_directory);
    heads.push_back(read_json(out / "campaign.json").at("registry").at("head").get<std::string>());
    logs.push_back(text_of(out / "trials.csv"));
    members.push_back(read_json(out / "mined_members.json").at("members").dump());
  }
  for (usize k = 1; k < heads.size(); ++k) {
    EXPECT_EQ(heads[k], heads[0]) << k;
    EXPECT_EQ(logs[k], logs[0]) << k;
    EXPECT_EQ(members[k], members[0]) << k;
  }
  auto again = f.config("w1a", 7, 1);
  again.output_directory = (f.dir.path / "out-w1a-again").string();
  std::ostringstream progress;
  const auto refused = st::run_mine(again, progress);
  ASSERT_FALSE(refused);
  EXPECT_NE(refused.error().message().find("--registry-head"), std::string::npos);
  EXPECT_FALSE(fs::exists(again.output_directory));
  again.registry_head_path = (f.dir.path / "out-w1a" / "registry_head.txt").string();
  const std::string log_before = text_of(again.registry_path);
  // Review MINE-3: the same recipe again -- the same seed, or another seed (stage 1 is the same
  // templates) -- is a second confirm read on the same identity, refused before the confirm read.
  for (const u64 seed : {u64{7}, u64{8}}) {
    again.seed = seed;
    const auto rerun = st::run_mine(again, progress);
    ASSERT_FALSE(rerun) << seed;
    EXPECT_NE(rerun.error().message().find("a second confirm read on the same identity"),
              std::string::npos)
        << rerun.error().to_string();
    EXPECT_FALSE(fs::exists(again.output_directory));
    EXPECT_EQ(text_of(again.registry_path), log_before);
  }
}

// Ruling E-33a: two campaigns on one registry. The second has another confirm window, which
// review MINE-3 puts in every trial's identity, so all its trials are new records; it reopens
// the registry against the first one's head. Each line counts only the records its campaign
// added and carries the registry's size as its total, so the two counts sum to the registry.
TEST(StrategyMineCampaign, SharedRegistryLinesCountEachCampaignsOwnRecords) {
  Fixture f;
  ASSERT_TRUE(f.ok);
  const auto first = f.config("shared-a", 3, 1);
  std::ostringstream progress;
  const auto a_status = st::run_mine(first, progress);
  ASSERT_TRUE(a_status) << a_status.error().to_string();
  auto second = f.config("shared-b", 3, 1);
  second.campaign_id = "fixture-b";
  second.registry_path = first.registry_path;
  second.registry_head_path = (fs::path(first.output_directory) / "registry_head.txt").string();
  second.confirm_begin = "2023-03-01"; // the discover window (the registry's calendar) is kept
  const auto b_status = st::run_mine(second, progress);
  ASSERT_TRUE(b_status) << b_status.error().to_string();
  const Json a = read_json(fs::path(first.output_directory) / "campaign.json");
  const Json b = read_json(fs::path(second.output_directory) / "campaign.json");
  const u64 a_new = a.at("registry").at("new_records").get<u64>();
  const u64 b_new = b.at("registry").at("new_records").get<u64>();
  const u64 b_total = b.at("registry").at("n_raw").get<u64>();
  EXPECT_EQ(a_new, a.at("registry").at("n_raw").get<u64>()); // a fresh registry
  EXPECT_EQ(b_new, b.at("trials").at("distinct").get<u64>());
  EXPECT_EQ(b_total, a_new + b_new);
  EXPECT_FALSE(b.at("registry").at("anchor").is_null());
  EXPECT_NE(a.at("recipe_sha256"), b.at("recipe_sha256"));
  const Json a_line = read_json(fs::path(first.output_directory) / "ledger_line.json");
  const Json b_line = read_json(fs::path(second.output_directory) / "ledger_line.json");
  EXPECT_EQ(a_line.at("registry").at("count").get<u64>(), a_new);
  EXPECT_EQ(b_line.at("registry").at("count").get<u64>(), b_new);
  EXPECT_EQ(b_line.at("registry").at("total").get<u64>(), b_total);
  EXPECT_EQ(a_line.at("registry").at("count").get<u64>() +
                b_line.at("registry").at("count").get<u64>(),
            b_total);
  // Ruling E-32a: both hurdles are the budget's, whatever the registry held before.
  EXPECT_EQ(a.at("hurdle").at("t").get<f64>(), st::mined_hurdle(kBudget));
  EXPECT_EQ(b.at("hurdle").at("t").get<f64>(), st::mined_hurdle(kBudget));
  expect_campaign_line(first, a, first.output_directory, "first");
  expect_campaign_line(second, b, second.output_directory, "second");
}

// Pre-registration rule 10 (Ruling E-32a): --budget is required and must cover the
// configuration's trial capacity (the templates plus the stage-2 population times its
// generations); otherwise the campaign is refused before any payload and writes nothing.
TEST(StrategyMineCampaign, RefusesAMissingBudgetOrOneBelowTheCapacity) {
  Fixture f;
  ASSERT_TRUE(f.ok);
  auto cfg = f.config("budget", 1, 1);
  const u64 templates = cfg.role.fields.size() * 11U;
  const u64 capacity = st::mine_trial_capacity(cfg);
  EXPECT_EQ(capacity, templates + 16U * 2U);
  std::ostringstream progress;
  for (const u64 budget : {u64{0}, capacity - 1U}) {
    cfg.budget = budget;
    const auto refused = st::run_mine(cfg, progress);
    ASSERT_FALSE(refused) << budget;
    EXPECT_NE(refused.error().message().find("--budget"), std::string::npos)
        << refused.error().to_string();
    EXPECT_FALSE(fs::exists(cfg.output_directory));
    EXPECT_FALSE(fs::exists(cfg.registry_path));
  }
  cfg.stage2_generations = 0; // no stage 2: the templates alone
  EXPECT_EQ(st::mine_trial_capacity(cfg), templates);
}

// Ruling PM4-13, lifted by lane MINE-STAT's table: the overlap factors are validated to --budget
// kMinedMaxBudget (10,000) only, so 10,001 is refused before any payload -- here the role and
// pool manifests do not even exist -- and writes nothing. The ledger twin refuses a line above
// the ceiling in campaign_line's words and accepts one at it.
TEST(StrategyMineCampaign, RefusesABudgetAboveTheCeilingBeforeAnyPayload) {
  Fixture f;
  ASSERT_TRUE(f.ok);
  EXPECT_EQ(st::kMinedMaxBudget, 10000U);
  auto cfg = f.config("ceiling", 1, 1);
  cfg.budget = st::kMinedMaxBudget + 1U;
  cfg.role.manifest = (f.dir.path / "absent-role.json").string();
  cfg.pool_path = (f.dir.path / "absent-pool.json").string();
  std::ostringstream progress;
  const auto refused = st::run_mine(cfg, progress);
  ASSERT_FALSE(refused);
  const std::string message = refused.error().message();
  EXPECT_NE(message.find("--budget 10001 is above kMinedMaxBudget 10000 (Ruling PM4-13)"),
            std::string::npos)
      << message;
  EXPECT_NE(message.find("validated to that budget only"), std::string::npos) << message;
  EXPECT_FALSE(fs::exists(cfg.output_directory));
  EXPECT_FALSE(fs::exists(cfg.registry_path));
  st::MineLedgerLine line;
  line.campaign_id = "ceiling";
  line.registry_path = "mine/registry.atxtrg";
  line.registry_head = std::string(64U, 'a');
  line.registry_bytes = 4096U;
  line.registry_count = 40U;
  line.registry_total = 40U;
  line.budget = st::kMinedMaxBudget;
  line.recipe_sha256 = std::string(64U, 'b');
  line.confirm_begin = "2023-01-01";
  line.confirm_end = "2024-01-01";
  line.window_id = "research-window-v2";
  const auto at_ceiling = st::mine_ledger_line(line);
  ASSERT_TRUE(at_ceiling) << at_ceiling.error().to_string();
  EXPECT_EQ(st::mine_ledger_line_problem(*at_ceiling), "");
  line.budget = st::kMinedMaxBudget + 1U;
  const auto above = st::mine_ledger_line(line);
  ASSERT_FALSE(above);
  EXPECT_NE(above.error().message().find(
                "budget is at most 10000 (kMinedMaxBudget, Ruling PM4-13: the overlap factor is "
                "validated to that budget only)"),
            std::string::npos)
      << above.error().to_string();
  Json over = Json::parse(*at_ceiling);
  over["budget"] = st::kMinedMaxBudget + 1U;
  EXPECT_NE(st::mine_ledger_line_problem(over.dump()).find("Ruling PM4-13"), std::string::npos);
}

// Ruling E-32a (review MINE-7): mined-v1 reads the book. A campaign without --pool, or with a
// pool that names no member or no regressor, is refused before any payload and writes nothing.
TEST(StrategyMineCampaign, RefusesACampaignWithoutTheBook) {
  Fixture f;
  ASSERT_TRUE(f.ok);
  std::ostringstream progress;
  auto cfg = f.config("nopool", 1, 1);
  cfg.pool_path.clear();
  cfg.pool_sha256.clear();
  const auto missing = st::run_mine(cfg, progress);
  ASSERT_FALSE(missing);
  EXPECT_NE(missing.error().message().find("mined-v1 needs --pool"), std::string::npos)
      << missing.error().to_string();
  EXPECT_FALSE(fs::exists(cfg.output_directory));
  EXPECT_FALSE(fs::exists(cfg.registry_path));
  const auto refused = [&](const std::string &manifest, const std::vector<std::string> &regressors,
                           const std::vector<std::string> &members) {
    std::string sha;
    ASSERT_TRUE(f.write_pool_manifest(manifest, regressors, members, sha)) << manifest;
    cfg.pool_path = (f.dir.path / "pool" / manifest).string();
    cfg.pool_sha256 = sha;
    const auto status = st::run_mine(cfg, progress);
    ASSERT_FALSE(status) << manifest;
    EXPECT_NE(status.error().message().find("at least one regressor and one member"),
              std::string::npos)
        << status.error().to_string();
    EXPECT_FALSE(fs::exists(cfg.output_directory)) << manifest;
    EXPECT_FALSE(fs::exists(cfg.registry_path)) << manifest;
  };
  refused("no-member.json", {"book"}, {});
  refused("no-regressor.json", {}, {"m2"});
}

// Research window: a role with a session on 2024-01-02 is refused from its manifest, before any
// payload, and so is a window past TRAIN. Nothing is written.
TEST(StrategyMineCampaign, RefusesSealedRolesAndWindowsPastTrain) {
  Fixture f;
  ASSERT_TRUE(f.ok);
  std::string sealed_sha;
  Json files;
  const std::vector<f64> flat((D + 2U) * N, 100.0);
  ASSERT_TRUE(write_role(f.dir.path / "sealed", D + 2U, flat, sealed_sha, files));
  auto cfg = f.config("sealed", 1, 1);
  cfg.role.manifest = (f.dir.path / "sealed" / "manifest.json").string();
  cfg.role.manifest_sha256 = sealed_sha;
  std::ostringstream progress;
  const auto sealed = st::run_mine(cfg, progress);
  ASSERT_FALSE(sealed);
  EXPECT_NE(sealed.error().message().find("research seal 2024-01-01"), std::string::npos)
      << sealed.error().to_string();
  EXPECT_FALSE(fs::exists(cfg.output_directory));
  EXPECT_FALSE(fs::exists(cfg.registry_path));
  auto late = f.config("late", 1, 1);
  late.confirm_end = "2024-01-02";
  const auto past = st::run_mine(late, progress);
  ASSERT_FALSE(past);
  EXPECT_NE(past.error().message().find("inside TRAIN"), std::string::npos);
  EXPECT_FALSE(fs::exists(late.output_directory));
  // Review MINE-2: a confirm window of 162 mature label rows is refused before any search.
  auto short_confirm = f.config("short", 1, 1);
  short_confirm.confirm_begin = "2023-07-01";
  const auto too_short = st::run_mine(short_confirm, progress);
  ASSERT_FALSE(too_short);
  EXPECT_NE(too_short.error().message().find("confirm window: 162 mature h 21 label rows"),
            std::string::npos)
      << too_short.error().to_string();
  EXPECT_FALSE(fs::exists(short_confirm.output_directory));
  EXPECT_FALSE(fs::exists(short_confirm.registry_path));
  // Review MINE-6: a discover window of 495 mature label rows is refused before any search.
  auto short_discover = f.config("short-discover", 1, 1);
  short_discover.discover_end = "2021-06-01";
  const auto too_few = st::run_mine(short_discover, progress);
  ASSERT_FALSE(too_few);
  EXPECT_NE(too_few.error().message().find("discover window: 495 mature h 21 label rows"),
            std::string::npos)
      << too_few.error().to_string();
  EXPECT_FALSE(fs::exists(short_discover.output_directory));
  EXPECT_FALSE(fs::exists(short_discover.registry_path));
}

// Review MINE-18: the window and bound refusals, one row each, all from the configuration before
// any payload (no output, no registry, no admission line): overlapping windows, confirm before
// discover, a discover window before TRAIN, a confirm window past it, an impossible date, and
// --min-dates under the IC recipe's floor of 8.
TEST(StrategyMineCampaign, RefusesBadWindowsAndBoundsBeforeAnyPayload) {
  Fixture f;
  ASSERT_TRUE(f.ok);
  struct Case {
    const char *tag;
    void (*edit)(st::MineConfig &);
    const char *message;
  };
  const std::array<Case, 6> cases{{
      {"overlap", [](st::MineConfig &m) { m.discover_end = "2022-08-01"; }, "non-overlapping"},
      {"order",
       [](st::MineConfig &m) {
         m.discover_begin = "2022-07-01";
         m.discover_end = "2024-01-01";
         m.confirm_begin = "2020-01-01";
         m.confirm_end = "2022-07-01";
       },
       "chronological"},
      {"early", [](st::MineConfig &m) { m.discover_begin = "2019-12-31"; }, "inside TRAIN"},
      {"late", [](st::MineConfig &m) { m.confirm_end = "2024-01-02"; }, "inside TRAIN"},
      {"date", [](st::MineConfig &m) { m.confirm_begin = "2022-02-30"; },
       "--confirm-begin must be a YYYY-MM-DD date"},
      {"min-dates", [](st::MineConfig &m) { m.min_dates = 7; }, "--min-dates >= 8"},
  }};
  for (const Case &c : cases) {
    auto cfg = f.config(c.tag, 1, 1);
    c.edit(cfg);
    std::ostringstream progress;
    const auto refused = st::run_mine(cfg, progress);
    ASSERT_FALSE(refused) << c.tag;
    EXPECT_NE(refused.error().message().find(c.message), std::string::npos)
        << c.tag << ": " << refused.error().to_string();
    EXPECT_FALSE(fs::exists(cfg.output_directory)) << c.tag;
    EXPECT_FALSE(fs::exists(cfg.registry_path)) << c.tag;
    EXPECT_TRUE(progress.str().empty()) << c.tag;
  }
}

// Review MINE-8 (Ruling E-10, review B-3): the shared research-role loader refuses a role built
// with --delisting-returns from its manifest, before any payload -- geometry(), load() and so the
// mining verb, which writes nothing. The same role declaring returns_applied false is read.
TEST(StrategyMineCampaign, RefusesADelistingReturnsRole) {
  Fixture f;
  ASSERT_TRUE(f.ok);
  const std::vector<f64> flat(D * N, 100.0);
  const auto role_spec = [&f, &flat](const std::string &name, bool applied) {
    std::string sha;
    Json files;
    const Json universe{{"delisting", {{"returns_applied", applied}}}};
    EXPECT_TRUE(write_role(f.dir.path / name, D, flat, sha, files, universe)) << name;
    st::ResearchRoleSpec spec;
    spec.manifest = (f.dir.path / name / "manifest.json").string();
    spec.manifest_sha256 = sha;
    return spec;
  };
  const st::ResearchRoleSpec listed = role_spec("listed", false);
  const auto listed_axes = st::ResearchRole::geometry(listed);
  ASSERT_TRUE(listed_axes) << listed_axes.error().to_string();
  EXPECT_EQ(listed_axes->dates, D);
  EXPECT_EQ(listed_axes->instruments, N);
  const st::ResearchRoleSpec delisted = role_spec("delisted", true);
  const auto delisted_axes = st::ResearchRole::geometry(delisted);
  ASSERT_FALSE(delisted_axes);
  EXPECT_NE(delisted_axes.error().message().find("--delisting-returns"), std::string::npos)
      << delisted_axes.error().to_string();
  const auto loaded = st::ResearchRole::load(delisted);
  ASSERT_FALSE(loaded);
  EXPECT_NE(loaded.error().message().find("--delisting-returns"), std::string::npos)
      << loaded.error().to_string();
  auto cfg = f.config("delisted", 1, 1);
  cfg.role.manifest = delisted.manifest;
  cfg.role.manifest_sha256 = delisted.manifest_sha256;
  std::ostringstream progress;
  const auto refused = st::run_mine(cfg, progress);
  ASSERT_FALSE(refused);
  EXPECT_NE(refused.error().message().find("--delisting-returns"), std::string::npos)
      << refused.error().to_string();
  EXPECT_FALSE(fs::exists(cfg.output_directory));
  EXPECT_FALSE(fs::exists(cfg.registry_path));
}
} // namespace
