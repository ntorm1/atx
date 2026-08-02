#pragma once

// Diagnostic-output seam (v1 plan item 5.4).
//
// atx-vol is a LIBRARY, and a library that writes to the process's stdout/stderr
// behind its host's back is a library the host cannot embed. Every diagnostic
// line the library emits is routed through the one sink installed here, so a
// host can capture, re-level, re-format, or silence all of it.
//
// THE DEFAULT IS THE OLD BEHAVIOUR, BYTE FOR BYTE. With no sink installed each
// record goes to the same stream (`stdout` vs `stderr`) it always went to, with
// the same text and the same trailing newline. A consumer that installs nothing
// sees no difference at all — that is a determinism requirement of this sprint,
// not merely a courtesy, and `LogSinkDefault.*` pins it.
//
// WHAT THIS IS NOT. There is no formatting DSL, no logger hierarchy, no
// compile-time level filtering, and no spdlog in the public surface. The library
// hands you a finished line and says how severe it is; every policy decision —
// including whether to filter on `LogLevel` at all — belongs to the host.
//
// Tier-B: public and includable, deliberately outside the frozen `vol.hpp`
// umbrella. No Tier-A header includes it (the emit seam is `detail/log_emit.hpp`,
// which only .cpp files include), so installing a sink is opt-in and the frozen
// v1 surface does not grow a logging dependency.

#include <cstdint>
#include <string_view>

namespace atx::vol {

// ── Severity ────────────────────────────────────────────────────────────────
//
// Three levels, because the emitting sites only ever distinguish three things:
// something failed, something was degraded/skipped, or something is progress
// and timing telemetry. The level is ADVISORY — it never decides which stream
// the default sink uses (see `LogStream`).
enum class LogLevel : std::uint8_t {
  // A failure the caller is being told about but which is NOT returned as an
  // error — e.g. a definitions-cache publish that failed and left the run
  // correct but uncached.
  Error = 0,
  // Something was degraded, deferred, or dropped, and the run continued with
  // less than it wanted — a dropped fit slice, a deferred roll.
  Warn = 1,
  // Progress, completion summaries, and timing/telemetry lines.
  Info = 2,
};

// ── Destination ─────────────────────────────────────────────────────────────
//
// Carried EXPLICITLY rather than derived from `LogLevel`, because the historical
// stream choice is not a function of severity and byte-identical default output
// is a hard requirement: the definitions-cache HIT line is Info-on-stderr while
// the corpus-build summary is Info-on-stdout. Deriving the stream from the level
// would silently move one of them. A sink that wants a single ordered log can
// simply ignore this field.
enum class LogStream : std::uint8_t {
  Stdout = 0,
  Stderr = 1,
};

// ── The sink ────────────────────────────────────────────────────────────────
//
// A plain function pointer plus an opaque `user` pointer, NOT a std::function:
// installation must not allocate, must not throw, and must be publishable as a
// single word so the emit path is a relaxed load and a call.
//
// `message` is ONE complete line WITHOUT a trailing newline, valid only for the
// duration of the call — copy it if you keep it. It is never null and never has
// embedded newlines.
//
// CONTRACT — the callback is user code running inside a no-exceptions library:
//   * It MUST NOT throw. The emit path is `noexcept`, so an escaping exception
//     calls std::terminate rather than unwinding through library frames that do
//     not offer an exception guarantee. This is deliberate: the alternative is a
//     silent swallow, and `.agents/cpp/agent.md` forbids the empty catch.
//   * It MUST NOT re-enter atx-vol. Emission happens at arbitrary points inside
//     runs, including while internal locks are held.
//   * It MUST tolerate being called concurrently (see below).
using LogSink = void (*)(LogLevel level, LogStream stream, std::string_view message,
                         void *user) noexcept;

// ── Thread-safety ───────────────────────────────────────────────────────────
//
// This header declares process-global mutable state — the second such seam in
// the library, after `detail/pricing_executor.hpp` — so its rules are stated
// rather than inferred.
//
// EMISSION IS CONCURRENT, AND YOUR CALLBACK MUST BE. The pricing pool is a
// process-global singleton and its worker threads run library code that emits;
// a fit fan-out therefore produces log records on threads the host never
// created. Two consequences the callback author owns:
//   * The callback may run on SEVERAL threads AT ONCE. Any state it touches
//     (a file handle, a std::vector<std::string> capture buffer, a counter) must
//     be synchronized BY THE CALLBACK. The library adds no lock of its own —
//     `.agents/cpp/agent.md` forbids holding a lock across a callback, and doing
//     so here would serialize every worker behind the host's logging.
//   * RECORD ORDER IS NOT DETERMINISTIC across threads, and neither is the
//     interleaving of records with any writes the host makes itself. Within one
//     thread, records arrive in emission order. A test that asserts on captured
//     output must therefore assert on SET membership, not on sequence, unless it
//     drives a single-threaded path.
//
// INSTALLATION IS A CONFIGURATION-TIME OPERATION. `install_log_sink` publishes
// the new sink atomically, so an emitting thread always sees either the complete
// old sink or the complete new one — never a torn pair. What it does NOT give
// you is an ORDERING guarantee: install concurrently with a run in flight and
// records already in the emit path may go to either sink. Install once, before
// the first library call that can emit, or while no run is in flight — the same
// discipline `configure_pricing_executor` asks for. Uninstalling (passing
// nullptr) has exactly the same rule.
//
// The default sink itself is safe to call from any thread: it issues one
// `std::fwrite` of the whole assembled line plus newline, so C stdio's own
// locking keeps a record from interleaving with another record mid-line.

// Install `sink`, which receives every subsequent diagnostic record along with
// `user` verbatim. Passing nullptr restores the default (write to the record's
// own stream). Returns the previously installed sink, so a scoped installer can
// put back exactly what it displaced; `prev_user` receives the matching user
// pointer when non-null.
LogSink install_log_sink(LogSink sink, void *user = nullptr, void **prev_user = nullptr) noexcept;

} // namespace atx::vol
