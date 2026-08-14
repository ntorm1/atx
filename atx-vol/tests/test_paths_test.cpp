#include "support/test_paths.hpp"

#include <gtest/gtest.h>

#include <algorithm>
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

// True when `child` is `parent` or lies beneath it. Compares path COMPONENTS,
// not characters: "C:/atx" is a character-prefix of "C:/atx-wt/pool-7" but is
// not a parent of it, and that is exactly the confusion this guards -- the
// forbidden checkout and this worktree differ first at a component boundary a
// string prefix test would sail straight past.
[[nodiscard]] bool is_under(const fs::path &child, const fs::path &parent) {
  // Drop the empty final component a trailing separator leaves behind, so
  // "C:/wt/pool/" and "C:/wt/pool" compare as the same directory.
  const auto trim = [](const fs::path &raw) {
    const fs::path n = raw.lexically_normal();
    return n.filename().empty() ? n.parent_path() : n;
  };
  const fs::path c = trim(child);
  const fs::path p = trim(parent);
  const auto it = std::mismatch(p.begin(), p.end(), c.begin(), c.end());
  return it.first == p.end();
}

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
  EXPECT_TRUE(is_under(tests_root(), root))
      << tests_root().string() << " is not under " << root.string();

  for (const auto &[name, fn] : resolvers()) {
    const fs::path p = fs::absolute(fn()).lexically_normal();
    EXPECT_TRUE(is_under(p, root)) << name << " escapes this worktree: " << p.string()
                                   << " is not under " << root.string();
  }
}

TEST(TestPathing, CommittedFixturesResolveToFilesThatExist) {
  // Two committed fixtures whose absence is what the ladders used to paper over
  // by falling through to another checkout.
  for (const char *name : {"oracle_pde_golden.tsv", "earnings_forecast_sample.tsv"}) {
    const fs::path p = test_fixture(name);
    EXPECT_TRUE(fs::exists(p)) << "committed fixture does not resolve: " << p.string();
  }
}
