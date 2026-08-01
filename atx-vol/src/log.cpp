// Diagnostic sink: installation, and the default byte-for-byte renderer.
//
// See `atx/vol/log.hpp` for the contract this implements and `detail/log_emit.hpp`
// for the internal emit seam.

#include "atx/vol/detail/log_emit.hpp"

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string_view>

namespace atx::vol {
namespace {

// The installed sink and its user pointer, as ONE atomically-published value.
//
// Two separate atomics would be the obvious implementation and would be wrong:
// an emitting thread could load a NEW function pointer beside the OLD user
// pointer and hand a fresh callback a stale context. Publishing the pair as a
// single `std::atomic` makes the header's "never a torn pair" claim true. The
// pair is 16 bytes, so this may be a locked atomic rather than a lock-free one
// on some targets; that cost is irrelevant on a path that runs a handful of
// times per run, and the lock is never held across the callback.
struct SinkPair {
  LogSink fn{nullptr};
  void *user{nullptr};
};

std::atomic<SinkPair> g_sink{SinkPair{}};

// Stream selection for the default renderer. This mapping IS the byte-identity
// contract: it reproduces exactly which of the two process streams each call
// site historically wrote to.
[[nodiscard]] std::FILE *default_stream(LogStream stream) noexcept {
  return stream == LogStream::Stderr ? stderr : stdout;
}

// Render one already-formatted record the way the pre-sink code did: the line
// followed by '\n', in ONE fwrite so C stdio's stream lock keeps concurrent
// records from interleaving mid-line.
void write_default(LogStream stream, const char *line_with_newline, std::size_t n) noexcept {
  std::FILE *const out = default_stream(stream);
  static_cast<void>(std::fwrite(line_with_newline, 1, n, out));
}

} // namespace

LogSink install_log_sink(LogSink sink, void *user, void **prev_user) noexcept {
  const SinkPair previous = g_sink.exchange(SinkPair{sink, user}, std::memory_order_acq_rel);
  if (prev_user != nullptr) {
    *prev_user = previous.user;
  }
  return previous.fn;
}

namespace detail {

void log_emitv(LogLevel level, LogStream stream, const char *fmt, std::va_list ap) noexcept {
  // One buffer serves both renderings: the sink sees [0, n) and the default
  // renderer writes [0, n] with a '\n' stuck on the end, so neither path copies
  // the message a second time just to add or remove a newline.
  char stack_buf[2048];
  char *buf = stack_buf;
  std::size_t cap = sizeof stack_buf;
  char *heap = nullptr;

  std::va_list measure;
  va_copy(measure, ap);
  const int needed = std::vsnprintf(buf, cap, fmt, measure);
  va_end(measure);

  if (needed < 0) {
    return; // encoding error: drop the record rather than emit garbage
  }

  std::size_t n = static_cast<std::size_t>(needed);
  // `needed + 1` for '\n'; vsnprintf also wants room for its own NUL, so the
  // buffer must hold n + 2.
  if (n + 2u > cap) {
    heap = static_cast<char *>(std::malloc(n + 2u));
    if (heap != nullptr) {
      buf = heap;
      cap = n + 2u;
      const int wrote = std::vsnprintf(buf, cap, fmt, ap);
      if (wrote < 0) {
        std::free(heap);
        return;
      }
      n = static_cast<std::size_t>(wrote);
    } else {
      // Allocation failed. Emit the truncated prefix vsnprintf already produced
      // rather than perturb the run: cap - 1 chars plus the NUL it wrote.
      n = cap - 2u;
    }
  }

  buf[n] = '\n';
  buf[n + 1u] = '\0';

  const SinkPair active = g_sink.load(std::memory_order_acquire);
  if (active.fn == nullptr) {
    write_default(stream, buf, n + 1u);
  } else {
    active.fn(level, stream, std::string_view{buf, n}, active.user);
  }

  if (heap != nullptr) {
    std::free(heap);
  }
}

void log_emitf(LogLevel level, LogStream stream, const char *fmt, ...) noexcept {
  std::va_list ap;
  va_start(ap, fmt);
  log_emitv(level, stream, fmt, ap);
  va_end(ap);
}

} // namespace detail
} // namespace atx::vol
