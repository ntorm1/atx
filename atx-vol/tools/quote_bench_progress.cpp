#include "quote_bench_progress.hpp"

#include "atx/core/sha256.hpp"

#include <cstddef>
#include <fcntl.h>
#include <stdexcept>
#include <sys/stat.h>

#ifdef _WIN32
#include <io.h>
#include <share.h>
#else
#include <unistd.h>
#endif

namespace quote_bench {

void ProgressJournal::CloseFile::operator()(std::FILE *file) const noexcept {
  if (file != nullptr) std::fclose(file);
}

ProgressJournal::ProgressJournal(const std::filesystem::path &path) {
  if (path.empty()) return;
  int descriptor = -1;
#ifdef _WIN32
  const auto error = _wsopen_s(&descriptor, path.c_str(), _O_CREAT | _O_EXCL | _O_WRONLY |
      _O_BINARY, _SH_DENYWR, _S_IREAD | _S_IWRITE);
  if (error != 0) throw std::runtime_error("Cannot exclusively create progress journal");
  file_.reset(_wfdopen(descriptor, L"wb"));
  if (!file_) _close(descriptor);
#else
  descriptor = ::open(path.c_str(), O_CREAT | O_EXCL | O_WRONLY, S_IRUSR | S_IWUSR);
  if (descriptor < 0) throw std::runtime_error("Cannot exclusively create progress journal");
  file_.reset(::fdopen(descriptor, "wb"));
  if (!file_) ::close(descriptor);
#endif
  if (!file_) throw std::runtime_error("Cannot open progress journal stream");
}

void ProgressJournal::append(const nlohmann::json &record) {
  if (!file_) return;
  constexpr std::size_t max_record_bytes = 4U * 1024U * 1024U;
  std::string line = record.dump();
  if (line.size() >= max_record_bytes) {
    throw std::runtime_error("Progress record exceeds 4 MiB bound");
  }
  line.push_back('\n');
  if (std::fwrite(line.data(), 1U, line.size(), file_.get()) != line.size() ||
      std::fflush(file_.get()) != 0) {
    throw std::runtime_error("Progress journal write/flush failed");
  }
#ifdef _WIN32
  const auto committed = _commit(_fileno(file_.get()));
#else
  const auto committed = ::fsync(::fileno(file_.get()));
#endif
  if (committed != 0) throw std::runtime_error("Progress journal commit failed");
}

std::string file_sha256(const std::filesystem::path &path) {
  const auto digest = atx::core::sha256_file(path.string());
  if (!digest) throw std::runtime_error(digest.error().to_string());
  return *digest;
}

std::string json_sha256(const nlohmann::json &value) {
  const auto digest = atx::core::sha256_hex(value.dump());
  if (!digest) throw std::runtime_error(digest.error().to_string());
  return *digest;
}

} // namespace quote_bench
