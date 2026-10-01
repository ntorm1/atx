# Factory admission evidence

`Factory::mine_into` now requires an explicit `ProductionAdmissionPolicy`.
The default `FactoryConfig`, including `oos_fraction=0`, returns an error before
search or library mutation. Use `mine_research_into` for exploratory persistence;
its report always says `AdmissionEvidence::ResearchOnly`, including when a
terminal validation window is configured. `mine()` is also research only.
`ResearchDriver` and the `atx-impl` discovery stage use this explicit research
entry. Discovery manifests and stage output say `admission_evidence=research_only`.

For independent statistical confirmation, provide a named policy, a dataset
identity, a requester, an open `eval::FileLockboxAudit`, and an explicitly declared
prior trial count (zero must be stated). Configure a terminal holdout and an
embargo covering the declared maximum label horizon plus the one-session stream
lag. The policy defaults to at least 20 held-out dates; a configured minimum
cannot be less than three. The final holdout DSR uses prior plus current trials.

The production entry searches and ranks exclusively on the visible training
prefix. It freezes the complete candidate family and order, then opens the
holdout once through `open_lockbox`. That operation atomically rejects previously
opened reservations or overlapping held-out date content and flushes the audit
receipt before returning the held-out panel. No candidate is inserted before
this succeeds. A new seed or fresh library cannot reuse that holdout with the
same durable audit. Holdout failures do not permit a retry on the same data.

The durable receipt contains policy and dataset identifiers, actual weight and
stream semantics, admission thresholds, training/embargo geometry, search digest,
seed, prior/total trial counts, and every frozen canonical candidate identity.
Its candidate commitment additionally hashes each expression and family order.
The reservation binds the actual panel values; an additional axis hash binds
field names and universe masks. `admission_policy_hash`, `admission_family_hash`,
`admission_receipt_hash`, and `admission_audit_receipts` in the report locate and
anchor that evidence. Accepted records link to the frozen family through their
canonical hashes. Their lifecycle admission date is the last held-out panel date,
not the historical research placeholder of period one. Library membership alone
does not establish independent qualification; retain the audit alongside it and
export its receipt-count/hash anchor outside the audit log.

`IndependentHoldout` describes this statistical confirmation protocol. It does
not certify an executable or live-ready strategy. The supported confirmation
stream is `LegacyStreamsV1`: previous-session weights earn close-to-close returns,
with only the configured per-dollar turnover commission. The durable metadata
explicitly identifies execution replay, slippage, impact and borrow as unavailable
or unmodeled. Decay is part of the expression; the weight transform, neutrality,
gross leverage, truncation and winsorization are recorded from the actual policy.

Current restrictions are deliberate errors, not silent fallbacks: production
requires a fresh library, a terminal rather than walk-forward window, no resumed
state or external weak/execution/residual context, and no approximate phenotype
deduplication or fidelity race. Audited production admission currently supports
only the in-process evaluator. Post-insert PBO blocking, marginal-IC admission,
and group-neutral confirmation without an aligned group map are unsupported.
The research entry preserves those historical research behaviors. PBO on the
production report is diagnostic and is not claimed as a blocking certificate.

Calendar/security identifiers and prior research history are declared by the
caller; `alpha::Panel` cannot authenticate their external source or prove that
someone never inspected the input previously. The durable audit must be shared
across the research programme. Creating a replacement audit, deleting records,
or changing the dataset field/instrument representation can evade its existing
content-based reuse guard. Secure audit retention, authenticated dataset lineage,
common-calendar alpha-pool confirmation, delayed net-execution qualification,
borrow/capacity checks and live operational approval remain acceptance work.

Compatibility: old calls to `mine_into` must either provide the production policy
or migrate explicitly to `mine_research_into`. Research search/admission digests
and the library wire schema are unchanged by that rename. Production digests also
bind the receipt, so they include audit history. Historical research tests were
migrated mechanically; the focused `FactoryProductionAdmission.*` checks cover
default refusal, unsupported inputs, durable one-use confirmation, and unchanged
selection when only the held-out outcomes change.
