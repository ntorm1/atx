// w0o1_report_kv.hpp — W0-O1: strict, non-throwing audit of a report stage's kvs.
//
// Root cause of the long-standing StageRunSyntheticSmoke failure ("invalid stod
// argument"): run_report's StageResult carries two IDENTITY kvs
// (research_artifact_id, books_artifact_id — a 64-hex artifact id, or "unknown" for an
// unidentified legacy panel) ahead of its numeric scorecard kvs. The smoke test called
// std::stod on EVERY report kv, so the identity value "unknown" threw
// std::invalid_argument and aborted the test body before any finiteness check ran.
//
// This header classifies each kv instead of assuming it is numeric: identity keys must
// hold a well-formed id, every other key must parse COMPLETELY as a floating value
// (std::from_chars, no throw, no partial parse such as "1.5x") and be finite. Anything
// else is a named failure, never an exception. Test-only; header-only.
#pragma once

#include <array>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace atx_test_w0_o1_report_kv {

// Strict parse: the WHOLE value must be one floating literal (from_chars "general"
// format: decimal or scientific; also nan/inf spellings, which the finiteness check then
// rejects). Empty, leading/trailing junk, or a non-numeric word -> nullopt.
[[nodiscard]] inline std::optional<double> parse_numeric_kv(std::string_view v) noexcept {
    if (v.empty()) {
        return std::nullopt;
    }
    double out = 0.0;
    const char *first = v.data();
    const char *last = v.data() + v.size();
    const std::from_chars_result r = std::from_chars(first, last, out);
    if (r.ec != std::errc{} || r.ptr != last) {
        return std::nullopt;
    }
    return out;
}

// run_report's non-numeric identity kvs (stage_report.cpp, run_report_impl sr.kvs).
inline constexpr std::array<std::string_view, 2> kReportIdentityKeys{
    "research_artifact_id", "books_artifact_id"};

[[nodiscard]] constexpr bool is_identity_key(std::string_view k) noexcept {
    for (std::string_view id : kReportIdentityKeys) {
        if (k == id) {
            return true;
        }
    }
    return false;
}

// An identity value is "unknown" (unidentified legacy panel) or a 64-char lowercase hex
// artifact id (the stage's own report_hash_valid rule).
[[nodiscard]] constexpr bool identity_value_valid(std::string_view v) noexcept {
    if (v == "unknown") {
        return true;
    }
    if (v.size() != 64U) {
        return false;
    }
    for (char c : v) {
        const bool hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        if (!hex) {
            return false;
        }
    }
    return true;
}

struct KvAudit {
    std::size_t numeric_checked = 0;   // numeric kvs that parsed AND were finite
    std::size_t identity_checked = 0;  // identity kvs with a well-formed value
    std::vector<std::string> failures; // one message per offending kv
};

// Audits every kv; never throws on malformed input.
[[nodiscard]] inline KvAudit
audit_report_kvs(const std::vector<std::pair<std::string, std::string>> &kvs) {
    KvAudit a;
    for (const auto &[k, v] : kvs) {
        if (is_identity_key(k)) {
            if (identity_value_valid(v)) {
                ++a.identity_checked;
            } else {
                a.failures.push_back("identity kv '" + k + "' malformed: '" + v + "'");
            }
            continue;
        }
        const std::optional<double> x = parse_numeric_kv(v);
        if (!x) {
            a.failures.push_back("numeric kv '" + k + "' does not parse: '" + v + "'");
        } else if (!std::isfinite(*x)) {
            a.failures.push_back("numeric kv '" + k + "' is not finite: '" + v + "'");
        } else {
            ++a.numeric_checked;
        }
    }
    return a;
}

} // namespace atx_test_w0_o1_report_kv
