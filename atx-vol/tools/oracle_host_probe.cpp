#include "oracle_host_probe.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
// tlhelp32.h consumes windows.h's types, so the order is load-bearing.
#include <tlhelp32.h>
#endif

namespace atx::vol::oracle {

using atx::core::Ok; // Err resolves via ADL (Error argument); Ok's arguments
                     // are not atx::core types — same convention as the rest
                     // of the oracle tool TUs.

namespace {

// Mirror of $busyNames in scripts/oracle-targeted-gate.ps1. `cl` and `link`
// are short enough to collide with an unrelated binary, and that is the safe
// direction: a false positive costs a re-run, a false negative publishes a
// timing number measured against a compile.
constexpr std::array<std::string_view, 7> kBusyNames{"clang-cl", "cl",      "link",
                                                     "lld-link", "ninja",   "msbuild",
                                                     "atx-vol-oracle-bench"};

// Lower-cases and strips one trailing ".exe" so a Win32 image name compares
// against the PowerShell gate's extension-less ProcessName vocabulary.
[[nodiscard]] std::string normalize_process_name(std::string_view image) {
  std::string name;
  name.reserve(image.size());
  for (const char ch : image) {
    name.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
  }
  constexpr std::string_view kExe = ".exe";
  if (name.size() > kExe.size() && std::string_view{name}.ends_with(kExe)) {
    name.resize(name.size() - kExe.size());
  }
  return name;
}

} // namespace

std::span<const std::string_view> quiet_host_busy_names() noexcept {
  return std::span<const std::string_view>{kBusyNames};
}

Status require_quiet_host(std::span<const std::string> busy) {
  if (busy.empty()) {
    return Ok();
  }
  std::string names;
  for (const std::string &name : busy) {
    if (!names.empty()) {
      names.append(",");
    }
    names.append(name);
  }
  return Err(ErrorCode::InvalidArgument,
             "--quiet-host: competing process(es) running, refusing to publish a timing "
             "measurement taken against them: " +
                 names);
}

#if defined(_WIN32)

Result<std::vector<std::string>> running_busy_processes() {
  // SAFETY: CreateToolhelp32Snapshot returns INVALID_HANDLE_VALUE (not null)
  // on failure; the handle is closed on every exit path below because the two
  // early returns happen before it is opened or immediately close it.
  const HANDLE snapshot = ::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snapshot == INVALID_HANDLE_VALUE) {
    return Err(ErrorCode::Unavailable, "--quiet-host: cannot enumerate processes on this host");
  }
  PROCESSENTRY32W entry{};
  entry.dwSize = sizeof(entry);
  std::vector<std::string> seen;
  if (::Process32FirstW(snapshot, &entry) != FALSE) {
    const DWORD self = ::GetCurrentProcessId();
    do {
      if (entry.th32ProcessID == self) {
        // The bench is ON the busy list, so a probe that reported the caller
        // could never pass. SAFETY: `continue` in a do-while jumps to the
        // CONDITION, which is the Process32NextW advance — the loop still makes
        // progress, unlike the usual do-while `continue` trap.
        continue;
      }
      // szExeFile is a fixed-size NUL-terminated array; the narrow copy below
      // is exact because every busy name is ASCII.
      std::string image;
      for (std::size_t i = 0; i < std::size(entry.szExeFile) && entry.szExeFile[i] != L'\0'; ++i) {
        const wchar_t wide = entry.szExeFile[i];
        image.push_back(wide < 128 ? static_cast<char>(wide) : '?');
      }
      const std::string name = normalize_process_name(image);
      const bool is_busy =
          std::find(kBusyNames.begin(), kBusyNames.end(), name) != kBusyNames.end();
      if (is_busy && std::find(seen.begin(), seen.end(), name) == seen.end()) {
        seen.push_back(name);
      }
    } while (::Process32NextW(snapshot, &entry) != FALSE);
  }
  ::CloseHandle(snapshot);
  std::sort(seen.begin(), seen.end());
  return Ok(std::move(seen));
}

#else

Result<std::vector<std::string>> running_busy_processes() {
  return Err(ErrorCode::NotImplemented,
             "--quiet-host has no process probe on this platform; the oracle loop's timing "
             "gates are Windows-only");
}

#endif

Status enforce_quiet_host() {
  ATX_TRY(const std::vector<std::string> busy, running_busy_processes());
  return require_quiet_host(busy);
}

} // namespace atx::vol::oracle
