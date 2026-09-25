#pragma once

// Shared dead-alpha crowding wire (p9 S1, extracted in S2-0 for metabook reuse).
// resolve/open/collect the accumulating library::Library that build_risk_model
// augments into Kakushadze-Yu dead-alpha crowding factors. Consumed by BOTH
// stage_optimize.cpp (S1, the run_optimize path) AND stage_metabook.cpp (S2, the
// run_metabook / mega-book path) -- the two build_risk_model sites the p9 ROADMAP
// R3/R4 names. Header-only inline: both TUs already pull library.hpp; the fail-open
// contract (nullptr/{} => byte-identical no-op) lives entirely in maybe_open_dead_lib.
//
// W0-I0a adds two things here, both about aligning a library's own period axis with
// the research panel's date axis:
//   * the library split-range ledger (I-01): every discover run records the panel
//     ranges it trained and admitted on, and every combine run with a final test
//     records that test range, in `<library_dir>/_split_ranges.txt`. Discover refuses
//     to read a recorded final test; combine refuses a final test that overlaps any
//     recorded discover range. The same ledger tells the dead-alpha wire which panel
//     date the library's period 0 is (the discover holdout begin).
//   * DeadAlphaRule (I-06): "dead" now means Dead or Decaying as of each rebalance
//     step, mapped through the ledger. The old reading (every admitted alpha, as of
//     the library's last period) stays reachable as AdmittedAsOfLastPeriodV1.

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "config.hpp" // atx::impl::RunConfig

#include "atx/core/error.hpp"
#include "atx/core/types.hpp"
#include "atx/engine/combine/gate.hpp"          // combine::GateConfig, AlphaId
#include "atx/engine/library/library.hpp"       // library::Library
#include "atx/engine/library/lifecycle.hpp"     // library::LifecycleState
#include "atx/engine/risk/factor_model.hpp"     // risk::RiskModelConfig

namespace atx::impl {

namespace combine = atx::engine::combine;
namespace library = atx::engine::library;
namespace risk    = atx::engine::risk;

// S1 (p9): resolve the on-disk library directory the dead-alpha crowding wire
// reads from. --dead-alpha-lib-dir wins when set; otherwise fall back to the
// discover stage's own accumulating --library-dir (the "library dir already in
// the pipeline" the p9 ROADMAP names as S1's source). Neither set -> "" -> the
// caller's fail-open no-op (maybe_open_dead_lib below).
[[nodiscard]] inline std::string resolve_dead_alpha_lib_dir(const RunConfig& cfg) {
    return !cfg.dead_alpha_lib_dir.empty() ? cfg.dead_alpha_lib_dir : cfg.library_dir;
}

// S1 (p9): open the accumulating library for the dead-alpha-factor wire, or
// return nullopt on any of three fail-open conditions: (1) the augmentation
// gate itself is off; (2) no directory resolves anywhere; (3) the resolved
// directory does not exist yet (an operator ran --dead-alpha-factors before any
// --library-dir discover run created it -- a MISSING library is a documented
// no-op, not a hard failure: crowding defense is a risk-reduction enhancement,
// never a run-blocking dependency -- mirrors build_risk_model's own dead_lib==
// nullptr contract, stage_riskmodel.hpp:120-126). GateConfig{} is inert here:
// this handle only ever calls the READ methods (n_alphas/n_periods/
// state_as_of); the gate floors matter only to admit()/try_admit(), never
// invoked on this path.
[[nodiscard]] inline std::optional<library::Library>
maybe_open_dead_lib(const RunConfig& cfg, const risk::RiskModelConfig& risk_cfg) {
    if (!risk_cfg.dead_alpha_factors) {
        return std::nullopt;
    }
    const std::string dir = resolve_dead_alpha_lib_dir(cfg);
    if (dir.empty()) {
        return std::nullopt;
    }
    std::error_code ec;
    if (!std::filesystem::exists(dir, ec) || !std::filesystem::is_directory(dir, ec)) {
        return std::nullopt;
    }
    return library::Library::open(dir, combine::GateConfig{}, {cfg.seed});
}

// ===========================================================================
//  DeadAlphaRule — what "dead" means for the crowding factors (I-06).
// ===========================================================================
//  DeadOrDecayingPerStepV2 (default): at each rebalance step, the alphas whose
//  lifecycle state AS OF THAT STEP is Decaying or Dead. The step's panel date is
//  mapped onto the library's period axis through the split-range ledger
//  (library_as_of below); a step before the library's first period, or a library
//  whose axis is not recorded or cannot be located on the deploy panel
//  (library_period_axis), has no dead set (fail-open, like a missing library).
//  AdmittedAsOfLastPeriodV1: the pre-W0 reading -- every alpha not Candidate/Recycled
//  as of the library's LAST period, applied to every step. That set is the live pool,
//  not the dead one, and it is read from the end of the sample (look-ahead). Kept only
//  so frozen artifacts can be re-derived.
enum class DeadAlphaRule : atx::u8 {
    DeadOrDecayingPerStepV2 = 0,
    AdmittedAsOfLastPeriodV1 = 1,
};

// S1 (p9) / V1: the pre-W0 "admitted dead-alpha pool" -- every AlphaId the library
// already admitted as of `dead_as_of` (state NOT IN {Candidate, Recycled}).
// Ascending AlphaId order by construction; NOT load-bearing for
// extract_dead_factors' own bit-reproducibility (it re-sorts internally,
// dead_factor.hpp:194-198) -- see the DeadIdOrderInvariant proof (S1-2).
[[nodiscard]] inline std::vector<combine::AlphaId>
collect_dead_alpha_ids(const library::Library& lib, atx::usize dead_as_of) {
    std::vector<combine::AlphaId> ids;
    const atx::u64 n = lib.n_alphas();
    ids.reserve(static_cast<atx::usize>(n));
    for (atx::u64 a = 0; a < n; ++a) {
        const combine::AlphaId id{static_cast<atx::u32>(a)};
        const auto st = lib.state_as_of(id, dead_as_of);
        if (st.has_value() && *st != library::LifecycleState::Candidate &&
            *st != library::LifecycleState::Recycled) {
            ids.push_back(id);
        }
    }
    return ids;
}

// V2: the alphas whose lifecycle state as of library period `as_of` is Decaying or
// Dead. Ascending AlphaId order. Pure read of the journal (PIT: state_as_of never
// sees a transition recorded after `as_of`).
[[nodiscard]] inline std::vector<combine::AlphaId>
collect_dead_or_decaying_ids(const library::Library& lib, atx::usize as_of) {
    std::vector<combine::AlphaId> ids;
    const atx::u64 n = lib.n_alphas();
    for (atx::u64 a = 0; a < n; ++a) {
        const combine::AlphaId id{static_cast<atx::u32>(a)};
        const auto st = lib.state_as_of(id, as_of);
        if (st.has_value() && (*st == library::LifecycleState::Decaying ||
                               *st == library::LifecycleState::Dead)) {
            ids.push_back(id);
        }
    }
    return ids;
}

// ===========================================================================
//  Library split-range ledger (I-01).
// ===========================================================================
//  One record per line in `<library_dir>/_split_ranges.txt`:
//    split_range v=1 role=<r> axis=<session|index> n_dates=<N> begin=<b> end=<e>
//                begin_key=<k0> last_key=<k1>
//  [begin, end) are panel-date indices. On the session axis the inclusive session
//  keys [begin_key, last_key] identify the dates across panels; on the index axis
//  (unidentified legacy panels) the keys are 0 and only records with the same
//  n_dates are comparable.
enum class SplitRole : atx::u8 {
    DiscoverTrain = 0,   // the discover search/selection window
    DiscoverHoldout = 1, // the discover admission lockbox (the library's period axis)
    FinalTest = 2,       // a combine/report final test window
};

struct SplitRange {
    SplitRole role = SplitRole::DiscoverTrain;
    bool session_keyed = false;
    atx::usize n_dates = 0;
    atx::usize begin = 0; // half-open panel-date index range [begin, end)
    atx::usize end = 0;
    atx::i64 begin_key = 0; // inclusive session keys, valid iff session_keyed
    atx::i64 last_key = 0;

    [[nodiscard]] bool operator==(const SplitRange&) const noexcept = default;
};

inline constexpr std::string_view kSplitRangesFile = "_split_ranges.txt";

[[nodiscard]] inline std::string split_ranges_path(const std::string& lib_dir) {
    return (std::filesystem::path{lib_dir} / std::string{kSplitRangesFile}).string();
}

[[nodiscard]] inline std::string_view split_role_name(SplitRole r) noexcept {
    switch (r) {
    case SplitRole::DiscoverTrain:   return "discover_train";
    case SplitRole::DiscoverHoldout: return "discover_holdout";
    case SplitRole::FinalTest:       return "final_test";
    }
    return "discover_train"; // unreachable: every SplitRole handled above
}

// Build a range over panel dates [begin, end). `session_keys` is the panel's session
// key axis (empty for an unidentified panel -> index axis). Err when the range is
// empty, exceeds n_dates, or the key axis length disagrees with n_dates.
[[nodiscard]] inline atx::core::Result<SplitRange>
make_split_range(SplitRole role, atx::usize begin, atx::usize end, atx::usize n_dates,
                 std::span<const atx::i64> session_keys) {
    if (begin >= end || end > n_dates) {
        return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                              "split range: empty or out-of-panel range [" +
                                  std::to_string(begin) + "," + std::to_string(end) + ") of " +
                                  std::to_string(n_dates));
    }
    SplitRange r;
    r.role = role;
    r.n_dates = n_dates;
    r.begin = begin;
    r.end = end;
    if (!session_keys.empty()) {
        if (session_keys.size() != n_dates) {
            return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                                  "split range: session key axis length != panel dates");
        }
        r.session_keyed = true;
        r.begin_key = session_keys[begin];
        r.last_key = session_keys[end - 1U];
    }
    return atx::core::Ok(r);
}

namespace split_detail {

// Parse "key=value" -> value for `key` inside one ledger line. nullopt when absent.
[[nodiscard]] inline std::optional<std::string_view> field(std::string_view line,
                                                           std::string_view key) {
    atx::usize pos = 0;
    while (pos < line.size()) {
        const atx::usize sp = line.find(' ', pos);
        const std::string_view tok =
            line.substr(pos, (sp == std::string_view::npos) ? line.size() - pos : sp - pos);
        if (tok.size() > key.size() && tok.substr(0, key.size()) == key &&
            tok[key.size()] == '=') {
            return tok.substr(key.size() + 1U);
        }
        if (sp == std::string_view::npos) {
            break;
        }
        pos = sp + 1U;
    }
    return std::nullopt;
}

template <class T>
[[nodiscard]] std::optional<T> number(std::string_view line, std::string_view key) {
    const auto v = field(line, key);
    if (!v.has_value() || v->empty()) {
        return std::nullopt;
    }
    T out{};
    const auto [ptr, ec] = std::from_chars(v->data(), v->data() + v->size(), out);
    if (ec != std::errc{} || ptr != v->data() + v->size()) {
        return std::nullopt;
    }
    return out;
}

[[nodiscard]] inline atx::core::Result<SplitRange> parse_line(std::string_view line) {
    const auto role = field(line, "role");
    const auto axis = field(line, "axis");
    const auto n = number<atx::usize>(line, "n_dates");
    const auto b = number<atx::usize>(line, "begin");
    const auto e = number<atx::usize>(line, "end");
    const auto bk = number<atx::i64>(line, "begin_key");
    const auto lk = number<atx::i64>(line, "last_key");
    if (!role || !axis || !n || !b || !e || !bk || !lk || *b >= *e || *e > *n) {
        return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                              "split range ledger: malformed line: " + std::string{line});
    }
    SplitRange r;
    if (*role == "discover_train") {
        r.role = SplitRole::DiscoverTrain;
    } else if (*role == "discover_holdout") {
        r.role = SplitRole::DiscoverHoldout;
    } else if (*role == "final_test") {
        r.role = SplitRole::FinalTest;
    } else {
        return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                              "split range ledger: unknown role: " + std::string{*role});
    }
    if (*axis != "session" && *axis != "index") {
        return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                              "split range ledger: unknown axis: " + std::string{*axis});
    }
    r.session_keyed = (*axis == "session");
    r.n_dates = *n;
    r.begin = *b;
    r.end = *e;
    r.begin_key = *bk;
    r.last_key = *lk;
    return atx::core::Ok(r);
}

[[nodiscard]] inline std::string format_line(const SplitRange& r) {
    return "split_range v=1 role=" + std::string{split_role_name(r.role)} +
           " axis=" + (r.session_keyed ? "session" : "index") +
           " n_dates=" + std::to_string(r.n_dates) + " begin=" + std::to_string(r.begin) +
           " end=" + std::to_string(r.end) + " begin_key=" + std::to_string(r.begin_key) +
           " last_key=" + std::to_string(r.last_key);
}

} // namespace split_detail

// Read every recorded range. A missing ledger is Ok(empty) (a library written before
// W0, or a fresh one); a malformed line is Err (never silently skipped).
[[nodiscard]] inline atx::core::Result<std::vector<SplitRange>>
read_split_ranges(const std::string& lib_dir) {
    std::vector<SplitRange> out;
    const std::string path = split_ranges_path(lib_dir);
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) {
        return atx::core::Ok(std::move(out));
    }
    std::ifstream in{path};
    if (!in.is_open()) {
        return atx::core::Err(atx::core::ErrorCode::IoError,
                              "split range ledger: cannot open " + path);
    }
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.empty()) {
            continue;
        }
        ATX_TRY(auto r, split_detail::parse_line(line));
        out.push_back(r);
    }
    return atx::core::Ok(std::move(out));
}

// Append `r` unless an identical record is already present (a re-run on the same
// panel re-uses the same ranges; the ledger stays one line per distinct range).
[[nodiscard]] inline atx::core::Status append_split_range(const std::string& lib_dir,
                                                          const SplitRange& r) {
    ATX_TRY(auto existing, read_split_ranges(lib_dir));
    if (std::find(existing.begin(), existing.end(), r) != existing.end()) {
        return atx::core::Ok();
    }
    const std::string path = split_ranges_path(lib_dir);
    std::ofstream out{path, std::ios::app};
    if (!out.is_open()) {
        return atx::core::Err(atx::core::ErrorCode::IoError,
                              "split range ledger: cannot append to " + path);
    }
    out << split_detail::format_line(r) << '\n';
    out.close();
    if (!out) {
        return atx::core::Err(atx::core::ErrorCode::IoError,
                              "split range ledger: write failed: " + path);
    }
    return atx::core::Ok();
}

// Do `a` and `b` share at least one date? Session-keyed ranges compare their
// inclusive key intervals; index ranges compare [begin, end) and are comparable only
// on the same panel length. Err when the two ranges live on incomparable axes (the
// caller must refuse rather than guess).
[[nodiscard]] inline atx::core::Result<bool> split_ranges_overlap(const SplitRange& a,
                                                                  const SplitRange& b) {
    if (a.session_keyed && b.session_keyed) {
        return atx::core::Ok(a.begin_key <= b.last_key && b.begin_key <= a.last_key);
    }
    if (!a.session_keyed && !b.session_keyed && a.n_dates == b.n_dates) {
        return atx::core::Ok(a.begin < b.end && b.begin < a.end);
    }
    return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                          "split range ledger: cannot compare a " +
                              std::string{a.session_keyed ? "session" : "index"} +
                              "-axis range with a " +
                              (b.session_keyed ? "session" : "index") +
                              "-axis range recorded on a different panel");
}

// ===========================================================================
//  LibraryPeriodAxis — where the library's period 0 sits on the panel date axis.
// ===========================================================================
//  A library's stored pnl/positions cover the discover admission holdout, so library
//  period p is the holdout's p-th date. Known only when the ledger records exactly one
//  distinct holdout range whose length equals lib.n_periods() AND that range can be
//  located on the DEPLOY panel (the panel optimize/metabook is running on, which need
//  not be the panel discover recorded the range on):
//    * both axes session-keyed: the recorded begin key must be a deploy session key at
//      some index b, and the deploy key at b + len - 1 must be the recorded last key
//      (same first/last session, same session count) -> holdout_begin = b;
//    * both axes index-only (unidentified legacy panels): the deploy panel must have the
//      recorded n_dates -> holdout_begin = the recorded begin;
//    * anything else (mixed axes, a deploy key axis whose length is not deploy_n_dates, a
//      holdout the deploy panel does not contain) cannot be placed.
//  An axis that cannot be placed is unknown and the V2 dead set is empty (fail-open):
//  placing the library's periods by raw index on a panel with a different start would
//  put the dead set on the wrong dates -- early dates would read later lifecycle states.
struct LibraryPeriodAxis {
    bool known = false;
    atx::usize holdout_begin = 0; // on the DEPLOY panel's date axis
    atx::usize n_periods = 0;
};

[[nodiscard]] inline LibraryPeriodAxis
library_period_axis(const std::string& lib_dir, const library::Library& lib,
                    atx::usize deploy_n_dates, std::span<const atx::i64> deploy_session_keys) {
    LibraryPeriodAxis axis;
    auto ranges = read_split_ranges(lib_dir);
    if (!ranges.has_value()) {
        return axis;
    }
    std::optional<SplitRange> holdout;
    for (const SplitRange& r : *ranges) {
        if (r.role != SplitRole::DiscoverHoldout) {
            continue;
        }
        if (holdout.has_value() && !(*holdout == r)) {
            return axis; // several distinct holdouts: the period axis is ambiguous
        }
        holdout = r;
    }
    if (!holdout.has_value() || lib.n_periods() == 0U ||
        holdout->end - holdout->begin != lib.n_periods()) {
        return axis;
    }
    const atx::usize len = holdout->end - holdout->begin;
    const bool deploy_keyed = !deploy_session_keys.empty();
    if (deploy_keyed && deploy_session_keys.size() != deploy_n_dates) {
        return axis; // malformed deploy axis: cannot place anything on it
    }
    if (holdout->session_keyed != deploy_keyed) {
        return axis; // one side keyed, the other not: the two axes are incomparable
    }
    atx::usize begin = 0;
    if (deploy_keyed) {
        const auto it = std::find(deploy_session_keys.begin(), deploy_session_keys.end(),
                                  holdout->begin_key);
        if (it == deploy_session_keys.end()) {
            return axis; // the deploy panel does not contain the holdout's first session
        }
        begin = static_cast<atx::usize>(it - deploy_session_keys.begin());
        if (begin + len > deploy_n_dates ||
            deploy_session_keys[begin + len - 1U] != holdout->last_key) {
            return axis; // different session count between the holdout's endpoints
        }
    } else {
        if (holdout->n_dates != deploy_n_dates) {
            return axis; // a different unidentified panel: raw indices are not comparable
        }
        begin = holdout->begin;
    }
    axis.known = true;
    axis.holdout_begin = begin;
    axis.n_periods = lib.n_periods();
    return axis;
}

// Library period as of panel date `date` (PIT: the latest library period whose panel
// date is <= `date`). nullopt when the axis is unknown or `date` precedes period 0.
[[nodiscard]] inline std::optional<atx::usize> library_as_of(const LibraryPeriodAxis& axis,
                                                             atx::usize date) noexcept {
    if (!axis.known || axis.n_periods == 0U || date < axis.holdout_begin) {
        return std::nullopt;
    }
    return std::min(date - axis.holdout_begin, axis.n_periods - 1U);
}

// The dead set + library as-of period in force at panel date `date` under `rule`.
struct DeadSet {
    std::vector<combine::AlphaId> ids;
    atx::usize as_of = 0;
};

[[nodiscard]] inline DeadSet dead_set_at(const library::Library& lib, const LibraryPeriodAxis& axis,
                                         DeadAlphaRule rule, atx::usize date) {
    DeadSet out;
    if (rule == DeadAlphaRule::AdmittedAsOfLastPeriodV1) {
        out.as_of = lib.n_periods() > 0 ? lib.n_periods() - 1 : 0;
        out.ids = collect_dead_alpha_ids(lib, out.as_of);
        return out;
    }
    const std::optional<atx::usize> as_of = library_as_of(axis, date);
    if (!as_of.has_value()) {
        return out; // nothing known about the library as of this date
    }
    out.as_of = *as_of;
    out.ids = collect_dead_or_decaying_ids(lib, *as_of);
    return out;
}

} // namespace atx::impl
