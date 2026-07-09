// Corpus builder + manifest serialization. See corpus.hpp for the design.

#include "atx/vol/corpus.hpp"

#include <algorithm>    // std::sort, std::min, std::max, std::find
#include <charconv>     // std::to_chars, std::from_chars
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>  // std::error_code
#include <thread>        // std::jthread, std::thread::hardware_concurrency
#include <utility>       // std::move
#include <vector>

#include "atx/vol/chain.hpp"            // OptionChain
#include "atx/vol/curve_selector.hpp"   // SelectorResult, CandidateScore
#include "atx/vol/dispersion.hpp"       // with_uid
#include "atx/vol/priced_surface.hpp"   // PricedSurface
#include "atx/vol/session.hpp"          // VolaSession::to_priced_surface
#include "atx/vol/universe.hpp"         // uid_for_symbol

namespace atx::vol {

using atx::core::Err;
using atx::core::Ok;

const char* to_string(CorpusFitStatus status) noexcept {
  switch (status) {
    case CorpusFitStatus::Ok:
      return "Ok";
    case CorpusFitStatus::Failed:
      return "Failed";
    case CorpusFitStatus::Skipped:
      return "Skipped";
  }
  return "Unrecognized";  // unreachable for valid enumerators
}

namespace {

// ── Per-board fit slot (worker output) ──────────────────────────────────────
//
// Move-only: owns the fitted (move-only) `PricedSurface`. Workers write disjoint
// slots (one per board), so aggregation is race-free.
struct FitSlot {
  CorpusFitStatus status{CorpusFitStatus::Skipped};
  VolCurveKind chosen_kind{VolCurveKind::ConvexDense};
  std::uint32_t n_slices{0};
  double oos_in_band{0.0};
  ErrorCode error_code{ErrorCode::Unknown};
  std::optional<PricedSurface> surface{};  // present iff status == Ok
};

// Fit ONE board through the blessed path (OptionChain::from_frame ->
// PricerFitter::fit -> VolaSession::to_priced_surface).
//
// NOTE (why not calibrate_pool): calib_pool.hpp's `calibrate_pool` produces a
// legacy `VolSurface` (Essvi/Svi/SviMm, fixed by `classify_underlier`). That
// path can neither auto-select the arb-free ConvexDense family nor become a
// `PricedSurface`, so it cannot feed a `SurfaceArchive`. The corpus therefore
// drives the PricerFitter -> to_priced_surface path directly and only reuses the
// pool's DETERMINISM discipline (stable pre-pass, per-worker scratch, disjoint
// output slots).
//
// Pure w.r.t. shared state: reads only its own `board` + `tmpl` (const),
// constructs its own chain / fitter. Safe to run concurrently on distinct boards.
[[nodiscard]] FitSlot fit_board(const CorpusBoard& board, const PricerConfig& tmpl) {
  FitSlot slot{};

  if (board.frame.rows.empty()) {
    slot.status = CorpusFitStatus::Skipped;  // nothing fittable
    return slot;
  }

  try {
    auto chain = OptionChain::from_frame(board.frame, board.env);
    if (!chain) {
      slot.status = CorpusFitStatus::Failed;
      slot.error_code = chain.error().code();
      return slot;
    }

    PricerConfig cfg = tmpl;
    cfg.n_threads = 1;  // each board fits single-threaded; fan-out is ACROSS boards
    if (board.curve.has_value()) {
      cfg.curve = *board.curve;  // per-board pin overrides the template policy
    }
    PricerFitter fitter{cfg};
    const Status st = fitter.fit(*chain);
    if (!st) {
      slot.status = CorpusFitStatus::Failed;
      slot.error_code = st.error().code();
      return slot;
    }

    const FittedSurface* fitted = fitter.surface();
    if (fitted == nullptr) {  // defensive: a successful fit always stores a surface
      slot.status = CorpusFitStatus::Failed;
      slot.error_code = ErrorCode::Internal;
      return slot;
    }

    auto ps = fitted->session().to_priced_surface();
    if (!ps) {
      slot.status = CorpusFitStatus::Failed;
      slot.error_code = ps.error().code();
      return slot;
    }

    slot.n_slices = static_cast<std::uint32_t>(ps->n_slices());
    const std::optional<SelectorResult>& sel = fitter.selection();
    if (sel.has_value()) {
      slot.chosen_kind = sel->chosen.kind;
      if (sel->chosen_index < sel->scores.size()) {
        slot.oos_in_band = sel->scores[sel->chosen_index].oos_in_band;
      }
    } else if (ps->n_slices() > 0) {
      slot.chosen_kind = ps->kind_at(0);  // curve was pinned; no OOS score
    }
    slot.surface = std::move(*ps);
    slot.status = CorpusFitStatus::Ok;
    return slot;
  } catch (...) {
    // SAFETY: a std::jthread worker must not let an exception escape (e.g.
    // std::bad_alloc from fit scratch) — that would std::terminate the process.
    // Record it as a Failed board instead.
    slot.status = CorpusFitStatus::Failed;
    slot.error_code = ErrorCode::Internal;
    return slot;
  }
}

// The per-date archive file path for `date` under `out_dir` (deterministic,
// forward-slash normalized).
[[nodiscard]] std::string archive_path_for(std::string_view out_dir, std::string_view date) {
  return (std::filesystem::path(out_dir) / (std::string(date) + ".atxvsa")).generic_string();
}

}  // namespace

Result<CorpusManifest> build_corpus(std::span<const CorpusBoard> boards,
                                    std::string_view out_dir, const CorpusConfig& cfg) {
  if (boards.empty()) {
    return Err(ErrorCode::InvalidArgument, "build_corpus: empty boards");
  }
  if (out_dir.empty()) {
    return Err(ErrorCode::InvalidArgument, "build_corpus: empty out_dir");
  }

  {
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(out_dir), ec);
    if (ec) {
      return Err(ErrorCode::IoError, "build_corpus: cannot create out_dir");
    }
  }

  const std::size_t n = boards.size();

  // ── Fan out board fits; each worker writes its own disjoint slot ───────────
  std::vector<FitSlot> slots(n);
  const auto run_range = [&boards, &slots, &cfg](std::size_t start, std::size_t end) {
    for (std::size_t i = start; i < end; ++i) {
      slots[i] = fit_board(boards[i], cfg.fit_template);
    }
  };

  std::size_t n_workers = (cfg.n_threads != 0u)
                              ? static_cast<std::size_t>(cfg.n_threads)
                              : std::max<std::size_t>(1u, std::thread::hardware_concurrency());
  n_workers = std::min(n_workers, n);  // n >= 1 here
  {
    std::vector<std::jthread> workers;
    workers.reserve(n_workers - 1u);
    const std::size_t base = n / n_workers;
    const std::size_t rem = n % n_workers;
    std::size_t pos = 0u;
    for (std::size_t w = 0u; w < n_workers; ++w) {
      const std::size_t sz = base + (w < rem ? std::size_t{1u} : std::size_t{0u});
      const std::size_t start = pos;
      const std::size_t end = pos + sz;
      pos = end;
      if (w + 1u < n_workers) {
        workers.emplace_back([&run_range, start, end] { run_range(start, end); });
      } else {
        run_range(start, end);  // last chunk on the calling thread
      }
    }
    // workers join here (jthread RAII) before we read `slots`.
  }

  // ── Deterministic output order: (date asc, symbol asc, board index) ────────
  std::vector<std::size_t> order(n);
  for (std::size_t i = 0; i < n; ++i) {
    order[i] = i;
  }
  std::sort(order.begin(), order.end(), [&boards](std::size_t a, std::size_t b) noexcept {
    if (boards[a].date != boards[b].date) {
      return boards[a].date < boards[b].date;
    }
    if (boards[a].symbol != boards[b].symbol) {
      return boards[a].symbol < boards[b].symbol;
    }
    return a < b;
  });

  // ── Group by date: build entries, write one archive per date with Ok boards ─
  CorpusManifest man{};
  man.entries.reserve(n);
  man.n_boards = static_cast<std::uint32_t>(n);

  std::size_t i = 0;
  while (i < n) {
    const std::string& date = boards[order[i]].date;
    std::size_t j = i;
    while (j < n && boards[order[j]].date == date) {
      ++j;
    }
    // group == order[i, j); already symbol-ascending within the date.
    const std::string apath = archive_path_for(out_dir, date);

    bool date_has_ok = false;
    for (std::size_t k = i; k < j; ++k) {
      if (slots[order[k]].status == CorpusFitStatus::Ok) {
        date_has_ok = true;
        break;
      }
    }

    man.dates.push_back(date);

    for (std::size_t k = i; k < j; ++k) {
      const std::size_t idx = order[k];
      const FitSlot& s = slots[idx];
      CorpusEntry e{};
      e.date = boards[idx].date;
      e.symbol = boards[idx].symbol;
      e.status = s.status;
      e.chosen_kind = s.chosen_kind;
      e.n_slices = s.n_slices;
      e.oos_in_band = s.oos_in_band;
      e.error_code = s.error_code;
      if (s.status == CorpusFitStatus::Ok && date_has_ok) {
        e.archive_path = apath;
      }
      switch (s.status) {
        case CorpusFitStatus::Ok:
          ++man.n_ok;
          break;
        case CorpusFitStatus::Failed:
          ++man.n_failed;
          break;
        case CorpusFitStatus::Skipped:
          ++man.n_skipped;
          break;
      }
      man.entries.push_back(std::move(e));
    }

    if (date_has_ok) {
      // Distinct per-symbol uid at write (S1-1 — the multi-name northstar
      // blocker). Each board fits in its own single-symbol `Universe`, so
      // `slots[idx].surface`'s in-memory uid is ALWAYS 1 (universe.cpp:71 — the
      // sole interned ticker of a fresh Universe). Left unstamped, a date with
      // more than one Ok symbol would archive every surface at uid=1 and
      // `MarketSnapshot::load`'s `SurfaceSet::create` would reject the archive
      // ("duplicate uid", portfolio_pricer.cpp). Stamp a symbol-derived uid
      // (`uid_for_symbol`) onto an ARCHIVED COPY — `with_uid` deep-clones
      // curves + context, a one-time cost at corpus-write time, not the pricing
      // hot path — so the in-memory `slots[idx].surface` (and any live session
      // built from the same board) is untouched: single-symbol served/session
      // pricing keeps uid=1 exactly as before.
      std::vector<PricedSurface> restamped;  // owns this date's uid-corrected copies
      restamped.reserve(j - i);
      // Non-owning items into `restamped` (reserved above, so no reallocation
      // invalidates the pointers taken below) + the still-live `boards` storage
      // (outlives this write). Symbol-ascending, matching the archive's own
      // directory sort.
      std::vector<SurfaceArchiveItem> items;
      items.reserve(j - i);
      for (std::size_t k = i; k < j; ++k) {
        const std::size_t idx = order[k];
        if (slots[idx].status == CorpusFitStatus::Ok) {
          const std::uint32_t uid = uid_for_symbol(boards[idx].symbol);
          Result<PricedSurface> stamped = with_uid(slots[idx].surface.value(), uid);
          if (!stamped) {
            return Err(stamped.error());
          }
          restamped.push_back(std::move(*stamped));
          items.push_back(SurfaceArchiveItem{boards[idx].symbol, &restamped.back()});
        }
      }
      const Status w = write_surface_archive_file(apath, items, cfg.write_opts);
      if (!w) {
        return Err(w.error());  // propagate IoError / AlreadyExists
      }
    }

    i = j;
  }

  // ── Manifest file ──────────────────────────────────────────────────────────
  const std::string mpath =
      (std::filesystem::path(out_dir) / "manifest.tsv").generic_string();
  ATX_TRY_VOID(write_manifest_file(mpath, man));

  return Ok(std::move(man));
}

// ── Manifest TSV (de)serialization ──────────────────────────────────────────

namespace {

constexpr std::string_view kManifestMagic = "atx-corpus-manifest\tv1";

// Append an unsigned integer as decimal text.
void append_u32(std::string& out, std::uint32_t v) {
  char buf[16];
  const auto [ptr, ec] = std::to_chars(buf, buf + sizeof buf, v);
  (void)ec;  // to_chars on a 16-byte buffer for a uint32 never fails
  out.append(buf, static_cast<std::size_t>(ptr - buf));
}

// Append a double at shortest round-trip precision (locale-independent).
void append_double(std::string& out, double v) {
  char buf[64];
  const auto [ptr, ec] = std::to_chars(buf, buf + sizeof buf, v);
  (void)ec;  // 64 bytes is always sufficient for a double
  out.append(buf, static_cast<std::size_t>(ptr - buf));
}

// Split `line` on '\t', preserving empty fields (including a trailing one).
[[nodiscard]] std::vector<std::string_view> split_tabs(std::string_view line) {
  std::vector<std::string_view> out;
  std::size_t start = 0;
  for (;;) {
    const std::size_t tab = line.find('\t', start);
    if (tab == std::string_view::npos) {
      out.push_back(line.substr(start));
      break;
    }
    out.push_back(line.substr(start, tab - start));
    start = tab + 1;
  }
  return out;
}

// Split `text` on '\n', dropping a single trailing empty line (the final '\n').
[[nodiscard]] std::vector<std::string_view> split_lines(std::string_view text) {
  std::vector<std::string_view> out;
  std::size_t start = 0;
  for (;;) {
    const std::size_t nl = text.find('\n', start);
    if (nl == std::string_view::npos) {
      if (start < text.size()) {
        out.push_back(text.substr(start));
      }
      break;
    }
    out.push_back(text.substr(start, nl - start));
    start = nl + 1;
  }
  return out;
}

[[nodiscard]] bool parse_u32(std::string_view sv, std::uint32_t& out) noexcept {
  const auto [ptr, ec] = std::from_chars(sv.data(), sv.data() + sv.size(), out);
  return ec == std::errc() && ptr == sv.data() + sv.size();
}

[[nodiscard]] bool parse_double(std::string_view sv, double& out) noexcept {
  const auto [ptr, ec] = std::from_chars(sv.data(), sv.data() + sv.size(), out);
  return ec == std::errc() && ptr == sv.data() + sv.size();
}

[[nodiscard]] bool to_fit_status(std::uint32_t v, CorpusFitStatus& out) noexcept {
  switch (v) {
    case 0u: out = CorpusFitStatus::Ok; return true;
    case 1u: out = CorpusFitStatus::Failed; return true;
    case 2u: out = CorpusFitStatus::Skipped; return true;
    default: return false;
  }
}

[[nodiscard]] bool to_curve_kind(std::uint32_t v, VolCurveKind& out) noexcept {
  switch (v) {
    case 0u: out = VolCurveKind::ConvexDense; return true;
    case 1u: out = VolCurveKind::Essvi; return true;
    case 2u: out = VolCurveKind::Svi; return true;
    default: return false;
  }
}

// ErrorCode spans 0..10 (see atx/core/error.hpp).
[[nodiscard]] bool to_error_code(std::uint32_t v, ErrorCode& out) noexcept {
  if (v > static_cast<std::uint32_t>(ErrorCode::ParseError)) {
    return false;
  }
  out = static_cast<ErrorCode>(v);
  return true;
}

}  // namespace

std::string serialize_manifest(const CorpusManifest& m) {
  std::string out;
  out.append(kManifestMagic);
  out.push_back('\n');

  out.append("counts");
  for (const std::uint32_t v : {m.n_boards, m.n_ok, m.n_failed, m.n_skipped}) {
    out.push_back('\t');
    append_u32(out, v);
  }
  out.push_back('\n');

  out.append("dates");
  for (const std::string& d : m.dates) {
    out.push_back('\t');
    out.append(d);
  }
  out.push_back('\n');

  out.append("date\tsymbol\tstatus\tchosen_kind\tn_slices\toos_in_band\terror_code\tarchive_path");
  out.push_back('\n');

  for (const CorpusEntry& e : m.entries) {
    out.append(e.date);
    out.push_back('\t');
    out.append(e.symbol);
    out.push_back('\t');
    append_u32(out, static_cast<std::uint32_t>(e.status));
    out.push_back('\t');
    append_u32(out, static_cast<std::uint32_t>(e.chosen_kind));
    out.push_back('\t');
    append_u32(out, e.n_slices);
    out.push_back('\t');
    append_double(out, e.oos_in_band);
    out.push_back('\t');
    append_u32(out, static_cast<std::uint32_t>(e.error_code));
    out.push_back('\t');
    out.append(e.archive_path);
    out.push_back('\n');
  }
  return out;
}

Result<CorpusManifest> parse_manifest(std::string_view tsv) {
  const std::vector<std::string_view> lines = split_lines(tsv);
  if (lines.size() < 4) {
    return Err(ErrorCode::ParseError, "parse_manifest: truncated (need >= 4 lines)");
  }
  if (lines[0] != kManifestMagic) {
    return Err(ErrorCode::ParseError, "parse_manifest: bad magic");
  }

  CorpusManifest m{};

  // Line 1: counts.
  {
    const std::vector<std::string_view> f = split_tabs(lines[1]);
    if (f.size() != 5 || f[0] != "counts") {
      return Err(ErrorCode::ParseError, "parse_manifest: bad counts line");
    }
    if (!parse_u32(f[1], m.n_boards) || !parse_u32(f[2], m.n_ok) ||
        !parse_u32(f[3], m.n_failed) || !parse_u32(f[4], m.n_skipped)) {
      return Err(ErrorCode::ParseError, "parse_manifest: non-numeric count");
    }
  }

  // Line 2: dates.
  {
    const std::vector<std::string_view> f = split_tabs(lines[2]);
    if (f.empty() || f[0] != "dates") {
      return Err(ErrorCode::ParseError, "parse_manifest: bad dates line");
    }
    m.dates.reserve(f.size() - 1);
    for (std::size_t k = 1; k < f.size(); ++k) {
      m.dates.emplace_back(f[k]);
    }
  }

  // Line 3: column header (validated loosely). Lines 4+: entries.
  {
    const std::vector<std::string_view> h = split_tabs(lines[3]);
    if (h.empty() || h[0] != "date") {
      return Err(ErrorCode::ParseError, "parse_manifest: bad column header");
    }
  }

  m.entries.reserve(lines.size() - 4);
  for (std::size_t li = 4; li < lines.size(); ++li) {
    const std::vector<std::string_view> f = split_tabs(lines[li]);
    if (f.size() != 8) {
      return Err(ErrorCode::ParseError, "parse_manifest: entry row must have 8 fields");
    }
    CorpusEntry e{};
    e.date = std::string(f[0]);
    e.symbol = std::string(f[1]);

    std::uint32_t status_v = 0;
    std::uint32_t kind_v = 0;
    std::uint32_t err_v = 0;
    if (!parse_u32(f[2], status_v) || !to_fit_status(status_v, e.status)) {
      return Err(ErrorCode::ParseError, "parse_manifest: bad status");
    }
    if (!parse_u32(f[3], kind_v) || !to_curve_kind(kind_v, e.chosen_kind)) {
      return Err(ErrorCode::ParseError, "parse_manifest: bad chosen_kind");
    }
    if (!parse_u32(f[4], e.n_slices)) {
      return Err(ErrorCode::ParseError, "parse_manifest: bad n_slices");
    }
    if (!parse_double(f[5], e.oos_in_band)) {
      return Err(ErrorCode::ParseError, "parse_manifest: bad oos_in_band");
    }
    if (!parse_u32(f[6], err_v) || !to_error_code(err_v, e.error_code)) {
      return Err(ErrorCode::ParseError, "parse_manifest: bad error_code");
    }
    e.archive_path = std::string(f[7]);
    m.entries.push_back(std::move(e));
  }

  return Ok(std::move(m));
}

Status write_manifest_file(std::string_view path, const CorpusManifest& m) {
  const std::filesystem::path dst{std::string(path)};
  if (dst.has_parent_path()) {
    std::error_code ec;
    std::filesystem::create_directories(dst.parent_path(), ec);
    if (ec) {
      return Err(ErrorCode::IoError, "write_manifest_file: cannot create parent dir");
    }
  }

  const std::string text = serialize_manifest(m);
  std::filesystem::path tmp = dst;
  tmp += ".tmp";
  {
    std::ofstream os(tmp, std::ios::binary | std::ios::trunc);
    if (!os) {
      return Err(ErrorCode::IoError, "write_manifest_file: cannot open temp file");
    }
    os.write(text.data(), static_cast<std::streamsize>(text.size()));
    if (!os) {
      std::error_code ec;
      std::filesystem::remove(tmp, ec);
      return Err(ErrorCode::IoError, "write_manifest_file: write failed");
    }
  }
  std::error_code ec;
  std::filesystem::rename(tmp, dst, ec);
  if (ec) {
    std::error_code ec2;
    std::filesystem::remove(tmp, ec2);
    return Err(ErrorCode::IoError, "write_manifest_file: rename failed");
  }
  return Ok();
}

Result<CorpusManifest> read_manifest_file(std::string_view path) {
  const std::filesystem::path p{std::string(path)};
  std::error_code ec;
  if (!std::filesystem::exists(p, ec) || ec) {
    return Err(ErrorCode::NotFound, "read_manifest_file: file not found");
  }
  std::ifstream is(p, std::ios::binary | std::ios::ate);
  if (!is) {
    return Err(ErrorCode::IoError, "read_manifest_file: cannot open file");
  }
  const std::streamoff size = is.tellg();
  if (size < 0) {
    return Err(ErrorCode::IoError, "read_manifest_file: cannot size file");
  }
  is.seekg(0);
  std::string text(static_cast<std::size_t>(size), '\0');
  if (size > 0) {
    is.read(text.data(), size);
    if (!is) {
      return Err(ErrorCode::IoError, "read_manifest_file: read failed");
    }
  }
  return parse_manifest(text);
}

}  // namespace atx::vol
