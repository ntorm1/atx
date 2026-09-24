// SAFETY: the single-writer lock (ruling R-2) needs C11 exclusive create, and
// `std::fopen(path, "wx")` is the only portable spelling of it in C++20 — `std::ofstream`
// gained `std::ios::noreplace` in C++23, and every non-atomic alternative reintroduces
// the test-then-create race the lock exists to remove. The UCRT marks `fopen`
// `_CRT_INSECURE_DEPRECATE(fopen_s)`, which clang-cl reports as
// `-Wdeprecated-declarations` and `/WX` turns into an error; `fopen_s` cannot replace it
// because it has no exclusive-create mode. The suppression is scoped to this TU and
// matches the established repo idiom (`atx-impl/tests/discover_test.cpp:10`,
// `seed_parse_test.cpp:21`, `atx-engine/examples/build_universe.cpp:8`). It must precede
// every include, which it does: `target_precompile_headers` is applied only to
// `atx-engine` and the engine test groups (`atx-engine/CMakeLists.txt:136`,
// `atx-engine/tests/CMakeLists.txt:91-96`), so no force-included header parses
// <stdio.h> ahead of this line in an atx-impl TU. Re-check that if atx-impl ever gains
// a PCH — the define would then arrive too late and the deprecation would return.
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS 1
#endif

#include "trial_ledger.hpp"

#include <array>
#include <cassert>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <ios>
#include <iterator>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "atx/core/sha256.hpp"

namespace atx::impl {
namespace {
namespace fs = std::filesystem;
using atx::core::Err;
using atx::core::ErrorCode;
using atx::core::Ok;
using atx::core::Result;
using atx::core::Status;

// Restated from the file-local writer at stage_discover.cpp:99-122. That copy has
// internal linkage in its own translation unit and this layer pulls in no JSON
// dependency for writing (stage_discover.cpp:91-92 records why), so the escaper is
// restated byte-for-byte rather than reached for — the two atx-impl writers must escape
// identically or a digest taken over one cannot be checked against the other.
[[nodiscard]] std::string json_escape(const std::string &text) {
    std::string out;
    out.reserve(text.size() + 2);
    for (const char c : text) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b";  break;
            case '\f': out += "\\f";  break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buffer[8];
                    std::snprintf(buffer, sizeof(buffer), "\\u%04x",
                                  static_cast<unsigned>(static_cast<unsigned char>(c)));
                    out += buffer;
                } else {
                    out += c;
                }
        }
    }
    return out;
}

// Shortest round-trip integer text. std::to_chars is locale-independent by construction,
// which is what the byte-determinism obligation needs (§7.4); no locale imbue exists to
// be forgotten. The buffer cannot overflow for any 64-bit integer, so the error branch
// is an invariant assertion rather than a runtime path.
template <class Integer> [[nodiscard]] std::string json_int(Integer value) {
    std::array<char, 32> bytes{};
    const auto parsed = std::to_chars(bytes.data(), bytes.data() + bytes.size(), value);
    assert(parsed.ec == std::errc{} && "32 bytes always hold a 64-bit integer");
    return {bytes.data(), parsed.ptr};
}

// Shortest round-trip real, the std::to_chars idiom of stage_equity_baseline.cpp:41-45,
// plus one rule: a token that reads back as an integer gains a ".0", so the ledger's
// reals stay visibly real and match the §4.5 sample line (5.0, 365.0, 1.0, 2.0). Callers
// reject non-finite values before reaching here, so no "inf"/"nan" token can appear.
[[nodiscard]] std::string json_real(atx::f64 value) {
    std::array<char, 64> bytes{};
    const auto parsed = std::to_chars(bytes.data(), bytes.data() + bytes.size(), value);
    assert(parsed.ec == std::errc{} && "64 bytes always hold a shortest-form double");
    std::string out{bytes.data(), parsed.ptr};
    if (out.find_first_not_of("-0123456789") == std::string::npos) out += ".0";
    return out;
}

[[nodiscard]] std::string json_text(const std::string &value) {
    return "\"" + json_escape(value) + "\"";
}

[[nodiscard]] std::string json_opt_text(const std::optional<std::string> &value) {
    return value ? json_text(*value) : std::string{"null"};
}

[[nodiscard]] std::string json_int_array(const std::vector<atx::i64> &values) {
    std::string out{"["};
    for (atx::usize i = 0; i < values.size(); ++i) {
        if (i != 0) out += ',';
        out += json_int(values[i]);
    }
    out += ']';
    return out;
}

[[nodiscard]] std::string json_real_array(const std::vector<atx::f64> &values) {
    std::string out{"["};
    for (atx::usize i = 0; i < values.size(); ++i) {
        if (i != 0) out += ',';
        out += json_real(values[i]);
    }
    out += ']';
    return out;
}

[[nodiscard]] std::string json_text_array(const std::vector<std::string> &values) {
    std::string out{"["};
    for (atx::usize i = 0; i < values.size(); ++i) {
        if (i != 0) out += ',';
        out += json_text(values[i]);
    }
    out += ']';
    return out;
}

// Ordered key/value objects. A std::vector of pairs, not a map: insertion order IS the
// serialization order, so no container's iteration order can perturb the bytes (§7.4,
// "No iteration over an unordered container anywhere in the writers").
[[nodiscard]] std::string
json_int_map(const std::vector<std::pair<std::string, atx::i64>> &entries) {
    std::string out{"{"};
    for (atx::usize i = 0; i < entries.size(); ++i) {
        if (i != 0) out += ',';
        out += json_text(entries[i].first) + ':' + json_int(entries[i].second);
    }
    out += '}';
    return out;
}

[[nodiscard]] std::string
json_text_map(const std::vector<std::pair<std::string, std::string>> &entries) {
    std::string out{"{"};
    for (atx::usize i = 0; i < entries.size(); ++i) {
        if (i != 0) out += ',';
        out += json_text(entries[i].first) + ':' + json_text(entries[i].second);
    }
    out += '}';
    return out;
}

[[nodiscard]] std::string parents_json(const std::vector<TrialLedgerParent> &parents) {
    std::string out{"["};
    for (atx::usize i = 0; i < parents.size(); ++i) {
        if (i != 0) out += ',';
        out += "{\"role\":" + json_text(parents[i].role);
        out += parents[i].artifact_id.empty()
                   ? ",\"sha256\":" + json_text(parents[i].sha256)
                   : ",\"artifact_id\":" + json_text(parents[i].artifact_id);
        out += '}';
    }
    out += ']';
    return out;
}

[[nodiscard]] std::string signals_json(const std::vector<TrialLedgerSignal> &signals) {
    std::string out{"["};
    for (atx::usize i = 0; i < signals.size(); ++i) {
        if (i != 0) out += ',';
        out += "{\"name\":" + json_text(signals[i].name);
        out += ",\"dsl\":" + json_text(signals[i].dsl);
        out += ",\"dsl_sha256\":" + json_text(signals[i].dsl_sha256) + "}";
    }
    out += ']';
    return out;
}

[[nodiscard]] std::string bootstrap_json(const TrialLedgerBootstrap &bootstrap) {
    std::string out{"{\"draws\":"};
    out += json_int(bootstrap.draws);
    out += ",\"seed\":" + json_int(bootstrap.seed);
    out += ",\"block_len_rule\":" + json_text(bootstrap.block_len_rule);
    out += ",\"block_lens\":" + json_int_array(bootstrap.block_lens);
    out += ",\"percentiles\":" + json_real_array(bootstrap.percentiles);
    out += ",\"reportable_rule\":" + json_text(bootstrap.reportable_rule);
    out += ",\"statistic_ids\":" + json_int_map(bootstrap.statistic_ids);
    out += ",\"stream_key\":" + json_text(bootstrap.stream_key);
    out += ",\"sample_ids\":" + json_int_map(bootstrap.sample_ids);
    out += ",\"predicted_null_horizons\":" + json_int_array(bootstrap.predicted_null_horizons);
    out += '}';
    return out;
}

[[nodiscard]] std::string cost_json(const TrialLedgerCost &cost) {
    std::string out{"{\"trade_bps\":"};
    out += json_real(cost.trade_bps);
    out += ",\"trade_bps_source\":" + json_text(cost.trade_bps_source);
    out += ",\"annual_borrow_bps\":" + json_real(cost.annual_borrow_bps);
    out += ",\"annual_borrow_bps_source\":" + json_text(cost.annual_borrow_bps_source);
    out += ",\"short_leg_gross\":" + json_real(cost.short_leg_gross);
    out += ",\"priced_book_gross\":" + json_real(cost.priced_book_gross);
    out += ",\"decile_weights\":" + json_text(cost.decile_weights);
    out += ",\"borrow_day_convention\":" + json_text(cost.borrow_day_convention);
    out += ",\"borrow_day_formula\":" + json_text(cost.borrow_day_formula);
    out += ",\"provenance\":" + json_text(cost.provenance);
    out += '}';
    return out;
}

[[nodiscard]] std::string recipe_json(const TrialLedgerRecipe &recipe) {
    std::string out{"{\"signals\":"};
    out += signals_json(recipe.signals);
    out += ",\"horizons\":" + json_int_array(recipe.horizons);
    out += ",\"quantiles\":" + json_int(recipe.quantiles);
    out += ",\"bootstrap\":" + bootstrap_json(recipe.bootstrap);
    out += ",\"autocorr_lags\":" + json_int_array(recipe.autocorr_lags);
    out += ",\"turnover_source\":" + json_text(recipe.turnover_source);
    out += ",\"forward_variants\":" + json_text_array(recipe.forward_variants);
    out += ",\"restrictions\":" + json_text_array(recipe.restrictions);
    out += ",\"samples\":" + json_text_array(recipe.samples);
    out += ",\"common_sample_rule\":" + json_text(recipe.common_sample_rule);
    out += ",\"alignment\":" + json_text(recipe.alignment);
    out += ",\"net_spread_rule\":" + json_text(recipe.net_spread_rule);
    out += ",\"cost\":" + cost_json(recipe.cost);
    out += ",\"seal\":{\"policy\":" + json_text(recipe.seal.policy);
    out += ",\"validation_begin\":" + json_text(recipe.seal.validation_begin);
    out += ",\"sealed_begin\":" + json_text(recipe.seal.sealed_begin) + "}";
    out += '}';
    return out;
}

[[nodiscard]] std::string exclusions_json(const TrialLedgerSourceExclusions &exclusions) {
    std::string out{"{\"required_mark_id_count\":"};
    out += json_int(exclusions.required_mark_id_count);
    out += ",\"terminal_hypothesis_ids\":" + json_int_array(exclusions.terminal_hypothesis_ids);
    out += ",\"terminal_hypothesis_note\":" + json_text(exclusions.terminal_hypothesis_note);
    out += ",\"terminal_evidenced_record_dates\":" +
           json_text_map(exclusions.terminal_evidenced_record_dates);
    out += ",\"evidenced_non_terminal_ids\":" +
           json_int_array(exclusions.evidenced_non_terminal_ids);
    out += ",\"unclassified_id_count\":" + json_int(exclusions.unclassified_id_count);
    out += ",\"terminal_unevidenced_ids\":" +
           json_int_array(exclusions.terminal_unevidenced_ids);
    out += ",\"ex34_restriction_ids\":" + json_text_array(exclusions.ex34_restriction_ids);
    out += ",\"pcs_statement\":" + json_text(exclusions.pcs_statement);
    out += '}';
    return out;
}

[[nodiscard]] bool is_digest(std::string_view value) {
    return value.size() == 64 &&
           value.find_first_not_of("0123456789abcdef") == std::string_view::npos;
}

// A JSON object key written by this file is emitted verbatim, so a key is confined to
// text that cannot forge a scannable `"<key>":` pattern or escape its own quotes (M-1).
// Values are escaped by json_escape and need no such restriction.
[[nodiscard]] bool is_plain_key(const std::string &value) {
    if (value.empty()) return false;
    for (const char c : value) {
        if (c == '"' || c == '\\' || static_cast<unsigned char>(c) < 0x20) return false;
    }
    return true;
}

[[nodiscard]] Status reject(std::string what) {
    return Err(ErrorCode::InvalidArgument, "trial ledger: " + std::move(what));
}

// Identity half of the §4.5 "every key required" rule: the fields that name the line.
[[nodiscard]] Status validate_identity(const TrialLedgerEntry &entry) {
    if (std::string_view{entry.schema} != kTrialLedgerSchema) {
        return reject("schema must be " + std::string{kTrialLedgerSchema});
    }
    if (entry.trial_id.empty()) return reject("trial_id is required");
    if (entry.appended_utc.empty()) return reject("appended_utc is required");
    if (entry.purpose.empty()) return reject("purpose is required");
    if (entry.notes.empty()) return reject("notes is required");
    if (!is_digest(entry.prev_sha256)) {
        return reject("prev_sha256 must be 64 lowercase hex characters");
    }
    if (!is_digest(entry.producer_executable_sha256)) {
        return reject("producer_executable_sha256 must be 64 lowercase hex characters");
    }
    if (entry.checkpoint <= 0) return reject("checkpoint must be positive");
    // cp15 §5.7 / R15-3: a non-trial purpose declares exactly 0; every other purpose keeps
    // the original positive-only rule, message included.
    if (is_non_trial_purpose(entry.purpose)) {
        if (entry.trial_count_declared < 0) return reject("trial_count_declared must be positive");
        if (entry.trial_count_declared > 0) return reject("a non-trial purpose must declare 0");
    } else if (entry.trial_count_declared <= 0) {
        return reject("trial_count_declared must be positive");
    }
    if (entry.parents.empty()) return reject("at least one parent is required");
    for (const auto &parent : entry.parents) {
        const bool has_artifact = !parent.artifact_id.empty();
        const bool has_sha = !parent.sha256.empty();
        if (parent.role.empty() || has_artifact == has_sha) {
            return reject("each parent needs a role and exactly one of artifact_id / sha256");
        }
    }
    return Ok();
}

// Body half: the two-lines-per-run discipline (§4.5) and the values the writers format.
[[nodiscard]] Status validate_body(const TrialLedgerEntry &entry) {
    const bool pre_registered = entry.status == "pre-registered";
    const bool completed = entry.status == "completed";
    const bool failed = entry.status == "failed";
    if (!pre_registered && !completed && !failed) {
        return reject("status must be pre-registered, completed or failed");
    }
    const std::string expected = pre_registered ? std::string{"pending"} : entry.status;
    if (entry.result.outcome != expected) {
        return reject("result.outcome must be " + expected + " when status is " + entry.status);
    }
    if (pre_registered && (entry.result.manifest_sha256 || entry.result.failure_sha256)) {
        return reject("a pre-registered line carries no result digest");
    }
    if (completed && !entry.result.manifest_sha256) {
        return reject("a completed line requires result.manifest_sha256");
    }
    if (failed && !entry.result.failure_sha256) {
        return reject("a failed line requires result.failure_sha256");
    }
    if (entry.recipe.signals.empty()) return reject("recipe.signals is required");
    if (entry.window.start.empty() || entry.window.end_exclusive.empty()) {
        return reject("window.start and window.end_exclusive are required");
    }
    if (entry.window.observations <= 0) return reject("window.observations must be positive");
    if (entry.fit_boundary.fit_kind.empty()) return reject("fit_boundary.fit_kind is required");
    // A non-finite real has no JSON literal, so it can never be written: rejecting it here
    // keeps json_real's contract total instead of emitting an unparseable token.
    const std::array<atx::f64, 4> costs{
        entry.recipe.cost.trade_bps, entry.recipe.cost.annual_borrow_bps,
        entry.recipe.cost.short_leg_gross, entry.recipe.cost.priced_book_gross};
    for (const atx::f64 value : costs) {
        if (!std::isfinite(value)) return reject("recipe.cost carries a non-finite value");
    }
    for (const atx::f64 value : entry.recipe.bootstrap.percentiles) {
        if (!std::isfinite(value)) return reject("bootstrap percentile is non-finite");
    }
    if (entry.runtime.wall_seconds && !std::isfinite(*entry.runtime.wall_seconds)) {
        return reject("runtime.wall_seconds is non-finite");
    }
    // Object keys are written verbatim (M-1). Rejecting the three caller-reachable key
    // sets here is what keeps scan_field's first-match rule sound independently of the
    // key-ordering argument, so a future key reshuffle cannot quietly break the reader.
    for (const auto &pair : entry.source_exclusions.terminal_evidenced_record_dates) {
        if (!is_plain_key(pair.first)) {
            return reject("terminal_evidenced_record_dates key must be plain text");
        }
    }
    for (const auto &pair : entry.recipe.bootstrap.statistic_ids) {
        if (!is_plain_key(pair.first)) return reject("statistic_ids key must be plain text");
    }
    for (const auto &pair : entry.recipe.bootstrap.sample_ids) {
        if (!is_plain_key(pair.first)) return reject("sample_ids key must be plain text");
    }
    return Ok();
}

// Read one top-level field of a canonical ledger line. A targeted scanner, not a JSON
// parser: it finds the first unescaped `"<key>":` and returns the token after it.
//
// THREE THINGS MAKE THAT SOUND, and all three are load-bearing (M-1):
//  1. Escaping — a quote inside any JSON *value* is written `\"`, so the pattern's
//     leading quote can never match inside a value.
//  2. KEY ORDERING — object keys are written verbatim, so a nested object could in
//     principle contain a key spelled like a top-level one. The four scanned fields
//     (`prev_sha256` 4th, `checkpoint` 5th, `status` 7th, `trial_count_declared` 8th)
//     all precede `parents`, `recipe` and `source_exclusions` in the §4.5 order, so the
//     first match is always the genuine top-level key. Reordering the schema would
//     break this reader; the byte-exact pin test is what guards the order.
//  3. Key validation — validate_body rejects a caller-supplied object key containing a
//     quote, a backslash or a control character, so (2) cannot be defeated by data.
[[nodiscard]] std::optional<std::string_view> scan_field(std::string_view line,
                                                         std::string_view key) {
    const std::string pattern = "\"" + std::string{key} + "\":";
    const auto at = line.find(pattern);
    if (at == std::string_view::npos) return std::nullopt;
    std::string_view rest = line.substr(at + pattern.size());
    if (rest.empty()) return std::nullopt;
    if (rest.front() == '"') {
        rest.remove_prefix(1);
        const auto close = rest.find('"');
        if (close == std::string_view::npos) return std::nullopt;
        return rest.substr(0, close);
    }
    const auto end = rest.find_first_of(",}");
    if (end == std::string_view::npos) return std::nullopt;
    return rest.substr(0, end);
}

[[nodiscard]] std::optional<atx::i64> scan_int(std::string_view line, std::string_view key) {
    const auto token = scan_field(line, key);
    if (!token) return std::nullopt;
    atx::i64 value{};
    const char *const first = token->data();
    const char *const last = first + token->size();
    const auto parsed = std::from_chars(first, last, value);
    if (parsed.ec != std::errc{} || parsed.ptr != last) return std::nullopt;
    return value;
}

[[nodiscard]] Result<std::vector<std::string>> read_ledger_lines(const std::string &path) {
    std::vector<std::string> lines;
    std::error_code ec;
    if (!fs::exists(fs::path{path}, ec)) return Ok(std::move(lines));
    std::ifstream in(path, std::ios::binary);
    if (!in) return Err(ErrorCode::IoError, "trial ledger: cannot open " + path);
    const std::string blob{std::istreambuf_iterator<char>(in),
                           std::istreambuf_iterator<char>()};
    if (in.bad()) return Err(ErrorCode::IoError, "trial ledger: read failed on " + path);
    if (blob.empty()) return Ok(std::move(lines));
    // A ledger is append-only and every append writes its LF, so a missing terminator is
    // a torn write or a hand truncation — a distinct fault from a broken link. The offset
    // of the end of the last COMPLETE line is reported so the manual repair is exact:
    // truncate the file to that many bytes, then regenerate the sidecar.
    if (blob.back() != '\n') {
        const auto last = blob.rfind('\n');
        const atx::usize keep = (last == std::string::npos) ? 0 : last + 1;
        return Err(ErrorCode::ParseError,
                   "trial ledger: truncated final line (no LF terminator) in " + path +
                       "; the last complete line ends at byte offset " +
                       std::to_string(keep) + " (truncate to that length, then regenerate " +
                       trial_ledger_manifest_path(path) + " to repair)");
    }
    atx::usize start = 0;
    while (start < blob.size()) {
        // Safe because blob ends with '\n': find always succeeds from any start < size().
        const auto stop = blob.find('\n', start);
        std::string line = blob.substr(start, stop - start);
        if (line.empty()) {
            return Err(ErrorCode::ParseError,
                       "trial ledger: empty line at index " + std::to_string(lines.size()));
        }
        if (line.find('\r') != std::string::npos) {
            return Err(ErrorCode::ParseError,
                       "trial ledger: CR in line " + std::to_string(lines.size()) +
                           " (LF endings only)");
        }
        lines.push_back(std::move(line));
        start = stop + 1;
    }
    return Ok(std::move(lines));
}

struct Walk {
    std::vector<std::string> lines;
    std::string head;
};

[[nodiscard]] Result<Walk> walk_chain(const std::string &path) {
    ATX_TRY(auto lines, read_ledger_lines(path));
    std::string previous{kTrialLedgerGenesisSha256};
    for (atx::usize index = 0; index < lines.size(); ++index) {
        const auto declared = scan_field(lines[index], "prev_sha256");
        if (!declared || !is_digest(*declared)) {
            return Err(ErrorCode::ParseError, "trial ledger: line " + std::to_string(index) +
                                                  " has no readable prev_sha256");
        }
        if (*declared != std::string_view{previous}) {
            return Err(ErrorCode::Internal,
                       "trial ledger: broken chain at line " + std::to_string(index) +
                           " (prev_sha256 " + std::string{*declared} + " does not match " +
                           previous + ")");
        }
        const std::string framed = lines[index] + "\n";
        ATX_TRY(auto digest, atx::core::sha256_hex(framed));
        previous = std::move(digest);
    }
    return Ok(Walk{std::move(lines), std::move(previous)});
}

struct ManifestRecord {
    atx::usize lines{};
    std::string head;
    std::string created_utc;
};

// Absent sidecar is not an error here — verified_walk decides what absence means.
[[nodiscard]] Result<std::optional<ManifestRecord>> read_manifest(const std::string &path) {
    std::error_code ec;
    if (!fs::exists(fs::path{path}, ec)) return Ok(std::optional<ManifestRecord>{});
    std::ifstream in(path, std::ios::binary);
    if (!in) return Err(ErrorCode::IoError, "trial ledger: cannot open sidecar " + path);
    const std::string blob{std::istreambuf_iterator<char>(in),
                           std::istreambuf_iterator<char>()};
    if (in.bad()) return Err(ErrorCode::IoError, "trial ledger: read failed on sidecar " + path);
    const auto lines = scan_int(blob, "lines");
    const auto head = scan_field(blob, "head_sha256");
    const auto created = scan_field(blob, "created_utc");
    if (!lines || *lines < 0 || !head || !is_digest(*head) || !created) {
        return Err(ErrorCode::ParseError, "trial ledger: malformed sidecar " + path);
    }
    ManifestRecord record{static_cast<atx::usize>(*lines), std::string{*head},
                          std::string{*created}};
    return Ok(std::optional<ManifestRecord>{std::move(record)});
}

// THE TAIL ANCHOR (ruling R-1). walk_chain compares line i against line i-1, so the LAST
// line's bytes are checked by nothing inside the ledger: an in-place edit of the newest
// line, or the deletion of the newest k lines, leaves a self-consistent chain. The
// sidecar is that missing anchor, so it is a REQUIRED input to verification rather than
// derived convenience: its `lines` and `head_sha256` are compared against the walk, and a
// sidecar missing beside a non-empty ledger is itself an error. Both files are git-tracked
// and the stage writes both, so the residual gap is exactly one case: an editor that
// changes the tail AND regenerates the sidecar consistently. That case is a documented
// limit, pinned by a test, not an undetected corruption.
[[nodiscard]] Result<Walk> verified_walk(const std::string &ledger_path) {
    ATX_TRY(auto walk, walk_chain(ledger_path));
    const std::string manifest = trial_ledger_manifest_path(ledger_path);
    ATX_TRY(const auto record, read_manifest(manifest));
    if (!record) {
        if (walk.lines.empty()) return Ok(std::move(walk));
        return Err(ErrorCode::NotFound,
                   "trial ledger: sidecar " + manifest + " is missing beside a ledger of " +
                       std::to_string(walk.lines.size()) +
                       " lines, so the tail cannot be anchored");
    }
    if (record->lines != walk.lines.size() || record->head != walk.head) {
        return Err(ErrorCode::Internal,
                   "trial ledger: sidecar records lines=" + std::to_string(record->lines) +
                       " head_sha256=" + record->head + " but the chain walks to lines=" +
                       std::to_string(walk.lines.size()) + " head_sha256=" + walk.head);
    }
    return Ok(std::move(walk));
}

// SINGLE-WRITER LOCK (ruling R-2). Two appenders that both verify before either writes
// read the same head, stamp the same prev_sha256, and the second line is a permanent
// break. `fopen(path, "wx")` is C11 exclusive create (UCRT honours the "x" mode), so
// creation IS the acquisition — there is no test-then-create window. By design this
// never waits and never removes a lock it did not create: a leftover lock means a writer
// died mid-append and a human should look at the ledger before the next write.
// SAFETY: the raw std::FILE* is owned solely by this RAII type, which is non-copyable
// and non-movable, so the handle is closed and the file removed on exactly one path.
class AppendLock {
public:
    explicit AppendLock(std::string path)
        : path_{std::move(path)}, handle_{std::fopen(path_.c_str(), "wx")} {}
    AppendLock(const AppendLock &) = delete;
    AppendLock &operator=(const AppendLock &) = delete;
    AppendLock(AppendLock &&) = delete;
    AppendLock &operator=(AppendLock &&) = delete;
    ~AppendLock() {
        if (handle_ == nullptr) return; // never touch a lock this object did not create
        // Nothing was written to the lock file, so a close failure carries no data loss
        // and there is no error channel out of a destructor to report it on.
        (void)std::fclose(handle_);
        std::error_code ec;
        fs::remove(fs::path{path_}, ec);
    }

    [[nodiscard]] bool held() const noexcept { return handle_ != nullptr; }

private:
    std::string path_;
    std::FILE *handle_{};
};

[[nodiscard]] Status ensure_parent_directory(const fs::path &path) {
    if (!path.has_parent_path()) return Ok();
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    if (!ec) return Ok();
    std::error_code probe;
    if (fs::is_directory(path.parent_path(), probe)) return Ok();
    return Err(ErrorCode::IoError,
               "trial ledger: cannot create directory " + path.parent_path().string());
}

// The sidecar is rewritten in full, never appended, and only AFTER the ledger line is
// flushed and its stream state checked — so a sidecar that names a head always names a
// head that is already on disk. The replacement writes a scratch neighbour and renames
// it over the target, so no open mode here can shorten the ledger or a published
// manifest (§8 T4 forbids a truncating open) and a crash mid-write leaves the previous
// manifest intact rather than a half-written one.
[[nodiscard]] Status write_manifest(const std::string &ledger_path, atx::usize lines,
                                    const std::string &head_sha256,
                                    const std::string &fallback_created_utc) {
    const std::string manifest = trial_ledger_manifest_path(ledger_path);
    ATX_TRY(const auto existing, read_manifest(manifest));
    const std::string created = existing ? existing->created_utc : fallback_created_utc;
    const std::string body = "{\"lines\":" + json_int(static_cast<atx::u64>(lines)) +
                             ",\"head_sha256\":" + json_text(head_sha256) +
                             ",\"created_utc\":" + json_text(created) + "}\n";
    const fs::path target{manifest};
    const fs::path staging = target.parent_path() / (target.filename().string() + ".staging");
    {
        std::ofstream out(staging, std::ios::binary);
        if (!out) return Err(ErrorCode::IoError, "trial ledger: cannot write " + manifest);
        out.write(body.data(), static_cast<std::streamsize>(body.size()));
        out.close();
        if (!out) return Err(ErrorCode::IoError, "trial ledger: manifest write failed");
    }
    std::error_code ec;
    fs::rename(staging, target, ec);
    if (ec) {
        std::error_code ignored;
        fs::remove(staging, ignored);
        return Err(ErrorCode::IoError, "trial ledger: cannot publish " + manifest);
    }
    return Ok();
}

} // namespace

std::string trial_ledger_manifest_path(const std::string &ledger_path) {
    constexpr std::string_view kSuffix = ".jsonl";
    const std::string_view view{ledger_path};
    if (view.size() > kSuffix.size() && view.ends_with(kSuffix)) {
        return ledger_path.substr(0, ledger_path.size() - kSuffix.size()) + ".manifest.json";
    }
    return ledger_path + ".manifest.json";
}

Result<std::string> serialize_trial_entry(const TrialLedgerEntry &entry) {
    ATX_TRY_VOID(validate_identity(entry));
    ATX_TRY_VOID(validate_body(entry));
    std::string out{"{\"schema\":"};
    out += json_text(entry.schema);
    out += ",\"trial_id\":" + json_text(entry.trial_id);
    out += ",\"appended_utc\":" + json_text(entry.appended_utc);
    out += ",\"prev_sha256\":" + json_text(entry.prev_sha256);
    out += ",\"checkpoint\":" + json_int(entry.checkpoint);
    out += ",\"purpose\":" + json_text(entry.purpose);
    out += ",\"status\":" + json_text(entry.status);
    out += ",\"trial_count_declared\":" + json_int(entry.trial_count_declared);
    out += ",\"parents\":" + parents_json(entry.parents);
    out += ",\"recipe\":" + recipe_json(entry.recipe);
    out += ",\"window\":{\"start\":" + json_text(entry.window.start);
    out += ",\"end_exclusive\":" + json_text(entry.window.end_exclusive);
    out += ",\"observations\":" + json_int(entry.window.observations) + "}";
    out += ",\"source_exclusions\":" + exclusions_json(entry.source_exclusions);
    out += ",\"fit_boundary\":{\"fit_kind\":" + json_text(entry.fit_boundary.fit_kind);
    out += ",\"fitted_observations\":" + json_int(entry.fit_boundary.fitted_observations) + "}";
    out += ",\"runtime\":{\"wall_seconds\":" +
           (entry.runtime.wall_seconds ? json_real(*entry.runtime.wall_seconds)
                                       : std::string{"null"});
    out += ",\"peak_working_set_bytes\":" +
           (entry.runtime.peak_working_set_bytes
                ? json_int(*entry.runtime.peak_working_set_bytes)
                : std::string{"null"}) +
           "}";
    out += ",\"result\":{\"outcome\":" + json_text(entry.result.outcome);
    out += ",\"manifest_sha256\":" + json_opt_text(entry.result.manifest_sha256);
    out += ",\"failure_sha256\":" + json_opt_text(entry.result.failure_sha256) + "}";
    out += ",\"producer_executable_sha256\":" + json_text(entry.producer_executable_sha256);
    out += ",\"notes\":" + json_text(entry.notes);
    out += '}';
    return Ok(std::move(out));
}

Result<TrialLedgerHead> verify_trial_ledger(const std::string &ledger_path) {
    ATX_TRY(auto walk, verified_walk(ledger_path));
    return Ok(TrialLedgerHead{walk.lines.size(), std::move(walk.head)});
}

Result<TrialLedgerEntry> append_trial(const std::string &ledger_path,
                                      const TrialLedgerEntry &entry) {
    const fs::path path{ledger_path};
    ATX_TRY_VOID(ensure_parent_directory(path));
    // The lock covers verify AND write: two appenders that verified before either wrote
    // would stamp the same prev_sha256 and the second line would be a permanent break.
    const std::string lock_path = ledger_path + ".lock";
    const AppendLock lock{lock_path};
    if (!lock.held()) {
        return Err(ErrorCode::Unavailable,
                   "trial ledger: another writer holds " + lock_path +
                       " (this API never waits and never removes a lock it did not "
                       "create; inspect the ledger, then delete the lock by hand)");
    }
    // §4.5: append "re-verifies the whole chain before writing and fails with
    // ErrorCode::Internal on a break". The verify error propagates unchanged, so a
    // refused append reports the same line index a bare verification would.
    ATX_TRY(const auto head, verify_trial_ledger(ledger_path));
    TrialLedgerEntry stamped = entry;
    stamped.prev_sha256 = head.head_sha256;
    ATX_TRY(const auto line, serialize_trial_entry(stamped));
    // NOT ATOMIC, and the comment must not claim otherwise (ruling R-3). std::ofstream
    // is not a syscall: filebuf flushes in buffer-sized chunks, so a line larger than the
    // buffer reaches the file as several OS writes, and a short write (ENOSPC, quota)
    // leaves those bytes behind. What IS guaranteed is fail-closed DETECTION: an
    // unterminated tail makes every later verify and append fail with ParseError, whose
    // message carries the byte offset to truncate to. Repair is manual and never done
    // here — silently discarding bytes from an append-only ledger is the one thing this
    // library must not do.
    std::ofstream out(path, std::ios::binary | std::ios::app);
    if (!out) return Err(ErrorCode::IoError, "trial ledger: cannot open " + ledger_path);
    const std::string framed = line + "\n";
    out.write(framed.data(), static_cast<std::streamsize>(framed.size()));
    out.flush();
    if (!out) return Err(ErrorCode::IoError, "trial ledger: append failed on " + ledger_path);
    out.close();
    if (!out) return Err(ErrorCode::IoError, "trial ledger: close failed on " + ledger_path);
    // Only now is the line durable, so only now may the sidecar name it as the head.
    ATX_TRY(const auto digest, atx::core::sha256_hex(framed));
    ATX_TRY_VOID(write_manifest(ledger_path, head.lines + 1, digest, stamped.appended_utc));
    return Ok(std::move(stamped));
}

Result<atx::u64> declared_trials_for_checkpoint(const std::string &ledger_path,
                                                atx::i64 checkpoint) {
    // verified_walk, not walk_chain: this function sums exactly the field a tail edit
    // would target (ruling R-1), so it must not answer from an unanchored chain.
    ATX_TRY(const auto walk, verified_walk(ledger_path));
    atx::u64 total = 0;
    bool found = false;
    for (atx::usize index = 0; index < walk.lines.size(); ++index) {
        const auto status = scan_field(walk.lines[index], "status");
        const auto line_checkpoint = scan_int(walk.lines[index], "checkpoint");
        const auto declared = scan_int(walk.lines[index], "trial_count_declared");
        if (!status || !line_checkpoint || !declared) {
            return Err(ErrorCode::ParseError,
                       "trial ledger: line " + std::to_string(index) +
                           " is not a readable atx-trial-ledger-v1 record");
        }
        if (*status != "pre-registered" || *line_checkpoint != checkpoint) continue;
        // cp15 §5.7: a non-trial purpose contributes 0 and still counts as found; a trial
        // purpose keeps the positive-only rule; a non-trial purpose declaring > 0 is as
        // unreadable as a trial purpose declaring 0 (§9.3 L5/L8).
        const auto purpose = scan_field(walk.lines[index], "purpose");
        const bool non_trial = purpose && is_non_trial_purpose(*purpose);
        if (*declared < 0 || (*declared == 0) != non_trial) {
            return Err(ErrorCode::ParseError,
                       "trial ledger: line " + std::to_string(index) +
                           (non_trial ? " declares a positive trial count for a non-trial purpose"
                                      : " declares a non-positive trial count"));
        }
        const auto addend = static_cast<atx::u64>(*declared);
        if (total > std::numeric_limits<atx::u64>::max() - addend) {
            return Err(ErrorCode::OutOfRange, "trial ledger: declared trial count overflow");
        }
        total += addend;
        found = true;
    }
    if (!found) {
        return Err(ErrorCode::NotFound, "trial ledger: no pre-registered line for checkpoint " +
                                            std::to_string(checkpoint));
    }
    return Ok(total);
}

bool is_non_trial_purpose(std::string_view purpose) {
    for (const std::string_view listed : kNonTrialPurposes) {
        if (listed == purpose) return true;
    }
    return false;
}

Result<atx::u64> pre_registered_lines_for_checkpoint(const std::string &ledger_path,
                                                     atx::i64 checkpoint) {
    // The same anchored walk as declared_trials_for_checkpoint: the ordinal a cp15 trial_id
    // is built from must not be answered from an unanchored chain either.
    ATX_TRY(const auto walk, verified_walk(ledger_path));
    atx::u64 count = 0;
    for (atx::usize index = 0; index < walk.lines.size(); ++index) {
        const auto status = scan_field(walk.lines[index], "status");
        const auto line_checkpoint = scan_int(walk.lines[index], "checkpoint");
        if (!status || !line_checkpoint) {
            return Err(ErrorCode::ParseError,
                       "trial ledger: line " + std::to_string(index) +
                           " is not a readable atx-trial-ledger-v1 record");
        }
        if (*status == "pre-registered" && *line_checkpoint == checkpoint) ++count;
    }
    return Ok(count);
}

} // namespace atx::impl
