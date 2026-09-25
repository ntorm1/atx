// W0-E0b (trial accounting): TrialRegistry windows and metadata (findings
// E-16), configuration counting (L-08, registry side), the V1 log format kept
// readable, the tamper-evident chain head exported outside the log and the
// multi-writer lock (the status-table "L4" gaps). Suites EvalRegistryWindows_*.
#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include "atx/core/error.hpp"
#include "atx/core/types.hpp"
#include "atx/engine/eval/trial_registry.hpp"

namespace atx_test_w0_e0b_registry_windows {

using namespace atx::engine::eval;
using atx::f64;
using atx::u64;
using atx::u8;
using atx::usize;
using atx::core::ErrorCode;

class Gauss {
public:
  explicit Gauss(u64 seed) : s_{seed} {}
  f64 next() {
    const f64 u1 = (static_cast<f64>(mix() >> 11U) + 0.5) * 0x1.0p-53;
    const f64 u2 = (static_cast<f64>(mix() >> 11U) + 0.5) * 0x1.0p-53;
    return std::sqrt(-2.0 * std::log(u1)) * std::cos(6.283185307179586 * u2);
  }

private:
  u64 mix() {
    u64 z = (s_ += 0x9e3779b97f4a7c15ULL);
    z = (z ^ (z >> 30U)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27U)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31U);
  }
  u64 s_;
};

std::vector<f64> noise(usize t, u64 seed) {
  Gauss g{seed};
  std::vector<f64> v(t);
  for (f64 &x : v) {
    x = 0.01 * g.next();
  }
  return v;
}

f64 sharpe_of(std::span<const f64> x) {
  f64 m = 0.0;
  for (const f64 v : x) {
    m += v;
  }
  m /= static_cast<f64>(x.size());
  f64 ss = 0.0;
  for (const f64 v : x) {
    ss += (v - m) * (v - m);
  }
  return m / std::sqrt(ss / static_cast<f64>(x.size() - 1U));
}

std::filesystem::path fresh_path(const std::string &name) {
  const std::filesystem::path p =
      std::filesystem::temp_directory_path() / ("atx_w0e0b_registry_" + name + ".bin");
  std::error_code ec;
  std::filesystem::remove(p, ec);
  return p;
}

void remove_file(const std::filesystem::path &p) {
  std::error_code ec;
  std::filesystem::remove(p, ec);
}

TrialMeta window(u64 a, u64 b, TrialSample s = TrialSample::Unspecified, u8 fidelity = 0U,
                 u64 family = 0U, u64 theme = 0U) {
  TrialMeta m;
  m.window_start = a;
  m.window_end = b;
  m.sample = s;
  m.fidelity = fidelity;
  m.family_tag = family;
  m.theme_tag = theme;
  return m;
}

TrialRegistryConfig config(usize c, usize d) {
  TrialRegistryConfig cfg;
  cfg.pnl_len = c;
  cfg.sketch_dim = d;
  return cfg;
}

TrialRegistry must_mem(const TrialRegistryConfig &cfg) {
  auto r = TrialRegistry::in_memory(cfg);
  EXPECT_TRUE(r.has_value());
  return std::move(*r);
}

// The log's own stable digest (FNV-1a 64 + splitmix64 finalizer), re-derived
// here so a test can forge a record whose checksum still verifies.
u64 stable_digest(const unsigned char *p, usize n) {
  u64 h = 0xcbf29ce484222325ULL;
  for (usize i = 0; i < n; ++i) {
    h ^= static_cast<u64>(p[i]);
    h *= 0x100000001b3ULL;
  }
  u64 z = h + 0x9e3779b97f4a7c15ULL;
  z = (z ^ (z >> 30U)) * 0xbf58476d1ce4e5b9ULL;
  z = (z ^ (z >> 27U)) * 0x94d049bb133111ebULL;
  return z ^ (z >> 31U);
}

std::vector<unsigned char> read_all(const std::filesystem::path &p) {
  std::ifstream in(p, std::ios::binary);
  return std::vector<unsigned char>((std::istreambuf_iterator<char>(in)),
                                    std::istreambuf_iterator<char>());
}

void write_all(const std::filesystem::path &p, const std::vector<unsigned char> &bytes) {
  std::ofstream out(p, std::ios::binary | std::ios::trunc);
  out.write(reinterpret_cast<const char *>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
}

// ---------------------------------------------------------------------------
//  E-16: windows and metadata
// ---------------------------------------------------------------------------
TEST(EvalRegistryWindows_Record, VariableWindowsShareOneCalendar) {
  TrialRegistry reg = must_mem(config(300U, 512U));
  const u64 mom = trial_tag("momentum");
  const u64 price = trial_tag("price");
  const std::array<TrialMeta, 3> metas{
      window(0U, 99U, TrialSample::InSample, 1U, mom, price),
      window(100U, 299U, TrialSample::OutOfSample, 0U, mom, price),
      window(50U, 249U),
  };
  for (usize i = 0; i < metas.size(); ++i) {
    const usize len = static_cast<usize>(metas[i].window_end - metas[i].window_start + 1U);
    const std::vector<f64> x = noise(len, 10U + i);
    const auto r = reg.record(TrialKind::MinerExpr, 100U + i, metas[i], x, sharpe_of(x));
    ASSERT_TRUE(r.has_value()) << r.error().to_string();
    EXPECT_TRUE(r->inserted);
  }
  ASSERT_EQ(reg.trials().size(), 3U);
  for (usize i = 0; i < metas.size(); ++i) {
    const TrialInfo &t = reg.trials()[i];
    EXPECT_EQ(t.meta, metas[i]);
    EXPECT_EQ(t.config_hash, 100U + i);
    EXPECT_EQ(t.kind, TrialKind::MinerExpr);
    EXPECT_EQ(t.id, trial_id(TrialKind::MinerExpr, 100U + i));
  }
  const TrialSummary s = reg.summary();
  EXPECT_EQ(s.n_raw, 3U);
  EXPECT_EQ(s.pnl_len, 300U);
  EXPECT_EQ(s.n_in_sample, 1U);
  EXPECT_EQ(s.n_out_of_sample, 1U);
  EXPECT_EQ(s.n_unspecified, 1U);

  // Every malformed window is refused and nothing is recorded.
  const std::vector<f64> x100 = noise(100U, 1U);
  const auto expect_invalid = [&](const TrialMeta &m, std::span<const f64> pnl) {
    const auto r = reg.record(TrialKind::MinerExpr, 999U, m, pnl, 0.1);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code(), ErrorCode::InvalidArgument);
  };
  expect_invalid(window(0U, 98U), x100);                          // length mismatch
  expect_invalid(window(250U, 349U), x100);                       // past the calendar
  expect_invalid(window(10U, 5U), x100);                          // start > end
  expect_invalid(window(0U, 1U), std::span<const f64>(x100).first(2U)); // < 3 periods
  TrialMeta bad_sample = window(0U, 99U);
  bad_sample.sample = static_cast<TrialSample>(7U);
  expect_invalid(bad_sample, x100);
  EXPECT_EQ(reg.size(), 3U);

  EXPECT_EQ(trial_tag(""), 0U);
  EXPECT_NE(trial_tag("momentum"), trial_tag("value"));
  EXPECT_EQ(trial_tag("momentum"), mom);
}

TEST(EvalRegistryWindows_Record, LegacyOverloadIsTheFullCalendarWindow) {
  const usize c = 120U;
  TrialRegistry legacy = must_mem(config(c, 256U));
  TrialRegistry explicit_window = must_mem(config(c, 256U));
  for (usize i = 0; i < 20U; ++i) {
    const std::vector<f64> x = noise(c, 40U + i);
    ASSERT_TRUE(legacy.record(TrialKind::RegimeCount, i, x, sharpe_of(x)).has_value());
    ASSERT_TRUE(explicit_window
                    .record(TrialKind::RegimeCount, i, window(0U, c - 1U), x, sharpe_of(x))
                    .has_value());
  }
  EXPECT_EQ(legacy.trials()[0].meta, window(0U, c - 1U));
  const TrialSummary a = legacy.summary();
  const TrialSummary b = explicit_window.summary();
  EXPECT_EQ(a.n_eff, b.n_eff);
  EXPECT_EQ(a.registry_hash, b.registry_hash);
  EXPECT_EQ(legacy.chain_head(), explicit_window.chain_head());
}

TEST(EvalRegistryWindows_Record, MetadataRoundTripsThroughTheV2Log) {
  const std::filesystem::path path = fresh_path("v2_roundtrip");
  const TrialRegistryConfig cfg = config(200U, 64U); // count-sketch mode
  std::vector<TrialInfo> before;
  TrialChainHead head;
  TrialSummary sum;
  {
    auto reg = TrialRegistry::open(path, cfg);
    ASSERT_TRUE(reg.has_value()) << reg.error().to_string();
    EXPECT_EQ(reg->format(), TrialLogFormat::V2);
    for (usize i = 0; i < 6U; ++i) {
      const u64 a = 10U * i;
      const u64 b = a + 100U + i;
      const TrialSample smp = i % 2U == 0U ? TrialSample::InSample : TrialSample::OutOfSample;
      const TrialMeta m = window(a, b, smp, static_cast<u8>(i),
                                 trial_tag("fam" + std::to_string(i % 3U)), trial_tag("theme"));
      const std::vector<f64> x = noise(static_cast<usize>(b - a + 1U), 70U + i);
      ASSERT_TRUE(reg->record(TrialKind::CombinerHyper, i, m, x, sharpe_of(x)).has_value());
    }
    before = reg->trials();
    head = reg->chain_head();
    sum = reg->summary();
  }
  const std::vector<unsigned char> bytes = read_all(path);
  ASSERT_GE(bytes.size(), 8U);
  EXPECT_EQ(std::string(bytes.begin(), bytes.begin() + 8), "ATXTRG02");
  EXPECT_EQ(bytes.size(), 48U + 6U * (72U + 8U * 64U));
  {
    auto reg = TrialRegistry::open(path, cfg);
    ASSERT_TRUE(reg.has_value()) << reg.error().to_string();
    EXPECT_EQ(reg->format(), TrialLogFormat::V2);
    ASSERT_EQ(reg->trials().size(), before.size());
    for (usize i = 0; i < before.size(); ++i) {
      EXPECT_EQ(reg->trials()[i].meta, before[i].meta);
      EXPECT_EQ(reg->trials()[i].id, before[i].id);
      EXPECT_EQ(reg->trials()[i].config_hash, before[i].config_hash);
      EXPECT_EQ(reg->trials()[i].sharpe, before[i].sharpe);
    }
    EXPECT_EQ(reg->chain_head(), head);
    const TrialSummary again = reg->summary();
    EXPECT_EQ(again.n_eff, sum.n_eff);
    EXPECT_EQ(again.registry_hash, sum.registry_hash);
    EXPECT_EQ(again.n_in_sample, 3U);
    EXPECT_EQ(again.n_out_of_sample, 3U);
  }
  remove_file(path);
}

// The pre-E-16 format stays readable and appendable, and reproduces the same
// numbers as the V2 in-memory registry for the same (full-window) history.
TEST(EvalRegistryWindows_Legacy, V1LogStillReadsAndAppends) {
  const std::filesystem::path path = fresh_path("v1_legacy");
  TrialRegistryConfig v1 = config(64U, 32U);
  v1.format = TrialLogFormat::V1;
  TrialRegistry mirror = must_mem(config(64U, 32U)); // default V2, in memory
  {
    auto reg = TrialRegistry::open(path, v1);
    ASSERT_TRUE(reg.has_value());
    EXPECT_EQ(reg->format(), TrialLogFormat::V1);
    for (usize i = 0; i < 5U; ++i) {
      const std::vector<f64> x = noise(64U, 300U + i);
      ASSERT_TRUE(reg->record(TrialKind::MinerExpr, i, x, sharpe_of(x)).has_value());
      ASSERT_TRUE(mirror.record(TrialKind::MinerExpr, i, x, sharpe_of(x)).has_value());
    }
    const std::vector<f64> y = noise(64U, 999U);
    const auto meta = reg->record(TrialKind::MinerExpr, 50U,
                                  window(0U, 63U, TrialSample::InSample), y, sharpe_of(y));
    ASSERT_FALSE(meta.has_value());
    EXPECT_EQ(meta.error().code(), ErrorCode::InvalidArgument);
  }
  const std::vector<unsigned char> bytes = read_all(path);
  EXPECT_EQ(std::string(bytes.begin(), bytes.begin() + 8), "ATXTRG01");
  EXPECT_EQ(bytes.size(), 48U + 5U * (40U + 8U * 32U));
  {
    // Reopened under the default (V2) config: the log keeps its own format.
    auto reg = TrialRegistry::open(path, config(64U, 32U));
    ASSERT_TRUE(reg.has_value()) << reg.error().to_string();
    EXPECT_EQ(reg->format(), TrialLogFormat::V1);
    EXPECT_EQ(reg->size(), 5U);
    const std::vector<f64> x = noise(64U, 305U);
    ASSERT_TRUE(reg->record(TrialKind::MinerExpr, 5U, x, sharpe_of(x)).has_value());
    ASSERT_TRUE(mirror.record(TrialKind::MinerExpr, 5U, x, sharpe_of(x)).has_value());
    const auto meta = reg->record(TrialKind::MinerExpr, 51U, window(0U, 10U), noise(11U, 1U), 0.1);
    EXPECT_FALSE(meta.has_value());
  }
  {
    auto reg = TrialRegistry::open(path, config(64U, 32U));
    ASSERT_TRUE(reg.has_value());
    EXPECT_EQ(reg->size(), 6U);
    const TrialSummary a = reg->summary();
    const TrialSummary b = mirror.summary();
    EXPECT_EQ(a.registry_hash, b.registry_hash); // (id, sharpe) chain unchanged since V1
    EXPECT_EQ(a.n_eff, b.n_eff);
    EXPECT_EQ(a.var_sr, b.var_sr);
    EXPECT_EQ(a.n_unspecified, 6U);
  }
  remove_file(path);
}

// ---------------------------------------------------------------------------
//  n_eff with partial windows
// ---------------------------------------------------------------------------
TEST(EvalRegistryWindows_Neff, PartialWindowsKeepTheBiasCorrection) {
  const usize c = 400U;
  {
    TrialRegistry reg = must_mem(config(c, 512U));
    for (usize i = 0; i < 40U; ++i) {
      const u64 a = (i * 37U) % 150U;
      const u64 b = a + 199U + (i * 13U) % 50U;
      const std::vector<f64> x = noise(static_cast<usize>(b - a + 1U), 500U + i);
      ASSERT_TRUE(reg.record(TrialKind::MinerExpr, i, window(a, b), x, sharpe_of(x)).has_value());
    }
    std::printf("[w0e0b] 40 independent trials on staggered windows: n_eff=%.2f "
                "(uncorrected %.2f)\n",
                reg.summary().n_eff, reg.summary().n_eff_uncorrected);
    EXPECT_NEAR(reg.summary().n_eff, 40.0, 6.0);
    EXPECT_LT(reg.summary().n_eff_uncorrected, reg.summary().n_eff);
  }
  {
    TrialRegistry reg = must_mem(config(c, 512U));
    for (usize i = 0; i < 50U; ++i) {
      const std::vector<f64> x = noise(252U, 900U + i);
      ASSERT_TRUE(
          reg.record(TrialKind::MinerExpr, i, window(100U, 351U), x, sharpe_of(x)).has_value());
    }
    std::printf("[w0e0b] 50 independent trials on one partial window: n_eff=%.2f\n",
                reg.summary().n_eff);
    EXPECT_NEAR(reg.summary().n_eff, 50.0, 5.0);
  }
  {
    TrialRegistry reg = must_mem(config(c, 512U));
    const std::vector<f64> base = noise(200U, 7U);
    for (usize i = 0; i < 30U; ++i) {
      std::vector<f64> y(base.size());
      for (usize s = 0; s < y.size(); ++s) {
        y[s] = base[s] * (1.0 + 0.1 * static_cast<f64>(i));
      }
      ASSERT_TRUE(
          reg.record(TrialKind::MinerExpr, i, window(100U, 299U), y, sharpe_of(y)).has_value());
    }
    EXPECT_NEAR(reg.summary().n_eff, 1.0, 1e-6);
  }
  {
    // One generating process on DISJOINT windows: disjoint evaluation windows
    // are independent draws of the selection, so they count fully.
    TrialRegistry reg = must_mem(config(c, 512U));
    const std::vector<f64> series = noise(c, 8U);
    for (usize i = 0; i < 8U; ++i) {
      const u64 a = 50U * i;
      const std::span<const f64> x = std::span<const f64>(series).subspan(a, 50U);
      ASSERT_TRUE(
          reg.record(TrialKind::MinerExpr, i, window(a, a + 49U), x, sharpe_of(x)).has_value());
    }
    EXPECT_NEAR(reg.summary().n_eff, 8.0, 1e-9);
  }
}

// ---------------------------------------------------------------------------
//  L-08 (registry side): the registry counts CONFIGURATIONS, not folds.
// ---------------------------------------------------------------------------
TEST(EvalRegistryWindows_Counting, FoldsOfOneConfigurationCountOnce) {
  TrialRegistry reg = must_mem(config(250U, 256U));
  for (usize cfg_i = 0; cfg_i < 3U; ++cfg_i) {
    for (usize fold = 0; fold < 5U; ++fold) {
      const u64 a = 50U * fold;
      const std::vector<f64> x = noise(50U, 1000U * cfg_i + fold);
      const auto r = reg.record(TrialKind::StackHyper, 7000U + cfg_i,
                                window(a, a + 49U, TrialSample::OutOfSample), x, sharpe_of(x));
      ASSERT_TRUE(r.has_value());
      EXPECT_EQ(r->inserted, fold == 0U) << "config " << cfg_i << " fold " << fold;
    }
  }
  EXPECT_EQ(reg.size(), 3U);
  EXPECT_EQ(reg.summary().n_raw, 3U);
  EXPECT_EQ(reg.trials()[1].meta.window_start, 0U); // the first fold's record is kept
}

// ---------------------------------------------------------------------------
//  Tamper-evident chain head exported outside the log.
// ---------------------------------------------------------------------------
TEST(EvalRegistryWindows_ChainHead, AnchorDetectsRemovedAndEditedRecords) {
  const std::filesystem::path path = fresh_path("anchor");
  std::filesystem::path sidecar = path;
  sidecar += ".head";
  remove_file(sidecar);
  const TrialRegistryConfig cfg = config(16U, 256U); // exact, record = 72 + 8*16 bytes
  const usize rb = 72U + 8U * 16U;
  TrialChainHead head10;
  {
    auto reg = TrialRegistry::open(path, cfg);
    ASSERT_TRUE(reg.has_value());
    EXPECT_EQ(reg->chain_head().records, 0U);
    for (usize i = 0; i < 10U; ++i) {
      const f64 sr = 0.01 * static_cast<f64>(i);
      ASSERT_TRUE(reg->record(TrialKind::MinerExpr, i, noise(16U, 40U + i), sr).has_value());
    }
    head10 = reg->chain_head();
    EXPECT_EQ(head10.records, 10U);
  }
  ASSERT_TRUE(write_chain_head(sidecar, head10).has_value());
  auto loaded = read_chain_head(sidecar);
  ASSERT_TRUE(loaded.has_value()) << loaded.error().to_string();
  EXPECT_EQ(*loaded, head10);
  {
    // Matching log: accepted; records appended after the anchor are fine.
    auto reg = TrialRegistry::open(path, cfg, *loaded);
    ASSERT_TRUE(reg.has_value()) << reg.error().to_string();
    for (usize i = 10; i < 12U; ++i) {
      ASSERT_TRUE(reg->record(TrialKind::MinerExpr, i, noise(16U, 40U + i), 0.2).has_value());
    }
  }
  {
    auto reg = TrialRegistry::open(path, cfg, head10);
    ASSERT_TRUE(reg.has_value());
    EXPECT_EQ(reg->size(), 12U);
  }
  // Remove the last 3 whole records: the log alone cannot tell ...
  std::filesystem::resize_file(path, 48U + 9U * rb);
  {
    auto reg = TrialRegistry::open(path, cfg);
    ASSERT_TRUE(reg.has_value());
    EXPECT_EQ(reg->size(), 9U);
  }
  // ... but the exported head can, and the file is left untouched.
  {
    auto reg = TrialRegistry::open(path, cfg, head10);
    ASSERT_FALSE(reg.has_value());
    EXPECT_EQ(reg.error().code(), ErrorCode::ParseError);
  }
  EXPECT_EQ(std::filesystem::file_size(path), 48U + 9U * rb);
  remove_file(path);

  // Edit a record and re-forge its checksum: invisible to the log's own
  // checks, caught by the anchored chain.
  TrialChainHead head_edit;
  {
    auto reg = TrialRegistry::open(path, cfg);
    ASSERT_TRUE(reg.has_value());
    for (usize i = 0; i < 10U; ++i) {
      ASSERT_TRUE(reg->record(TrialKind::MinerExpr, i, noise(16U, 60U + i), 0.01).has_value());
    }
    head_edit = reg->chain_head();
  }
  std::vector<unsigned char> bytes = read_all(path);
  unsigned char *rec = bytes.data() + 48U + 4U * rb; // record 5
  f64 sharpe = 0.0;
  std::memcpy(&sharpe, rec + 16U, sizeof(sharpe));
  sharpe = 3.0; // an inflated Sharpe
  std::memcpy(rec + 16U, &sharpe, sizeof(sharpe));
  const u64 sum = stable_digest(rec, rb - 8U);
  std::memcpy(rec + rb - 8U, &sum, sizeof(sum));
  write_all(path, bytes);
  {
    auto reg = TrialRegistry::open(path, cfg);
    ASSERT_TRUE(reg.has_value()) << "a forged record passes the per-record checksum";
    EXPECT_EQ(reg->summary().max_sr, 3.0);
  }
  {
    auto reg = TrialRegistry::open(path, cfg, head_edit);
    ASSERT_FALSE(reg.has_value());
    EXPECT_EQ(reg.error().code(), ErrorCode::ParseError);
  }
  remove_file(path);

  // A fresh log matches only the empty head.
  {
    auto reg = TrialRegistry::open(path, cfg, head10);
    ASSERT_FALSE(reg.has_value());
  }
  remove_file(path);
  TrialChainHead empty_head;
  {
    auto reg = TrialRegistry::in_memory(cfg);
    ASSERT_TRUE(reg.has_value());
    empty_head = reg->chain_head();
  }
  {
    auto reg = TrialRegistry::open(path, cfg, empty_head);
    ASSERT_TRUE(reg.has_value()) << reg.error().to_string();
  }
  remove_file(path);

  // The sidecar codec rejects damage.
  {
    std::ofstream out(sidecar, std::ios::binary | std::ios::trunc);
    out << "ATXTRGH1 a 1f 0\n";
  }
  auto bad = read_chain_head(sidecar);
  ASSERT_FALSE(bad.has_value());
  EXPECT_EQ(bad.error().code(), ErrorCode::ParseError);
  remove_file(sidecar);
  EXPECT_FALSE(read_chain_head(sidecar).has_value());
}

// ---------------------------------------------------------------------------
//  Multiple writers on one log.
// ---------------------------------------------------------------------------
TEST(EvalRegistryWindows_MultiWriter, TwoHandlesNeverDoubleCount) {
  const std::filesystem::path path = fresh_path("two_handles");
  const TrialRegistryConfig cfg = config(32U, 256U);
  {
    auto a = TrialRegistry::open(path, cfg);
    auto b = TrialRegistry::open(path, cfg);
    ASSERT_TRUE(a.has_value() && b.has_value());
    for (usize i = 0; i < 10U; ++i) {
      ASSERT_TRUE(a->record(TrialKind::MinerExpr, i, noise(32U, i), 0.1).has_value());
    }
    for (usize i = 5; i < 15U; ++i) {
      const auto r = b->record(TrialKind::MinerExpr, i, noise(32U, i), 0.1);
      ASSERT_TRUE(r.has_value()) << r.error().to_string();
      EXPECT_EQ(r->inserted, i >= 10U) << "config " << i;
    }
    EXPECT_EQ(b->size(), 15U);
    const auto got = a->refresh();
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(*got, 5U);
    EXPECT_EQ(a->size(), 15U);
    EXPECT_EQ(a->chain_head(), b->chain_head());
    EXPECT_EQ(a->summary().registry_hash, b->summary().registry_hash);
  }
  EXPECT_EQ(std::filesystem::file_size(path), 48U + 15U * (72U + 8U * 32U));
  {
    auto c = TrialRegistry::open(path, cfg);
    ASSERT_TRUE(c.has_value());
    EXPECT_EQ(c->size(), 15U);
  }
  remove_file(path);
}

TEST(EvalRegistryWindows_MultiWriter, ConcurrentThreadsProduceOneConsistentLog) {
  const std::filesystem::path path = fresh_path("threads");
  const TrialRegistryConfig cfg = config(48U, 256U);
  constexpr usize kThreads = 4U;
  constexpr usize kPer = 25U;
  constexpr usize kStride = 15U; // consecutive threads share 10 configurations
  std::vector<TrialRegistry> handles;
  for (usize t = 0; t < kThreads; ++t) {
    auto h = TrialRegistry::open(path, cfg);
    ASSERT_TRUE(h.has_value());
    handles.push_back(std::move(*h));
  }
  std::array<usize, kThreads> inserted{};
  std::array<usize, kThreads> failures{};
  std::vector<std::thread> threads;
  for (usize t = 0; t < kThreads; ++t) {
    threads.emplace_back([&, t] {
      for (usize k = 0; k < kPer; ++k) {
        const usize cfg_hash = t * kStride + k;
        const auto r =
            handles[t].record(TrialKind::MinerExpr, cfg_hash, noise(48U, cfg_hash), 0.05);
        if (!r.has_value()) {
          ++failures[t];
        } else if (r->inserted) {
          ++inserted[t];
        }
      }
    });
  }
  for (std::thread &th : threads) {
    th.join();
  }
  const usize distinct = (kThreads - 1U) * kStride + kPer; // 70
  usize total = 0U;
  for (usize t = 0; t < kThreads; ++t) {
    EXPECT_EQ(failures[t], 0U);
    total += inserted[t];
  }
  EXPECT_EQ(total, distinct);
  for (TrialRegistry &h : handles) {
    ASSERT_TRUE(h.refresh().has_value());
    EXPECT_EQ(h.size(), distinct);
    EXPECT_EQ(h.chain_head(), handles[0].chain_head());
  }
  const TrialChainHead head = handles[0].chain_head();
  handles.clear();
  EXPECT_EQ(std::filesystem::file_size(path), 48U + distinct * (72U + 8U * 48U));
  {
    auto reopened = TrialRegistry::open(path, cfg, head);
    ASSERT_TRUE(reopened.has_value()) << reopened.error().to_string();
    EXPECT_EQ(reopened->size(), distinct);
  }
  remove_file(path);
}

// Memory (review finding, fix pass 1): with keep_sketches (the default) a
// registry retains 8·d sketch bytes plus one TrialInfo per trial. A 10^6-trial
// caller sets keep_sketches = false and keeps the summary, the chain head and
// trials() bit-for-bit; only correlation / mc_max_null / accounting need the
// sketches.
TEST(EvalRegistryWindows_Memory, LeanRegistryKeepsTheSummaryBitForBit) {
  const usize t = 120U;
  TrialRegistryConfig full_cfg;
  full_cfg.pnl_len = t;
  full_cfg.sketch_dim = 64U;
  TrialRegistryConfig lean_cfg = full_cfg;
  lean_cfg.keep_sketches = false;
  auto full = TrialRegistry::in_memory(full_cfg);
  auto lean = TrialRegistry::in_memory(lean_cfg);
  ASSERT_TRUE(full.has_value() && lean.has_value());
  for (u64 i = 0; i < 50U; ++i) {
    const std::vector<f64> x = noise(t, 9000U + i);
    ASSERT_TRUE(full->record(TrialKind::MinerExpr, i, x, sharpe_of(x)).has_value());
    ASSERT_TRUE(lean->record(TrialKind::MinerExpr, i, x, sharpe_of(x)).has_value());
  }
  const TrialSummary a = full->summary();
  const TrialSummary b = lean->summary();
  EXPECT_EQ(a.n_raw, b.n_raw);
  EXPECT_EQ(a.n_eff, b.n_eff);
  EXPECT_EQ(a.n_eff_uncorrected, b.n_eff_uncorrected);
  EXPECT_EQ(a.var_sr, b.var_sr);
  EXPECT_EQ(a.registry_hash, b.registry_hash);
  EXPECT_EQ(full->chain_head(), lean->chain_head());
  ASSERT_EQ(full->trials().size(), lean->trials().size());
  EXPECT_TRUE(full->correlation().has_value());
  EXPECT_FALSE(lean->correlation().has_value());
  EXPECT_FALSE(lean->accounting(TrialAccountingConfig{}).has_value());
  std::printf("[w0e0b] per-trial retained bytes: TrialInfo=%zu, sketch=8*d (d=64 -> %zu); "
              "10^6 trials at d=64 ~ %.2f GB with sketches, %.2f GB without\n",
              sizeof(TrialInfo), static_cast<usize>(8U * 64U),
              1e6 * static_cast<f64>(sizeof(TrialInfo) + 8U * 64U) / 1e9,
              1e6 * static_cast<f64>(sizeof(TrialInfo)) / 1e9);
}

} // namespace atx_test_w0_e0b_registry_windows
