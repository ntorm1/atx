// persist_api_check — compiles every public header of the wave-3 persistence layer in
// one translation unit, so the FROZEN API contract of lane P (persist_format,
// persist_file, state_archive, surface_timeline, risk_run_archive) is type-checked on
// the scaffold before any lane implements it, and an ODR / include-order drift
// between the headers fails the build here rather than in a lane. Deliberately no
// definitions: this TU must stay empty (risk_api_check.cpp is the pattern).

#include "atx/vol/api/persist/persist_file.hpp"
#include "atx/vol/api/persist/persist_format.hpp"
#include "atx/vol/api/persist/risk_run_archive.hpp"
#include "atx/vol/api/persist/state_archive.hpp"
#include "atx/vol/api/persist/surface_timeline.hpp"

namespace atx::vol {
namespace {
// The fingerprint is a constant expression of the spec tables (persist D5).
constexpr PersistFieldSpec kCheckFields[] = {{"a", 0, 8}, {"b", 8, 4}, {"c", 12, 4}};
constexpr PersistSectionSpec kCheckSchema[] = {
    {1, 1, PersistDType::Pod, 16, "check", kCheckFields, true}};
static_assert(persist_fields_tile(kCheckFields, 16));
static_assert(!persist_fields_tile(std::span<const PersistFieldSpec>{kCheckFields}.first(2), 16));
static_assert(persist_fingerprint(persist_salt("ATXVCK01"), kCheckSchema) != 0);
static_assert(persist_fingerprint(persist_salt("ATXVCK01"), kCheckSchema) !=
              persist_fingerprint(persist_salt("ATXVCK02"), kCheckSchema));
static_assert(persist_dtype_of<double>() == PersistDType::F64);
static_assert(persist_dtype_of<Side>() == PersistDType::U8);
static_assert(persist_dtype_of<PersistManifestEntry>() == PersistDType::Pod);
} // namespace
} // namespace atx::vol
