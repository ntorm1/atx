// Env-gated Andersen-Lake hot-path probe — see al_probe.hpp for the rationale.

#include "pricing/al_probe.hpp"

#include <cstdlib>
#include <cstring>
#include <mutex>
#include <vector>

namespace atx::vol::alprobe {

const char *const kZoneNames[kZoneCount] = {
    "board_fit",           "al_boundary_solve_cold", "al_boundary_solve_warm",
    "al_premium_quad",     "al_price_from_boundary", "american_iv",
    "sigma_interp_build",  "slice_sigma",            "alo_pricer_price",
    "al_seed_baw",         "al_sweep_jn",            "al_sweep_fp",
    "al_bind_geo_static",  "al_bind_geo_sigma",      "al_seed_baw_ACC",
    "al_sweep_jn_ACC",     "al_sweep_fp_ACC",
};

const char *const kEventNames[kEventCount] = {
    "shared_side_considered", "shared_side_guard_skip", "shared_side_build_fail",
    "shared_side_certified",  "shared_side_rejected",   "shared_rows_laned",
    "shared_rows_fallback",
};

namespace {

// One 6-double record per cold boundary solve, in the order the dump writes them.
struct StateRec {
  double T;
  double sigma;
  double r;
  double q;
  double K;
  double nb;
};

struct Block {
  std::uint64_t cycles[kZoneCount]{};
  std::uint64_t calls[kZoneCount]{};
  std::uint64_t events[kEventCount]{};
  std::vector<StateRec> states{};
  Block *next{nullptr};
};

std::mutex g_mutex;
Block *g_head{nullptr};
std::uint64_t g_retired_cycles[kZoneCount]{};
std::uint64_t g_retired_calls[kZoneCount]{};
std::uint64_t g_retired_events[kEventCount]{};
std::vector<StateRec> g_retired_states{};

// RAII per-thread block owner: links on first touch, folds + unlinks at thread exit
// so a fit worker that exits mid-run never drops its tally.
struct Registrar {
  Block block;

  Registrar() {
    const std::lock_guard<std::mutex> lk(g_mutex);
    block.next = g_head;
    g_head = &block;
  }

  ~Registrar() {
    const std::lock_guard<std::mutex> lk(g_mutex);
    for (unsigned i = 0; i < kZoneCount; ++i) {
      g_retired_cycles[i] += block.cycles[i];
      g_retired_calls[i] += block.calls[i];
    }
    for (unsigned i = 0; i < kEventCount; ++i) {
      g_retired_events[i] += block.events[i];
    }
    if (!block.states.empty()) {
      g_retired_states.insert(g_retired_states.end(), block.states.begin(), block.states.end());
      block.states.clear();
      block.states.shrink_to_fit();
    }
    Block **p = &g_head;
    while (*p != nullptr && *p != &block) {
      p = &(*p)->next;
    }
    if (*p == &block) {
      *p = block.next;
    }
  }

  Registrar(const Registrar &) = delete;
  Registrar &operator=(const Registrar &) = delete;
};

// Constructed lazily on the first `add`/`bump`, both of which are only reached
// behind `detail::g_on`, so an un-probed run never instantiates it.
thread_local Registrar t_reg;

[[nodiscard]] bool env_value(const char *name, char *buf, std::size_t cap) noexcept {
#if defined(_MSC_VER)
  std::size_t sz = 0;
  return getenv_s(&sz, buf, cap, name) == 0 && sz > 0;
#else
  const char *v = std::getenv(name);
  if (v == nullptr || *v == '\0') {
    return false;
  }
  std::snprintf(buf, cap, "%s", v);
  return true;
#endif
}

struct Mode {
  bool on{false};
  bool states{false};
};

[[nodiscard]] Mode read_mode() noexcept {
  char buf[64] = {};
  Mode m;
  if (!env_value("ATX_VOL_AL_PROBE", buf, sizeof(buf))) {
    return m;
  }
  m.on = true;
  m.states = std::strchr(buf, 's') != nullptr || std::strchr(buf, 'S') != nullptr;
  return m;
}

// Defined BEFORE detail::g_on below, so intra-TU dynamic-initialization order
// guarantees it is live when g_on/g_states are initialized.
const Mode g_mode = read_mode();

// Cost of an empty Scope on this host, so a very-high-call-count zone can be
// corrected for its own instrumentation.
[[nodiscard]] std::uint64_t measure_overhead() noexcept {
  constexpr unsigned kReps = 1u << 16;
  static volatile std::uint64_t sink = 0;
  const std::uint64_t t0 = __rdtsc();
  for (unsigned i = 0; i < kReps; ++i) {
    const std::uint64_t a = __rdtsc();
    const std::uint64_t b = __rdtsc();
    sink = sink + (b - a);
  }
  const std::uint64_t t1 = __rdtsc();
  return (t1 - t0) / kReps;
}

} // namespace

namespace detail {
const bool g_on = g_mode.on;
const bool g_states = g_mode.on && g_mode.states;
} // namespace detail

void add(Zone z, std::uint64_t cycles) noexcept {
  Block &b = t_reg.block;
  const unsigned i = static_cast<unsigned>(z);
  b.cycles[i] += cycles;
  b.calls[i] += 1u;
}

void bump(Event e, std::uint64_t n) noexcept {
  if (!detail::g_on) {
    return;
  }
  t_reg.block.events[static_cast<unsigned>(e)] += n;
}

void record_boundary_state(double T, double sigma, double r, double q, double K,
                           unsigned nb) noexcept {
  if (!detail::g_states) {
    return;
  }
  std::vector<StateRec> &v = t_reg.block.states;
  if (v.capacity() == 0) {
    v.reserve(1u << 16);
  }
  v.push_back(StateRec{T, sigma, r, q, K, static_cast<double>(nb)});
}

std::uint64_t zone_overhead_cycles() noexcept {
  static const std::uint64_t v = measure_overhead();
  return v;
}

void dump(std::FILE *out) noexcept {
  if (!detail::g_on || out == nullptr) {
    return;
  }
  std::uint64_t cycles[kZoneCount]{};
  std::uint64_t calls[kZoneCount]{};
  std::uint64_t events[kEventCount]{};
  std::vector<StateRec> states;
  {
    const std::lock_guard<std::mutex> lk(g_mutex);
    for (unsigned i = 0; i < kZoneCount; ++i) {
      cycles[i] = g_retired_cycles[i];
      calls[i] = g_retired_calls[i];
    }
    for (unsigned i = 0; i < kEventCount; ++i) {
      events[i] = g_retired_events[i];
    }
    states = g_retired_states;
    for (const Block *b = g_head; b != nullptr; b = b->next) {
      for (unsigned i = 0; i < kZoneCount; ++i) {
        cycles[i] += b->cycles[i];
        calls[i] += b->calls[i];
      }
      for (unsigned i = 0; i < kEventCount; ++i) {
        events[i] += b->events[i];
      }
      states.insert(states.end(), b->states.begin(), b->states.end());
    }
  }

  std::fprintf(out, "alprobe.rdtsc_overhead_cycles %llu\n",
               static_cast<unsigned long long>(zone_overhead_cycles()));
  const double denom = static_cast<double>(cycles[static_cast<unsigned>(Zone::BoardFit)]);
  for (unsigned i = 0; i < kZoneCount; ++i) {
    const double total = static_cast<double>(cycles[i]);
    const double per_call = calls[i] > 0 ? total / static_cast<double>(calls[i]) : 0.0;
    const double share = denom > 0.0 ? total / denom : 0.0;
    std::fprintf(out,
                 "alprobe.zone %-24s calls=%-12llu cycles=%-16llu cyc_per_call=%-12.1f "
                 "share_of_board_fit=%.5f\n",
                 kZoneNames[i], static_cast<unsigned long long>(calls[i]),
                 static_cast<unsigned long long>(cycles[i]), per_call, share);
  }
  for (unsigned i = 0; i < kEventCount; ++i) {
    std::fprintf(out, "alprobe.event %-24s %llu\n", kEventNames[i],
                 static_cast<unsigned long long>(events[i]));
  }

  if (!detail::g_states) {
    return;
  }
  std::fprintf(out, "alprobe.states_recorded %llu\n",
               static_cast<unsigned long long>(states.size()));
  char path[512] = {};
  if (!env_value("ATX_VOL_AL_PROBE_OUT", path, sizeof(path))) {
    std::fprintf(out, "alprobe.states_out <unset: set ATX_VOL_AL_PROBE_OUT to write the trace>\n");
    return;
  }
  std::FILE *f = nullptr;
#if defined(_MSC_VER)
  if (fopen_s(&f, path, "wb") != 0) {
    f = nullptr;
  }
#else
  f = std::fopen(path, "wb");
#endif
  if (f == nullptr) {
    std::fprintf(out, "alprobe.states_out FAILED to open %s\n", path);
    return;
  }
  // 6 little-endian f64 per record: T, sigma, r, q, K, n_boundary.
  const std::size_t written =
      states.empty() ? 0u : std::fwrite(states.data(), sizeof(StateRec), states.size(), f);
  std::fclose(f);
  std::fprintf(out, "alprobe.states_out %s records=%llu\n", path,
               static_cast<unsigned long long>(written));
}

} // namespace atx::vol::alprobe
