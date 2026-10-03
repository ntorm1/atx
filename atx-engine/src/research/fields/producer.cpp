#include "atx/engine/research/fields/producer.hpp"

#include <algorithm>
#include <filesystem>
#include <system_error>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "atx/engine/research/fields/file_io.hpp"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#endif

namespace atx::engine::research::fields {
namespace {

using Json = nlohmann::json;
using MaybePath = std::optional<std::filesystem::path>;

// The running executable's path; nullopt on a platform with no way to name it.
[[nodiscard]] core::Result<MaybePath> executable_path() {
#if defined(_WIN32)
  std::vector<wchar_t> buffer(32768);
  const DWORD size = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
  if (size == 0 || size >= buffer.size()) {
    return core::Err(core::ErrorCode::IoError,
                     "research fields: cannot name the running executable");
  }
  return core::Ok(MaybePath(std::filesystem::path(std::wstring(buffer.data(), size))));
#elif defined(__linux__)
  std::error_code ec;
  auto path = std::filesystem::read_symlink("/proc/self/exe", ec);
  if (ec) {
    return core::Err(core::ErrorCode::IoError,
                     "research fields: cannot name the running executable");
  }
  return core::Ok(MaybePath(std::move(path)));
#else
  return core::Ok(MaybePath{});
#endif
}

[[nodiscard]] core::Result<std::string> executable_sha256() {
  ATX_TRY(const auto path, executable_path());
  if (!path) {
    return core::Ok(std::string(kUnknownIdentity));
  }
  ATX_TRY(const auto digest, digest_file(*path));
  return core::Ok(digest.sha256);
}

[[nodiscard]] std::string baked(std::string_view value) {
  return value.empty() ? std::string(kUnknownIdentity) : std::string(value);
}

} // namespace

bool is_known(const ProducerIdentity &identity) noexcept {
  const std::string &digest = identity.exe_sha256;
  return digest.size() == 64 && std::all_of(digest.begin(), digest.end(), [](char c) {
           return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
         });
}

core::Result<ProducerIdentity> current_producer() {
  // Hashed once per process (thread-safe static initialisation); the executable does not change
  // under a running process.
  static const core::Result<std::string> exe = executable_sha256();
  if (!exe) {
    return core::Err(exe.error());
  }
  return core::Ok(ProducerIdentity{*exe, baked(build_git_sha()), baked(build_type_name())});
}

Json producer_block(const ProducerIdentity &identity, std::string_view receipt_sha256) {
  return Json{{"kind", std::string(kEngineProducerKind)},
              {"exe_sha256", identity.exe_sha256},
              {"git_sha", identity.git_sha},
              {"build_type", identity.build_type},
              {"receipt_sha256", std::string(receipt_sha256)}};
}

core::Result<std::optional<ProducerIdentity>> engine_producer_of(const Json &entry) {
  using Out = std::optional<ProducerIdentity>;
  if (!entry.is_object()) {
    return core::Ok(Out{});
  }
  const auto producer = entry.find("producer");
  if (producer == entry.end() || !producer->is_object()) {
    return core::Ok(Out{});
  }
  const auto kind = producer->find("kind");
  if (kind == producer->end()) {
    return core::Ok(Out{}); // the legacy Python shape {module, code_sha256, ...}
  }
  if (!kind->is_string() || kind->get_ref<const std::string &>() != kEngineProducerKind) {
    return core::Err(core::ErrorCode::InvalidArgument,
                     "research fields: a producer kind other than engine: " + kind->dump());
  }
  const auto text = [&producer](const char *key) -> std::optional<std::string> {
    const auto it = producer->find(key);
    if (it == producer->end() || !it->is_string()) {
      return std::nullopt;
    }
    return it->get<std::string>();
  };
  auto exe = text("exe_sha256");
  auto git = text("git_sha");
  auto build = text("build_type");
  if (!exe || !git || !build) {
    return core::Err(core::ErrorCode::InvalidArgument,
                     "research fields: an engine producer block lacks exe_sha256, git_sha or "
                     "build_type");
  }
  return core::Ok(Out(ProducerIdentity{std::move(*exe), std::move(*git), std::move(*build)}));
}

} // namespace atx::engine::research::fields
