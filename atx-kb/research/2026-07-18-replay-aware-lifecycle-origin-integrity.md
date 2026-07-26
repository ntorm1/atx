# Replay-aware lifecycle-origin integrity

Date: 2026-07-18

## Integrity gap

Schema v21 reserves three `consumer.*` event types for DLQ lifecycle origins and proves that each such event has an immutable lifecycle mapping. That inverse check currently applies to every redrive generation. If a separate monitor dead-letters and redrives a lifecycle control, the generation-one copy correctly preserves its type, subject, payload, root, and lineage but is not a second DLQ state transition. Treating the copy as another origin falsely reports an orphan lifecycle occurrence.

The CloudEvents primer distinguishes event identity from occurrence correlation and notes that replay support may reuse identity to recognize the replay; relays commonly forward the specification attributes unchanged. Amazon EventBridge similarly resends archived events to their source bus while adding `replay-name` metadata, and its archive rule explicitly filters replay-marked events so a replay is not mistaken for a new archive input. Both models preserve the domain envelope while using separate replay context to decide which origin-only rules apply.

Primary sources:

- CloudEvents primer, event identity and replay: https://github.com/cloudevents/spec/blob/main/cloudevents/primer.md
- CloudEvents specification, distinct event identity and duplicate delivery: https://github.com/cloudevents/spec/blob/main/cloudevents/spec.md
- Amazon EventBridge archive and replay metadata: https://docs.aws.amazon.com/eventbridge/latest/userguide/eb-archive.html

Atx-db already carries stronger replay context than an optional metadata field: every occurrence has `root_sequence` and `redrive_count`, and every positive generation has one exact redrive mapping. Origin-only lifecycle validation should therefore use generation zero, while the existing lineage validator governs replay copies.

## Contract correction

1. A generation-zero event using `consumer.dead_lettered`, `consumer.dead_letter_redriven`, or `consumer.dead_letter_quarantined` must have exactly one matching lifecycle mapping. A caller cannot forge a new origin in this reserved namespace.
2. The mapped lifecycle event must remain a self-rooted generation-zero occurrence with exact workspace, consumer subject, DLQ payload, transition type, and activation-boundary semantics.
3. A positive-generation event with a lifecycle type is a replay, not another state transition. It must have exactly one redrive parent and preserve the complete envelope while incrementing generation by one; the existing redrive-lineage invariant proves this.
4. Redriving a lifecycle control does not create or mutate a lifecycle mapping, DLQ state, or transition timestamp for the original subject consumer.
5. Separate monitors may receive the replay. Schema-v22 local-control suppression continues to prevent the target consumer from consuming either the original or replayed self control after its cutoff.
6. No schema migration is needed: persisted lineage and lifecycle mappings already encode the distinction. This is a verifier correction plus regression coverage.

## Test obligations

- a monitor can dead-letter and redrive another consumer's lifecycle control;
- the replay preserves type, target subject, payload, root, and increments generation;
- integrity accepts the replay through its redrive mapping without demanding a second lifecycle mapping;
- the original lifecycle mapping remains unique and unchanged;
- a forged generation-zero lifecycle type without a mapping still fails integrity;
- exact redrive retry and backup/restore preserve the replay identity.
