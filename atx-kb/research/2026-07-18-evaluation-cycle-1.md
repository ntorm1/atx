# Evaluation cycle 1: evidence quality gate (2026-07-18)

## Basis

- BEIR: https://arxiv.org/abs/2104.08663
- RAGChecker: https://arxiv.org/abs/2408.08067
- MTEB: https://aclanthology.org/2023.eacl-main.148/

The first dependency-free `atx-kb-eval` suite uses a fixed seed, 78 unique
single-chunk documents, and 41 queries. It separates lexical, selective-filter,
oracle-vector, hybrid-conflict, context-budget, citation, and abstention behavior.
The evaluator emits one deterministic JSON report and returns a failing exit code
when any production threshold is missed.

The initial baseline was honestly red. A 512-character context budget retained
no evidence, only 42.105% of context evidence was relevant, and the default local
embedding abstained on only 50% of unsupported queries. Other gates already
passed, including perfect lexical/filter/oracle-vector recall, hybrid NDCG@10,
citation validity, context source recall, and ranking determinism.

The measured fixes were narrow and policy-driven:

- context assembly now retains hits within 75% of the best fused score instead
  of treating every requested search result as prompt evidence;
- a compact JSON evidence record omits optional title/URI fields when the full
  record cannot fit the advertised context budget;
- the default minimum vector similarity increased from 0.15 to 0.35 to suppress
  weak collisions from the dependency-free local feature embedding.

After the changes, every unchanged threshold passes: context-budget source recall
is 12.5% (gate 10%), context evidence precision is 100% (gate 80%), and default
abstention is 100% (gate 95%). Lexical Recall@1, filtered Recall@1, oracle-vector
Recall@1, citation validity, context validity, source recall, lexical abstention,
and repeat determinism remain 100%; hybrid NDCG@10 remains 1.0 and filter leakage
remains zero.

Thirty consecutive eight-writer duplicate-submission stress runs also passed
after initialization stopped reissuing schema DDL and WAL mode changes on every
already-initialized connection.
