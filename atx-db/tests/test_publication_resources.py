"""Release CLI budgets reach publication and survive a connection replacement."""

import pytest


@pytest.mark.parametrize(
    ("resource_args", "expected_memory_limit", "expected_settings"),
    [
        ([], "1GB", ("953.6 MiB", 1, False)),
        (["--memory-limit", "512MB", "--threads", "2"], "512MB", ("488.2 MiB", 2, False)),
    ],
    ids=["defaults", "override"],
)
def test_cli_configures_publication_resources_and_replays_on_reopen(
    built_warehouse, tmp_path, monkeypatch, resource_args, expected_memory_limit, expected_settings
):
    from atx_db import cli, publication

    db_path = built_warehouse("publication_resources.duckdb")
    observed_settings = []
    settings_query = (
        "SELECT current_setting('memory_limit'), current_setting('threads'), "
        "current_setting('preserve_insertion_order')"
    )

    def inspect_publication_resources(store, release_id, out_dir, **kwargs):
        assert store.analytical_memory_limit == expected_memory_limit
        assert store.analytical_threads == expected_settings[1]
        observed_settings.append(store.con.execute(settings_query).fetchone())

        original_connection = store.con
        store.close()
        store.reopen()
        assert store.con is not original_connection
        observed_settings.append(store.con.execute(settings_query).fetchone())

        release_dir = out_dir / release_id
        return publication.ReleaseResult(
            release_id=release_id,
            out_dir=release_dir,
            manifest_path=release_dir / "manifest.json",
            manifest_sha256="boundary-test",
            previous_release_id=None,
            datasets=(),
        )

    monkeypatch.setattr(publication, "publish_release", inspect_publication_resources)
    code = cli.main(
        [
            "publish-release",
            "--db-path",
            str(db_path),
            "--release-id",
            "resource-test",
            "--out-dir",
            str(tmp_path / "releases"),
            *resource_args,
        ]
    )

    # The stub release was never gate-evaluated: fail closed as a candidate.
    assert code == publication.CANDIDATE_EXIT_CODE
    assert observed_settings == [expected_settings, expected_settings]
