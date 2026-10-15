#pragma once

// The `sign` verb of atx-equity-strategy-ic (platform P9 lane SIGN, ruling C3-11): a research
// wave's sign rule applied to the rows of its admission screen by the engine's one predicate
// (research/admission/sign_rule.hpp: pm7_35 under kPm735SignRule). It retires the Python copy of
// the rule (scripts/wave_rules.py sign_pm7_35); the wave's screen stage runs it on the IC exe its
// screen spec already pins (exes.ic), so no new pinned exe.
//
//   sign --rule pm7-35 --rows FILE --output NEWFILE
//
// --rule: the wave manifest's sign_rule id. pm7-35 = pm7_35(kPm735SignRule, ...).
// FILE: a JSON object whose "rows" array lists one row per screened candidate, in wave order:
//   {"id": TEXT, "kind": "add" | "replace", "prior_sign": 1 | -1, "status": TEXT,
//    "runner_sign": -1 | 0 | 1}
// (the manifest's candidate with its row of the fit's admission.json). Other keys are ignored, so
// the screen block of a wave result (whose rows also carry the recorded decision and reason) is
// accepted as it stands. A row is admitted iff its status is "admitted"
// (screen.hpp status_name(Status::Admitted)); any other non-empty status is not admitted.
// NEWFILE (exclusive; never overwritten) receives the receipt (kSignReceiptSchema): the rule, the
// predicate's sign rule, FILE's path and SHA-256 (of its bytes), every row with its verdict
// ("keep" | "drop") and reason, and the kept and dropped ids in wave order.
//
// Exit 0: the receipt is written. Exit 1: refused, before any output -- FILE unreadable or over
// 16 MiB; not a JSON object with a non-empty "rows" array; a row that is not an object, lacks a
// key (a row without status or runner_sign is a string the screen did not score: a stop, never a
// drop) or holds a mistyped or out-of-range value (an unknown kind among them); a duplicate id;
// NEWFILE existing. Exit 2: usage -- an unknown, repeated or missing flag, an unknown rule id.

#include <iosfwd>
#include <string_view>

namespace atx::impl::strategy {
inline constexpr std::string_view kSignReceiptSchema = "atx.sign-receipt/v1";

// argv[0] is the "sign" verb (atx-equity-strategy-ic's verb table). See the header comment.
[[nodiscard]] int dispatch_sign(int argc, char** argv, std::ostream& out, std::ostream& err);
} // namespace atx::impl::strategy
