#include "atx/engine/parallel/process_executor.hpp"

#include <atomic>
#include <charconv> // std::from_chars (worker-id parse)
#include <cstddef>
#include <cstdint> // std::uintptr_t (cursor alignment guard)
#include <cstdlib> // std::getenv
#include <cstring> // std::memcpy, std::memset
#include <span>
#include <string>
#include <system_error> // std::errc (from_chars result)
#include <vector>

#include "atx/core/error.hpp"
#include "atx/core/macro.hpp"
#include "atx/core/types.hpp"

#include "atx/engine/parallel/executor.hpp"
#include "atx/engine/parallel/shm_segment.hpp"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
// NOLINTBEGIN(misc-include-cleaner) — windows.h is the Win32 umbrella; its
// sub-headers (processthreadsapi.h, synchapi.h, libloaderapi.h, …) are not
// self-contained on all SDK versions, so we include the umbrella. All Win32
// symbol warnings in the blocks below are suppressed for this reason.
#include <windows.h>
#else
#include <cerrno>  // errno, EINTR (waitpid retry)
#include <spawn.h> // posix_spawn
#include <sys/wait.h>
#include <unistd.h> // readlink, access, sysconf, getpid
extern char **environ; // the spawned child inherits the parent environment
#endif

namespace atx::engine::parallel {

using atx::core::Err;
using atx::core::ErrorCode;
using atx::core::Result;
using atx::core::Status;

namespace {

// Name of the dedicated worker executable, beside the current module.
#if defined(_WIN32)
constexpr const char *kWorkerExeName = "atx-shm-worker.exe";
#else
constexpr const char *kWorkerExeName = "atx-shm-worker";
#endif

// Resolve a requested worker count: 0 => max(1, hw_concurrency - 2) — the same
// bandwidth-knee default DetPool uses (we replicate it rather than spin up a
// throwaway DetPool, which would create OS threads just to read a number).
[[nodiscard]] atx::usize resolve_workers(atx::usize requested) noexcept {
  if (requested != 0) {
    return requested;
  }
#if defined(_WIN32)
  SYSTEM_INFO si{};
  GetSystemInfo(&si);
  const atx::usize hw = static_cast<atx::usize>(si.dwNumberOfProcessors);
#else
  const long n = ::sysconf(_SC_NPROCESSORS_ONLN);
  const atx::usize hw = (n > 0) ? static_cast<atx::usize>(n) : 0;
#endif
  return (hw <= 3) ? atx::usize{1} : hw - 2;
}

// A process-unique segment-name stem: "atx-s7-<pid>-<seq>". The per-submit
// monotonic counter keeps names distinct within one process (concurrent ctest
// shards each run their own pid, so cross-process collisions cannot occur).
[[nodiscard]] std::string name_stem() {
  static std::atomic<unsigned> seq{0};
#if defined(_WIN32)
  const unsigned long pid = GetCurrentProcessId();
#else
  const unsigned long pid = static_cast<unsigned long>(::getpid());
#endif
  std::string s = "atx-s7-";
  s += std::to_string(pid);
  s += '-';
  s += std::to_string(seq.fetch_add(1, std::memory_order_relaxed));
  return s;
}

// Copy a NUL-terminated name into a fixed wire field, rejecting an over-long
// name (the field must stay NUL-terminated for the worker to read it back).
[[nodiscard]] Status set_name_field(char (&field)[256], const std::string &name) noexcept {
  if (name.size() + 1U > sizeof(field)) {
    return Err(ErrorCode::InvalidArgument, "ProcessExecutor: segment name too long for wire field");
  }
  std::memset(field, 0, sizeof(field));
  std::memcpy(field, name.data(), name.size());
  return atx::core::Ok();
}

// The three segments + their names, created together and freed together by RAII.
struct Segments {
  ShmSegment input;
  ShmSegment output;
  ShmSegment control;
  std::string input_name;
  std::string output_name;
  std::string control_name;
};

// Create the input (read-only payload, sized max(1, input_bytes)), output (the
// pre-indexed slot region), and control (ControlBlock + ErrorSlot[]) segments.
[[nodiscard]] Result<Segments> make_segments(InputView inputs, SlotView out, atx::usize n_workers) {
  Segments segs;
  const std::string stem = name_stem();
  segs.input_name = stem + "-in";
  segs.output_name = stem + "-out";
  segs.control_name = stem + "-ctl";

  // ShmSegment rejects bytes==0, so a zero-length payload is sized as one byte
  // (the ControlBlock carries the true logical input_bytes the worker slices to).
  const atx::usize in_bytes = inputs.bytes.empty() ? atx::usize{1} : inputs.bytes.size();
  ATX_TRY(segs.input, ShmSegment::create(segs.input_name, in_bytes));
  ATX_TRY(segs.output, ShmSegment::create(segs.output_name, out.bytes.size()));
  ATX_TRY(segs.control, ShmSegment::create(segs.control_name, control_segment_bytes(n_workers)));
  return segs;
}

// Fill the input segment, zero the output, and stamp the ControlBlock + zeroed
// ErrorSlots. After this returns the control segment is the single source of
// truth the workers read; nothing else is shared.
[[nodiscard]] Status fill_segments(Segments &segs, WorkloadId workload, InputView inputs,
                                   atx::usize n, atx::usize n_workers, SlotView out) {
  // Input: copy the caller's logical bytes (the rest of a padded 1-byte segment
  // is never read — the worker slices to input_bytes).
  if (!inputs.bytes.empty()) {
    std::span<std::byte> dst = segs.input.view_rw();
    ATX_ASSERT(dst.size() >= inputs.bytes.size());
    std::memcpy(dst.data(), inputs.bytes.data(), inputs.bytes.size());
  }
  // Output: zero it so untouched padding is deterministic (the gather copies the
  // whole segment back; padding bytes must not be uninitialised garbage).
  std::span<std::byte> obuf = segs.output.view_rw();
  std::memset(obuf.data(), 0, obuf.size());

  // Control: stamp the block, then zero the ErrorSlot array.
  std::span<std::byte> cbuf = segs.control.view_rw();
  ATX_ASSERT(cbuf.size() >= control_segment_bytes(n_workers));
  // SAFETY: cbuf is the control segment the parent just created at >= sizeof(ControlBlock);
  // the first sizeof(ControlBlock) bytes are the block. We construct it in place via a
  // typed pointer then memcpy is unnecessary (placement over zeroed shared memory of a
  // trivially-copyable standard-layout type is well defined). The reinterpret_cast is the
  // standard shared-memory typed-view idiom, required to address the block.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
  auto *ctrl = reinterpret_cast<ControlBlock *>(cbuf.data());
  std::memset(ctrl, 0, sizeof(*ctrl));
  ctrl->magic = kControlMagic;
  // n_workers narrows to the u32 wire field; it is resolved from the hardware
  // concurrency (a small count), so this is never tight, but guard the contract.
  ATX_ASSERT(n_workers <= 0xFFFFFFFFULL);
  ctrl->n_workers = static_cast<atx::u32>(n_workers);
  ctrl->n_shards = static_cast<atx::u64>(n);
  ctrl->workload = static_cast<atx::u32>(workload);
  ctrl->input_bytes = static_cast<atx::u64>(inputs.bytes.size());
  ctrl->slot_stride = static_cast<atx::u64>(out.slot_stride);
  ctrl->slot_size = static_cast<atx::u64>(out.slot_size);
  ATX_TRY_VOID(set_name_field(ctrl->input_name, segs.input_name));
  ATX_TRY_VOID(set_name_field(ctrl->output_name, segs.output_name));
  ctrl->claim_cursor = 0;

  // ErrorSlot[n_workers] immediately after the block — already zeroed by the
  // segment's create() (we created it fresh), but zero explicitly for clarity.
  ErrorSlot *slots = error_slots(cbuf);
  std::memset(slots, 0, n_workers * sizeof(ErrorSlot));
  return atx::core::Ok();
}

// A worker that exited abnormally (nonzero code, or — POSIX — killed by a signal)
// BEFORE the barrier. Detected by the parent from the OS wait, independently of
// the worker's ErrorSlot: a crash/OOM-kill/early-_exit leaves has_error==0, so
// this is the ONLY signal that the gathered output is not trustworthy.
struct AbnormalExit {
  atx::usize worker;  // the lowest-index worker that exited abnormally
  atx::i64 code;      // its exit code (or the signal number for a POSIX kill)
  bool by_signal;     // true => `code` is a signal number (POSIX); false => an exit code
};

// ---------------------------------------------------------------------------
// Worker-exe path discovery: <dir of the current module>/atx-shm-worker[.exe],
// overridable via the ATX_SHM_WORKER env var (a full path) for test robustness.
// ---------------------------------------------------------------------------
[[nodiscard]] Result<std::string> worker_exe_path();

// Spawn all workers, barrier on their exit, and report the lowest-index worker
// that exited abnormally (nonzero code / killed by signal) into `*abnormal`
// (left untouched if every worker exited cleanly). On a SPAWN failure the
// already-spawned children are waited on (no orphan/leak) before returning Err.
// A non-Ok return means the OS wait machinery itself failed; an abnormal child
// exit is NOT an Err here (it is folded into the deterministic failure reduction
// by the caller) — it is reported via `*abnormal`.
[[nodiscard]] Status spawn_and_wait(const std::string &exe, const std::string &control_name,
                                    atx::usize n_workers, AbnormalExit *abnormal);

// ---------------------------------------------------------------------------
// Parent-side reduce + gather (OS-agnostic).
// ---------------------------------------------------------------------------

// Reduce the GLOBAL lowest failed shard across all workers' ErrorSlots. Returns
// Ok() if every worker reported clean, else Err(code-of-the-lowest, "shard <id>
// failed") — the deterministic global-lowest-id error (DetPool's rethrow_lowest,
// across the process boundary; independent of worker count or which worker failed).
[[nodiscard]] Status reduce_error_slots(std::span<std::byte> cbuf, atx::usize n_workers) {
  const ErrorSlot *slots = error_slots(cbuf);
  bool found = false;
  atx::u64 lowest = 0;
  atx::u32 code = 0;
  for (atx::usize w = 0; w < n_workers; ++w) {
    if (slots[w].has_error != 0 && (!found || slots[w].lowest_failed_shard < lowest)) {
      found = true;
      lowest = slots[w].lowest_failed_shard;
      code = slots[w].code;
    }
  }
  if (!found) {
    return atx::core::Ok();
  }
  std::string msg = "ProcessExecutor: shard ";
  msg += std::to_string(lowest);
  msg += " failed";
  return Err(static_cast<ErrorCode>(static_cast<atx::u16>(code)), std::move(msg));
}

// The FAILURE CONTRACT (deterministic): a run failed iff (a) any worker set its
// ErrorSlot, OR (b) any worker exited abnormally. (a) is preferred because it
// carries the deterministic global-lowest SHARD id (the digest-invariant error a
// re-run reproduces); (b) is the silent-corruption guard — a worker that crashed
// before writing its ErrorSlot still fails the run rather than returning a
// half-written output as Ok(). Both reductions pick the lowest index, so neither
// depends on which worker finished first.
[[nodiscard]] Status reduce_failures(std::span<std::byte> cbuf, atx::usize n_workers,
                                     const AbnormalExit *abnormal) {
  // Prefer the deterministic shard-level error when a worker reported one.
  Status slot_err = reduce_error_slots(cbuf, n_workers);
  if (!slot_err) {
    return slot_err;
  }
  // No ErrorSlot set, but a worker died without reporting => surface that, naming
  // the lowest-index offender and its OS exit/signal (the corruption tripwire).
  if (abnormal != nullptr) {
    std::string msg = "ProcessExecutor: worker ";
    msg += std::to_string(abnormal->worker);
    msg += abnormal->by_signal ? " killed by signal " : " exited abnormally (code ";
    msg += std::to_string(abnormal->code);
    msg += abnormal->by_signal ? "" : ")";
    msg += " without reporting";
    return Err(ErrorCode::Internal, std::move(msg));
  }
  return atx::core::Ok();
}

} // namespace

// ---------------------------------------------------------------------------
// ProcessExecutor::submit — the parent orchestration (kept short; the work is in
// the helpers above so this reads as the 8-step recipe from the brief).
// ---------------------------------------------------------------------------
ProcessExecutor::ProcessExecutor(ExecutorConfig c) noexcept : n_workers_{resolve_workers(c.workers)} {}

Status ProcessExecutor::submit(WorkloadId workload, InputView inputs, atx::usize n, SlotView out,
                               std::span<const ShardId> dispatch_order) {
  // S7-4: dispatch_order is accepted for seam parity but ignored here — the
  // cross-process claim cursor already dispenses ids dynamically; honouring the
  // permutation across the control segment is deferred to S7.6 (the digest is
  // invariant to dispatch order regardless, so this costs only tail-shortening,
  // never a bit). The ThreadExecutor honours the order and proves no bit contact.
  (void)dispatch_order;
  if (out.n() != n) {
    return Err(ErrorCode::InvalidArgument, "ProcessExecutor: SlotView slot count != n");
  }
  if (n == 0) {
    return atx::core::Ok(); // no shards => no segments, no spawn, no barrier
  }
  // Cap workers to n (no idle worker should hang waiting on an empty dispenser —
  // a capped worker simply finds the cursor already >= n and exits clean).
  const atx::usize n_workers = (n_workers_ < n) ? n_workers_ : n;

  ATX_TRY(const std::string exe, worker_exe_path());
  ATX_TRY(Segments segs, make_segments(inputs, out, n_workers));
  ATX_TRY_VOID(fill_segments(segs, workload, inputs, n, n_workers, out));

  // Spawn + barrier: blocks until every worker process has exited. Captures the
  // lowest-index abnormal exit (crash / nonzero code) so a worker that died
  // before writing its ErrorSlot cannot masquerade as success.
  bool had_abnormal = false;
  AbnormalExit abnormal{};
  {
    AbnormalExit found{};
    found.worker = static_cast<atx::usize>(-1);
    ATX_TRY_VOID(spawn_and_wait(exe, segs.control_name, n_workers, &found));
    if (found.worker != static_cast<atx::usize>(-1)) {
      had_abnormal = true;
      abnormal = found;
    }
  }

  // Reduce to the deterministic failure: a set ErrorSlot (preferred — carries the
  // global-lowest shard id) or an abnormal exit (the corruption tripwire). On
  // failure, surface it; the half-written output is intentionally NOT gathered.
  ATX_TRY_VOID(reduce_failures(segs.control.view_rw(), n_workers, had_abnormal ? &abnormal : nullptr));

  // Gather: copy the output slot bytes back into the caller's buffer (one O(out)
  // copy, in the PARENT; the caller digests from here — workers never hash, §0.3).
  std::span<const std::byte> osrc = segs.output.view_ro();
  ATX_ASSERT(out.bytes.size() == osrc.size());
  std::memcpy(out.bytes.data(), osrc.data(), out.bytes.size());
  return atx::core::Ok();
  // segs (all three ShmSegments) freed here by RAII; the owner unlinks the names.
}

// ===========================================================================
// run_shm_worker — the WORKER side (OS-agnostic; only segment IO + the kernel).
// ===========================================================================
int run_shm_worker(int argc, char **argv) noexcept {
  if (argc < 3) {
    return 1; // usage: atx-shm-worker <control-seg-name> <worker-id>
  }
  const char *control_name = argv[1];
  // Parse the worker id strictly: the ENTIRE argument must be a decimal number.
  // (strtoul silently maps junk to 0, which would alias two workers onto
  // ErrorSlot 0 and corrupt the lowest-id reduction — refuse instead.)
  atx::usize worker_id = 0;
  {
    const char *first = argv[2];
    const char *last = first + std::strlen(first);
    const std::from_chars_result pr = std::from_chars(first, last, worker_id);
    if (pr.ec != std::errc{} || pr.ptr != last) {
      return 1; // not a clean integer (empty, negative, or trailing junk)
    }
  }

  // Open the control segment ReadWrite — the worker fetch_adds the claim cursor
  // and writes its own ErrorSlot.
  auto ctl_seg = ShmSegment::open(control_name, ShmAccess::ReadWrite);
  if (!ctl_seg) {
    return 1;
  }
  std::span<std::byte> cbuf = ctl_seg->view_rw();
  if (cbuf.size() < sizeof(ControlBlock)) {
    return 1;
  }
  // SAFETY: cbuf maps the control segment the parent created and stamped; the
  // first sizeof(ControlBlock) bytes are a valid, alive ControlBlock (same binary,
  // standard-layout, trivially-copyable). The reinterpret_cast is the shared-memory
  // typed-view idiom, required to read the block in place.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
  auto *ctrl = reinterpret_cast<ControlBlock *>(cbuf.data());
  if (ctrl->magic != kControlMagic || worker_id >= ctrl->n_workers) {
    return 1;
  }
  if (cbuf.size() < control_segment_bytes(ctrl->n_workers)) {
    return 1; // truncated control segment — refuse to address the ErrorSlot array
  }
  ErrorSlot &my_err = error_slots(cbuf)[worker_id];

  // Resolve the workload body. A null body is this worker's NotFound error.
  const ShardFn fn = lookup_workload(static_cast<WorkloadId>(ctrl->workload));
  if (fn == nullptr) {
    my_err.has_error = 1;
    my_err.lowest_failed_shard = 0;
    my_err.code = static_cast<atx::u32>(ErrorCode::NotFound);
    return 1;
  }

  // Open input (ReadOnly — a write FAULTS, R5) and output (ReadWrite, R4).
  auto in_seg = ShmSegment::open(ctrl->input_name, ShmAccess::ReadOnly);
  auto out_seg = ShmSegment::open(ctrl->output_name, ShmAccess::ReadWrite);
  if (!in_seg || !out_seg) {
    my_err.has_error = 1;
    my_err.lowest_failed_shard = 0;
    my_err.code = static_cast<atx::u32>(ErrorCode::IoError);
    return 1;
  }
  // Logical input view: the first input_bytes of the (possibly 1-byte-padded)
  // segment. input_bytes==0 yields an empty span (the kernel handles it).
  const InputView inputs{in_seg->view_ro().first(static_cast<atx::usize>(ctrl->input_bytes))};
  std::span<std::byte> obuf = out_seg->view_rw();

  const atx::u64 n = ctrl->n_shards;
  const atx::usize stride = static_cast<atx::usize>(ctrl->slot_stride);
  const atx::usize slot_size = static_cast<atx::usize>(ctrl->slot_size);

  // Validate the cross-process geometry ONCE up front (these are untrusted values
  // read from shared memory, not local invariants — a debug-only ATX_ASSERT per
  // shard is not enough). slot_size must fit the stride, and the whole table
  // (n * stride) must fit the mapped output segment. Overflow-clean: reject if n
  // would overflow usize when multiplied by stride. On any violation this worker
  // reports an InvalidArgument failure rather than risking an OOB slot write.
  if (slot_size > stride || stride == 0 ||
      n > static_cast<atx::u64>(obuf.size() / (stride == 0 ? 1 : stride))) {
    my_err.has_error = 1;
    my_err.lowest_failed_shard = 0;
    my_err.code = static_cast<atx::u32>(ErrorCode::InvalidArgument);
    return 1;
  }

  // Drain the cross-process claim cursor.
  // SAFETY (alignment): &claim_cursor must meet atomic_ref's required alignment or
  // the construction is UB. The ControlBlock is mapped at page+8 (ShmSegment's
  // length header), so the cursor is 8-aligned (offsetof is a multiple of 8 and
  // required_alignment is 8 — both static_asserted in the header); guard it at
  // runtime here too, since the base address comes from an OS mapping.
  // SAFETY (ordering): claim_cursor lives in the shared control segment;
  // std::atomic_ref over a lock-free usize uses real CPU atomics, so this is a
  // correct cross-process dispenser. It publishes NO result data (the OUTPUT slots
  // carry results, each written by exactly one worker), so relaxed ordering
  // suffices — identical reasoning to DetPool::run_job's fetch_add.
  ATX_ASSERT(reinterpret_cast<std::uintptr_t>(&ctrl->claim_cursor) % // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
                 std::atomic_ref<atx::usize>::required_alignment ==
             0);
  std::atomic_ref<atx::usize> cursor(ctrl->claim_cursor);
  static_assert(std::atomic_ref<atx::usize>::is_always_lock_free,
                "claim cursor must be lock-free for a correct cross-process dispenser");
  for (;;) {
    const atx::usize id = cursor.fetch_add(1, std::memory_order_relaxed);
    if (static_cast<atx::u64>(id) >= n) {
      break;
    }
    // Each shard writes ONLY its own pre-indexed slot — no cross-shard state (R4).
    const atx::usize off = id * stride; // in-bounds: validated n*stride <= obuf.size() above
    std::span<std::byte> slot = obuf.subspan(off, slot_size);
    const Status s = fn(inputs, id, slot);
    if (!s) {
      // Record the LOWEST failed id this worker saw; keep draining so every shard
      // still runs (matches DetPool — the parent reduces the global lowest).
      const atx::u64 fid = static_cast<atx::u64>(id);
      if (my_err.has_error == 0 || fid < my_err.lowest_failed_shard) {
        my_err.has_error = 1;
        my_err.lowest_failed_shard = fid;
        my_err.code = static_cast<atx::u32>(s.error().code());
      }
    }
  }
  return (my_err.has_error != 0) ? 1 : 0;
}

// ===========================================================================
// Platform backends: worker-exe path discovery + spawn/barrier.
// ===========================================================================
#if defined(_WIN32)

namespace {

[[nodiscard]] std::wstring widen_utf8(const std::string &s) {
  const int wlen = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
  if (wlen <= 0) {
    return {};
  }
  std::wstring w(static_cast<std::size_t>(wlen - 1), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), wlen);
  return w;
}

// Read an env var into `out`, returning true iff it was present and non-empty.
// Uses _dupenv_s (the non-deprecated MSVC form); frees the heap copy it returns.
[[nodiscard]] bool get_env(const char *name, std::string &out) {
  char *value = nullptr;
  std::size_t len = 0;
  if (_dupenv_s(&value, &len, name) != 0 || value == nullptr) {
    return false;
  }
  const bool present = value[0] != '\0';
  if (present) {
    out.assign(value);
  }
  std::free(value);
  return present;
}

[[nodiscard]] Result<std::string> worker_exe_path() {
  if (std::string override_path; get_env("ATX_SHM_WORKER", override_path)) {
    // Validate the override resolves to an existing file (a bogus path must
    // surface as NotFound at submit, never as a downstream spawn failure).
    const std::wstring wpath = widen_utf8(override_path);
    if (wpath.empty() || GetFileAttributesW(wpath.c_str()) == INVALID_FILE_ATTRIBUTES) {
      return Err(ErrorCode::NotFound, "ATX_SHM_WORKER override path does not exist");
    }
    return override_path;
  }
  // Directory of the current module, then append the worker exe name.
  wchar_t buf[MAX_PATH];
  const DWORD len = GetModuleFileNameW(nullptr, buf, static_cast<DWORD>(std::size(buf)));
  if (len == 0 || len >= std::size(buf)) {
    return Err(ErrorCode::NotFound, "ProcessExecutor: GetModuleFileNameW failed");
  }
  std::wstring path(buf, len);
  const std::size_t slash = path.find_last_of(L"\\/");
  std::wstring dir = (slash == std::wstring::npos) ? std::wstring{} : path.substr(0, slash + 1);
  // Append the worker name (ASCII) to the wide dir.
  std::wstring wexe = dir;
  for (const char *p = kWorkerExeName; *p != '\0'; ++p) {
    wexe.push_back(static_cast<wchar_t>(*p));
  }
  if (GetFileAttributesW(wexe.c_str()) == INVALID_FILE_ATTRIBUTES) {
    return Err(ErrorCode::NotFound, "atx-shm-worker not found beside the current module");
  }
  // Narrow back to UTF-8 for the uniform std::string return (path is ASCII here
  // in practice; widen only mattered to call the W API correctly).
  const int n8 = WideCharToMultiByte(CP_UTF8, 0, wexe.c_str(), -1, nullptr, 0, nullptr, nullptr);
  if (n8 <= 0) {
    return Err(ErrorCode::Internal, "ProcessExecutor: WideCharToMultiByte failed");
  }
  std::string out(static_cast<std::size_t>(n8 - 1), '\0');
  WideCharToMultiByte(CP_UTF8, 0, wexe.c_str(), -1, out.data(), n8, nullptr, nullptr);
  return out;
}

// Wait on a batch of handles, honouring the MAXIMUM_WAIT_OBJECTS (64) limit by
// chaining waits, then query EACH process's exit code. `handles[i]` is worker i;
// the lowest-index worker with a nonzero exit code is recorded into `*abnormal`
// (left untouched if every worker exited 0). Returns Err only if the wait/query
// machinery itself failed — an abnormal child exit is reported via `*abnormal`.
[[nodiscard]] Status wait_all(std::vector<HANDLE> &handles, AbnormalExit *abnormal) {
  Status result = atx::core::Ok();
  atx::usize i = 0;
  while (i < handles.size()) {
    const atx::usize remaining = handles.size() - i;
    const DWORD batch =
        static_cast<DWORD>(remaining < MAXIMUM_WAIT_OBJECTS ? remaining : MAXIMUM_WAIT_OBJECTS);
    const DWORD wr = WaitForMultipleObjects(batch, &handles[i], TRUE, INFINITE);
    if (wr == WAIT_FAILED) {
      result = Err(ErrorCode::Internal, "ProcessExecutor: WaitForMultipleObjects failed");
    }
    // Query each handle in this batch; a nonzero exit code is an abnormal exit
    // (a crash on Windows surfaces as a nonzero/exception exit code too). Record
    // the lowest worker index so the failure is deterministic.
    for (atx::usize k = 0; k < batch; ++k) {
      const atx::usize w = i + k;
      DWORD code = 0;
      if (GetExitCodeProcess(handles[w], &code) == FALSE) {
        result = Err(ErrorCode::Internal, "ProcessExecutor: GetExitCodeProcess failed");
      } else if (code != 0 && abnormal != nullptr &&
                 (abnormal->worker == static_cast<atx::usize>(-1) || w < abnormal->worker)) {
        abnormal->worker = w;
        abnormal->code = static_cast<atx::i64>(static_cast<atx::i32>(code));
        abnormal->by_signal = false;
      }
    }
    i += batch;
  }
  return result;
}

[[nodiscard]] Status spawn_and_wait(const std::string &exe, const std::string &control_name,
                                    atx::usize n_workers, AbnormalExit *abnormal) {
  const std::wstring wexe = widen_utf8(exe);
  if (wexe.empty()) {
    return Err(ErrorCode::Internal, "ProcessExecutor: worker path not valid UTF-8");
  }
  std::vector<HANDLE> handles;
  handles.reserve(n_workers);

  // RAII-ish: any spawn failure waits on the already-spawned children, so no
  // orphaned process / leaked handle escapes this function. (Exit codes are
  // irrelevant on the spawn-failure path — we are already returning an error.)
  auto cleanup_on_fail = [&handles]() {
    if (!handles.empty()) {
      Status ignored = wait_all(handles, nullptr); // best-effort barrier so children exit
      (void)ignored;
      for (HANDLE h : handles) {
        CloseHandle(h);
      }
      handles.clear();
    }
  };

  for (atx::usize w = 0; w < n_workers; ++w) {
    // Command line: "<exe>" <control_name> <worker_id>. CreateProcessW needs a
    // MUTABLE buffer; build a fresh wstring per spawn.
    std::wstring cmd = L"\"";
    cmd += wexe;
    cmd += L"\" ";
    for (char c : control_name) {
      cmd.push_back(static_cast<wchar_t>(c)); // control name is ASCII (our stem)
    }
    cmd += L' ';
    cmd += std::to_wstring(w);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    std::wstring cmd_mut = cmd; // CreateProcessW may modify this buffer
    const BOOL ok = CreateProcessW(wexe.c_str(), cmd_mut.data(), nullptr, nullptr, FALSE, 0, nullptr,
                                   nullptr, &si, &pi);
    if (ok == FALSE) {
      cleanup_on_fail();
      return Err(ErrorCode::Internal, "ProcessExecutor: CreateProcessW failed");
    }
    CloseHandle(pi.hThread); // we only barrier on the process, not the thread
    handles.push_back(pi.hProcess);
  }

  // Barrier: wait for ALL workers (capturing the lowest abnormal exit), then
  // close every process handle.
  Status wait_status = wait_all(handles, abnormal);
  for (HANDLE h : handles) {
    CloseHandle(h);
  }
  return wait_status;
}

} // namespace

// NOLINTEND(misc-include-cleaner)

#else // ---------------------------------- POSIX ----------------------------------

namespace {

[[nodiscard]] Result<std::string> worker_exe_path() {
  if (const char *override_path = std::getenv("ATX_SHM_WORKER")) {
    if (override_path[0] != '\0') {
      // Validate the override resolves to an existing executable (a bogus path
      // must surface as NotFound at submit, never as a downstream spawn failure).
      if (::access(override_path, X_OK) != 0) {
        return Err(ErrorCode::NotFound, "ATX_SHM_WORKER override path does not exist");
      }
      return std::string{override_path};
    }
  }
  char buf[4096];
  const ssize_t len = ::readlink("/proc/self/exe", buf, sizeof(buf) - 1);
  if (len <= 0) {
    return Err(ErrorCode::NotFound, "ProcessExecutor: readlink(/proc/self/exe) failed");
  }
  buf[len] = '\0';
  std::string path(buf, static_cast<std::size_t>(len));
  const std::size_t slash = path.find_last_of('/');
  std::string dir = (slash == std::string::npos) ? std::string{} : path.substr(0, slash + 1);
  std::string exe = dir + kWorkerExeName;
  if (::access(exe.c_str(), X_OK) != 0) {
    return Err(ErrorCode::NotFound, "atx-shm-worker not found beside the current executable");
  }
  return exe;
}

// Wait on every spawned child, retrying waitpid on EINTR (else a signal leaves a
// child unreaped — a zombie — plus a spurious failure). `pids[i]` is worker i; a
// child that exited nonzero OR was killed by a signal is recorded into
// `*abnormal` (lowest worker index wins, for determinism). Returns Err only if
// the waitpid machinery itself failed (after EINTR retries).
[[nodiscard]] Status wait_all(const std::vector<pid_t> &pids, AbnormalExit *abnormal) {
  Status result = atx::core::Ok();
  for (atx::usize w = 0; w < pids.size(); ++w) {
    int status = 0;
    int r = 0;
    do {
      r = ::waitpid(pids[w], &status, 0);
    } while (r < 0 && errno == EINTR); // retry on interrupt; do not leak a zombie
    if (r < 0) {
      result = Err(ErrorCode::Internal, "ProcessExecutor: waitpid failed");
      continue;
    }
    const bool bad_exit = WIFEXITED(status) && WEXITSTATUS(status) != 0;
    const bool killed = WIFSIGNALED(status);
    if ((bad_exit || killed) && abnormal != nullptr &&
        (abnormal->worker == static_cast<atx::usize>(-1) || w < abnormal->worker)) {
      abnormal->worker = w;
      abnormal->by_signal = killed;
      abnormal->code = killed ? static_cast<atx::i64>(WTERMSIG(status))
                              : static_cast<atx::i64>(WEXITSTATUS(status));
    }
  }
  return result;
}

[[nodiscard]] Status spawn_and_wait(const std::string &exe, const std::string &control_name,
                                    atx::usize n_workers, AbnormalExit *abnormal) {
  std::vector<pid_t> pids;
  pids.reserve(n_workers);

  for (atx::usize w = 0; w < n_workers; ++w) {
    const std::string wid = std::to_string(w);
    // posix_spawn argv: { exe, control_name, worker_id, nullptr }. The casts strip
    // const for the POSIX char*[] argv contract (the callee does not modify them).
    char *argv[4];
    argv[0] = const_cast<char *>(exe.c_str());          // NOLINT(cppcoreguidelines-pro-type-const-cast)
    argv[1] = const_cast<char *>(control_name.c_str()); // NOLINT(cppcoreguidelines-pro-type-const-cast)
    argv[2] = const_cast<char *>(wid.c_str());          // NOLINT(cppcoreguidelines-pro-type-const-cast)
    argv[3] = nullptr;

    pid_t pid = 0;
    const int rc = ::posix_spawn(&pid, exe.c_str(), nullptr, nullptr, argv, environ);
    if (rc != 0) {
      // Reap the children already spawned so none orphan, then fail. (Exit codes
      // are irrelevant on the spawn-failure path — already returning an error.)
      Status ignored = wait_all(pids, nullptr);
      (void)ignored;
      return Err(ErrorCode::Internal, "ProcessExecutor: posix_spawn failed");
    }
    pids.push_back(pid);
  }

  return wait_all(pids, abnormal); // barrier
}

} // namespace

#endif

} // namespace atx::engine::parallel
