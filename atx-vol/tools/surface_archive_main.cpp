// atx-vol-surface-archive: inspect | dump | verify an ATXVFA fitted-surface
// archive (docs/storage/ARCHIVE_FORMAT.md) or a series directory.
//
//   inspect <file|dir>              framing, section table, record counts
//   dump    <file> [--underlier S]  one line per record
//   verify  <file|dir>              Paranoid open; on refusal, localise the damage
//
// Exit codes: 0 ok, 1 usage / IO, 2 the archive is refused (status printed).
// Read-only: never writes, never maps for longer than the command.

#include <algorithm>
#include <array>
#include <cinttypes>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

#include "atx/vol/api/storage/fit_archive.hpp"
#include "atx/vol/api/storage/fit_archive_records.hpp"
#include "atx/vol/api/storage/fit_archive_series.hpp"

namespace {

using namespace atx::vol;
using Type = FitArchiveSectionType;

constexpr int kOk = 0;
constexpr int kUsage = 1;
constexpr int kRefused = 2;

void usage() {
  std::cerr << "usage: atx-vol-surface-archive inspect <file|series-dir>\n"
               "       atx-vol-surface-archive dump <file> [--underlier SYMBOL]\n"
               "       atx-vol-surface-archive verify <file|series-dir>\n";
}

std::string hex32(std::uint32_t v) {
  std::array<char, 16> buf{};
  std::snprintf(buf.data(), buf.size(), "0x%08" PRIx32, v);
  return buf.data();
}

int refused(std::string_view what, const ArchiveError &e) {
  std::cout << what << ": REFUSED " << to_string(e.status);
  if (e.section != kFitArchiveNone) {
    std::cout << " section=" << to_string(static_cast<Type>(e.section)) << "(" << e.section << ")";
  }
  std::cout << " detail=" << e.detail << "\n";
  return kRefused;
}

const char *grade_name(std::uint8_t grade) {
  return grade <= static_cast<std::uint8_t>(QuoteSliceGrade::Refused)
             ? to_string(static_cast<QuoteSliceGrade>(grade))
             : "?";
}

void print_key(const FitArchive &a, const FitArchiveKey &k) {
  std::cout << a.underlier_name(k.underlier) << '\t' << k.snapshot_ns << '\t' << k.expiry_day << '\t' << k.sub;
}

void inspect_one(const FitArchive &a, std::string_view label) {
  const FitArchiveHeader &h = a.header();
  std::cout << "file: " << label << "\n"
            << "format: ATXVFA " << h.format_major << "." << h.format_minor
            << "  writer: " << std::string_view(h.writer_name.data()) << " (" << h.writer_version << ")\n"
            << "created_unix_ns: " << h.created_unix_ns << "  archive_id: " << h.archive_id
            << "  segment_sequence: " << h.segment_sequence << "\n"
            << "size: " << h.file_size << " bytes  header_crc32c: " << hex32(h.header_crc32c)
            << "  file_crc32c: " << hex32(a.footer().file_crc32c) << "\n"
            << "sections: " << a.sections().size() << "\n"
            << "  type                 ver   offset      length       count   stride  crc32c\n";
  for (const FitArchiveSectionEntry &e : a.sections()) {
    std::array<char, 160> line{};
    std::snprintf(line.data(), line.size(), "  %-20s %u.%u  %-10" PRIu64 "  %-11" PRIu64 "  %-7" PRIu64 " %-7u %s\n",
                  to_string(static_cast<Type>(e.type)), static_cast<unsigned>(e.type_major),
                  static_cast<unsigned>(e.type_minor), e.offset, e.length, e.element_count,
                  static_cast<unsigned>(e.element_stride), hex32(e.crc32c).c_str());
    std::cout << line.data();
  }
  std::array<std::size_t, 5> grades{}; // A, B, C, Refused, standalone
  std::int64_t lo = 0, hi = 0;
  bool any = false;
  for (const FitArchiveFitRecord &f : a.fits()) {
    ++grades[f.slice.present != 0u ? f.slice.grade : 4u];
    lo = any ? std::min(lo, f.key.snapshot_ns) : f.key.snapshot_ns;
    hi = any ? std::max(hi, f.key.snapshot_ns) : f.key.snapshot_ns;
    any = true;
  }
  std::cout << "records: symbols=" << a.symbols().size() << " surfaces=" << a.surfaces().size()
            << " fits=" << a.fits().size() << " expiry_records=" << a.expiry_meta().size()
            << " ladders=" << a.ladders().size() << " markov=" << a.markov().size()
            << " series_segments=" << a.series_segments().size() << "\n"
            << "fit grades: A=" << grades[0] << " B=" << grades[1] << " C=" << grades[2]
            << " refused=" << grades[3] << " standalone=" << grades[4] << "\n";
  if (any) std::cout << "fit snapshots: " << lo << " .. " << hi << "\n";
  std::cout << "provenance: " << a.provenance().size() << " bytes\n";
}

int open_series_or_file(std::string_view path, FitArchiveVerify verify, bool print) {
  std::error_code ec;
  if (std::filesystem::is_directory(std::string(path), ec)) {
    auto series = FitArchiveSeries::open(path, {FitArchiveOpenMode::Mapped, verify});
    if (!series) return refused(path, series.error());
    std::cout << "series: " << path << "  segments: " << series->segment_count() << "\n";
    for (std::size_t i = 0; i < series->segment_count(); ++i) {
      const auto &m = series->manifest()[i];
      std::cout << "  seq " << m.sequence << "  " << std::string_view(m.file_name.data()) << "  "
                << m.file_size << " bytes  fits=" << m.fit_records << "  snapshots " << m.min_snapshot_ns
                << " .. " << m.max_snapshot_ns << "\n";
      if (print) inspect_one(series->segment(i), m.file_name.data());
    }
    return kOk;
  }
  auto a = FitArchive::open(path, {FitArchiveOpenMode::Mapped, verify});
  if (!a) return refused(path, a.error());
  if (print) inspect_one(*a, path);
  return kOk;
}

int verify(std::string_view path) {
  const int status = open_series_or_file(path, FitArchiveVerify::Paranoid, false);
  if (status == kOk) {
    std::cout << path << ": OK (paranoid: framing, every section and whole-file checksum, records)\n";
    return kOk;
  }
  // Localise: when the framing still opens, name the damaged sections.
  std::error_code ec;
  if (std::filesystem::is_directory(std::string(path), ec)) return status;
  auto framing = FitArchive::open(path, {FitArchiveOpenMode::Read, FitArchiveVerify::Framing});
  if (!framing) return status;
  for (std::size_t i = 0; i < framing->sections().size(); ++i) {
    if (auto ok = framing->verify_section(i); !ok) {
      std::cout << "  damaged section " << i << " " << to_string(static_cast<Type>(framing->sections()[i].type))
                << "\n";
    }
  }
  if (!framing->verify_file_checksum()) std::cout << "  whole-file checksum mismatch\n";
  return status;
}

int dump(std::string_view path, std::string_view only) {
  auto a = FitArchive::open(path, {FitArchiveOpenMode::Mapped, FitArchiveVerify::Full});
  if (!a) return refused(path, a.error());
  const auto keep = [&](const FitArchiveKey &k) { return only.empty() || a->underlier_name(k.underlier) == only; };
  std::cout << "# surface\tunderlier\tsnapshot_ns\texpiry_day\tsub\tprofile\tpolicy_source\texpiries\tA\tB\tC\trefused\n";
  for (const FitArchiveSurfaceRecord &s : a->surfaces()) {
    if (!keep(s.key)) continue;
    std::cout << "surface\t";
    print_key(*a, s.key);
    std::cout << '\t' << a->string(s.profile) << '\t' << a->string(s.policy_source) << '\t' << s.fit_count;
    for (const std::uint32_t g : s.grade_counts) std::cout << '\t' << g;
    std::cout << "\n";
  }
  std::cout << "# fit\tunderlier\tsnapshot_ns\texpiry_day\tsub\tgrade\tfit_status\tcurve\tnodes\trows\t"
               "spot\tvariance_years\trefusal\n";
  for (const FitArchiveFitRecord &f : a->fits()) {
    if (!keep(f.key)) continue;
    std::cout << "fit\t";
    print_key(*a, f.key);
    std::cout << '\t' << (f.slice.present != 0u ? grade_name(f.slice.grade) : "-") << '\t'
              << to_string(static_cast<QuoteSliceFitStatus>(f.fit_status)) << '\t'
              << (f.curve.present != 0u ? to_string(static_cast<VolCurveKind>(f.curve.kind)) : "none") << '\t'
              << f.prices.nodes.count << '\t' << f.rows.count << '\t' << f.market.spot << '\t'
              << f.market.variance_years << '\t' << a->string(f.slice.refusal) << "\n";
  }
  std::cout << "# expiry\tunderlier\tsnapshot_ns\texpiry_day\tsub\tside\tT\trate\tcarry\tk_box\tsigma_box\t"
               "cert_err_fwd\tladder\n";
  for (const FitArchiveExpiryMeta &m : a->expiry_meta()) {
    if (!keep(m.key)) continue;
    std::cout << "expiry\t";
    print_key(*a, m.key);
    std::cout << '\t' << (m.side == 0u ? "call" : "put") << '\t' << m.expiry << '\t' << m.rate << '\t' << m.carry
              << '\t' << m.k_min << ".." << m.k_max << '\t' << m.sigma_min << ".." << m.sigma_max << '\t'
              << m.certified_max_abs_error_fwd << '\t'
              << (m.ladder == kFitArchiveNone ? std::string("-") : std::to_string(m.ladder)) << "\n";
  }
  for (const FitArchiveLadderRecord &l : a->ladders()) {
    if (!keep(l.key)) continue;
    std::cout << "ladder\t";
    print_key(*a, l.key);
    std::cout << "\tspot=" << l.spot << "\tcontracts=" << l.contracts.count << "\trecords=" << l.record_count << "\n";
  }
  for (const FitArchiveMarkovRecord &r : a->markov()) {
    if (!keep(r.key)) continue;
    std::cout << "markov\t";
    print_key(*a, r.key);
    std::cout << "\tspot=" << r.spot << "\tintervals=" << r.intervals.count << "\tgrid=" << r.state_grid.count
              << "\tfit=" << static_cast<unsigned>(r.has_fit) << "\tlabel=" << a->string(r.label) << "\n";
  }
  return kOk;
}

} // namespace

int main(int argc, char **argv) {
  if (argc < 3) {
    usage();
    return kUsage;
  }
  const std::string_view command{argv[1]};
  const std::string_view path{argv[2]};
  if (command == "inspect" && argc == 3) return open_series_or_file(path, FitArchiveVerify::Full, true);
  if (command == "verify" && argc == 3) return verify(path);
  if (command == "dump" && (argc == 3 || (argc == 5 && std::string_view{argv[3]} == "--underlier"))) {
    return dump(path, argc == 5 ? std::string_view{argv[4]} : std::string_view{});
  }
  usage();
  return kUsage;
}
