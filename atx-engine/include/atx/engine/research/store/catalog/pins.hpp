#pragma once

// atx::engine::research::store::catalog -- pins (P9 SQL2; sql-design section 3.7, ruling SQL-7).
//
// A pin is a SHA-256 a holder states for a target, at an RFC 6901 pointer into the holder. The
// extractors (pins.cpp) read, per holder class:
//   spec            /inputs/<name>/sha256 (input; target inputs.<name>.path),
//                   /fields/manifest_sha256 (fields-manifest; target <fields.output>/manifest.json)
//   template        /locked/<name>/sha256 (locked), /change/inputs/<name>/sha256 (input)
//   wave manifest   /fields/<x>_sha256 (fields-manifest for manifest_sha256: <fields.dir>/
//                   manifest.json; else the sibling <x>), /rule_cell/<x>_sha256 (rule-template;
//                   target the sibling <x>)
//   run receipt     /bindings/<i>/sha256 (receipt-binding), /executable_sha256 (exe; target
//                   command[0]), /logs/<name> (receipt-log; target <run dir>/<name>)
//   cycle binding   /spec_sha256 (spec-digest: unresolved, research_spec.spec_digest is E2's)
//   cycle verdict   /spec_sha256 (spec-digest), /ledger/head (ledger-head: SQL4 checks it
//                   through B2's chain-head function): both unresolved
//   wave result     /manifest/sha256 (wave-manifest), /receipts/<stem> (stage-receipt; target
//                   <wave dir>/receipts/<stem>.json; unresolved under driver.receipt_digest
//                   "content", whose digests the catalog does not re-implement)
//   fields manifest /fields/<i>/sha256 and /files/<name>/sha256 (field-payload), /registry/sha256
//                   (field-registry), /fields/<i>/sources/<j>/sha256 (field-source: recorded,
//                   never followed)
// A pin's target_path is the target's path_key when the holder's path text names a file inside
// the catalog root, else NULL (unresolved). A sha256 value that is not 64 lower-case hex
// digits (an unlocked null pin) is not a pin.

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace atx::engine::research::store::catalog {

// True when a catalog walk opens the target of a pin of this kind: every kind but
// field-source (builder inputs outside the TRAIN outputs), spec-digest and ledger-head.
[[nodiscard]] bool follows_pin(std::string_view pin_kind) noexcept;

// The path_key a holder's path text names inside `root` (normal_root), or nullopt.
[[nodiscard]] std::optional<std::string> pin_target(const std::filesystem::path &root,
                                                    std::string_view text);

} // namespace atx::engine::research::store::catalog
