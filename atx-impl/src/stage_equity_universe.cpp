#include "stage_equity_universe.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "artifacts.hpp"
#include "atx/core/sha256.hpp"
#include "atx/engine/data/orats_history.hpp"
#include "atx/engine/data/point_in_time_universe.hpp"
#include "atx/tsdb/segment_reader.hpp"
#include "stage_data_provenance.hpp"
#include "trial_ledger.hpp"

namespace atx::impl {

// §5.8 / R15-16. Defined here (not in the header) because the runner scans this
// file for exactly one assignment of a 64-hex string literal to the constant named
// below (see the header comment). Never read at runtime: the runner does the
// on-disk preflight, exactly as stage_equity_ic.cpp:49-59 / ruling I-2.
const std::string_view kEquityUniverseDesignNoteRelativePath =
    "atx-engine/reviews/2026-09-20-iteration15-point-in-time-universe-design.md";
const std::string_view kEquityUniverseDesignNoteSha256 =
    "4810fda251c6c285b29413ab6bea05b46db66e9bb0620cf17950b45075267dc8";

namespace {
namespace fs = std::filesystem;
namespace data = atx::engine::data;
using Json = nlohmann::json;
using atx::core::Err;
using atx::core::ErrorCode;
using atx::core::Ok;
using atx::core::Result;
using atx::core::Status;

// The TU-local spellings the brief asks for; both alias the exported constants.
const std::string_view kDesignNotePath = kEquityUniverseDesignNoteRelativePath;
const std::string_view kDesignNoteSha256 = kEquityUniverseDesignNoteSha256;

constexpr atx::u64 kOverheadReserve = 512'000'000; // stage_equity_ic.cpp:45
constexpr std::string_view kDomain = "atx-equity-universe-v1\n";
constexpr std::string_view kDefaultTrialLedger = "atx-engine/reviews/trial-ledger.jsonl";
constexpr std::string_view kPeakWorkingSetSource =
    "measured by build-equity/audits/iteration15_run_equity_universe.py at 100 ms sampling; "
    "the producer measures wall time only";

// §5.6 — the ledger line, and §6 — the frozen pre-registration block. Changing any
// value here is a NEW pre-registration (new ledger line, new design SHA).
constexpr atx::i64 kCheckpoint = 15;
constexpr atx::i64 kTrialCountDeclared = 0;
constexpr std::string_view kPurpose = "point-in-time-universe-construction";
constexpr std::string_view kTrialIdPrefix = "iteration15-point-in-time-universe-";
constexpr std::array<atx::usize, 3> kTopN{1000, 2000, 3000};
constexpr std::array<atx::u32, 2> kBandBp{0, 1000};
constexpr atx::usize kCuts = kTopN.size() * kBandBp.size();
static_assert(kCuts <= data::kPitMaxCuts, "the frozen cut set must fit the engine bound");
static_assert(kTopN.size() <= data::kPitMaxTopNValues && kBandBp.size() <= data::kPitMaxBandValues,
              "the frozen top_n / band sets must fit the engine config arrays");
constexpr std::string_view kSignalName = "adv63_median_dollar_volume";
constexpr std::string_view kSignalDsl =
    "median_63(close*volume) over sessions <= rank date; valid>=57; raw close>1.0 on rank date";
constexpr std::string_view kSignalDslV2 =
    "common-stock-v2; median_63(close*volume)>=5000000; valid>=57; raw close>=5; "
    "verified dated common-stock type; available_at<rank_session; no conflicting type";
constexpr std::string_view kAlignment = "rank-at-t-effective-at-t-plus-1-session";
constexpr std::string_view kCadence =
    "monthly: rank session = attached session whose UTC month differs from the next attached "
    "session's month; the last attached session is never one; [rank-start,rank-end] filter after";
constexpr std::string_view kLedgerNotes =
    "Checkpoint 15 universe construction; not a forecast trial (trial_count_declared 0). "
    "recipe.horizons/quantiles/bootstrap/autocorr_lags/turnover_source/forward_variants/samples/"
    "common_sample_rule/net_spread_rule/cost and source_exclusions carry atx-trial-ledger-v1 "
    "defaults and are NOT APPLICABLE to this line. The six top_n x band cuts are reported side "
    "by side (AR-7 semantics); selecting among them later is a new trial.";
constexpr std::string_view kSealPolicy = "RefuseAtOrAfterValidationBeginV1";
constexpr std::string_view kValidationBegin = "2020-01-01";
constexpr std::string_view kSealedBegin = "2023-01-01";
constexpr std::string_view kSurvivorshipCaveat =
    "Both metrics are LOWER BOUNDS on survivorship bias: the archive's backfill policy is "
    "unknown (historical_availability: unknown-archive-snapshot), the archive has no delisting "
    "date/code/return fields, and a name absent from the archive from its first day is "
    "invisible here.";

// §7 preflight terms beyond the engine state: one mapped segment, JSON manifests read one
// at a time, the two streamed CSV buffers and the post-run membership.bin string.
constexpr atx::u64 kManifestReadBytes = 16ULL * 1024ULL * 1024ULL;
constexpr atx::u64 kStreamBufferBytes = 1ULL * 1024ULL * 1024ULL;
constexpr atx::u64 kMembershipBinReserve = 20ULL * 1024ULL * 1024ULL;

// ---------------------------------------------------------------------------
//  Formatting and small IO helpers, restated TU-locally exactly as
//  stage_equity_ic.cpp:145-262 does (the design copies the idiom, §4 intro).
// ---------------------------------------------------------------------------
template <class Number> std::string number(Number value) {
    if constexpr (std::is_floating_point_v<Number>) {
        // An empty cell is the honest rendering of "no value" (§4.1: NaN cells are empty).
        if (!std::isfinite(value)) return std::string();
    }
    std::array<char, 64> bytes{};
    const auto result = std::to_chars(bytes.data(), bytes.data() + bytes.size(), value);
    if (result.ec != std::errc{}) {
        throw std::runtime_error("equity universe: numeric formatting failed");
    }
    return {bytes.data(), result.ptr};
}

Result<atx::u64> add(atx::u64 a, atx::u64 b) {
    if (b > std::numeric_limits<atx::u64>::max() - a) {
        return Err(ErrorCode::OutOfRange, "equity universe: memory estimate overflow");
    }
    return Ok(a + b);
}

Result<atx::u64> multiply(atx::u64 a, atx::u64 b) {
    if (a != 0 && b > std::numeric_limits<atx::u64>::max() / a) {
        return Err(ErrorCode::OutOfRange, "equity universe: memory estimate overflow");
    }
    return Ok(a * b);
}

// §3.1 civil algorithm, through the engine's own pit_civil_of: YYYY-MM-DD.
std::string date_text(atx::i64 session_key) {
    const data::PitCivilDate civil = data::pit_civil_of(session_key);
    std::array<char, 32> buffer{};
    const int written = std::snprintf(buffer.data(), buffer.size(), "%04d-%02u-%02u",
                                      static_cast<int>(civil.year),
                                      static_cast<unsigned>(civil.month),
                                      static_cast<unsigned>(civil.day));
    if (written <= 0) throw std::runtime_error("equity universe: date formatting failed");
    return std::string(buffer.data(), static_cast<atx::usize>(written));
}

// §4.1: `band` is the literal `0.00` / `0.10` (basis points over 10000, two decimals).
std::string band_text(atx::u32 band_bp) {
    const atx::u32 hundredths = band_bp / 100U;
    std::array<char, 32> buffer{};
    const int written = std::snprintf(buffer.data(), buffer.size(), "%u.%02u",
                                      static_cast<unsigned>(hundredths / 100U),
                                      static_cast<unsigned>(hundredths % 100U));
    if (written <= 0) throw std::runtime_error("equity universe: band formatting failed");
    return std::string(buffer.data(), static_cast<atx::usize>(written));
}

// Cut index c = top_n_index * band_count + band_index (§3.1); §4.3 column suffix
// `_t1000_b0`; §5.6 restriction name `top1000_b0`.
atx::usize cut_top_n(atx::usize cut) { return kTopN[cut / kBandBp.size()]; }
atx::u32 cut_band_bp(atx::usize cut) { return kBandBp[cut % kBandBp.size()]; }
std::string cut_suffix(atx::usize cut) {
    return "_t" + number(cut_top_n(cut)) + "_b" + number(cut_band_bp(cut));
}
std::string cut_name(atx::usize cut) {
    return "top" + number(cut_top_n(cut)) + "_b" + number(cut_band_bp(cut));
}

// §4.1: `gics` empty when NaN, else the integer text of the f64.
std::string gics_text(atx::f64 gics) {
    if (!std::isfinite(gics)) return std::string();
    if (gics == std::floor(gics) && std::fabs(gics) < 9.0e18) {
        return number(static_cast<atx::i64>(gics));
    }
    return number(gics);
}

// §5.3 — the M-6 diagnostic fixed locally: AlreadyExists only when the root exists,
// IoError (with the OS message) for a failed parent or root creation.
Status reserve_directory(const fs::path &directory) {
    std::error_code ec;
    if (fs::exists(directory, ec) || ec) {
        return Err(ErrorCode::AlreadyExists, "equity universe: output root must not exist");
    }
    const fs::path parent = directory.parent_path();
    if (!parent.empty()) {
        fs::create_directories(parent, ec);
        if (ec) {
            return Err(ErrorCode::IoError,
                       "equity universe: cannot create parent: " + ec.message());
        }
    }
    if (!fs::create_directory(directory, ec) || ec) {
        return Err(ErrorCode::IoError,
                   "equity universe: cannot create output root: " + ec.message());
    }
    if (!fs::create_directory(directory / ".pending", ec) || ec) {
        return Err(ErrorCode::IoError, "equity universe: cannot reserve publication");
    }
    return Ok();
}

// §4 intro: `.partial` -> hard link -> remove partial, digest sha256_hex.
Result<Json> write_text(const fs::path &directory, const std::string &name, std::string_view text) {
    const auto partial = directory / (name + ".partial");
    const auto final_path = directory / name;
    std::ofstream out(partial, std::ios::binary);
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    out.close();
    if (!out) return Err(ErrorCode::IoError, "equity universe: cannot write " + name);
    ATX_TRY(auto sha, atx::core::sha256_hex(text));
    std::error_code ec;
    fs::create_hard_link(partial, final_path, ec);
    if (ec) return Err(ErrorCode::IoError, "equity universe: cannot publish " + name);
    fs::remove(partial, ec);
    if (ec) return Err(ErrorCode::IoError, "equity universe: cannot remove published partial");
    return Ok(Json{{"filename", name}, {"sha256", sha}, {"size_bytes", text.size()}});
}

Result<std::string> read_file(const fs::path &path, atx::u64 max_bytes) {
    std::error_code ec;
    const auto length = fs::file_size(path, ec);
    if (ec || length == 0 || length > max_bytes) {
        return Err(ErrorCode::IoError, "equity universe: invalid companion size: " + path.string());
    }
    std::ifstream in(path, std::ios::binary);
    std::string text(static_cast<atx::usize>(length), '\0');
    if (!in.read(text.data(), static_cast<std::streamsize>(length)) ||
        in.peek() != std::char_traits<char>::eof()) {
        return Err(ErrorCode::IoError, "equity universe: cannot read " + path.string());
    }
    return Ok(std::move(text));
}

Json strict_json(const std::string &text) {
    std::vector<std::set<std::string>> keys;
    auto callback = [&](int depth, Json::parse_event_t event, Json &parsed) {
        if (depth > 64) throw std::invalid_argument("equity universe: JSON nesting exceeds 64");
        if (event == Json::parse_event_t::object_start) keys.emplace_back();
        if (event == Json::parse_event_t::key &&
            !keys.back().insert(parsed.get<std::string>()).second) {
            throw std::invalid_argument("equity universe: duplicate JSON key");
        }
        if (event == Json::parse_event_t::object_end) keys.pop_back();
        return true;
    };
    return Json::parse(text, callback);
}

// A non-negative integer count from a manifest, refusing reals, negatives and text.
Result<atx::u64> count(const Json &value) {
    if (!value.is_number_integer() || value.is_number_float()) {
        return Err(ErrorCode::ParseError, "equity universe: manifest count is not an integer");
    }
    const auto parsed = value.get<atx::i64>();
    if (parsed < 0) return Err(ErrorCode::ParseError, "equity universe: negative manifest count");
    return Ok(static_cast<atx::u64>(parsed));
}

atx::f64 elapsed_seconds(std::chrono::steady_clock::time_point started) {
    const std::chrono::duration<atx::f64> span = std::chrono::steady_clock::now() - started;
    return span.count();
}

std::string utc_now() {
    const auto now = std::chrono::system_clock::now();
    const std::time_t tt = std::chrono::system_clock::to_time_t(now);
    std::tm gm{};
#if defined(_WIN32)
    gmtime_s(&gm, &tt);
#else
    gmtime_r(&tt, &gm);
#endif
    std::array<char, 32> buffer{};
    std::strftime(buffer.data(), buffer.size(), "%Y-%m-%dT%H:%M:%SZ", &gm);
    return std::string(buffer.data());
}

// §2.1 instrument identity: canonical decimal positive i64 securityID, the
// history_panel.cpp:117-125 rule (from_chars, > 0, round-trip string equality).
Result<atx::i64> parse_security_id(std::string_view text) {
    atx::i64 id{};
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), id);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || id <= 0 ||
        std::string_view(number(id)) != text) {
        return Err(ErrorCode::InvalidArgument,
                   "equity universe: noncanonical positive i64 securityID '" + std::string(text) +
                       "'");
    }
    return Ok(id);
}

// ---------------------------------------------------------------------------
//  Profile — §5.2 flags, validated.
// ---------------------------------------------------------------------------
struct Profile {
    std::vector<std::string> segments_dirs;
    std::vector<std::string> preparation_manifests;
    atx::i64 rank_start{};
    atx::i64 rank_end{};
    std::string ledger_path;
    std::string executable;
    atx::u64 max_working_bytes{};
    data::PitUniverseRule rule{data::PitUniverseRule::LegacyV1};
    std::string instrument_types;
};

// §5.2 / DR15-16 Q3: `;`-separated lists; an empty entry is rejected. A path that
// itself contains `;` cannot be expressed, which is the "rejected" of §5.2.
Result<std::vector<std::string>> split_list(std::string_view text, std::string_view flag) {
    std::vector<std::string> out;
    std::string_view rest = text;
    while (true) {
        const auto at = rest.find(';');
        const std::string_view token = rest.substr(0, at);
        if (token.empty()) {
            return Err(ErrorCode::InvalidArgument,
                       "equity universe: --" + std::string(flag) + " has an empty list entry");
        }
        out.emplace_back(token);
        if (at == std::string_view::npos) break;
        rest.remove_prefix(at + 1);
    }
    return Ok(std::move(out));
}

// §5.2: the frozen allow-list; no --top-n, --band, --rank-key, --cadence, --config.
Result<Profile> resolve(const RunConfig &cfg) {
    const std::set<std::string> allowed{"segments-dirs", "preparation-manifests", "out",
        "rank-start", "rank-end", "max-working-bytes", "trial-ledger", "quiet", "digest-only",
        "universe-rule", "instrument-types"};
    for (const auto &flag : cfg.set_flags) {
        if (!allowed.contains(flag)) {
            return Err(ErrorCode::InvalidArgument, "equity universe: unsupported flag --" + flag);
        }
    }
    // The same "no override of the frozen recipe" predicate as stage_equity_ic.cpp:360-373.
    if (cfg.allow_unidentified_panels || !cfg.config_file.empty() || cfg.cost_bps != 0 ||
        cfg.borrow_bps != 0 || cfg.gross != 0 || cfg.name_cap != 0 || !cfg.rebalance.empty() ||
        cfg.trade_rate != 1 || cfg.risk_aversion != 0 || cfg.turnover_penalty != 0 ||
        !cfg.method.empty() || cfg.conviction || cfg.kelly_fraction != 0 || cfg.sector_neutral ||
        cfg.industry_neutral || cfg.group_neutralize || cfg.risk_model != "diagonal" ||
        cfg.dead_alpha_factors || cfg.metabook || cfg.gp_trading || cfg.participation_cap != 0 ||
        cfg.book_turnover_gate != 0 || cfg.fit_begin != 0 || cfg.fit_end != 0 ||
        cfg.combine_holdout_frac != 0 || cfg.weight_transform != "rank" ||
        cfg.winsorize_limit != 0.025 || cfg.gross_leverage != 1 || !cfg.seed_exprs.empty() ||
        !cfg.library_dir.empty() || cfg.gated || cfg.walk_forward != 0 || cfg.corr_penalty != 0 ||
        cfg.capacity_floor != 0 || !cfg.combo.empty() || !cfg.books.empty() ||
        !cfg.panel.empty() || !cfg.equity_baseline_dir.empty() ||
        !cfg.equity_evaluation_start.empty() || !cfg.equity_evaluation_end.empty()) {
        return Err(ErrorCode::InvalidArgument,
                   "equity universe: unsupported override of the frozen pre-registered recipe");
    }
    if (cfg.equity_segments_dirs.empty() || cfg.equity_preparation_manifests.empty() ||
        cfg.out.empty() || cfg.equity_rank_start.empty() || cfg.equity_rank_end.empty() ||
        cfg.equity_max_working_bytes <= kOverheadReserve) {
        return Err(ErrorCode::InvalidArgument,
                   "equity universe: segments-dirs, preparation-manifests, fresh out, rank-start, "
                   "rank-end and a budget above the overhead reserve are required");
    }
    Profile profile;
    if (cfg.equity_universe_rule != "legacy-v1" && cfg.equity_universe_rule != "common-stock-v2")
        return Err(ErrorCode::InvalidArgument, "equity universe: invalid universe rule");
    profile.rule = cfg.equity_universe_rule == "common-stock-v2" ?
        data::PitUniverseRule::CommonStockV2 : data::PitUniverseRule::LegacyV1;
    profile.instrument_types = cfg.equity_instrument_types;
    if ((profile.rule == data::PitUniverseRule::CommonStockV2) != !profile.instrument_types.empty())
        return Err(ErrorCode::InvalidArgument,
                   "equity universe: V2 requires --instrument-types; legacy-v1 refuses type input");
    ATX_TRY(profile.segments_dirs, split_list(cfg.equity_segments_dirs, "segments-dirs"));
    ATX_TRY(profile.preparation_manifests,
            split_list(cfg.equity_preparation_manifests, "preparation-manifests"));
    if (profile.segments_dirs.size() != profile.preparation_manifests.size()) {
        return Err(ErrorCode::InvalidArgument,
                   "equity universe: --preparation-manifests must pair positionally with "
                   "--segments-dirs (same count)");
    }
    using atx::engine::data::detail::date_to_nanos;
    const auto start = date_to_nanos(cfg.equity_rank_start);
    const auto end = date_to_nanos(cfg.equity_rank_end);
    // R15-13 / C-6: rank-end <= 2019-12-31, i.e. strictly before the validation boundary.
    if (!start || !end || *start > *end || *end >= data::kPitSessionKeyEndExclusive) {
        return Err(ErrorCode::InvalidArgument,
                   "equity universe: rank window must be YYYY-MM-DD, ordered, and end before "
                   "2020-01-01");
    }
    profile.rank_start = *start;
    profile.rank_end = *end;
    profile.ledger_path = cfg.equity_trial_ledger.empty() ? std::string(kDefaultTrialLedger)
                                                          : cfg.equity_trial_ledger;
    profile.max_working_bytes = cfg.equity_max_working_bytes;
    ATX_TRY(profile.executable, current_executable_sha256());
    return Ok(std::move(profile));
}

// ---------------------------------------------------------------------------
//  Inputs — §5.4 seal refusal, §5.5 step 2 binding, DR15-8 rank sessions.
// ---------------------------------------------------------------------------
struct SegmentFile {
    fs::path path;
    std::string filename;
    atx::i64 key{};
    atx::u64 size_bytes{};
    std::string sha256;
    atx::usize dir{};
};

struct SegmentDir {
    std::string path;
    std::string role; // §5.6 parent role: segments-<year> or segments-<first>-<last>
    std::string ingestion_manifest_path;
    std::string ingestion_manifest_sha256;
    std::string loader_executable_sha256;
    std::string preparation_manifest_path;
    std::string preparation_manifest_sha256;
    atx::i64 first_key{};
    atx::i64 last_key{};
    atx::usize files{};
};

// §4.3's three preparation columns, summed per calendar year from `daily_counts[]`.
struct YearPreparation {
    atx::u64 dates_with_quarantined_duplicates{};
    atx::u64 duplicate_positive_keys{};
    atx::u64 rejected_rows{};
};

struct Inputs {
    data::PitUniverseRule rule{data::PitUniverseRule::LegacyV1};
    std::vector<data::PitInstrumentTypeEvidence> types;
    std::string types_text;
    std::string types_sha256;
    std::vector<SegmentDir> dirs;
    std::vector<SegmentFile> files; // global date order
    std::vector<atx::i64> keys;     // parallel to files
    std::vector<atx::i64> rank_keys;
    std::map<atx::i32, YearPreparation> preparation; // ordered: iteration is deterministic
    atx::u64 max_segment_bytes{};
    atx::usize first_rank_ordinal{};
};

// §5.4: every `*.seg` name in one directory, parsed before any file is mapped.
Result<std::vector<SegmentFile>> list_segments(const fs::path &dir, atx::usize dir_index) {
    std::error_code ec;
    if (!fs::is_directory(dir, ec) || ec) {
        return Err(ErrorCode::InvalidArgument,
                   "equity universe: not a segment directory: " + dir.string());
    }
    std::vector<SegmentFile> files;
    for (const auto &entry : fs::directory_iterator(dir, ec)) {
        std::error_code probe;
        if (!entry.is_regular_file(probe) || probe) continue;
        const std::string name = entry.path().filename().string();
        if (!name.ends_with(".seg")) continue;
        const std::string stem = name.substr(0, name.size() - 4);
        const auto key = atx::engine::data::detail::date_to_nanos(stem);
        if (stem.size() != 10 || !key) {
            return Err(ErrorCode::InvalidArgument,
                       "equity universe: segment name is not YYYY-MM-DD.seg: " + name);
        }
        if (*key >= data::kPitSessionKeyEndExclusive) {
            return Err(ErrorCode::PermissionDenied,
                       "equity universe: segment at/after validation boundary: " + name);
        }
        const auto size = entry.file_size(probe);
        if (probe) return Err(ErrorCode::IoError, "equity universe: cannot stat " + name);
        files.push_back(SegmentFile{entry.path(), name, *key, static_cast<atx::u64>(size),
                                    std::string(), dir_index});
    }
    if (ec) return Err(ErrorCode::IoError, "equity universe: cannot list " + dir.string());
    if (files.empty()) {
        return Err(ErrorCode::InvalidArgument,
                   "equity universe: no .seg files in " + dir.string());
    }
    std::sort(files.begin(), files.end(),
              [](const SegmentFile &a, const SegmentFile &b) { return a.key < b.key; });
    return Ok(std::move(files));
}

// §5.5 step 2, binding I-12: every selected `.seg` hashes to its manifest entry, and
// every manifest entry has a file. Fills SegmentDir's manifest fields.
Status bind_ingestion(SegmentDir &sd, std::vector<SegmentFile> &files) {
    const fs::path receipt = fs::path(sd.path) / "_ingestion.manifest.json";
    sd.ingestion_manifest_path = receipt.string();
    ATX_TRY(const auto text, read_file(receipt, kManifestReadBytes));
    ATX_TRY(sd.ingestion_manifest_sha256, atx::core::sha256_hex(text));
    const Json doc = strict_json(text);
    if (doc.at("schema") != "atx-ingestion-v1" || doc.at("status") != "complete" ||
        !doc.at("segments").is_array()) {
        return Err(ErrorCode::InvalidArgument,
                   "equity universe: invalid ingestion receipt: " + receipt.string());
    }
    std::map<std::string, std::string> declared; // filename -> sha256
    for (const auto &item : doc.at("segments")) {
        const auto name = item.at("filename").get<std::string>();
        const auto sha = item.at("sha256").get<std::string>();
        if (!declared.emplace(name, sha).second) {
            return Err(ErrorCode::ParseError,
                       "equity universe: duplicate ingestion segment binding: " + name);
        }
    }
    std::set<std::string> present;
    for (auto &file : files) {
        present.insert(file.filename);
        const auto found = declared.find(file.filename);
        if (found == declared.end()) {
            return Err(ErrorCode::InvalidArgument,
                       "equity universe: segment absent from the ingestion manifest: " +
                           file.filename);
        }
        ATX_TRY(file.sha256, atx::core::sha256_file(file.path.string()));
        if (file.sha256 != found->second) {
            return Err(ErrorCode::InvalidArgument,
                       "equity universe: segment digest differs from the ingestion manifest: " +
                           file.filename);
        }
    }
    for (const auto &item : declared) {
        if (!present.contains(item.first)) {
            return Err(ErrorCode::InvalidArgument,
                       "equity universe: ingestion manifest names a segment with no file: " +
                           item.first);
        }
    }
    const auto &prep = doc.at("preparation");
    if (!prep.is_object() || !prep.contains("manifest_sha256") ||
        !prep.at("manifest_sha256").is_string()) {
        return Err(ErrorCode::InvalidArgument,
                   "equity universe: no preparation binding in the ingestion receipt");
    }
    sd.preparation_manifest_sha256 = prep.at("manifest_sha256").get<std::string>();
    sd.loader_executable_sha256 = "unknown";
    if (doc.at("recipe").is_string()) {
        const Json recipe = strict_json(doc.at("recipe").get<std::string>());
        if (recipe.contains("executable_sha256") && recipe.at("executable_sha256").is_string()) {
            sd.loader_executable_sha256 = recipe.at("executable_sha256").get<std::string>();
        }
    }
    return Ok();
}

// §5.2 `--preparation-manifests`: positional, must hash to the ingestion binding; the
// source of §4.3's `dates_with_quarantined_duplicates` / `duplicate_positive_keys` /
// `rejected_rows` (R15-9, I-11).
Status bind_preparation(const SegmentDir &sd, std::map<atx::i32, YearPreparation> &years) {
    ATX_TRY(const auto sha, atx::core::sha256_file(sd.preparation_manifest_path));
    if (sha != sd.preparation_manifest_sha256) {
        return Err(ErrorCode::InvalidArgument,
                   "equity universe: preparation manifest does not hash to the ingestion "
                   "binding: " + sd.preparation_manifest_path);
    }
    ATX_TRY(const auto text, read_file(sd.preparation_manifest_path, kManifestReadBytes));
    const Json doc = strict_json(text);
    if (!doc.contains("daily_counts") || !doc.at("daily_counts").is_array()) {
        return Err(ErrorCode::InvalidArgument,
                   "equity universe: preparation manifest has no daily_counts: " +
                       sd.preparation_manifest_path);
    }
    for (const auto &day : doc.at("daily_counts")) {
        const auto date = day.at("date").get<std::string>();
        const auto key = atx::engine::data::detail::date_to_nanos(date);
        if (!key) {
            return Err(ErrorCode::ParseError,
                       "equity universe: preparation daily_counts date is not YYYY-MM-DD: " + date);
        }
        ATX_TRY(const auto duplicates, count(day.value("duplicate_positive_keys", Json(0))));
        ATX_TRY(const auto rejected, count(day.value("rejected", Json(0))));
        YearPreparation &year = years[data::pit_year_of(*key)];
        if (duplicates > 0) ++year.dates_with_quarantined_duplicates;
        ATX_TRY(year.duplicate_positive_keys, add(year.duplicate_positive_keys, duplicates));
        ATX_TRY(year.rejected_rows, add(year.rejected_rows, rejected));
    }
    return Ok();
}

std::string dir_role(atx::i64 first_key, atx::i64 last_key) {
    const atx::i32 first = data::pit_year_of(first_key);
    const atx::i32 last = data::pit_year_of(last_key);
    std::string role = "segments-" + number(first);
    if (last != first) role += "-" + number(last);
    return role;
}

// §5.5 steps 2-3 (without the budget): enumerate, seal-check, bind, sort, select.
bool type_sha_valid(std::string_view sha) {
    return sha.size() == 64 && std::all_of(sha.begin(), sha.end(), [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}

Result<atx::i64> type_integer(const Json& value) {
    if (!value.is_number_integer() || (value.is_number_unsigned() &&
        value.get<atx::u64>() > static_cast<atx::u64>(std::numeric_limits<atx::i64>::max())))
        return Err(ErrorCode::InvalidArgument, "equity universe: type clock/ID must be a signed integer");
    return Ok(value.get<atx::i64>());
}

Status bind_types(const Profile& profile, Inputs& inputs) {
    inputs.rule = profile.rule;
    if (profile.rule == data::PitUniverseRule::LegacyV1) return Ok();
    ATX_TRY(inputs.types_text, read_file(profile.instrument_types, kManifestReadBytes));
    ATX_TRY(inputs.types_sha256, atx::core::sha256_hex(inputs.types_text));
    const auto doc = strict_json(inputs.types_text);
    if (doc.at("schema") != "atx-instrument-types-v1" || doc.at("status") != "complete" ||
        doc.at("sealed_end_exclusive") != "2020-01-01" ||
        doc.at("clock_rule") != "verified-publication-strict-before-session" ||
        doc.at("validity_rule") != "endpoints-known-at-source-clock" ||
        !doc.at("sources").is_array() || doc.at("sources").size() > data::kPitMaxTypeRecords ||
        !doc.at("rows").is_array() || doc.at("rows").size() > data::kPitMaxTypeRecords)
        return Err(ErrorCode::InvalidArgument, "equity universe: unsupported/unsealed type projection");
    std::set<std::string> sources;
    for (const auto& source : doc.at("sources")) {
        const auto id = source.at("id").get<std::string>();
        if (id.empty() || !sources.insert(id).second ||
            !type_sha_valid(source.at("sha256").get<std::string>()) ||
            source.at("locator").get<std::string>().empty())
            return Err(ErrorCode::InvalidArgument, "equity universe: invalid type source binding");
    }
    constexpr std::string_view names[] = {"unknown", "common-stock", "etf", "adr", "preferred",
                                         "fund", "reit", "limited-partnership", "other"};
    for (const auto& item : doc.at("rows")) {
        data::PitInstrumentTypeEvidence row;
        ATX_TRY(row.security_id, type_integer(item.at("security_id")));
        ATX_TRY(row.valid_from, type_integer(item.at("valid_from_ns")));
        ATX_TRY(row.valid_to, type_integer(item.at("valid_to_ns")));
        ATX_TRY(row.source_published_at, type_integer(item.at("source_published_at_ns")));
        ATX_TRY(row.available_at, type_integer(item.at("available_at_ns")));
        const auto type = item.at("instrument_type").get<std::string>();
        const auto found = std::find(std::begin(names), std::end(names), type);
        if (found == std::end(names) || !sources.contains(item.at("source_id").get<std::string>()) ||
            item.at("evidence_locator").get<std::string>().empty())
            return Err(ErrorCode::InvalidArgument, "equity universe: type row lacks valid classification/provenance");
        row.type = static_cast<data::PitInstrumentType>(found - std::begin(names));
        const auto origin = item.at("source_kind").get<std::string>();
        if (origin != "vendor" && origin != "sec")
            return Err(ErrorCode::InvalidArgument, "equity universe: type source must be vendor or sec");
        row.source = origin == "vendor" ? data::PitTypeSource::Vendor : data::PitTypeSource::Sec;
        row.verified = true;
        for (auto name : {"evidence_status", "availability_status", "vintage_status"}) {
            const auto status = item.at(name).get<std::string>();
            if (status != "verified" && status != "unverified")
                return Err(ErrorCode::InvalidArgument, "equity universe: unknown type qualification status");
            row.verified = row.verified && status == "verified";
        }
        if (item.at("endpoints_known_at_source_clock") != true) row.verified = false;
        row.source_row = static_cast<atx::u32>(inputs.types.size() + 1U);
        inputs.types.push_back(row);
    }
    return Ok(); // builder create validates geometry/clocks before a ledger registration
}

Result<Inputs> enumerate_inputs(const Profile &profile) {
    Inputs inputs;
    ATX_TRY_VOID(bind_types(profile, inputs));
    std::set<atx::i64> seen_keys;
    for (atx::usize d = 0; d < profile.segments_dirs.size(); ++d) {
        SegmentDir sd;
        sd.path = profile.segments_dirs[d];
        sd.preparation_manifest_path = profile.preparation_manifests[d];
        ATX_TRY(auto files, list_segments(fs::path(sd.path), d));
        for (const auto &file : files) {
            if (!seen_keys.insert(file.key).second) {
                return Err(ErrorCode::InvalidArgument,
                           "equity universe: duplicate session date across directories: " +
                               file.filename);
            }
        }
        sd.first_key = files.front().key;
        sd.last_key = files.back().key;
        sd.files = files.size();
        sd.role = dir_role(sd.first_key, sd.last_key);
        ATX_TRY_VOID(bind_ingestion(sd, files));
        ATX_TRY_VOID(bind_preparation(sd, inputs.preparation));
        for (auto &file : files) inputs.files.push_back(std::move(file));
        inputs.dirs.push_back(std::move(sd));
    }
    // §5.2: each directory's date range is disjoint from every other's.
    std::vector<const SegmentDir *> ordered;
    for (const auto &sd : inputs.dirs) ordered.push_back(&sd);
    std::sort(ordered.begin(), ordered.end(),
              [](const SegmentDir *a, const SegmentDir *b) { return a->first_key < b->first_key; });
    for (atx::usize i = 1; i < ordered.size(); ++i) {
        if (ordered[i]->first_key <= ordered[i - 1]->last_key) {
            return Err(ErrorCode::InvalidArgument,
                       "equity universe: segment directory date ranges overlap: " +
                           ordered[i - 1]->path + " and " + ordered[i]->path);
        }
    }
    std::sort(inputs.files.begin(), inputs.files.end(),
              [](const SegmentFile &a, const SegmentFile &b) { return a.key < b.key; });
    for (const auto &file : inputs.files) {
        inputs.keys.push_back(file.key);
        inputs.max_segment_bytes = std::max(inputs.max_segment_bytes, file.size_bytes);
    }
    if (inputs.keys.size() > data::kPitMaxSessions) {
        return Err(ErrorCode::OutOfRange, "equity universe: more sessions than kPitMaxSessions");
    }
    // DR15-8, then the §5.2 guards: end < last attached session, >= 63 sessions at or
    // before the first rank session, at least one rank session.
    if (profile.rank_end >= inputs.keys.back()) {
        return Err(ErrorCode::InvalidArgument,
                   "equity universe: --rank-end must precede the last attached session " +
                       date_text(inputs.keys.back()));
    }
    std::vector<atx::i64> selected(inputs.keys.size());
    atx::usize n = 0;
    ATX_TRY_VOID(data::select_monthly_rank_sessions(inputs.keys, profile.rank_start,
                                                    profile.rank_end, selected, n));
    if (n == 0) {
        return Err(ErrorCode::InvalidArgument,
                   "equity universe: no monthly rank session inside [rank-start, rank-end]");
    }
    selected.resize(n);
    inputs.rank_keys = std::move(selected);
    const auto first =
        std::lower_bound(inputs.keys.begin(), inputs.keys.end(), inputs.rank_keys[0]);
    inputs.first_rank_ordinal = static_cast<atx::usize>(first - inputs.keys.begin());
    if (inputs.first_rank_ordinal + 1 < data::kPitAdvWindow) {
        return Err(ErrorCode::InvalidArgument,
                   "equity universe: fewer than " + number(data::kPitAdvWindow) +
                       " sessions at or before the first rank session " +
                       date_text(inputs.rank_keys[0]));
    }
    return Ok(std::move(inputs));
}

// §7 with the §15.3 T1 deviation-2 scratch: every product overflow-checked.
Status budget_preflight(const Inputs &inputs, atx::u64 max_working_bytes) {
    const atx::u64 U = data::kPitMaxSourceIds;
    const atx::u64 W = data::kPitAdvWindow;
    atx::u64 H = 1;
    while (H < 4 * U) H *= 2;
    const atx::u64 R = inputs.rank_keys.size();
    const atx::u64 C = kCuts;
    const atx::u64 N = kTopN.back();
    const atx::u64 S = inputs.files.size();
    atx::u64 total = 0;
    const auto accumulate = [&total](Result<atx::u64> term) -> Status {
        if (!term) return Err(term.error());
        ATX_TRY(total, add(total, *term));
        return Ok();
    };
    ATX_TRY(const auto uw, multiply(U, W));
    ATX_TRY_VOID(accumulate(multiply(8, uw)));                  // ring
    ATX_TRY_VOID(accumulate(multiply(16, H)));                  // id_table
    ATX_TRY_VOID(accumulate(multiply(48 + 9 * C + C, U)));      // per-slot / per-(slot,cut)
    ATX_TRY(const auto rcn, multiply(R, C));
    ATX_TRY(const auto rcn2, multiply(rcn, N));
    ATX_TRY_VOID(accumulate(multiply(8, rcn2)));                // retained
    ATX_TRY_VOID(accumulate(multiply(16 + 20 * C + 12 + 12 * C, R)));
    ATX_TRY_VOID(accumulate(multiply(12, S)));
    ATX_TRY_VOID(accumulate(multiply(61, U)));                  // scratch
    ATX_TRY_VOID(accumulate(multiply(8, W)));
    ATX_TRY_VOID(accumulate(multiply(20 * C, N)));
    ATX_TRY_VOID(accumulate(multiply(56 + 1 + 1 + 2, U)));      // T1 deviation 2
    ATX_TRY_VOID(accumulate(multiply(8, std::max(S, R))));
    ATX_TRY_VOID(accumulate(Ok(inputs.max_segment_bytes)));
    ATX_TRY_VOID(accumulate(Ok(kManifestReadBytes)));
    ATX_TRY_VOID(accumulate(Ok(2 * kStreamBufferBytes)));
    ATX_TRY_VOID(accumulate(Ok(kMembershipBinReserve)));
    ATX_TRY_VOID(accumulate(Ok(kOverheadReserve)));
    if (inputs.rule == data::PitUniverseRule::CommonStockV2) {
        ATX_TRY_VOID(accumulate(multiply(2 * sizeof(data::PitInstrumentTypeEvidence), inputs.types.size())));
        ATX_TRY_VOID(accumulate(multiply(sizeof(data::PitExcludedRow), U)));
        ATX_TRY_VOID(accumulate(Ok(static_cast<atx::u64>(inputs.types_text.size()))));
        ATX_TRY_VOID(accumulate(Ok(kStreamBufferBytes)));
    }
    if (total > max_working_bytes) {
        return Err(ErrorCode::OutOfRange,
                   "equity universe: memory estimate " + number(total) + " exceeds budget " +
                       number(max_working_bytes));
    }
    return Ok();
}

// §6: the frozen engine configuration; only the two exact counts vary per run.
data::PitUniverseConfig frozen_config(const Inputs &inputs) {
    data::PitUniverseConfig cfg;
    for (atx::usize i = 0; i < kTopN.size(); ++i) cfg.top_n[i] = kTopN[i];
    cfg.top_n_count = kTopN.size();
    for (atx::usize i = 0; i < kBandBp.size(); ++i) cfg.band_bp[i] = kBandBp[i];
    cfg.band_count = kBandBp.size();
    cfg.max_rebalances = inputs.rank_keys.size();
    cfg.max_sessions = inputs.files.size();
    cfg.rule = inputs.rule;
    if (cfg.rule == data::PitUniverseRule::CommonStockV2) {
        cfg.min_adv_usd = data::kPitV2MinAdvUsd;
        cfg.min_raw_price_inclusive = data::kPitV2MinRawPriceInclusive;
        std::copy(inputs.types_sha256.begin(), inputs.types_sha256.end(), cfg.instrument_types_sha256.begin());
    }
    return cfg;
}

// §4.8 / §6: the pre-registration table as JSON (request.json and manifest.json).
Json config_json(const RunConfig &cfg, const Inputs &inputs) {
    Json cuts = Json::array();
    for (atx::usize c = 0; c < kCuts; ++c) cuts.push_back(cut_name(c));
    Json recipe{{"rank_key", kSignalName}, {"rank_key_dsl", kSignalDsl},
                {"adv_window", data::kPitAdvWindow},
                {"min_valid_observations", data::kPitMinValidObservations},
                {"min_raw_price_exclusive", data::kPitMinRawPriceExclusive},
                {"min_adv_usd", data::kPitMinAdvUsd},
                {"bar_on_rank_session_required", true},
                {"missing_bar", "NaN slot; never filled"},
                {"top_n", Json::array({kTopN[0], kTopN[1], kTopN[2]})},
                {"band", Json::array({band_text(kBandBp[0]), band_text(kBandBp[1])})},
                {"band_bp", Json::array({kBandBp[0], kBandBp[1]})}, {"cuts", cuts},
                {"band_rule", "incumbent kept iff rank <= top_n + top_n*band_bp/10000; "
                              "best-ranked top_n when band-kept incumbents exceed top_n"},
                {"tie_break", "key descending, then first-seen slot ascending"},
                {"median", "(a+b)*0.5 binary64; dv = close*volume one product"},
                {"drop_classification", "last_bar < rank ordinal => last_bar; else rank"},
                {"cadence", kCadence}, {"effective", "next observed session"},
                {"alignment", kAlignment},
                {"market_cap", "vendor shares x raw close, no-filing-vintage, reported only"},
                {"instrument_type_eligibility", "unknown"},
                {"historical_availability", "unknown-archive-snapshot"},
                {"raw_data_economics", "unverified"},
                {"max_source_ids", data::kPitMaxSourceIds},
                {"max_rebalances", inputs.rank_keys.size()},
                {"max_sessions", inputs.files.size()},
                {"seal", Json{{"policy", kSealPolicy}, {"validation_begin", kValidationBegin},
                              {"sealed_begin", kSealedBegin}}},
                {"rank_start", cfg.equity_rank_start}, {"rank_end", cfg.equity_rank_end},
                {"max_working_bytes", number(cfg.equity_max_working_bytes)},
                {"trial_count_declared", kTrialCountDeclared}, {"purpose", kPurpose},
                {"live_orders_authorized", false},
                {"investment_qualification", "unverified-no-promotion"}};
    if (inputs.rule == data::PitUniverseRule::CommonStockV2) {
        recipe.erase("min_raw_price_exclusive");
        recipe["rank_key_dsl"] = kSignalDslV2;
        recipe["universe_rule"] = "common-stock-v2";
        recipe["min_raw_price_inclusive"] = data::kPitV2MinRawPriceInclusive;
        recipe["min_adv_usd"] = data::kPitV2MinAdvUsd;
        recipe["instrument_type_eligibility"] = "verified-common-stock-only";
        recipe["instrument_types_sha256"] = inputs.types_sha256;
        recipe["type_clock_rule"] = "verified-publication-strict-before-session";
        recipe["type_validity_rule"] = "endpoints-known-at-source-clock";
        recipe["excluded_type_policy"] = "ETF/ADR/preferred/fund/REIT/LP/other excluded; unknown/unverified/conflict excluded for trading, never inferred common";
        recipe["exclusion_reason_bits"] = Json{{"no_rank_bar", 1}, {"below_price", 2}, {"insufficient_history", 4},
            {"below_adv", 8}, {"type_unknown", 16}, {"type_unverified", 32}, {"type_unavailable", 64},
            {"type_conflict", 128}, {"not_common_stock", 256}};
    }
    return recipe;
}

// ---------------------------------------------------------------------------
//  StreamedCsv — DR15-11 / §5.1: one std::ofstream kept open with a 1 MiB buffer,
//  appended per rebalance, hard-link-published, digested with sha256_file.
// ---------------------------------------------------------------------------
class StreamedCsv {
public:
    StreamedCsv(fs::path directory, std::string name)
        : directory_(std::move(directory)), name_(std::move(name)),
          buffer_(static_cast<atx::usize>(kStreamBufferBytes)) {}

    [[nodiscard]] Status open() {
        partial_ = directory_ / (name_ + ".partial");
        out_.rdbuf()->pubsetbuf(buffer_.data(), static_cast<std::streamsize>(buffer_.size()));
        out_.open(partial_, std::ios::binary);
        if (!out_) return Err(ErrorCode::IoError, "equity universe: cannot open " + name_);
        return Ok();
    }
    void append(std::string_view text) {
        out_.write(text.data(), static_cast<std::streamsize>(text.size()));
    }
    [[nodiscard]] Result<Json> publish() {
        out_.close();
        if (!out_) return Err(ErrorCode::IoError, "equity universe: cannot write " + name_);
        ATX_TRY(auto sha, atx::core::sha256_file(partial_.string()));
        std::error_code ec;
        const auto size = fs::file_size(partial_, ec);
        if (ec) return Err(ErrorCode::IoError, "equity universe: cannot stat " + name_);
        fs::create_hard_link(partial_, directory_ / name_, ec);
        if (ec) return Err(ErrorCode::IoError, "equity universe: cannot publish " + name_);
        fs::remove(partial_, ec);
        if (ec) return Err(ErrorCode::IoError, "equity universe: cannot remove published partial");
        return Ok(Json{{"filename", name_}, {"sha256", sha}, {"size_bytes", size}});
    }

private:
    fs::path directory_;
    std::string name_;
    fs::path partial_;
    std::vector<char> buffer_;
    std::ofstream out_;
};

// ---------------------------------------------------------------------------
//  Per-session feed (§2.2, §2.3, §5.5 step 5): four fields, presence -> NaN,
//  pre-sized buffers, one mapping alive.
// ---------------------------------------------------------------------------
struct SessionBuffers {
    std::vector<atx::i64> ids;
    std::vector<atx::f64> close;
    std::vector<atx::f64> volume;
    std::vector<atx::f64> shares;
    std::vector<atx::f64> gics;
    atx::usize count{};
    explicit SessionBuffers(atx::usize capacity)
        : ids(capacity, 0), close(capacity, 0.0), volume(capacity, 0.0), shares(capacity, 0.0),
          gics(capacity, 0.0) {}
};

Status load_session(const atx::tsdb::SegmentReader &reader, const SegmentFile &file,
                    SessionBuffers &b) {
    const auto times = reader.times();
    // §5.4: exactly one time entry per file and it equals the filename date.
    if (times.size() != 1 || times[0] != file.key) {
        return Err(ErrorCode::InvalidArgument,
                   "equity universe: segment axis must carry exactly the filename date: " +
                       file.filename);
    }
    const auto n = static_cast<atx::usize>(reader.instrument_count());
    if (n > b.ids.size()) {
        return Err(ErrorCode::OutOfRange,
                   "equity universe: segment exceeds max_source_ids: " + file.filename);
    }
    const auto close_id = reader.field_index("close");
    const auto volume_id = reader.field_index("volume");
    const auto shares_id = reader.field_index("shares");
    const auto gics_id = reader.field_index("gics");
    if (!close_id || !volume_id || !shares_id || !gics_id) {
        return Err(ErrorCode::InvalidArgument,
                   "equity universe: segment lacks close/volume/shares/gics: " + file.filename);
    }
    constexpr atx::f64 nan = std::numeric_limits<atx::f64>::quiet_NaN();
    for (atx::usize j = 0; j < n; ++j) {
        const auto inst = static_cast<atx::u32>(j);
        ATX_TRY(b.ids[j], parse_security_id(reader.symbol_name(inst)));
        const bool present = reader.present(0, inst);
        b.close[j] = present ? reader.value(*close_id, 0, inst) : nan;
        b.volume[j] = present ? reader.value(*volume_id, 0, inst) : nan;
        b.shares[j] = present ? reader.value(*shares_id, 0, inst) : nan;
        b.gics[j] = present ? reader.value(*gics_id, 0, inst) : nan;
    }
    b.count = n;
    return Ok();
}

// ---------------------------------------------------------------------------
//  Row writers (§4.1, §4.2): one rebalance, once its effective session is known.
// ---------------------------------------------------------------------------
constexpr std::string_view kMembershipHeader =
    "rebalance_rank_date,effective_date,top_n,band,security_id,rank,adv63_usd,raw_close,"
    "vendor_market_cap_usd,gics,valid_observations,nonmissing_fraction,status\n";
constexpr std::string_view kChurnHeader =
    "rebalance_rank_date,effective_date,top_n,band,adds,drops_rank,drops_last_bar,kept,members,"
    "one_way_turnover\n";

Status write_rebalance_rows(StreamedCsv &membership, StreamedCsv &churn,
                            const data::PitRebalanceView &view, atx::i64 effective_key) {
    const std::string rank_date = date_text(view.rank_session_key);
    const std::string effective_date = date_text(effective_key);
    const auto window = static_cast<atx::f64>(data::kPitAdvWindow);
    std::string row;
    for (atx::usize c = 0; c < view.members.size(); ++c) {
        const std::string prefix = rank_date + "," + effective_date + "," + number(cut_top_n(c)) +
                                   "," + band_text(cut_band_bp(c)) + ",";
        for (const data::PitMemberRow &member : view.members[c]) {
            if (member.rank == 0 || member.rank > view.ranked.size() ||
                view.ranked[member.rank - 1].slot != member.slot) {
                return Err(ErrorCode::Internal,
                           "equity universe: member rank does not index its ranked row");
            }
            const data::PitRankedRow &ranked = view.ranked[member.rank - 1];
            row.clear();
            row += prefix;
            row += number(ranked.security_id) + "," + number(member.rank) + ",";
            row += number(ranked.adv63_usd) + "," + number(ranked.raw_close) + ",";
            row += number(ranked.vendor_market_cap_usd) + "," + gics_text(ranked.gics) + ",";
            row += number(ranked.valid_observations) + ",";
            // DR15-4: the fraction is a writer-side rendering of an integer count.
            row += number(static_cast<atx::f64>(ranked.valid_observations) / window) + ",";
            row += (member.status == data::PitMemberStatus::Keep) ? "keep" : "add";
            row += '\n';
            membership.append(row);
        }
        const data::PitChurn &ch = view.churn[c];
        // §4.2 / T3 pin 7: one binary64 division.
        const atx::u64 moved = static_cast<atx::u64>(ch.adds) + ch.drops_rank + ch.drops_last_bar;
        const atx::f64 turnover =
            static_cast<atx::f64>(moved) / static_cast<atx::f64>(2 * cut_top_n(c));
        row = prefix + number(ch.adds) + "," + number(ch.drops_rank) + "," +
              number(ch.drops_last_bar) + "," + number(ch.kept) + "," + number(ch.members) + "," +
              number(turnover) + "\n";
        churn.append(row);
    }
    return Ok();
}

// ---------------------------------------------------------------------------
//  Aggregate writers (§4.3-§4.6) over the engine's post-run views.
// ---------------------------------------------------------------------------
std::string coverage_csv(std::span<const data::PitCoverageYear> rows,
                         const std::map<atx::i32, YearPreparation> &preparation) {
    std::string out = "year,sessions,dates_with_quarantined_duplicates,duplicate_positive_keys,"
                      "rejected_rows,ids_seen,ids_with_valid_bar_median,rebalances,eligible_median";
    for (const char *stem : {"members_median", "nonmissing_fraction_median",
                             "gics_missing_members_median"}) {
        for (atx::usize c = 0; c < kCuts; ++c) out += "," + std::string(stem) + cut_suffix(c);
    }
    out += '\n';
    for (const auto &row : rows) {
        YearPreparation prep;
        const auto found = preparation.find(row.year);
        if (found != preparation.end()) prep = found->second;
        // T3 pin 5/6: a year with no rebalance leaves every rebalance-derived cell empty.
        const bool any = row.rebalances > 0;
        out += number(row.year) + "," + number(row.sessions) + "," +
               number(prep.dates_with_quarantined_duplicates) + "," +
               number(prep.duplicate_positive_keys) + "," + number(prep.rejected_rows) + "," +
               number(row.ids_seen) + "," + number(row.ids_with_valid_bar_median) + "," +
               number(row.rebalances) + "," + (any ? number(row.eligible_median) : std::string());
        for (atx::usize c = 0; c < kCuts; ++c) {
            out += "," + (any ? number(row.members_median[c]) : std::string());
        }
        for (atx::usize c = 0; c < kCuts; ++c) {
            out += "," + (any ? number(row.nonmissing_fraction_median[c]) : std::string());
        }
        for (atx::usize c = 0; c < kCuts; ++c) {
            out += "," + (any ? number(row.gics_missing_members_median[c]) : std::string());
        }
        out += '\n';
    }
    return out;
}

std::string union_csv(std::span<const data::PitUnionYear> rows) {
    std::string out = "year";
    for (atx::usize c = 0; c < kCuts; ++c) out += ",distinct" + cut_suffix(c);
    for (atx::usize c = 0; c < kCuts; ++c) out += ",cumulative" + cut_suffix(c);
    out += '\n';
    for (const auto &row : rows) {
        out += number(row.year);
        for (atx::usize c = 0; c < kCuts; ++c) out += "," + number(row.distinct[c]);
        for (atx::usize c = 0; c < kCuts; ++c) out += "," + number(row.cumulative[c]);
        out += '\n';
    }
    return out;
}

std::string_view exit_kind_text(data::PitExitKind kind) {
    switch (kind) {
    case data::PitExitKind::RankDrop: return "rank_drop";
    case data::PitExitKind::LastBarWithinWindow: return "last_bar_within_window";
    case data::PitExitKind::WindowEnd: return "window_end";
    }
    return "unknown"; // unreachable for valid enumerators
}

std::string ordinal_date(const data::PitUniverseBuilder &b, atx::u32 ordinal) {
    if (ordinal == data::kPitNoBar || ordinal >= b.sessions()) return std::string();
    return date_text(b.session_key(ordinal));
}

// §4.5 delisting.csv: cut order, then security_id ascending (the engine sorts).
Result<std::string> delisting_csv(const data::PitUniverseBuilder &b) {
    std::string out = "top_n,band,security_id,first_bar,last_bar,first_member_effective_date,"
                      "last_member_rank_date,exit_kind\n";
    std::vector<data::PitExitRecord> records(b.source_ids());
    for (atx::usize c = 0; c < b.cuts(); ++c) {
        atx::usize n = 0;
        ATX_TRY_VOID(b.exits(c, records, n));
        const std::string prefix = number(cut_top_n(c)) + "," + band_text(cut_band_bp(c)) + ",";
        for (atx::usize i = 0; i < n; ++i) {
            const auto &rec = records[i];
            out += prefix + number(rec.security_id) + "," + ordinal_date(b, rec.first_bar) + "," +
                   ordinal_date(b, rec.last_bar) + "," +
                   date_text(b.effective_key(rec.first_member_rebalance)) + "," +
                   date_text(b.rank_key(rec.last_member_rebalance)) + "," +
                   std::string(exit_kind_text(rec.exit_kind)) + "\n";
        }
    }
    return Ok(std::move(out));
}

Json fraction_json(atx::usize numerator, atx::usize denominator) {
    if (denominator == 0) return Json(nullptr);
    return Json(static_cast<atx::f64>(numerator) / static_cast<atx::f64>(denominator));
}

// §4.6 survivorship.json (R15-10).
Json survivorship_json(const data::PitSurvivorship &s, atx::i64 window_end_key) {
    Json cuts = Json::array();
    for (atx::usize c = 0; c < s.cuts; ++c) {
        const auto &cut = s.cut[c];
        Json per_year = Json::array();
        for (atx::usize k = 0; k < cut.year_count; ++k) {
            per_year.push_back(Json{{"year", cut.years[k]},
                                    {"members_at_first_rebalance", cut.year_start_members[k]},
                                    {"exited_within_year", cut.year_exits[k]},
                                    {"fraction", fraction_json(cut.year_exits[k],
                                                               cut.year_start_members[k])}});
        }
        cuts.push_back(Json{{"top_n", cut_top_n(c)}, {"band", band_text(cut_band_bp(c))},
                            {"ever_members", cut.ever_members},
                            {"ended_before_window_end", cut.ended_before_window_end},
                            {"censored", cut.censored},
                            {"fraction_ended",
                             fraction_json(cut.ended_before_window_end, cut.ever_members)},
                            {"per_year", per_year}});
    }
    return Json{{"schema", "atx-equity-universe-survivorship-v1"},
                {"window_end", date_text(window_end_key)}, {"cuts", cuts},
                {"comparison", Json{{"russell_3000_2019_reconstitution_deletions", 157},
                                    {"positions", 3000},
                                    {"deletions_per_year_fraction", 0.0523},
                                    {"source", "nasdaq.com 2019-06-24 (research §D.4)"}}},
                {"caveat", kSurvivorshipCaveat}};
}

// The FNV-1a-64 trailer of membership.bin, read back from its last eight bytes (LE).
atx::u64 bin_trailer(const std::string &bytes) {
    if (bytes.size() < 8) return 0;
    atx::u64 trailer = 0;
    for (atx::usize i = 0; i < 8; ++i) {
        trailer |= static_cast<atx::u64>(static_cast<unsigned char>(bytes[bytes.size() - 8 + i]))
                   << (8 * i);
    }
    return trailer;
}

// ---------------------------------------------------------------------------
//  Trial ledger (§5.6, §5.7). Two lines per run, never one.
// ---------------------------------------------------------------------------
TrialLedgerEntry base_entry(const Profile &profile, const Inputs &inputs,
                            const std::string &trial_id) {
    TrialLedgerEntry entry;
    entry.trial_id = trial_id;
    entry.appended_utc = utc_now();
    entry.checkpoint = kCheckpoint;
    entry.purpose = std::string(kPurpose);
    entry.trial_count_declared = kTrialCountDeclared;
    for (const auto &sd : inputs.dirs) {
        entry.parents.push_back({sd.role, "", sd.ingestion_manifest_sha256});
    }
    entry.parents.push_back({"design-note", "", std::string(kDesignNoteSha256)});
    const auto dsl = profile.rule == data::PitUniverseRule::CommonStockV2 ? kSignalDslV2 : kSignalDsl;
    if (profile.rule == data::PitUniverseRule::CommonStockV2)
        entry.parents.push_back({"instrument-types-v1", "", inputs.types_sha256});
    const auto dsl_sha = atx::core::sha256_hex(dsl);
    entry.recipe.signals.push_back(
        {std::string(kSignalName), std::string(dsl), dsl_sha ? *dsl_sha : std::string()});
    entry.recipe.restrictions.clear();
    for (atx::usize c = 0; c < kCuts; ++c) entry.recipe.restrictions.push_back(cut_name(c));
    entry.recipe.alignment = std::string(kAlignment);
    // window.start = the first effective session; end_exclusive = the day after the
    // last attached session (2020-01-01 in the real run); observations = rebalances.
    entry.window = {date_text(inputs.keys[inputs.first_rank_ordinal + 1]),
                    date_text(inputs.keys.back() + data::kPitNanosPerDay),
                    static_cast<atx::i64>(inputs.rank_keys.size())};
    entry.producer_executable_sha256 = profile.executable.empty() ? "unknown" : profile.executable;
    entry.notes = std::string(kLedgerNotes);
    if (profile.rule == data::PitUniverseRule::CommonStockV2)
        entry.notes += " W1-D5 common-stock-v2 supersedes cp15 eligibility: inclusive $5 and $5m floors, "
                       "strict verified dated type evidence. The old design note remains historical provenance only.";
    return entry;
}

// §5.6: NNNN = 1 + the number of existing pre-registered checkpoint-15 lines (§5.7).
Result<std::string> next_trial_id(const std::string &ledger_path) {
    ATX_TRY(const auto prior, pre_registered_lines_for_checkpoint(ledger_path, kCheckpoint));
    std::string digits = number(prior + 1);
    while (digits.size() < 4U) digits.insert(digits.begin(), '0');
    return Ok(std::string(kTrialIdPrefix) + digits);
}

// ---------------------------------------------------------------------------
//  execute — §5.5 steps 3-6 with the cp14 C-1 body/outer split.
// ---------------------------------------------------------------------------
Result<StageResult> execute(const RunConfig &cfg, const Profile &profile, const Inputs &inputs,
                            Json &attempt, const fs::path &directory, bool &registered) {
    const auto started = std::chrono::steady_clock::now();
    Json files = Json::array();
    ATX_TRY(auto builder, data::PitUniverseBuilder::create(frozen_config(inputs), inputs.types));

    // §5.6 — the pre-registration line goes in BEFORE any computation.
    ATX_TRY(auto trial_id, next_trial_id(profile.ledger_path));
    auto pre = base_entry(profile, inputs, trial_id);
    pre.status = "pre-registered";
    pre.result.outcome = "pending";
    ATX_TRY(auto pre_line, append_trial(profile.ledger_path, pre));
    registered = true;
    ATX_TRY(auto pre_text, serialize_trial_entry(pre_line));
    ATX_TRY(auto pre_sha, atx::core::sha256_hex(pre_text + "\n"));

    // ---- POINT OF NO RETURN (§5.5 step 4, cp14 ruling C-1) -----------------
    // The ledger now carries a "pre-registered" line for this trial_id. Everything
    // below runs inside ONE guarded phase whose every exit — Err, write failure, or
    // exception — falls through to the terminal append at the bottom. No `return`
    // may appear between here and it.
    std::string manifest_sha;
    std::optional<atx::f64> published_seconds;
    Result<StageResult> outcome = [&]() -> Result<StageResult> {
      try {
        Json segments = Json::array();
        for (const auto &file : inputs.files) {
            segments.push_back(Json{{"filename", file.filename}, {"sha256", file.sha256},
                                    {"size_bytes", file.size_bytes}});
        }
        Json dirs = Json::array();
        for (const auto &sd : inputs.dirs) {
            dirs.push_back(Json{{"segments_dir", sd.path}, {"role", sd.role},
                {"first_date", date_text(sd.first_key)}, {"last_date", date_text(sd.last_key)},
                {"files", sd.files},
                {"ingestion_manifest", sd.ingestion_manifest_path},
                {"ingestion_manifest_sha256", sd.ingestion_manifest_sha256},
                {"loader_executable_sha256", sd.loader_executable_sha256},
                {"preparation_manifest", sd.preparation_manifest_path},
                {"preparation_manifest_sha256", sd.preparation_manifest_sha256}});
        }
        attempt["config"] = config_json(cfg, inputs);
        attempt["rebalance_count"] = inputs.rank_keys.size();
        attempt["sessions"] = inputs.files.size();
        attempt["rank_session_first"] = date_text(inputs.rank_keys.front());
        attempt["rank_session_last"] = date_text(inputs.rank_keys.back());
        attempt["latest_attached"] = date_text(inputs.keys.back());
        attempt["segments_dirs"] = std::move(dirs);
        attempt["segments"] = std::move(segments);
        attempt["design_note"] =
            Json{{"path", kDesignNotePath}, {"sha256", kDesignNoteSha256},
                 {"binding", "embedded constant; the runner refuses the run unless the on-disk "
                             "design note hashes to exactly this value"}};
        attempt["producer_executable_sha256"] =
            profile.executable.empty() ? "unknown" : profile.executable;
        attempt["trial_ledger"] = Json{{"path", profile.ledger_path}, {"trial_id", trial_id},
            {"pre_registration_line_sha256", pre_sha},
            {"trial_count_declared", kTrialCountDeclared}, {"purpose", kPurpose}};
        ATX_TRY(auto request, write_text(directory, "request.json", attempt.dump(2) + "\n"));
        files.push_back(std::move(request));

        // §5.4 / §4.8 seal.json: the refusal already happened at enumeration.
        const Json seal{{"policy", kSealPolicy}, {"validation_begin", kValidationBegin},
                        {"sealed_begin", kSealedBegin}, {"segments_refused", 0},
                        {"latest_attached", date_text(inputs.keys.back())},
                        {"statement", "no segment at/after 2020-01-01 was mapped"}};
        ATX_TRY(auto seal_file, write_text(directory, "seal.json", seal.dump(2) + "\n"));
        files.push_back(std::move(seal_file));
        if (inputs.rule == data::PitUniverseRule::CommonStockV2) {
            ATX_TRY(auto types_file, write_text(directory, "instrument_types.json", inputs.types_text));
            files.push_back(std::move(types_file));
        }

        // §3.7 / §5.5 step 5: stream, one mapping alive, rows once effective.
        StreamedCsv membership(directory, "membership.csv");
        StreamedCsv churn(directory, "churn.csv");
        std::unique_ptr<StreamedCsv> excluded;
        std::map<std::pair<atx::i32, atx::u32>, atx::u64> exclusion_counts; // <=16 years *512 reason masks
        if (inputs.rule == data::PitUniverseRule::CommonStockV2) {
            excluded = std::make_unique<StreamedCsv>(directory, "excluded_instruments.csv");
            ATX_TRY_VOID(excluded->open());
            excluded->append("rank_session,effective_session,security_id,reason_bits,instrument_type,source_row,available_at_ns,raw_close,adv_usd,valid_observations,type_projection_sha256\n");
        }
        ATX_TRY_VOID(membership.open());
        ATX_TRY_VOID(churn.open());
        membership.append(kMembershipHeader);
        churn.append(kChurnHeader);
        SessionBuffers buffers(data::kPitMaxSourceIds);
        data::PitRebalanceView pending{};
        bool pending_active = false;
        atx::usize next_rank = 0;
        for (const auto &file : inputs.files) {
            {
                ATX_TRY(const auto reader, atx::tsdb::SegmentReader::attach(file.path.string()));
                ATX_TRY_VOID(load_session(reader, file, buffers));
            }
            const atx::usize n = buffers.count;
            ATX_TRY_VOID(builder.observe_session(file.key,
                std::span<const atx::i64>(buffers.ids.data(), n),
                std::span<const atx::f64>(buffers.close.data(), n),
                std::span<const atx::f64>(buffers.volume.data(), n),
                std::span<const atx::f64>(buffers.shares.data(), n),
                std::span<const atx::f64>(buffers.gics.data(), n)));
            if (pending_active) {
                if (builder.effective_key(pending.index) != file.key) {
                    return Err(ErrorCode::Internal,
                               "equity universe: effective session disagrees with the feed");
                }
                ATX_TRY_VOID(write_rebalance_rows(membership, churn, pending, file.key));
                if (excluded) for (const auto& row : pending.excluded) {
                    ++exclusion_counts[{data::pit_year_of(pending.rank_session_key), row.reasons}];
                    excluded->append(date_text(pending.rank_session_key) + "," + date_text(file.key) + "," +
                        number(row.security_id) + "," + number(row.reasons) + "," +
                        number(static_cast<atx::u32>(row.type)) + "," + number(row.source_row) + "," +
                        number(row.available_at) + "," + number(row.raw_close) + "," + number(row.adv_usd) + "," +
                        number(row.valid_observations) + "," + inputs.types_sha256 + "\n");
                }
                pending_active = false;
            }
            if (next_rank < inputs.rank_keys.size() && inputs.rank_keys[next_rank] == file.key) {
                ATX_TRY(pending, builder.rebalance(file.key));
                pending_active = true;
                ++next_rank;
            }
        }
        if (pending_active) {
            return Err(ErrorCode::InvalidArgument,
                       "equity universe: final rebalance has no effective session");
        }
        if (next_rank != inputs.rank_keys.size()) {
            return Err(ErrorCode::Internal, "equity universe: a rank session was never observed");
        }
        ATX_TRY(auto membership_file, membership.publish());
        files.push_back(std::move(membership_file));
        ATX_TRY(auto churn_file, churn.publish());
        files.push_back(std::move(churn_file));
        if (excluded) {
            ATX_TRY(auto excluded_file, excluded->publish());
            files.push_back(std::move(excluded_file));
            std::string text = "year,reason_bits,security_decisions\n";
            for (const auto& [key, total] : exclusion_counts)
                text += number(key.first) + "," + number(key.second) + "," + number(total) + "\n";
            ATX_TRY(auto counts_file, write_text(directory, "exclusion_counts_by_year.csv", text));
            files.push_back(std::move(counts_file));
        }

        // §5.5 step 6: aggregates, then the small files, manifest last.
        std::vector<data::PitCoverageYear> coverage(data::kPitMaxYears);
        atx::usize coverage_n = 0;
        ATX_TRY_VOID(builder.coverage_by_year(coverage, coverage_n));
        std::vector<data::PitUnionYear> unions(data::kPitMaxYears);
        atx::usize union_n = 0;
        ATX_TRY_VOID(builder.union_by_year(unions, union_n));
        ATX_TRY(const auto survivorship, builder.survivorship());
        ATX_TRY(auto delisting, delisting_csv(builder));
        ATX_TRY(auto bin, data::encode_membership_bin(builder));
        const std::string coverage_text =
            coverage_csv(std::span<const data::PitCoverageYear>(coverage.data(), coverage_n),
                         inputs.preparation);
        const std::string union_text =
            union_csv(std::span<const data::PitUnionYear>(unions.data(), union_n));
        const std::string survivorship_text =
            survivorship_json(survivorship, inputs.keys.back()).dump(2) + "\n";
        for (const auto &[name, text] :
             {std::pair<std::string, const std::string *>{"coverage_by_year.csv", &coverage_text},
              std::pair<std::string, const std::string *>{"union_by_year.csv", &union_text},
              std::pair<std::string, const std::string *>{"delisting.csv", &delisting},
              std::pair<std::string, const std::string *>{"survivorship.json",
                                                          &survivorship_text}}) {
            ATX_TRY(auto file, write_text(directory, name, *text));
            files.push_back(std::move(file));
        }
        ATX_TRY(auto bin_file, write_text(directory, "membership.bin", bin));
        const std::string bin_sha = bin_file.at("sha256").get<std::string>();
        files.push_back(std::move(bin_file));

        Json parents = Json::array();
        for (const auto &file : inputs.files) {
            parents.push_back(Json{{"role", "segment"}, {"filename", file.filename},
                                   {"sha256", file.sha256}, {"size_bytes", file.size_bytes}});
        }
        std::vector<std::string> loaders;
        for (const auto &sd : inputs.dirs) {
            // No `filename|file|name|path` key here (T3 pin 10): the runner's digest walker
            // must not mistake a preparation `manifest.json` for the output manifest.
            parents.push_back(Json{{"role", "ingestion-manifest"}, {"segments_dir", sd.path},
                                   {"ingestion_manifest", sd.ingestion_manifest_path},
                                   {"sha256", sd.ingestion_manifest_sha256},
                                   {"loader_executable_sha256", sd.loader_executable_sha256}});
            parents.push_back(Json{{"role", "preparation-manifest"},
                                   {"preparation_manifest", sd.preparation_manifest_path},
                                   {"sha256", sd.preparation_manifest_sha256}});
            if (std::find(loaders.begin(), loaders.end(), sd.loader_executable_sha256) ==
                loaders.end()) {
                loaders.push_back(sd.loader_executable_sha256);
            }
        }
        parents.push_back(Json{{"role", "design-note"}, {"path", kDesignNotePath},
                               {"sha256", kDesignNoteSha256}});
        const atx::f64 publish_seconds = elapsed_seconds(started);
        Json manifest{{"schema", "atx-equity-universe-v1"}, {"status", "complete"},
            {"config", config_json(cfg, inputs)},
            {"rebalance_count", inputs.rank_keys.size()}, {"sessions", inputs.files.size()},
            {"source_ids", builder.source_ids()},
            {"rank_session_first", date_text(inputs.rank_keys.front())},
            {"rank_session_last", date_text(inputs.rank_keys.back())},
            {"latest_attached", date_text(inputs.keys.back())},
            {"parents", parents},
            {"loader_executables",
             Json{{"sha256", loaders},
                  {"statement", "heterogeneous by construction (R15-9, M-6): the 2012-2013 "
                                "directory and the 2014-2019 directories were loaded by "
                                "different atx-impl executables; each is pinned per directory"}}},
            {"producer_executable_sha256",
             profile.executable.empty() ? "unknown" : profile.executable},
            {"design_note", Json{{"path", kDesignNotePath}, {"sha256", kDesignNoteSha256}}},
            {"trial_ledger", Json{{"path", profile.ledger_path}, {"trial_id", trial_id},
                {"pre_registration_line_sha256", pre_sha},
                {"trial_count_declared", kTrialCountDeclared}, {"purpose", kPurpose},
                {"terminal_line", "appended after this manifest and carries manifest_sha256; "
                                  "its own digest cannot be recorded here"}}},
            {"runtime_seconds", publish_seconds},
            {"runtime", Json{{"runtime_seconds", publish_seconds},
                             {"peak_working_set_bytes", nullptr},
                             {"peak_working_set_source", kPeakWorkingSetSource}}},
            {"membership_bin", Json{{"sha256", bin_sha}, {"byte_length", bin.size()},
                                    {"fnv1a64_trailer", number(bin_trailer(bin))},
                                    {"rebalance_count", inputs.rank_keys.size()}}},
            {"qualifications", Json::array({
                "No alpha, no forecast, no Sharpe, no capacity: membership lists are inputs "
                "to a future measurement.",
                "No float or common-stock eligibility (R15-6): ETFs, ADRs, preferreds and "
                "funds can rank in; gics_missing_members is reported, not applied.",
                "Survivorship fractions are lower bounds (R15-10): backfill policy unknown; "
                "names never in the archive are invisible; no delisting return is imputed.",
                "Vendor shares are unreliable (lag splits, zeros): vendor_market_cap_usd is "
                "no-filing-vintage and must not be used as a size screen.",
                "Quarantined duplicate keys thin the affected sessions; counted in "
                "coverage_by_year.csv, not repaired.",
                "securityID reuse (77622) makes at most a handful of histories composite.",
                "The ledger line carries cp14 recipe defaults declared inapplicable in notes.",
                "No sanitizer, static analyser, include-clean build or CI covers this work.",
                "No live trading and no broker action is performed or authorized."})},
            {"acceptance", "membership lists and counts only; not a forecast, not alpha"},
            {"files", files}};
        if (inputs.rule == data::PitUniverseRule::CommonStockV2) {
            manifest["schema"] = "atx-equity-universe-v2";
            manifest["parents"].push_back(Json{{"role", "instrument-types-v1"}, {"sha256", inputs.types_sha256}});
            manifest["membership_bin"]["version"] = 2;
            manifest["qualifications"][1] = "Verified dated common stock only; unknown/unverified/conflicting type is excluded for trading. No historical type coverage acceptance has been measured.";
            manifest["d5_acceptance"] = Json{{"historical_byte_parity_2013_2015", "unmeasured"},
                {"membership_filter_identity", "unmeasured"}, {"rebuilt_2018_context", "not-built"}};
        }
        ATX_TRY(auto universe_id, atx::core::sha256_hex(
            std::string(inputs.rule == data::PitUniverseRule::CommonStockV2 ? "atx-equity-universe-v2\n" : kDomain) + manifest.dump()));
        manifest["universe_id"] = universe_id;
        // Manifest FIRST, `.pending` last: a failure on the manifest write must leave a
        // directory that still advertises itself as incomplete.
        const auto manifest_text = manifest.dump(2) + "\n";
        ATX_TRY(auto manifest_file, write_text(directory, "manifest.json", manifest_text));
        std::error_code ec;
        if (!fs::remove(directory / ".pending", ec) || ec) {
            return Err(ErrorCode::IoError, "equity universe: cannot release pending marker");
        }
        manifest_sha = manifest_file.at("sha256").get<std::string>();
        published_seconds = publish_seconds;

        StageResult result{};
        result.digest = fnv1a64(universe_id.data(), universe_id.size());
        result.kvs.emplace_back("universe_id", universe_id);
        result.kvs.emplace_back("trial_id", trial_id);
        result.kvs.emplace_back("rebalances", number(inputs.rank_keys.size()));
        result.kvs.emplace_back("sessions", number(inputs.files.size()));
        result.kvs.emplace_back("source_ids", number(builder.source_ids()));
        result.kvs.emplace_back("acceptance", "membership-only-not-a-forecast");
        return Ok(std::move(result));
      } catch (const std::exception &error) {
        // An exception here would otherwise unwind past the terminal append.
        return Err(ErrorCode::IoError, "equity universe: " + std::string(error.what()));
      }
    }();
    // ---- the terminal line, on EVERY path (§5.5 step 4, cp14 C-1) ----------
    // Building the entry allocates strings; an exception here would unwind past the
    // append and leave the pre-registration dangling, so it is confined and reported
    // as its own error (the residual C-1 shape of stage_equity_ic.cpp).
    auto terminal = [&]() -> Result<TrialLedgerEntry> {
      try {
        auto done = base_entry(profile, inputs, trial_id);
        done.runtime.wall_seconds = published_seconds.value_or(elapsed_seconds(started));
        if (outcome) {
            done.status = "completed";
            done.result.outcome = "completed";
            done.result.manifest_sha256 = manifest_sha;
            return Ok(std::move(done));
        }
        done.status = "failed";
        done.result.outcome = "failed";
        // A terminal line needs a digest, so failure.json is published HERE, before the
        // append, rather than by the caller.
        std::string failure_text;
        try {
            attempt["status"] = "failed";
            attempt["error"] = outcome.error().to_string();
            attempt["runtime_seconds"] = done.runtime.wall_seconds.value_or(0.0);
            failure_text = attempt.dump(2) + "\n";
        } catch (const std::exception &) {
            failure_text =
                "{\"schema\":\"atx-equity-universe-request-v1\",\"status\":\"failed\"}\n";
        }
        const auto failure_sha = atx::core::sha256_hex(failure_text);
        if (failure_sha) {
            done.result.failure_sha256 = *failure_sha;
        } else {
            done.result.failure_sha256 = std::string(kTrialLedgerGenesisSha256);
            done.notes += " Failure digest unavailable; the zero digest is a placeholder.";
        }
        // A write failure must not stop the terminal line: the ledger is the artifact
        // that cannot be repaired afterwards.
        try { (void)write_text(directory, "failure.json", failure_text); }
        catch (const std::exception &) { /* the ledger line is the obligation */ }
        return Ok(std::move(done));
      } catch (const std::exception &error) {
        return Err(ErrorCode::Internal,
                   "equity universe: terminal ledger line could not be built: " +
                       std::string(error.what()));
      }
    }();
    if (!terminal) {
        std::string message = terminal.error().to_string() +
                              "; the pre-registration line is left without a terminal line";
        if (!outcome) message += "; original failure: " + outcome.error().to_string();
        try {
            attempt["status"] = "failed";
            attempt["ledger_terminal_error"] = message;
            (void)write_text(directory, "failure.json", attempt.dump(2) + "\n");
        } catch (const std::exception &) { /* the returned error still names it */ }
        return Err(terminal.error().code(), message);
    }
    auto appended = append_trial(profile.ledger_path, *terminal);
    if (!appended) {
        // Nothing further can be done; this error replaces the returned one.
        std::string message =
            "equity universe: completion ledger append failed: " + appended.error().to_string();
        if (!outcome) message += "; original failure: " + outcome.error().to_string();
        try {
            attempt["status"] = "failed";
            attempt["ledger_completion_error"] = appended.error().to_string();
            (void)write_text(directory, "failure.json", attempt.dump(2) + "\n");
        } catch (const std::exception &) { /* the returned error still names it */ }
        return Err(appended.error().code(), message);
    }
    return outcome;
}

} // namespace

Result<StageResult> run_equity_universe(const RunConfig &config) {
    bool reserved = false;
    // Set the instant the pre-registration line lands. Past that point `execute` owns
    // both `failure.json` and the terminal ledger line, so this function must not
    // write a second, digest-less failure.json over it.
    bool registered = false;
    const fs::path directory(config.out);
    Json attempt{{"schema", "atx-equity-universe-request-v1"}, {"status", "started"}};
    auto work = [&]() -> Result<StageResult> {
        ATX_TRY(auto profile, resolve(config));
        ATX_TRY(auto inputs, enumerate_inputs(profile));
        ATX_TRY_VOID(budget_preflight(inputs, profile.max_working_bytes));
        ATX_TRY_VOID(reserve_directory(directory));
        reserved = true;
        return execute(config, profile, inputs, attempt, directory, registered);
    };
    Result<StageResult> result = [&]() -> Result<StageResult> {
        try { return work(); }
        catch (const std::exception &error) {
            return Err(ErrorCode::IoError, "equity universe: " + std::string(error.what()));
        }
    }();
    if (!result && reserved && !registered) {
        attempt["status"] = "failed";
        attempt["error"] = result.error().to_string();
        try { (void)write_text(directory, "failure.json", attempt.dump(2) + "\n"); }
        catch (const std::exception &) { /* Original failure and .pending remain authoritative. */ }
    }
    return result;
}

} // namespace atx::impl
