#include "atx/engine/eval/trial_epoch.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <initializer_list>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <stdexcept>
#include <string_view>
#include <utility>

#include <nlohmann/json.hpp>

#include "atx/core/sha256.hpp"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <io.h>
#else
#include <sys/file.h>
#include <unistd.h>
#endif

namespace atx::engine::eval {
namespace {
using Json = nlohmann::json;
using core::Err;
using core::ErrorCode;
using core::Ok;
using core::Result;
using core::Status;
constexpr usize kMiB = 1024U * 1024U;
const std::string kGenesis(64, '0');
constexpr std::string_view kMagic = "ATXE201\n";

void require(bool valid, const char *message) {
    if (!valid) throw std::invalid_argument(message);
}
bool digest(std::string_view s) {
    return s.size() == 64 && std::all_of(s.begin(), s.end(), [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}
bool safe_id(std::string_view s, usize cap = 256) {
    return !s.empty() && s.size() <= cap && std::all_of(s.begin(), s.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
               (c >= '0' && c <= '9') || c == '_' || c == '-';
    });
}
void keys(const Json &j, std::initializer_list<std::string_view> expected) {
    require(j.is_object() && j.size() == expected.size(), "unexpected object keys");
    for (const auto key : expected) require(j.contains(std::string(key)), "missing object key");
}
std::string string(const Json &j, usize cap) {
    require(j.is_string(), "expected string");
    const auto &s = j.get_ref<const std::string &>();
    require(!s.empty() && s.size() <= cap && s.find('\0') == std::string::npos,
            "invalid string extent");
    return s;
}
usize integer(const Json &j, usize cap) {
    require(j.is_number_unsigned() || j.is_number_integer(), "expected integer");
    if (!j.is_number_unsigned()) require(j.get<i64>() >= 0, "negative count");
    const auto n = j.get<u64>();
    require(n <= cap, "count exceeds limit");
    return static_cast<usize>(n);
}
Json parse(const std::string &s) {
    std::vector<std::set<std::string>> objects;
    return Json::parse(s, [&objects](int depth, Json::parse_event_t event, Json &value) {
        require(depth <= 12, "JSON depth exceeds limit");
        if (event == Json::parse_event_t::object_start) objects.emplace_back();
        if (event == Json::parse_event_t::key) {
            require(!objects.empty() && objects.back().insert(value.get<std::string>()).second,
                    "duplicate JSON key");
        }
        if (event == Json::parse_event_t::object_end) objects.pop_back();
        return true;
    });
}
struct FileCloser { void operator()(std::FILE *f) const noexcept { if (f) std::fclose(f); } };
using File = std::unique_ptr<std::FILE, FileCloser>;
class FileLock {
public:
    FileLock(const FileLock &) = delete;
    FileLock &operator=(const FileLock &) = delete;
    FileLock(FileLock &&o) noexcept : file_(std::exchange(o.file_, nullptr)) {}
    FileLock &operator=(FileLock &&) = delete;
    ~FileLock() {
        if (!file_) return;
#if defined(_WIN32)
        OVERLAPPED ov{};
        (void)::UnlockFileEx(handle(file_), 0, MAXDWORD, MAXDWORD, &ov);
#else
        (void)::flock(::fileno(file_), LOCK_UN);
#endif
    }
    static Result<FileLock> acquire(std::FILE *f) {
#if defined(_WIN32)
        OVERLAPPED ov{};
        if (!::LockFileEx(handle(f), LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY,
                          0, MAXDWORD, MAXDWORD, &ov)) {
#else
        if (::flock(::fileno(f), LOCK_EX | LOCK_NB) != 0) {
#endif
            return Err(ErrorCode::Unavailable, "trial epoch: exclusive catalog lock unavailable");
        }
        return FileLock(f);
    }
private:
    explicit FileLock(std::FILE *f) : file_(f) {}
#if defined(_WIN32)
    static HANDLE handle(std::FILE *f) noexcept {
        // CRT documents this conversion from its OS-handle integer representation.
        return reinterpret_cast<HANDLE>(::_get_osfhandle(::_fileno(f)));
    }
#endif
    std::FILE *file_;
};
Status read(std::FILE *f, char *out, usize size) {
    if (std::fread(out, 1, size, f) != size)
        return Err(ErrorCode::ParseError, "trial epoch: incomplete frame; no repair performed");
    return Ok();
}
Status flush(std::FILE *f) {
    if (std::fflush(f) != 0) return Err(ErrorCode::IoError, "trial epoch: flush failed");
#if defined(_WIN32)
    const auto rc = ::_commit(::_fileno(f));
#else
    const auto rc = ::fsync(::fileno(f));
#endif
    if (rc != 0) return Err(ErrorCode::IoError, "trial epoch: durable sync failed");
    return Ok();
}
struct CellProof { std::string key, family; };
struct AttemptState {
    std::string payload_sha, prereg, trial, reservation_head, recipe_sha;
    std::vector<CellProof> cells;
    usize new_unique{}, retained{};
    int terminal{};
    std::string result_sha;
};
struct State {
    std::map<std::string, AttemptState> attempts;
    std::map<std::string, std::string> trial_tokens;
    std::set<std::string> unique_cells;
    TrialEpochCounts counts;
    std::string head{kGenesis};
    usize events{}, file_bytes{};
};
// Conservative admission model, including map/string/node overhead. JSON parse
// scratch is charged separately at 64x encoded bytes; no unbounded file preload.
usize state_bytes(const State &s) {
    return s.counts.declared_cells * 1024U + s.counts.attempts * 4096U;
}
Status validate_limits(const TrialEpochLimits &l) {
    if (!l.max_file_bytes || l.max_file_bytes > 64U * kMiB ||
        !l.max_record_bytes || l.max_record_bytes > 4U * kMiB ||
        !l.max_attempts || l.max_attempts > 4096 ||
        !l.max_declared_cells || l.max_declared_cells > 65536 ||
        l.max_working_bytes < 4096 || l.max_working_bytes > 1024U * kMiB)
        return Err(ErrorCode::InvalidArgument, "trial epoch: invalid resource limits");
    return Ok();
}
Status room(const State &s, usize encoded_bytes, const TrialEpochLimits &l, usize held_bytes = 0) {
    const auto retained = state_bytes(s);
    if (retained > l.max_working_bytes || held_bytes > l.max_working_bytes - retained ||
        encoded_bytes > (l.max_working_bytes - retained - held_bytes) / 64U)
        return Err(ErrorCode::OutOfRange, "trial epoch: working-memory admission exceeded");
    return Ok();
}

Result<std::string> cell_key(const Json &c, const std::string &recipe_sha) {
    keys(c, {"dsl", "sign", "horizon", "stream_signal_index", "stream_horizon_index", "variant", "restriction", "family_sha256",
             "alias", "retained"});
    const auto dsl = string(c.at("dsl"), 4096);
    require(c.at("sign") == 1 || c.at("sign") == -1, "invalid sign");
    const auto h = integer(c.at("horizon"), 4096);
    require(h > 0, "zero horizon");
    const auto signal_index = integer(c.at("stream_signal_index"), 66);
    const auto horizon_index = integer(c.at("stream_horizon_index"), 7);
    require(c.at("variant") == "DropMissingForward" ||
            c.at("variant") == "IncludeAuditedTerminalV1", "unknown forward variant");
    require(c.at("restriction") == "full" || c.at("restriction") == "_ex34",
            "unknown restriction");
    require(digest(string(c.at("family_sha256"), 64)), "invalid family hash");
    require(safe_id(string(c.at("alias"), 96)), "invalid display alias");
    const Json numerical{{"recipe_sha256", recipe_sha}, {"dsl", dsl},
        {"sign", c.at("sign")}, {"horizon", h}, {"variant", c.at("variant")},
        {"stream_signal_index", signal_index}, {"stream_horizon_index", horizon_index},
        {"restriction", c.at("restriction")}};
    return core::sha256_hex("atx-e2-ic-cell-v1\n" + numerical.dump());
}
Status verify_lineage(const State &s, const Json &ref, const std::string &key) {
    keys(ref, {"prereg_sha256", "family_sha256", "trial_id"});
    const auto trial = string(ref.at("trial_id"), 256);
    const auto prereg = string(ref.at("prereg_sha256"), 64);
    const auto family = string(ref.at("family_sha256"), 64);
    require(safe_id(trial) && digest(prereg) && digest(family), "invalid retained reference");
    const auto previous = s.trial_tokens.find(trial);
    if (previous == s.trial_tokens.end())
        return Err(ErrorCode::InvalidArgument, "trial epoch: retained trial is not catalog-bound");
    const auto &a = s.attempts.at(previous->second);
    if (a.prereg != prereg || !std::any_of(a.cells.begin(), a.cells.end(), [&](const auto &c) {
            return c.key == key && c.family == family;
        })) return Err(ErrorCode::InvalidArgument, "trial epoch: retained numerical cell proof differs");
    return Ok();
}
Status apply_reserve(State &s, const Json &payload, const std::string &head,
                     const TrialEpochLimits &limits) {
    keys(payload, {"token", "trial_id", "prereg_sha256", "numerical_recipe", "cells"});
    const auto token = string(payload.at("token"), 64);
    const auto trial = string(payload.at("trial_id"), 256);
    const auto prereg = string(payload.at("prereg_sha256"), 64);
    require(digest(token) && safe_id(trial) && digest(prereg), "invalid attempt proof");
    require(payload.at("numerical_recipe").is_object() &&
            !payload.at("numerical_recipe").empty(), "empty/nonobject numerical recipe");
    const auto recipe = payload.at("numerical_recipe").dump();
    require(recipe.size() <= 32768, "numerical recipe too large");
    const auto &cells = payload.at("cells");
    require(cells.is_array() && !cells.empty() && cells.size() <= 2048, "invalid cell count");
    if (s.attempts.contains(token) || s.trial_tokens.contains(trial))
        return Err(ErrorCode::InvalidArgument, "trial epoch: duplicate reservation event");
    if (s.counts.attempts >= limits.max_attempts ||
        cells.size() > limits.max_declared_cells - s.counts.declared_cells)
        return Err(ErrorCode::OutOfRange, "trial epoch: declaration/attempt budget exceeded");
    if (cells.size() * 1024U + 4096U > limits.max_working_bytes - state_bytes(s))
        return Err(ErrorCode::OutOfRange, "trial epoch: retained-index budget exceeded");
    AttemptState a;
    a.prereg = prereg;
    a.trial = trial;
    a.reservation_head = head;
    ATX_TRY(a.recipe_sha, core::sha256_hex("atx-e2-numerical-recipe-v1\n" + recipe));
    ATX_TRY(a.payload_sha, core::sha256_hex(payload.dump()));
    a.cells.reserve(cells.size());
    std::set<std::string> attempt_keys;
    for (const auto &c : cells) {
        ATX_TRY(auto key, cell_key(c, a.recipe_sha));
        require(attempt_keys.insert(key).second, "duplicate numerical cell in attempt");
        if (!c.at("retained").is_null()) {
            ATX_TRY_VOID(verify_lineage(s, c.at("retained"), key));
            ++a.retained;
        }
        if (!s.unique_cells.contains(key)) ++a.new_unique;
        a.cells.push_back({std::move(key), c.at("family_sha256").get<std::string>()});
    }
    for (const auto &c : a.cells) s.unique_cells.insert(c.key);
    s.counts.unique_cells = s.unique_cells.size();
    s.counts.declared_cells += cells.size();
    s.counts.verified_retained_cells += a.retained;
    ++s.counts.attempts;
    ++s.counts.incomplete;
    s.trial_tokens.emplace(trial, token);
    s.attempts.emplace(token, std::move(a));
    return Ok();
}
Status apply_terminal(State &s, const Json &p) {
    keys(p, {"token", "status", "result_sha256"});
    const auto token = string(p.at("token"), 64);
    const auto result = string(p.at("result_sha256"), 64);
    const auto status = integer(p.at("status"), 2);
    require(digest(token) && digest(result) && status > 0, "invalid terminal proof");
    const auto found = s.attempts.find(token);
    if (found == s.attempts.end() || found->second.terminal != 0)
        return Err(ErrorCode::InvalidArgument, "trial epoch: unknown/already terminal attempt");
    found->second.terminal = static_cast<int>(status);
    found->second.result_sha = result;
    --s.counts.incomplete;
    if (status == 1) ++s.counts.completed;
    else ++s.counts.failed;
    return Ok();
}
Status apply(State &s, const Json &event, const std::string &hash,
             const std::string &epoch, const TrialEpochLimits &limits) {
    keys(event, {"schema", "epoch", "sequence", "previous_sha256", "kind", "payload"});
    require(event.at("schema") == "atx-trial-epoch-e2-v1" && event.at("epoch") == epoch,
            "catalog epoch/schema differs");
    require(integer(event.at("sequence"), 8192) == s.events + 1 &&
            event.at("previous_sha256") == s.head, "broken catalog chain");
    if (event.at("kind") == "reserve") ATX_TRY_VOID(apply_reserve(s, event.at("payload"), hash, limits));
    else if (event.at("kind") == "terminal") ATX_TRY_VOID(apply_terminal(s, event.at("payload")));
    else return Err(ErrorCode::ParseError, "trial epoch: unknown event kind");
    ++s.events;
    s.head = hash;
    return Ok();
}
Result<State> scan(std::FILE *f, const std::string &epoch, const TrialEpochLimits &limits,
                   usize held_bytes) {
    if (std::fseek(f, 0, SEEK_END) != 0) return Err(ErrorCode::IoError, "trial epoch: seek failed");
    const auto extent = std::ftell(f);
    if (extent < 0 || static_cast<usize>(extent) > limits.max_file_bytes)
        return Err(ErrorCode::OutOfRange, "trial epoch: catalog exceeds size budget");
    if (std::fseek(f, 0, SEEK_SET) != 0) return Err(ErrorCode::IoError, "trial epoch: rewind failed");
    State s;
    const auto bytes = static_cast<usize>(extent);
    while (s.file_bytes < bytes) {
        if (s.events >= limits.max_attempts * 2U || bytes - s.file_bytes < 16U + 65U)
            return Err(ErrorCode::ParseError, "trial epoch: incomplete frame/event limit");
        std::array<char, 16> prefix{};
        ATX_TRY_VOID(read(f, prefix.data(), prefix.size()));
        require(std::string_view(prefix.data(), 8) == kMagic, "invalid frame magic");
        usize length = 0;
        for (usize i = 8; i < 16; ++i) {
            require(prefix[i] >= '0' && prefix[i] <= '9', "invalid frame length");
            length = length * 10U + static_cast<usize>(prefix[i] - '0');
        }
        if (!length || length > limits.max_record_bytes || length > bytes - s.file_bytes - 81U)
            return Err(ErrorCode::ParseError, "trial epoch: incomplete/oversized frame; preserved");
        ATX_TRY_VOID(room(s, length, limits, held_bytes));
        std::string body(length, '\0');
        std::array<char, 65> trailer{};
        ATX_TRY_VOID(read(f, body.data(), body.size()));
        ATX_TRY_VOID(read(f, trailer.data(), trailer.size()));
        ATX_TRY(auto hash, core::sha256_hex(body));
        require(trailer[64] == '\n' && std::string_view(trailer.data(), 64) == hash,
                "frame SHA256 mismatch");
        const auto event = parse(body);
        require(event.dump() == body, "noncanonical event encoding");
        ATX_TRY_VOID(apply(s, event, hash, epoch, limits));
        s.file_bytes += 81U + length;
    }
    return s;
}
Json encode(const TrialEpochAttempt &a) {
    require(digest(a.token) && safe_id(a.trial_id) && digest(a.prereg_sha256), "invalid attempt");
    require(!a.cells.empty() && a.cells.size() <= 2048, "invalid declaration size");
    require(!a.numerical_recipe_json.empty() && a.numerical_recipe_json.size() <= 32768,
            "recipe extent exceeds limit");
    const auto recipe = parse(a.numerical_recipe_json);
    require(recipe.is_object() && recipe.dump() == a.numerical_recipe_json,
            "recipe must be canonical object JSON");
    Json cells = Json::array();
    for (const auto &c : a.cells) {
        require(c.canonical_dsl.size() <= 4096 && c.display_alias.size() <= 96 &&
                c.family_sha256.size() == 64, "cell extent exceeds limit");
        Json ref = nullptr;
        if (c.retained) ref = Json{{"prereg_sha256", c.retained->prereg_sha256},
            {"family_sha256", c.retained->family_sha256}, {"trial_id", c.retained->trial_id}};
        cells.push_back(Json{{"dsl", c.canonical_dsl}, {"sign", c.sign}, {"horizon", c.horizon},
            {"stream_signal_index", c.stream_signal_index}, {"stream_horizon_index", c.stream_horizon_index},
            {"variant", c.forward_variant}, {"restriction", c.restriction},
            {"family_sha256", c.family_sha256}, {"alias", c.display_alias}, {"retained", ref}});
    }
    return Json{{"token", a.token}, {"trial_id", a.trial_id}, {"prereg_sha256", a.prereg_sha256},
                {"numerical_recipe", recipe}, {"cells", std::move(cells)}};
}
} // namespace

struct TrialEpochCatalog::Impl {
    File file;
    std::string epoch, expected_head;
    TrialEpochLimits limits;
    State state;
    bool poisoned{};

    Status refresh() {
        if (poisoned) return Err(ErrorCode::IoError, "trial epoch: failed write; reopen and inspect");
        // Preserve the prior acknowledged snapshot on read/anchor error; its
        // storage coexists with the replay index and must be charged as well.
        ATX_TRY(auto next, scan(file.get(), epoch, limits, state_bytes(state)));
        if (next.head != expected_head)
            return Err(ErrorCode::InvalidArgument, "trial epoch: external anchor/head mismatch");
        state = std::move(next);
        return Ok();
    }
    Status append(const std::string &kind, const Json &payload) {
        Json event{{"schema", "atx-trial-epoch-e2-v1"}, {"epoch", epoch},
                   {"sequence", state.events + 1}, {"previous_sha256", state.head},
                   {"kind", kind}, {"payload", payload}};
        const auto body = event.dump();
        if (body.size() > limits.max_record_bytes || state.file_bytes > limits.max_file_bytes ||
            body.size() + 81U > limits.max_file_bytes - state.file_bytes)
            return Err(ErrorCode::OutOfRange, "trial epoch: record/file budget exceeded");
        ATX_TRY_VOID(room(state, body.size(), limits));
        // One copy allows validation/allocation to finish before durable mutation.
        if (state_bytes(state) > (limits.max_working_bytes - body.size() * 64U) / 2U)
            return Err(ErrorCode::OutOfRange, "trial epoch: transactional-index budget exceeded");
        State next = state;
        ATX_TRY(auto hash, core::sha256_hex(body));
        ATX_TRY_VOID(apply(next, event, hash, epoch, limits));
        auto length = std::to_string(body.size());
        length.insert(0, 8 - length.size(), '0');
        const auto frame = std::string(kMagic) + length + body + hash + "\n";
        poisoned = true; // Any partial write/sync failure requires explicit inspection.
        if (std::fseek(file.get(), 0, SEEK_END) != 0 ||
            std::fwrite(frame.data(), 1, frame.size(), file.get()) != frame.size())
            return Err(ErrorCode::IoError, "trial epoch: append failed; bytes preserved");
        ATX_TRY_VOID(flush(file.get()));
        next.file_bytes += frame.size();
        state = std::move(next);
        expected_head = hash;
        poisoned = false;
        return Ok();
    }
};

TrialEpochCatalog::TrialEpochCatalog(std::unique_ptr<Impl> p) : impl_(std::move(p)) {}
TrialEpochCatalog::TrialEpochCatalog(TrialEpochCatalog &&) noexcept = default;
TrialEpochCatalog &TrialEpochCatalog::operator=(TrialEpochCatalog &&) noexcept = default;
TrialEpochCatalog::~TrialEpochCatalog() = default;

Result<TrialEpochCatalog> TrialEpochCatalog::open(const std::string &path, const std::string &epoch,
                                                const std::string &anchor, TrialEpochLimits limits) {
    ATX_TRY_VOID(validate_limits(limits));
    if (path.empty() || !safe_id(epoch, 96) || !digest(anchor))
        return Err(ErrorCode::InvalidArgument, "trial epoch: path/epoch/external anchor required");
    try {
        auto p = std::make_unique<Impl>();
#if defined(_WIN32)
        std::FILE *raw = nullptr;
        if (::_wfopen_s(&raw, std::filesystem::path(path).c_str(), L"a+b") != 0) raw = nullptr;
#else
        auto *raw = std::fopen(path.c_str(), "a+b");
#endif
        p->file.reset(raw);
        if (!p->file) return Err(ErrorCode::IoError, "trial epoch: cannot open catalog");
        p->epoch = epoch;
        p->expected_head = anchor;
        p->limits = limits;
        ATX_TRY(auto lock, FileLock::acquire(p->file.get()));
        ATX_TRY_VOID(p->refresh());
        return TrialEpochCatalog(std::move(p));
    } catch (const std::exception &e) {
        return Err(ErrorCode::ParseError, "trial epoch: " + std::string(e.what()));
    }
}
Result<TrialEpochReservation> TrialEpochCatalog::reserve_attempt(const TrialEpochAttempt &a) {
    if (!impl_) return Err(ErrorCode::InvalidArgument, "trial epoch: moved-from handle");
    try {
        ATX_TRY(auto lock, FileLock::acquire(impl_->file.get()));
        ATX_TRY_VOID(impl_->refresh());
        // Bound caller-controlled strings/JSON before building the encoded record.
        if (a.cells.empty() || a.cells.size() > 2048 || a.numerical_recipe_json.size() > 32768 ||
            a.token.size() != 64 || a.prereg_sha256.size() != 64 || a.trial_id.size() > 256)
            return Err(ErrorCode::OutOfRange, "trial epoch: caller declaration extent exceeded");
        usize estimate = a.numerical_recipe_json.size() + 1024U;
        for (const auto &c : a.cells) {
            if (c.canonical_dsl.size() > 4096 || c.forward_variant.size() > 64 ||
                c.restriction.size() > 16 || c.display_alias.size() > 96 || c.family_sha256.size() != 64 ||
                (c.retained && (c.retained->prereg_sha256.size() != 64 ||
                    c.retained->family_sha256.size() != 64 || c.retained->trial_id.size() > 256)))
                return Err(ErrorCode::OutOfRange, "trial epoch: caller cell extent exceeded");
            const auto bytes = c.canonical_dsl.size() + 2048U;
            if (bytes > impl_->limits.max_record_bytes || estimate > impl_->limits.max_record_bytes - bytes)
                return Err(ErrorCode::OutOfRange, "trial epoch: declaration encoding budget exceeded");
            estimate += bytes;
        }
        ATX_TRY_VOID(room(impl_->state, estimate, impl_->limits));
        const auto payload = encode(a);
        ATX_TRY(auto payload_sha, core::sha256_hex(payload.dump()));
        const auto found = impl_->state.attempts.find(a.token);
        const bool inserted = found == impl_->state.attempts.end();
        if (!inserted && found->second.payload_sha != payload_sha)
            return Err(ErrorCode::InvalidArgument, "trial epoch: token reused for a different attempt");
        if (inserted) ATX_TRY_VOID(impl_->append("reserve", payload));
        const auto &saved = impl_->state.attempts.at(a.token);
        return TrialEpochReservation{inserted, saved.new_unique, saved.retained,
            saved.reservation_head, saved.recipe_sha, impl_->state.counts};
    } catch (const std::exception &e) {
        return Err(ErrorCode::InvalidArgument, "trial epoch: " + std::string(e.what()));
    }
}
Status TrialEpochCatalog::finish_attempt(const std::string &token, TrialEpochTerminal terminal,
                                        const std::string &result_sha256) {
    if (!impl_ || !digest(token) || !digest(result_sha256) ||
        (terminal != TrialEpochTerminal::Completed && terminal != TrialEpochTerminal::Failed))
        return Err(ErrorCode::InvalidArgument, "trial epoch: invalid terminal arguments");
    try {
        ATX_TRY(auto lock, FileLock::acquire(impl_->file.get()));
        ATX_TRY_VOID(impl_->refresh());
        const auto found = impl_->state.attempts.find(token);
        if (found == impl_->state.attempts.end())
            return Err(ErrorCode::NotFound, "trial epoch: unknown reservation token");
        const int status = static_cast<int>(terminal);
        if (found->second.terminal) {
            if (found->second.terminal == status && found->second.result_sha == result_sha256) return Ok();
            return Err(ErrorCode::InvalidArgument, "trial epoch: conflicting terminal event");
        }
        return impl_->append("terminal", Json{{"token", token}, {"status", status},
                                              {"result_sha256", result_sha256}});
    } catch (const std::exception &e) {
        return Err(ErrorCode::InvalidArgument, "trial epoch: " + std::string(e.what()));
    }
}
TrialEpochCounts TrialEpochCatalog::counts() const noexcept { return impl_ ? impl_->state.counts : TrialEpochCounts{}; }
const std::string &TrialEpochCatalog::head_sha256() const noexcept { return impl_ ? impl_->state.head : kGenesis; }
} // namespace atx::engine::eval
