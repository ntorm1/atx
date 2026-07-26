# Temporal agent-memory patterns (2026-07-18)

Primary sources:

- Graphiti overview: https://help.getzep.com/graphiti/getting-started/welcome
- Zep graph model: https://help.getzep.com/v2/understanding-the-graph
- Zep versus Graphiti: https://help.getzep.com/zep-vs-graphiti
- GraphRAG indexing: https://microsoft.github.io/graphrag/index/overview/

Graphiti treats raw agent inputs as immutable episodes and incrementally builds
entities and facts without batch recomputation. Its distinguishing model is
bi-temporal: facts have a validity interval in the represented world as well as
the time at which the database learned them. New evidence can invalidate a fact
without erasing history. Zep organizes episodic nodes, entity nodes, and factual
entity edges, while GraphRAG derives text units, entities, relationships,
claims, summaries, and embeddings.

This model is directly relevant to autonomous development agents. They need to
distinguish current decisions from superseded ones, reconstruct what was known
when a change was made, and preserve conflicting evidence rather than silently
overwriting it. They also need project, run, agent, task, artifact, observation,
and decision identities so retrieved context can be scoped to the current job.

Immediate atx gaps: the current DAG has creation time but no valid-time model,
no supersession/retraction, no episodes or agent runs, no task coordination,
and no namespace isolation. The next higher-level `atx-db` layer should use
`atx-kb` as evidence storage while adding temporal facts and durable agent work
coordination.

Measurable gates:

- An assertion can be superseded without deleting either version.
- As-of-valid-time and as-of-transaction-time queries return the expected fact.
- Concurrent agents can lease tasks without double ownership.
- Every observation/decision links back to an immutable atx-kb source/chunk.
- Namespace filters are mandatory at the storage boundary.
