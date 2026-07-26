# Retrieval and memory evaluation basis (2026-07-18)

Primary sources:

- BEIR paper: https://arxiv.org/abs/2104.08663
- MTEB paper: https://aclanthology.org/2023.eacl-main.148/
- LoCoMo paper: https://arxiv.org/abs/2402.17753
- RAGChecker paper: https://arxiv.org/abs/2408.08067

BEIR evaluates heterogeneous retrieval across domains and uses NDCG@10 as a
primary measure; its results establish BM25 as a robust baseline and show that
late-interaction/reranking can improve quality at higher compute cost. MTEB
separates retrieval from other embedding tasks and exists because narrow model
evaluations do not generalize. LoCoMo measures long-term agent memory across
roughly 300-turn, multi-session conversations and includes temporal and causal
questions. RAGChecker separates retrieval and generation diagnostics at claim
level instead of reporting only end-to-end answer similarity.

The atx program should therefore use layered gates: deterministic unit fixtures
for invariants, golden relevance queries for ranking quality, large synthetic
corpora for performance, temporal episode fixtures for memory correctness, and
claim/citation checks for context quality. A single latency number or a handful
of happy-path searches cannot substantiate a state-of-the-art claim.

Initial checked-in evaluation gates should include precision@k, recall@k, MRR,
NDCG@k, citation coverage, tenant isolation, temporal-as-of accuracy, exact
versus ANN recall, p50/p95 latency, ingest throughput, reopen durability, and
crash/snapshot recovery. Results should be emitted as stable JSON so every
self-improvement cycle can compare against its predecessor.
