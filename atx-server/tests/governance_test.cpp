#include <atx/server/core.hpp>
#include "legacy_schema.hpp"
#include <atx/core/sha256.hpp>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include <sqlite3.h>

#include <atomic>
#include <filesystem>
#include <future>
#include <string>
#include <system_error>

namespace atx::server {
namespace {
using Json = nlohmann::json;
AlphaInput hypothesis(std::string actor = "author") {
    return {"Hypothesis", "", {}, "Causal hypothesis", R"({"mode":"synthetic_demo"})",
        "", "local", std::move(actor)};
}
std::string result(const AlphaSnapshot& alpha) {
    return Json{{"schema_version", "atx.research-result/v1"}, {"mode", "synthetic_demo"},
        {"metrics", {{"observations", 2}}}, {"provenance", {{"synthetic", true},
        {"evaluation", "synthetic_test"}, {"submitted_request_sha256", alpha.request_sha256},
        {"warnings", Json::array({"Synthetic evidence is not trading certification"})}}}}.dump();
}
Job completed(Store& store, const AlphaSnapshot& alpha) {
    const auto job = store.create("", "", 0, 32, {alpha.id, alpha.revision, "Evidence", "author"});
    const auto running = store.claim_due();
    if (!running || running->id != job.id) throw std::runtime_error("invalid test queue");
    store.finish(job.id, "SUCCEEDED", result(alpha), "");
    return store.get(job.id);
}
ReviewInput submission(const AlphaSnapshot& alpha, const Job& job) {
    return {alpha.id, alpha.revision, {job.id}, "Review modeled costs and data limitations",
        "requester", "review-key"};
}
struct GovernanceFile {
    std::filesystem::path path{std::filesystem::temp_directory_path() /
        ("atx-governance-test-" + std::to_string(unix_ms()) + "-" +
         std::to_string(sequence.fetch_add(1)) + ".db")};
    ~GovernanceFile() {
        std::error_code ignored;
        for (const auto* suffix : {"", "-wal", "-shm"}) {
            std::filesystem::remove(path.string() + suffix, ignored);
        }
    }
    static inline std::atomic<unsigned> sequence{};
};
} // namespace
TEST(ServerGovernance, UserMutationsAttributeActorsAndAuditOnlyBoundedMetadata) {
    Store store(":memory:");
    const auto desk = store.create_workspace("Desk", "", R"({"schema_version":1,"layout":{}})",
        "alice");
    EXPECT_EQ(desk.actor_id, "alice");
    const auto alpha = store.create_alpha(hypothesis("alice"));
    EXPECT_EQ(alpha.actor_id, "alice");
    const auto job = store.create("", "", 0, 32, {alpha.id, 1, "Run", "bob"});
    EXPECT_EQ(job.actor_id, "bob");
    static_cast<void>(store.cancel(job.id, "alice"));
    const auto modified = store.update_workspace(desk.id, 1, "Changed", "",
        R"({"schema_version":1,"layout":{}})", "bob");
    EXPECT_EQ(modified.actor_id, "bob");
    auto page = store.list_audit(100, 0);
    ASSERT_EQ(page.total, 5U);
    EXPECT_EQ(store.list_audit(100, 0, "workspace", desk.id, "bob").total, 1U);
    for (const auto& row : page.records) {
        EXPECT_LT(row.metadata_json.size(), 4097U);
        EXPECT_EQ(row.metadata_json.find("layout"), std::string::npos);
        EXPECT_EQ(row.metadata_json.find("synthetic_demo"), std::string::npos);
        EXPECT_EQ(row.metadata_json.find("Causal hypothesis"), std::string::npos);
    }
    store.delete_workspace(desk.id, 2, "alice");
    EXPECT_EQ(store.list_audit(100, 0, "workspace", desk.id).total, 3U);
}
TEST(ServerGovernance, ReviewSubmissionPinsServerEvidenceAndRetriesWithoutDuplicatingAudit) {
    Store store(":memory:");
    const auto alpha = store.create_alpha(hypothesis());
    const auto job = completed(store, alpha);
    const auto review = store.submit_review(submission(alpha, job));
    EXPECT_EQ(review.state, "submitted");
    EXPECT_EQ(review.revision, 1U);
    EXPECT_TRUE(review.current);
    EXPECT_EQ(review.author_actor_id, "author");
    EXPECT_EQ(review.requester_actor_id, "requester");
    const auto evidence = Json::parse(review.evidence_json);
    ASSERT_EQ(evidence.at("jobs").size(), 1U);
    EXPECT_TRUE(evidence.at("jobs")[0].at("synthetic").get<bool>());
    EXPECT_EQ(evidence.at("jobs")[0].at("observations"), 2);
    const auto digest = atx::core::sha256_hex(job.result_json);
    ASSERT_TRUE(digest);
    EXPECT_EQ(evidence.at("jobs")[0].at("result_sha256"), *digest);
    EXPECT_EQ(store.submit_review(submission(alpha, job)).id, review.id);
    EXPECT_EQ(store.list_reviews(100, 0).total, 1U);
    EXPECT_EQ(store.list_audit(100, 0, "review", review.id).total, 1U);
    auto changed = submission(alpha, job); changed.note = "Different submission";
    EXPECT_THROW(static_cast<void>(store.submit_review(changed)), Error);
}
TEST(ServerGovernance, IndependentDecisionsRequireRationaleAndCurrentAlphaAndRetainHistory) {
    Store store(":memory:");
    const auto alpha = store.create_alpha(hypothesis());
    const auto review = store.submit_review(submission(alpha, completed(store, alpha)));
    EXPECT_THROW(static_cast<void>(store.decide_review(review.id, 1, "approved", "Checked",
        "author", true)), Error);
    EXPECT_THROW(static_cast<void>(store.decide_review(review.id, 1, "approved", "Checked",
        "requester", true)), Error);
    EXPECT_THROW(static_cast<void>(store.decide_review(review.id, 1, "approved", " ",
        "reviewer", true)), Error);
    const auto approved = store.decide_review(review.id, 1, "approved", "Evidence acknowledged",
        "reviewer", true);
    EXPECT_EQ(approved.revision, 2U);
    EXPECT_EQ(approved.state, "approved");
    EXPECT_EQ(approved.evidence_json, review.evidence_json);
    EXPECT_THROW(static_cast<void>(store.decide_review(review.id, 1, "rejected", "Stale",
        "reviewer", true)), Error);
    auto modified = hypothesis(); modified.name = "Metadata edit";
    static_cast<void>(store.update_alpha(alpha.id, 1, modified));
    EXPECT_FALSE(store.get_review(review.id).current);
    EXPECT_EQ(store.get_review(review.id).state, "approved");
    EXPECT_THROW(static_cast<void>(store.decide_review(review.id, 2, "withdrawn", "Withdraw",
        "stranger")), Error);
    const auto withdrawn = store.decide_review(review.id, 2, "withdrawn", "Superseded alpha",
        "requester");
    EXPECT_EQ(withdrawn.state, "withdrawn");
    EXPECT_EQ(withdrawn.revision, 3U);
    const auto history = store.list_review_decisions(review.id, 100, 0);
    ASSERT_EQ(history.decisions.size(), 2U);
    EXPECT_EQ(history.decisions[0].state, "withdrawn");
    EXPECT_EQ(history.decisions[1].state, "approved");
}
TEST(ServerGovernance, StaleSubmissionsCannotBeApprovedButCanBeRejectedOrAdminWithdrawn) {
    Store store(":memory:");
    const auto alpha = store.create_alpha(hypothesis());
    auto input = submission(alpha, completed(store, alpha));
    const auto review = store.submit_review(input);
    static_cast<void>(store.set_alpha_archived(alpha.id, 1, true, "author"));
    EXPECT_FALSE(store.get_review(review.id).current);
    EXPECT_THROW(static_cast<void>(store.decide_review(review.id, 1, "approved", "Stale",
        "reviewer", true)), Error);
    EXPECT_EQ(store.decide_review(review.id, 1, "rejected", "Archived source",
        "reviewer", true).state, "rejected");
    EXPECT_EQ(store.submit_review(input).id, review.id);
    input.idempotency_key = "new-stale-request";
    EXPECT_THROW(static_cast<void>(store.submit_review(input)), Error);
}
TEST(ServerGovernance, FailedMalformedDuplicateAndMismatchedEvidenceNeverCreatesReviewRows) {
    Store store(":memory:");
    const auto alpha = store.create_alpha(hypothesis());
    const auto good = completed(store, alpha);
    auto input = submission(alpha, good); input.job_ids = {good.id, good.id};
    EXPECT_THROW(static_cast<void>(store.submit_review(input)), Error);
    const auto failed = store.create(alpha.request_json, "", 0, 32);
    ASSERT_TRUE(store.claim_due());
    store.finish(failed.id, "FAILED", "", "Invalid model");
    input = submission(alpha, failed);
    EXPECT_THROW(static_cast<void>(store.submit_review(input)), Error);
    const auto malformed = store.create(alpha.request_json, "", 0, 32);
    ASSERT_TRUE(store.claim_due());
    store.finish(malformed.id, "SUCCEEDED", "{}", "");
    input = submission(alpha, malformed);
    EXPECT_THROW(static_cast<void>(store.submit_review(input)), Error);
    const auto different = store.create(R"({"mode":"synthetic_demo","sessions":64})", "", 0, 32);
    ASSERT_TRUE(store.claim_due());
    store.finish(different.id, "SUCCEEDED", result(alpha), "");
    input = submission(alpha, different);
    EXPECT_THROW(static_cast<void>(store.submit_review(input)), Error);
    EXPECT_EQ(store.list_reviews(100, 0).total, 0U);
}
TEST(ServerGovernance, ConcurrentDecisionsAdmitOneWinnerAndAuditFailureRollsBackMutation) {
    GovernanceFile file;
    Store store(file.path.string());
    const auto alpha = store.create_alpha(hypothesis());
    const auto review = store.submit_review(submission(alpha, completed(store, alpha)));
    auto decide = [&](const char* actor) {
        try { static_cast<void>(store.decide_review(review.id, 1, "approved", "Reviewed", actor,
            true)); return true; }
        catch (const Error& error) { EXPECT_EQ(error.code(), ErrorCode::conflict); return false; }
    };
    auto first = std::async(std::launch::async, [&] { return decide("one"); });
    auto second = std::async(std::launch::async, [&] { return decide("two"); });
    EXPECT_EQ(static_cast<unsigned>(first.get()) + static_cast<unsigned>(second.get()), 1U);
    EXPECT_EQ(store.list_review_decisions(review.id, 100, 0).total, 1U);
    sqlite3* raw{};
    ASSERT_EQ(sqlite3_open(file.path.string().c_str(), &raw), SQLITE_OK);
    struct Close { sqlite3* value; ~Close() { sqlite3_close(value); } } close{raw};
    ASSERT_EQ(sqlite3_exec(raw, "CREATE TRIGGER fail_audit BEFORE INSERT ON audit_records "
        "BEGIN SELECT RAISE(ABORT,'fixture disk failure'); END;", nullptr, nullptr, nullptr),
        SQLITE_OK);
    EXPECT_THROW(static_cast<void>(store.create_workspace("Rollback", "",
        R"({"schema_version":1,"layout":{}})", "alice")), Error);
    EXPECT_EQ(store.list_workspaces(100, 0).total, 0U);
    auto changed = hypothesis(); changed.name = "Rollback";
    EXPECT_THROW(static_cast<void>(store.update_alpha(alpha.id, 1, changed)), Error);
    EXPECT_EQ(store.get_alpha(alpha.id).revision, 1U);
}
TEST(ServerGovernance, ActorDecisionAndAuditPaginationBoundsRejectInvalidInputs) {
    Store store(":memory:");
    auto invalid = hypothesis("bad actor");
    EXPECT_THROW(static_cast<void>(store.create_alpha(invalid)), Error);
    invalid.actor_id = std::string(65, 'a');
    EXPECT_THROW(static_cast<void>(store.create_alpha(invalid)), Error);
    EXPECT_THROW(static_cast<void>(store.list_audit(101, 0)), Error);
    EXPECT_THROW(static_cast<void>(store.list_audit(1, 1000001)), Error);
    EXPECT_THROW(static_cast<void>(store.list_audit(1, 0, "unknown")), Error);
    EXPECT_THROW(static_cast<void>(store.list_reviews(1, 0, "", "pretend-live")), Error);
    EXPECT_EQ(store.list_reviews(100, 0).total, 0U);
}
TEST(ServerGovernance, VersionFiveMigrationPreservesEvidenceWithoutInventingActorHistory) {
    GovernanceFile file;
    std::string workspace_id, alpha_id, job_id, experiment_id;
    {
        Store store(file.path.string());
        workspace_id = store.create_workspace("Legacy", "",
            R"({"schema_version":1,"layout":{}})").id;
        const auto alpha = store.create_alpha(hypothesis());
        alpha_id = alpha.id; job_id = completed(store, alpha).id;
        ExperimentInput batch{"Legacy experiment", "", {{"Member", "{}", "", 0}}, "", 0};
        experiment_id = store.create_experiment(batch, 32).id;
    }
    sqlite3* raw{};
    ASSERT_EQ(sqlite3_open(file.path.string().c_str(), &raw), SQLITE_OK);
    {
        struct Close { sqlite3* value; ~Close() { sqlite3_close(value); } } close{raw};
        ASSERT_EQ(sqlite3_exec(raw, (std::string(test_detail::remove_governance) +
            "PRAGMA user_version=5;").c_str(), nullptr, nullptr, nullptr), SQLITE_OK);
    }
    Store migrated(file.path.string());
    EXPECT_EQ(migrated.get_workspace(workspace_id).actor_id, "migration_unknown");
    const auto alpha = migrated.get_alpha(alpha_id);
    EXPECT_EQ(alpha.actor_id, "migration_unknown");
    EXPECT_EQ(migrated.get(job_id).actor_id, "migration_unknown");
    EXPECT_EQ(migrated.get_experiment(experiment_id).actor_id, "migration_unknown");
    EXPECT_EQ(migrated.list_audit(100, 0).total, 0U);
    const auto review = migrated.submit_review(submission(alpha, migrated.get(job_id)));
    EXPECT_EQ(review.author_actor_id, "migration_unknown");
    const auto evidence = Json::parse(review.evidence_json);
    EXPECT_FALSE(evidence.at("warnings").empty());
    EXPECT_EQ(evidence.at("jobs")[0].at("actor_id"), "migration_unknown");
    EXPECT_EQ(migrated.list_audit(100, 0).total, 1U);
}
TEST(ServerGovernance, DurableSubmittedEvidenceDecisionAndAuditRowsAreImmutable) {
    GovernanceFile file;
    std::string review_id, frozen;
    {
        Store store(file.path.string());
        const auto alpha = store.create_alpha(hypothesis());
        auto review = store.submit_review(submission(alpha, completed(store, alpha)));
        review_id = review.id; frozen = review.evidence_json;
        static_cast<void>(store.decide_review(review.id, 1, "approved", "Reviewed", "reviewer",
            true));
        sqlite3* raw{};
        ASSERT_EQ(sqlite3_open(file.path.string().c_str(), &raw), SQLITE_OK);
        struct Close { sqlite3* value; ~Close() { sqlite3_close(value); } } close{raw};
        for (const auto* sql : {"UPDATE audit_records SET action='forged'",
            "DELETE FROM audit_records", "UPDATE review_submissions SET evidence_json='{}'",
            "DELETE FROM review_submissions", "UPDATE review_decisions SET rationale='forged'",
            "DELETE FROM review_decisions", "UPDATE jobs SET actor_id='forged'"}) {
            EXPECT_EQ(sqlite3_exec(raw, sql, nullptr, nullptr, nullptr), SQLITE_CONSTRAINT);
        }
        EXPECT_TRUE(store.list_reviews(100, 0).reviews[0].evidence_json.empty());
    }
    Store reopened(file.path.string());
    const auto review = reopened.get_review(review_id);
    EXPECT_EQ(review.evidence_json, frozen);
    EXPECT_EQ(review.state, "approved");
    EXPECT_EQ(reopened.list_review_decisions(review_id, 100, 0).decisions[0].actor_id, "reviewer");
}
TEST(ServerGovernance, EvidenceProvenanceTypesBoundsAndDistinctAlphaLineageAreChecked) {
    Store store(":memory:");
    const auto alpha = store.create_alpha(hypothesis());
    const auto template_result = Json::parse(result(alpha));
    auto invalid = [&](Json value) {
        const auto job = store.create(alpha.request_json, "", 0, 32);
        ASSERT_TRUE(store.claim_due());
        store.finish(job.id, "SUCCEEDED", value.dump(), "");
        auto input = submission(alpha, store.get(job.id)); input.idempotency_key.clear();
        EXPECT_THROW(static_cast<void>(store.submit_review(input)), Error);
    };
    for (const auto& observations : {Json(0), Json(-1), Json(1.5), Json(1000001),
        Json(1e100), Json(true), Json("2"), Json(nullptr)}) {
        auto value = template_result; value["metrics"]["observations"] = observations;
        invalid(value);
    }
    auto value = template_result; value["provenance"]["synthetic"] = false; invalid(value);
    value = template_result; value["provenance"]["warnings"] = Json::array({42}); invalid(value);
    value = template_result; value["provenance"]["warnings"] =
        std::vector<std::string>(33, "warning"); invalid(value);
    value = template_result; value["provenance"]["submitted_request_sha256"] =
        std::string(64, 'a'); invalid(value);
    const auto other = store.create_alpha(hypothesis());
    const auto other_job = completed(store, other);
    auto input = submission(alpha, other_job); input.idempotency_key.clear();
    EXPECT_THROW(static_cast<void>(store.submit_review(input)), Error);
    EXPECT_EQ(store.list_reviews(100, 0).total, 0U);
}
TEST(ServerGovernance, ReviewMembershipNotesPagingAndExplicitLocalReviewExceptionAreBounded) {
    Store store(":memory:");
    const auto alpha = store.create_alpha(hypothesis());
    std::vector<std::string> jobs;
    for (unsigned i = 0; i < 13; ++i) jobs.push_back(completed(store, alpha).id);
    auto input = submission(alpha, store.get(jobs[0])); input.job_ids = jobs;
    EXPECT_THROW(static_cast<void>(store.submit_review(input)), Error);
    input.job_ids.resize(12); input.note = std::string(4097, 'n');
    EXPECT_THROW(static_cast<void>(store.submit_review(input)), Error);
    input.note.clear(); input.idempotency_key = std::string(129, 'k');
    EXPECT_THROW(static_cast<void>(store.submit_review(input)), Error);
    input.idempotency_key.clear();
    const auto review = store.submit_review(input);
    EXPECT_EQ(Json::parse(review.evidence_json).at("jobs").size(), 12U);
    EXPECT_THROW(static_cast<void>(store.decide_review(review.id, 1, "approved", "\xE3\x80\x80",
        "author")), Error);
    // Local standalone operation explicitly permits the author to acknowledge their own evidence.
    EXPECT_EQ(store.decide_review(review.id, 1, "approved", "Local acknowledgement", "author",
        false).state, "approved");
    EXPECT_EQ(store.decide_review(review.id, 2, "withdrawn", "Administrative withdrawal", "admin",
        true, true).state, "withdrawn");
    EXPECT_EQ(store.list_review_decisions(review.id, 1, 1).decisions[0].state, "approved");
    EXPECT_EQ(store.list_audit(1, 0, "review", review.id).total, 3U);
    EXPECT_EQ(store.list_audit(1, 0, "review", review.id).records[0].actor_id, "admin");
    EXPECT_THROW(static_cast<void>(store.list_review_decisions(review.id, 101, 0)), Error);
}
TEST(ServerGovernance, FailedAuditRollsBackCancellationButWorkerCompletionDoesNotRequireAudit) {
    GovernanceFile file;
    Store store(file.path.string());
    ExperimentInput batch{"Atomic cancellation", "", {{"One", "{}", "", 0},
        {"Two", "{}", "", 0}}, "", 0, "alice"};
    const auto experiment = store.create_experiment(batch, 32);
    EXPECT_EQ(experiment.actor_id, "alice");
    EXPECT_EQ(store.list_audit(100, 0).total, 3U);
    sqlite3* raw{};
    ASSERT_EQ(sqlite3_open(file.path.string().c_str(), &raw), SQLITE_OK);
    struct Close { sqlite3* value; ~Close() { sqlite3_close(value); } } close{raw};
    ASSERT_EQ(sqlite3_exec(raw, "CREATE TRIGGER fail_audit BEFORE INSERT ON audit_records "
        "BEGIN SELECT RAISE(ABORT,'fixture disk failure'); END;", nullptr, nullptr, nullptr),
        SQLITE_OK);
    EXPECT_THROW(static_cast<void>(store.cancel_experiment(experiment.id, "bob")), Error);
    EXPECT_EQ(store.get_experiment(experiment.id).queued, 2U);
    EXPECT_THROW(static_cast<void>(store.cancel(experiment.jobs[0].id, "bob")), Error);
    EXPECT_EQ(store.get(experiment.jobs[0].id).status, "QUEUED");
    const auto running = store.claim_due(); ASSERT_TRUE(running);
    EXPECT_NO_THROW(store.progress(running->id, 0.5));
    EXPECT_NO_THROW(store.finish(running->id, "FAILED", "", "worker failure"));
    EXPECT_EQ(store.get(running->id).status, "FAILED");
    ASSERT_EQ(sqlite3_exec(raw, "DROP TRIGGER fail_audit", nullptr, nullptr, nullptr), SQLITE_OK);
    EXPECT_EQ(store.cancel_experiment(experiment.id, "bob").cancelled, 1U);
    EXPECT_EQ(store.list_audit(100, 0, "experiment", experiment.id, "bob").total, 1U);
    static_cast<void>(store.cancel_experiment(experiment.id, "bob"));
    EXPECT_EQ(store.list_audit(100, 0, "experiment", experiment.id, "bob").total, 1U);
}
TEST(ServerGovernance, ConcurrentReviewRetriesPreserveOneSubmissionAndActorCannotStealKeys) {
    GovernanceFile file;
    Store first(file.path.string());
    const auto alpha = first.create_alpha(hypothesis());
    const auto job = completed(first, alpha);
    Store second(file.path.string());
    const auto input = submission(alpha, job);
    auto one = std::async(std::launch::async, [&] { return first.submit_review(input); });
    auto two = std::async(std::launch::async, [&] { return second.submit_review(input); });
    EXPECT_EQ(one.get().id, two.get().id);
    EXPECT_EQ(first.list_reviews(100, 0).total, 1U);
    EXPECT_EQ(first.list_audit(100, 0, "review").total, 1U);
    auto stolen = input; stolen.actor_id = "stranger";
    EXPECT_THROW(static_cast<void>(second.submit_review(stolen)), Error);
    const auto submitted = first.create("{}", "job-key", 0, 32, {"", 0, "", "alice"});
    EXPECT_EQ(first.create("{}", "job-key", 0, 32, {"", 0, "", "alice"}).id, submitted.id);
    EXPECT_THROW(static_cast<void>(second.create("{}", "job-key", 0, 32,
        {"", 0, "", "bob"})), Error);
    ExperimentInput batch{"Actor-scoped batch", "", {{"One", "{}", "", 0}}, "batch-key",
        0, "alice"};
    const auto experiment = first.create_experiment(batch, 32);
    EXPECT_EQ(first.create_experiment(batch, 32).id, experiment.id);
    batch.actor_id = "bob";
    EXPECT_THROW(static_cast<void>(second.create_experiment(batch, 32)), Error);
}
TEST(ServerGovernance, AggregateEvidenceResultByteLimitRejectsBeforeCreatingSubmission) {
    Store store(":memory:");
    const auto alpha = store.create_alpha(hypothesis());
    auto large = Json::parse(result(alpha)); large["padding"] = std::string(8 * 1024 * 1024, 'p');
    std::vector<std::string> jobs;
    for (unsigned i = 0; i < 2; ++i) {
        const auto job = store.create(alpha.request_json, "", 0, 32);
        ASSERT_TRUE(store.claim_due());
        store.finish(job.id, "SUCCEEDED", large.dump(), ""); jobs.push_back(job.id);
    }
    ReviewInput input{alpha.id, 1, jobs, "", "requester", ""};
    EXPECT_THROW(static_cast<void>(store.submit_review(input)), Error);
    EXPECT_EQ(store.list_reviews(100, 0).total, 0U);
    EXPECT_EQ(store.list_audit(100, 0, "review").total, 0U);
}
TEST(ServerGovernance, IndependentReviewerCannotApproveOwnRevisionOfAnotherAuthorsAlpha) {
    Store store(":memory:");
    const auto original = store.create_alpha(hypothesis("original_author"));
    auto edited = hypothesis("revision_author");
    edited.request_json = R"({"mode":"synthetic_demo","sessions":64})";
    const auto revision = store.update_alpha(original.id, 1, edited);
    auto input = submission(revision, completed(store, revision));
    input.actor_id = "third_party_requester";
    const auto review = store.submit_review(input);
    EXPECT_EQ(review.author_actor_id, "original_author");
    EXPECT_THROW(static_cast<void>(store.decide_review(review.id, 1, "approved", "Own code",
        "revision_author", true)), Error);
    EXPECT_THROW(static_cast<void>(store.decide_review(review.id, 1, "rejected", "Own code",
        "revision_author", true)), Error);
    const auto evidence = Json::parse(review.evidence_json);
    ASSERT_TRUE(evidence.contains("revision_actor_id"));
    EXPECT_EQ(evidence.at("revision_actor_id"), "revision_author");
    EXPECT_THROW(static_cast<void>(store.decide_review(review.id, 1, "approved", "Original",
        "original_author", true)), Error);
    EXPECT_THROW(static_cast<void>(store.decide_review(review.id, 1, "approved", "Requester",
        "third_party_requester", true)), Error);
    EXPECT_EQ(store.decide_review(review.id, 1, "approved", "Independent reviewer checked",
        "independent_reviewer", true).state, "approved");
}
TEST(ServerGovernance, LegacySubmittedEvidenceWithoutRevisionActorStillEnforcesIndependence) {
    GovernanceFile file;
    Store store(file.path.string());
    const auto original = store.create_alpha(hypothesis("original_author"));
    const auto revision = store.update_alpha(original.id, 1, hypothesis("revision_author"));
    auto input = submission(revision, completed(store, revision));
    input.actor_id = "third_party_requester";
    const auto review = store.submit_review(input);
    sqlite3* raw{};
    ASSERT_EQ(sqlite3_open(file.path.string().c_str(), &raw), SQLITE_OK);
    struct Close { sqlite3* value; ~Close() { sqlite3_close(value); } } close{raw};
    const auto legacy_id = std::string(32, 'f');
    const auto sql = "INSERT INTO review_submissions SELECT '" + legacy_id +
        "',alpha_id,alpha_revision,request_sha256,requester_actor_id,author_actor_id,note,"
        "json_remove(evidence_json,'$.revision_actor_id','$.recipe_actor_ids'),"
        "created_at_ms,NULL,semantic_json "
        "FROM review_submissions WHERE id='" + review.id + "';"
        "INSERT INTO review_heads SELECT '" + legacy_id +
        "','submitted',1,updated_at_ms FROM review_heads WHERE id='" + review.id + "';";
    ASSERT_EQ(sqlite3_exec(raw, sql.c_str(), nullptr, nullptr, nullptr), SQLITE_OK);
    const auto frozen = store.get_review(legacy_id).evidence_json;
    EXPECT_FALSE(Json::parse(frozen).contains("revision_actor_id"));
    EXPECT_THROW(static_cast<void>(store.decide_review(legacy_id, 1, "approved", "Own revision",
        "revision_author", true)), Error);
    EXPECT_EQ(store.decide_review(legacy_id, 1, "approved", "Independent check", "independent",
        true).state, "approved");
    EXPECT_EQ(store.get_review(legacy_id).evidence_json, frozen);
}
TEST(ServerGovernance, MetadataOnlyRevisionCannotLaunderTheFrozenRecipesAuthor) {
    Store store(":memory:");
    const auto original = store.create_alpha(hypothesis("original_author"));
    auto authored = hypothesis("recipe_author");
    authored.request_json = R"({"mode":"synthetic_demo","sessions":64})";
    const auto recipe_revision = store.update_alpha(original.id, 1, authored);
    auto metadata = authored; metadata.actor_id = "metadata_editor"; metadata.name = "Renamed";
    const auto frozen = store.update_alpha(original.id, 2, metadata);
    EXPECT_EQ(frozen.request_sha256, recipe_revision.request_sha256);
    auto input = submission(frozen, completed(store, frozen)); input.actor_id = "requester";
    const auto review = store.submit_review(input);
    EXPECT_THROW(static_cast<void>(store.decide_review(review.id, 1, "approved", "My recipe",
        "recipe_author", true)), Error);
    EXPECT_THROW(static_cast<void>(store.decide_review(review.id, 1, "rejected", "My recipe",
        "recipe_author", true)), Error);
    const auto evidence = Json::parse(review.evidence_json);
    ASSERT_TRUE(evidence.contains("recipe_actor_ids"));
    EXPECT_EQ(evidence.at("recipe_actor_ids"), Json::array({"metadata_editor", "recipe_author"}));
    // Unchanged recipe reproduction does not make a run producer its alpha author.
    EXPECT_EQ(store.decide_review(review.id, 1, "approved", "Independent reproduction", "author",
        true).state, "approved");
}
} // namespace atx::server
