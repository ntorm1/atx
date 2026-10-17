"""S9 (COV-9): the role's universe.delisting.returns_applied flag and the per-event flags; booleans and counts only."""
import hashlib, json, sys
p, pin = sys.argv[1], sys.argv[2]
raw = open(p, "rb").read()
assert hashlib.sha256(raw).hexdigest() == pin, "role manifest pin mismatch"
d = json.loads(raw)["universe"]["delisting"]
print(f"role manifest {p} sha256 {pin}")
print(f"universe.delisting keys {sorted(d)}")
print(f"universe.delisting.returns_applied = {str(d['returns_applied']).lower()}")
print(f"events with returns_applied true: {sum(1 for e in d['events'] if e.get('returns_applied'))} of {len(d['events'])}")
print("S9 HOLDS" if d["returns_applied"] is False else "S9 FAILS")
