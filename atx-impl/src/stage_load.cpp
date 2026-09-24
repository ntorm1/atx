#include "stages.hpp"

#include <array>
#include <string>

#include <nlohmann/json.hpp>

#include "atx/core/error.hpp"
#include "atx/engine/data/orats_history.hpp"
#include "artifacts.hpp"
#include "config.hpp"
#include "stage_data_provenance.hpp"

namespace atx::impl {

atx::core::Result<StageResult> run_load(const RunConfig& cfg) {
    // 1. Validate required args.
    if (cfg.zip.empty() || cfg.out.empty()) {
        return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                              "load: --zip and --out required");
    }

    // 2. Convert min-date string to epoch nanos.
    if (cfg.min_date.empty()) {
        return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                              "load: --min-date must be YYYY-MM-DD");
    }
    auto md = atx::engine::data::detail::date_to_nanos(cfg.min_date);
    if (!md.has_value()) {
        return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                              "load: --min-date must be YYYY-MM-DD");
    }

    // 3. Build loader config.
    atx::engine::data::OratsLoadConfig lc;
    lc.zip_path         = cfg.zip;
    lc.out_dir          = cfg.out;
    lc.min_date_nanos   = *md;
    lc.exclude_no_sector = cfg.exclude_no_sector; // Missing-sector screen, not an asset-type proof.
    lc.created_at_nanos = 0;  // FIXED for determinism: stamped into seg headers;
                               // a clock value would break byte-identical re-runs.

    ATX_TRY(auto executable_sha, current_executable_sha256());
    ATX_TRY(auto input, begin_ingestion_provenance(cfg.zip, cfg.out,
                                                  cfg.preparation_manifest));

    // 4. Run the loader. An error leaves an unverified reserved output directory.
    auto st = atx::engine::data::load_orats_history(lc);
    if (!st) {
        return atx::core::Err(st.error());
    }

    const nlohmann::json recipe{
        {"version", "tickerhistory-native-load-v1"},
        {"min_date_nanos", std::to_string(lc.min_date_nanos)},
        {"exclude_no_sector", lc.exclude_no_sector},
        {"created_at_nanos", std::to_string(lc.created_at_nanos)},
        {"positive_security_id_required", true},
        {"duplicate_positive_date_id", "reject-before-sector-filter"},
        {"date_order", "ascending-date-groups"},
        {"executable_sha256", executable_sha.empty() ? "unknown" : executable_sha},
        {"rows_read", st->rows_read}, {"rows_kept", st->rows_kept},
        {"rows_filtered", st->rows_filtered}, {"rows_malformed", st->rows_malformed},
        {"dates_written", st->dates_written}, {"distinct_securities", st->distinct_securities},
        {"historical_availability", "unknown-archive-snapshot"},
        {"instrument_type_eligibility", "unknown"}};
    ATX_TRY(auto receipt_sha, finish_ingestion_provenance(
        input, cfg.zip, cfg.out, cfg.preparation_manifest, recipe.dump(),
        st->dates_written, st->rows_read));

    // 5. Digest: fold the 6 stats fields in fixed order.
    const std::array<atx::i64, 6> d{
        st->rows_read,
        st->rows_kept,
        st->rows_filtered,
        st->rows_malformed,
        st->dates_written,
        st->distinct_securities
    };
    atx::u64 digest = fnv1a64(d.data(), d.size() * sizeof(atx::i64));

    // 6. Return result with printed counts.
    StageResult r;
    r.digest = digest;
    r.kvs = { {"rows_read",    std::to_string(st->rows_read)},
              {"rows_kept",    std::to_string(st->rows_kept)},
              {"rows_filtered", std::to_string(st->rows_filtered)},
              {"rows_malformed", std::to_string(st->rows_malformed)},
              {"dates_written", std::to_string(st->dates_written)},
              {"distinct",      std::to_string(st->distinct_securities)},
              {"ingestion_manifest_sha256", receipt_sha},
              {"input_sha256", input.input.sha256} };
    return atx::core::Ok(std::move(r));
}

} // namespace atx::impl
