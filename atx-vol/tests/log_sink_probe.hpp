#pragma once

// Test-only probes for the diagnostic sink (`atx/vol/log.hpp`).
//
// Shared by log_sink_test.cpp (which tests the seam itself) and backtest_test.cpp
// (whose exit-criterion test acts as a HOST: capturing every library diagnostic
// while cancelling a running backtest). Test support only — never shipped, never
// included by library code.

#include "atx/vol/log.hpp"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#if defined(_WIN32)
#include <io.h>
#define ATX_VOL_TEST_DUP _dup
#define ATX_VOL_TEST_DUP2 _dup2
#define ATX_VOL_TEST_CLOSE _close
#define ATX_VOL_TEST_FILENO _fileno
#else
#include <unistd.h>
#define ATX_VOL_TEST_DUP dup
#define ATX_VOL_TEST_DUP2 dup2
#define ATX_VOL_TEST_CLOSE close
#define ATX_VOL_TEST_FILENO fileno
#endif

namespace atx::vol::testing {

struct Record {
  LogLevel level{};
  LogStream stream{};
  std::string message;
};

// A sink whose capture buffer is mutex-guarded, because `log.hpp` says the
// callback may be invoked concurrently and the CALLBACK — not the library — owns
// that synchronization. This is the reference implementation of that contract.
class CapturingSink {
public:
  static void callback(LogLevel level, LogStream stream, std::string_view message,
                       void *user) noexcept {
    auto *self = static_cast<CapturingSink *>(user);
    const std::lock_guard<std::mutex> lock{self->mu_};
    self->records_.push_back(Record{level, stream, std::string{message}});
  }

  [[nodiscard]] std::vector<Record> snapshot() {
    const std::lock_guard<std::mutex> lock{mu_};
    return records_;
  }

  [[nodiscard]] std::size_t size() {
    const std::lock_guard<std::mutex> lock{mu_};
    return records_.size();
  }

private:
  std::mutex mu_;
  std::vector<Record> records_;
};

// RAII installer: puts back exactly what it displaced, so one test can never leak
// a sink into the next (gtest shares one process across the whole binary, and a
// leaked sink would silently swallow another suite's diagnostics).
class ScopedSink {
public:
  explicit ScopedSink(CapturingSink &sink)
      : prev_{install_log_sink(&CapturingSink::callback, &sink, &prev_user_)} {}
  ~ScopedSink() { install_log_sink(prev_, prev_user_); }

  ScopedSink(const ScopedSink &) = delete;
  ScopedSink &operator=(const ScopedSink &) = delete;

private:
  void *prev_user_{nullptr};
  LogSink prev_{nullptr};
};

// Redirects a C stdio stream at the FILE DESCRIPTOR level (not by swapping the
// FILE*), so it captures writes made through the `stderr`/`stdout` macros from
// inside the library — exactly what the default renderer does and exactly what
// the pre-sink code did. `_dup` of the original fd makes the redirect reversible.
class StreamCapture {
public:
  StreamCapture(std::FILE *stream, std::filesystem::path path)
      : stream_{stream}, path_{std::move(path)} {
    std::fflush(stream_);
    saved_fd_ = ATX_VOL_TEST_DUP(ATX_VOL_TEST_FILENO(stream_));
#if defined(_WIN32)
    ::fopen_s(&sink_file_, path_.string().c_str(), "wb");
#else
    sink_file_ = std::fopen(path_.string().c_str(), "wb");
#endif
    if (sink_file_ != nullptr) {
      ATX_VOL_TEST_DUP2(ATX_VOL_TEST_FILENO(sink_file_), ATX_VOL_TEST_FILENO(stream_));
    }
  }

  // Stop capturing and return every byte the stream received meanwhile.
  [[nodiscard]] std::string release() {
    if (saved_fd_ < 0) {
      return {};
    }
    std::fflush(stream_);
    ATX_VOL_TEST_DUP2(saved_fd_, ATX_VOL_TEST_FILENO(stream_));
    ATX_VOL_TEST_CLOSE(saved_fd_);
    saved_fd_ = -1;
    if (sink_file_ != nullptr) {
      std::fclose(sink_file_);
      sink_file_ = nullptr;
    }
    std::ifstream in{path_, std::ios::binary};
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
  }

  ~StreamCapture() { static_cast<void>(release()); }

  StreamCapture(const StreamCapture &) = delete;
  StreamCapture &operator=(const StreamCapture &) = delete;

private:
  std::FILE *stream_{nullptr};
  std::filesystem::path path_;
  std::FILE *sink_file_{nullptr};
  int saved_fd_{-1};
};

[[nodiscard]] inline std::filesystem::path sink_scratch_file(std::string_view stem) {
  const std::filesystem::path dir =
      std::filesystem::temp_directory_path() / "atx-vol-log-sink-test";
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  return dir / (std::string{stem} + ".txt");
}

} // namespace atx::vol::testing
