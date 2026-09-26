#include "stages.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <ctime>
#include <limits>
#include <filesystem>
#include <fstream>
#include <numeric>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "artifacts.hpp"          // fnv1a64, to_hex16
#include "asof_field.hpp"         // R21-3 --asof-field CSV loader + strict as-of join
#include "panel_artifact.hpp"
#include "panel_pipeline.hpp"
#include "stage_data_provenance.hpp"

#include "atx/core/error.hpp"
#include "atx/core/sha256.hpp"                 // sha256_hex (membership.bin content binding)
#include "atx/engine/alpha/augment.hpp"        // with_alpha101_fields (opt-in panel augmentation)
#include "atx/engine/alpha/segment_panel.hpp"  // alpha::TimeWindow
#include "atx/engine/data/history_panel.hpp"   // build_history_panel, HistoryDataConfig
#include "atx/engine/data/panel_store.hpp"
#include "atx/engine/data/orats_history.hpp"   // detail::date_to_nanos
#include "atx/engine/data/panel_digest.hpp"
#include "atx/engine/data/point_in_time_universe.hpp" // decode_membership_bin, PitMembershipImage
#include "atx/engine/data/universe.hpp"        // UniverseConfig
#include "atx/tsdb/mapping.hpp"

namespace atx::impl {

namespace {

// A membership image is post-run, cold-path input; the cap bounds an unbounded read
// of an operator-supplied path rather than expressing a real size expectation.
inline constexpr std::streamoff kMaxMembershipBytes = 512LL * 1024LL * 1024LL;

// §4.1 band text is `<units>.<hundredths>` of the fraction carried as basis points:
// "0.00" -> 0 bp, "0.10" -> 1000 bp — the exact spelling stage_equity_universe emits.
// The literal is parsed digit-wise, never through a double, so the flag and the codec
// agree with no rounding step between them.
atx::core::Result<atx::u32> band_bp_from_text(std::string_view text) {
    using EC = atx::core::ErrorCode;
    const auto dot = text.find('.');
    if (dot == std::string_view::npos || dot == 0 || text.size() - dot != 3) {
        return atx::core::Err(EC::InvalidArgument,
            "panel: --universe-cut band must be <units>.<hundredths>, e.g. 0.00");
    }
    const std::string_view units_text = text.substr(0, dot);
    const std::string_view frac_text = text.substr(dot + 1);
    atx::u32 units = 0;
    atx::u32 frac = 0;
    const auto u = std::from_chars(units_text.data(), units_text.data() + units_text.size(), units);
    const auto f = std::from_chars(frac_text.data(), frac_text.data() + frac_text.size(), frac);
    if (u.ec != std::errc{} || u.ptr != units_text.data() + units_text.size() ||
        f.ec != std::errc{} || f.ptr != frac_text.data() + frac_text.size() || units > 100U) {
        return atx::core::Err(EC::InvalidArgument,
            "panel: --universe-cut band is not a two-decimal literal: " + std::string(text));
    }
    return atx::core::Ok(static_cast<atx::u32>((units * 100U + frac) * 100U));
}

// Cut index c = top_n_index * band_count + band_index (§3.1) — the layout
// decode_membership_bin fills and stage_equity_universe's cut_top_n/cut_band_bp read.
atx::core::Result<atx::usize>
resolve_cut_index(const atx::engine::data::PitMembershipImage &image, atx::u32 top_n,
                  atx::u32 band_bp) {
    const auto ti = std::find(image.top_n.begin(), image.top_n.end(), top_n);
    const auto bi = std::find(image.band_bp.begin(), image.band_bp.end(), band_bp);
    if (ti == image.top_n.end() || bi == image.band_bp.end()) {
        return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
            "panel: --universe-cut names a (top_n, band) the membership image does not carry");
    }
    const auto t_index = static_cast<atx::usize>(ti - image.top_n.begin());
    const auto b_index = static_cast<atx::usize>(bi - image.band_bp.begin());
    return atx::core::Ok(t_index * image.band_bp.size() + b_index);
}

atx::core::Result<std::string> read_membership_bytes(const std::string &path) {
    using EC = atx::core::ErrorCode;
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in.is_open()) {
        return atx::core::Err(EC::IoError, "panel: cannot open membership image: " + path);
    }
    const std::streamoff size = in.tellg();
    if (size < 0 || size > kMaxMembershipBytes) {
        return atx::core::Err(EC::InvalidArgument,
            "panel: membership image is missing or implausibly large: " + path);
    }
    std::string bytes(static_cast<std::size_t>(size), '\0');
    in.seekg(0, std::ios::beg);
    if (size > 0 && !in.read(bytes.data(), static_cast<std::streamsize>(size))) {
        return atx::core::Err(EC::IoError, "panel: cannot read membership image: " + path);
    }
    return atx::core::Ok(std::move(bytes));
}

// Everything the recipe must declare about the restriction, plus the resolved list.
struct MembershipRestriction {
    bool active = false;
    std::string membership_sha256;
    std::string cut;        // "<top_n>:<band>", exactly as supplied
    std::string eval_start; // ISO date, exactly as supplied
    std::vector<atx::i64> allow_ids;
    std::optional<atx::engine::data::PitMembershipImage> store_image;
    atx::usize store_cut{};
};

// Decode membership.bin, pick the named cut, and union its members over the panel's
// window. Inactive (the flags absent) returns a default restriction and opens no
// file, so the no-flag path does no extra work and adds no recipe key.
atx::core::Result<MembershipRestriction>
resolve_membership_restriction(const RunConfig &cfg, atx::i64 end_exclusive_nanos) {
    using EC = atx::core::ErrorCode;
    MembershipRestriction out;
    ATX_TRY_VOID(validate_membership_flags(cfg));
    if (cfg.panel_universe_membership.empty()) return atx::core::Ok(std::move(out));

    const std::string_view cut_text{cfg.panel_universe_cut};
    const auto colon = cut_text.find(':');
    if (colon == std::string_view::npos || colon == 0) {
        return atx::core::Err(EC::InvalidArgument,
            "panel: --universe-cut must be <top_n>:<band>, e.g. 3000:0.00");
    }
    const std::string_view top_text = cut_text.substr(0, colon);
    atx::u32 top_n = 0;
    const auto p = std::from_chars(top_text.data(), top_text.data() + top_text.size(), top_n);
    if (p.ec != std::errc{} || p.ptr != top_text.data() + top_text.size() || top_n == 0) {
        return atx::core::Err(EC::InvalidArgument,
            "panel: --universe-cut top_n must be a positive integer: " + cfg.panel_universe_cut);
    }
    ATX_TRY(auto band_bp, band_bp_from_text(cut_text.substr(colon + 1)));

    const auto eval_ns =
        atx::engine::data::detail::date_to_nanos(cfg.panel_universe_eval_start);
    if (!eval_ns.has_value()) {
        return atx::core::Err(EC::InvalidArgument,
            "panel: --universe-eval-start is not a valid date: " + cfg.panel_universe_eval_start);
    }
    ATX_TRY(auto bytes, read_membership_bytes(cfg.panel_universe_membership));
    // Hash the very bytes that were decoded, not a second read of the path: the
    // recipe's binding is then to the image this panel actually used.
    ATX_TRY(auto sha, atx::core::sha256_hex(std::string_view{bytes}));
    ATX_TRY(auto image, atx::engine::data::decode_membership_bin(bytes));
    ATX_TRY(auto cut, resolve_cut_index(image, top_n, band_bp));
    ATX_TRY(out.allow_ids, pit_membership_allow_ids(image, cut, *eval_ns, end_exclusive_nanos));
    out.active = true;
    out.membership_sha256 = std::move(sha);
    out.cut = cfg.panel_universe_cut;
    out.eval_start = cfg.panel_universe_eval_start;
    if (cfg.panel_storage_rule == "mmap-f32-v2") {
        out.store_cut = cut;
        out.store_image = std::move(image);
    }
    return atx::core::Ok(std::move(out));
}

// R21-3: one --asof-field entry, loaded and parsed BEFORE the (expensive) history
// build so a bad CSV fails fast; counts are filled once the column is joined.
struct AsofFieldInput {
    std::string name;
    std::string path;
    std::string sha256; // of the exact bytes parsed
    AsofTable table;
    atx::usize rows_ignored_unknown_id = 0;
};

atx::core::Result<std::vector<AsofFieldInput>> load_asof_fields(const RunConfig &cfg) {
    using EC = atx::core::ErrorCode;
    std::vector<AsofFieldInput> out;
    if (cfg.panel_asof_max_stale_days < 0) {
        return atx::core::Err(EC::InvalidArgument, "panel: --asof-max-stale-days must be >= 0");
    }
    for (const auto &[name, path] : cfg.panel_asof_fields) {
        if (!is_dsl_identifier(name)) {
            return atx::core::Err(EC::InvalidArgument,
                "panel: --asof-field name is not a DSL identifier ([A-Za-z_][A-Za-z0-9_]*): '" +
                    name + "'");
        }
        for (const auto &prior : out) {
            if (prior.name == name) {
                return atx::core::Err(EC::InvalidArgument,
                    "panel: --asof-field name given twice: '" + name + "'");
            }
        }
        AsofFieldInput in;
        in.name = name;
        in.path = path;
        ATX_TRY(auto bytes, read_asof_bytes(path));
        ATX_TRY(in.sha256, atx::core::sha256_hex(std::string_view{bytes}));
        auto parsed = parse_asof_csv(bytes);
        if (!parsed) {
            return atx::core::Err(parsed.error().code(),
                "panel: --asof-field " + name + " (" + path + "): " + parsed.error().message());
        }
        in.table = std::move(*parsed);
        out.push_back(std::move(in));
    }
    return atx::core::Ok(std::move(out));
}

// Append every --asof-field column (command-line order) to `hp.panel` and refresh
// hp.digest, mirroring the alpha101 augmentation: rebuild the owned field list plus
// the universe mask, then Panel::create. A name already on the panel is an error.
atx::core::Status append_asof_fields(atx::engine::data::HistoryPanel &hp,
                                     std::vector<AsofFieldInput> &fields,
                                     atx::i64 max_stale_days) {
    using EC = atx::core::ErrorCode;
    if (fields.empty()) return atx::core::Ok();
    const auto &base = hp.panel;
    const atx::usize D = base.dates();
    const atx::usize I = base.instruments();
    std::vector<std::string> names;
    std::vector<std::vector<atx::f64>> data;
    names.reserve(base.num_fields() + fields.size());
    data.reserve(base.num_fields() + fields.size());
    for (atx::usize f = 0; f < base.num_fields(); ++f) {
        names.emplace_back(base.field_name(f));
        const auto col = base.field_all(static_cast<atx::engine::alpha::FieldId>(f));
        data.emplace_back(col.begin(), col.end());
    }
    std::vector<std::uint8_t> universe(D * I, std::uint8_t{1});
    for (atx::usize d = 0; d < D; ++d) {
        for (atx::usize n = 0; n < I; ++n) {
            universe[d * I + n] = base.in_universe(d, n) ? std::uint8_t{1} : std::uint8_t{0};
        }
    }
    for (auto &field : fields) {
        if (std::find(names.begin(), names.end(), field.name) != names.end()) {
            return atx::core::Err(EC::InvalidArgument,
                "panel: --asof-field name collides with an existing panel field: '" +
                    field.name + "'");
        }
        ATX_TRY(auto column, build_asof_column(field.table, hp.session_keys, hp.instrument_ids,
                                               max_stale_days));
        field.rows_ignored_unknown_id = column.rows_ignored_unknown_id;
        names.push_back(field.name);
        data.push_back(std::move(column.values));
    }
    ATX_TRY(hp.panel, atx::engine::alpha::Panel::create(D, I, std::move(names), std::move(data),
                                                        std::move(universe)));
    hp.digest = atx::engine::data::digest_panel(hp.panel);
    return atx::core::Ok();
}

std::string panel_recipe(const atx::engine::data::HistoryDataConfig &cfg,
                         const std::vector<atx::u16> &adv_windows,
                         const PanelSourceProvenance &sources,
                         const std::string &executable_sha,
                         const MembershipRestriction &membership,
                         const std::vector<AsofFieldInput> &asof_fields,
                         atx::i64 asof_max_stale_days,
                         atx::engine::alpha::VwapRule vwap_rule) {
    // nlohmann's ordered object keys and exact dump bytes are our versioned
    // serialization contract; this is not a claim of RFC 8785 canonical JSON.
    nlohmann::json recipe{
        {"version", "tickerhistory-panel-v2-identified"},
        {"producer_executable_sha256", executable_sha.empty() ? "unknown" : executable_sha},
        {"start_inclusive_nanos", std::to_string(cfg.window.start_nanos)},
        {"end_exclusive_nanos", std::to_string(cfg.window.end_nanos)},
        {"universe", {{"adv_window_bars", cfg.universe.adv_window},
                       {"min_adv_usd", cfg.universe.min_adv_usd},
                       {"min_mktcap_usd", cfg.universe.min_mktcap_usd},
                       {"top_n_by_adv", cfg.universe.top_n_by_adv},
                       {"min_raw_price_exclusive", cfg.universe.min_price},
                       {"require_sector", cfg.universe.require_sector},
                       {"adv_basis", "raw_close*raw_volume"},
                       {"adv_missing", "full-trailing-window-any-NaN-invalid"},
                       {"top_n_tie_break", "original-instrument-index"},
                       {"current_session_data_included", true}}},
        {"compact_to_universe", cfg.compact_to_universe},
        {"compaction", "keep-original-order-if-ever-in-universe-in-selected-window"},
        {"augmentation", {{"enabled", !adv_windows.empty()}, {"adv_windows", adv_windows},
                           {"dollar_volume_basis", "raw_close*raw_volume"},
                           {"vwap_rule", atx::engine::alpha::vwap_rule_name(vwap_rule)},
                           {"vwap_kind", vwap_rule == atx::engine::alpha::VwapRule::RawDailyCloseV2
                               ? "daily-close-price-proxy-not-intraday-vwap"
                               : "adjusted-typical-price-legacy-proxy"},
                           {"vwap_basis", vwap_rule == atx::engine::alpha::VwapRule::RawDailyCloseV2
                               ? "raw" : "adjusted_level"}}},
        {"research_ohlc", "raw-OHLC*cumulReturnFactor-pointwise"},
        {"invalid_price_factor_or_product", "NaN-gap-next-valid-cell-independent"},
        {"raw_close", "unadjusted-as-traded"},
        {"volume", "raw-reported-volume-no-total-return-factor-rescaling"},
        {"market_cap", "raw-close*reported-shares-no-share-revision-validation"},
        {"other_fields", "sector-and-earnings-options-columns-pass-through"},
        {"historical_availability", "unknown-archive-snapshot"},
        {"historical_vintages_verified", false},
        {"session_keys", "session-label-not-availability-or-execution-time"},
        {"instrument_type_eligibility", "unknown-includes-possible-funds-and-other-types"},
        {"todayTicker", "not-used-for-identity-or-universe"},
        {"ingestion_content_binding_verified", sources.ingestion_verified},
        {"preparation_content_binding_verified", sources.preparation_verified},
        {"original_source_binding", sources.preparation_verified
             ? "declared-by-bound-preparation-manifest-not-authenticated" : "unknown"}};
    // ADDITIVE and strictly opt-in: without the membership flags not one key is
    // added, so the recipe bytes the equity-baseline/equity-ic pins read stay
    // byte-identical. Those pins are strict_json + .at() on named keys — duplicate
    // keys and deep nesting are the only rejections — so extra keys are tolerated.
    if (membership.active) {
        recipe["universe_membership_sha256"] = membership.membership_sha256;
        recipe["universe_cut"] = membership.cut;
        recipe["universe_eval_start"] = membership.eval_start;
        recipe["allow_list_size"] = membership.allow_ids.size();
        recipe["membership_rule"] = "year-union-plus-last-prior-rebalance;not-as-of";
    }
    // R21-3, same additive contract: the key exists only when --asof-field was given.
    // Join rule: a session labelled date(s) sees the latest row with
    // available_at < date(s) STRICTLY (visible the first session AFTER availability),
    // NaN before the first row, NaN once older than max_stale_days (0 = no cap).
    if (!asof_fields.empty()) {
        nlohmann::json entries = nlohmann::json::array();
        for (const auto &field : asof_fields) {
            entries.push_back({{"name", field.name},
                               {"sha256", field.sha256},
                               {"rows", field.table.rows.size()},
                               {"rows_ignored_unknown_id", field.rows_ignored_unknown_id},
                               {"join_rule", "available_at<session_date"},
                               {"max_stale_days", asof_max_stale_days}});
        }
        recipe["asof_fields"] = std::move(entries);
    }
    return recipe.dump();
}

atx::core::Result<std::vector<SourceFileDigest>> snapshot_store_sources(
    const std::string& directory, atx::i64 begin, atx::i64 end) {
    namespace fs = std::filesystem;
    using atx::core::Err; using atx::core::ErrorCode;
    std::vector<fs::path> paths; std::error_code ec;
    for (const auto& e : fs::directory_iterator(directory, ec)) {
        if (e.path().extension() != ".seg") continue;
        if (paths.size() == 10000 || e.path().string().size() > 4096)
            return Err(ErrorCode::InvalidArgument, "panel store: source metadata limit");
        paths.push_back(e.path());
    }
    if (ec) return Err(ErrorCode::IoError, "panel store: source enumeration failed");
    std::sort(paths.begin(), paths.end());
    std::vector<SourceFileDigest> result;
    for (const auto& path : paths) {
        bool selected = false;
        {
            ATX_TRY(auto reader, atx::tsdb::SegmentReader::attach(path.string(), 256ULL * 1024 * 1024));
            for (auto date : reader.times()) if (date >= begin && date < end) { selected = true; break; }
        }
        if (!selected) continue;
        ATX_TRY(auto captured, atx::tsdb::Mapping::map_file_ro(path.string(), 0, 256ULL * 1024 * 1024));
        ATX_TRY(auto sha, atx::core::sha256_hex(std::as_bytes(std::span{captured.base(), captured.size()})));
        result.push_back({path.filename().string(), std::move(sha), captured.size()});
    }
    if (result.empty()) return Err(ErrorCode::InvalidArgument, "panel store: no selected source");
    return atx::core::Ok(std::move(result));
}

atx::core::Result<StageResult> run_panel_store_v2(const RunConfig& cfg) {
    using namespace atx::engine;
    using atx::core::Err; using atx::core::ErrorCode; using atx::core::Ok;
    if (cfg.segs.empty() || cfg.panel_out.empty() || cfg.start.empty() || cfg.end.empty() ||
        cfg.incremental_panel || cfg.compact_universe || !cfg.panel_asof_fields.empty())
        return Err(ErrorCode::InvalidArgument,
            "panel store V2 requires explicit dates/fresh output/no compaction; custom asof fields require basis metadata");
    const auto start = data::detail::date_to_nanos(cfg.start), end = data::detail::date_to_nanos(cfg.end);
    if (!start || !end || *start <= 0 || *start >= *end || *end > data::kPitSessionKeyEndExclusive)
        return Err(ErrorCode::InvalidArgument, "panel store V2 requires a sealed pre-2020 window");
    ATX_TRY(auto membership, resolve_membership_restriction(cfg, *end));
    if (!membership.store_image || membership.store_image->rule != data::PitUniverseRule::CommonStockV2)
        return Err(ErrorCode::InvalidArgument, "panel store V2 requires dated common-stock-v2 membership");
    const auto& image = *membership.store_image;
    data::PanelStoreConfig store_cfg;
    for (const auto& r : image.rebalances) {
        if (r.rank_session_key <= 0 || r.rank_session_key >= r.effective_session_key ||
            r.effective_session_key >= data::kPitSessionKeyEndExclusive || membership.store_cut >= r.cuts.size())
            return Err(ErrorCode::InvalidArgument, "panel store: invalid/sealed membership clock/cut");
        const auto& ids = r.cuts[membership.store_cut].security_ids;
        if (ids.size() > 4'000'000 - store_cfg.instrument_ids.size())
            return Err(ErrorCode::InvalidArgument, "panel store: global membership-union input bound");
        store_cfg.instrument_ids.insert(store_cfg.instrument_ids.end(), ids.begin(), ids.end());
    }
    std::sort(store_cfg.instrument_ids.begin(), store_cfg.instrument_ids.end());
    store_cfg.instrument_ids.erase(std::unique(store_cfg.instrument_ids.begin(), store_cfg.instrument_ids.end()), store_cfg.instrument_ids.end());
    if (store_cfg.instrument_ids.empty() || store_cfg.instrument_ids.size() > 100000)
        return Err(ErrorCode::InvalidArgument, "panel store: empty/oversized complete membership union");
    // The source axis is the membership artifact's COMPLETE sorted union, not the
    // subset selected by this year/window or names whose current bars exist.
    store_cfg.original_indices.resize(store_cfg.instrument_ids.size());
    std::iota(store_cfg.original_indices.begin(), store_cfg.original_indices.end(), atx::u64{0});
    ATX_TRY(store_cfg.session_keys, data::history_session_keys(cfg.segs, {*start, *end}));
    ATX_TRY(auto before, snapshot_store_sources(cfg.segs, *start, *end));
    std::vector<std::string> initial_paths;
    for (const auto& file : before) initial_paths.push_back((std::filesystem::path(cfg.segs) / file.filename).string());
    ATX_TRY(auto sources, validate_panel_sources(cfg.segs, before, initial_paths, cfg.preparation_manifest));
    ATX_TRY(auto executable_sha, current_executable_sha256());
    std::vector<atx::u16> windows;
    if (cfg.augment_panel || !cfg.adv_windows.empty()) {
        if (cfg.adv_windows.empty()) {
            if (cfg.adv_window <= 0 || cfg.adv_window > 65535)
                return Err(ErrorCode::InvalidArgument, "panel store: invalid ADV window");
            windows.push_back(static_cast<atx::u16>(cfg.adv_window));
        } else windows = cfg.adv_windows;
        if (std::find(windows.begin(), windows.end(), 0) != windows.end())
            return Err(ErrorCode::InvalidArgument, "panel store: zero ADV window");
    }
    atx::usize warmup = 1;
    for (auto window : windows) warmup = std::max(warmup, static_cast<atx::usize>(window));
    if (windows.size() > 256)
        return Err(ErrorCode::InvalidArgument, "panel store: augmentation field bound");
    constexpr atx::u64 assembly_budget = 2ULL * 1024 * 1024 * 1024;
    constexpr atx::u64 fixed_budget = 384ULL * 1024 * 1024; // source mapping + writer + metadata
    const atx::u64 assembly_cells = atx::u64{std::min(warmup + store_cfg.chunk_dates, store_cfg.session_keys.size())} * store_cfg.instrument_ids.size();
    const atx::u64 bytes_per_cell = 512 + 24 * windows.size(); // raw/derived/augmentation overlap
    if (assembly_cells > (assembly_budget - fixed_budget) / bytes_per_cell)
        return Err(ErrorCode::InvalidArgument, "panel store: assembly/augmentation working budget exceeded");
    store_cfg.instrument_namespace = kSpiderRockSecurityIdNamespace;
    store_cfg.membership_sha256 = membership.membership_sha256;
    for (const auto& p : sources.parents) store_cfg.parents.push_back({p.role, p.sha256});
    store_cfg.parents.push_back({"membership", membership.membership_sha256});
    if (!executable_sha.empty()) store_cfg.parents.push_back({"producer_executable", executable_sha});
    store_cfg.recipe = nlohmann::json{{"rule", "mmap-f32-v2"}, {"source_window_start", std::to_string(*start)},
        {"source_window_end_exclusive", std::to_string(*end)}, {"axis", "complete-bound-membership-union-numeric-order"},
        {"source_index_namespace", "complete-bound-membership-union"},
        {"membership_rule", "common-stock-v2;rank<session;effective<=session;last-effective"},
        {"membership_cut", membership.cut}, {"masks", "present-independent-of-asof-member"},
        {"precision", "f32-fields;original-f64-adjusted-close;original-f64-returns"},
        {"return_rule", "original-adjusted-close-ratio-minus-one;no-terminal-imputation"},
        {"augmentation_adv_windows", windows}, {"vwap_rule", alpha::vwap_rule_name(cfg.vwap_rule)},
        {"warmup_sessions", warmup}, {"assembly_max_working_bytes", assembly_budget},
        {"assembly_admission_bytes_per_cell", bytes_per_cell},
        {"screen", "dated-membership-artifact;no-current-session-reranking"},
        {"historical_availability", "unknown-archive-snapshot;session-label-is-not-publication"},
        {"historical_vintages_verified", false}, {"ingestion_content_binding_verified", sources.ingestion_verified},
        {"preparation_content_binding_verified", sources.preparation_verified}}.dump();
    ATX_TRY(auto output_guard, reserve_pipeline_output(cfg.panel_out, true)); (void)output_guard;
    std::optional<data::PanelStoreWriter> writer;
    std::set<std::string> selected_paths;
    const auto n = store_cfg.instrument_ids.size();
    std::vector<atx::u8> present(n), tradable(n);
    for (atx::usize begin = 0; begin < store_cfg.session_keys.size(); begin += store_cfg.chunk_dates) {
        const auto finish = std::min(begin + store_cfg.chunk_dates, store_cfg.session_keys.size());
        const auto first = begin > warmup ? begin - warmup : 0;
        data::HistoryDataConfig hc;
        hc.seg_dir = cfg.segs;
        hc.window = {store_cfg.session_keys[first], finish < store_cfg.session_keys.size() ? store_cfg.session_keys[finish] : *end};
        hc.fixed_axis_ids = store_cfg.instrument_ids;
        hc.universe.min_adv_usd = 0; hc.universe.adv_window = 1;
        ATX_TRY(auto hp, data::build_history_panel(hc));
        if (hp.session_keys.size() != finish - first || hp.panel.instruments() != n ||
            !std::equal(hp.session_keys.begin(), hp.session_keys.end(), store_cfg.session_keys.begin() + static_cast<std::ptrdiff_t>(first)))
            return Err(ErrorCode::InvalidArgument, "panel store: source axes changed during chunk assembly");
        selected_paths.insert(hp.source_segment_paths.begin(), hp.source_segment_paths.end());
        if (!windows.empty()) {
            ATX_TRY(hp.panel, alpha::with_alpha101_fields(hp.panel, windows,
                alpha::DollarVolumeBasis::RawCloseV2, cfg.vwap_rule));
        }
        if (!writer) {
            for (atx::usize f = 0; f < hp.panel.num_fields(); ++f) {
                const auto name = hp.panel.field_name(f);
                const auto basis = data::history_field_level_basis(name, alpha::DollarVolumeBasis::RawCloseV2, cfg.vwap_rule);
                if (!basis) return Err(ErrorCode::InvalidArgument, "panel store: untagged output field");
                store_cfg.fields.push_back({name, *basis, name == "returns" ? data::PanelStorePrecision::ExactFloat64V2 : data::PanelStorePrecision::Float32V2});
            }
            ATX_TRY(auto created, data::PanelStoreWriter::create(cfg.panel_out, store_cfg));
            writer.emplace(std::move(created));
        }
        ATX_TRY(auto close, hp.panel.field_id("close"));
        std::vector<std::span<const atx::f64>> rows(hp.panel.num_fields());
        for (atx::usize date = begin; date < finish; ++date) {
            const auto local = date - first;
            const auto session = store_cfg.session_keys[date];
            const data::PitMembershipRebalance* active = nullptr;
            for (const auto& r : image.rebalances) {
                if (r.effective_session_key <= session && r.rank_session_key < session &&
                    (!active || r.effective_session_key > active->effective_session_key)) active = &r;
            }
            std::fill(tradable.begin(), tradable.end(), atx::u8{0});
            if (active) for (auto id : active->cuts[membership.store_cut].security_ids) {
                const auto pos = std::lower_bound(store_cfg.instrument_ids.begin(), store_cfg.instrument_ids.end(), id);
                if (pos == store_cfg.instrument_ids.end() || *pos != id)
                    return Err(ErrorCode::Internal, "panel store: member absent from fixed union");
                tradable[static_cast<atx::usize>(pos - store_cfg.instrument_ids.begin())] = 1;
            }
            for (atx::usize i = 0; i < n; ++i) present[i] = hp.panel.in_universe(local, i) ? 1 : 0;
            for (atx::usize f = 0; f < rows.size(); ++f) rows[f] = hp.panel.field_cross_section(static_cast<alpha::FieldId>(f), local);
            ATX_TRY_VOID(writer->append_date(date, rows, hp.panel.field_cross_section(close, local), present, tradable,
                                            active ? active->rank_session_key : 0));
        }
    }
    const std::vector<std::string> actual_paths(selected_paths.begin(), selected_paths.end());
    ATX_TRY(auto unchanged, validate_panel_sources(cfg.segs, before, actual_paths, cfg.preparation_manifest)); (void)unchanged;
    ATX_TRY(auto manifest_sha, writer->finish());
    StageResult result;
    result.digest = fnv1a64(manifest_sha.data(), manifest_sha.size());
    result.kvs = {{"panel_storage_rule", "mmap-f32-v2"}, {"manifest_sha256", manifest_sha},
        {"digest_rule", "fnv1a64-of-manifest-sha256-ascii"},
        {"dates", std::to_string(store_cfg.session_keys.size())}, {"instruments", std::to_string(n)},
        {"fields", std::to_string(store_cfg.fields.size())}, {"precision_acceptance", "unqualified"}};
    return Ok(std::move(result));
}

} // namespace

atx::core::Result<std::vector<atx::i64>>
pit_membership_allow_ids(const atx::engine::data::PitMembershipImage &image, atx::usize cut,
                         atx::i64 eval_start_nanos, atx::i64 end_exclusive_nanos) {
    using EC = atx::core::ErrorCode;
    // The single last rebalance effective BEFORE the evaluation start is the cut a
    // session at eval_start actually trades under, so the union would be missing its
    // incumbents without it. Rebalances are stored ascending, but the scan does not
    // rely on that: it keeps the maximum effective key below the start.
    const atx::engine::data::PitMembershipRebalance *prior = nullptr;
    std::vector<atx::i64> ids;
    for (const auto &rebalance : image.rebalances) {
        if (cut >= rebalance.cuts.size()) {
            return atx::core::Err(EC::InvalidArgument,
                "panel: membership image has no such cut index");
        }
        const atx::i64 effective = rebalance.effective_session_key;
        if (effective < eval_start_nanos) {
            if (prior == nullptr || effective > prior->effective_session_key) {
                prior = &rebalance;
            }
            continue;
        }
        if (effective >= end_exclusive_nanos) continue;
        const auto &members = rebalance.cuts[cut].security_ids;
        ids.insert(ids.end(), members.begin(), members.end());
    }
    if (prior != nullptr) {
        const auto &members = prior->cuts[cut].security_ids;
        ids.insert(ids.end(), members.begin(), members.end());
    }
    std::sort(ids.begin(), ids.end());
    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
    if (ids.empty()) {
        return atx::core::Err(EC::InvalidArgument,
            "panel: the membership cut has no member in [--universe-eval-start, --end)");
    }
    return atx::core::Ok(std::move(ids));
}

atx::core::Result<StageResult> run_panel(const RunConfig& cfg) {

    if (cfg.panel_storage_rule == "mmap-f32-v2") return run_panel_store_v2(cfg);
    if (cfg.panel_storage_rule != "legacy-f64-v1")
        return atx::core::Err(atx::core::ErrorCode::InvalidArgument, "panel: unsupported storage rule");

    // 1. Validate required fields.
    if (cfg.segs.empty() || cfg.panel_out.empty()) {
        return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                              "panel: --segs and --out (panel_out) required");
    }
    if (cfg.incremental_panel) {
        return atx::core::Err(atx::core::ErrorCode::NotImplemented,
            "panel: incremental mode requires verified old axes and a separate fresh output; "
            "rebuild the selected history into a new identified artifact");
    }
    if (!std::isfinite(cfg.min_adv_usd) || cfg.min_adv_usd < 0.0 ||
        !std::isfinite(cfg.min_price) || cfg.min_price < 0.0 || cfg.top_n_by_adv < 0) {
        return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                              "panel: universe thresholds must be finite and nonnegative");
    }

    // 2. Build TimeWindow.
    atx::engine::alpha::TimeWindow w{};
    if (!cfg.start.empty()) {
        auto ns = atx::engine::data::detail::date_to_nanos(cfg.start);
        if (!ns.has_value()) {
            return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                                  "panel: --start is not a valid date: " + cfg.start);
        }
        w.start_nanos = *ns;
    }
    if (!cfg.end.empty()) {
        auto ns = atx::engine::data::detail::date_to_nanos(cfg.end);
        if (!ns.has_value()) {
            return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                                  "panel: --end is not a valid date: " + cfg.end);
        }
        w.end_nanos = *ns;
    }

    // 3. Build UniverseConfig.
    atx::engine::data::UniverseConfig u{};
    u.min_adv_usd = cfg.min_adv_usd;
    u.top_n_by_adv = static_cast<atx::usize>(cfg.top_n_by_adv);
    u.min_price = cfg.min_price;            // raw-close floor (0 = off)
    u.require_sector = cfg.require_sector;  // Sector availability is not asset-type eligibility.
    // adv_window=21 and min_mktcap_usd=0 remain at their defaults.

    std::vector<atx::u16> adv_wins;
    if (cfg.augment_panel || !cfg.adv_windows.empty()) {
        if (cfg.adv_windows.empty()) {
            if (cfg.adv_window <= 0 ||
                cfg.adv_window > std::numeric_limits<atx::u16>::max()) {
                return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                                      "panel: augmentation ADV window must fit positive u16");
            }
            adv_wins.push_back(static_cast<atx::u16>(cfg.adv_window));
        } else {
            adv_wins = cfg.adv_windows;
        }
        for (const auto value : adv_wins) {
            if (value == 0) {
                return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                                      "panel: augmentation ADV window cannot be zero");
            }
        }
    }

    // 4. Keep exact source axes alongside the numerical panel through augmentation.
    ATX_TRY(auto output_guard, reserve_pipeline_output(cfg.panel_out, true));
    (void)output_guard;
    atx::engine::data::HistoryDataConfig hc{cfg.segs, w, u};
    hc.compact_to_universe = cfg.compact_universe; // drop never-in-universe columns
    // Checkpoint 16: optional point-in-time membership restriction. Absent the flags
    // this resolves to an inactive restriction, hc.allow_ids stays empty, and the
    // engine's restriction step is skipped entirely.
    ATX_TRY(auto membership, resolve_membership_restriction(cfg, w.end_nanos));
    hc.allow_ids = membership.allow_ids;
    // R21-3: load + validate every --asof-field CSV before the history build.
    ATX_TRY(auto asof_fields, load_asof_fields(cfg));
    ATX_TRY(auto before, snapshot_panel_sources(cfg.segs, w.start_nanos, w.end_nanos));
    ATX_TRY(auto hp, atx::engine::data::build_history_panel(hc));

    // S7-3: opt-in Alpha101 panel augmentation. Default (augment_panel=false, adv_windows
    // empty) skips this entirely -> panel.bin byte-identical. Empty list falls back to the
    // single legacy --adv-window.
    if (!adv_wins.empty()) {
        ATX_TRY(hp.panel, atx::engine::alpha::with_alpha101_fields(hp.panel, adv_wins,
            atx::engine::alpha::DollarVolumeBasis::RawCloseV2, cfg.vwap_rule));
        hp.field_basis.clear();
        hp.field_basis.reserve(hp.panel.num_fields());
        for (atx::usize f = 0; f < hp.panel.num_fields(); ++f) {
            const auto basis = atx::engine::data::history_field_level_basis(
                hp.panel.field_name(static_cast<atx::engine::alpha::FieldId>(f)),
                atx::engine::alpha::DollarVolumeBasis::RawCloseV2, cfg.vwap_rule);
            if (!basis) return atx::core::Err(atx::core::ErrorCode::Internal,
                "panel augmentation produced a field without a level basis");
            hp.field_basis.push_back(*basis);
        }
        hp.digest = atx::engine::data::digest_panel(hp.panel);
    }

    // R21-3: append the point-in-time --asof-field columns LAST, in command-line
    // order (no flag -> no-op, panel.bin byte-identical). See asof_field.hpp.
    ATX_TRY_VOID(append_asof_fields(hp, asof_fields, cfg.panel_asof_max_stale_days));

    // 5. Serialize.
    ATX_TRY(auto sources, validate_panel_sources(cfg.segs, before, hp.source_segment_paths,
                                                 cfg.preparation_manifest));
    ATX_TRY(auto executable_sha, current_executable_sha256());
    PanelIdentity identity;
    identity.instrument_namespace = kSpiderRockSecurityIdNamespace;
    identity.session_keys = std::move(hp.session_keys);
    identity.instrument_ids = std::move(hp.instrument_ids);
    identity.original_instrument_indices = std::move(hp.original_instrument_indices);
    identity.recipe = panel_recipe(hc, adv_wins, sources, executable_sha, membership,
                                   asof_fields, cfg.panel_asof_max_stale_days, cfg.vwap_rule);
    identity.parents = std::move(sources.parents);
    if (!executable_sha.empty()) identity.parents.push_back({"producer_executable", executable_sha});
    // PanelParent carries (role, sha256) only; the CSV path goes to the sidecar.
    for (const auto &field : asof_fields) {
        identity.parents.push_back({"asof_field:" + field.name, field.sha256});
    }

    // 6. Write provenance sidecar (<panel_out>.meta.txt).
    // The sidecar is informational only — it does NOT affect panel.bin or its digest.
    // A write failure is surfaced as Err(IoError) per the repo "never ignore an error" bar.
    {
        // Capture wall-clock UTC for the build timestamp (sidecar is non-deterministic by design).
        const auto now = std::chrono::system_clock::now();
        const std::time_t tt = std::chrono::system_clock::to_time_t(now);
        std::tm gm{};
#if defined(_WIN32)
        gmtime_s(&gm, &tt);
#else
        gmtime_r(&tt, &gm);
#endif
        char ts_buf[32]{};
        std::strftime(ts_buf, sizeof(ts_buf), "%Y-%m-%dT%H:%M:%SZ", &gm);

        // Runtime values reflect actual augmentation state (S7-3 wired).
        const bool did_augment = cfg.augment_panel || !cfg.adv_windows.empty();
        std::string adv_windows_val = "none";
        if (did_augment) {
            if (cfg.adv_windows.empty()) {
                adv_windows_val = std::to_string(cfg.adv_window);
            } else {
                adv_windows_val.clear();
                for (std::size_t i = 0; i < cfg.adv_windows.size(); ++i) {
                    if (i) adv_windows_val += ",";
                    adv_windows_val += std::to_string(cfg.adv_windows[i]);
                }
            }
        }
        const char* augmented_val = did_augment ? "true" : "false";

        const std::string meta_path = cfg.panel_out + ".meta.txt";
        std::ofstream mf(meta_path, std::ios::out | std::ios::trunc);
        if (!mf.is_open()) {
            return atx::core::Err(atx::core::ErrorCode::IoError,
                                  "panel: cannot open sidecar for writing: " + meta_path);
        }

        mf << "atx_panel_meta_v1\n"
           << "built_utc=" << ts_buf << "\n"
           << "panel_bin=" << cfg.panel_out << "\n"
           << "universe_min_adv_usd=" << cfg.min_adv_usd << "\n"
           << "universe_top_n_by_adv=" << cfg.top_n_by_adv << "\n"
           << "universe_min_price=" << cfg.min_price << "\n"
           << "universe_require_sector=" << (cfg.require_sector ? "true" : "false") << "\n"
           << "adv_windows=" << adv_windows_val << "\n"
           << "vwap_rule=" << atx::engine::alpha::vwap_rule_name(cfg.vwap_rule) << "\n"
           << "augmented=" << augmented_val << "\n"
           << "engine_digest=" << to_hex16(hp.digest) << "\n"
           << "dates=" << hp.panel.dates() << "\n"
           << "instruments=" << hp.panel.instruments() << "\n"
           << "fields=" << hp.panel.num_fields() << "\n";
        for (const auto &field : asof_fields) {
            mf << "asof_field=" << field.name << " path=" << field.path
               << " sha256=" << field.sha256 << " rows=" << field.table.rows.size()
               << " rows_ignored_unknown_id=" << field.rows_ignored_unknown_id
               << " max_stale_days=" << cfg.panel_asof_max_stale_days << "\n";
        }

        if (!mf.good()) {
            return atx::core::Err(atx::core::ErrorCode::IoError,
                                  "panel: sidecar write failed: " + meta_path);
        }
        mf.close();
        if (!mf.good()) {
            return atx::core::Err(atx::core::ErrorCode::IoError,
                                  "panel: sidecar close failed: " + meta_path);
        }
    }

    // Publish the required identity manifest only after all companion writes succeed.
    ATX_TRY(auto artifact, write_panel_artifact(hp.panel, cfg.panel_out, identity));

    // 7. Return StageResult.
    StageResult sr;
    sr.digest = artifact.payload_digest;
    sr.kvs = {
        {"dates",         std::to_string(hp.panel.dates())},
        {"instruments",   std::to_string(hp.panel.instruments())},
        {"fields",        std::to_string(hp.panel.num_fields())},
        {"engine_digest", to_hex16(hp.digest)},
        {"artifact_id", artifact.artifact_id},
        {"payload_sha256", artifact.payload_sha256},
    };
    // Additive and opt-in, so the no-flag digest line is unchanged. `instruments`
    // above is already the kept-column count; these say what restricted it.
    if (membership.active) {
        sr.kvs.push_back({"allow_list_size", std::to_string(membership.allow_ids.size())});
        sr.kvs.push_back({"allow_list_excluded_columns",
                          std::to_string(hp.allow_list_excluded_columns)});
    }
    return atx::core::Ok(std::move(sr));
}

} // namespace atx::impl
