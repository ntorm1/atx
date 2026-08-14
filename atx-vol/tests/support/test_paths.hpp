#pragma once

// THE fixture/resource path resolver for the atx-vol test tree. Every fixture,
// golden table, market-data file and cache directory this tree reaches is
// resolved through one of the functions below, and each is anchored on an
// absolute path baked at configure time by tests/CMakeLists.txt.
//
// WHY a baked absolute rather than a relative probe: the test binary is
// launched from at least four different working directories in practice --
// `build/atx-vol/tests` under ctest (gtest_discover_tests bakes that absolute
// WORKING_DIRECTORY), `build/bin` because atx-vol/README.md tells the reader to
// run the exe directly, `build/`, and the repo root. A relative path therefore
// names a DIFFERENT file per launch directory while looking identical in the
// source. This tree previously carried six independent hand-pasted probe
// "ladders" with three, four and five `../` rungs plus one with none, and they
// disagreed about which directories they could reach.
//
// Two consequences were not hypothetical. `support/oracle_pde_golden.cpp` ended
// its ladder at a hard-coded `C:/atx/...` fallback into a different checkout,
// and its regeneration path opens that result in APPEND mode -- so a documented
// invocation plus a documented env var wrote into another worktree. And the
// bare-relative `artifact-cache` produced five separate caches on disk, one per
// launch directory, which for a year read as a cache-freshness flake.
//
// The property that matters is machine-checked in test_paths_test.cpp
// (TestPathing) by flipping the working directory and demanding the answers not
// move -- a check no future ladder spelling can defeat. Add resolvers here
// rather than probing from a call site; a second mechanism is how the first one
// rotted.

#include <filesystem>
#include <string>
#include <system_error>

#if !defined(ATX_VOL_TESTS_ROOT) || !defined(ATX_VOL_REPO_ROOT) || !defined(ATX_VOL_ARTIFACT_CACHE)
#error "test_paths.hpp needs ATX_VOL_TESTS_ROOT / ATX_VOL_REPO_ROOT / ATX_VOL_ARTIFACT_CACHE. \
Any target including this header must bake them the way atx-vol/tests/CMakeLists.txt does. \
There is deliberately no relative fallback: that is the defect this header exists to remove."
#endif

namespace atx::vol::testkit {

namespace detail {

// lexically_normal() leaves a TRAILING SEPARATOR when the input ends in "..",
// e.g. "C:/wt/pool/atx-vol/.." normalizes to "C:/wt/pool/". That trailing
// separator is a real difference, not cosmetic: it gives the path an empty
// final component, so component-wise containment checks and equality
// comparisons against the same directory spelled without it both fail.
[[nodiscard]] inline std::filesystem::path normalized(const std::filesystem::path &p) {
  const std::filesystem::path n = p.lexically_normal();
  return n.filename().empty() ? n.parent_path() : n;
}

} // namespace detail

// Absolute path of atx-vol/tests/.
[[nodiscard]] inline std::filesystem::path tests_root() {
  return detail::normalized(std::filesystem::path{ATX_VOL_TESTS_ROOT});
}

// Absolute path of the repository root.
[[nodiscard]] inline std::filesystem::path repo_root() {
  return detail::normalized(std::filesystem::path{ATX_VOL_REPO_ROOT});
}

// A committed fixture under atx-vol/tests/support/.
[[nodiscard]] inline std::filesystem::path test_fixture(const std::string &name) {
  return tests_root() / "support" / name;
}

// Committed test data under atx-vol/tests/data/.
[[nodiscard]] inline std::filesystem::path test_data(const std::string &rel) {
  return tests_root() / "data" / rel;
}

// Root of the repo-level data/ tree: licensed vendor market data, gitignored
// and NOT committed (.gitignore:115-119).
[[nodiscard]] inline std::filesystem::path data_root() { return repo_root() / "data"; }

// A path under data/, whether or not it exists.
[[nodiscard]] inline std::filesystem::path market_data(const std::string &rel) {
  return data_root() / rel;
}

// `rel` under data/ when it is present, else an EMPTY path.
//
// Absence is reported as a value rather than an error because absence is the
// NORMAL case here: the data is licensed, deliberately uncommitted, and every
// caller turns an empty result into a skip. Callers must keep treating it so.
//
// One predicate, because it was open-coded identically at nine call sites --
// six of which I introduced myself while unifying the path resolution. A
// duplicated predicate is the same defect as a duplicated ladder, one layer up.
[[nodiscard]] inline std::filesystem::path market_data_if_present(const std::string &rel) {
  const std::filesystem::path p = market_data(rel);
  std::error_code ec;
  return std::filesystem::exists(p, ec) ? p : std::filesystem::path{};
}

// A path anywhere else in the repository, e.g. a shipped example config.
[[nodiscard]] inline std::filesystem::path repo_file(const std::string &rel) {
  return repo_root() / rel;
}

// Root of the on-disk fitted-artifact cache: one per BUILD TREE, not one per
// repository. See the rationale in tests/CMakeLists.txt -- the cache key has no
// build-configuration component, so sharing it across build trees would let one
// configuration serve another's fitted archives.
[[nodiscard]] inline std::filesystem::path artifact_cache_root() {
  return detail::normalized(std::filesystem::path{ATX_VOL_ARTIFACT_CACHE});
}

} // namespace atx::vol::testkit
