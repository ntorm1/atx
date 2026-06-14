#include "atx/engine/parallel/scheduler.hpp"

#include <atomic>
#include <cstddef>
#include <span>
#include <vector>

#include "atx/core/macro.hpp" // ATX_ASSERT, ATX_WARN
#include "atx/core/types.hpp"

#include "atx/engine/parallel/executor.hpp" // kCacheLine

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
// NOLINTBEGIN(misc-include-cleaner) — windows.h is the Win32 umbrella; its
// sub-headers (sysinfoapi.h, processtopologyapi.h, systemtopologyapi.h,
// processthreadsapi.h, …) are not self-contained on all SDK versions, so we
// include the umbrella. All Win32 symbol warnings in the blocks below are
// suppressed for this reason.
#include <windows.h>
#else
#include <sched.h>  // sched_setaffinity, cpu_set_t
#include <unistd.h> // sysconf, getpagesize
#endif

namespace atx::engine::parallel {

namespace {

// Process-global "exactly one pool" flag (the §1A oversubscription guard). A
// plain atomic bool: claim_single_pool flips false->true and ATX_ASSERTs it was
// false (a second live pool is a programmer error). No result data ever flows
// through it — it is the same "the counter does not enter the bits" guarantee the
// DetPool dispenser documents.
std::atomic<bool> g_single_pool_live{false};

// OS page size (for the first-touch stride). A conservative 4 KiB fallback keeps
// first-touch correct (touching one byte per <= real-page stride still faults
// every page) even if the query fails.
[[nodiscard]] atx::usize os_page_bytes() noexcept {
#if defined(_WIN32)
  SYSTEM_INFO si{};
  GetSystemInfo(&si);
  const atx::usize p = static_cast<atx::usize>(si.dwPageSize);
#else
  const long p_raw = ::sysconf(_SC_PAGESIZE);
  const atx::usize p = (p_raw > 0) ? static_cast<atx::usize>(p_raw) : 0;
#endif
  return (p == 0) ? atx::usize{4096} : p;
}

} // namespace

// ===========================================================================
// (d) Oversubscription guard — platform-independent (a process-global atomic).
// ===========================================================================

void Scheduler::claim_single_pool() noexcept {
  // exchange returns the PRIOR value; it must have been false (no live pool).
  const bool was_live = g_single_pool_live.exchange(true, std::memory_order_acq_rel);
  ATX_ASSERT(!was_live); // a second live global pool => oversubscription (programmer error)
  (void)was_live;        // released in NDEBUG where ATX_ASSERT compiles out
}

void Scheduler::release_single_pool() noexcept {
  g_single_pool_live.store(false, std::memory_order_release);
}

bool Scheduler::single_pool_live() noexcept {
  return g_single_pool_live.load(std::memory_order_acquire);
}

// ===========================================================================
// (b) First-touch — platform-independent (a plain page-strided write). Pinning
// is the only platform-specific part; first-touch is just writing a byte per page
// of the worker's OWN slot region so the page faults in on THIS (already-pinned)
// thread's node. Writing zero into the worker's own slots is harmless: the shard
// overwrites them with its result before the gather reads them, so first-touch
// cannot leak into a result bit (it is sequenced strictly before the shard write).
// ===========================================================================

namespace {

void first_touch(std::span<std::byte> my_slots) noexcept {
  if (my_slots.empty()) {
    return;
  }
  const atx::usize page = os_page_bytes();
  // Touch one byte per page so every page in the region faults in locally. We
  // write 0 (not read) because Linux first-touch places a page on the node of the
  // thread that first WRITES it (§1A B6); a read of a zero-fill page may not.
  for (atx::usize off = 0; off < my_slots.size(); off += page) {
    my_slots[off] = std::byte{0};
  }
  // Touch the final byte too, in case the region is not a whole multiple of a
  // page (the last partial page would otherwise be skipped by the stride above).
  my_slots[my_slots.size() - 1] = std::byte{0};
}

} // namespace

// ===========================================================================
// Win32 backend
// ===========================================================================
#if defined(_WIN32)

Topology query_topology() {
  Topology topo{};
  topo.line_bytes = kCacheLine;

  SYSTEM_INFO si{};
  GetSystemInfo(&si);
  const atx::usize n_logical = static_cast<atx::usize>(si.dwNumberOfProcessors);
  topo.n_pus = (n_logical == 0) ? atx::usize{1} : n_logical;
  topo.n_cores = topo.n_pus;

  // NUMA node count: GetNumaHighestNodeNumber gives the highest node index, so the
  // count is that + 1. A failure leaves the conservative single-node default.
  ULONG highest_node = 0;
  if (GetNumaHighestNodeNumber(&highest_node) != FALSE) {
    topo.n_numa_nodes = static_cast<atx::usize>(highest_node) + 1U;
  } else {
    topo.n_numa_nodes = 1;
  }
  if (topo.n_numa_nodes == 0) {
    topo.n_numa_nodes = 1; // defensive: never zero nodes
  }

  // Map each PU to a NUMA node. GetNumaProcessorNode works per logical processor
  // for systems with <= 64 PUs (one processor group); above that it reports the
  // in-group index, which is acceptable here because pu_to_node is only a
  // first-touch/spread HINT (a wrong node costs bandwidth, never a bit). On query
  // failure for a PU we fall back to spreading it round-robin across the nodes.
  topo.pu_to_node.resize(topo.n_pus);
  for (atx::usize pu = 0; pu < topo.n_pus; ++pu) {
    UCHAR node = 0;
    if (pu < 64 && GetNumaProcessorNode(static_cast<UCHAR>(pu), &node) != FALSE) {
      topo.pu_to_node[pu] = static_cast<atx::usize>(node) % topo.n_numa_nodes;
    } else {
      topo.pu_to_node[pu] = pu % topo.n_numa_nodes; // deterministic spread fallback
    }
  }
  return topo;
}

void Scheduler::pin_and_first_touch(atx::usize worker_id,
                                    std::span<std::byte> my_slots) const noexcept {
  // Spread pinning (OMP_PROC_BIND=spread analogue): place consecutive workers on
  // different NUMA nodes for bandwidth. Worker w -> PU index (w % n_pus); the OS
  // affinity mask is a single-bit mask for that PU. SetThreadAffinityMask only
  // addresses the first 64 PUs of the current processor group (a HINT, so the
  // wrap is acceptable). BEST-EFFORT: a failure is logged and ignored (R1 — pin is
  // never correctness).
  const atx::usize n_pus = (topo.n_pus == 0) ? atx::usize{1} : topo.n_pus;
  const atx::usize pu = worker_id % n_pus;
  if (pu < 64) {
    const DWORD_PTR mask = (static_cast<DWORD_PTR>(1) << pu);
    if (SetThreadAffinityMask(GetCurrentThread(), mask) == 0) {
      ATX_WARN("scheduler: SetThreadAffinityMask failed for worker {} (pin is best-effort)",
               worker_id);
    }
  }
  first_touch(my_slots);
}

// ===========================================================================
// POSIX backend
// ===========================================================================
#else

Topology query_topology() {
  Topology topo{};
  topo.line_bytes = kCacheLine;

  const long n_online = ::sysconf(_SC_NPROCESSORS_ONLN);
  const atx::usize n_logical = (n_online > 0) ? static_cast<atx::usize>(n_online) : 0;
  topo.n_pus = (n_logical == 0) ? atx::usize{1} : n_logical;
  topo.n_cores = topo.n_pus;

  // NUMA node discovery without libnuma: count /sys/devices/system/node/nodeN
  // directories. We avoid a <filesystem> dependency in this hot-cold-path TU and
  // a missing sysfs (containers, non-Linux POSIX) is a clean single-node fallback.
  // The full sysfs scan is a Linux-CI residual; on this Windows build it compiles
  // out entirely (the whole #else branch is dead on _WIN32). For portability we
  // keep the conservative default: one node, every PU local to it.
  topo.n_numa_nodes = 1;
  topo.pu_to_node.assign(topo.n_pus, 0);
  return topo;
}

void Scheduler::pin_and_first_touch(atx::usize worker_id,
                                    std::span<std::byte> my_slots) const noexcept {
  // Spread pinning via sched_setaffinity: pin THIS thread to a single PU. Worker
  // w -> PU (w % n_pus). BEST-EFFORT: failure is logged and ignored (R1).
  const atx::usize n_pus = (topo.n_pus == 0) ? atx::usize{1} : topo.n_pus;
  const atx::usize pu = worker_id % n_pus;

  cpu_set_t set;
  CPU_ZERO(&set);
  CPU_SET(static_cast<int>(pu), &set);
  if (::sched_setaffinity(0, sizeof(set), &set) != 0) {
    ATX_WARN("scheduler: sched_setaffinity failed for worker {} (pin is best-effort)", worker_id);
  }
  first_touch(my_slots);
}

#endif

} // namespace atx::engine::parallel
