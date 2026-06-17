#include "atx/core/io/zip_reader.hpp"

#include <cstring>

#include <miniz.h>

namespace atx::core::io {

struct ZipEntryReader::Impl {
  mz_zip_archive zip{};
  mz_zip_reader_extract_iter_state *iter{nullptr};
  mz_zip_archive_file_stat stat{};
  bool zip_open{false};

  ~Impl() {
    if (iter != nullptr) {
      mz_zip_reader_extract_iter_free(iter);
    }
    if (zip_open) {
      mz_zip_reader_end(&zip);
    }
  }
};

ZipEntryReader::ZipEntryReader(std::unique_ptr<Impl> p) noexcept : p_{std::move(p)} {}
ZipEntryReader::ZipEntryReader(ZipEntryReader &&) noexcept = default;
ZipEntryReader &ZipEntryReader::operator=(ZipEntryReader &&) noexcept = default;
ZipEntryReader::~ZipEntryReader() = default;

atx::core::Result<ZipEntryReader> ZipEntryReader::open(std::string_view zip_path,
                                                       std::string_view entry_name_substr) {
  auto impl = std::make_unique<Impl>();
  if (!mz_zip_reader_init_file(&impl->zip, std::string{zip_path}.c_str(), 0)) {
    return atx::core::Err(atx::core::ErrorCode::IoError,
                          std::string{"ZipEntryReader: cannot open "} + std::string{zip_path});
  }
  impl->zip_open = true;

  const mz_uint count = mz_zip_reader_get_num_files(&impl->zip);
  mz_uint chosen = count; // sentinel == not found
  for (mz_uint i = 0; i < count; ++i) {
    mz_zip_archive_file_stat st{};
    if (!mz_zip_reader_file_stat(&impl->zip, i, &st)) {
      continue;
    }
    if (st.m_is_directory != 0) {
      continue;
    }
    const std::string_view name{static_cast<const char *>(st.m_filename)};
    if (entry_name_substr.empty() || name.find(entry_name_substr) != std::string_view::npos) {
      chosen = i;
      impl->stat = st;
      break;
    }
  }
  if (chosen == count) {
    return atx::core::Err(atx::core::ErrorCode::NotFound,
                          std::string{"ZipEntryReader: no entry matching '"} +
                              std::string{entry_name_substr} + "'");
  }

  impl->iter = mz_zip_reader_extract_iter_new(&impl->zip, chosen, 0);
  if (impl->iter == nullptr) {
    return atx::core::Err(atx::core::ErrorCode::ParseError,
                          "ZipEntryReader: failed to start extraction iterator");
  }
  return atx::core::Ok(ZipEntryReader{std::move(impl)});
}

atx::core::Result<atx::usize> ZipEntryReader::read(std::span<char> dst) {
  const size_t got =
      mz_zip_reader_extract_iter_read(p_->iter, dst.data(), dst.size());
  // miniz signals an inflate error by setting iter->status < 0; a short read at EOF
  // simply returns fewer bytes, and a subsequent call returns 0.
  if (p_->iter->status < 0) {
    return atx::core::Err(atx::core::ErrorCode::ParseError,
                          "ZipEntryReader: inflate error mid-stream");
  }
  return atx::core::Ok(static_cast<atx::usize>(got));
}

atx::u64 ZipEntryReader::uncompressed_size() const noexcept {
  return static_cast<atx::u64>(p_->stat.m_uncomp_size);
}

std::string_view ZipEntryReader::entry_name() const noexcept {
  return std::string_view{static_cast<const char *>(p_->stat.m_filename)};
}

} // namespace atx::core::io
