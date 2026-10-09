#pragma once
// absent-members-v1 (P9 lane ABS; rulings OD3P-1, ABS-1; contract od3p-absent-contract.md
// sections 2-5): the IC exe's opt-in list of library members that the pass treats, on every date,
// exactly as members whose data is missing (`--absent-members FILE --absent-members-sha256 HEX`).
// The file is the cycle's drop list itself (schema atx.od3-flagged-members/v1); the exe reads its
// `schema` and `members` only and ignores every other key (the rule keys are not composition
// inputs).
//
// Semantics (IC runner, strategy_ic_runner.cpp score_role): a listed member is evaluated, and its
// exact signal served from or stored in the candidate cache, as without the list; then every cell
// of its signal is replaced by a quiet NaN before anything reads it. Its IC rows, its TRAIN
// orientation (none: sign 0) and every composition rule therefore see a member with no data on any
// date, through the engine's existing missing-member rule (IcComposition: it adds nothing, the
// theme masses fixed at create from the pinned weights are untouched, so its mass stays in its
// theme and a theme with every member listed is a theme with no present member; the no-theme blend
// keeps its fixed denominator; v3 walk-forward rows alike). The pinned weights, signs, theme blocks
// and their verify are unchanged. Its IC result is never read from or written to the IC-result
// cache, whose entries key on the real signal bytes, so a cache shared by runs with and without the
// list never serves either an entry whose use would change its bytes. The list enters no cache key
// and not the recipe; summary.json records it (absent_members_record).
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include <nlohmann/json.hpp>
#include "atx/core/error.hpp"
#include "atx/core/types.hpp"

namespace atx::impl::strategy {
struct IcRunnerConfig;
namespace ic_detail { struct Library; }

inline constexpr std::string_view absent_members_rule = "absent-members-v1";
inline constexpr std::string_view absent_members_schema = "atx.od3-flagged-members/v1";

// A checked list: its pin, the member ids as listed, and per library candidate 1 when listed.
struct IcAbsentMembers {
  std::string list_sha256;
  std::vector<std::string> members;
  std::vector<atx::u8> absent; // library order
};

// The list a run applies: nullopt without --absent-members. Err(InvalidArgument), before any
// payload or output, for one of the two flags without the other; the flag with --no-composition
// (a screen builds no blend; the cycle never sends the pair); a SHA-256 pin that is not 64
// lowercase hex; a file missing, unreadable, empty or over 1 MiB (ic_detail::metadata_text); bytes
// whose SHA-256 is not the pin (bytes as read, no newline normalisation); text that is not a JSON
// object; `schema` other than absent_members_schema; `members` missing, not an array or empty, an
// entry that is not a string or does not match [A-Za-z0-9_]+, an id that is not a candidate of the
// library, or an id listed twice.
[[nodiscard]] atx::core::Result<std::optional<IcAbsentMembers>> absent_members(
    const IcRunnerConfig& cfg, const ic_detail::Library& lib);

// summary.json's top-level record: {rule, list_sha256, members (as listed)}.
[[nodiscard]] nlohmann::json absent_members_record(const IcAbsentMembers& list);
} // namespace atx::impl::strategy
