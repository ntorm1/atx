"""Synthetic D1 evidence; isolated imports never load the warehouse facade."""
import datetime as dt
import importlib.util
import io
import json
from dataclasses import replace
from pathlib import Path
import sys
import tempfile
import types
import unittest

SOURCE = Path(__file__).resolve().parents[1] / "src/atx_db"
PKG = "_d1_synthetic"
package = types.ModuleType(PKG)
package.__path__ = [str(SOURCE)]
sys.modules[PKG] = package


def load(name):
    spec = importlib.util.spec_from_file_location(PKG + "." + name, SOURCE / (name + ".py"))
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


sl = load("security_link")
sec = load("sec_identity_sources")
D = dt.date
UTC = dt.timezone.utc
H = "a" * 64


def clock(day):
    return dt.datetime(2013, 1, day, 18, tzinfo=UTC)


def decision(day):
    return clock(day) + dt.timedelta(hours=4)


def filing(name, day, cik="1", available=None, **kw):
    fields = dict(evidence_id=name, accession="acc-" + name, cik=cik, ticker="XYZ",
                  effective_date=D(2013, 1, day), accepted_at=clock(day),
                  source_sha256=H, source_locator="synthetic:" + name, observed_at=clock(31),
                  published_at=clock(available or day), revision_status="original-confirmed")
    fields.update(kw)
    return sl.FilingEvidence(**fields)


def vendor(name="v1", sr="7", begin=2, end=10, available=2):
    return sl.VendorInterval(name, sr, "xyz", D(2013, 1, begin), D(2013, 1, end),
                             clock(available), H, "synthetic:" + name, True)


class SecurityLinkTest(unittest.TestCase):
    def test_prospective_open_line_is_live_only_after_independent_corroboration(self):
        v = replace(vendor(), valid_to=None)
        first, second = filing("first", 2), filing("second", 5)
        links, gaps = sl.build_prospective_links([v], [first, second])
        self.assertFalse(gaps)
        self.assertEqual(len(links), 1)
        self.assertEqual((links[0].valid_from, links[0].valid_to, links[0].end_kind),
                         (D(2013, 1, 5), sl.SEAL, "seal-bound"))
        self.assertIsNone(sl.resolve_link(links, "7", D(2013, 1, 5), clock(5)).link)
        self.assertIsNotNone(sl.resolve_link(links, "7", D(2013, 1, 5), decision(5)).link)
        self.assertIsNotNone(sl.resolve_link(links, "7", D(2013, 1, 25), decision(25)).link)
        report = sl.coverage_report(links, [("7", D(2013, 1, 4), decision(4)),
                                           ("7", D(2013, 1, 25), decision(25))])
        self.assertEqual(report["linked"], 1)
        self.assertFalse(report["plan_coverage_qualified"])
        self.assertEqual((links, gaps), sl.build_prospective_links([v, v], [second, first, first]))

    def test_prospective_observations_must_be_inside_line_and_independent(self):
        v = replace(vendor(), valid_to=None)
        first = filing("first", 2)
        for second in [filing("second", 1), filing("second", 2),
                       filing("second", 5, accession=first.accession),
                       filing("second", 5, published_at=None)]:
            with self.subTest(second=second):
                links, gaps = sl.build_prospective_links([v], [first, second])
                self.assertFalse(links)
                self.assertTrue(gaps)

    def test_future_effective_conflict_and_late_known_expiry_preserve_prefix(self):
        v = replace(vendor(), valid_to=None)
        observations = [filing("a", 2), filing("b", 5)]
        original, _ = sl.build_prospective_links([v], observations)
        rival = filing("rival", 10, cik="2", accepted_at=clock(3), published_at=clock(4))
        conflicted, _ = sl.build_prospective_links([v], observations + [rival])
        self.assertEqual(sl.resolve_link(original, "7", D(2013, 1, 8), decision(8)).reason,
                         sl.resolve_link(conflicted, "7", D(2013, 1, 8), decision(8)).reason)
        self.assertIsNone(sl.resolve_link(conflicted, "7", D(2013, 1, 10), decision(10)).link)
        expiry = sl.VendorExpiry("expiry", "v1", D(2013, 1, 10), clock(15), H, "synthetic:expiry", True)
        expired, _ = sl.build_prospective_links([v], observations, [expiry])
        self.assertIsNotNone(sl.resolve_link(expired, "7", D(2013, 1, 12), decision(12)).link)
        self.assertIsNotNone(sl.resolve_link(expired, "7", D(2013, 1, 15), clock(15)).link)
        self.assertIsNone(sl.resolve_link(expired, "7", D(2013, 1, 15), decision(15)).link)
        self.assertIsNone(sl.resolve_link(expired, "7", D(2013, 1, 25), decision(25)).link)
        self.assertIsNone(next(x for x in expired if x.method == "prospective-expiry-v2").accepted_at)

    def test_known_early_expiry_waits_for_effective_date_and_unknown_clock_is_not_used(self):
        v = replace(vendor(), valid_to=None)
        observations = [filing("a", 2), filing("b", 5)]
        expiry = sl.VendorExpiry("expiry", "v1", D(2013, 1, 10), clock(4), H, "synthetic:expiry", True)
        links, _ = sl.build_prospective_links([v], observations, [expiry])
        self.assertIsNotNone(sl.resolve_link(links, "7", D(2013, 1, 9), decision(9)).link)
        self.assertIsNone(sl.resolve_link(links, "7", D(2013, 1, 10), decision(10)).link)
        unknown, gaps = sl.build_prospective_links([v], observations, [replace(expiry, availability_verified=False)])
        self.assertIsNotNone(sl.resolve_link(unknown, "7", D(2013, 1, 10), decision(10)).link)
        self.assertIn("expiry_availability_unverified", {g["reason"] for g in gaps})

    def test_finite_vendor_row_known_only_after_end_does_not_backfill(self):
        links, gaps = sl.build_prospective_links([vendor(available=15)], [filing("a", 2), filing("b", 5)])
        self.assertFalse(links)
        self.assertEqual(gaps[0]["reason"], "corroboration_after_known_expiry")

    def test_prospective_expiry_roundtrip_and_rule_version_binding(self):
        v = replace(vendor(), valid_to=None)
        expiry = sl.VendorExpiry("expiry", "v1", D(2013, 1, 10), clock(10), H, "synthetic:expiry", True)
        links, gaps = sl.build_prospective_links([v], [filing("a", 2), filing("b", 5)], [expiry])
        with tempfile.TemporaryDirectory(prefix="atx-d1-v2-") as tmp:
            path = Path(tmp) / "links"
            digest = sl.write_link_artifact(path, links, gaps, {p: H for x in links for p in x.evidence_ids})
            loaded, manifest = sl.read_link_artifact(path, digest)
            self.assertEqual(loaded, links)
            self.assertEqual(manifest["schema"], "atx.security-link/v3")
            self.assertIn("prospective-two-filings-v2", manifest["rules"])
            manifest["schema"] = "atx.security-link/v1"
            (path / "manifest.json").write_text(json.dumps(manifest))
            with self.assertRaisesRegex(ValueError, "v3 conflict retirement"):
                sl.read_link_artifact(path)

    def test_expired_open_predecessor_releases_successor_only_after_both_retirement_clocks(self):
        prior = replace(vendor(), valid_to=None)
        successor = sl.VendorInterval("v2", "8", "XYZ", D(2013, 1, 10), None,
                                     clock(10), H, "synthetic:successor", True)
        observations = [filing("a", 2), filing("b", 5),
                        filing("c", 12, "2"), filing("d", 13, "2")]
        for effective, available in [(10, 15), (15, 5)]:
            with self.subTest(effective=effective, available=available):
                expiry = sl.VendorExpiry("expiry", "v1", D(2013, 1, effective), clock(available),
                                         H, "synthetic:expiry", True)
                links, _ = sl.build_prospective_links([prior, successor], observations, [expiry])
                self.assertIsNone(sl.resolve_link(links, "8", D(2013, 1, 14), decision(14)).link)
                self.assertIsNotNone(sl.resolve_link(links, "8", D(2013, 1, 15), decision(15)).link)
                self.assertIsNotNone(sl.resolve_link(links, "8", D(2013, 1, 25), decision(25)).link)
                self.assertIsNone(sl.resolve_link(links, "7", D(2013, 1, 25), decision(25)).link)
                if available == 15:
                    self.assertIsNone(sl.resolve_link(links, "8", D(2013, 1, 15), clock(15)).link)
                conflict = next(x for x in links if x.sr_id == "8" and x.retired_from)
                self.assertIn("expiry", conflict.evidence_ids)
                with self.assertRaisesRegex(ValueError, "both effective"):
                    replace(conflict, retired_available_at=None)
        with self.assertRaisesRegex(ValueError, "economic expiry dates"):
            sl.build_prospective_links([prior, successor], observations, [expiry,
                replace(expiry, evidence_id="contradiction", effective_date=D(2013, 1, 18))])

    def test_bracket_cannot_backdate_right_proof_or_revision(self):
        left, right = filing("left", 1), filing("right", 9, available=9)
        links, gaps = sl.build_links([vendor()], [right, left])
        self.assertFalse(gaps)
        self.assertEqual(len(links), 1)
        self.assertEqual(links[0].available_at, clock(9))
        self.assertEqual(links[0].cik, "0000000001")
        self.assertEqual(links[0].ticker, "XYZ")
        self.assertIsNone(sl.resolve_link(links, "7", D(2013, 1, 8), clock(8)).link)
        self.assertIsNone(sl.resolve_link(links, "7", D(2013, 1, 9), clock(9)).link)
        self.assertIsNone(sl.resolve_link(links, "7", D(2013, 1, 9), decision(9)).link)
        self.assertIsNotNone(sl.resolve_link(links, "7", D(2013, 1, 9), decision(9), retrospective_audit=True).link)
        self.assertIsNone(sl.resolve_link(links, "7", D(2013, 1, 10), clock(10)).link)
        revised = replace(right, revision_status="revision-confirmed", revision_available_at=clock(12))
        late, _ = sl.build_links([vendor()], [left, revised])
        self.assertEqual(late[0].available_at, clock(12))
        self.assertIsNone(sl.resolve_link(late, "7", D(2013, 1, 9), clock(9)).link)

    def test_missing_clocks_vintage_and_candidates_remain_unresolved(self):
        for mutation, reason in [({"accepted_at": None}, "missing_acceptance"),
                                 ({"published_at": None}, "public_availability_unverified"),
                                 ({"revision_status": "unverified"}, "unverified_identity_vintage"),
                                 ({"revision_status": "revision-confirmed"}, "missing_revision_clock"),
                                 ({"evidence_kind": "instance-prefix-candidate"}, "not_dated")]:
            with self.subTest(reason=reason):
                links, gaps = sl.build_links([vendor()], [filing("l", 1), filing("r", 9, **mutation)])
                self.assertFalse(links)
                self.assertIn(reason, gaps[0]["reason"])
        links, gaps = sl.build_links([replace(vendor(), valid_to=None)], [])
        self.assertEqual(gaps[0]["reason"], "unknown_interval_end")
        self.assertFalse(links)

    def test_duplicate_evidence_and_vendor_handling_is_deterministic(self):
        a, b, v, other = filing("a", 1), filing("b", 9), vendor(), vendor("v2", "8")
        original = sl.build_links([v, other], [a, b])
        self.assertEqual(original, sl.build_links([other, v, v], [b, a, a]))
        with self.assertRaisesRegex(ValueError, "duplicate filing"):
            sl.build_links([v], [a, replace(a, cik="2"), b])
        with self.assertRaisesRegex(ValueError, "duplicate vendor"):
            sl.build_links([v, replace(v, sr_id="99")], [a, b])

    def test_future_rival_preserves_prefix_and_overlapping_lines_fail_closed(self):
        v = vendor(end=20)
        a, b = filing("a", 1), filing("b", 19)
        rival = filing("rival", 10, cik="2", available=20)
        links, _ = sl.build_links([v], [a, b, rival])
        self.assertIsNotNone(sl.resolve_link(links, "7", D(2013, 1, 19), decision(19), retrospective_audit=True).link)
        self.assertEqual(sl.resolve_link(links, "7", D(2013, 1, 19), decision(20), retrospective_audit=True).reason,
                         "conflicting_available_identity_evidence")
        overlap, _ = sl.build_links([vendor(), vendor("v2", "8")], [filing("l", 1), filing("r", 9)])
        self.assertIsNone(sl.resolve_link(overlap, "7", D(2013, 1, 9), decision(9)).link)

    def test_dated_override_is_explicit_and_has_its_own_clock(self):
        proof = filing("proof", 1)
        header = "sr_id,cik,ticker,valid_from,valid_to,evidence_ids,reviewed_at,reviewer,reason\n"
        row = "7,1,xyz,2013-01-02,2013-01-20,proof,2013-01-10T18:00:00+00:00,reviewer,original proof\n"
        links = sl.read_overrides(io.StringIO(header + row), {"proof": proof})
        self.assertEqual(links[0].available_at, clock(10))
        self.assertIsNone(sl.resolve_link(links, "7", D(2013, 1, 9), clock(9)).link)
        self.assertIsNone(sl.resolve_link(links, "7", D(2013, 1, 10), clock(10)).link)
        self.assertIsNotNone(sl.resolve_link(links, "7", D(2013, 1, 10), decision(10)).link)
        with self.assertRaisesRegex(ValueError, "qualified"):
            sl.read_overrides(io.StringIO(header + row), {"proof": replace(proof, published_at=None)})

    def test_artifact_hash_binding_exclusivity_and_unqualified_coverage(self):
        links, gaps = sl.build_links([vendor()], [filing("a", 1), filing("b", 9)])
        sources = {key: H for link in links for key in link.evidence_ids}
        with tempfile.TemporaryDirectory(prefix="atx-d1-") as tmp:
            path = Path(tmp) / "links"
            digest = sl.write_link_artifact(path, links, gaps, sources)
            loaded, manifest = sl.read_link_artifact(path, digest)
            self.assertEqual(loaded, links)
            self.assertFalse(manifest["plan_coverage_qualified"])
            with self.assertRaises(FileExistsError):
                sl.write_link_artifact(path, links, gaps, sources)
            with self.assertRaisesRegex(ValueError, "checksum"):
                sl.read_link_artifact(path, "0" * 64)
            member = path / "links.jsonl"
            member.write_bytes(member.read_bytes()[:-1])
            with self.assertRaisesRegex(ValueError, "byte count"):
                sl.read_link_artifact(path)
        report = sl.coverage_report(links, [("7", D(2013, 1, 8), clock(8)),
                                           ("7", D(2013, 1, 9), decision(9))])
        self.assertEqual((report["requests"], report["linked"], report["rate"]), (2, 0, 0.0))
        self.assertFalse(report["plan_coverage_qualified"])
        self.assertIsNone(sl.coverage_report(links, [])["rate"])

    def test_source_adapters_use_issuer_not_accession_and_never_upgrade_fsds(self):
        source = sec.SourceProvenance(H, "synthetic:submission", clock(31))
        row = {"ACCESSION_NUMBER": "0000999999-13-000001", "ISSUERCIK": "1",
               "ISSUERTRADINGSYMBOL": "XYZ", "FILING_DATE": "09-JAN-2013"}
        missing = sec.insider_submission_rows([row], source, {})[0]
        self.assertEqual(missing.cik, "0000000001")
        self.assertEqual(missing.reason, "missing_acceptance")
        joined = {row["ACCESSION_NUMBER"]: sec.FilingClock(clock(9), clock(9), None,
                                                          "original-confirmed", "20130109180000")}
        self.assertEqual(sec.insider_submission_rows([row], source, joined)[0].available_at, clock(9))
        fsds = {"adsh": row["ACCESSION_NUMBER"], "cik": "1", "filed": "20130109",
                "accepted": "20130109180000", "instance": "xyz-20121231.xml", "prevrpt": "1"}
        candidate = sec.fsds_submission_rows([fsds], source, joined, UTC)[0]
        self.assertEqual(candidate.reason, "symbol_candidate_not_dated_issuer_evidence")
        self.assertIsNone(sec.acceptance_timestamp("20130109180000"))
        self.assertEqual(sec.acceptance_timestamp("20130109180000", UTC), clock(9))
        with self.assertRaises(KeyError):
            sec.dated_submission_rows([{"tickers": ["XYZ"]}], source, {})

    def test_schema_refuses_noncanonical_ambiguous_and_unsealed_inputs(self):
        with self.assertRaisesRegex(ValueError, "timezone"):
            filing("x", 1, accepted_at=dt.datetime(2013, 1, 1))
        with self.assertRaisesRegex(ValueError, "sealed"):
            filing("x", 1, effective_date=D(2020, 1, 1))
        self.assertNotEqual(sl.symbol_key("BRK.B"), sl.symbol_key("BRK-B"))
        links, _ = sl.build_links([vendor()], [filing("a", 1), filing("b", 9)])
        with self.assertRaisesRegex(ValueError, "canonical"):
            replace(links[0], cik="1")


if __name__ == "__main__":
    unittest.main()
