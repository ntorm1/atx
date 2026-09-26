#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "atx/core/error.hpp"
#include "atx/core/types.hpp"

namespace atx::impl {

// Runtime family recipes use the existing two forward variants and two restrictions.
// A negative sign multiplies the evaluated DSL exactly once. No sign is inferred.
struct PreregFamily {
    std::string id;
    std::string name;
    std::string dsl;
    std::string theme;
    int sign{1};
    std::vector<atx::usize> horizons;
    bool retained{};
    std::string configuration_sha256;
};

struct EquityIcPrereg {
    std::string epoch;
    atx::i64 checkpoint{};
    atx::i64 declared_n{};
    // Retained lineage is a declaration, not a verified E2 registry reconciliation.
    // The legacy ledger conservatively charges this full measured configuration count.
    atx::i64 measured_n{};
    atx::usize retained_count{};
    std::vector<PreregFamily> families;
    std::vector<atx::usize> horizons;
    std::string canonical_json;
    std::string canonical_sha256;
    std::string file_sha256;
};

// Strict bounded JSON; unknown/duplicate keys, duplicate configurations, malformed
// lineage, inconsistent declared N and unsupported recipes are rejected before VM work.
[[nodiscard]] atx::core::Result<EquityIcPrereg> parse_equity_ic_prereg(std::string_view text);
// Hashes the exact bytes read, then parses those same bytes (no second file read).
[[nodiscard]] atx::core::Result<EquityIcPrereg> load_equity_ic_prereg(
    const std::string &path, std::string_view expected_file_sha256);

} // namespace atx::impl
