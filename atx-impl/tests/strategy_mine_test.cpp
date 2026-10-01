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
#include "strategy_mine.hpp"
#include "strategy_mine_ledger.hpp"
#include "strategy_mine_rule.hpp"

// platform v8 H-3: `atx-equity-strategy-mine` on a synthetic role (the sprint plan's fixture
// acceptance) and the mined-v1 arithmetic.
//
// The world: 16 names, one session per calendar day from 2018-12-14 to 2023-12-31 (1,844
// sessions; score_begin 383 is 2020-01-01), every name a decision member from session 63.
// Drivers p1, p2, p3, m1, n1 and the noise e are i.i.d. N(0, 1) over names and days, so no
// expression of a driver's past predicts anything: the one-day return is
// r(t) = .01 (p1 + p2 + p3 + m1)(t - 2) + .01 e(t), and the h 21 label of decision t
// (close[t+22] / close[t+1] - 1) carries each driver at t with rank IC about .09. The pool is the
// book m1: its centred rank is the marginal term's regressor and m1 its member. The mined fields
// are p1, p2, p3 (planted), copy = m1 + .02 noise (the book again) and n1 (noise). Discover is
// [2020-01-01, 2023-01-01) (1,074 mature label rows), confirm [2023-01-01, 2024-01-01) (343).
// A numpy replica of this exact world (same generator; Bartlett-21 t, not the IC recipe's
// conservative one) reads: rank(p_i) f1 11.6 to 14.8, f2 11.2 to 14.5, confirm t 4.9 to 8.5;
// rank(copy) f1 12.2, f2 1.1; rank(n1) f1 0.0. The Bonferroni value is about 3.4 at ~80 trials.
namespace {
using namespace atx;
using Json = nlohmann::json;
namespace fs = std::filesystem;
namespace st = atx::impl::strategy;
namespace cb = atx::engine::combine;
namespace ev = atx::engine::eval;
constexpr i64 day = 86'400'000'000'000LL;
constexpr i64 first_day = 17879; // 2018-12-14
constexpr usize D = 1844, N = 16, score_begin = 383;
constexpr f64 missing = std::numeric_limits<f64>::quiet_NaN();

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
  std::vector<f64> p1, p2, p3, m1, n1, copy, close, book;
  World() {
    Rng g{0x6d696e65U};
    p1 = draws(g, D * N);
    p2 = draws(g, D * N);
    p3 = draws(g, D * N);
    m1 = draws(g, D * N);
    n1 = draws(g, D * N);
    const auto e = draws(g, D * N), jitter = draws(g, D * N);
    copy.resize(D * N);
    for (usize c = 0; c < D * N; ++c) copy[c] = m1[c] + 0.02 * jitter[c];
    close.assign(D * N, 100.0);
    for (usize t = 1; t < D; ++t)
      for (usize i = 0; i < N; ++i) {
        const usize lag = (t >= 2 ? t - 2 : 0) * N + i;
        const f64 driver = t >= 2 ? p1[lag] + p2[lag] + p3[lag] + m1[lag] : 0.0;
        close[t * N + i] = close[(t - 1) * N + i] * (1.0 + 0.01 * driver + 0.01 * e[t * N + i]);
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

// Writes an atx.recent-research-role/v1 role of `days` daily sessions from 2018-12-14.
bool write_role(const fs::path &dir, usize days, const std::vector<f64> &close, std::string &sha,
                Json &files) {
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
  return json_file(dir / "manifest.json",
                   {{"schema", "atx.recent-research-role/v1"}, {"status", "complete"},
                    {"instrument_namespace", "spiderrock.securityID"}, {"dates", days},
                    {"instruments", N}, {"score_begin", score_begin}, {"score_end", days},
                    {"score_start_ns", keys[score_begin]}, {"score_end_ns", keys.back() + day},
                    {"source_sha256", std::string(64, 'a')},
                    {"membership_recipe", membership.dump()},
                    {"clock_recipe", "modeled-session+22h-mark+23h-decision-v1"},
                    {"close_basis", "f64(raw-f32-close)*f64-cumulReturnFactor"},
                    {"volume_basis", "raw-share-volume"}, {"common_stock_verified", false},
                    {"historical_vintage_verified", false},
                    {"declared_output_bytes", days * N * 26 + days * 8 + N * 8}, {"files", files}},
                   sha);
}

struct Fixture {
  Directory dir;
  std::string role_sha, fields_sha, pool_sha;
  bool ok{};
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
        {"p1", &world.p1}, {"p2", &world.p2}, {"p3", &world.p3}, {"copy", &world.copy},
        {"n1", &world.n1}};
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
    Json files = Json::object();
    if (!payload(pool, files, "book.f64", world.book) || !payload(pool, files, "m1.f64", world.m1))
      return false;
    const auto row = [&files](const char *name) {
      const std::string file = std::string(name) + ".f64";
      return Json{{"name", name}, {"file", file}, {"sha256", files[file]["sha256"]},
                  {"bytes", files[file]["bytes"]}};
    };
    return json_file(pool / "manifest.json",
                     {{"schema", "atx.mine-pool/v1"}, {"status", "complete"},
                      {"role_manifest_sha256", role_sha}, {"dates", D}, {"instruments", N},
                      {"regressors", Json::array({row("book")})},
                      {"members", Json::array({row("m1")})}},
                     pool_sha);
  }
  st::MineConfig config(const std::string &tag, u64 seed, usize workers) const {
    st::MineConfig cfg;
    cfg.role.manifest = (dir.path / "role" / "manifest.json").string();
    cfg.role.manifest_sha256 = role_sha;
    cfg.role.fields_directory = (dir.path / "fields").string();
    cfg.role.fields_sha256 = fields_sha;
    cfg.role.fields = {"p1", "p2", "p3", "copy", "n1"};
    cfg.pool_path = (dir.path / "pool" / "manifest.json").string();
    cfg.pool_sha256 = pool_sha;
    cfg.discover_begin = "2020-01-01";
    cfg.discover_end = "2023-01-01";
    cfg.confirm_begin = "2023-01-01";
    cfg.confirm_end = "2024-01-01";
    cfg.registry_path = (dir.path / ("registry-" + tag + ".atxtrg")).string();
    cfg.campaign_id = "fixture";
    cfg.output_directory = (dir.path / ("out-" + tag)).string();
    cfg.seed = seed;
    cfg.workers = workers;
    cfg.stage2_seeds = 8;
    cfg.stage2_population = 16;
    cfg.stage2_generations = 2;
    // The rung keeps half: rank(copy) ranks 6th of the 55 templates on every second name in the
    // numpy replica of this world, rank(p3) 11th, the 28th |t| is under 3.
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
  std::string status, reason, dsl;
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
// `bytes` bytes. The pre-fix verb's 16-hex head is a line that check refuses.
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
  const auto ident = core::sha256_hex(Json::array({"mining-campaign", head_hex}).dump());
  ASSERT_TRUE(ident.has_value());
  const Json expected{{"schema", "atx.trial-ledger/v1"},
                      {"kind", "mining-campaign"},
                      {"count", 0},
                      {"campaign", campaign.at("campaign_id")},
                      {"origin", "mined"},
                      {"window_id", campaign.at("research_window").at("id")},
                      {"registry",
                       {{"path", registry_path},
                        {"chain_head", head_hex},
                        {"bytes", bytes},
                        {"count", registry.at("n_raw")}}},
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

TEST(StrategyMineRule, ShortlistIsByF2ThenHashAndCapped) {
  const std::vector<st::MinedRead> reads{{9, 5.0}, {3, missing}, {2, 7.0}, {1, 5.0}, {4, 3.0}};
  EXPECT_EQ(st::mined_shortlist(reads, 4.0, 8), (std::vector<usize>{2, 3, 0}));
  EXPECT_EQ(st::mined_shortlist(reads, 4.0, 2), (std::vector<usize>{2, 3}));
  EXPECT_TRUE(st::mined_shortlist(reads, missing, 8).empty());
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

TEST(StrategyMineRule, ConfirmIsOneSidedWithBenjaminiYekutieli) {
  const std::vector<f64> t{6.0, 2.5, 1.9, missing};
  const auto out = st::mined_confirm(t);
  ASSERT_EQ(out.size(), 4U);
  EXPECT_NEAR(out[1].p, 0.0062097, 1e-6);
  EXPECT_NEAR(out[1].p_by, 0.0258736, 1e-6); // 4 x 2.0833 / 2 x p
  EXPECT_NEAR(out[2].p_by, 0.0797682, 1e-6);
  EXPECT_EQ(out[3].p, 1.0);
  EXPECT_TRUE(out[0].confirmed);
  EXPECT_TRUE(out[1].confirmed);
  EXPECT_FALSE(out[2].confirmed); // p_BY passes, t < 2 does not
  EXPECT_FALSE(out[3].confirmed);
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

// ---- the fixture acceptance --------------------------------------------------------------------
// 3 planted signals promoted, the planted copy rejected by the marginal term, no noise expression
// promoted, in 5 seeds; registry count = evaluated + racing-rejected + screen-rejected.
TEST(StrategyMineCampaign, PromotesThePlantedSignalsOnlyInFiveSeeds) {
  Fixture f;
  ASSERT_TRUE(f.ok);
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
    EXPECT_EQ(trials.at("failed").get<u64>(), 0U) << "seed " << seed;
    EXPECT_GT(trials.at("racing_rejected").get<u64>(), 0U) << "seed " << seed;
    EXPECT_EQ(rows.size(), trials.at("distinct").get<usize>()) << "seed " << seed;
    EXPECT_EQ(registry.at("new_records"), registry.at("n_raw")) << "seed " << seed;
    EXPECT_GT(hurdle, 3.0);
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
    EXPECT_GE(copy->f1, hurdle) << "seed " << seed;
    EXPECT_LT(copy->f2, hurdle) << "seed " << seed;
    expect_campaign_line(cfg, campaign, out, "seed " + std::to_string(seed));
  }
}

// Same seed twice and at 1 and 4 workers: the same registry chain head, trial log and members.
// An existing registry reopens only against its exported head, and a rerun adds no trial.
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
  const auto status = st::run_mine(again, progress);
  ASSERT_TRUE(status) << status.error().to_string();
  const Json campaign = read_json(fs::path(again.output_directory) / "campaign.json");
  EXPECT_EQ(campaign.at("registry").at("new_records").get<u64>(), 0U);
  EXPECT_EQ(campaign.at("registry").at("head").get<std::string>(), heads[0]);
  EXPECT_FALSE(campaign.at("registry").at("anchor").is_null());
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
}
} // namespace
