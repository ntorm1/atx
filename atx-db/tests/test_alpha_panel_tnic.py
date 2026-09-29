"""TNIC-style text industries (lane TXT, S7.2): vocabulary, bitset cosine, calibration, PIT clocks, build."""

from __future__ import annotations

import datetime as dt
import itertools
from pathlib import Path
from typing import Any

import numpy as np
import pyarrow as pa
import pyarrow.parquet as pq
import pytest

from atx_db.alpha_panel import filing_text as FT
from atx_db.alpha_panel import tnic as T


def test_vocabulary_words_filters_stop_geo_and_short() -> None:
    text = "The Company sells Pumps and valves in Texas and Europe; it is an OEM of 3D pumps."
    assert T.vocabulary_words(text) == ["oem", "pumps", "sells", "valves"]


def test_sic3_pair_share() -> None:
    share, n = T.sic3_pair_share(["3561", "3562", "2834", None, "0000", "2834"])
    # groups 356: 2, 283: 2 -> 2 same pairs of C(4, 2) = 6
    assert n == 4 and share == pytest.approx(2 / 6)
    assert T.sic3_pair_share(["3561"]) == (None, 1)


def test_threshold_from_hist() -> None:
    hist = np.zeros(T.HIST_BINS, dtype=np.int64)
    scores = np.linspace(0.0, 0.999, 1000)
    np.add.at(hist, (scores * T.HIST_BINS).astype(int), 1)
    tau = T.threshold_from_hist(hist, 0.1)
    assert 0.89 <= tau <= 0.91
    assert (scores >= tau).mean() <= 0.1 + 1e-9


def _brute(docs: list[set[int]]) -> dict[tuple[int, int], float]:
    out = {}
    for i, j in itertools.combinations(range(len(docs)), 2):
        a, b = docs[i], docs[j]
        out[(i, j)] = len(a & b) / np.sqrt(len(a) * len(b))
    return out


def test_bitset_pair_scores_match_brute_force() -> None:
    rng = np.random.default_rng(7)
    docs = [set(rng.choice(300, size=rng.integers(20, 80), replace=False).tolist()) for _ in range(25)]
    ids = [np.array(sorted(d), dtype=np.int32) for d in docs]
    sizes = np.array([len(d) for d in docs])
    hist, kept, best = T.pair_scores(T.bitsets(ids, 300), sizes, 0.2)
    ref = _brute(docs)
    assert int(hist.sum()) == len(ref)
    assert {(i, j) for i, j, _ in kept} == {k for k, v in ref.items() if v > 0.2}
    for i, j, s in kept:
        assert s == pytest.approx(ref[(i, j)])
    for i in range(len(docs)):
        assert best[i] == pytest.approx(max(v for k, v in ref.items() if i in k))


def test_pair_scores_top_k_neighbours() -> None:
    rng = np.random.default_rng(11)
    docs = [set(rng.choice(200, size=rng.integers(20, 60), replace=False).tolist()) for _ in range(30)]
    ids = [np.array(sorted(d), dtype=np.int32) for d in docs]
    top = (np.full((30, 3), -1.0), np.full((30, 3), -1, dtype=np.int64))
    T.pair_scores(T.bitsets(ids, 200), np.array([len(d) for d in docs]), None, top=top)
    ref = _brute(docs)
    for i in range(30):
        mine = sorted(((v, j) for (a, b), v in ref.items() if i in (a, b) for j in ((b if a == i else a),)),
                      reverse=True)[:3]
        assert sorted(top[0][i], reverse=True) == pytest.approx([v for v, _ in mine])


def test_pair_scores_exclude_same_accession() -> None:
    ids = [np.array([1, 2, 3], dtype=np.int32)] * 3
    hist, kept, _ = T.pair_scores(T.bitsets(ids, 8), np.array([3, 3, 3]), 0.5, exclude=np.array([0, 0, 1]))
    assert {(i, j) for i, j, _ in kept} == {(0, 2), (1, 2)} and int(hist.sum()) == 2


def test_encode_and_df_cap() -> None:
    docs = [{"accession": f"a{i}", "ids": np.array(ids, dtype=np.int32)}
            for i, ids in enumerate([[1, 2, 5], [1, 3, 5], [1, 4, 6], [1, 2, 7]])]
    common = T._df_common(docs)
    assert common.tolist() == [1, 2, 5]  # 1 in 4 of 4 docs, 2 and 5 in 2 of 4: all above 25%
    docs2 = docs + [{"accession": f"b{i}", "ids": np.array([100 + i], dtype=np.int32)} for i in range(8)]
    assert T._df_common(docs2).tolist() == [1]  # 4 of 12 > 3
    enc, sizes, vocab = T._encode(docs, np.array([1], dtype=np.int32))
    assert sizes.tolist() == [2, 2, 2, 2]
    assert vocab == 2  # only 2 and 5 are shared by 2+ docs after the cap
    assert [e.tolist() for e in enc] == [[0, 1], [1], [], [0]]


# ------------------------------------------------------------------ end-to-end build on a temp lake

PUMP = ("pumps valves impellers seals hydraulic compressors turbines bearings castings motors gearboxes "
        "flowmeters actuators couplings diaphragms nozzles manifolds strainers ")
BANK = ("deposits loans mortgages checking savings branches lending underwriting treasury brokerage "
        "annuities escrow overdraft remittance custody refinancing amortization collateral ")
SHOP = ("apparel footwear handbags cosmetics fragrances jewelry outlets merchandise storefronts shoppers "
        "markdowns catalogs boutiques accessories swimwear outerwear denim sneakers ")
BIO = ("antibodies oncology biologics clinical trials molecules enzymes proteins genomics vaccines assays "
       "biomarkers peptides cytokines receptors inhibitors antigens plasmids ")


def _letters(n: int) -> str:
    out = ""
    for _ in range(3):
        out += "abcdefghijklmnopqrstuvwxyz"[n % 26]
        n //= 26
    return out


def _vocab(base: str, k: int) -> str:
    extra = " ".join(f"own{_letters(k)}{_letters(i)}" for i in range(40))  # firm-specific words
    filler = " ".join(f"gen{_letters(i)}" for i in range(30))  # used by every firm: removed by the DF cap
    return f"{base} {extra} {filler}"


def _landing(tmp: Path, root: Path) -> None:
    rows, filings, links = [], [], []
    ciks = list(range(1, 13))
    for year in (2018, 2019):
        for n, cik in enumerate(ciks):
            base = (PUMP, BANK, BIO, SHOP)[n % 4]
            acc = f"00000000{cik:02d}-{year % 100:02d}-000001"
            fd = dt.date(year, 2, 1) + dt.timedelta(days=n)
            clock = dt.datetime(year, 2, 1, 21, 0) + dt.timedelta(days=n)
            rows.append({"accession": acc, "cik": cik, "ciks": [cik], "form": "10-K", "filing_date": fd,
                         "report_date": None, "available_at": clock, "section": "business", "item": "1",
                         "method": "item", "flags": [], "n_chars": 0, "source": "primary", "parser": "t",
                         "text": _vocab(base, cik)})
            filings.append({"cik": cik, "accession": acc, "form": "10-K", "filing_date": fd})
        # a firm alike to nobody: its own words plus two of firm 1's -> no TNIC-3 peer, flagged nearest neighbours
        acc = f"0000000013-{year % 100:02d}-000001"
        lone = " ".join(f"solo{_letters(i)}" for i in range(60)) + f" own{_letters(1)}{_letters(0)} own{_letters(1)}{_letters(1)}"
        rows.append({"accession": acc, "cik": 13, "ciks": [13], "form": "10-K", "filing_date": dt.date(year, 3, 1),
                     "report_date": None, "available_at": dt.datetime(year, 3, 1, 21, 0), "section": "business",
                     "item": "1", "method": "item", "flags": [], "n_chars": 0, "source": "primary", "parser": "t",
                     "text": lone})
        filings.append({"cik": 13, "accession": acc, "form": "10-K", "filing_date": dt.date(year, 3, 1)})
    for cik in [*ciks, 13]:
        links.append({"security_id": cik, "cik": cik, "valid_from": dt.date(2018, 1, 1),
                      "valid_to": dt.date(2026, 1, 1), "ever_member": True})
    (tmp / "sections").mkdir(parents=True)
    pq.write_table(pa.Table.from_pylist(rows, schema=FT.SECTION_SCHEMA), tmp / "sections" / "part-00001.parquet")
    (root / "sec_filings").mkdir(parents=True)
    (root / "identity").mkdir(parents=True)
    pq.write_table(pa.Table.from_pylist(filings), root / "sec_filings" / "filings.parquet")
    pq.write_table(pa.Table.from_pylist(links), root / "identity" / "link_table.parquet")
    sic = {**{n: ("3561", "6022", "2834", "5651")[(n - 1) % 4] for n in ciks}, 13: "9999"}
    pq.write_table(pa.Table.from_pylist([{"cik": c, "sic": s} for c, s in sic.items()]),
                   root / "sec_filings" / "issuer_profile.parquet")


def test_build_end_to_end(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    land, root = tmp_path / "land", tmp_path / "lake"
    monkeypatch.setenv("ATX_SEC_TEXT_ROOT", str(land))
    monkeypatch.setenv("ATX_ALPHA_PANEL_ROOT", str(root))
    _landing(land, root)
    out = T.build(2019)
    years = {r["year"]: r for r in out["years"]}
    assert years[2018]["published"] is False and years[2019]["published"] is True
    assert years[2019]["calibrated_on"] == 2018
    assert years[2019]["coverage_linked_with_peers"] == pytest.approx(12 / 13, abs=1e-4)
    assert years[2019]["coverage_linked_with_peer_set"] == 1.0
    allpairs = pq.read_table(root / T.STAGE / "pairs" / "year=2019.parquet").to_pylist()
    lone = [p for p in allpairs if p["cik"] == 13]
    assert lone and all(p["peer_basis"] == T.FALLBACK_BASIS for p in lone)
    assert {p["peer_cik"] for p in lone} == {1}  # the only firm it shares words with
    assert all(p["available_at"] == dt.datetime(2019, 3, 1, 21, 0) for p in lone)  # the year's last filing clock
    pairs = [p for p in allpairs if p["peer_basis"] == "tnic3"]
    groups = {c: (c - 1) % 4 for c in range(1, 13)}
    assert all(p["cik"] != 13 and p["peer_cik"] != 13 for p in pairs)
    assert pairs and all(groups[p["cik"]] == groups[p["peer_cik"]] for p in pairs)  # peers within the industry
    for p in pairs:  # clock = the later 10-K's acceptance
        clocks = {r["cik"]: r for r in pq.read_table(root / T.STAGE / "firms.parquet").to_pylist()}
        assert p["available_at"] == max(clocks[p["cik"]]["filing_available_at"],
                                        clocks[p["peer_cik"]]["filing_available_at"])
    firms = pq.read_table(root / T.STAGE / "firms.parquet").to_pylist()
    assert len(firms) == 13 and all(f["n_peers"] == 2 for f in firms if f["cik"] != 13)
    first = min(firms, key=lambda f: f["filing_available_at"])
    assert first["n_peers_own_clock"] == 0  # its peers filed later
    assert (root / T.STAGE / "manifest.json").exists()
