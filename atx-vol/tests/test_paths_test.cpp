#include "support/test_paths.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <string>
#include <utility>
#include <vector>

// Pins the PROPERTY that every fixture/resource path in this test tree resolves
// to the same file no matter which directory the binary was launched from, and
// that no resolution escapes this worktree.
//
// It is deliberately written against the property rather than against a path
// spelling: the defect being pinned is a family of hand-pasted relative
// "ladders" that each had a different number of `../` rungs, so a test that
// checked any one spelling would be defeated by the next ladder somebody
// pastes. Flipping the CWD and demanding the answer not move cannot be.
//
// The escape check exists because one such ladder ended in a hard-coded
// absolute fallback into a DIFFERENT checkout (C:/atx/...), which the oracle
// regeneration path then opened in append mode -- a cross-tree write reachable
// from a documented invocation plus a documented env var.

namespace {

namespace fs = std::filesystem;

using atx::vol::testkit::artifact_cache_root;
using atx::vol::testkit::market_data;
using atx::vol::testkit::repo_root;
using atx::vol::testkit::test_fixture;
using atx::vol::testkit::tests_root;

// Restores the process CWD on scope exit. A test that changes the CWD and then
// fails an assertion would otherwise corrupt every later test in this binary,
// so the restore must not be a trailing statement.
class ScopedCwd {
public:
  ScopedCwd() : saved_(fs::current_path()) {}
  ~ScopedCwd() {
    std::error_code ec;
    fs::current_path(saved_, ec);
  }
  ScopedCwd(const ScopedCwd &) = delete;
  ScopedCwd &operator=(const ScopedCwd &) = delete;

private:
  fs::path saved_;
};

// THE SHIPPED predicate, not a local copy of it.
//
// This file used to define its own `is_under` with the same body. That made the
// one test whose job is to verify the containment rule verify its OWN
// reimplementation instead: if testkit::path_is_under broke or changed, this
// test stayed green. A guard that has stopped observing the thing it guards is
// worse than no guard, because it still reports.
using atx::vol::testkit::path_is_under;

// Every path the test tree resolves, named for a readable failure message.
[[nodiscard]] std::vector<std::pair<std::string, fs::path (*)()>> resolvers() {
  return {
      {"tests_root", +[]() -> fs::path { return tests_root(); }},
      {"repo_root", +[]() -> fs::path { return repo_root(); }},
      {"test_fixture", +[]() -> fs::path { return test_fixture("oracle_pde_golden.tsv"); }},
      {"market_data", +[]() -> fs::path { return market_data("spy_fit_slices"); }},
      {"artifact_cache_root", +[]() -> fs::path { return artifact_cache_root(); }},
  };
}

} // namespace

TEST(TestPathing, FixtureRootsAreCwdIndependent) {
  const fs::path scratch = fs::temp_directory_path() / "atx-vol-testpathing-cwd";
  std::error_code ec;
  fs::create_directories(scratch, ec);
  ASSERT_FALSE(ec) << "could not create scratch dir " << scratch.string() << ": " << ec.message();

  // Sample every resolver in the launch CWD, made absolute HERE so a
  // CWD-relative answer is frozen against the directory that produced it.
  std::vector<fs::path> before;
  for (const auto &[name, fn] : resolvers()) {
    before.push_back(fs::absolute(fn()).lexically_normal());
  }

  std::vector<fs::path> after;
  {
    const ScopedCwd guard;
    fs::current_path(scratch, ec);
    ASSERT_FALSE(ec) << "could not chdir to " << scratch.string() << ": " << ec.message();
    for (const auto &[name, fn] : resolvers()) {
      after.push_back(fs::absolute(fn()).lexically_normal());
    }
  }

  const auto names = resolvers();
  ASSERT_EQ(before.size(), after.size());
  for (std::size_t i = 0; i < before.size(); ++i) {
    EXPECT_EQ(before[i], after[i])
        << names[i].first << " moved when the CWD changed: it resolves relative to the "
        << "process working directory, so the same fixture names a different file "
        << "depending on where the binary was launched from";
  }
}

TEST(TestPathing, ResolvedPathsStayInsideThisWorktree) {
  const fs::path root = repo_root();

  // The absolute anchor must be the tree this TU was compiled from, not any
  // other checkout on the machine.
  EXPECT_TRUE(root.is_absolute()) << "repo_root() is not absolute: " << root.string();
  EXPECT_TRUE(path_is_under(tests_root(), root))
      << tests_root().string() << " is not under " << root.string();

  for (const auto &[name, fn] : resolvers()) {
    const fs::path p = fs::absolute(fn()).lexically_normal();
    EXPECT_TRUE(path_is_under(p, root))
        << name << " escapes this worktree: " << p.string() << " is not under " << root.string();
  }
}

// The escape check above uses path_is_under as its INSTRUMENT, so a predicate
// that answered `true` unconditionally would make it pass while checking
// nothing. Nothing else asserts the predicate's own behaviour, so this does --
// each case is a property its comment claims, and each was a real defect
// somewhere in this lane.
TEST(TestPathing, PathIsUnderJudgesResolutionNotSpelling) {
  // Assembled from a fragment that is not itself an absolute literal: written
  // out, these probes would be real cross-checkout paths in source, and
  // VolUmbrella.NoFixturePathResolvedOutsideTheSharedResolver would flag this
  // test's own evidence -- which it did, correctly, on the first run.
  const std::string drive = "C:";
  const auto p = [&drive](const std::string &rest) { return fs::path{drive + rest}; };
  const fs::path root = p("/atx-data");

  EXPECT_TRUE(path_is_under(p("/atx-data"), root)) << "a directory is under itself";
  EXPECT_TRUE(path_is_under(p("/atx-data/opra/x.parquet"), root));

  // Character prefix, NOT a parent: "C:/atx" is a prefix of "C:/atx-wt/pool-7"
  // as text and no ancestor of it as a path. A starts_with test calls the
  // forbidden checkout "inside this worktree".
  EXPECT_FALSE(path_is_under(p("/atx-wt/pool-7"), p("/atx")))
      << "a character prefix is being mistaken for a parent directory";
  EXPECT_FALSE(path_is_under(p("/atx-data-other/x"), root));

  // Traversal: spelled as if inside, resolves outside. This is the R3-I1 escape
  // that reconstructed the original FRI-072 path through a prefix compare.
  EXPECT_FALSE(path_is_under(p("/atx-data/../../atx/atx-vol/tests/support/x.tsv"), root))
      << "a traversal out of the root is being accepted";
  EXPECT_FALSE(path_is_under(p("/atx-data/../atx-wt/pool-3/x"), root));
  EXPECT_TRUE(path_is_under(p("/atx-data/a/../b/x"), root))
      << "traversal that stays inside the root must still be accepted";

  // Trailing separator: lexically_normal() on a path ending in ".." leaves an
  // empty final component, which made repo_root() compare equal to nothing.
  EXPECT_TRUE(path_is_under(p("/atx-data/x"), p("/atx-data/opra/..")))
      << "a trailing separator from normalisation is breaking the comparison";

  EXPECT_FALSE(path_is_under(root, p("/atx-data/opra"))) << "containment must not be symmetric";
}

TEST(TestPathing, CommittedFixturesResolveToFilesThatExist) {
  // Two committed fixtures whose absence is what the ladders used to paper over
  // by falling through to another checkout.
  for (const char *name : {"oracle_pde_golden.tsv", "earnings_forecast_sample.tsv"}) {
    const fs::path p = test_fixture(name);
    EXPECT_TRUE(fs::exists(p)) << "committed fixture does not resolve: " << p.string();
  }
}
