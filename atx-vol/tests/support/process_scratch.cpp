// FIX-2/F2-A: give every atx-vol test PROCESS its own private temp root.
//
// WHY THIS TU EXISTS AND WHY IT HAS NO HEADER. ~35 fixtures in this suite build a
// scratch path as `temp_directory_path() / "<fixed name>"`, and most `remove_all()`
// it on entry (canonical shape: `test_root()` at surface_db_populate_test.cpp:48-53).
// `temp_directory_path()` is machine-wide, so two concurrently running processes of
// this binary -- the normal case in this sprint, with parallel agents in parallel
// worktrees -- resolve the SAME directory and wipe each other's trees mid-run. That
// surfaced twice within an hour on two different branches as
// `IoError: write_surface_archive_v2_file: cannot open temp file` in
// SurfaceDbPopulate.* and CorpusGeneratedProperty.*, every one of which passes in
// isolation on the same binary.
//
// Fixing the ~35 call sites one by one would touch test files owned by half the
// sprint's workstreams and would leave the NEXT fixed-name fixture free to
// reintroduce the bug. So the fix is applied once, at the root of the class: before
// main() runs, this TU creates a directory unique to this process and repoints the
// process's TMP/TEMP/TMPDIR at it. Every existing and future fixture keeps calling
// `temp_directory_path()` unchanged and transparently lands inside a per-process
// sandbox. No fixture, and no CMake wiring beyond linking this file, has to know.
//
// UNIQUENESS IS SETTLED BY THE FILESYSTEM, NOT BY THE NAME. The candidate name mixes
// the process id with a hash of this executable's own path (so roots from different
// worktrees are also distinguishable by eye), but the loop below accepts a candidate
// ONLY when `create_directory` reports that THIS call created it. A recycled pid, a
// leftover root, or a genuine race therefore cannot hand two live processes the same
// directory -- the loser just takes the next candidate.
//
// CLEANUP. The root is removed on the way out via `std::atexit`, which covers a
// clean exit, an exit with failing tests, and a `--gtest_list_tests` enumeration
// (ctest's PRE_TEST discovery mode) alike -- a unique-but-immortal root would just
// trade a collision for a disk leak. Reclamation is registered ONLY when we actually
// created a fresh directory, so the degraded path can never delete the machine-wide
// temp dir. Set ATX_VOL_KEEP_SCRATCH=1 to keep the tree for post-mortem inspection.
//
// Proven by tests/process_scratch_test.cpp, which launches a real second process of
// this binary and asserts the two do not share a root, do not wipe each other's
// fixture trees, and do not leak their roots (including on a failing exit).

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <system_error>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace atx::vol::test {
namespace {

namespace fs = std::filesystem;

[[nodiscard]] std::uint64_t fnv1a(const std::string &s) noexcept {
  std::uint64_t h = 1469598103934665603ULL;
  for (const char c : s) {
    h ^= static_cast<std::uint8_t>(c);
    h *= 1099511628211ULL;
  }
  return h;
}

// This executable's own path: differs per worktree, so it is what makes a root
// attributable to the tree that produced it.
[[nodiscard]] std::string image_path() {
#ifdef _WIN32
  char buf[4096];
  const DWORD n = ::GetModuleFileNameA(nullptr, buf, static_cast<DWORD>(sizeof buf));
  return std::string(buf, static_cast<std::size_t>(n));
#else
  std::error_code ec;
  return fs::read_symlink("/proc/self/exe", ec).string();
#endif
}

[[nodiscard]] unsigned long long current_pid() noexcept {
#ifdef _WIN32
  return static_cast<unsigned long long>(::GetCurrentProcessId());
#else
  return static_cast<unsigned long long>(::getpid());
#endif
}

void set_env_var(const char *name, const std::string &value) {
#ifdef _WIN32
  // _putenv_s updates the CRT copy; SetEnvironmentVariableA updates the process
  // environment block that GetTempPath (and therefore temp_directory_path) reads.
  ::_putenv_s(name, value.c_str());
  ::SetEnvironmentVariableA(name, value.c_str());
#else
  ::setenv(name, value.c_str(), 1);
#endif
}

[[nodiscard]] bool keep_scratch_requested() {
#ifdef _WIN32
  char buf[16];
  const DWORD n = ::GetEnvironmentVariableA("ATX_VOL_KEEP_SCRATCH", buf, static_cast<DWORD>(sizeof buf));
  return n > 0 && n < sizeof buf && buf[0] != '0';
#else
  const char *v = std::getenv("ATX_VOL_KEEP_SCRATCH");
  return v != nullptr && v[0] != '\0' && v[0] != '0';
#endif
}

// Kept short on purpose: it is prepended to every fixture path in the suite, and
// Windows still enforces MAX_PATH on the deeper SurfaceDb trees.
[[nodiscard]] std::string scratch_leaf(unsigned long long pid, std::uint64_t image_salt,
                                       unsigned attempt) {
  char buf[64];
  if (attempt == 0) {
    std::snprintf(buf, sizeof buf, "atxv-%04llx%04llx",
                  static_cast<unsigned long long>(image_salt & 0xffffULL), pid & 0xffffULL);
  } else {
    std::snprintf(buf, sizeof buf, "atxv-%04llx%04llx-%u",
                  static_cast<unsigned long long>(image_salt & 0xffffULL), pid & 0xffffULL,
                  attempt);
  }
  return std::string(buf);
}

// Leaked on purpose. The atexit handler is registered DURING this TU's dynamic
// initialization, so it runs after the destructors of anything constructed later;
// a raw pointer to a never-destroyed path is the only shape guaranteed to still be
// readable at that point.
const fs::path *g_scratch_root = nullptr;

void reclaim_scratch_root() noexcept {
  if (g_scratch_root == nullptr) {
    return;
  }
  std::error_code ec;
  fs::remove_all(*g_scratch_root, ec);
}

bool install_process_scratch_root() {
  std::error_code ec;
  const fs::path machine = fs::temp_directory_path(ec);
  if (ec || machine.empty()) {
    return false; // no temp dir at all: leave the process exactly as we found it.
  }

  const std::uint64_t salt = fnv1a(image_path());
  const unsigned long long pid = current_pid();

  fs::path root;
  for (unsigned attempt = 0; attempt < 4096; ++attempt) {
    const fs::path candidate = machine / scratch_leaf(pid, salt, attempt);
    // True ONLY when THIS call created the directory, so the winner owns it outright.
    if (fs::create_directory(candidate, ec) && !ec) {
      root = candidate;
      break;
    }
  }
  if (root.empty()) {
    // Degraded: could not carve out a private root. Keep the historical behavior
    // (shared temp) rather than inventing one, and register NO reclamation -- the
    // machine-wide temp dir must never be handed to remove_all.
    return false;
  }

  g_scratch_root = new fs::path(root);
  const std::string value = root.string();
  set_env_var("TMP", value);
  set_env_var("TEMP", value);
  set_env_var("TMPDIR", value); // POSIX spelling, harmless on Windows

  if (!keep_scratch_requested()) {
    (void)std::atexit(&reclaim_scratch_root);
  }
  return true;
}

// Namespace-scope dynamic initialization: runs before main(), and therefore before
// any fixture can call temp_directory_path().
const bool g_scratch_installed = install_process_scratch_root();

} // namespace

// Referenced so the initializer above can never be considered unused.
[[nodiscard]] bool process_scratch_installed() noexcept { return g_scratch_installed; }

} // namespace atx::vol::test
