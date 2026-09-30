"""era_pool.py (platform v8 H-1): window refusals (order, overlap, straddle, seal) and the one-era identities.

Run: "C:/Program Files/Python312/python.exe" -m pytest -q -p no:cacheprovider atx-engine/tools/test_era_pool.py

Synthetic instants only: the TRAIN and seal instants are arguments of check_windows (the repository window's values
are used where a test needs real ones), so no session of any data set is opened.
"""
from __future__ import annotations

import datetime as dt
import hashlib
import json
from pathlib import Path
import sys

import numpy as np
import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent))
import era_pool as EP  # noqa: E402

DAY = 86_400_000_000_000


def ns(text: str) -> int:
    return (dt.date.fromisoformat(text) - dt.date(1970, 1, 1)).days * DAY


TRAIN_BEGIN, TRAIN_END, SEAL = ns("2020-01-01"), ns("2024-01-01"), ns("2024-01-01")


def windows(*items):
    return EP.check_windows([(i, ns(b), ns(e)) for i, b, e in items], TRAIN_BEGIN, TRAIN_END, SEAL)


def test_history_eras_and_the_train_role_are_accepted_in_date_order():
    got = windows(("E1", "2014-01-01", "2017-01-01"), ("E2", "2017-01-01", "2020-01-01"),
                  ("E3", "2020-01-01", "2024-01-01"))
    assert [g[0] for g in got] == ["E1", "E2", "E3"]
    assert got[0][1:] == (ns("2014-01-01"), ns("2017-01-01"))
    windows(("E1", "2014-01-01", "2017-01-01"))                     # one era
    windows(("a_1", "2021-06-01", "2022-01-01"), ("b_2", "2022-01-01", "2023-01-01"))  # both inside TRAIN


@pytest.mark.parametrize("items, needle", [
    ((("E1", "2017-01-01", "2020-01-01"), ("E2", "2014-01-01", "2017-01-01")), "date order"),
    ((("E1", "2014-01-01", "2017-06-01"), ("E2", "2017-01-01", "2020-01-01")), "overlaps era E1"),
    ((("E1", "2018-01-01", "2020-06-01"),), "straddles the TRAIN begin"),
    ((("E3", "2020-01-01", "2024-01-02"),), "after the research seal"),
    ((("E9", "2025-01-01", "2025-06-01"),), "after the research seal"),
    ((("E1", "2014-01-01", "2014-01-01"),), "empty window"),
    ((("E1", "2014-01-01", "2015-01-01"), ("E1", "2015-01-01", "2016-01-01")), "duplicate id"),
    ((("E-1", "2014-01-01", "2015-01-01"),), "must match"),
    ((("E/1", "2014-01-01", "2015-01-01"),), "must match"),
])
def test_window_refusals(items, needle):
    with pytest.raises(EP.PoolError, match=needle):
        windows(*items)


def test_a_train_era_ending_after_the_train_end_is_refused_before_the_seal():
    early_end = ns("2023-01-01")   # a window whose TRAIN ends before its seal
    with pytest.raises(EP.PoolError, match="TRAIN era ends after the TRAIN end"):
        EP.check_windows([("E3", TRAIN_BEGIN, ns("2023-06-01"))], TRAIN_BEGIN, early_end, SEAL)
    with pytest.raises(EP.PoolError, match="no era"):
        EP.check_windows([], TRAIN_BEGIN, TRAIN_END, SEAL)


def test_the_repository_window_refuses_an_era_past_its_seal():
    import research_window as rw
    doc = rw.current()                                         # the JSON's values (a harness bind cannot move them)
    with pytest.raises(EP.PoolError, match="sealed"):
        EP.check_windows([("E3", doc["TRAIN_BEGIN_NS"], doc["SEAL_NS"] + DAY)], doc["TRAIN_BEGIN_NS"],
                         doc["TRAIN_END_NS"], doc["SEAL_NS"])


def test_segment_starts_and_refusals():
    assert EP.segment_starts([("E1", [1, 2, 3]), ("E2", [5, 6])]) == [0, 3]
    for eras, needle in (([("E1", [1, 2]), ("E2", [2, 3])], "not after"), ([("E1", [3, 1])], "strictly increasing"),
                         ([("E1", [])], "no session"), ([("E1", [1, 2]), ("E2", [0, 5])], "not after")):
        with pytest.raises(EP.PoolError, match=needle):
            EP.segment_starts(eras)


def test_pool_rows_concatenates_in_order_with_segments():
    a = {"session_ns": np.array([1.0, 2.0]), "x": np.array([0.1, 0.2]), "tag": ["a", "b"]}
    b = {"session_ns": np.array([5.0]), "x": np.array([0.5]), "tag": ["c"]}
    got = EP.pool_rows([("E1", a), ("E2", b)])
    assert list(got) == ["session_ns", "x", "tag", EP.SEGMENTS]
    assert got["x"].tolist() == [0.1, 0.2, 0.5] and got["tag"] == ["a", "b", "c"] and got[EP.SEGMENTS] == [0, 2]
    one = EP.pool_rows([("E1", a)])
    assert np.array_equal(one["x"], a["x"]) and one[EP.SEGMENTS] == [0]
    with pytest.raises(EP.PoolError, match="columns differ"):
        EP.pool_rows([("E1", a), ("E2", {"x": b["x"], "session_ns": b["session_ns"], "tag": b["tag"]})])
    with pytest.raises(EP.PoolError, match="not after"):
        EP.pool_rows([("E2", b), ("E1", a)])


def test_pool_matrix():
    s, m, starts = EP.pool_matrix([("E1", [1, 2], np.array([[1.0, 2.0], [3.0, 4.0]])),
                                   ("E2", [7], np.array([[5.0], [6.0]]))])
    assert s.tolist() == [1, 2, 7] and m.tolist() == [[1.0, 2.0, 5.0], [3.0, 4.0, 6.0]] and starts == [0, 2]
    with pytest.raises(EP.PoolError, match="different numbers of rows"):
        EP.pool_matrix([("E1", [1], np.zeros((2, 1))), ("E2", [2], np.zeros((3, 1)))])
    with pytest.raises(EP.PoolError, match="matrix columns"):
        EP.pool_matrix([("E1", [1, 2], np.zeros((2, 1)))])


def test_one_era_identities():
    x = 0.1 + 0.2                                      # a value no weighted arithmetic would reproduce exactly
    assert EP.weighted_mean([x], [17]) is x
    assert EP.weighted_mean([1.0, 3.0], [1, 3]) == pytest.approx(2.5, abs=1e-15)
    for bad in (([1.0], []), ([1.0, 2.0], [0, 0]), ([1.0, 2.0], [-1, 2])):
        with pytest.raises(EP.PoolError):
            EP.weighted_mean(*bad)
    sha = "ab" * 32
    assert EP.pooled_sha256([sha]) == sha                     # a one-era pool is the era: the same trial
    two = EP.pooled_sha256([sha, "cd" * 32])
    doc = json.dumps({"schema": "atx.era-pool/v1", "parts": [sha, "cd" * 32]}, sort_keys=True, separators=(",", ":"))
    assert two == hashlib.sha256(doc.encode()).hexdigest() != EP.pooled_sha256(["cd" * 32, sha])
    assert EP.pool_label(["a/N-E1", "a\\N-E2"]) == "pool:a/N-E1,a/N-E2"
    assert EP.era_weights_name("E1") == "composition_weights.E1.json"
    with pytest.raises(EP.PoolError):
        EP.era_weights_name("../x")


def test_import_is_standard_library_only():
    """research_cycle imports era_pool through research_roles: numpy loads only inside the array functions."""
    text = (Path(__file__).resolve().parent / "era_pool.py").read_text(encoding="utf-8")
    top = [line for line in text.splitlines() if line.startswith(("import ", "from "))]
    assert top == ["from __future__ import annotations", "import hashlib", "import json", "import re"]
