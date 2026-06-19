// store_fingerprint_test.cpp — run fingerprint determinism + replay lookup (Task 4).
#include <gtest/gtest.h>
#include "atx/engine/store/fingerprint.hpp"

namespace atxtest_store_fingerprint_test {
using atx::engine::store::RunInputs;
namespace fp = atx::engine::store::fingerprint;

RunInputs sample() {
  return RunInputs{"sha_abc", "cfg{mode:long}", 0xAAAAull, 0xBBBBull, 7, "gates{pbo<0.5}"};
}

TEST(Fingerprint, IdenticalInputsSameHash) {
  EXPECT_EQ(fp::compute(sample()), fp::compute(sample()));
}

TEST(Fingerprint, PerturbedInputDiffersOnEveryField) {
  const atx::u64 base = fp::compute(sample());
  { auto x = sample(); x.engine_git_sha = "sha_xyz";       EXPECT_NE(base, fp::compute(x)); }
  { auto x = sample(); x.config_normalized = "cfg{mode:short}"; EXPECT_NE(base, fp::compute(x)); }
  { auto x = sample(); x.universe_content_hash = 0xCCCCull;  EXPECT_NE(base, fp::compute(x)); }
  { auto x = sample(); x.snapshot_content_hash = 0xDDDDull;  EXPECT_NE(base, fp::compute(x)); }
  { auto x = sample(); x.master_seed = 8;                    EXPECT_NE(base, fp::compute(x)); }
  { auto x = sample(); x.gate_config = "gates{pbo<0.4}";     EXPECT_NE(base, fp::compute(x)); }
}

}  // namespace atxtest_store_fingerprint_test
