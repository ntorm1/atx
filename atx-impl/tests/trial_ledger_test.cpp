#include <atomic>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "atx/core/error.hpp"
#include "atx/core/sha256.hpp"
#include "trial_ledger.hpp"

namespace {
namespace fs = std::filesystem;
namespace impl = atx::impl;
using Json = nlohmann::json;
using atx::core::ErrorCode;

std::string contents(const fs::path &path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

void overwrite(const fs::path &path, const std::string &blob) {
    std::ofstream out(path, std::ios::binary);
    out.write(blob.data(), static_cast<std::streamsize>(blob.size()));
    out.close();
    ASSERT_TRUE(out) << "cannot rewrite " << path.string();
}

std::vector<std::string> split_lines(const std::string &blob) {
    std::vector<std::string> lines;
    for (std::size_t start = 0; start < blob.size();) {
        const auto stop = blob.find('\n', start);
        if (stop == std::string::npos) break;
        lines.push_back(blob.substr(start, stop - start));
        start = stop + 1;
    }
    return lines;
}

// Regenerate the sidecar so it agrees with whatever the ledger now contains. Used only
// to construct the ONE documented undetectable case (tail edit + consistent sidecar).
void regenerate_sidecar(const std::string &ledger_path, const std::string &created) {
    const auto lines = split_lines(contents(ledger_path));
    std::string head(64, '0');
    if (!lines.empty()) {
        const auto digest = atx::core::sha256_hex(lines.back() + "\n");
        ASSERT_TRUE(digest.has_value());
        head = *digest;
    }
    const std::string body = "{\"lines\":" + std::to_string(lines.size()) +
                             ",\"head_sha256\":\"" + head + "\",\"created_utc\":\"" + created +
                             "\"}\n";
    ASSERT_NO_FATAL_FAILURE(overwrite(impl::trial_ledger_manifest_path(ledger_path), body));
}

// The frozen §4.5 pre-registration shape: every recipe / source-exclusion constant comes
// from the struct defaults, so this fixture supplies only the caller-owned fields.
impl::TrialLedgerEntry pre_registered(const std::string &trial_id) {
    impl::TrialLedgerEntry entry{};
    entry.trial_id = trial_id;
    entry.appended_utc = "2026-09-20T00:00:00Z";
    entry.prev_sha256 = std::string(64, '0');
    entry.checkpoint = 14;
    entry.purpose = "training-only-forecast-evaluation";
    entry.status = "pre-registered";
    entry.trial_count_declared = 30;
    entry.parents = {{"source-context", "ec572b82", ""},
                     {"design-note", "", std::string(64, 'a')}};
    entry.recipe.signals = {{"momentum_252",
                             "ts_mean(delay(close, 21) / delay(close, 252) - 1, 5)",
                             std::string(64, 'b')}};
    entry.window = {"2013-04-04", "2014-01-01", 189};
    entry.source_exclusions.ex34_restriction_ids = {"37648", "35715"};
    entry.producer_executable_sha256 = std::string(64, 'c');
    entry.notes = "Stage 1 of D-1; sign-and-shape only; not accepted alpha.";
    return entry;
}

impl::TrialLedgerEntry completed(const std::string &trial_id) {
    impl::TrialLedgerEntry entry = pre_registered(trial_id);
    entry.status = "completed";
    entry.runtime.wall_seconds = 12.5;
    entry.runtime.peak_working_set_bytes = atx::u64{1026482176};
    entry.result.outcome = "completed";
    entry.result.manifest_sha256 = std::string(64, 'd');
    return entry;
}

class TrialLedger : public ::testing::Test {
protected:
    fs::path root;

    void SetUp() override {
        static std::atomic<unsigned> sequence{};
        std::error_code ec;
        for (unsigned attempt = 0; attempt < 1000; ++attempt) {
            root = fs::temp_directory_path() /
                ("atx_trial_ledger_" + std::to_string(sequence.fetch_add(1)));
            if (fs::create_directory(root, ec)) return;
        }
        FAIL() << "Cannot reserve fixture root";
    }

    void TearDown() override {
        std::error_code ec;
        const auto parent = fs::weakly_canonical(fs::temp_directory_path(), ec);
        if (ec) return;
        const auto resolved = fs::weakly_canonical(root, ec);
        if (!ec && resolved.parent_path() == parent &&
            resolved.filename().string().starts_with("atx_trial_ledger_")) {
            fs::remove_all(resolved, ec);
        }
    }

    [[nodiscard]] std::string ledger() const { return (root / "trial-ledger.jsonl").string(); }

    // Append three lines: the pre-registration, its completion, then a retry's
    // pre-registration (§4.5: "A failed run keeps both lines; the retry is …-0002").
    void seed_three_lines() {
        ASSERT_TRUE(impl::append_trial(ledger(), pre_registered("cp14-0001")).has_value());
        ASSERT_TRUE(impl::append_trial(ledger(), completed("cp14-0001")).has_value());
        ASSERT_TRUE(impl::append_trial(ledger(), pre_registered("cp14-0002")).has_value());
    }
};

// The pre-registered line is a pre-commitment, so its exact bytes are pinned here: any
// drift in key order, key set, number formatting or a frozen §4.5 constant breaks this.
TEST_F(TrialLedger, SerializesAPreRegisteredLineByteExactly) {
    const std::string expected =
        std::string{"{\"schema\":\"atx-trial-ledger-v1\""} +
        ",\"trial_id\":\"iteration14-cross-section-ic-0001\"" +
        ",\"appended_utc\":\"2026-09-20T00:00:00Z\"" +
        ",\"prev_sha256\":\"" + std::string(64, '0') + "\"" +
        ",\"checkpoint\":14" +
        ",\"purpose\":\"training-only-forecast-evaluation\"" +
        ",\"status\":\"pre-registered\"" +
        ",\"trial_count_declared\":30" +
        ",\"parents\":[{\"role\":\"source-context\",\"artifact_id\":\"ec572b82\"}," +
        "{\"role\":\"design-note\",\"sha256\":\"" + std::string(64, 'a') + "\"}]" +
        ",\"recipe\":{\"signals\":[{\"name\":\"momentum_252\"," +
        "\"dsl\":\"ts_mean(delay(close, 21) / delay(close, 252) - 1, 5)\"," +
        "\"dsl_sha256\":\"" + std::string(64, 'b') + "\"}]" +
        ",\"horizons\":[1,5,10,21,63]" +
        ",\"quantiles\":10" +
        ",\"bootstrap\":{\"draws\":2000,\"seed\":20260920," +
        "\"block_len_rule\":\"max(5, ceil(h/2))\",\"block_lens\":[5,5,5,11,32]," +
        "\"percentiles\":[2.5,97.5]," +
        "\"reportable_rule\":\"draws>=1 && n>=20 && floor(n/L)>=10\"," +
        "\"statistic_ids\":{\"IcMean\":0,\"Icir\":1,\"RankIcMean\":2,\"RankIcir\":3," +
        "\"SpreadGross\":4,\"SpreadNet\":5}," +
        "\"stream_key\":\"seed ^ (stat<<8) ^ (horizon_index<<16) ^ (signal<<24)" +
        " ^ (variant_id<<32) ^ (restriction_id<<40) ^ (sample_id<<48)\"," +
        "\"sample_ids\":{\"full\":0,\"common\":1},\"predicted_null_horizons\":[63]}" +
        ",\"autocorr_lags\":[1]" +
        ",\"turnover_source\":\"rho_rank\"" +
        ",\"forward_variants\":[\"DropMissingForward\",\"IncludeAuditedTerminalV1\"]" +
        ",\"restrictions\":[\"full\",\"_ex34\"]" +
        ",\"samples\":[\"full\",\"_common\"]" +
        ",\"common_sample_rule\":\"prefix t < (T - max(H)); incomplete prefix =>" +
        " _common unreportable\"" +
        ",\"alignment\":\"signal-at-t-return-from-t-deployed-book-executes-at-t-plus-1\"" +
        ",\"net_spread_rule\":\"per-date net_h(t) = gross_h(t) - cost_drag_h(t);" +
        " no rebalance grid\"" +
        ",\"cost\":{\"trade_bps\":5.0," +
        "\"trade_bps_source\":\"constexpr EquityAllocationConfig{}.trade_bps\"," +
        "\"annual_borrow_bps\":365.0," +
        "\"annual_borrow_bps_source\":\"deployed equity-book run config (handoff)\"," +
        "\"short_leg_gross\":1.0,\"priced_book_gross\":2.0," +
        "\"decile_weights\":\"+1.0/n_top, -1.0/n_bottom\"," +
        "\"borrow_day_convention\":\"calendar_days_from_session_keys\"," +
        "\"borrow_day_formula\":\"(session_keys[t+h]-session_keys[t])/86400000000000" +
        " integer division\"," +
        "\"provenance\":\"constants-restated-not-a-replay-call; replay.cpp borrow_charge" +
        " is file-local\"}" +
        ",\"seal\":{\"policy\":\"RejectSealedV1\",\"validation_begin\":\"2020-01-01\"," +
        "\"sealed_begin\":\"2023-01-01\"}}" +
        ",\"window\":{\"start\":\"2013-04-04\",\"end_exclusive\":\"2014-01-01\"," +
        "\"observations\":189}" +
        ",\"source_exclusions\":{\"required_mark_id_count\":34," +
        "\"terminal_hypothesis_ids\":[37648,35715,39970]," +
        "\"terminal_hypothesis_note\":\"terminal-cash-event hypothesis per the audit's" +
        " wording, not a settled classification\"," +
        "\"terminal_evidenced_record_dates\":{\"35715\":\"2013-10-28\"}," +
        "\"evidenced_non_terminal_ids\":[150340,351548],\"unclassified_id_count\":29," +
        "\"terminal_unevidenced_ids\":[146189]," +
        "\"ex34_restriction_ids\":[\"37648\",\"35715\"]," +
        "\"pcs_statement\":\"PCS is never applied; admission remains rejected\"}" +
        ",\"fit_boundary\":{\"fit_kind\":\"unfit-no-fitting-performed\"," +
        "\"fitted_observations\":0}" +
        ",\"runtime\":{\"wall_seconds\":null,\"peak_working_set_bytes\":null}" +
        ",\"result\":{\"outcome\":\"pending\",\"manifest_sha256\":null," +
        "\"failure_sha256\":null}" +
        ",\"producer_executable_sha256\":\"" + std::string(64, 'c') + "\"" +
        ",\"notes\":\"Stage 1 of D-1; sign-and-shape only; not accepted alpha.\"}";

    const auto line = impl::serialize_trial_entry(
        pre_registered("iteration14-cross-section-ic-0001"));
    ASSERT_TRUE(line.has_value()) << line.error().to_string();
    EXPECT_EQ(*line, expected);
    EXPECT_EQ(line->find('\n'), std::string::npos);
    EXPECT_NO_THROW((void)Json::parse(*line));
}

TEST_F(TrialLedger, MissingLedgerIsAValidEmptyChain) {
    const auto head = impl::verify_trial_ledger(ledger());
    ASSERT_TRUE(head.has_value()) << head.error().to_string();
    EXPECT_EQ(head->lines, 0u);
    EXPECT_EQ(head->head_sha256, std::string(64, '0'));
}

TEST_F(TrialLedger, ThreeAppendsFormAVerifiableChain) {
    ASSERT_NO_FATAL_FAILURE(seed_three_lines());

    const auto head = impl::verify_trial_ledger(ledger());
    ASSERT_TRUE(head.has_value()) << head.error().to_string();
    EXPECT_EQ(head->lines, 3u);

    const std::string blob = contents(ledger());
    ASSERT_FALSE(blob.empty());
    EXPECT_EQ(blob.back(), '\n');
    EXPECT_EQ(blob.find('\r'), std::string::npos);

    std::vector<std::string> lines;
    for (std::size_t start = 0; start < blob.size();) {
        const auto stop = blob.find('\n', start);
        lines.push_back(blob.substr(start, stop - start));
        start = stop + 1;
    }
    ASSERT_EQ(lines.size(), 3u);

    // §4.5: each prev_sha256 is the SHA-256 of the previous line's bytes INCLUDING its LF.
    std::string previous(64, '0');
    for (std::size_t index = 0; index < lines.size(); ++index) {
        EXPECT_EQ(Json::parse(lines[index])["prev_sha256"].get<std::string>(), previous)
            << "link " << index;
        const auto digest = atx::core::sha256_hex(lines[index] + "\n");
        ASSERT_TRUE(digest.has_value());
        previous = *digest;
    }
    EXPECT_EQ(head->head_sha256, previous);
    EXPECT_EQ(Json::parse(lines[1])["status"].get<std::string>(), "completed");
}

TEST_F(TrialLedger, TamperedMiddleLineBreaksTheChainAtTheFollowingIndex) {
    ASSERT_NO_FATAL_FAILURE(seed_three_lines());
    std::string blob = contents(ledger());
    const auto at = blob.find("Stage 1 of D-1", blob.find('\n') + 1);
    ASSERT_NE(at, std::string::npos);
    blob[at + 6] = '2'; // one byte of line 1, outside its own prev_sha256 field
    ASSERT_NO_FATAL_FAILURE(overwrite(ledger(), blob));

    const auto head = impl::verify_trial_ledger(ledger());
    ASSERT_FALSE(head.has_value());
    EXPECT_EQ(head.error().code(), ErrorCode::Internal);
    EXPECT_NE(head.error().message().find("line 2"), std::string::npos)
        << head.error().message();
}

// A torn append is detected, never repaired; the message must carry the exact offset a
// human truncates to (ruling R-3), or the documented manual repair is unusable.
TEST_F(TrialLedger, TruncatedFinalLineIsDetectedAsParseErrorNamingTheRepairOffset) {
    ASSERT_NO_FATAL_FAILURE(seed_three_lines());
    std::string blob = contents(ledger());
    const std::size_t keep = blob.rfind('\n', blob.size() - 2) + 1;
    blob.resize(blob.size() - 40); // drop the LF and the tail of the last line
    ASSERT_NO_FATAL_FAILURE(overwrite(ledger(), blob));

    const auto head = impl::verify_trial_ledger(ledger());
    ASSERT_FALSE(head.has_value());
    EXPECT_EQ(head.error().code(), ErrorCode::ParseError);
    const std::string message = head.error().message();
    EXPECT_NE(message.find("truncated"), std::string::npos) << message;
    EXPECT_NE(message.find("byte offset " + std::to_string(keep)), std::string::npos) << message;

    // The named offset is exact: truncating there and regenerating the sidecar repairs it.
    blob.resize(keep);
    ASSERT_NO_FATAL_FAILURE(overwrite(ledger(), blob));
    ASSERT_NO_FATAL_FAILURE(regenerate_sidecar(ledger(), "2026-09-20T00:00:00Z"));
    const auto repaired = impl::verify_trial_ledger(ledger());
    ASSERT_TRUE(repaired.has_value()) << repaired.error().to_string();
    EXPECT_EQ(repaired->lines, 2u);
}

TEST_F(TrialLedger, AppendAfterABrokenChainIsRefused) {
    ASSERT_NO_FATAL_FAILURE(seed_three_lines());
    std::string blob = contents(ledger());
    const auto at = blob.find("Stage 1 of D-1", blob.find('\n') + 1);
    ASSERT_NE(at, std::string::npos);
    blob[at + 6] = '2';
    ASSERT_NO_FATAL_FAILURE(overwrite(ledger(), blob));
    const std::size_t before = contents(ledger()).size();

    const auto appended = impl::append_trial(ledger(), pre_registered("cp14-0003"));
    ASSERT_FALSE(appended.has_value());
    EXPECT_EQ(appended.error().code(), ErrorCode::Internal);
    EXPECT_EQ(contents(ledger()).size(), before) << "a refused append must write nothing";
}

TEST_F(TrialLedger, DeclaredTrialsForCheckpointReturnsTheDeclaredN) {
    ASSERT_NO_FATAL_FAILURE(seed_three_lines());
    impl::TrialLedgerEntry other = pre_registered("cp15-0001");
    other.checkpoint = 15;
    other.trial_count_declared = 7;
    ASSERT_TRUE(impl::append_trial(ledger(), other).has_value());

    // Two pre-registered lines for checkpoint 14 (the run and its retry), one completion
    // line that must not be counted: 30 + 30.
    const auto fourteen = impl::declared_trials_for_checkpoint(ledger(), 14);
    ASSERT_TRUE(fourteen.has_value()) << fourteen.error().to_string();
    EXPECT_EQ(*fourteen, 60u);

    const auto fifteen = impl::declared_trials_for_checkpoint(ledger(), 15);
    ASSERT_TRUE(fifteen.has_value()) << fifteen.error().to_string();
    EXPECT_EQ(*fifteen, 7u);

    const auto absent = impl::declared_trials_for_checkpoint(ledger(), 99);
    ASSERT_FALSE(absent.has_value());
    EXPECT_EQ(absent.error().code(), ErrorCode::NotFound);
}

TEST_F(TrialLedger, ManifestTracksTheHeadAndPreservesCreatedUtc) {
    impl::TrialLedgerEntry first = pre_registered("cp14-0001");
    first.appended_utc = "2026-09-20T00:00:00Z";
    ASSERT_TRUE(impl::append_trial(ledger(), first).has_value());
    impl::TrialLedgerEntry second = completed("cp14-0001");
    second.appended_utc = "2026-09-21T11:22:33Z";
    ASSERT_TRUE(impl::append_trial(ledger(), second).has_value());

    const std::string manifest = impl::trial_ledger_manifest_path(ledger());
    EXPECT_EQ(fs::path{manifest}.filename().string(), "trial-ledger.manifest.json");
    const Json sidecar = Json::parse(contents(manifest));
    const auto head = impl::verify_trial_ledger(ledger());
    ASSERT_TRUE(head.has_value()) << head.error().to_string();
    EXPECT_EQ(sidecar["lines"].get<std::size_t>(), 2u);
    EXPECT_EQ(sidecar["head_sha256"].get<std::string>(), head->head_sha256);
    EXPECT_EQ(sidecar["created_utc"].get<std::string>(), "2026-09-20T00:00:00Z");
}

TEST_F(TrialLedger, PreRegisteredLineRejectsAResultDigest) {
    impl::TrialLedgerEntry entry = pre_registered("cp14-0001");
    entry.result.manifest_sha256 = std::string(64, 'd');
    const auto line = impl::serialize_trial_entry(entry);
    ASSERT_FALSE(line.has_value());
    EXPECT_EQ(line.error().code(), ErrorCode::InvalidArgument);

    impl::TrialLedgerEntry missing = completed("cp14-0001");
    missing.result.manifest_sha256.reset();
    const auto second = impl::serialize_trial_entry(missing);
    ASSERT_FALSE(second.has_value());
    EXPECT_EQ(second.error().code(), ErrorCode::InvalidArgument);
}

// Ruling R-1. The chain alone cannot see a tail edit — the last line is compared with
// nothing — so the sidecar's head is the anchor. This is the attack that matters: it
// lowers the very N that eval::deflated_sharpe is fed.
TEST_F(TrialLedger, TailEditIsDetectedBySidecarHeadMismatch) {
    ASSERT_NO_FATAL_FAILURE(seed_three_lines());
    std::string blob = contents(ledger());
    const auto at = blob.rfind("\"trial_count_declared\":30");
    ASSERT_NE(at, std::string::npos);
    blob.replace(at, 25, "\"trial_count_declared\":10"); // same length, last line only
    ASSERT_NO_FATAL_FAILURE(overwrite(ledger(), blob));

    const auto head = impl::verify_trial_ledger(ledger());
    ASSERT_FALSE(head.has_value());
    EXPECT_EQ(head.error().code(), ErrorCode::Internal);
    const std::string message = head.error().message();
    EXPECT_NE(message.find("sidecar records"), std::string::npos) << message;
    EXPECT_NE(message.find("walks to"), std::string::npos) << message;

    // The forged count must not be readable either, and no append may extend it.
    const auto declared = impl::declared_trials_for_checkpoint(ledger(), 14);
    ASSERT_FALSE(declared.has_value());
    EXPECT_EQ(declared.error().code(), ErrorCode::Internal);
    const auto appended = impl::append_trial(ledger(), pre_registered("cp14-0003"));
    ASSERT_FALSE(appended.has_value());
}

TEST_F(TrialLedger, LastLineDeletionIsDetectedBySidecarCountMismatch) {
    ASSERT_NO_FATAL_FAILURE(seed_three_lines());
    std::string blob = contents(ledger());
    blob.resize(blob.rfind('\n', blob.size() - 2) + 1);
    ASSERT_NO_FATAL_FAILURE(overwrite(ledger(), blob));

    const auto head = impl::verify_trial_ledger(ledger());
    ASSERT_FALSE(head.has_value());
    EXPECT_EQ(head.error().code(), ErrorCode::Internal);
    const std::string message = head.error().message();
    EXPECT_NE(message.find("lines=3"), std::string::npos) << message;
    EXPECT_NE(message.find("lines=2"), std::string::npos) << message;
}

// The ONE documented limit: an editor who rewrites the tail AND regenerates the sidecar
// to match is not detectable from the two files alone. Pinned so it stays a known,
// git-visible limit rather than an assumption someone later mistakes for a guarantee.
TEST_F(TrialLedger, LastLineDeletionWithAConsistentSidecarIsTheDocumentedLimit) {
    ASSERT_NO_FATAL_FAILURE(seed_three_lines());
    std::string blob = contents(ledger());
    blob.resize(blob.rfind('\n', blob.size() - 2) + 1);
    ASSERT_NO_FATAL_FAILURE(overwrite(ledger(), blob));
    ASSERT_NO_FATAL_FAILURE(regenerate_sidecar(ledger(), "2026-09-20T00:00:00Z"));

    const auto head = impl::verify_trial_ledger(ledger());
    ASSERT_TRUE(head.has_value()) << head.error().to_string();
    EXPECT_EQ(head->lines, 2u);
    const auto declared = impl::declared_trials_for_checkpoint(ledger(), 14);
    ASSERT_TRUE(declared.has_value()) << declared.error().to_string();
    EXPECT_EQ(*declared, 30u); // the retry's pre-registration is gone, undetectably
}

TEST_F(TrialLedger, MissingSidecarBesideANonEmptyLedgerIsRefused) {
    ASSERT_NO_FATAL_FAILURE(seed_three_lines());
    std::error_code ec;
    ASSERT_TRUE(fs::remove(fs::path{impl::trial_ledger_manifest_path(ledger())}, ec));
    const std::size_t before = contents(ledger()).size();

    const auto head = impl::verify_trial_ledger(ledger());
    ASSERT_FALSE(head.has_value());
    EXPECT_EQ(head.error().code(), ErrorCode::NotFound);
    const auto appended = impl::append_trial(ledger(), pre_registered("cp14-0003"));
    ASSERT_FALSE(appended.has_value());
    EXPECT_EQ(appended.error().code(), ErrorCode::NotFound);
    EXPECT_EQ(contents(ledger()).size(), before) << "a refused append must write nothing";
}

TEST_F(TrialLedger, ForgedGenesisLinkFailsAtLineZero) {
    ASSERT_NO_FATAL_FAILURE(seed_three_lines());
    std::string blob = contents(ledger());
    const auto at = blob.find(std::string(64, '0'));
    ASSERT_NE(at, std::string::npos);
    blob.replace(at, 64, std::string(64, 'f'));
    ASSERT_NO_FATAL_FAILURE(overwrite(ledger(), blob));

    const auto head = impl::verify_trial_ledger(ledger());
    ASSERT_FALSE(head.has_value());
    EXPECT_EQ(head.error().code(), ErrorCode::Internal);
    EXPECT_NE(head.error().message().find("line 0"), std::string::npos)
        << head.error().message();
}

// Ruling R-2. Concurrent appends would stamp the same prev_sha256 and break the chain
// permanently, so a pre-existing lock refuses the write outright: no wait, no takeover.
TEST_F(TrialLedger, ConcurrentAppendIsRefusedWhileTheLockExists) {
    ASSERT_NO_FATAL_FAILURE(seed_three_lines());
    const std::string lock_path = ledger() + ".lock";
    ASSERT_NO_FATAL_FAILURE(overwrite(lock_path, ""));
    const std::size_t before = contents(ledger()).size();

    const auto refused = impl::append_trial(ledger(), pre_registered("cp14-0003"));
    ASSERT_FALSE(refused.has_value());
    EXPECT_EQ(refused.error().code(), ErrorCode::Unavailable);
    EXPECT_NE(refused.error().message().find(lock_path), std::string::npos)
        << refused.error().message();
    EXPECT_EQ(contents(ledger()).size(), before) << "a refused append must write nothing";
    std::error_code ec;
    EXPECT_TRUE(fs::exists(fs::path{lock_path}, ec)) << "a foreign lock must not be removed";

    ASSERT_TRUE(fs::remove(fs::path{lock_path}, ec));
    ASSERT_TRUE(impl::append_trial(ledger(), pre_registered("cp14-0003")).has_value());
    EXPECT_FALSE(fs::exists(fs::path{lock_path}, ec)) << "the lock must not outlive an append";
}

// M-1: an object key is written verbatim, so a key able to forge a scannable top-level
// field is rejected before it can reach the file.
TEST_F(TrialLedger, RejectsAnObjectKeyThatCouldForgeAScannableField) {
    impl::TrialLedgerEntry entry = pre_registered("cp14-0001");
    entry.source_exclusions.terminal_evidenced_record_dates = {
        {"x\",\"checkpoint\":99,\"y", "2013-10-28"}};
    const auto line = impl::serialize_trial_entry(entry);
    ASSERT_FALSE(line.has_value());
    EXPECT_EQ(line.error().code(), ErrorCode::InvalidArgument);
}

// ---------------------------------------------------------------------------
//  Checkpoint 15 (design 2026-09-20-iteration15 §5.7, §9.3 L1-L8): the non-trial
//  purpose allow-list. Only `point-in-time-universe-construction` may declare 0.
// ---------------------------------------------------------------------------
namespace {
impl::TrialLedgerEntry non_trial(const std::string &trial_id) {
    impl::TrialLedgerEntry entry = pre_registered(trial_id);
    entry.checkpoint = 15;
    entry.purpose = "point-in-time-universe-construction";
    entry.trial_count_declared = 0;
    return entry;
}

// A hand-written pre-registered line: `line` serializes any entry (so the validator can
// be bypassed by editing the text), then the chain is rebuilt so verified_walk accepts it.
void append_raw(const std::string &ledger_path, const impl::TrialLedgerEntry &entry,
                const std::string &from, const std::string &to) {
    const auto lines = split_lines(contents(ledger_path));
    std::string head(64, '0');
    if (!lines.empty()) head = *atx::core::sha256_hex(lines.back() + "\n");
    impl::TrialLedgerEntry stamped = entry;
    stamped.prev_sha256 = head;
    auto text = impl::serialize_trial_entry(stamped);
    ASSERT_TRUE(text.has_value()) << text.error().message();
    const auto at = text->find(from);
    ASSERT_NE(at, std::string::npos);
    text->replace(at, from.size(), to);
    ASSERT_NO_FATAL_FAILURE(overwrite(ledger_path, contents(ledger_path) + *text + "\n"));
    ASSERT_NO_FATAL_FAILURE(regenerate_sidecar(ledger_path, "2026-09-20T00:00:00Z"));
}
} // namespace

TEST_F(TrialLedger, ZeroDeclared_AcceptedOnlyForNonTrialPurpose) {
    EXPECT_TRUE(impl::is_non_trial_purpose("point-in-time-universe-construction"));
    EXPECT_FALSE(impl::is_non_trial_purpose("training-only-forecast-evaluation"));
    EXPECT_FALSE(impl::is_non_trial_purpose(""));
    const auto ok = impl::serialize_trial_entry(non_trial("cp15-0001"));
    ASSERT_TRUE(ok.has_value()) << ok.error().message();
    EXPECT_NE(ok->find("\"trial_count_declared\":0,"), std::string::npos);
    ASSERT_TRUE(impl::append_trial(ledger(), non_trial("cp15-0001")).has_value());

    impl::TrialLedgerEntry trial = pre_registered("cp14-0001");
    trial.trial_count_declared = 0;
    const auto rejected = impl::serialize_trial_entry(trial);
    ASSERT_FALSE(rejected.has_value());
    EXPECT_EQ(rejected.error().code(), ErrorCode::InvalidArgument);
    EXPECT_NE(rejected.error().message().find("trial_count_declared must be positive"),
              std::string::npos);
    EXPECT_FALSE(impl::append_trial(ledger(), trial).has_value());
}

TEST_F(TrialLedger, PositiveDeclared_RejectedForNonTrialPurpose) {
    impl::TrialLedgerEntry entry = non_trial("cp15-0001");
    entry.trial_count_declared = 1;
    const auto rejected = impl::serialize_trial_entry(entry);
    ASSERT_FALSE(rejected.has_value());
    EXPECT_EQ(rejected.error().code(), ErrorCode::InvalidArgument);
    EXPECT_NE(rejected.error().message().find("non-trial purpose must declare 0"),
              std::string::npos);
}

TEST_F(TrialLedger, NegativeDeclared_AlwaysRejected) {
    impl::TrialLedgerEntry trial = pre_registered("cp14-0001");
    trial.trial_count_declared = -1;
    EXPECT_FALSE(impl::serialize_trial_entry(trial).has_value());
    impl::TrialLedgerEntry universe = non_trial("cp15-0001");
    universe.trial_count_declared = -1;
    const auto rejected = impl::serialize_trial_entry(universe);
    ASSERT_FALSE(rejected.has_value());
    EXPECT_EQ(rejected.error().code(), ErrorCode::InvalidArgument);
    // The cp14 wording is kept for a negative count under either purpose.
    EXPECT_NE(rejected.error().message().find("trial_count_declared must be positive"),
              std::string::npos);
}

TEST_F(TrialLedger, DeclaredTrials_SumsZeroLinesAndFindsThem) {
    // A checkpoint whose ONLY line is allow-listed: Ok(0), not NotFound.
    ASSERT_TRUE(impl::append_trial(ledger(), non_trial("cp15-0001")).has_value());
    const auto only = impl::declared_trials_for_checkpoint(ledger(), 15);
    ASSERT_TRUE(only.has_value()) << only.error().to_string();
    EXPECT_EQ(*only, 0u);
    // Mixed cp14/cp15: the cp14 sum is untouched by the cp15 zero lines.
    ASSERT_NO_FATAL_FAILURE(seed_three_lines());
    impl::TrialLedgerEntry done = non_trial("cp15-0001");
    done.status = "completed";
    done.result.outcome = "completed";
    done.result.manifest_sha256 = std::string(64, 'd');
    done.runtime.wall_seconds = 1.5;
    ASSERT_TRUE(impl::append_trial(ledger(), done).has_value());
    ASSERT_TRUE(impl::append_trial(ledger(), non_trial("cp15-0002")).has_value());
    const auto fourteen = impl::declared_trials_for_checkpoint(ledger(), 14);
    ASSERT_TRUE(fourteen.has_value()) << fourteen.error().to_string();
    EXPECT_EQ(*fourteen, 60u);
    const auto fifteen = impl::declared_trials_for_checkpoint(ledger(), 15);
    ASSERT_TRUE(fifteen.has_value()) << fifteen.error().to_string();
    EXPECT_EQ(*fifteen, 0u);
    const auto absent = impl::declared_trials_for_checkpoint(ledger(), 99);
    ASSERT_FALSE(absent.has_value());
    EXPECT_EQ(absent.error().code(), ErrorCode::NotFound);
}

TEST_F(TrialLedger, DeclaredTrials_ZeroWithTrialPurpose_IsParseError) {
    ASSERT_NO_FATAL_FAILURE(append_raw(ledger(), pre_registered("cp14-0001"),
                                       "\"trial_count_declared\":30,",
                                       "\"trial_count_declared\":0,"));
    ASSERT_TRUE(impl::verify_trial_ledger(ledger()).has_value());
    const auto walked = impl::declared_trials_for_checkpoint(ledger(), 14);
    ASSERT_FALSE(walked.has_value());
    EXPECT_EQ(walked.error().code(), ErrorCode::ParseError);
    EXPECT_NE(walked.error().message().find("non-positive trial count"), std::string::npos);
}

TEST_F(TrialLedger, DeclaredTrials_PositiveWithNonTrialPurposeInWalk_IsParseError) {
    ASSERT_NO_FATAL_FAILURE(append_raw(ledger(), non_trial("cp15-0001"),
                                       "\"trial_count_declared\":0,",
                                       "\"trial_count_declared\":1,"));
    ASSERT_TRUE(impl::verify_trial_ledger(ledger()).has_value());
    const auto walked = impl::declared_trials_for_checkpoint(ledger(), 15);
    ASSERT_FALSE(walked.has_value());
    EXPECT_EQ(walked.error().code(), ErrorCode::ParseError);
    EXPECT_NE(walked.error().message().find("non-trial purpose"), std::string::npos);
}

TEST_F(TrialLedger, PreRegisteredLines_CountsByCheckpoint) {
    const auto none = impl::pre_registered_lines_for_checkpoint(ledger(), 15);
    ASSERT_TRUE(none.has_value()) << none.error().to_string();
    EXPECT_EQ(*none, 0u);
    ASSERT_TRUE(impl::append_trial(ledger(), non_trial("cp15-0001")).has_value());
    const auto one = impl::pre_registered_lines_for_checkpoint(ledger(), 15);
    ASSERT_TRUE(one.has_value());
    EXPECT_EQ(*one, 1u);
    impl::TrialLedgerEntry failed = non_trial("cp15-0001");
    failed.status = "failed";
    failed.result.outcome = "failed";
    failed.result.failure_sha256 = std::string(64, 'e');
    ASSERT_TRUE(impl::append_trial(ledger(), failed).has_value());
    ASSERT_TRUE(impl::append_trial(ledger(), non_trial("cp15-0002")).has_value());
    // Terminal lines are not counted; the retry is.
    const auto two = impl::pre_registered_lines_for_checkpoint(ledger(), 15);
    ASSERT_TRUE(two.has_value());
    EXPECT_EQ(*two, 2u);
    ASSERT_NO_FATAL_FAILURE(seed_three_lines());
    const auto cp14 = impl::pre_registered_lines_for_checkpoint(ledger(), 14);
    ASSERT_TRUE(cp14.has_value());
    EXPECT_EQ(*cp14, 2u);
    EXPECT_EQ(*impl::pre_registered_lines_for_checkpoint(ledger(), 15), 2u);
}

TEST(TrialLedgerRepository, ExistingCp14Ledger_StillVerifies) {
    // The committed ledger is reachable only when the test runs from the repository
    // root; elsewhere the case is skipped rather than reinterpreted.
    const std::string committed = "atx-engine/reviews/trial-ledger.jsonl";
    std::error_code ec;
    if (!fs::exists(committed, ec) || ec) GTEST_SKIP() << "not run from the repository root";
    const auto head = impl::verify_trial_ledger(committed);
    ASSERT_TRUE(head.has_value()) << head.error().to_string();
    EXPECT_GE(head->lines, 2u);
    const auto declared = impl::declared_trials_for_checkpoint(committed, 14);
    ASSERT_TRUE(declared.has_value()) << declared.error().to_string();
    EXPECT_EQ(*declared % 30u, 0u);
}

} // namespace
