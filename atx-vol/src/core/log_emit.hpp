#pragma once

// Internal emit seam for the diagnostic sink (v1 plan item 5.4).
//
// The PUBLIC half of the seam is `atx/vol/log.hpp` (installation only). This is
// the half the library's own .cpp files call, and it is deliberately internal:
// nothing outside atx-vol should be emitting records that claim to come from
// atx-vol, and keeping the emitter out of the public surface means no Tier-A
// header ever has to include the logging seam.
//
// PRINTF-STYLE, ON PURPOSE. Every routed call site already had a printf format
// string and a byte-for-byte output contract (one of them, the corpus phase
// line, is parsed by operator scripts outside this repo). Keeping the same
// format strings and the same arguments is what makes "byte-identical by
// default" a mechanical property of the change rather than a claim to re-verify
// line by line.
//
// THE NEWLINE RULE. Format strings passed here must NOT end in "\n". One call is
// one record is one line; the trailing newline belongs to the default sink's
// rendering, and a sink that writes JSON or feeds a ring buffer should never
// have to strip it back off.
//
// Internal (`detail/`): no stability promise, not part of the frozen umbrella.

#include <cstdarg>

#include "core/log.hpp"

namespace atx::vol::detail {

// Format one record and hand it to the installed sink (or, with none installed,
// write it plus a newline to `stream` in a single fwrite).
//
// noexcept: this is a diagnostic path inside a no-exceptions library, and it is
// called from pool worker threads. It allocates only when a record exceeds the
// on-stack buffer, and on allocation failure it TRUNCATES rather than failing —
// losing the tail of a diagnostic is always preferable to perturbing the run
// that produced it.
void log_emitv(LogLevel level, LogStream stream, const char *fmt, std::va_list ap) noexcept;

#if defined(__clang__) || defined(__GNUC__)
#define ATX_VOL_LOG_PRINTF_LIKE(fmt_idx, first_arg)                                                \
  __attribute__((format(printf, fmt_idx, first_arg)))
#else
#define ATX_VOL_LOG_PRINTF_LIKE(fmt_idx, first_arg)
#endif

// Variadic front end for `log_emitv`. The format attribute makes the compiler
// check every routed call site's arguments against its format string, which is
// the cheap half of proving the reroute did not change any rendered text.
void log_emitf(LogLevel level, LogStream stream, const char *fmt, ...) noexcept
    ATX_VOL_LOG_PRINTF_LIKE(3, 4);

} // namespace atx::vol::detail
