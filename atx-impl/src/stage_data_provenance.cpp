#include "stage_data_provenance.hpp"

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <utility>

#include <nlohmann/json.hpp>

#include "atx/core/sha256.hpp"
#include "atx/engine/data/orats_history.hpp"
#include "atx/tsdb/segment_reader.hpp"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#endif

namespace atx::impl {
namespace {
namespace fs = std::filesystem;
using Json = nlohmann::json;
using atx::core::Err;
using atx::core::ErrorCode;
using atx::core::Ok;
using atx::core::Result;
using atx::core::Status;
constexpr std::string_view kReceipt = "_ingestion.manifest.json";
constexpr std::string_view kPending = "_ingestion.pending";
constexpr atx::u64 kMaxManifestBytes = 16U * 1024U * 1024U;

bool valid_sha(const std::string &value) {
    return value.size() == 64 && std::all_of(value.begin(), value.end(), [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}

bool basename_only(const std::string &name) {
    return !name.empty() && name != "." && name != ".." &&
           name.find_first_of("/\\:") == std::string::npos &&
           name.find('\0') == std::string::npos;
}

Result<Json> read_json(const fs::path &path) {
    std::error_code ec;
    const auto bytes = fs::file_size(path, ec);
    if (ec || bytes == 0 || bytes > kMaxManifestBytes) {
        return Err(ErrorCode::IoError, "provenance: missing, empty, or oversized manifest: " +
                                          path.string());
    }
    std::ifstream file(path, std::ios::binary);
    std::string body(static_cast<atx::usize>(bytes), '\0');
    if (!file.read(body.data(), static_cast<std::streamsize>(body.size())) ||
        file.peek() != std::char_traits<char>::eof()) {
        return Err(ErrorCode::IoError, "provenance: cannot read stable manifest: " + path.string());
    }
    try {
        std::vector<std::set<std::string>> keys;
        auto value = Json::parse(body, [&keys](int depth, Json::parse_event_t event, Json &item) {
            if (depth > 64) throw std::invalid_argument("manifest nesting exceeds 64");
            if (event == Json::parse_event_t::object_start) keys.emplace_back();
            if (event == Json::parse_event_t::key &&
                (keys.empty() || !keys.back().insert(item.get<std::string>()).second)) {
                throw std::invalid_argument("duplicate manifest object key");
            }
            if (event == Json::parse_event_t::object_end) keys.pop_back();
            return true;
        });
        if (!value.is_object()) {
            return Err(ErrorCode::ParseError, "provenance: expected JSON object: " + path.string());
        }
        return Ok(std::move(value));
    } catch (const Json::exception &error) {
        return Err(ErrorCode::ParseError, "provenance: invalid JSON: " + std::string(error.what()));
    } catch (const std::invalid_argument &error) {
        return Err(ErrorCode::ParseError, "provenance: invalid JSON: " + std::string(error.what()));
    }
}

Result<atx::u64> count(const Json &value) {
    if (value.is_number_unsigned()) return Ok(value.get<atx::u64>());
    if (value.is_number_integer() && value.get<atx::i64>() >= 0) {
        return Ok(static_cast<atx::u64>(value.get<atx::i64>()));
    }
    return Err(ErrorCode::ParseError, "provenance: expected a nonnegative integer count");
}

Result<SourceFileDigest> hash_file(const fs::path &path) {
    std::error_code ec;
    const auto size = fs::file_size(path, ec);
    if (ec) return Err(ErrorCode::IoError, "provenance: cannot stat " + path.string());
    ATX_TRY(auto digest, atx::core::sha256_file(path.string()));
    const auto after = fs::file_size(path, ec);
    if (ec || after != size) {
        return Err(ErrorCode::IoError, "provenance: file changed while hashing: " + path.string());
    }
    return Ok(SourceFileDigest{path.filename().string(), std::move(digest),
                               static_cast<atx::u64>(size)});
}

bool same_file(const SourceFileDigest &a, const SourceFileDigest &b) {
    return a.filename == b.filename && a.sha256 == b.sha256 && a.size_bytes == b.size_bytes;
}

Result<std::vector<fs::path>> segment_paths(const std::string &directory) {
    std::error_code ec;
    std::vector<fs::path> paths;
    fs::directory_iterator iter(directory, ec);
    if (ec) return Err(ErrorCode::IoError, "provenance: cannot list " + directory);
    const fs::directory_iterator end;
    for (; iter != end; iter.increment(ec)) {
        if (ec) return Err(ErrorCode::IoError, "provenance: directory enumeration failed");
        if (iter->path().extension() == ".seg") paths.push_back(iter->path());
    }
    if (ec) return Err(ErrorCode::IoError, "provenance: directory enumeration failed");
    std::sort(paths.begin(), paths.end());
    return Ok(std::move(paths));
}

Result<IngestionInput> validate_preparation(const std::string &path,
                                           const SourceFileDigest &input) {
    ATX_TRY(auto manifest_digest, atx::core::sha256_file(path));
    ATX_TRY(auto doc, read_json(path));
    try {
        const auto policy = doc.at("policy_version").get<std::string>();
        const bool qa2 = policy == "tickerhistory-qa-v2";
        if (doc.at("status") != "complete" ||
            (policy != "tickerhistory-qa-v1" && !qa2) ||
            (!qa2 && doc.at("accepted").at("rows_preserved_byte_for_byte") != true) ||
            doc.at("source").at("crc_verified") != true) {
            return Err(ErrorCode::InvalidArgument, "provenance: incomplete/unsupported preparation");
        }
        const auto accepted_name = doc.at("accepted").at("filename").get<std::string>();
        const auto accepted_sha = doc.at("accepted").at("sha256").get<std::string>();
        ATX_TRY(auto accepted_size, count(doc.at("accepted").at("size_bytes")));
        const auto original_sha = doc.at("source").at("sha256").get<std::string>();
        if (!basename_only(accepted_name) || accepted_name != input.filename ||
            accepted_sha != input.sha256 || accepted_size != input.size_bytes ||
            !valid_sha(original_sha) || doc.at("source").at("member").get<std::string>().empty()) {
            return Err(ErrorCode::InvalidArgument, "provenance: preparation input binding mismatch");
        }
        const auto &counts = doc.at("counts");
        ATX_TRY(auto source, count(counts.at("source_rows")));
        // The preparation tool serializes a Counter: zero rejection/outside counts
        // are absent in valid runs covering the entire source without quarantine.
        ATX_TRY(auto outside, count(counts.value("outside_window_rows", Json(0))));
        ATX_TRY(auto selected, count(counts.at("selected_rows")));
        ATX_TRY(auto accepted, count(counts.at("accepted_rows")));
        ATX_TRY(auto rejected, count(counts.value("rejected_rows", Json(0))));
        const auto start = atx::engine::data::detail::date_to_nanos(
            doc.at("window").at("start_inclusive").get<std::string>());
        const auto end = atx::engine::data::detail::date_to_nanos(
            doc.at("window").at("end_inclusive").get<std::string>());
        if (outside > source || selected != source - outside || accepted > selected ||
            rejected != selected - accepted || !start || !end || *start > *end) {
            return Err(ErrorCode::InvalidArgument, "provenance: invalid preparation counts/window");
        }
        if (qa2) {
            constexpr std::string_view dates[] = {
                "2016-01-15", "2016-02-12", "2016-03-24", "2016-05-27", "2016-07-01",
                "2016-09-02", "2016-11-23", "2016-12-23", "2016-12-30", "2017-01-13",
                "2017-02-17", "2017-04-13", "2017-05-26", "2017-07-03", "2017-09-01",
                "2017-11-22", "2017-12-22", "2018-01-12", "2018-02-16"};
            Json expected = Json::array();
            for (auto date : dates) {
                const auto key = atx::engine::data::detail::date_to_nanos(date);
                if (*start <= *key && *key <= *end) expected.push_back(date);
            }
            ATX_TRY(auto changed, count(doc.at("accepted").at("rows_modified_qa_v2")));
            ATX_TRY(auto unchanged, count(doc.at("accepted").at("unmodified_rows")));
            ATX_TRY(auto rescued, count(counts.value("qa_v2_rescued_rows", Json(0))));
            if (*end >= 1'577'836'800'000'000'000LL || changed > accepted || unchanged != accepted - changed ||
                rescued != changed || doc.at("qa_version") != "v2" ||
                doc.at("accepted").at("rows_preserved_byte_for_byte") != Json(changed == 0) ||
                doc.at("accepted").at("unmodified_rows_preserved_byte_for_byte") != true ||
                doc.at("qa_v2_allowlist_sha256") != "0589dc9ae5c96e68d183820f4733ade7df94245d805e285e43e1bdf29c0fef60" ||
                doc.at("qa_v2_blanked_fields") != Json::array({"open", "high", "low"}) ||
                doc.at("qa_v2_rescuable_reasons") != Json::array({"ohlc_order_violation"}) ||
                doc.at("qa_v2_dates") != expected ||
                !doc.at("qa_v2_daily_counts").is_object() || doc.at("qa_v2_daily_counts").size() != expected.size())
                return Err(ErrorCode::InvalidArgument, "provenance: invalid QA-v2 policy/counts/allowlist");
            atx::u64 total_rescued = 0, total_accepted = 0, total_unchanged = 0;
            for (const auto& date : expected) {
                const auto& daily = doc.at("qa_v2_daily_counts").at(date.get<std::string>());
                ATX_TRY(auto old_rows, count(daily.at("accepted_v1")));
                ATX_TRY(auto new_rows, count(daily.at("accepted_v2_rescued")));
                ATX_TRY(auto all_rows, count(daily.at("accepted")));
                if (new_rows > changed - total_rescued || all_rows > accepted - total_accepted ||
                    old_rows > unchanged - total_unchanged || new_rows > all_rows || old_rows != all_rows - new_rows)
                    return Err(ErrorCode::InvalidArgument, "provenance: inconsistent QA-v2 daily counts");
                total_rescued += new_rows;
                total_accepted += all_rows;
                total_unchanged += old_rows;
            }
            if (total_rescued != changed)
                return Err(ErrorCode::InvalidArgument, "provenance: QA-v2 rescue total mismatch");
        }
        ATX_TRY(auto after, atx::core::sha256_file(path));
        if (after != manifest_digest) {
            return Err(ErrorCode::IoError, "provenance: preparation changed while reading");
        }
        return Ok(IngestionInput{input, std::move(manifest_digest), original_sha,
                                  policy, accepted});
    } catch (const Json::exception &error) {
        return Err(ErrorCode::ParseError, "provenance: preparation schema: " +
                                              std::string(error.what()));
    }
}

Json file_json(const SourceFileDigest &file) {
    return Json{{"filename", file.filename}, {"sha256", file.sha256},
                {"size_bytes", file.size_bytes}};
}

Result<SourceFileDigest> parse_file(const Json &value) {
    const auto name = value.at("filename").get<std::string>();
    const auto sha = value.at("sha256").get<std::string>();
    ATX_TRY(auto size, count(value.at("size_bytes")));
    if (!basename_only(name) || !valid_sha(sha)) {
        return Err(ErrorCode::ParseError, "provenance: invalid file binding");
    }
    return Ok(SourceFileDigest{name, sha, size});
}

Status reserve_output(const std::string &out_dir) {
    std::error_code ec;
    if (fs::exists(out_dir, ec)) {
        if (ec || !fs::is_directory(out_dir, ec) || !fs::is_empty(out_dir, ec) || ec) {
            return Err(ErrorCode::AlreadyExists, "load: provenance requires an empty output directory");
        }
    } else {
        if (ec || !fs::create_directories(out_dir, ec) || ec) {
            return Err(ErrorCode::IoError, "load: cannot create output directory");
        }
    }
    if (!fs::create_directory(fs::path(out_dir) / kPending, ec) || ec) {
        return Err(ErrorCode::AlreadyExists, "load: ingestion directory is already reserved");
    }
    return Ok();
}

Result<std::string> publish_receipt(const fs::path &directory, const Json &doc) {
    const auto final = directory / kReceipt;
    const auto partial = directory / "_ingestion.manifest.json.partial";
    std::error_code ec;
    if (fs::exists(final, ec) || ec || fs::exists(partial, ec) || ec) {
        return Err(ErrorCode::AlreadyExists, "load: ingestion receipt already exists");
    }
    std::ofstream stream(partial, std::ios::binary);
    const std::string bytes = doc.dump(2) + "\n";
    stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    stream.close();
    if (!stream) return Err(ErrorCode::IoError, "load: cannot write ingestion receipt");
    ATX_TRY(auto digest, atx::core::sha256_file(partial.string()));
    if (!fs::remove(directory / kPending, ec) || ec) {
        return Err(ErrorCode::IoError, "load: cannot release ingestion pending marker");
    }
    fs::rename(partial, final, ec);
    if (ec) return Err(ErrorCode::IoError, "load: cannot publish ingestion receipt");
    return Ok(std::move(digest));
}

} // namespace

Result<IngestionInput> begin_ingestion_provenance(const std::string &zip,
                                                 const std::string &out_dir,
                                                 const std::string &preparation_manifest) {
    ATX_TRY(auto file, hash_file(zip));
    IngestionInput input{std::move(file), {}, {}, {}, 0};
    if (!preparation_manifest.empty()) {
        ATX_TRY(input, validate_preparation(preparation_manifest, input.input));
    }
    ATX_TRY_VOID(reserve_output(out_dir));
    return Ok(std::move(input));
}

Result<std::string> finish_ingestion_provenance(const IngestionInput &input,
                                               const std::string &zip,
                                               const std::string &out_dir,
                                               const std::string &preparation_manifest,
                                               const std::string &recipe,
                                               atx::i64 dates_written, atx::i64 rows_read) {
    ATX_TRY(auto after, hash_file(zip));
    if (!same_file(input.input, after)) {
        return Err(ErrorCode::IoError, "load: input ZIP changed during ingestion");
    }
    if (!input.preparation_sha256.empty()) {
        ATX_TRY(auto prep_after, validate_preparation(preparation_manifest, input.input));
        if (prep_after.preparation_sha256 != input.preparation_sha256 || rows_read < 0 ||
            static_cast<atx::u64>(rows_read) != input.prepared_rows) {
            return Err(ErrorCode::InvalidArgument, "load: preparation changed or row count mismatch");
        }
    }
    ATX_TRY(auto paths, segment_paths(out_dir));
    if (dates_written < 0 || static_cast<atx::u64>(dates_written) != paths.size()) {
        return Err(ErrorCode::InvalidArgument, "load: generated segment count mismatch");
    }
    Json segments = Json::array();
    for (const auto &path : paths) {
        ATX_TRY(auto file, hash_file(path));
        segments.push_back(file_json(file));
    }
    Json prep = nullptr;
    if (!input.preparation_sha256.empty()) {
        prep = Json{{"manifest_sha256", input.preparation_sha256},
                    {"original_source_sha256", input.original_source_sha256},
                    {"accepted_sha256", input.input.sha256},
                    {"policy_version", input.preparation_policy}};
    }
    Json doc{{"schema", "atx-ingestion-v1"}, {"status", "complete"},
              {"input", file_json(input.input)}, {"preparation", std::move(prep)},
              {"recipe", recipe}, {"segments", std::move(segments)},
              {"historical_vintages_verified", false},
              {"binding_scope", "local-content-derivation-not-authentication"}};
    return publish_receipt(out_dir, doc);
}

Result<std::vector<SourceFileDigest>> snapshot_panel_sources(const std::string &seg_dir,
                                                            atx::i64 start, atx::i64 end) {
    if (start >= end) return Err(ErrorCode::InvalidArgument, "panel: empty/reversed date window");
    ATX_TRY(auto paths, segment_paths(seg_dir));
    std::vector<SourceFileDigest> selected;
    for (const auto &path : paths) {
        ATX_TRY(auto reader, atx::tsdb::SegmentReader::attach(path.string()));
        const auto times = reader.times();
        if (std::any_of(times.begin(), times.end(), [start, end](atx::i64 t) {
                return t >= start && t < end;
            })) {
            ATX_TRY(auto file, hash_file(path));
            selected.push_back(std::move(file));
        }
    }
    if (selected.empty()) return Err(ErrorCode::InvalidArgument, "panel: no selected segments");
    return Ok(std::move(selected));
}

Result<PanelSourceProvenance> validate_panel_sources(
    const std::string &seg_dir, std::span<const SourceFileDigest> before,
    std::span<const std::string> actual_paths, const std::string &preparation_manifest) {
    if (before.empty() || before.size() != actual_paths.size()) {
        return Err(ErrorCode::InvalidArgument, "panel: selected source set changed during assembly");
    }
    PanelSourceProvenance result;
    for (atx::usize i = 0; i < before.size(); ++i) {
        const fs::path expected = fs::path(seg_dir) / before[i].filename;
        std::error_code ec;
        if (!fs::equivalent(expected, actual_paths[i], ec) || ec) {
            return Err(ErrorCode::InvalidArgument, "panel: selected source path/order mismatch");
        }
        ATX_TRY(auto after, hash_file(expected));
        if (!same_file(before[i], after)) {
            return Err(ErrorCode::IoError, "panel: selected source changed during assembly");
        }
        result.parents.push_back({"segment:" + before[i].filename, before[i].sha256});
    }
    const fs::path receipt_path = fs::path(seg_dir) / kReceipt;
    std::error_code ec;
    if (fs::exists(fs::path(seg_dir) / kPending, ec) || ec ||
        fs::exists(fs::path(seg_dir) / "_ingestion.manifest.json.partial", ec) || ec) {
        return Err(ErrorCode::InvalidArgument, "panel: ingestion is incomplete");
    }
    const bool has_receipt = fs::exists(receipt_path, ec);
    if (ec) return Err(ErrorCode::IoError, "panel: cannot stat ingestion receipt");
    if (!has_receipt) {
        if (!preparation_manifest.empty()) {
            return Err(ErrorCode::InvalidArgument,
                       "panel: preparation manifest needs a verified ingestion receipt");
        }
        return Ok(std::move(result));
    }
    ATX_TRY(auto receipt_sha, atx::core::sha256_file(receipt_path.string()));
    ATX_TRY(auto doc, read_json(receipt_path));
    try {
        if (doc.at("schema") != "atx-ingestion-v1" || doc.at("status") != "complete" ||
            doc.at("historical_vintages_verified") != false ||
            !doc.at("recipe").is_string() || doc.at("recipe").get<std::string>().empty() ||
            !doc.at("segments").is_array()) {
            return Err(ErrorCode::ParseError, "panel: invalid ingestion receipt schema");
        }
        ATX_TRY(auto input, parse_file(doc.at("input")));
        std::map<std::string, SourceFileDigest> files;
        for (const auto &entry : doc.at("segments")) {
            ATX_TRY(auto file, parse_file(entry));
            if (!files.emplace(file.filename, file).second) {
                return Err(ErrorCode::ParseError, "panel: duplicate ingestion segment binding");
            }
        }
        for (const auto &file : before) {
            const auto found = files.find(file.filename);
            if (found == files.end() || !same_file(found->second, file)) {
                return Err(ErrorCode::InvalidArgument, "panel: ingestion segment hash mismatch");
            }
        }
        result.parents.push_back({"ingestion_manifest", receipt_sha});
        result.parents.push_back({"ingestion_input_zip", input.sha256});
        result.ingestion_verified = true;
        const auto &prep = doc.at("preparation");
        if (!prep.is_null()) {
            const auto prep_sha = prep.at("manifest_sha256").get<std::string>();
            const auto original_sha = prep.at("original_source_sha256").get<std::string>();
            if (!valid_sha(prep_sha) || !valid_sha(original_sha) ||
                prep.at("accepted_sha256") != input.sha256 ||
                (prep.at("policy_version") != "tickerhistory-qa-v1" &&
                 prep.at("policy_version") != "tickerhistory-qa-v2")) {
                return Err(ErrorCode::ParseError, "panel: invalid preparation receipt binding");
            }
            if (!preparation_manifest.empty()) {
                ATX_TRY(auto explicit_prep, validate_preparation(preparation_manifest, input));
                if (explicit_prep.preparation_sha256 != prep_sha ||
                    explicit_prep.original_source_sha256 != original_sha ||
                    explicit_prep.preparation_policy != prep.at("policy_version")) {
                    return Err(ErrorCode::InvalidArgument, "panel: preparation manifest mismatch");
                }
            }
            result.parents.push_back({"preparation_manifest", prep_sha});
            result.parents.push_back({"preparation_declared_original_zip", original_sha});
            result.preparation_verified = true;
        } else if (!preparation_manifest.empty()) {
            return Err(ErrorCode::InvalidArgument, "panel: no preparation binding in ingestion");
        }
        ATX_TRY(auto after, atx::core::sha256_file(receipt_path.string()));
        if (after != receipt_sha) {
            return Err(ErrorCode::IoError, "panel: ingestion receipt changed while reading");
        }
    } catch (const Json::exception &error) {
        return Err(ErrorCode::ParseError, "panel: ingestion receipt schema: " +
                                              std::string(error.what()));
    }
    return Ok(std::move(result));
}

Result<std::string> current_executable_sha256() {
    static const Result<std::string> digest = []() -> Result<std::string> {
#if defined(_WIN32)
        std::array<wchar_t, 32768> path{};
        const auto size = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
        if (size == 0 || size >= path.size()) {
            return Err(ErrorCode::IoError, "provenance: cannot resolve running executable");
        }
        return atx::core::sha256_file(fs::path(std::wstring(path.data(), size)).string());
#elif defined(__linux__)
        std::error_code ec;
        const auto path = fs::read_symlink("/proc/self/exe", ec);
        if (ec) return Err(ErrorCode::IoError, "provenance: cannot resolve running executable");
        return atx::core::sha256_file(path.string());
#else
        return Ok(std::string{});
#endif
    }();
    return digest;
}

} // namespace atx::impl
