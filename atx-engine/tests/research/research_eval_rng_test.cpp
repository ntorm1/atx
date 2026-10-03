// Platform P9 B2 (P11, G-P7): numpy's default_rng in the engine draws the house statistics'
// randomness exactly. T1's statistics tie fixture (fixtures/eval_tie) records the paired dSR's
// circular block bootstrap starts (cbb_starts.u16 = default_rng(20260929).integers(0, 500,
// (4999, 24))) and every k-means++ pick of the house ONC (default_rng(20260927)); the engine's
// generator reproduces both, and the recorded picks replayed through KmeansDraws give the
// recorded clusters.

#include <algorithm>
#include <cmath>
#include <cstring>
#include <span>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "atx/core/types.hpp"
#include "atx/engine/research/eval/numpy_rng.hpp"
#include "atx/engine/research/eval/onc_house.hpp"

namespace {

namespace fs = std::filesystem;
namespace ev = atx::engine::research::eval;
using atx::f64;
using atx::u16;
using atx::u64;
using atx::usize;
using Json = nlohmann::json;

constexpr usize kT = 500;
constexpr usize kK = 12;
constexpr usize kDraws = 4999;
constexpr usize kBlocks = 24;

std::string read_bytes(const fs::path &p) {
  std::ifstream f(p, std::ios::binary);
  return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

const Json &expected() {
  static const Json doc = Json::parse(read_bytes(fs::path(ATX_EVAL_TIE_FIXTURE) / "expected.json"));
  return doc;
}

template <class T> std::vector<T> read_array(const char *name, usize count) {
  const std::string bytes = read_bytes(fs::path(ATX_EVAL_TIE_FIXTURE) / name);
  EXPECT_EQ(bytes.size(), count * sizeof(T)) << name;
  std::vector<T> out(count);
  std::memcpy(out.data(), bytes.data(), std::min(bytes.size(), count * sizeof(T)));
  return out; // little-endian host (x64)
}

struct Pick {
  std::string kind;
  usize n{};
  usize value{};
};

std::vector<Pick> recorded_picks() {
  std::vector<Pick> out;
  for (const Json &p : expected()["inputs"]["onc_picks"]["picks"]) {
    out.push_back(Pick{p[0].get<std::string>(), p[1].get<usize>(), p[2].get<usize>()});
  }
  return out;
}

// Records every draw of numpy's generator, in call order.
class RecordingDraws final : public ev::KmeansDraws {
public:
  explicit RecordingDraws(u64 seed) : inner_(seed) {}
  [[nodiscard]] atx::core::Result<usize> integers(usize n) override {
    auto v = inner_.integers(n);
    if (v) {
      picks.push_back(Pick{"integers", n, *v});
    }
    return v;
  }
  [[nodiscard]] atx::core::Result<usize> choice(std::span<const f64> p) override {
    auto v = inner_.choice(p);
    if (v) {
      picks.push_back(Pick{"choice", p.size(), *v});
    }
    return v;
  }
  std::vector<Pick> picks;

private:
  ev::NumpyKmeansDraws inner_;
};

// Feeds the recorded picks back; a call that differs from the record is an error.
class ReplayDraws final : public ev::KmeansDraws {
public:
  explicit ReplayDraws(std::vector<Pick> picks) : picks_(std::move(picks)) {}
  [[nodiscard]] atx::core::Result<usize> integers(usize n) override {
    return next("integers", n);
  }
  [[nodiscard]] atx::core::Result<usize> choice(std::span<const f64> p) override {
    return next("choice", p.size());
  }
  usize used{};

private:
  atx::core::Result<usize> next(const char *kind, usize n) {
    if (used >= picks_.size() || picks_[used].kind != kind || picks_[used].n != n) {
      return atx::core::Err(atx::core::ErrorCode::Internal,
                            "replay: call " + std::to_string(used) + " differs from the record");
    }
    return atx::core::Ok(picks_[used++].value);
  }
  std::vector<Pick> picks_;
};

std::vector<f64> series() { return read_array<f64>("series.f64", kT * kK); }

std::vector<std::vector<std::string>> named(const std::vector<std::vector<usize>> &clusters) {
  std::vector<std::vector<std::string>> out;
  for (const auto &cl : clusters) {
    std::vector<std::string> names;
    for (const usize k : cl) {
      names.push_back((k < 10U ? "trial_0" : "trial_") + std::to_string(k));
    }
    out.push_back(std::move(names));
  }
  return out;
}

TEST(ResearchEvalRng, CbbStartsAreDefaultRngIntegers) {
  const std::vector<u16> starts = read_array<u16>("cbb_starts.u16", kDraws * kBlocks);
  ev::NumpyGenerator g(20260929U);
  usize mismatches = 0;
  for (const u16 want : starts) {
    mismatches += g.integers(kT) == want ? 0U : 1U;
  }
  EXPECT_EQ(mismatches, 0U);
}

TEST(ResearchEvalRng, OncDrawsAreTheRecordedPicks) {
  const std::vector<f64> x = series();
  const std::vector<f64> corr = ev::trial_correlation(x, kT, kK);
  RecordingDraws draws(20260927U);
  const auto part = ev::onc_house_v1(corr, kK, 10U, draws);
  ASSERT_TRUE(part.has_value()) << part.error().to_string();
  const std::vector<Pick> want = recorded_picks();
  ASSERT_EQ(draws.picks.size(), want.size());
  for (usize i = 0; i < want.size(); ++i) {
    ASSERT_EQ(draws.picks[i].kind, want[i].kind) << i;
    ASSERT_EQ(draws.picks[i].n, want[i].n) << i;
    ASSERT_EQ(draws.picks[i].value, want[i].value) << i;
  }
  EXPECT_EQ(Json(named(part->clusters)), expected()["values"]["onc"]["clusters"]);
}

TEST(ResearchEvalRng, ReplayedPicksGiveTheRecordedClustersAndSilhouettes) {
  const std::vector<f64> x = series();
  const std::vector<f64> corr = ev::trial_correlation(x, kT, kK);
  ReplayDraws draws(recorded_picks());
  const auto part = ev::onc_house_v1(corr, kK, 10U, draws);
  ASSERT_TRUE(part.has_value()) << part.error().to_string();
  EXPECT_EQ(draws.used, recorded_picks().size());
  const Json &replay = expected()["values"]["onc_replay"];
  EXPECT_EQ(Json(named(part->clusters)), replay["clusters"]);
  ASSERT_EQ(part->silhouettes.size(), kK);
  for (usize k = 0; k < kK; ++k) {
    const f64 want = replay["silhouettes"][k].get<f64>();
    EXPECT_LE(std::fabs(part->silhouettes[k] - want), 1e-12 * std::fmax(std::fabs(want), 1e-300))
        << k;
  }
}

} // namespace
