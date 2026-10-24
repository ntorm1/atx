"""Exact bridge to the reviewed seed replay; no fit or timing qualification."""
import argparse
import collections
import json
from pathlib import Path
import sys

import capture_audit as audit

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "cold-seed"))
import replay


def extract(capture):
    summary = audit.validate_capture(capture)
    rows, joins, without_proof, groups = [], [], [], []
    previous = None
    for record in capture["records"]:
        trace = record["trace"]
        proofs = [p for p in trace["phases"] if p["phase"] in (1, 3, 5) and p["visited"]]
        if not proofs:
            without_proof.append(dict(sequence=record["sequence"], outcome=trace["outcome"],
                                      phase_outcomes=[p["outcome"] for p in trace["phases"]]))
        for phase in proofs:
            # The component accepts only finite, positive seeds. Refuse an
            # unsupported extraction instead of silently losing its denominator.
            sigma = audit.real(phase["seed_sigma"])
            audit.require(sigma > .005, "proof seed outside replay domain")
            raw = phase["options"]
            resolved = dict(n_collocation=12, n_quadrature=24, n_quad_price=48,
                            max_newton_iter=6, tol=1e-10, profile=0) if raw is None else {
                **raw, "tol": audit.real(raw["tol"])}
            audit.require(resolved["profile"] == 0, "proof is not Reference")
            key = (record["cache_id"], json.dumps(raw, sort_keys=True),
                   trace["options"]["tol"]["bits"])
            if key != previous:
                groups.append(dict(batch=len(groups), cache_id=record["cache_id"],
                                   options=raw, sequences=[]))
                previous = key
            request = trace["request"]
            row = {k: audit.real(request[k]) for k in ("S", "K", "T", "r", "q", "budget")}
            row.update(batch=len(groups)-1, index=len(rows), target=audit.real(request["price"]),
                       tol=audit.real(trace["options"]["tol"]), sigma=sigma,
                       side=request["side"], accepted=phase["outcome"] == 1,
                       opts_explicit=raw is not None,
                       opts_tol=resolved["tol"], **{k: resolved[k] for k in (
                           "n_collocation", "n_quadrature", "n_quad_price", "max_newton_iter", "profile")})
            joins.append(dict(index=len(rows), sequence=record["sequence"],
                              phase=phase["phase"], seed_bits=phase["seed_sigma"]["bits"]))
            rows.append(row)
            groups[-1]["sequences"].append(record["sequence"])
    result = dict(schema="atx.cold-seed.requests.v1", requests=rows,
                  extraction=dict(capture_attempts=summary["attempts"],
                      capture_accepted=summary["accepted"], capture_refused=summary["refused"],
                      proof_requests=len(rows), calls_without_proof=without_proof,
                      source_join=joins, groups=groups,
                      exact_expiry_models=len({replay.model_key(r) for r in rows}),
                      side_counts=dict(collections.Counter(r["side"] for r in rows)),
                      timing_qualified=False,
                      scope="Proof component only. Original order and contiguous cache/options groups; "
                            "each replay group starts cold. Interleaved direct seed calls are absent, "
                            "so warm-cache sequencing and whole-fit equivalence are not established."))
    replay.requests(result)
    return result


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("capture", type=Path)
    parser.add_argument("out", type=Path)
    args = parser.parse_args()
    audit.require(not args.out.exists(), "output already exists")
    result = extract(audit.read_json(args.capture))
    result["source_capture_sha256"] = audit.sha256(args.capture)
    result["extractor_sha256"] = audit.sha256(__file__)
    with args.out.open("x", encoding="utf-8") as output:
        json.dump(result, output, indent=2, allow_nan=False)
        output.write("\n")


if __name__ == "__main__":
    main()
