#include "stages.hpp"

#include <array>
#include <string>

#include "atx/core/error.hpp"
#include "atx/engine/data/orats_history.hpp"
#include "artifacts.hpp"
#include "config.hpp"

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
    lc.created_at_nanos = 0;  // FIXED for determinism: stamped into seg headers;
                               // a clock value would break byte-identical re-runs.

    // 4. Run the loader.
    auto st = atx::engine::data::load_orats_history(lc);
    if (!st) {
        return atx::core::Err(st.error());
    }

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
              {"dates_written", std::to_string(st->dates_written)},
              {"distinct",      std::to_string(st->distinct_securities)} };
    return atx::core::Ok(std::move(r));
}

} // namespace atx::impl
