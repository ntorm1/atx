"""SQL3-1 evidence: the proposed classes.json rows, applied to an in-memory copy (classes.json is NOT edited).
Proposal: "registers": ["atx.run-store/v1"] on the run-receipt and run-start rows. Prints what the classes guard
would report with it: the unregistered literals and the shared-literal problems."""
import copy, datetime, json, sys
from pathlib import Path
REPO = Path(sys.argv[1])
sys.path.insert(0, str(REPO / "atx-engine" / "tools"))
import test_research_store_classes as G
reg = G.load()
prop = copy.deepcopy(reg)
for c in prop["classes"]:
    if c["id"] in ("run-receipt", "run-start"):
        c["registers"] = sorted(set(c.get("registers", [])) | {"atx.run-store/v1"})
found = G.scan(REPO)
today = datetime.date.today()
print("as committed: unregistered", sorted(G.unregistered(found, reg, today)))
print("with the proposal: unregistered", sorted(G.unregistered(found, prop, today)))
print("with the proposal: shared_literal_problems", G.shared_literal_problems(prop))
print("rows:", [json.dumps({k: c[k] for k in ("id", "registers")}) for c in prop["classes"] if "atx.run-store/v1" in c.get("registers", [])])
