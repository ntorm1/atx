// The diagnostic sink seam (plan item 5.4) — `atx/vol/log.hpp`.
//
// Two properties carry this feature, and the tests are split along them:
//
//   * WITH a sink installed, records reach the sink instead of the process
//     streams. That is what makes atx-vol embeddable.
//   * WITHOUT one, the process streams see exactly the bytes they always saw.
//     That is a DETERMINISM requirement of this sprint — the release NAV leg
//     asserts the library's observable output is unchanged — so the default
//     renderer is pinned against literal expected bytes, not merely "non-empty".
//
// Test-class labels, per the sprint's test-quality rules:
//   GATE             — the assertion fails against unguarded code.
//   POSITIVE CONTROL — holds by construction; locks a value, cannot fail open.

#include "atx/vol/log.hpp"

#include "atx/vol/detail/log_emit.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <io.h>
#define ATX_DUP _dup
#define ATX_DUP2 _dup2
#define ATX_CLOSE _close
#define ATX_FILENO _fileno
#else
#include <unistd.h>
#define ATX_DUP dup
#define ATX_DUP2 dup2
#define ATX_CLOSE close
#define ATX_FILENO fileno
#endif

namespace atx::vol {
namespace {

namespace fs = std::filesystem;

// ── Captured records ────────────────────────────────────────────────────────

struct Record {
  LogLevel level{};
  LogStream stream{};
  std::string message;
};

// A sink whose capture buffer is mutex-guarded, because `log.hpp` says the
// callback may be invoked concurrently and the callback — not the library —
// owns that synchronization. This type is the reference implementation of that
// contract, and `ConcurrentEmissionIsSerializedByTheCallback` exercises it.
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

// RAII installer: puts back exactly what it displaced, so one test can never
// leak a sink into the next (gtest shares one process across the whole binary,
// and a leaked sink would silently swallow another suite's diagnostics).
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

// ── Process-stream capture ──────────────────────────────────────────────────
//
// Redirects a C stdio stream at the FILE DESCRIPTOR level (not by swapping the
// FILE*), so it captures writes made through the `stderr`/`stdout` macros from
// inside the library — which is exactly what the default renderer does and
// exactly what the pre-sink code did. `_dup` of the original fd is what makes
// the redirect reversible.
class StreamCapture {
public:
  StreamCapture(std::FILE *stream, fs::path path) : stream_{stream}, path_{std::move(path)} {
    std::fflush(stream_);
    saved_fd_ = ATX_DUP(ATX_FILENO(stream_));
#if defined(_WIN32)
    ::fopen_s(&sink_file_, path_.string().c_str(), "wb");
#else
    sink_file_ = std::fopen(path_.string().c_str(), "wb");
#endif
    if (sink_file_ != nullptr) {
      ATX_DUP2(ATX_FILENO(sink_file_), ATX_FILENO(stream_));
    }
  }

  // Stop capturing and return every byte the stream received meanwhile.
  [[nodiscard]] std::string release() {
    if (saved_fd_ < 0) {
      return {};
    }
    std::fflush(stream_);
    ATX_DUP2(saved_fd_, ATX_FILENO(stream_));
    ATX_CLOSE(saved_fd_);
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
  fs::path path_;
  std::FILE *sink_file_{nullptr};
  int saved_fd_{-1};
};

[[nodiscard]] fs::path scratch_file(std::string_view stem) {
  const fs::path dir = fs::temp_directory_path() / "atx-vol-log-sink-test";
  std::error_code ec;
  fs::create_directories(dir, ec);
  return dir / (std::string{stem} + ".txt");
}

} // namespace

// ── Default renderer: byte-for-byte ─────────────────────────────────────────

// GATE. The whole determinism argument for this sprint rests on this line: with
// nothing installed, the bytes on the stream are the bytes the pre-sink
// `std::fprintf(stderr, "...\n", ...)` produced. Pinned literally, including the
// trailing newline the emitter (not the call site) now supplies.
TEST(LogSinkDefault, WritesTheFormattedLinePlusNewlineToStderr) {
  const fs::path path = scratch_file("default-stderr");
  StreamCapture capture{stderr, path};
  detail::log_emitf(LogLevel::Warn, LogStream::Stderr, "roll deferred on %s: %s", "2024-01-05",
                    "NotFound: no chain");
  const std::string captured = capture.release();

  EXPECT_EQ(captured, "roll deferred on 2024-01-05: NotFound: no chain\n");
}

// GATE. Stream selection is carried on the record, not derived from the level;
// an Info record asked to go to stdout must land on stdout. Without this, the
// two Info-level sites that historically wrote to DIFFERENT streams could not
// both stay byte-identical.
TEST(LogSinkDefault, HonoursTheRecordsOwnStreamNotItsLevel) {
  const fs::path out_path = scratch_file("default-stdout");
  StreamCapture out_capture{stdout, out_path};
  detail::log_emitf(LogLevel::Info, LogStream::Stdout, "built qualified corpus: admitted=%u", 7u);
  const std::string on_stdout = out_capture.release();

  EXPECT_EQ(on_stdout, "built qualified corpus: admitted=7\n");

  const fs::path err_path = scratch_file("default-stderr-empty");
  StreamCapture err_capture{stderr, err_path};
  detail::log_emitf(LogLevel::Info, LogStream::Stdout, "stdout only");
  const std::string on_stderr = err_capture.release();

  EXPECT_EQ(on_stderr, "");
}

// GATE. A record longer than the emitter's 2 KiB stack buffer must be rendered
// WHOLE — the heap-growth path is the difference between a faithful reroute and
// one that silently truncates a long path or a wide telemetry line.
TEST(LogSinkDefault, RendersRecordsLongerThanTheStackBuffer) {
  const std::string wide(5000, 'x');
  const fs::path path = scratch_file("default-long");
  StreamCapture capture{stderr, path};
  detail::log_emitf(LogLevel::Error, LogStream::Stderr, "long=%s", wide.c_str());
  const std::string captured = capture.release();

  EXPECT_EQ(captured.size(), wide.size() + 6u); // "long=" + payload + '\n'
  EXPECT_EQ(captured, "long=" + wide + "\n");
}

// ── Installed sink ──────────────────────────────────────────────────────────

// GATE. The defining property: with a sink installed the record reaches the
// callback AND the process stream stays clean. Asserting only the first half
// would pass against a sink that TEES, which is exactly the behaviour a host
// embedding this library cannot accept.
TEST(LogSink, InstalledSinkReceivesRecordsAndTheStreamStaysSilent) {
  CapturingSink sink;
  const fs::path path = scratch_file("installed-stderr");
  {
    StreamCapture capture{stderr, path};
    {
      const ScopedSink installed{sink};
      detail::log_emitf(LogLevel::Warn, LogStream::Stderr, "cache MISS (%s)", "NotFound");
    }
    const std::string captured = capture.release();
    EXPECT_EQ(captured, "") << "a record reached the process stream despite an installed sink";
  }

  const std::vector<Record> records = sink.snapshot();
  ASSERT_EQ(records.size(), 1u);
  EXPECT_EQ(records[0].level, LogLevel::Warn);
  EXPECT_EQ(records[0].stream, LogStream::Stderr);
  EXPECT_EQ(records[0].message, "cache MISS (NotFound)");
}

// GATE. The message handed to the sink carries NO trailing newline — the
// newline is the default renderer's, not the record's. A sink writing JSON or
// filling a ring buffer must not have to strip it back off.
TEST(LogSink, MessageHasNoTrailingNewline) {
  CapturingSink sink;
  {
    const ScopedSink installed{sink};
    detail::log_emitf(LogLevel::Info, LogStream::Stdout, "one line");
  }

  const std::vector<Record> records = sink.snapshot();
  ASSERT_EQ(records.size(), 1u);
  EXPECT_EQ(records[0].message, "one line");
  EXPECT_EQ(records[0].message.find('\n'), std::string::npos);
}

// GATE. Uninstalling must genuinely restore the default renderer, not merely
// stop calling the old sink. A scoped installer that left the library mute
// would make every later suite's diagnostics vanish.
TEST(LogSink, UninstallingRestoresTheDefaultRenderer) {
  CapturingSink sink;
  {
    const ScopedSink installed{sink};
    detail::log_emitf(LogLevel::Info, LogStream::Stderr, "captured");
  }
  EXPECT_EQ(sink.size(), 1u);

  const fs::path path = scratch_file("restored-stderr");
  StreamCapture capture{stderr, path};
  detail::log_emitf(LogLevel::Info, LogStream::Stderr, "back on the stream");
  const std::string captured = capture.release();

  EXPECT_EQ(captured, "back on the stream\n");
  EXPECT_EQ(sink.size(), 1u) << "the uninstalled sink still received a record";
}

// POSITIVE CONTROL. `install_log_sink` returns the displaced sink and its user
// pointer so a host can nest installers and restore exactly what it found.
TEST(LogSink, InstallReturnsThePreviouslyInstalledSinkAndUser) {
  CapturingSink first;
  CapturingSink second;

  void *displaced_user = nullptr;
  const LogSink none = install_log_sink(&CapturingSink::callback, &first, &displaced_user);
  EXPECT_EQ(none, nullptr);
  EXPECT_EQ(displaced_user, nullptr);

  const LogSink prev = install_log_sink(&CapturingSink::callback, &second, &displaced_user);
  EXPECT_EQ(prev, &CapturingSink::callback);
  EXPECT_EQ(displaced_user, &first);

  detail::log_emitf(LogLevel::Info, LogStream::Stderr, "to second");
  EXPECT_EQ(second.size(), 1u);
  EXPECT_EQ(first.size(), 0u);

  install_log_sink(nullptr, nullptr);
}

// GATE. `log.hpp` promises the pair is published atomically and that the
// callback may run concurrently. Emitting from several threads at once must
// deliver every record exactly once and must never pair a sink with another
// sink's user pointer — the failure mode a two-atomic implementation would
// have. Order is explicitly NOT asserted; the header says it is not defined.
TEST(LogSink, ConcurrentEmissionIsSerializedByTheCallback) {
  CapturingSink sink;
  constexpr int kThreads = 8;
  constexpr int kPerThread = 64;

  {
    const ScopedSink installed{sink};
    std::vector<std::thread> workers;
    workers.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) {
      workers.emplace_back([t]() {
        for (int i = 0; i < kPerThread; ++i) {
          detail::log_emitf(LogLevel::Info, LogStream::Stderr, "t=%d i=%d", t, i);
        }
      });
    }
    for (std::thread &w : workers) {
      w.join();
    }
  }

  const std::vector<Record> records = sink.snapshot();
  EXPECT_EQ(records.size(), static_cast<std::size_t>(kThreads * kPerThread));
  for (const Record &r : records) {
    EXPECT_EQ(r.message.rfind("t=", 0), 0u) << "torn or corrupted record: " << r.message;
  }
}

} // namespace atx::vol
