#pragma once

// Host-quietness probe for atx-vol-oracle-bench --quiet-host.
//
// A rows/s number measured while a compile or a second bench is running is not
// a measurement of this build, so `--quiet-host` refuses to produce one. The
// busy-name registry MIRRORS `$busyNames` in
// scripts/oracle-targeted-gate.ps1 (Assert-OracleQuietHost) so the in-binary
// check and the PowerShell gate cannot disagree about what "quiet" means.
//
// Split out of the driver TU on purpose: the enumeration needs <windows.h>,
// and the driver is TEXTUALLY #included by tests/oracle_bench_test.cpp beside
// the Arrow headers. Keeping the platform header behind this seam also makes
// the RULE (require_quiet_host) testable without an actually-busy host.

#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "atx/vol/api/core/types.hpp"

namespace atx::vol::oracle {

// Process base names (no extension, lower-cased) whose presence makes a timing
// measurement untrustworthy. Verbatim mirror of the PowerShell gate's list.
[[nodiscard]] std::span<const std::string_view> quiet_host_busy_names() noexcept;

// Base names (lower-cased, extension stripped) of the busy processes running
// right now, EXCLUDING this process — the bench itself is on the list, and a
// run that refused because it saw itself would never pass. Deterministic order
// (registry order, then first-seen). Err on an OS enumeration failure: an
// unverifiable host is not a quiet one.
[[nodiscard]] Result<std::vector<std::string>> running_busy_processes();

// The RULE, pure and total: empty span -> Ok, otherwise InvalidArgument naming
// every offender so the failure is diagnosable without re-running.
[[nodiscard]] Status require_quiet_host(std::span<const std::string> busy);

// running_busy_processes() + require_quiet_host(), the composition the driver
// calls. Err propagates from either half.
[[nodiscard]] Status enforce_quiet_host();

} // namespace atx::vol::oracle
