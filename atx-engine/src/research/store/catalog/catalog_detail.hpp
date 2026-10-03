#pragma once

// Private plumbing of the catalog library (P9 SQL2): strict JSON reading, the typed-field
// helpers the ingesters share, the per-family ingest entry points and the pin extractors.
// Included only by the catalog library's own .cpp files (nlohmann is a PRIVATE dependency;
// no public header names a JSON type).

#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "atx/core/db/sqlite.hpp"
#include "atx/core/error.hpp"
#include "atx/core/types.hpp"
#include "atx/engine/research/store/catalog/ingest.hpp"
#include "atx/engine/research/store/catalog/rows_records.hpp"

namespace atx::engine::research::store::catalog::detail {

using OJson = nlohmann::ordered_json;

// ---------------------------------------------------------------------------------------------
//  JSON text (json_text.cpp)
// ---------------------------------------------------------------------------------------------

// Parse `text` as JSON (nlohmann ordered_json: key order kept), or nullopt when the catalog
// cannot hold it losslessly ("unparsed", sql-design 3.9): a parse error (NaN / Infinity
// tokens, invalid UTF-8, trailing bytes), an integer literal outside i64 / u64, or a number
// literal that does not convert to a finite double.
[[nodiscard]] std::optional<OJson> parse_strict(std::string_view text);

// nlohmann's compact dump (insertion order, UTF-8 kept): the catalog's JSON column text.
[[nodiscard]] std::string compact(const OJson &value);

// Python's json.dumps(value, separators=(",", ":"), ensure_ascii=True, allow_nan=False
// [, sort_keys=True]) of a parsed value: floats as Python's repr. Err(InvalidArgument) for a
// non-finite float.
[[nodiscard]] core::Result<std::string> py_dumps(const OJson &value, bool sort_keys);

// Python's repr of a finite double ("0.1", "1e+16", "1000000000000000.0", "-0.0").
[[nodiscard]] std::string py_float_repr(f64 value);

[[nodiscard]] bool is_sha256(std::string_view text) noexcept;

// RFC 6901: "/" + each token with '~' -> "~0" and '/' -> "~1".
[[nodiscard]] std::string json_pointer(std::initializer_list<std::string_view> tokens);

// The document's top-level "schema" string, if it is an object holding one.
[[nodiscard]] std::optional<std::string> schema_of(const OJson &doc);

// Value fits: a string; a 64-hex lower-case string; a relpath string; an integer in i64; a
// finite non-negative-zero float (a JSON float, never an integer); a boolean.
[[nodiscard]] std::optional<std::string> fit_text(const OJson &v);
[[nodiscard]] std::optional<std::string> fit_sha(const OJson &v);
[[nodiscard]] std::optional<std::string> fit_relpath(const OJson &v);
[[nodiscard]] std::optional<i64> fit_int(const OJson &v);
[[nodiscard]] std::optional<f64> fit_real(const OJson &v);
[[nodiscard]] std::optional<bool> fit_bool(const OJson &v);

// The member `key` of an object (nullptr when absent or `obj` is not an object).
[[nodiscard]] const OJson *member(const OJson &obj, std::string_view key);
// A nested member by path ({"ledger", "head"}).
[[nodiscard]] const OJson *member_at(const OJson &obj,
                                     std::initializer_list<std::string_view> keys);

// Typed columns of a typed-render document (run, cycle_binding, stage_receipt). Every top-level
// key a column takes is claimed; a key whose value does not fit its column stays unclaimed and
// lands in extra(); a required column whose key is absent or does not fit marks the document
// as not of the class's shape (shape_ok() false: the file is catalogued as `unparsed`).
class DocFields {
public:
  explicit DocFields(const OJson &doc) : doc_{doc} {}

  // Nullable columns: absent or null -> nullopt (claimed); fits -> value (claimed); else
  // nullopt and the key stays for extra().
  [[nodiscard]] std::optional<std::string> text(std::string_view key);
  [[nodiscard]] std::optional<std::string> sha(std::string_view key);
  [[nodiscard]] std::optional<std::string> relpath(std::string_view key);
  [[nodiscard]] std::optional<i64> integer(std::string_view key);
  [[nodiscard]] std::optional<f64> real(std::string_view key);
  // Any present, non-null value as compact JSON (claimed); null / absent -> nullopt (claimed).
  [[nodiscard]] std::optional<std::string> json(std::string_view key);

  // Required columns: the value, or the zero value with shape_ok() turned false.
  [[nodiscard]] std::string req_text(std::string_view key);
  [[nodiscard]] std::string req_relpath(std::string_view key);
  [[nodiscard]] i64 req_integer(std::string_view key);
  [[nodiscard]] f64 req_real(std::string_view key);
  [[nodiscard]] std::string req_json(std::string_view key);

  // Claim a key handled by the caller (child rows).
  void claim(std::string_view key);

  [[nodiscard]] bool shape_ok() const noexcept { return shape_ok_; }
  // The document's top-level keys in order, as a compact JSON array.
  [[nodiscard]] std::string key_order() const;
  // The unclaimed keys in document order as a compact JSON object, or nullopt when none.
  [[nodiscard]] std::optional<std::string> extra() const;

private:
  const OJson &doc_;
  std::vector<std::string> claimed_;
  bool shape_ok_{true};
};

// ---------------------------------------------------------------------------------------------
//  Families (ingest_*.cpp). Each fills `out` (writes, pins, named files / dirs) for one parsed
//  document; out.unparsed = true when the document is not of the family's shape.
// ---------------------------------------------------------------------------------------------

struct FamilyInput {
  const IngestContext &ctx;
  const FileView &file;
  const OJson &doc;
};

void ingest_run(const FamilyInput &in, IngestResult &out);
void ingest_run_start(const FamilyInput &in, IngestResult &out);
void ingest_stage_receipt(const FamilyInput &in, IngestResult &out);
void ingest_cycle_binding(const FamilyInput &in, IngestResult &out);
void ingest_cycle_verdict(const FamilyInput &in, IngestResult &out);
void ingest_wave_result(const FamilyInput &in, IngestResult &out);
void ingest_spec_doc(const FamilyInput &in, IngestResult &out);
void ingest_candidate(const FamilyInput &in, IngestResult &out);
void ingest_field_manifest(const FamilyInput &in, IngestResult &out);
void ingest_build_receipt(const FamilyInput &in, IngestResult &out);
// The ledger is JSON lines, not one document.
[[nodiscard]] core::Status ingest_trial_ledger(const IngestContext &ctx, const FileView &file,
                                               IngestResult &out);

// A write that replaces every row of `table` whose `column` equals `value` (child rows of a
// re-ingested file). `table` / `column` are fixed identifiers of this library.
[[nodiscard]] core::Status delete_where(core::db::Database &db, std::string_view table,
                                        std::string_view column, std::string_view value);

// ---------------------------------------------------------------------------------------------
//  Pins (pins.cpp). Each appends the holder's pins; target paths are path_keys inside the
//  root, or nullopt (unresolved).
// ---------------------------------------------------------------------------------------------

struct PinScope {
  const IngestContext &ctx;
  const std::string &holder; // root-relative holder path
};

void spec_pins(const PinScope &s, const OJson &doc, std::vector<PinRow> &out);
void template_pins(const PinScope &s, const OJson &doc, std::vector<PinRow> &out);
void wave_manifest_pins(const PinScope &s, const OJson &doc, std::vector<PinRow> &out);
void receipt_pins(const PinScope &s, const OJson &doc, std::vector<PinRow> &out);
void binding_pins(const PinScope &s, const OJson &doc, std::vector<PinRow> &out);
void verdict_pins(const PinScope &s, const OJson &doc, std::vector<PinRow> &out);
void wave_result_pins(const PinScope &s, const OJson &doc, std::vector<PinRow> &out);
void fields_manifest_pins(const PinScope &s, const OJson &doc, std::vector<PinRow> &out);

// Append to out.pins and out.writes (delete the holder's pins, insert these) and to
// out.named_files the targets a catalog walk follows (follows_pin()).
void emit_pins(const std::string &holder, std::vector<PinRow> pins, IngestResult &out);

} // namespace atx::engine::research::store::catalog::detail
