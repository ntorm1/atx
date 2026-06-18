#include "atx/core/io/zip_reader.hpp"

#include <cstring>
#include <string>

// Suppress miniz's zlib-compatible convenience typedefs (voidpc, alloc_func, …).
// We only use miniz's mz_zip_* API (whose types are mz_-prefixed), and under
// ATX_FAST_INFLATE zlib-ng.h defines the same zlib-compat names — including both
// otherwise collides with a typedef-redefinition error.
#define MINIZ_NO_ZLIB_COMPATIBLE_NAMES
#include <miniz.h>

#if ATX_FAST_INFLATE
#include <algorithm>
#include <cstdint>
#include <fstream>
#include <vector>

#include <zlib-ng.h>
#endif

namespace atx::core::io {

struct ZipEntryReader::Impl {
  mz_zip_archive zip{};
  mz_zip_reader_extract_iter_state *iter{nullptr}; // miniz inflate (default path / fast fallback)
  mz_zip_archive_file_stat stat{};
  bool zip_open{false};

#if ATX_FAST_INFLATE
  // zlib-ng streaming inflate state (only used when `fast` is true, i.e. the
  // entry is DEFLATE-compressed). miniz still parses the central directory above;
  // here we re-open the file, seek past the local header to the raw compressed
  // bytes, and stream them through zlib-ng — validating CRC32 at end-of-stream.
  bool fast{false};
  std::ifstream file;
  zng_stream strm{};
  bool strm_init{false};
  std::vector<std::uint8_t> inbuf;
  std::uint64_t comp_remaining{0}; // compressed bytes not yet read from the file
  std::uint32_t crc{0};            // running CRC32 of the inflated output
  std::uint32_t crc_expected{0};   // CRC32 from the central directory
  bool done{false};                // Z_STREAM_END reached AND CRC validated

  // Fill `dst` with the next inflated bytes via zlib-ng. Mirrors read()'s
  // contract: returns the count written (0 == end of entry), Err(ParseError) on
  // an inflate/CRC/truncation error.
  atx::core::Result<atx::usize> read_fast(std::span<char> dst) {
    if (done || dst.empty()) return atx::core::Ok(static_cast<atx::usize>(0));

    const std::size_t cap = std::min<std::size_t>(dst.size(), 0xFFFFFFFFu);
    strm.next_out = reinterpret_cast<std::uint8_t *>(dst.data());
    strm.avail_out = static_cast<std::uint32_t>(cap);

    int rc = Z_OK;
    while (strm.avail_out > 0) {
      if (strm.avail_in == 0 && comp_remaining > 0) {
        const std::size_t want =
            static_cast<std::size_t>(std::min<std::uint64_t>(inbuf.size(), comp_remaining));
        file.read(reinterpret_cast<char *>(inbuf.data()), static_cast<std::streamsize>(want));
        const std::streamsize got = file.gcount();
        if (got <= 0)
          return atx::core::Err(atx::core::ErrorCode::ParseError,
                                "ZipEntryReader(fast): short read of compressed data");
        comp_remaining -= static_cast<std::uint64_t>(got);
        strm.next_in = inbuf.data();
        strm.avail_in = static_cast<std::uint32_t>(got);
      }

      rc = zng_inflate(&strm, Z_NO_FLUSH);
      if (rc == Z_STREAM_END) break;
      if (rc == Z_BUF_ERROR) {
        // No progress possible. Legitimate only when zlib-ng has drained its
        // input and more compressed bytes remain on disk — refill next iteration.
        if (strm.avail_in == 0 && comp_remaining > 0) continue;
        return atx::core::Err(atx::core::ErrorCode::ParseError,
                              "ZipEntryReader(fast): truncated deflate stream");
      }
      if (rc != Z_OK)
        return atx::core::Err(atx::core::ErrorCode::ParseError,
                              "ZipEntryReader(fast): inflate error mid-stream");
    }

    const std::size_t produced = cap - strm.avail_out;
    crc = zng_crc32(crc, reinterpret_cast<const std::uint8_t *>(dst.data()),
                    static_cast<std::uint32_t>(produced));
    if (rc == Z_STREAM_END) {
      if (crc != crc_expected)
        return atx::core::Err(atx::core::ErrorCode::ParseError,
                              "ZipEntryReader(fast): CRC32 mismatch");
      done = true;
    }
    return atx::core::Ok(static_cast<atx::usize>(produced));
  }
#endif

  ~Impl() {
#if ATX_FAST_INFLATE
    if (strm_init) zng_inflateEnd(&strm);
#endif
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

#if ATX_FAST_INFLATE
  // DEFLATE entries (method 8) take the zlib-ng fast path; STORED/other methods
  // fall back to the miniz iterator so behavior is never silently wrong.
  if (impl->stat.m_method == 8) {
    impl->file.open(std::string{zip_path}, std::ios::binary);
    if (!impl->file) {
      return atx::core::Err(atx::core::ErrorCode::IoError,
                            "ZipEntryReader(fast): cannot reopen zip for streaming");
    }
    // The local file header is variable-length; its name/extra fields can differ
    // from the central directory, so read it from disk to find the data offset.
    impl->file.seekg(static_cast<std::streamoff>(impl->stat.m_local_header_ofs));
    unsigned char lh[30];
    impl->file.read(reinterpret_cast<char *>(lh), 30);
    if (impl->file.gcount() != 30) {
      return atx::core::Err(atx::core::ErrorCode::ParseError,
                            "ZipEntryReader(fast): short read of local header");
    }
    if (!(lh[0] == 0x50 && lh[1] == 0x4b && lh[2] == 0x03 && lh[3] == 0x04)) {
      return atx::core::Err(atx::core::ErrorCode::ParseError,
                            "ZipEntryReader(fast): bad local header signature");
    }
    const std::uint16_t nlen = static_cast<std::uint16_t>(lh[26] | (lh[27] << 8));
    const std::uint16_t elen = static_cast<std::uint16_t>(lh[28] | (lh[29] << 8));
    const std::uint64_t data_ofs =
        static_cast<std::uint64_t>(impl->stat.m_local_header_ofs) + 30u + nlen + elen;
    impl->file.seekg(static_cast<std::streamoff>(data_ofs));
    if (!impl->file) {
      return atx::core::Err(atx::core::ErrorCode::ParseError,
                            "ZipEntryReader(fast): cannot seek to compressed data");
    }

    impl->comp_remaining = static_cast<std::uint64_t>(impl->stat.m_comp_size);
    impl->crc_expected = static_cast<std::uint32_t>(impl->stat.m_crc32);
    impl->inbuf.resize(std::size_t{1} << 18); // 256 KiB compressed-read buffer
    if (zng_inflateInit2(&impl->strm, -15) != Z_OK) { // negative bits => raw deflate (no zlib hdr)
      return atx::core::Err(atx::core::ErrorCode::ParseError,
                            "ZipEntryReader(fast): zng_inflateInit2 failed");
    }
    impl->strm_init = true;
    impl->fast = true;
    return atx::core::Ok(ZipEntryReader{std::move(impl)});
  }
#endif

  impl->iter = mz_zip_reader_extract_iter_new(&impl->zip, chosen, 0);
  if (impl->iter == nullptr) {
    return atx::core::Err(atx::core::ErrorCode::ParseError,
                          "ZipEntryReader: failed to start extraction iterator");
  }
  return atx::core::Ok(ZipEntryReader{std::move(impl)});
}

atx::core::Result<atx::usize> ZipEntryReader::read(std::span<char> dst) {
#if ATX_FAST_INFLATE
  if (p_->fast) return p_->read_fast(dst);
#endif
  const size_t got = mz_zip_reader_extract_iter_read(p_->iter, dst.data(), dst.size());
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
