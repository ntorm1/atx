// ATXVFA fit-archive bindings (lane PY, 2026-10-23): read the sprint's fitted-surface
// archives from Python. Spec: atx-vol/docs/storage/ARCHIVE_FORMAT.md; C++ API:
// include/atx/vol/api/storage/fit_archive{,_records,_series}.hpp.
//
// OWNERSHIP. `FitArchive` holds the C++ archive by shared_ptr. Record accessors return
// COPIES (the records are small trivially-copyable structs, with their string references
// resolved at copy time). Pool accessors (quote rows, price nodes, curve words, ...)
// return ZERO-COPY read-only numpy views whose base object holds its own share of the
// archive, so a view stays valid after `close()` (and keeps a mapped file mapped until
// the last view dies); `close()` only stops the archive answering new calls.
//
// REFUSALS. Every ArchiveError surfaces as `FitArchiveError` (a subclass of AtxError)
// carrying the typed `status` (ArchiveStatus), the `section` involved, the
// status-specific `detail`, and `code` (the nearest ErrorCode, so `except AtxError`
// dispatch keeps working). Never a silent number.
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <pybind11/numpy.h>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include "atx/vol/api/fitting/quote_slice.hpp"
#include "atx/vol/api/fitting/vol_curve.hpp"
#include "atx/vol/api/storage/fit_archive.hpp"
#include "atx/vol/api/storage/fit_archive_records.hpp"
#include "atx/vol/api/storage/fit_archive_series.hpp"
#include "common.hpp"

namespace py = pybind11;
using namespace atx::vol;
using atx::core::ErrorCode;

namespace {

// ── Typed refusals ──────────────────────────────────────────────────────────────────

class FitArchiveException : public std::runtime_error {
public:
  explicit FitArchiveException(const ArchiveError &error)
      : std::runtime_error(message_of(error)), error_(error) {}
  [[nodiscard]] const ArchiveError &error() const noexcept { return error_; }

  static std::string message_of(const ArchiveError &e) {
    std::string out = std::string{"fit archive refused: "} + to_string(e.status);
    if (e.section != kFitArchiveNone) {
      out += " (section ";
      out += to_string(static_cast<FitArchiveSectionType>(e.section));
      out += ", detail " + std::to_string(e.detail) + ")";
    } else if (e.detail != 0) {
      out += " (detail " + std::to_string(e.detail) + ")";
    }
    return out;
  }

private:
  ArchiveError error_;
};

// The nearest ErrorCode, so code that dispatches on AtxError.code keeps working.
ErrorCode error_code_of(ArchiveStatus status) noexcept {
  switch (status) {
  case ArchiveStatus::Ok:
    return ErrorCode::Unknown;
  case ArchiveStatus::InvalidArgument:
  case ArchiveStatus::Misaligned:
    return ErrorCode::InvalidArgument;
  case ArchiveStatus::NotFound:
    return ErrorCode::NotFound;
  case ArchiveStatus::IoError:
    return ErrorCode::IoError;
  case ArchiveStatus::DuplicateKey:
    return ErrorCode::AlreadyExists;
  case ArchiveStatus::Busy:
    return ErrorCode::Unavailable;
  case ArchiveStatus::UnsupportedEndian:
  case ArchiveStatus::UnsupportedMajor:
  case ArchiveStatus::UnsupportedSection:
  case ArchiveStatus::UnsupportedRecordVersion:
    return ErrorCode::NotImplemented;
  case ArchiveStatus::Truncated:
  case ArchiveStatus::SizeMismatch:
  case ArchiveStatus::BadMagic:
  case ArchiveStatus::HeaderInvalid:
  case ArchiveStatus::HeaderChecksum:
  case ArchiveStatus::FooterInvalid:
  case ArchiveStatus::FooterChecksum:
  case ArchiveStatus::FooterMismatch:
  case ArchiveStatus::SectionTableChecksum:
  case ArchiveStatus::SectionBounds:
  case ArchiveStatus::SectionOverlap:
  case ArchiveStatus::SectionAlignment:
  case ArchiveStatus::SectionStride:
  case ArchiveStatus::DuplicateSection:
  case ArchiveStatus::MissingSection:
  case ArchiveStatus::SectionChecksum:
  case ArchiveStatus::PaddingNotZero:
  case ArchiveStatus::FileChecksum:
  case ArchiveStatus::IndexUnsorted:
  case ArchiveStatus::IndexBounds:
  case ArchiveStatus::RecordBounds:
  case ArchiveStatus::RecordInvalid:
  case ArchiveStatus::LayoutMismatch:
  case ArchiveStatus::SegmentMismatch:
    return ErrorCode::ParseError;
  }
  return ErrorCode::Unknown;
}

std::span<const std::byte> bytes_view(const py::bytes &data) {
  const std::string_view v{data};
  return std::as_bytes(std::span<const char>{v.data(), v.size()});
}

py::bytes to_bytes(std::span<const std::byte> data) {
  // SAFETY: std::byte -> char is an object-representation read (both are byte types
  // the aliasing rules allow); py::bytes copies the range.
  return py::bytes(reinterpret_cast<const char *>(data.data()), data.size());
}

template <class T> T unwrap_archive(ArchiveResult<T> result) {
  if (!result) throw FitArchiveException(result.error());
  return std::move(*result);
}

void unwrap_archive(const ArchiveOutcome &outcome) {
  if (!outcome) throw FitArchiveException(outcome.error());
}

// ── Python-side record copies ───────────────────────────────────────────────────────

std::string string_of(const FitArchive &a, FitArchiveStringRef ref) {
  return std::string{a.string(ref)};
}

struct ArchiveHold { // the numpy views' base: one share of the archive
  std::shared_ptr<const FitArchive> archive;
};

struct PyFitRecord {
  FitArchiveFitRecord rec{};
  std::size_t index = 0;
  std::string underlier, fit_error, carry_reason, refusal, error, point_error;
};

struct PySurfaceRecord {
  FitArchiveSurfaceRecord rec{};
  std::size_t index = 0;
  std::string underlier, profile, policy_source;
};

py::dict key_dict(const FitArchive &a, const FitArchiveKey &k) {
  py::dict d;
  d["underlier"] = std::string{a.underlier_name(k.underlier)};
  d["underlier_id"] = k.underlier;
  d["snapshot_ns"] = k.snapshot_ns;
  d["expiry_day"] = k.expiry_day;
  d["sub"] = k.sub;
  return d;
}

py::dict market_dict(const FitArchiveMarket &m) {
  py::dict d;
  d["spot"] = m.spot;
  d["variance_years"] = m.variance_years;
  d["calendar_years"] = m.calendar_years;
  d["rate"] = m.rate;
  d["continuous_yield"] = m.continuous_yield;
  d["dividend_pv"] = m.dividend_pv;
  return d;
}

py::dict repair_dict(const FitArchiveRepair &r) {
  py::dict d;
  d["nodes"] = r.nodes;
  d["material_nodes"] = r.material_nodes;
  d["unidentified"] = r.unidentified;
  d["max_price_move"] = r.max_price_move;
  d["max_vol_move"] = r.max_vol_move;
  d["max_unquoted_vol_move"] = r.max_unquoted_vol_move;
  d["outcome"] = r.outcome;
  return d;
}

py::dict diagnostics_dict(const FitArchiveDiagnostics &g) {
  py::dict d;
  d["quotes_seen"] = g.quotes_seen;
  d["iv_attempted"] = g.iv_attempted;
  d["quotes_admitted"] = g.quotes_admitted;
  d["distinct_strikes"] = g.distinct_strikes;
  d["prices_produced"] = g.prices_produced;
  d["weights_capped"] = g.weights_capped;
  d["forward"] = g.forward;
  d["discount"] = g.discount;
  d["effective_rate"] = g.effective_rate;
  d["effective_yield"] = g.effective_yield;
  py::dict c;
  c["rmse_vol_vega_weighted"] = g.curve_fit.rmse_vol_vega_weighted;
  c["max_residual_vol"] = g.curve_fit.max_residual_vol;
  c["final_grad_norm"] = (g.curve_fit.flags & kFitDiagHasGradNorm) != 0
                             ? py::object(py::float_(g.curve_fit.final_grad_norm))
                             : py::object(py::none());
  c["n_quotes_used"] = g.curve_fit.n_quotes_used;
  c["n_butterfly_viol"] = g.curve_fit.n_butterfly_viol;
  c["n_calendar_viol"] = g.curve_fit.n_calendar_viol;
  c["outer_iters"] = g.curve_fit.outer_iters;
  c["inner_iters_total"] = g.curve_fit.inner_iters_total;
  c["termination"] = g.curve_fit.termination;
  c["projection"] = g.curve_fit.projection;
  c["reverted_to_seed"] = (g.curve_fit.flags & kFitDiagRevertedToSeed) != 0;
  c["warm_started"] = (g.curve_fit.flags & kFitDiagWarmStarted) != 0;
  d["curve_fit"] = c;
  py::dict ch;
  ch["channel_quotes"] = g.channel.channel_quotes;
  ch["mid_quotes"] = g.channel.mid_quotes;
  ch["mid_strikes"] = g.channel.mid_strikes;
  ch["violations"] = g.channel.violations;
  ch["iterations"] = g.channel.iterations;
  ch["repaired_nodes"] = g.channel.repaired_nodes;
  ch["max_violation_half_widths"] = g.channel.max_violation_half_widths;
  ch["prior_strength"] = g.channel.prior_strength;
  ch["repair_max_price"] = g.channel.repair_max_price;
  ch["repair_max_half_spreads"] = g.channel.repair_max_half_spreads;
  ch["used"] = (g.channel.flags & kChannelUsed) != 0;
  ch["term_prior"] = (g.channel.flags & kChannelTermPrior) != 0;
  ch["converged"] = (g.channel.flags & kChannelConverged) != 0;
  d["channel"] = ch;
  d["repair"] = repair_dict(g.repair);
  return d;
}

py::dict carry_dict(const FitArchiveCarry &c, const std::string &reason) {
  py::dict d;
  d["supplied_forward"] = c.supplied_forward;
  d["supplied_yield"] = c.supplied_yield;
  d["implied_forward"] = c.implied_forward;
  d["implied_yield"] = c.implied_yield;
  d["implied_total_yield"] = c.implied_total_yield;
  d["parity_forward"] = c.parity_forward;
  d["forward_lower"] = c.forward_lower;
  d["forward_upper"] = c.forward_upper;
  d["dispersion_sd"] = c.dispersion_sd;
  d["exercise_shift_sd"] = c.exercise_shift_sd;
  d["atm_total_vol"] = c.atm_total_vol;
  d["pairs_valid"] = c.pairs_valid;
  d["pairs_selected"] = c.pairs_selected;
  d["pairs_retained"] = c.pairs_retained;
  d["pairs_contaminated"] = c.pairs_contaminated;
  d["pairs_unidentified"] = c.pairs_unidentified;
  d["reason"] = reason;
  d["mode"] = c.mode;
  d["applied"] = c.applied != 0;
  d["fallback"] = c.fallback;
  d["status"] = c.status;
  return d;
}

py::dict slice_dict(const PyFitRecord &r) {
  const FitArchiveSlice &s = r.rec.slice;
  py::dict d;
  d["present"] = s.present != 0;
  d["grade"] = static_cast<QuoteSliceGrade>(s.grade);
  d["point_status"] = static_cast<QuoteSliceFitStatus>(s.point_status);
  d["calendar_gap"] = s.calendar_gap;
  d["calendar_gap_strike"] = s.calendar_gap_strike;
  d["calendar_repair_required"] = s.calendar_repair_required;
  d["point_outside_fraction"] = s.point_outside_fraction;
  d["point_outside_quotes"] = s.point_outside_quotes;
  d["rung_repair"] = repair_dict(s.rung_repair);
  d["calendar_channel_repair"] = repair_dict(s.calendar_channel_repair);
  d["refusal"] = r.refusal;
  d["error"] = r.error;
  d["point_error"] = r.point_error;
  d["channel_refit"] = (s.flags & kSliceChannelRefit) != 0;
  d["calendar_retry"] = (s.flags & kSliceCalendarRetry) != 0;
  d["calendar_repaired"] = (s.flags & kSliceCalendarRepaired) != 0;
  d["repair_rung"] = (s.flags & kSliceRepairRung) != 0;
  d["calendar_channel_repaired"] = (s.flags & kSliceCalendarChannelRepaired) != 0;
  d["point_gated"] = (s.flags & kSlicePointGated) != 0;
  d["point_fallback"] = (s.flags & kSlicePointFallback) != 0;
  d["calendar_readmitted"] = (s.flags & kSliceCalendarReadmitted) != 0;
  return d;
}

PyFitRecord copy_fit(const FitArchive &a, std::size_t index) {
  const FitArchiveFitRecord &rec = a.fits()[index];
  PyFitRecord out;
  out.rec = rec;
  out.index = index;
  out.underlier = std::string{a.underlier_name(rec.key.underlier)};
  out.fit_error = string_of(a, rec.fit_error);
  out.carry_reason = string_of(a, rec.carry.reason);
  out.refusal = string_of(a, rec.slice.refusal);
  out.error = string_of(a, rec.slice.error);
  out.point_error = string_of(a, rec.slice.point_error);
  return out;
}

PySurfaceRecord copy_surface(const FitArchive &a, std::size_t index) {
  const FitArchiveSurfaceRecord &rec = a.surfaces()[index];
  PySurfaceRecord out;
  out.rec = rec;
  out.index = index;
  out.underlier = std::string{a.underlier_name(rec.key.underlier)};
  out.profile = string_of(a, rec.profile);
  out.policy_source = string_of(a, rec.policy_source);
  return out;
}

// ── The archive handle ──────────────────────────────────────────────────────────────

py::dtype quote_row_dtype() {
  using R = FitArchiveQuoteRow;
  py::list names, formats, offsets;
  const auto add = [&](const char *name, const char *fmt, std::size_t off) {
    names.append(name);
    formats.append(fmt);
    offsets.append(off);
  };
  add("market_iv", "<f8", offsetof(R, market_iv));
  add("fitted_iv", "<f8", offsetof(R, fitted_iv));
  add("fitted_price", "<f8", offsetof(R, fitted_price));
  add("spread_vol", "<f8", offsetof(R, spread_vol));
  add("weight_w", "<f8", offsetof(R, weight_w));
  add("quote_status", "u1", offsetof(R, quote_status));
  add("evaluation_status", "u1", offsetof(R, evaluation_status));
  add("admitted", "u1", offsetof(R, admitted));
  add("weight_capped", "u1", offsetof(R, weight_capped));
  add("iv_lower", "<f8", offsetof(R, iv_lower));
  add("iv_upper", "<f8", offsetof(R, iv_upper));
  add("price_only_reason", "u1", offsetof(R, price_only_reason));
  add("vol_source", "u1", offsetof(R, vol_source));
  add("flags", "u1", offsetof(R, flags));
  return py::dtype(names, formats, offsets, sizeof(R));
}

py::dtype pair_dtype(const char *a, const char *b, std::size_t off_b, std::size_t size) {
  py::list names, formats, offsets;
  names.append(a);
  names.append(b);
  formats.append("<f8");
  formats.append("<f8");
  offsets.append(0);
  offsets.append(off_b);
  return py::dtype(names, formats, offsets, static_cast<py::ssize_t>(size));
}

py::dtype price_node_dtype() {
  py::list names, formats, offsets;
  for (const char *n : {"strike", "call", "put"}) {
    names.append(n);
    formats.append("<f8");
  }
  offsets.append(offsetof(AmericanPriceNode, strike));
  offsets.append(offsetof(AmericanPriceNode, call));
  offsets.append(offsetof(AmericanPriceNode, put));
  return py::dtype(names, formats, offsets, sizeof(AmericanPriceNode));
}

class PyFitArchive {
public:
  explicit PyFitArchive(std::shared_ptr<const FitArchive> archive) : archive_(std::move(archive)) {}

  static PyFitArchive open(const std::string &path, FitArchiveOpenMode mode,
                           FitArchiveVerify verify) {
    FitArchive a = unwrap_archive(FitArchive::open(path, FitArchiveOpenOptions{mode, verify}));
    return PyFitArchive{std::make_shared<const FitArchive>(std::move(a))};
  }
  static PyFitArchive from_bytes(const py::bytes &data, FitArchiveVerify verify) {
    // open_copy copies once into an aligned owned buffer: the Python bytes may die.
    FitArchive a = unwrap_archive(FitArchive::open_copy(bytes_view(data), verify));
    return PyFitArchive{std::make_shared<const FitArchive>(std::move(a))};
  }

  void close() noexcept { archive_.reset(); }
  [[nodiscard]] bool closed() const noexcept { return archive_ == nullptr; }

  [[nodiscard]] const FitArchive &get() const {
    if (!archive_) throw py::value_error("FitArchive is closed");
    return *archive_;
  }
  [[nodiscard]] const std::shared_ptr<const FitArchive> &share() const {
    static_cast<void>(get());
    return archive_;
  }

  [[nodiscard]] std::size_t fit_index(std::size_t i) const {
    if (i >= get().fits().size()) throw py::index_error("fit index out of range");
    return i;
  }
  [[nodiscard]] const FitArchiveFitRecord &fit(std::size_t i) const {
    return get().fits()[fit_index(i)];
  }

  template <class T>
  [[nodiscard]] py::array view(std::span<const T> s, const py::dtype &dtype) const {
    py::object base = py::cast(ArchiveHold{share()});
    return atxvol::python::readonly_view(dtype, s.data(), s.size(), sizeof(T), base);
  }

private:
  std::shared_ptr<const FitArchive> archive_;
};

std::uint32_t resolve_underlier(const FitArchive &a, const py::object &underlier) {
  if (py::isinstance<py::str>(underlier)) {
    const auto id = a.find_underlier(underlier.cast<std::string>());
    if (!id) throw FitArchiveException(ArchiveError{ArchiveStatus::NotFound, kFitArchiveNone, 0});
    return *id;
  }
  return underlier.cast<std::uint32_t>();
}

py::object index_or_none(const void *found, const void *base, std::size_t stride) {
  if (found == nullptr) return py::none();
  const auto off = static_cast<std::size_t>(static_cast<const std::byte *>(found) -
                                            static_cast<const std::byte *>(base));
  return py::int_(off / stride);
}

py::dict header_dict(const FitArchive &a) {
  const FitArchiveHeader &h = a.header();
  py::dict d;
  d["format_major"] = h.format_major;
  d["format_minor"] = h.format_minor;
  d["file_size"] = h.file_size;
  d["section_count"] = h.section_count;
  d["created_unix_ns"] = h.created_unix_ns;
  d["writer_version"] = h.writer_version;
  d["archive_id"] = h.archive_id;
  d["segment_sequence"] = h.segment_sequence;
  std::size_t n = 0;
  while (n < h.writer_name.size() && h.writer_name[n] != '\0') ++n;
  d["writer_name"] = std::string{h.writer_name.data(), n};
  return d;
}

py::list sections_list(const FitArchive &a) {
  py::list out;
  for (const FitArchiveSectionEntry &e : a.sections()) {
    py::dict d;
    d["type"] = e.type;
    d["name"] = std::string{to_string(static_cast<FitArchiveSectionType>(e.type))};
    d["type_major"] = e.type_major;
    d["type_minor"] = e.type_minor;
    d["offset"] = e.offset;
    d["length"] = e.length;
    d["element_count"] = e.element_count;
    d["element_stride"] = e.element_stride;
    d["required"] = (e.flags & kFitArchiveSectionRequired) != 0;
    out.append(d);
  }
  return out;
}

void bind_enums(py::module_ &m) {
  py::enum_<ArchiveStatus>(m, "ArchiveStatus")
      .value("OK", ArchiveStatus::Ok)
      .value("INVALID_ARGUMENT", ArchiveStatus::InvalidArgument)
      .value("NOT_FOUND", ArchiveStatus::NotFound)
      .value("IO_ERROR", ArchiveStatus::IoError)
      .value("TRUNCATED", ArchiveStatus::Truncated)
      .value("SIZE_MISMATCH", ArchiveStatus::SizeMismatch)
      .value("BAD_MAGIC", ArchiveStatus::BadMagic)
      .value("UNSUPPORTED_ENDIAN", ArchiveStatus::UnsupportedEndian)
      .value("UNSUPPORTED_MAJOR", ArchiveStatus::UnsupportedMajor)
      .value("HEADER_INVALID", ArchiveStatus::HeaderInvalid)
      .value("HEADER_CHECKSUM", ArchiveStatus::HeaderChecksum)
      .value("FOOTER_INVALID", ArchiveStatus::FooterInvalid)
      .value("FOOTER_CHECKSUM", ArchiveStatus::FooterChecksum)
      .value("FOOTER_MISMATCH", ArchiveStatus::FooterMismatch)
      .value("SECTION_TABLE_CHECKSUM", ArchiveStatus::SectionTableChecksum)
      .value("SECTION_BOUNDS", ArchiveStatus::SectionBounds)
      .value("SECTION_OVERLAP", ArchiveStatus::SectionOverlap)
      .value("SECTION_ALIGNMENT", ArchiveStatus::SectionAlignment)
      .value("SECTION_STRIDE", ArchiveStatus::SectionStride)
      .value("DUPLICATE_SECTION", ArchiveStatus::DuplicateSection)
      .value("MISSING_SECTION", ArchiveStatus::MissingSection)
      .value("UNSUPPORTED_SECTION", ArchiveStatus::UnsupportedSection)
      .value("UNSUPPORTED_RECORD_VERSION", ArchiveStatus::UnsupportedRecordVersion)
      .value("SECTION_CHECKSUM", ArchiveStatus::SectionChecksum)
      .value("PADDING_NOT_ZERO", ArchiveStatus::PaddingNotZero)
      .value("FILE_CHECKSUM", ArchiveStatus::FileChecksum)
      .value("INDEX_UNSORTED", ArchiveStatus::IndexUnsorted)
      .value("INDEX_BOUNDS", ArchiveStatus::IndexBounds)
      .value("RECORD_BOUNDS", ArchiveStatus::RecordBounds)
      .value("RECORD_INVALID", ArchiveStatus::RecordInvalid)
      .value("LAYOUT_MISMATCH", ArchiveStatus::LayoutMismatch)
      .value("MISALIGNED", ArchiveStatus::Misaligned)
      .value("DUPLICATE_KEY", ArchiveStatus::DuplicateKey)
      .value("SEGMENT_MISMATCH", ArchiveStatus::SegmentMismatch)
      .value("BUSY", ArchiveStatus::Busy);
  atxvol::python::register_enum_names<ArchiveStatus>("ArchiveStatus");

  py::enum_<FitArchiveSectionType>(m, "FitArchiveSectionType")
      .value("SYMBOLS", FitArchiveSectionType::Symbols)
      .value("STRINGS", FitArchiveSectionType::Strings)
      .value("PROVENANCE", FitArchiveSectionType::Provenance)
      .value("SURFACE_RECORDS", FitArchiveSectionType::SurfaceRecords)
      .value("SURFACE_INDEX", FitArchiveSectionType::SurfaceIndex)
      .value("FIT_RECORDS", FitArchiveSectionType::FitRecords)
      .value("FIT_INDEX", FitArchiveSectionType::FitIndex)
      .value("PRICE_NODES", FitArchiveSectionType::PriceNodes)
      .value("QUOTE_ROWS", FitArchiveSectionType::QuoteRows)
      .value("MOVED_NODES", FitArchiveSectionType::MovedNodes)
      .value("DIVIDENDS", FitArchiveSectionType::Dividends)
      .value("CARRY_POINTS", FitArchiveSectionType::CarryPoints)
      .value("CURVE_WORDS", FitArchiveSectionType::CurveWords)
      .value("FIT_TIMINGS", FitArchiveSectionType::FitTimings)
      .value("PRICE_ONLY_NODES", FitArchiveSectionType::PriceOnlyNodes)
      .value("EXPIRY_META", FitArchiveSectionType::ExpiryMeta)
      .value("EXPIRY_DATA", FitArchiveSectionType::ExpiryData)
      .value("EXPIRY_INDEX", FitArchiveSectionType::ExpiryIndex)
      .value("LADDERS", FitArchiveSectionType::Ladders)
      .value("LADDER_INDEX", FitArchiveSectionType::LadderIndex)
      .value("LADDER_CONTRACTS", FitArchiveSectionType::LadderContracts)
      .value("MARKOV_RECORDS", FitArchiveSectionType::MarkovRecords)
      .value("MARKOV_INDEX", FitArchiveSectionType::MarkovIndex)
      .value("MARKOV_INTERVALS", FitArchiveSectionType::MarkovIntervals)
      .value("MARKOV_LOCAL_VOL", FitArchiveSectionType::MarkovLocalVol)
      .value("MARKOV_WORDS", FitArchiveSectionType::MarkovWords)
      .value("SERIES_SEGMENTS", FitArchiveSectionType::SeriesSegments);
  atxvol::python::register_enum_names<FitArchiveSectionType>("FitArchiveSectionType");

  py::enum_<FitArchiveOpenMode>(m, "FitArchiveOpenMode")
      .value("MAPPED", FitArchiveOpenMode::Mapped)
      .value("READ", FitArchiveOpenMode::Read);
  py::enum_<FitArchiveVerify>(m, "FitArchiveVerify")
      .value("FRAMING", FitArchiveVerify::Framing)
      .value("FULL", FitArchiveVerify::Full)
      .value("PARANOID", FitArchiveVerify::Paranoid);
}

void bind_exception(py::module_ &m) {
  static py::exception<FitArchiveException> exc(m, "FitArchiveError", m.attr("AtxError"));
  exc.attr("status") = py::none();
  exc.attr("section") = py::none();
  exc.attr("section_name") = py::none();
  exc.attr("detail") = py::none();
  py::register_exception_translator([](std::exception_ptr ptr) {
    try {
      if (ptr) std::rethrow_exception(ptr);
    } catch (const FitArchiveException &error) {
      const ArchiveError &e = error.error();
      py::object exc_type = py::reinterpret_borrow<py::object>(py::handle(exc));
      py::object instance = exc_type(error.what());
      instance.attr("status") = py::cast(e.status);
      instance.attr("code") = py::cast(error_code_of(e.status));
      instance.attr("detail") = py::int_(e.detail);
      if (e.section == kFitArchiveNone) {
        instance.attr("section") = py::none();
        instance.attr("section_name") = py::none();
      } else {
        instance.attr("section") = py::int_(e.section);
        instance.attr("section_name") =
            std::string{to_string(static_cast<FitArchiveSectionType>(e.section))};
      }
      py::set_error(exc, instance);
    }
  });
}

void bind_records(py::module_ &m) {
  py::class_<ArchiveHold>(m, "_FitArchiveHold");

  py::class_<atxvol::python::CurveHandle>(m, "FittedVolCurve",
                                          "A read-only fitted IVolCurve (k = ln(K / F)).")
      .def_property_readonly("kind",
                             [](const atxvol::python::CurveHandle &h) { return h.curve->kind(); })
      .def_property_readonly("dof",
                             [](const atxvol::python::CurveHandle &h) { return h.curve->dof(); })
      .def(
          "w",
          [](const atxvol::python::CurveHandle &h,
             const py::array_t<double, py::array::c_style | py::array::forcecast> &k) {
            py::array_t<double> out(k.size());
            auto *o = out.mutable_data();
            const double *in = k.data();
            for (py::ssize_t i = 0; i < k.size(); ++i) o[i] = h.curve->w(in[i]);
            return out.reshape(std::vector<py::ssize_t>(k.shape(), k.shape() + k.ndim()));
          },
          py::arg("k"), "Total implied variance w(k), elementwise over a numpy array.")
      .def(
          "iv",
          [](const atxvol::python::CurveHandle &h,
             const py::array_t<double, py::array::c_style | py::array::forcecast> &k) {
            py::array_t<double> out(k.size());
            auto *o = out.mutable_data();
            const double *in = k.data();
            for (py::ssize_t i = 0; i < k.size(); ++i) o[i] = h.curve->iv(in[i]);
            return out.reshape(std::vector<py::ssize_t>(k.shape(), k.shape() + k.ndim()));
          },
          py::arg("k"), "Implied volatility at log-moneyness k, elementwise.")
      .def(
          "is_extrapolated",
          [](const atxvol::python::CurveHandle &h, double k) {
            return h.curve->is_extrapolated(k);
          },
          py::arg("k"));

  py::class_<PyFitRecord>(m, "FitArchiveFitRecord",
                          "A COPY of one fit record (string references resolved).")
      .def_readonly("index", &PyFitRecord::index)
      .def_readonly("underlier", &PyFitRecord::underlier)
      .def_property_readonly("snapshot_ns", [](const PyFitRecord &r) { return r.rec.key.snapshot_ns; })
      .def_property_readonly("expiry_day", [](const PyFitRecord &r) { return r.rec.key.expiry_day; })
      .def_property_readonly("sub", [](const PyFitRecord &r) { return r.rec.key.sub; })
      .def_property_readonly("fit_status", [](const PyFitRecord &r) {
        return static_cast<QuoteSliceFitStatus>(r.rec.fit_status);
      })
      .def_readonly("fit_error", &PyFitRecord::fit_error)
      .def_property_readonly("dividend_model", [](const PyFitRecord &r) {
        return static_cast<QuoteSliceDividendModel>(r.rec.dividend_model);
      })
      .def_property_readonly("floor_policy", [](const PyFitRecord &r) {
        return static_cast<QuoteSliceFloorPolicy>(r.rec.floor_policy);
      })
      .def_property_readonly("carry_convention", [](const PyFitRecord &r) {
        return static_cast<QuoteSliceCarryConvention>(r.rec.carry_convention);
      })
      .def_property_readonly("spot_rule", [](const PyFitRecord &r) {
        return static_cast<QuoteSliceSpotRule>(r.rec.spot_rule);
      })
      .def_property_readonly("quote_ns", [](const PyFitRecord &r) { return r.rec.quote_ns; })
      .def_property_readonly("underlying_ns",
                             [](const PyFitRecord &r) { return r.rec.underlying_ns; })
      .def_property_readonly("raw_spot",
                             [](const PyFitRecord &r) -> py::object {
                               if ((r.rec.snapshot_flags & kFitSnapshotHasRawSpot) == 0)
                                 return py::none();
                               return py::float_(r.rec.raw_spot);
                             })
      .def_property_readonly("spot_stale", [](const PyFitRecord &r) {
        return (r.rec.snapshot_flags & kFitSnapshotSpotStale) != 0;
      })
      .def_property_readonly("spot_version", [](const PyFitRecord &r) { return r.rec.spot_version; })
      .def_property_readonly("carry_version",
                             [](const PyFitRecord &r) { return r.rec.carry_version; })
      .def_property_readonly("surface",
                             [](const PyFitRecord &r) -> py::object {
                               if (r.rec.surface == kFitArchiveNone) return py::none();
                               return py::int_(r.rec.surface);
                             })
      .def_property_readonly("discrete_div_steps",
                             [](const PyFitRecord &r) { return r.rec.discrete_div_steps; })
      .def_property_readonly("curve_kind",
                             [](const PyFitRecord &r) -> py::object {
                               if (r.rec.curve.present == 0) return py::none();
                               return py::cast(static_cast<VolCurveKind>(r.rec.curve.kind));
                             })
      .def_property_readonly("curve_T", [](const PyFitRecord &r) { return r.rec.curve.T; })
      .def_property_readonly("curve_F", [](const PyFitRecord &r) { return r.rec.curve.F; })
      .def_property_readonly("curve_df", [](const PyFitRecord &r) { return r.rec.curve.df; })
      .def_property_readonly("has_prices",
                             [](const PyFitRecord &r) { return r.rec.prices.present != 0; })
      .def_property_readonly("n_rows", [](const PyFitRecord &r) { return r.rec.rows.count; })
      .def_property_readonly("n_price_nodes",
                             [](const PyFitRecord &r) { return r.rec.prices.nodes.count; })
      .def_property_readonly("market", [](const PyFitRecord &r) { return market_dict(r.rec.market); })
      .def_property_readonly("diagnostics",
                             [](const PyFitRecord &r) { return diagnostics_dict(r.rec.diagnostics); })
      .def_property_readonly("carry",
                             [](const PyFitRecord &r) { return carry_dict(r.rec.carry, r.carry_reason); })
      .def_property_readonly("slice", &slice_dict)
      .def("__repr__", [](const PyFitRecord &r) {
        return "<FitArchiveFitRecord " + std::to_string(r.index) + " " + r.underlier +
               " snapshot_ns=" + std::to_string(r.rec.key.snapshot_ns) +
               " expiry_day=" + std::to_string(r.rec.key.expiry_day) + ">";
      });

  py::class_<PySurfaceRecord>(m, "FitArchiveSurfaceRecord",
                              "A COPY of one surface record (string references resolved).")
      .def_readonly("index", &PySurfaceRecord::index)
      .def_readonly("underlier", &PySurfaceRecord::underlier)
      .def_readonly("profile", &PySurfaceRecord::profile)
      .def_readonly("policy_source", &PySurfaceRecord::policy_source)
      .def_property_readonly("snapshot_ns",
                             [](const PySurfaceRecord &r) { return r.rec.key.snapshot_ns; })
      .def_property_readonly("first_fit", [](const PySurfaceRecord &r) { return r.rec.first_fit; })
      .def_property_readonly("fit_count", [](const PySurfaceRecord &r) { return r.rec.fit_count; })
      .def_property_readonly("grade_counts",
                             [](const PySurfaceRecord &r) {
                               py::dict d;
                               d["A"] = r.rec.grade_counts[0];
                               d["B"] = r.rec.grade_counts[1];
                               d["C"] = r.rec.grade_counts[2];
                               d["Refused"] = r.rec.grade_counts[3];
                               return d;
                             })
      .def_property_readonly("policy_digest",
                             [](const PySurfaceRecord &r) {
                               return to_bytes(std::as_bytes(std::span{r.rec.policy_digest}));
                             })
      .def_property_readonly("config", [](const PySurfaceRecord &r) {
        py::dict d;
        d["term_prior_weight"] = r.rec.term_prior_weight;
        d["prior_decay_log_time"] = r.rec.prior_decay_log_time;
        d["prior_quote_halflife"] = r.rec.prior_quote_halflife;
        d["channel_node_weight"] = r.rec.channel_node_weight;
        d["calendar_retry_factor"] = r.rec.calendar_retry_factor;
        d["calendar_repair_half_spreads"] = r.rec.calendar_repair_half_spreads;
        d["grade_b_min_mid_strikes"] = r.rec.grade_b_min_mid_strikes;
        return d;
      });
}

void bind_archive(py::module_ &m) {
  py::class_<PyFitArchive>(m, "FitArchive",
                           "A validated ATXVFA fit archive (read-only). Use FitArchive.open.")
      .def_static("open", &PyFitArchive::open, py::arg("path"),
                  py::arg("mode") = FitArchiveOpenMode::Mapped,
                  py::arg("verify") = FitArchiveVerify::Full,
                  "Open and validate an archive; raises FitArchiveError on any refusal.")
      .def_static("from_bytes", &PyFitArchive::from_bytes, py::arg("data"),
                  py::arg("verify") = FitArchiveVerify::Full,
                  "Validate an in-memory image (copied once into an aligned buffer).")
      .def("close", &PyFitArchive::close,
           "Stop answering calls. Views already handed out stay valid (they hold the bytes).")
      .def_property_readonly("closed", &PyFitArchive::closed)
      .def("__enter__", [](PyFitArchive &self) -> PyFitArchive & { return self; },
           py::return_value_policy::reference)
      .def("__exit__", [](PyFitArchive &self, const py::args &) { self.close(); })
      .def_property_readonly("header", [](const PyFitArchive &s) { return header_dict(s.get()); })
      .def("sections", [](const PyFitArchive &s) { return sections_list(s.get()); })
      .def("symbols",
           [](const PyFitArchive &s) {
             std::vector<std::string> out;
             const FitArchive &a = s.get();
             for (std::uint32_t i = 0; i < a.symbols().size(); ++i) {
               out.emplace_back(a.underlier_name(i));
             }
             return out;
           })
      .def(
          "find_underlier",
          [](const PyFitArchive &s, const std::string &name) -> py::object {
            const auto id = s.get().find_underlier(name);
            return id ? py::object(py::int_(*id)) : py::object(py::none());
          },
          py::arg("name"))
      .def(
          "underlier_name",
          [](const PyFitArchive &s, std::uint32_t id) {
            return std::string{s.get().underlier_name(id)};
          },
          py::arg("id"))
      .def("provenance", [](const PyFitArchive &s) { return to_bytes(s.get().provenance()); })
      .def_property_readonly("n_surfaces", [](const PyFitArchive &s) { return s.get().surfaces().size(); })
      .def_property_readonly("n_fits", [](const PyFitArchive &s) { return s.get().fits().size(); })
      .def_property_readonly("n_expiry_records",
                             [](const PyFitArchive &s) { return s.get().expiry_meta().size(); })
      .def_property_readonly("n_ladders", [](const PyFitArchive &s) { return s.get().ladders().size(); })
      .def_property_readonly("n_markov", [](const PyFitArchive &s) { return s.get().markov().size(); })
      .def("fits",
           [](const PyFitArchive &s) {
             std::vector<PyFitRecord> out;
             const FitArchive &a = s.get();
             for (std::size_t i = 0; i < a.fits().size(); ++i) out.push_back(copy_fit(a, i));
             return out;
           })
      .def(
          "fit", [](const PyFitArchive &s, std::size_t i) { return copy_fit(s.get(), s.fit_index(i)); },
          py::arg("index"))
      .def("surfaces",
           [](const PyFitArchive &s) {
             std::vector<PySurfaceRecord> out;
             const FitArchive &a = s.get();
             for (std::size_t i = 0; i < a.surfaces().size(); ++i) out.push_back(copy_surface(a, i));
             return out;
           })
      .def(
          "find_fit",
          [](const PyFitArchive &s, const py::object &underlier, std::int64_t snapshot_ns,
             std::int32_t expiry_day, std::uint32_t sub) -> py::object {
            const FitArchive &a = s.get();
            if (py::isinstance<py::str>(underlier) &&
                !a.find_underlier(underlier.cast<std::string>())) {
              return py::none();
            }
            const FitArchiveKey key{resolve_underlier(a, underlier), expiry_day, snapshot_ns, sub,
                                    0};
            return index_or_none(a.find_fit(key), a.fits().data(), sizeof(FitArchiveFitRecord));
          },
          py::arg("underlier"), py::arg("snapshot_ns"), py::arg("expiry_day"), py::arg("sub") = 0,
          "Index of the fit with this key (underlier as a name or an id), or None.")
      .def(
          "find_surface",
          [](const PyFitArchive &s, const py::object &underlier,
             std::int64_t snapshot_ns) -> py::object {
            const FitArchive &a = s.get();
            if (py::isinstance<py::str>(underlier) &&
                !a.find_underlier(underlier.cast<std::string>())) {
              return py::none();
            }
            return index_or_none(a.find_surface(resolve_underlier(a, underlier), snapshot_ns),
                                 a.surfaces().data(), sizeof(FitArchiveSurfaceRecord));
          },
          py::arg("underlier"), py::arg("snapshot_ns"))
      .def(
          "fits_of",
          [](const PyFitArchive &s, std::size_t surface) {
            const FitArchive &a = s.get();
            if (surface >= a.surfaces().size()) throw py::index_error("surface index out of range");
            const FitArchiveSurfaceRecord &rec = a.surfaces()[surface];
            std::vector<std::size_t> out;
            for (std::uint32_t i = 0; i < rec.fit_count; ++i) out.push_back(rec.first_fit + i);
            return out;
          },
          py::arg("surface"), "Fit indices of one surface, in expiry order.")
      // ── zero-copy pools (read-only numpy views; see the file comment) ──
      .def(
          "quote_rows",
          [](const PyFitArchive &s, std::size_t i) {
            return s.view(s.get().quote_rows(s.fit(i)), quote_row_dtype());
          },
          py::arg("fit"), "Per-quote rows of a fit: a structured read-only numpy view.")
      .def(
          "price_nodes",
          [](const PyFitArchive &s, std::size_t i) {
            return s.view(atx::vol::price_nodes(s.get(), s.fit(i)), price_node_dtype());
          },
          py::arg("fit"), "Certified AmericanPriceSlice nodes (strike, call, put).")
      .def(
          "curve_payload",
          [](const PyFitArchive &s, std::size_t i) {
            return s.view(s.get().curve_payload(s.fit(i)), py::dtype("<f8"));
          },
          py::arg("fit"), "The curve payload words (ARCHIVE_FORMAT.md section 6), float64.")
      .def(
          "moved_nodes",
          [](const PyFitArchive &s, std::size_t i) {
            return s.view(s.get().moved_nodes(s.fit(i)), py::dtype("u1"));
          },
          py::arg("fit"))
      .def(
          "price_only_nodes",
          [](const PyFitArchive &s, std::size_t i) {
            return s.view(s.get().price_only_nodes(s.fit(i)), py::dtype("u1"));
          },
          py::arg("fit"))
      .def(
          "dividends",
          [](const PyFitArchive &s, std::size_t i) {
            return s.view(fit_dividends(s.get(), s.fit(i)),
                          pair_dtype("tau", "amount", offsetof(CashDividend, amount),
                                     sizeof(CashDividend)));
          },
          py::arg("fit"))
      .def(
          "carry_curve",
          [](const PyFitArchive &s, std::size_t i) {
            return s.view(fit_carry_curve(s.get(), s.fit(i)),
                          pair_dtype("strike", "continuous_yield",
                                     offsetof(QuoteSliceCarryPoint, continuous_yield),
                                     sizeof(QuoteSliceCarryPoint)));
          },
          py::arg("fit"))
      // ── owned reconstructions (independent of the archive) ──
      .def(
          "decode_fit",
          [](const PyFitArchive &s, std::size_t i) {
            return unwrap_archive(atx::vol::decode_fit(s.get(), s.fit(i)));
          },
          py::arg("fit"), "The QuoteSliceFit, bit-identical to the one written.")
      .def(
          "decode_curve",
          [](const PyFitArchive &s, std::size_t i) {
            std::unique_ptr<IVolCurve> curve = unwrap_archive(atx::vol::decode_curve(s.get(), s.fit(i)));
            return atxvol::python::CurveHandle{std::shared_ptr<const IVolCurve>(std::move(curve))};
          },
          py::arg("fit"))
      .def(
          "verify_section",
          [](const PyFitArchive &s, std::size_t table_index) {
            if (table_index >= s.get().sections().size()) {
              throw py::index_error("section table index out of range");
            }
            unwrap_archive(s.get().verify_section(table_index));
            return true;
          },
          py::arg("table_index"), "Recompute one stored section's checksum (raises on damage).")
      .def("verify_file_checksum", [](const PyFitArchive &s) {
        unwrap_archive(s.get().verify_file_checksum());
        return true;
      });
}

// ── Writer and series ───────────────────────────────────────────────────────────────

void bind_writer(py::module_ &m) {
  py::class_<FitArchiveWriter>(m, "FitArchiveWriter",
                               "Builds an ATXVFA archive from fits (inputs are copied on add).")
      .def(py::init([](std::int64_t created_unix_ns, std::uint64_t archive_id,
                       std::uint64_t segment_sequence) {
             return FitArchiveWriter{FitArchiveFileInfo{created_unix_ns, archive_id, segment_sequence}};
           }),
           py::arg("created_unix_ns") = 0, py::arg("archive_id") = 0,
           py::arg("segment_sequence") = 0)
      .def(
          "add_fit",
          [](FitArchiveWriter &w, const std::string &underlier, std::int64_t snapshot_ns,
             std::int32_t expiry_day, const QuoteSliceFit &fit, std::uint32_t sub) {
            unwrap_archive(w.add_fit(FitArchiveFitInput{underlier, snapshot_ns, expiry_day, sub, &fit}));
          },
          py::arg("underlier"), py::arg("snapshot_ns"), py::arg("expiry_day"), py::arg("fit"),
          py::arg("sub") = 0, "Add one standalone expiry fit; FitArchiveError on refusal.")
      .def(
          "set_provenance",
          [](FitArchiveWriter &w, const py::bytes &data) { w.set_provenance(bytes_view(data)); },
          py::arg("data"))
      .def("record_timings", &FitArchiveWriter::record_timings, py::arg("on"))
      .def_property_readonly("surface_count", &FitArchiveWriter::surface_count)
      .def_property_readonly("fit_count", &FitArchiveWriter::fit_count)
      .def("finish",
           [](const FitArchiveWriter &w) {
             const std::vector<std::byte> image = unwrap_archive(w.finish());
             return to_bytes(image);
           })
      .def(
          "write_file",
          [](const FitArchiveWriter &w, const std::string &path) {
            unwrap_archive(w.write_file(path));
          },
          py::arg("path"), "finish() + an atomic publish onto `path`.");

  m.def(
      "append_fit_archive_segment",
      [](const std::string &directory, const FitArchiveWriter &writer, std::int64_t created_unix_ns,
         std::uint64_t archive_id) {
        FitArchiveSeriesOptions options;
        options.created_unix_ns = created_unix_ns;
        options.archive_id = archive_id;
        return unwrap_archive(atx::vol::append_fit_archive_segment(directory, writer, options));
      },
      py::arg("directory"), py::arg("writer"), py::arg("created_unix_ns") = 0,
      py::arg("archive_id") = 0, "Publish the writer's records as the next series segment.");
  m.def(
      "compact_fit_archive_series",
      [](const std::string &directory, std::size_t first, std::optional<std::size_t> count) {
        const FitArchiveCompaction c = unwrap_archive(atx::vol::compact_fit_archive_series(
            directory, first, count.value_or(std::numeric_limits<std::size_t>::max())));
        py::dict d;
        d["sequence"] = c.sequence;
        d["merged"] = c.merged;
        d["removed"] = c.removed;
        d["kept"] = c.kept;
        return d;
      },
      py::arg("directory"), py::arg("first") = 0, py::arg("count") = py::none());
}

struct PySeries {
  std::shared_ptr<const FitArchiveSeries> series;
};

py::object series_hit(const PySeries &s, const FitArchive *archive, const void *record,
                      const void *base, std::size_t stride) {
  if (record == nullptr) return py::none();
  for (std::size_t i = 0; i < s.series->segment_count(); ++i) {
    if (&s.series->segment(i) == archive) {
      return py::make_tuple(i, index_or_none(record, base, stride));
    }
  }
  return py::none();
}

void bind_series(py::module_ &m) {
  py::class_<PySeries>(m, "FitArchiveSeries", "A directory of immutable archive segments.")
      .def_static(
          "open",
          [](const std::string &directory, FitArchiveOpenMode mode, FitArchiveVerify verify) {
            FitArchiveSeries s =
                unwrap_archive(FitArchiveSeries::open(directory, FitArchiveOpenOptions{mode, verify}));
            return PySeries{std::make_shared<const FitArchiveSeries>(std::move(s))};
          },
          py::arg("directory"), py::arg("mode") = FitArchiveOpenMode::Mapped,
          py::arg("verify") = FitArchiveVerify::Full)
      .def_property_readonly("segment_count",
                             [](const PySeries &s) { return s.series->segment_count(); })
      .def("manifest",
           [](const PySeries &s) {
             py::list out;
             for (const FitArchiveSeriesSegment &g : s.series->manifest()) {
               py::dict d;
               d["sequence"] = g.sequence;
               d["file_size"] = g.file_size;
               d["min_snapshot_ns"] = g.min_snapshot_ns;
               d["max_snapshot_ns"] = g.max_snapshot_ns;
               d["surface_records"] = g.surface_records;
               d["fit_records"] = g.fit_records;
               d["expiry_records"] = g.expiry_records;
               d["ladder_records"] = g.ladder_records;
               d["markov_records"] = g.markov_records;
               std::size_t n = 0;
               while (n < g.file_name.size() && g.file_name[n] != '\0') ++n;
               d["file_name"] = std::string{g.file_name.data(), n};
               out.append(d);
             }
             return out;
           })
      .def(
          "segment",
          [](const PySeries &s, std::size_t i) {
            if (i >= s.series->segment_count()) throw py::index_error("segment index out of range");
            // Aliasing share: the segment's FitArchive lives inside the series object.
            return PyFitArchive{std::shared_ptr<const FitArchive>(s.series, &s.series->segment(i))};
          },
          py::arg("index"))
      .def(
          "find_fit",
          [](const PySeries &s, const std::string &underlier, std::int64_t snapshot_ns,
             std::int32_t expiry_day, std::uint32_t sub) {
            const auto hit = s.series->find_fit(underlier, snapshot_ns, expiry_day, sub);
            if (!hit) return py::object(py::none());
            return series_hit(s, hit.archive, hit.record, hit.archive->fits().data(),
                              sizeof(FitArchiveFitRecord));
          },
          py::arg("underlier"), py::arg("snapshot_ns"), py::arg("expiry_day"), py::arg("sub") = 0,
          "(segment index, fit index) of the key, or None.")
      .def(
          "find_surface",
          [](const PySeries &s, const std::string &underlier, std::int64_t snapshot_ns) {
            const auto hit = s.series->find_surface(underlier, snapshot_ns);
            if (!hit) return py::object(py::none());
            return series_hit(s, hit.archive, hit.record, hit.archive->surfaces().data(),
                              sizeof(FitArchiveSurfaceRecord));
          },
          py::arg("underlier"), py::arg("snapshot_ns"));
}

} // namespace

void bind_fit_archive(py::module_ &m) {
  bind_enums(m);
  bind_exception(m);
  bind_records(m);
  bind_archive(m);
  bind_writer(m);
  bind_series(m);
  m.def(
      "fit_archive_crc32c",
      [](const py::bytes &data) { return fit_archive_crc32c(bytes_view(data)); },
      py::arg("data"));
}
