from __future__ import annotations

from atx_factor.trial_ledger import ResearchTrialLedger


def test_trial_ledger_commits_before_results_and_is_idempotent(tmp_path) -> None:
    ledger = ResearchTrialLedger(tmp_path / "trials.json")
    spec = {"candidate_id": "candidate", "variants": ["rank", "tails"]}
    first, first_total = ledger.commit(
        spec,
        configuration_trials=6,
        historical_trial_count=66,
    )
    second, second_total = ledger.commit(
        spec,
        configuration_trials=6,
        historical_trial_count=66,
    )
    assert first == second
    assert first_total == second_total == 72
    _, third_total = ledger.commit(
        {"candidate_id": "candidate_2", "variants": ["rank"]},
        configuration_trials=3,
        historical_trial_count=66,
    )
    assert third_total == 75
    assert len(ledger.load()["experiments"]) == 2
