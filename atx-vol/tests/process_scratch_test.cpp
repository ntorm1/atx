// FIX-2/F2-A: the atx-vol test suite must be safe to run in two worktrees at once.
//
// THE DEFECT. ~35 fixtures in this suite derive a scratch path by joining a FIXED
// leaf name onto `std::filesystem::temp_directory_path()`, and most of them
// `remove_all()` that path on entry (the canonical shape is `test_root()` at
// surface_db_populate_test.cpp:48-53). `temp_directory_path()` is machine-wide, so
// two concurrently running processes of this binary -- the normal case in this
// sprint, where parallel agents run parallel worktrees -- resolve the SAME directory
// and destroy each other's trees mid-run. Observed symptom, twice within an hour on
// two different branches: `IoError: write_surface_archive_v2_file: cannot open temp
// file` from SurfaceDbPopulate.* and CorpusGeneratedProperty.*, all of which pass in
// isolation on the same binary. A suite that corrupts its own results under
// concurrency invalidates every count quoted under load, so it is a defect in the
// suite, not an environment quirk.
//
// WHAT THESE TESTS ASSERT. Not a naming formula -- the property. Each driver test
// actually launches a SECOND process of this same binary (a real concurrent-execution
// check, sequenced so it is deterministic rather than racy) and asserts:
//   1. the two processes do not resolve the same scratch root;
//   2. a fixture-shaped `remove_all` + rewrite in the other process does not wipe
//      this process's tree under the same fixed leaf name -- the exact mechanism;
//   3. the other process's scratch root does not outlive it, on a clean exit AND on
//      a failing exit, so isolation is not traded for a disk leak.
//
// The `ProcessScratchChild.*` tests are the child half. They skip when the probe
// env var is unset, i.e. in every ordinary run of the suite.

#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

#ifdef _WIN32
#include <process.h> // _spawnl, _P_WAIT
#include <windows.h> // GetModuleFileNameA, GetEnvironmentVariableA
#endif

namespace atx::vol {
namespace {

namespace fs = std::filesystem;

// Set by the parent to an absolute path; the child writes its answer there.
constexpr const char *kProbeEnv = "ATX_VOL_SCRATCH_PROBE";

// The fixed leaf both processes use -- the shape every affected fixture has.
constexpr const char *kSharedLeaf = "atx_scratch_probe_shared_tree";

[[nodiscard]] std::string normalized(const fs::path &p) {
  std::string s = p.lexically_normal().string();
  while (s.size() > 3 && (s.back() == '\\' || s.back() == '/')) {
    s.pop_back();
  }
  return s;
}

// Empty when unset. (std::getenv is /WX-deprecated under this toolchain.)
[[nodiscard]] std::string probe_out() {
#ifdef _WIN32
  char buf[4096];
  const DWORD n = ::GetEnvironmentVariableA(kProbeEnv, buf, static_cast<DWORD>(sizeof buf));
  if (n == 0 || n >= sizeof buf) {
    return {};
  }
  return std::string(buf, static_cast<std::size_t>(n));
#else
  const char *v = std::getenv(kProbeEnv);
  return v == nullptr ? std::string{} : std::string{v};
#endif
}

[[nodiscard]] fs::path this_image() {
#ifdef _WIN32
  char buf[4096];
  const DWORD n = ::GetModuleFileNameA(nullptr, buf, static_cast<DWORD>(sizeof buf));
  return fs::path(std::string(buf, static_cast<std::size_t>(n)));
#else
  std::error_code ec;
  return fs::read_symlink("/proc/self/exe", ec);
#endif
}

// Launch this same test binary as a second process, running only `filter`, and wait
// for it. Returns its exit code.
[[nodiscard]] int run_second_process(const std::string &filter, const fs::path &out_file) {
  const std::string exe = this_image().string();
  const std::string arg_filter = "--gtest_filter=" + filter;
#ifdef _WIN32
  ::_putenv_s(kProbeEnv, out_file.string().c_str());
  const std::string quoted_argv0 = "\"" + exe + "\"";
  const std::intptr_t rc = ::_spawnl(_P_WAIT, exe.c_str(), quoted_argv0.c_str(),
                                     arg_filter.c_str(), "--gtest_brief=1", nullptr);
  ::_putenv_s(kProbeEnv, "");
  return static_cast<int>(rc);
#else
  ::setenv(kProbeEnv, out_file.string().c_str(), 1);
  const std::string cmd = "\"" + exe + "\" " + arg_filter + " --gtest_brief=1";
  const int rc = std::system(cmd.c_str());
  ::unsetenv(kProbeEnv);
  return rc;
#endif
}

[[nodiscard]] std::string read_line(const fs::path &p) {
  std::ifstream in(p, std::ios::binary);
  if (!in.is_open()) {
    return {};
  }
  std::string line;
  std::getline(in, line);
  return line;
}

void write_text(const fs::path &p, const std::string &text) {
  std::ofstream out(p, std::ios::binary | std::ios::trunc);
  out << text;
}

// --------------------------------------------------------------------------------
// Child half. Skipped unless the parent set the probe env var.
// --------------------------------------------------------------------------------

TEST(ProcessScratchChild, ReportScratchRoot) {
  const std::string out = probe_out();
  if (out.empty()) {
    GTEST_SKIP() << "child-side probe: runs only when the driver test sets " << kProbeEnv;
  }
  write_text(fs::path(out), normalized(fs::temp_directory_path()));
}

TEST(ProcessScratchChild, ReportScratchRootThenFailOnPurpose) {
  const std::string out = probe_out();
  if (out.empty()) {
    GTEST_SKIP() << "child-side probe: runs only when the driver test sets " << kProbeEnv;
  }
  write_text(fs::path(out), normalized(fs::temp_directory_path()));
  // Deliberate failure: the driver uses this to prove the scratch root is still
  // reclaimed when the process exits non-zero.
  ADD_FAILURE() << "deliberate failure (FIX-2/F2-A cleanup-on-failure probe)";
}

TEST(ProcessScratchChild, ClobberFixtureStyleScratchTree) {
  const std::string out = probe_out();
  if (out.empty()) {
    GTEST_SKIP() << "child-side probe: runs only when the driver test sets " << kProbeEnv;
  }
  // Verbatim the shape of surface_db_populate_test.cpp's test_root(): a fixed leaf
  // under the process temp root, remove_all'd on entry.
  const fs::path root = fs::temp_directory_path() / kSharedLeaf;
  std::error_code ec;
  fs::remove_all(root, ec);
  fs::create_directories(root, ec);
  write_text(root / "payload.txt", "child");
  write_text(fs::path(out), normalized(root));
}

// --------------------------------------------------------------------------------
// Driver half.
// --------------------------------------------------------------------------------

TEST(ProcessScratchIsolation, TwoConcurrentProcessesDoNotShareAScratchRoot) {
  if (!probe_out().empty()) {
    GTEST_SKIP() << "driver test; not re-entered in the child";
  }
  const fs::path probe = fs::temp_directory_path() / "atx_scratch_probe_root.txt";
  std::error_code ec;
  fs::remove(probe, ec);

  ASSERT_EQ(run_second_process("ProcessScratchChild.ReportScratchRoot", probe), 0);
  const std::string child_root = read_line(probe);
  ASSERT_FALSE(child_root.empty()) << "second process did not report its scratch root";

  EXPECT_NE(child_root, normalized(fs::temp_directory_path()))
      << "a second, concurrently running process of this binary resolves the SAME scratch "
         "root, so every fixture deriving a fixed-name path from temp_directory_path() "
         "collides with its twin in another worktree";
  fs::remove(probe, ec);
}

TEST(ProcessScratchIsolation, AConcurrentProcessDoesNotWipeThisProcessesFixtureTree) {
  if (!probe_out().empty()) {
    GTEST_SKIP() << "driver test; not re-entered in the child";
  }
  std::error_code ec;
  const fs::path mine = fs::temp_directory_path() / kSharedLeaf;
  fs::remove_all(mine, ec);
  fs::create_directories(mine, ec);
  write_text(mine / "payload.txt", "parent");

  const fs::path probe = fs::temp_directory_path() / "atx_scratch_probe_clobber.txt";
  fs::remove(probe, ec);
  ASSERT_EQ(run_second_process("ProcessScratchChild.ClobberFixtureStyleScratchTree", probe), 0);

  ASSERT_TRUE(fs::exists(mine / "payload.txt"))
      << "this process's fixture tree was destroyed by the other process's remove_all: "
      << mine.string();
  EXPECT_EQ(read_line(mine / "payload.txt"), "parent")
      << "the other process overwrote this process's fixture file at the same path";

  fs::remove_all(mine, ec);
  fs::remove(probe, ec);
}

TEST(ProcessScratchIsolation, AProcessScratchRootDoesNotOutliveThatProcess) {
  if (!probe_out().empty()) {
    GTEST_SKIP() << "driver test; not re-entered in the child";
  }
  const fs::path probe = fs::temp_directory_path() / "atx_scratch_probe_cleanup.txt";
  std::error_code ec;
  fs::remove(probe, ec);

  ASSERT_EQ(run_second_process("ProcessScratchChild.ReportScratchRoot", probe), 0);
  const std::string child_root = read_line(probe);
  ASSERT_FALSE(child_root.empty());
  EXPECT_FALSE(fs::exists(fs::path(child_root)))
      << "the other process's scratch root outlived it (a unique root that is never "
         "reclaimed trades a collision for a disk leak): "
      << child_root;
  fs::remove(probe, ec);
}

TEST(ProcessScratchIsolation, AFailingProcessStillReclaimsItsScratchRoot) {
  if (!probe_out().empty()) {
    GTEST_SKIP() << "driver test; not re-entered in the child";
  }
  const fs::path probe = fs::temp_directory_path() / "atx_scratch_probe_failexit.txt";
  std::error_code ec;
  fs::remove(probe, ec);

  // This child fails on purpose, so a NON-zero exit is the expected outcome.
  EXPECT_NE(run_second_process("ProcessScratchChild.ReportScratchRootThenFailOnPurpose", probe), 0);
  const std::string child_root = read_line(probe);
  ASSERT_FALSE(child_root.empty());
  EXPECT_FALSE(fs::exists(fs::path(child_root)))
      << "a FAILING process left its scratch root behind: " << child_root;
  fs::remove(probe, ec);
}

} // namespace
} // namespace atx::vol
