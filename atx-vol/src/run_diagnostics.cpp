// run_diagnostics — the `diagnostics` RunArchive section encoder (PhaseTimer
// itself is header-only in run_diagnostics.hpp). Mirrors write_diagnostics
// (examples/spy_dispersion_backtest.cpp): the phase rows in the timer's
// pre-declared order, then a `total` row. Columns are staged in kDiagnosticsCols
// registry order (subcommand DictStr, phase DictStr, wall_ms F64, count I64) and
// every synthesized array is parked in the section's type-erased `storage`, so a
// returned section's spans stay valid for its lifetime (the Task 5 encoder rule).

#include "atx/vol/run_diagnostics.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace atx::vol {

namespace {

// Milliseconds of a steady-clock duration (the example's phase_ms).
double phase_ms(PhaseTimer::Duration d) {
  return std::chrono::duration<double, std::milli>(d).count();
}

// Owned backing for the encoder's synthesized columns. Parked on the returned
// section's `storage`; the RaColumnData spans point into these vectors, whose
// heap blocks stay put once the section is built.
struct DiagStorage {
  std::vector<std::uint32_t> subcommand_codes;
  std::vector<std::string> subcommand_table;
  std::vector<std::uint32_t> phase_codes;
  std::vector<std::string> phase_table;
  std::vector<double> wall_ms;
  std::vector<std::int64_t> count;
};

} // namespace

RaSectionData encode_diagnostics_section(const PhaseTimer &timer, std::string_view subcommand,
                                         std::uint64_t total_count) {
  const std::vector<PhaseTimer::Phase> &phases = timer.phases();
  const std::size_t n = phases.size() + 1; // one row per phase + the `total` row

  auto storage = std::make_shared<DiagStorage>();
  DiagStorage &s = *storage;

  // subcommand: a dict-str column carrying the same token on every row (the TSV
  // writer prints the subcommand on each line) — one table entry, all-zero codes.
  s.subcommand_table.emplace_back(subcommand);
  s.subcommand_codes.assign(n, 0u);

  // phase: dict-str, table in first-appearance order — the DictBuilder
  // convention the other encoders use, so identical inputs yield identical bytes.
  // (A phase literally named "total" simply shares the summary row's dict code;
  // dict dedup handles it, mirroring write_diagnostics printing the token twice.)
  s.phase_codes.reserve(n);
  s.wall_ms.reserve(n);
  s.count.reserve(n);
  std::unordered_map<std::string, std::uint32_t> phase_index;
  const auto push_phase = [&](std::string_view name) {
    const auto [it, inserted] =
        phase_index.try_emplace(std::string(name), static_cast<std::uint32_t>(s.phase_table.size()));
    if (inserted) {
      s.phase_table.emplace_back(name);
    }
    s.phase_codes.push_back(it->second);
  };

  // Phase rows, then the `total` row. The total wall time is the sum of the
  // phase wall times (integer ticks summed, converted once), and `total_count`
  // is the caller-supplied denominator; both u64 counts ride as I64 bit
  // patterns per the registry (there is no U64 dtype).
  PhaseTimer::Duration total_elapsed = PhaseTimer::Duration::zero();
  for (const PhaseTimer::Phase &phase : phases) {
    push_phase(phase.name);
    s.wall_ms.push_back(phase_ms(phase.elapsed));
    s.count.push_back(static_cast<std::int64_t>(phase.count));
    total_elapsed += phase.elapsed;
  }
  push_phase("total");
  s.wall_ms.push_back(phase_ms(total_elapsed));
  s.count.push_back(static_cast<std::int64_t>(total_count));

  RaSectionData sec;
  sec.name = "diagnostics";
  sec.kind = RaSectionKind::SubTable;
  sec.n_rows = n;
  sec.columns.reserve(4);
  sec.columns.emplace_back("subcommand",
                           RaColumnData::of_dict(s.subcommand_codes, s.subcommand_table));
  sec.columns.emplace_back("phase", RaColumnData::of_dict(s.phase_codes, s.phase_table));
  sec.columns.emplace_back("wall_ms", RaColumnData::of_f64(s.wall_ms));
  sec.columns.emplace_back("count", RaColumnData::of_i64(s.count));
  sec.storage = std::move(storage);
  return sec;
}

} // namespace atx::vol
