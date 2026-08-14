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

#include "core/log.hpp"

#include "core/log_emit.hpp"
#include "atx/vol/research/listed_definitions_cache.hpp" // a real routed call site

#include "log_sink_probe.hpp" // CapturingSink / ScopedSink / StreamCapture

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

using atx::vol::testing::CapturingSink;
using atx::vol::testing::Record;
using atx::vol::testing::ScopedSink;
using atx::vol::testing::StreamCapture;

[[nodiscard]] fs::path scratch_file(std::string_view stem) {
  return atx::vol::testing::sink_scratch_file(stem);
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

// ── A real emitting path ────────────────────────────────────────────────────

// GATE, and the exit criterion's first half. Everything above drives the
// emitter directly, which proves the SEAM works but not that the library
// actually uses it. This drives a genuine production path —
// `read_listed_definitions_cached`, whose MISS line is one of the 13 routed
// sites — and asserts the record reaches the sink and NOT stderr.
//
// The fixture is deliberately minimal: the MISS line is emitted after the
// cache lookup fails but BEFORE the TSV is parsed, so any file contents at all
// drive the path. Whether the subsequent parse succeeds is irrelevant here and
// is covered by listed_definitions_cache_test.cpp.
//
// RED BEFORE ROUTING: with the seam in place but the site still calling
// std::fprintf(stderr, ...) directly, `captured` holds the MISS line and the
// sink holds nothing — both assertions below fail.
TEST(LogSinkRouting, DefinitionsCacheMissGoesToTheSinkNotStderr) {
  const fs::path dir = fs::temp_directory_path() / "atx-vol-log-sink-test" / "defs-miss";
  std::error_code ec;
  fs::remove_all(dir, ec);
  fs::create_directories(dir, ec);
  const fs::path tsv = dir / "definitions.tsv";
  const fs::path cache = dir / "cache";
  {
    std::ofstream out{tsv, std::ios::binary};
    out << "not-a-real-definitions-table\n";
  }

  CapturingSink sink;
  const fs::path capture_path = scratch_file("defs-miss-stderr");
  {
    StreamCapture capture{stderr, capture_path};
    {
      const ScopedSink installed{sink};
      static_cast<void>(read_listed_definitions_cached(tsv.string(), cache.string()));
    }
    const std::string captured = capture.release();
    EXPECT_EQ(captured, "") << "the library wrote to stderr despite an installed sink";
  }

  const std::vector<Record> records = sink.snapshot();
  ASSERT_GE(records.size(), 1u) << "no record reached the sink from a known-emitting path";

  bool saw_miss = false;
  for (const Record &r : records) {
    if (r.message.rfind("listed definitions cache: MISS", 0) == 0) {
      saw_miss = true;
      EXPECT_EQ(r.stream, LogStream::Stderr) << "the MISS line historically went to stderr";
      EXPECT_EQ(r.message.find('\n'), std::string::npos);
    }
  }
  EXPECT_TRUE(saw_miss) << "the definitions-cache MISS line did not reach the sink";
}

} // namespace atx::vol
