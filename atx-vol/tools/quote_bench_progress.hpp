#pragma once

#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>

#include <nlohmann/json.hpp>

namespace quote_bench {

// Offline, fresh-file-only journal. Each bounded JSON line is flushed and
// committed before returning. A missing completion record means incomplete;
// only newline-terminated records form the durable prefix after interruption.
class ProgressJournal {
public:
  explicit ProgressJournal(const std::filesystem::path &path);
  void append(const nlohmann::json &record);

private:
  struct CloseFile {
    void operator()(std::FILE *file) const noexcept;
  };
  std::unique_ptr<std::FILE, CloseFile> file_;
};

[[nodiscard]] std::string file_sha256(const std::filesystem::path &path);
[[nodiscard]] std::string json_sha256(const nlohmann::json &value);

} // namespace quote_bench
