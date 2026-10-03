"""P9 lane AL-SIG's blind registrations: the two queue candidates (scripts/specs/p9/candidates/gia_13f.json,
russell_recon.json) and the two blocked_on_data registrations (scripts/specs/p9/blocked/lazy_prices.json,
hf_crowd.json).

Each is a valid wave candidate registration (wave_manifest.candidate_problems, the queue's own checks for the two
candidates), carries the K-P9-11 keys, the horizon class and half-life of PM8-5 and the prior sign inside its string;
a candidate reads exactly its field, an engine-only row of the committed field registry (DEC-5) whose builder kind is
the field; a blocked registration names no field the registry holds and is not in the queue. At most 10 strings.

Run: "C:/Program Files/Python312/python.exe" -m pytest -q -p no:cacheprovider
     scripts/tests/test_p9_al_sig_registrations.py
"""
from __future__ import annotations

import json
from pathlib import Path
import re
import sys

import pytest

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
sys.path.insert(0, str(HERE.parent))
import wave_manifest as WM  # noqa: E402
import wave_queue as Q  # noqa: E402

QUEUE = "scripts/specs/p9/candidates"
BLOCKED = ROOT / "scripts" / "specs" / "p9" / "blocked"
REGISTRY = ROOT / "atx-engine" / "tools" / "field_registry.json"
CANDIDATES = ("gia_13f", "russell_recon")
BLOCKED_IDS = ("lazy_prices", "hf_crowd")
HORIZON = re.compile(r"horizon class (fast|medium|slow), half-life (\d+) sessions \(PM8-5")


def queue_docs() -> dict:
    return {cid: doc for cid, doc in Q.load(ROOT, QUEUE).items() if cid in CANDIDATES}


def blocked_docs() -> dict:
    return {p.stem: json.loads(p.read_text(encoding="utf-8")) for p in sorted(BLOCKED.glob("*.json"))}


def field_rows() -> dict:
    return {r["name"]: r for r in json.loads(REGISTRY.read_text(encoding="utf-8"))["fields"]}


def check_registration(cid: str, doc: dict) -> None:
    assert doc["id"] == cid and doc["lane"] == "AL-SIG" and doc["report"] == "task-AL-SIG-report.md"
    assert doc["origin"] == "prior" and doc["hypothesis"] == cid and doc.get("kind", "add") == "add"
    for key in WM.REGISTRATION_KEYS:                       # K-P9-11: every key filled
        assert key in doc, (cid, key)
    assert WM.registration_problems(doc, cid) == []
    assert doc["theme"] in json.loads((ROOT / Q.REGISTRY).read_text(encoding="utf-8"))["themes"]
    m = HORIZON.search(doc["notes"])                       # PM8-5
    assert m, (cid, doc["notes"])
    lo, hi = {"fast": (1, 4), "medium": (5, 21), "slow": (22, 252)}[m.group(1)]
    assert lo <= int(m.group(2)) <= hi, (cid, m.groups())  # the class of the half-life (YSIG section 7)
    # The prior sign lives in the string: prior_sign is +1 and the DSL is rank(<field>) of the field as signed.
    assert doc["prior_sign"] == 1 and doc["dsl"] == f"rank({cid})" and doc["fields"] == [cid]
    assert doc["dsl_sha256"] == WM.dsl_sha256(doc["dsl"])


@pytest.mark.parametrize("cid", CANDIDATES)
def test_candidate_is_a_valid_queue_registration(cid):
    docs = queue_docs()
    assert cid in docs, f"{QUEUE}/{cid}.json is missing"
    doc = docs[cid]
    assert Q.doc_problems(cid, doc) == []
    assert doc["status"] == Q.PROPOSED and doc["wave"] is None
    assert [h["status"] for h in doc["history"]] == [Q.PROPOSED]
    check_registration(cid, doc)


def test_candidates_pass_the_queue_checks_together():
    # Duplicate DSLs, the alpha registry (an id or DSL registered under another id), hypothesis variants.
    assert Q.problems(ROOT, queue_docs()) == []
    by_id, by_dsl = Q.registry_index(ROOT)
    for cid, doc in queue_docs().items():
        assert cid not in by_id and doc["dsl_sha256"] not in by_dsl, cid


@pytest.mark.parametrize("cid", CANDIDATES)
def test_candidate_reads_an_engine_only_registry_row(cid):
    row = field_rows().get(cid)
    assert row is not None, f"{cid} is not a row of {REGISTRY.name}"
    assert row["kind"] == "engine" and row["builder"] == cid and row["dtype"] == "f64" and row["point_in_time"]
    assert row["requires"] == [] and row["first_session"] is None and "AL-SIG" in row["owner"]
    assert row["sources"] == [{"gia_13f": "thirteenf", "russell_recon": "indexes"}[cid]]
    assert re.fullmatch(r"[0-9a-f]{64}", row["formula_sha256"])
    assert set(row["spec_text"]) == {"clock", "definition", "domain", "non_pit_aspects", "point_in_time",
                                     "source_columns", "staleness", "units"}


@pytest.mark.parametrize("cid", BLOCKED_IDS)
def test_blocked_registration(cid):
    docs = blocked_docs()
    assert cid in docs, f"scripts/specs/p9/blocked/{cid}.json is missing"
    doc = docs[cid]
    where = f"scripts/specs/p9/blocked/{cid}.json"
    assert WM.candidate_problems(doc, where, ("schema", "status", "blocked_on", "history")) == []
    assert doc["schema"] == "atx.p9-blocked-registration/v1" and doc["status"] == "blocked_on_data"
    assert isinstance(doc["blocked_on"], str) and doc["blocked_on"].strip()
    assert [h["status"] for h in doc["history"]] == ["blocked_on_data"]
    assert doc["data_class"] == "P"                         # partly landed (lit section 3.1 F1, F2)
    check_registration(cid, doc)
    # No field exists yet: not a registry row, and not a queue candidate a wave could pin.
    assert cid not in field_rows()
    assert cid not in Q.load(ROOT, QUEUE)


def test_blocked_dir_holds_only_the_two_and_the_lane_stays_within_ten_strings():
    assert sorted(blocked_docs()) == sorted(BLOCKED_IDS)
    lane = [d for d in Q.load(ROOT, QUEUE).values() if isinstance(d, dict) and d.get("lane") == "AL-SIG"]
    assert len(lane) + len(blocked_docs()) <= 10
    dsls = [d["dsl_sha256"] for d in lane] + [d["dsl_sha256"] for d in blocked_docs().values()]
    assert len(set(dsls)) == len(dsls)
