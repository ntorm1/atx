#pragma once

// atx::engine::research::fields -- producer identity of engine-built field payloads (contract
// K-P9-3; fields review finding FD-1: the engine path used to record C++ payloads as produced by
// the Python builder).
//
// Every manifest entry whose payload this engine wrote carries
//   "producer": {"kind": "engine", "exe_sha256", "git_sha", "build_type", "receipt_sha256"}
// exe_sha256 is the SHA-256 of the running executable's bytes. git_sha and build_type are baked at
// configure time by the same rule as atx-impl's engine_git_sha: `git rev-parse HEAD` with "-dirty"
// appended for a modified tree, "unknown" without git; CMAKE_BUILD_TYPE, "unknown" when empty.
// receipt_sha256 is the SHA-256 of the receipt bytes of the run that wrote the payload. A producer
// block without "kind" is the legacy Python shape {module, code_sha256, ...} (ruling P6) and never
// an engine producer.
//
// What the identity claims: the payload bytes and their coverage are this executable's output. It
// makes no claim that a Python builder would write the same manifest. Reuse of an engine entry is
// keyed on {exe_sha256, git_sha, build_type} (reuse.hpp); an unknown exe_sha256 never matches.

#include <optional>
#include <string>
#include <string_view>

#include <nlohmann/json_fwd.hpp>

#include "atx/core/error.hpp"

namespace atx::engine::research::fields {

inline constexpr std::string_view kEngineProducerKind = "engine";
inline constexpr std::string_view kUnknownIdentity = "unknown";

struct ProducerIdentity {
  std::string exe_sha256;
  std::string git_sha;
  std::string build_type;

  friend bool operator==(const ProducerIdentity &, const ProducerIdentity &) = default;
};

// True when exe_sha256 is 64 lower-case hex digits: an identity a reuse decision may key on.
[[nodiscard]] bool is_known(const ProducerIdentity &identity) noexcept;

// The configure-time values (generated source build_identity.cpp.in).
[[nodiscard]] std::string_view build_git_sha() noexcept;
[[nodiscard]] std::string_view build_type_name() noexcept;

// This process: the SHA-256 of the running executable (read once, then cached) with the baked
// git_sha and build_type. exe_sha256 is "unknown" on a platform with no way to name the running
// executable. Err(IoError) when the executable cannot be named or read.
[[nodiscard]] core::Result<ProducerIdentity> current_producer();

// The K-P9-3 block {"kind": "engine", "exe_sha256", "git_sha", "build_type", "receipt_sha256"}.
[[nodiscard]] nlohmann::json producer_block(const ProducerIdentity &identity,
                                            std::string_view receipt_sha256);

// The engine identity of a manifest entry's producer block: nullopt when the entry carries no
// producer object or its producer has no "kind" (the legacy Python shape). Err(InvalidArgument)
// for a kind other than engine or an engine block whose identity keys are not strings.
[[nodiscard]] core::Result<std::optional<ProducerIdentity>>
engine_producer_of(const nlohmann::json &entry);

} // namespace atx::engine::research::fields
