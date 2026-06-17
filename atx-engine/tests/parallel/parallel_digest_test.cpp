#include <gtest/gtest.h>

#include <cmath>
#include <utility>
#include <vector>

#include "atx/engine/alpha/panel.hpp"
#include "atx/engine/parallel/digest.hpp"

namespace atxtest_parallel_digest_test {

using atx::engine::alpha::SignalSet;
using atx::engine::parallel::signal_set_digest;

static SignalSet make_fixture() {
  SignalSet ss;
  ss.dates = 2;
  ss.instruments = 3;
  ss.alphas.push_back({"alpha0", {0.0, 1.5, -2.25, 3.125, 4.0, -5.5}});
  return ss;
}

static SignalSet make_two_alpha_fixture() {
  SignalSet ss = make_fixture();
  ss.alphas.push_back({"alpha1", {9.0, 8.0, 7.0, 6.0, 5.0, 4.0}});
  return ss;
}

TEST(ParallelDigest, IdenticalSignalSetsHashEqual) {
  SignalSet a = make_fixture(), b = a;
  EXPECT_EQ(signal_set_digest(a), signal_set_digest(b));
}

TEST(ParallelDigest, OneUlpDifferenceFlipsDigest) {
  SignalSet a = make_fixture(), b = a;
  b.alphas[0].values[3] = std::nextafter(b.alphas[0].values[3], 1e9); // 1 ULP
  EXPECT_NE(signal_set_digest(a), signal_set_digest(b));
}

TEST(ParallelDigest, RootOrderMatters) {
  SignalSet a = make_two_alpha_fixture(), b = a;
  std::swap(b.alphas[0], b.alphas[1]);
  EXPECT_NE(signal_set_digest(a), signal_set_digest(b));
}

TEST(ParallelDigest, NameMatters) {
  SignalSet a = make_fixture(), b = a;
  b.alphas[0].name = "renamed";
  EXPECT_NE(signal_set_digest(a), signal_set_digest(b));
}

TEST(ParallelDigest, ShapeMatters) {
  SignalSet a = make_fixture(), b = a;
  b.dates = 3;
  b.instruments = 2; // same value count, different shape
  EXPECT_NE(signal_set_digest(a), signal_set_digest(b));
}


}  // namespace atxtest_parallel_digest_test
