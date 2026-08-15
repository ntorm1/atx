export const meta = {
  name: 'vol-oracle-iter',
  description: 'One gated SpiderRock capability bootstrap or one exact-SHA oracle RSI iteration',
  whenToUse: 'Run exactly one oracle step. Bootstrap is one fixed implementation lane with independent review/fix/re-review/verification.',
  phases: [
    { title: 'Capability', detail: 'freeze canonical SHA and committed holdout digest receipt without opening membership' },
    { title: 'Bootstrap', detail: 'one fixed Build -> Review -> Fix -> Review -> scoped Verify -> atomic land' },
    { title: 'Measure', detail: 'ready only: aggregate smoke+tune Mode A+B' },
    { title: 'Attribute', detail: 'ready only: tool-less aggregate attribution' },
    { title: 'Improve', detail: 'ready only: gated vol-sprint exact integration SHA' },
    { title: 'Ratchet', detail: 'ready only: isolated exact-SHA holdout and atomic ACCEPT landing' },
  ],
}

const REQUESTED_BASE = (args && args.base) || 'main'
const FOCUS = (args && args.focus) || ''
const RUN_ID = `vol-oracle-${Date.now()}-${Math.random().toString(16).slice(2)}`
const RUN_SLUG = RUN_ID.replace(/[^A-Za-z0-9._-]/g, '-')
const CANONICAL_REF = 'refs/heads/oracle/canonical'
const ZERO_SHA = '0000000000000000000000000000000000000000'

const EVIDENCE_ITEM = {
  type: 'object', required: ['command', 'exit_code', 'output'],
  properties: {
    command: { type: 'string' }, exit_code: { type: 'integer' },
    output: { type: 'string', description: 'verbatim aggregate-only output; never licensed rows' },
  },
}
const FINDING = {
  type: 'object', required: ['location', 'severity', 'problem', 'fix'],
  properties: {
    location: { type: 'string' }, severity: { type: 'string', enum: ['blocker', 'major', 'minor'] },
    problem: { type: 'string' }, fix: { type: 'string' },
  },
}
const CAPABILITY = {
  type: 'object',
  required: ['state', 'canonical_ref', 'canonical_exists', 'base_ref', 'base_sha', 'holdout_digest_receipt', 'next_iter', 'evidence'],
  properties: {
    state: { type: 'string', enum: ['missing_data', 'missing_mode_a', 'missing_conventions', 'missing_mode_b', 'ready'] },
    canonical_ref: { type: 'string' }, canonical_exists: { type: 'boolean' },
    base_ref: { type: 'string' }, base_sha: { type: 'string' },
    holdout_digest_receipt: { type: 'string', description: 'committed 64-hex receipt; empty only for missing_data' },
    next_iter: { type: 'string' }, evidence: { type: 'array', items: EVIDENCE_ITEM },
    diagnostics: { type: 'array', items: EVIDENCE_ITEM },
  },
}
const BOOTSTRAP_REPORT = {
  type: 'object',
  required: ['state', 'outcome', 'branch', 'sha', 'base_sha', 'worktree', 'lease_name', 'lease_run_id', 'heartbeat_id', 'holdout_digest_receipt', 'evidence', 'deviations'],
  properties: {
    state: { type: 'string', enum: ['missing_data', 'missing_mode_a', 'missing_conventions', 'missing_mode_b'] },
    outcome: { type: 'string', enum: ['DONE', 'BLOCKED'] },
    branch: { type: 'string' }, sha: { type: 'string' }, base_sha: { type: 'string' },
    worktree: { type: 'string' }, lease_name: { type: 'string' }, lease_run_id: { type: 'string' },
    heartbeat_id: { type: 'string' }, holdout_digest_receipt: { type: 'string' },
    evidence: { type: 'array', items: EVIDENCE_ITEM }, diagnostics: { type: 'array', items: EVIDENCE_ITEM },
    deviations: { type: 'string' },
  },
}
const REVIEW = {
  type: 'object', required: ['verdict', 'reviewed_sha', 'evidence', 'findings'],
  properties: {
    verdict: { type: 'string', enum: ['APPROVE', 'BLOCK'] }, reviewed_sha: { type: 'string' },
    evidence: { type: 'array', items: EVIDENCE_ITEM }, diagnostics: { type: 'array', items: EVIDENCE_ITEM },
    findings: { type: 'array', items: FINDING },
  },
}
const CLEANUP = {
  type: 'object', required: ['passed', 'released', 'evidence'],
  properties: {
    passed: { type: 'boolean' }, released: { type: 'array', items: { type: 'string' } },
    evidence: { type: 'array', items: EVIDENCE_ITEM }, diagnostics: { type: 'array', items: EVIDENCE_ITEM },
  },
}
const BOOTSTRAP_GATE = {
  type: 'object',
  required: ['passed', 'reviewed_sha', 'integration_branch', 'integration_sha', 'integration_worktree', 'integration_lease', 'lease_run_id', 'integration_heartbeat_id', 'holdout_digest_receipt', 'canonical_ref', 'canonical_expected_old', 'canonical_after', 'next_state', 'leases_released', 'evidence'],
  properties: {
    passed: { type: 'boolean' }, reviewed_sha: { type: 'string' },
    integration_branch: { type: 'string' }, integration_sha: { type: 'string' },
    integration_worktree: { type: 'string' }, integration_lease: { type: 'string' },
    lease_run_id: { type: 'string' }, integration_heartbeat_id: { type: 'string' },
    holdout_digest_receipt: { type: 'string' }, canonical_ref: { type: 'string' },
    canonical_expected_old: { type: 'string' }, canonical_after: { type: 'string' },
    next_state: { type: 'string', enum: ['missing_mode_a', 'missing_conventions', 'missing_mode_b', 'ready'] },
    leases_released: { type: 'array', items: { type: 'string' } },
    evidence: { type: 'array', items: EVIDENCE_ITEM }, diagnostics: { type: 'array', items: EVIDENCE_ITEM },
  },
}
const MEASURE = {
  type: 'object',
  required: ['status', 'iter', 'scorecard_path', 'branch', 'sha', 'base_sha', 'worktree', 'lease_name', 'lease_run_id', 'heartbeat_id', 'lease_released', 'attribution_payload', 'summary', 'top_cells', 'evidence'],
  properties: {
    status: { type: 'string', enum: ['ok', 'failed'] }, iter: { type: 'string' },
    scorecard_path: { type: 'string' }, branch: { type: 'string' }, sha: { type: 'string' }, base_sha: { type: 'string' },
    worktree: { type: 'string' }, lease_name: { type: 'string' }, lease_run_id: { type: 'string' }, heartbeat_id: { type: 'string' },
    lease_released: { type: 'boolean' },
    attribution_payload: { type: 'string', description: 'self-contained aggregate smoke+tune scorecard/context; no paths, hashes, membership, or rows' },
    summary: { type: 'string' }, top_cells: { type: 'array', items: { type: 'string' } },
    evidence: { type: 'array', items: EVIDENCE_ITEM }, diagnostics: { type: 'array', items: EVIDENCE_ITEM },
  },
}
const METRIC_DELTA = {
  type: 'object',
  required: ['metric', 'baseline', 'candidate', 'delta', 'unit', 'evidence_index'],
  properties: {
    metric: { type: 'string' }, baseline: { type: 'number' }, candidate: { type: 'number' },
    delta: { type: 'number' }, unit: { type: 'string' }, evidence_index: { type: 'integer' },
  },
}
const ATTR = {
  type: 'object', required: ['hypotheses', 'new_suspect_candidates'],
  properties: {
    hypotheses: { type: 'array', items: {
      type: 'object', required: ['id', 'target_cells', 'mechanism', 'prediction', 'blast_radius', 'effort'],
      properties: {
        id: { type: 'string' }, target_cells: { type: 'array', items: { type: 'string' } },
        mechanism: { type: 'string' }, prediction: { type: 'string' },
        blast_radius: { type: 'string' }, effort: { type: 'string', enum: ['S', 'M', 'L'] },
      },
    } },
    new_suspect_candidates: { type: 'array', items: { type: 'string' } },
  },
}
const RATCHET = {
  type: 'object',
  required: ['verdict', 'tested_branch', 'tested_sha', 'base_sha', 'ratchet_branch', 'ratchet_sha', 'worktree', 'lease_name', 'lease_run_id', 'heartbeat_id', 'holdout_digest_verified', 'canonical_ref', 'canonical_expected_old', 'canonical_after', 'landed_sha', 'lease_released', 'holdout_summary', 'headline_metric_deltas', 'oracle_suspects_excluded', 'hypotheses_confirmed', 'hypotheses_refuted', 'ledger_appended', 'northstar_updated', 'evidence'],
  properties: {
    verdict: { type: 'string', enum: ['ACCEPT', 'REJECT'] },
    tested_branch: { type: 'string' }, tested_sha: { type: 'string' }, base_sha: { type: 'string' },
    ratchet_branch: { type: 'string' }, ratchet_sha: { type: 'string' },
    worktree: { type: 'string' }, lease_name: { type: 'string' }, lease_run_id: { type: 'string' }, heartbeat_id: { type: 'string' },
    holdout_digest_verified: { type: 'string' }, canonical_ref: { type: 'string' },
    canonical_expected_old: { type: 'string' }, canonical_after: { type: 'string' }, landed_sha: { type: 'string' },
    lease_released: { type: 'boolean' }, holdout_summary: { type: 'string' },
    headline_metric_deltas: { type: 'array', items: METRIC_DELTA },
    oracle_suspects_excluded: { type: 'array', items: { type: 'string' } },
    hypotheses_confirmed: { type: 'array', items: { type: 'string' } },
    hypotheses_refuted: { type: 'array', items: { type: 'string' } },
    ledger_appended: { type: 'array', items: { type: 'string' } }, northstar_updated: { type: 'boolean' },
    evidence: { type: 'array', items: EVIDENCE_ITEM }, diagnostics: { type: 'array', items: EVIDENCE_ITEM },
  },
}

const BOOTSTRAP_LANES = {
  missing_data: {
    stage: '1', slug: 'data', next: 'missing_mode_a', scope: 'data/cohorts/frozen digest receipt',
    contract: 'Verify >=15 GiB, ingest, create/repair smoke+tune+holdout manifests, and commit holdout.sha256 from canonical membership. Never benchmark holdout or emit membership/rows.',
  },
  missing_mode_a: {
    stage: '2', slug: 'mode-a', next: 'missing_conventions', scope: 'Mode A only',
    contract: 'Implement/test Mode A, run aggregate smoke only, commit bootstrap/mode-a.json. Do not implement/stub Mode B; never benchmark holdout or emit rows.',
  },
  missing_conventions: {
    stage: '3', slug: 'conventions', next: 'missing_mode_b', scope: 'convention floor',
    contract: 'Resolve conventions on aggregate smoke+tune Mode A, commit CONVENTIONS.md + iter-000 + evidenced memory. Never benchmark holdout or read Mode B.',
  },
  missing_mode_b: {
    stage: '4', slug: 'mode-b', next: 'ready', scope: 'Mode B after conventions',
    contract: 'Implement/test Mode B, run aggregate smoke+tune, commit bootstrap/mode-b.json. Never change holdout/conventions or benchmark holdout.',
  },
}

function validSuccessEvidence(evidence) {
  return Array.isArray(evidence) && evidence.length > 0 && evidence.every(item =>
    item && typeof item.command === 'string' && item.command.trim() &&
    item.exit_code === 0 && typeof item.output === 'string' && item.output.trim())
}

function reviewContractError(review, expectedSha) {
  if (!review) return 'missing review'
  if (!['APPROVE', 'BLOCK'].includes(review.verdict) || !Array.isArray(review.findings)) return 'review shape invalid'
  if (review.reviewed_sha !== expectedSha) return 'reviewed SHA mismatch'
  if (!validSuccessEvidence(review.evidence)) return 'review lacks successful evidence'
  const blockers = review.findings.filter(f => f.severity === 'blocker')
  if (review.verdict === 'APPROVE' && blockers.length) return 'APPROVE contains blocker'
  if (review.verdict === 'BLOCK' && !blockers.length) return 'BLOCK lacks blocker'
  return null
}

function bootstrapContractError(report, review, gate, expected) {
  if (!report || report.outcome !== 'DONE') return 'bootstrap build incomplete'
  if (report.state !== expected.state || report.branch !== expected.branch || report.base_sha !== expected.base_sha) return 'bootstrap build identity mismatch'
  if (!/^[0-9a-f]{40}$/i.test(report.sha || '') || !validSuccessEvidence(report.evidence)) return 'bootstrap build evidence/SHA invalid'
  if (!/^[0-9a-f]{64}$/i.test(report.holdout_digest_receipt || '') ||
      (expected.holdout_digest_receipt && report.holdout_digest_receipt !== expected.holdout_digest_receipt)) return 'bootstrap holdout receipt invalid'
  if (!/^pool-[0-9]+$/.test(report.lease_name || '') || report.lease_run_id !== expected.run_id || report.heartbeat_id !== expected.heartbeat_id) return 'bootstrap lease identity mismatch'
  if (!/[\\/]atx-wt[\\/]pool-[0-9]+$/i.test(report.worktree || '') ||
      !report.worktree.replace(/\//g, '\\').toLowerCase().endsWith(`\\${report.lease_name.toLowerCase()}`)) return 'bootstrap build not isolated'
  const reviewError = reviewContractError(review, report.sha)
  if (reviewError || review.verdict !== 'APPROVE') return reviewError || 'bootstrap not approved'
  if (!gate || !gate.passed || !validSuccessEvidence(gate.evidence)) return 'scoped verifier missing/failed'
  if (gate.reviewed_sha !== report.sha || gate.integration_sha !== report.sha || gate.canonical_after !== report.sha) return 'exact reviewed SHA not landed'
  if (gate.integration_branch !== expected.integration_branch || gate.canonical_ref !== expected.canonical_ref) return 'bootstrap integration identity mismatch'
  if (gate.holdout_digest_receipt !== report.holdout_digest_receipt) return 'bootstrap holdout receipt changed during verification'
  if (gate.canonical_expected_old !== expected.canonical_expected_old || gate.next_state !== expected.next_state) return 'canonical CAS or state advance mismatch'
  if (!/[\\/]atx-wt[\\/]pool-[0-9]+$/i.test(gate.integration_worktree || '')) return 'bootstrap verifier not isolated'
  if (!/^pool-[0-9]+$/.test(gate.integration_lease || '') || gate.lease_run_id !== expected.run_id || gate.integration_heartbeat_id !== expected.integration_heartbeat_id) return 'bootstrap integration lease identity mismatch'
  if (!gate.integration_worktree.replace(/\//g, '\\').toLowerCase().endsWith(`\\${gate.integration_lease.toLowerCase()}`)) return 'bootstrap integration worktree/lease mismatch'
  if (!Array.isArray(gate.leases_released) || !gate.leases_released.includes(report.lease_name) || !gate.leases_released.includes(gate.integration_lease)) return 'bootstrap lane/integration lease not released'
  if (!gate.evidence.some(item => item.command.includes('git update-ref') && item.exit_code === 0)) return 'bootstrap lacks atomic update-ref evidence'
  return null
}

function attributionPayloadError(payload) {
  if (typeof payload !== 'string' || !payload.trim()) return 'aggregate attribution payload missing'
  const forbidden = [
    /holdout/i, /sha-?256/i, /[0-9a-f]{40,64}/i, /[A-Za-z]:[\\/]/,
    /(?:^|\s)[A-Za-z0-9_.-]+[\\/][A-Za-z0-9_.\\/-]+\.(?:cpp|hpp|json|md|parquet)\b/i,
    /(?:^|[\\/])(?:date|underlier|bucket_et)=/i, /(?:bidPrc|askPrc|srPrc|srVol)\s*[:=]\s*[-+0-9.]+/i,
  ]
  return forbidden.some(pattern => pattern.test(payload)) ? 'payload exposes holdout/hash/path/row data' : null
}

function ratchetContractError(ratchet, expected) {
  if (!ratchet || !validSuccessEvidence(ratchet.evidence)) return 'Ratchet evidence missing/failed'
  if (!['ACCEPT', 'REJECT'].includes(ratchet.verdict)) return 'Ratchet verdict invalid'
  if (ratchet.tested_sha !== expected.tested_sha || ratchet.tested_branch !== expected.tested_branch) return 'Ratchet tested wrong integration'
  if (ratchet.base_sha !== expected.tested_sha || ratchet.lease_run_id !== expected.run_id || ratchet.heartbeat_id !== expected.heartbeat_id) return 'Ratchet base/lease identity mismatch'
  if (ratchet.ratchet_branch !== expected.ratchet_branch || ratchet.holdout_digest_verified !== expected.holdout_digest) return 'Ratchet branch/digest mismatch'
  if (ratchet.canonical_ref !== expected.canonical_ref || ratchet.canonical_expected_old !== expected.canonical_before) return 'Ratchet canonical CAS mismatch'
  if (!ratchet.lease_released || !/[\\/]atx-wt[\\/]pool-[0-9]+$/i.test(ratchet.worktree || '')) return 'Ratchet lease/worktree invalid'
  if (!/^pool-[0-9]+$/.test(ratchet.lease_name || '') || !ratchet.northstar_updated || !Array.isArray(ratchet.ledger_appended) || !ratchet.ledger_appended.length) return 'Ratchet memory/lease result incomplete'
  if (!ratchet.worktree.replace(/\//g, '\\').toLowerCase().endsWith(`\\${ratchet.lease_name.toLowerCase()}`)) return 'Ratchet worktree/lease mismatch'
  if (!ratchet.evidence.some(item => /atx-vol-oracle-bench/i.test(item.command) && /holdout/i.test(item.command))) return 'Ratchet lacks holdout benchmark evidence'
  if (typeof ratchet.holdout_summary !== 'string' || !ratchet.holdout_summary.trim() ||
      !Array.isArray(ratchet.headline_metric_deltas) || !ratchet.headline_metric_deltas.length ||
      !Array.isArray(ratchet.oracle_suspects_excluded)) return 'Ratchet aggregate scorecard missing'
  for (const metric of ratchet.headline_metric_deltas) {
    if (!metric || typeof metric.metric !== 'string' || !metric.metric.trim() ||
        !Number.isFinite(metric.baseline) || !Number.isFinite(metric.candidate) || !Number.isFinite(metric.delta) ||
        typeof metric.unit !== 'string' || !metric.unit.trim() || !Number.isInteger(metric.evidence_index) ||
        metric.evidence_index < 0 || metric.evidence_index >= ratchet.evidence.length) return 'Ratchet metric evidence reference invalid'
    const evidence = ratchet.evidence[metric.evidence_index]
    if (!/atx-vol-oracle-bench/i.test(evidence.command) || !/holdout/i.test(evidence.command) ||
        !evidence.output.includes(metric.metric) || !evidence.output.includes(String(metric.baseline)) ||
        !evidence.output.includes(String(metric.candidate)) || !evidence.output.includes(String(metric.delta))) {
      return 'Ratchet metric not supported by referenced output'
    }
  }
  if (ratchet.verdict === 'ACCEPT') {
    if (!/^[0-9a-f]{40}$/i.test(ratchet.landed_sha || '') || ratchet.canonical_after !== ratchet.landed_sha || ratchet.ratchet_sha !== ratchet.landed_sha) return 'ACCEPT did not atomically land Ratchet SHA'
    if (!ratchet.evidence.some(item => item.command.includes('git update-ref') && item.exit_code === 0)) return 'ACCEPT lacks atomic update-ref evidence'
  } else {
    if (ratchet.canonical_after !== expected.canonical_before || ratchet.landed_sha) return 'REJECT changed canonical base'
    if (ratchet.evidence.some(item => item.command.includes('git update-ref'))) return 'REJECT attempted canonical update'
  }
  return null
}

phase('Capability')
const capability = await agent(
  `Read-only capability inspection for run_id=${RUN_ID}. If ${CANONICAL_REF} exists, freeze it; otherwise freeze ${REQUESTED_BASE}^{commit} without creating a ref. Inspect committed metadata/aggregate receipts only. You may test that holdout.json exists but MUST NOT open it; read only the already committed holdout.sha256 digest receipt. Never access parquet/raw rows. Select first missing state in order: missing_data, missing_mode_a, missing_conventions, missing_mode_b, ready. Return only exit_code=0 evidence under evidence; failed probes under diagnostics.`,
  { schema: CAPABILITY, label: 'capability' },
)
if (!capability || capability.canonical_ref !== CANONICAL_REF || !/^[0-9a-f]{40}$/i.test(capability.base_sha || '') || !validSuccessEvidence(capability.evidence)) {
  throw new Error('capability did not freeze canonical base with successful evidence')
}
if (capability.state !== 'missing_data' && !/^[0-9a-f]{64}$/i.test(capability.holdout_digest_receipt || '')) {
  throw new Error('committed holdout digest receipt is missing/invalid')
}
const BASE_SHA = capability.base_sha
const CANONICAL_EXPECTED_OLD = capability.canonical_exists ? BASE_SHA : ZERO_SHA

if (capability.state !== 'ready') {
  const lane = BOOTSTRAP_LANES[capability.state]
  const branch = `lane/oracle-bootstrap-${lane.slug}-${RUN_SLUG}`
  const heartbeat = `${RUN_SLUG}-bootstrap-${lane.slug}`
  const integrationBranch = `integration/oracle-bootstrap-${lane.slug}-${RUN_SLUG}`
  phase('Bootstrap Build')
  let report = await agent(
    `ONE fixed bootstrap implementation lane; no planner and no holdout. State=${capability.state}, stage=${lane.stage}, scope=${lane.scope}, frozen base=${BASE_SHA}. Read CHARTER stage ${lane.stage}. ${lane.contract} Lease with powershell scripts\\lease-worktree.ps1 -Branch ${branch} -Base ${BASE_SHA} -Agent oracle-bootstrap-${lane.slug} -RunId ${RUN_ID} -HeartbeatId ${heartbeat} -MaxPool 20. Pulse before/after long commands. Implement, target-test, commit explicit paths, keep lease held. Successful evidence contains only exit_code=0; failures go in diagnostics.`,
    { agentType: 'vol-builder', schema: BOOTSTRAP_REPORT, label: `bootstrap-build:${capability.state}` },
  )
  let review = null
  let contractFailure = null
  if (!report || report.outcome !== 'DONE' || report.state !== capability.state || report.branch !== branch ||
      report.base_sha !== BASE_SHA || report.lease_run_id !== RUN_ID || report.heartbeat_id !== heartbeat ||
      !/^pool-[0-9]+$/.test(report.lease_name || '') ||
      !/[\\/]atx-wt[\\/]pool-[0-9]+$/i.test(report.worktree || '') ||
      !report.worktree.replace(/\//g, '\\').toLowerCase().endsWith(`\\${report.lease_name.toLowerCase()}`) ||
      !/^[0-9a-f]{64}$/i.test(report.holdout_digest_receipt || '') ||
      (capability.holdout_digest_receipt && report.holdout_digest_receipt !== capability.holdout_digest_receipt) ||
      !/^[0-9a-f]{40}$/i.test(report.sha || '') || !validSuccessEvidence(report.evidence)) {
    contractFailure = 'bootstrap build report invalid/incomplete'
  } else {
    phase('Bootstrap Review')
    review = await agent(
      `Independent fresh review of fixed bootstrap stage ${lane.stage}. Exact diff ${BASE_SHA}...${report.sha}; verify charter, holdout prohibition, tests, and evidence. Review exactly ${report.sha}. Successful review evidence must have exit_code=0. APPROVE with any blocker is invalid.`,
      { agentType: 'vol-reviewer', schema: REVIEW, label: `bootstrap-review:${capability.state}` },
    )
    contractFailure = reviewContractError(review, report.sha)
  }
  if (!contractFailure && review.verdict === 'BLOCK') {
    phase('Bootstrap Fix')
    const blockers = review.findings.filter(f => f.severity === 'blocker')
    report = await agent(
      `Fix exactly these blockers in ${report.worktree} at ${report.branch}: ${JSON.stringify(blockers)}. Pulse the lease, rerun scoped checks, commit, keep lease held, and report the new SHA. Evidence success requires exit_code=0; failed attempts go in diagnostics.`,
      { agentType: 'vol-builder', schema: BOOTSTRAP_REPORT, label: `bootstrap-fix:${capability.state}` },
    )
    if (!report || report.outcome !== 'DONE' || report.state !== capability.state || report.branch !== branch || report.base_sha !== BASE_SHA ||
        report.lease_run_id !== RUN_ID || report.heartbeat_id !== heartbeat || !/^pool-[0-9]+$/.test(report.lease_name || '') ||
        !/[\\/]atx-wt[\\/]pool-[0-9]+$/i.test(report.worktree || '') ||
        !report.worktree.replace(/\//g, '\\').toLowerCase().endsWith(`\\${report.lease_name.toLowerCase()}`) ||
        !/^[0-9a-f]{64}$/i.test(report.holdout_digest_receipt || '') ||
        (capability.holdout_digest_receipt && report.holdout_digest_receipt !== capability.holdout_digest_receipt) ||
        !/^[0-9a-f]{40}$/i.test(report.sha || '') || !validSuccessEvidence(report.evidence)) {
      contractFailure = 'bootstrap Fix report invalid'
    } else {
      phase('Bootstrap Re-review')
      review = await agent(
        `FRESH post-Fix review; do not reuse the prior verdict. Inspect ${BASE_SHA}...${report.sha}, run a scoped check, and review exactly ${report.sha}. APPROVE with a blocker or nonzero success evidence is invalid.`,
        { agentType: 'vol-reviewer', schema: REVIEW, label: `bootstrap-rereview:${capability.state}` },
      )
      contractFailure = reviewContractError(review, report.sha)
    }
  }

  if (contractFailure || !review || review.verdict !== 'APPROVE') {
    let cleanup = null
    if (report && /^pool-[0-9]+$/.test(report.lease_name || '') && report.lease_run_id === RUN_ID) {
      cleanup = await agent(
        `Bootstrap abort before integration: ${contractFailure || 'final review BLOCK'}. Release only ${report.lease_name} with -RunId ${RUN_ID}; do not integrate, benchmark, or edit memory.`,
        { agentType: 'vol-verifier', schema: CLEANUP, label: 'bootstrap-abort-cleanup' },
      )
    }
    return { iteration: `bootstrap-${lane.stage}`, capability_state: capability.state, verdict: 'FAILED', holdout: null, confirmed: [], refuted: [], sprint: null, ledger: [], ratchet_evidence: [], failure: contractFailure || 'bootstrap not approved', cleanup, run_id: RUN_ID, base_sha: BASE_SHA }
  }

  phase('Bootstrap Verify')
  const gate = await agent(
    `Scoped bootstrap verifier for stage ${lane.stage}. First release lane lease ${report.lease_name} with -RunId ${RUN_ID}. Then acquire NEW isolated worktree with powershell scripts\\lease-worktree.ps1 -Branch ${integrationBranch} -Base ${BASE_SHA} -Agent oracle-bootstrap-verifier -RunId ${RUN_ID} -HeartbeatId ${RUN_SLUG}-bootstrap-integration -MaxPool 20. In that worktree only, fast-forward exactly to reviewed SHA ${review.reviewed_sha}; integration HEAD must equal it. Run only CHARTER stage-${lane.stage} scoped verification; never holdout. Re-run read-only capability inspection without opening holdout membership/raw rows and require next_state=${lane.next}; report its committed holdout digest receipt unchanged. Atomically update ${CANONICAL_REF} from ${CANONICAL_EXPECTED_OLD} to exact reviewed SHA using git update-ref; failure means passed=false. Release integration lease on all paths. Report lease_run_id=${RUN_ID}, both released lease names, and successful evidence with exit_code=0 only.`,
    { agentType: 'vol-verifier', schema: BOOTSTRAP_GATE, label: `bootstrap-gate:${capability.state}` },
  )
  const bootstrapError = bootstrapContractError(report, review, gate, {
    state: capability.state, branch, base_sha: BASE_SHA, integration_branch: integrationBranch,
    canonical_ref: CANONICAL_REF, canonical_expected_old: CANONICAL_EXPECTED_OLD, next_state: lane.next,
    holdout_digest_receipt: capability.holdout_digest_receipt,
    run_id: RUN_ID, heartbeat_id: heartbeat, integration_heartbeat_id: `${RUN_SLUG}-bootstrap-integration`,
  })
  return {
    iteration: `bootstrap-${lane.stage}`, capability_state: capability.state,
    verdict: bootstrapError ? 'FAILED' : 'BOOTSTRAP', holdout: null, confirmed: [], refuted: [], sprint: null,
    ledger: [], ratchet_evidence: [], bootstrap: { report, review, gate }, failure: bootstrapError,
    run_id: RUN_ID, base_sha: BASE_SHA, canonical_after: bootstrapError ? null : gate.canonical_after,
  }
}

if (!capability.canonical_exists) throw new Error('ready state requires an existing canonical ref')
phase('Measure')
const measureBranch = `lane/oracle-measure-${capability.next_iter}-${RUN_SLUG}`
const measureHeartbeat = `${RUN_SLUG}-measure`
const measure = await agent(
  `Ready Measure ${capability.next_iter}, canonical SHA=${BASE_SHA}. Lease with powershell scripts\\lease-worktree.ps1 -Branch ${measureBranch} -Base ${BASE_SHA} -Agent oracle-measure -RunId ${RUN_ID} -HeartbeatId ${measureHeartbeat} -MaxPool 20. Pulse long commands. Run smoke+tune only, Mode A+B; dev correctness and pinned rel-avx2 speed. Commit aggregate scorecard then release. Return a self-contained attribution_payload with aggregate scorecard, prior refuted IDs, oracle-suspect cells, convention summary, and source-symbol hints. It must contain no filesystem paths, hashes, membership, or rows. Successful evidence exit_code=0 only.`,
  { schema: MEASURE, label: 'measure' },
)
if (!measure || measure.status !== 'ok' || measure.iter !== capability.next_iter || measure.branch !== measureBranch ||
    measure.base_sha !== BASE_SHA || measure.lease_run_id !== RUN_ID || measure.heartbeat_id !== measureHeartbeat ||
    !/^pool-[0-9]+$/.test(measure.lease_name || '') || !/[\\/]atx-wt[\\/]pool-[0-9]+$/i.test(measure.worktree || '') ||
    !measure.worktree.replace(/\//g, '\\').toLowerCase().endsWith(`\\${measure.lease_name.toLowerCase()}`) ||
    !/^[0-9a-f]{40}$/i.test(measure.sha || '') || !measure.lease_released ||
    !validSuccessEvidence(measure.evidence) || attributionPayloadError(measure.attribution_payload)) {
  return { iteration: capability.next_iter, capability_state: 'ready', verdict: 'FAILED', holdout: null, confirmed: [], refuted: [], sprint: null, ledger: [], ratchet_evidence: [], failure: 'Measure failed contract; no holdout', run_id: RUN_ID, base_sha: BASE_SHA }
}

phase('Attribute')
const attribution = await agent(
  `Tool-less aggregate attribution only. Payload:\n${measure.attribution_payload}\nRank 1-3 falsifiable hypotheses by expected aggregate error reduction/effort. Do not request files, hashes, membership, rows, or tools.${FOCUS ? `\nOperator focus: ${FOCUS}` : ''}`,
  { agentType: 'vol-analyst', schema: ATTR, label: 'attribute' },
)
if (!attribution || !Array.isArray(attribution.hypotheses) || attribution.hypotheses.length < 1) {
  return { iteration: measure.iter, capability_state: 'ready', verdict: 'FAILED', holdout: null, confirmed: [], refuted: [], sprint: null, ledger: [], ratchet_evidence: [], failure: 'Attribution failed; no holdout', run_id: RUN_ID, base_sha: BASE_SHA }
}

phase('Improve')
const task = `Oracle RSI ${measure.iter}; aggregate smoke+tune hypotheses:\n${JSON.stringify(attribution.hypotheses, null, 2)}\nHard cutover only; CHANGELOG BREAKING; no compatibility flags/shims; no licensed rows.`
let sprint
try { sprint = await workflow('vol-sprint', { task, base: measure.sha }) }
catch (error) { sprint = { passed: false, error: String(error) } }
const sprintValid = sprint && sprint.passed && /^integration\//.test(sprint.integration_branch || '') &&
  /^[0-9a-f]{40}$/i.test(sprint.integration_sha || '') && validSuccessEvidence(sprint.gate_evidence)
if (!sprintValid) {
  return { iteration: measure.iter, capability_state: 'ready', verdict: 'FAILED', holdout: null, confirmed: [], refuted: [], sprint: sprint || null, ledger: [], ratchet_evidence: [], failure: 'Sprint incomplete/invalid; no holdout and no REJECT increment', run_id: RUN_ID, base_sha: BASE_SHA }
}

phase('Ratchet')
const ratchetBranch = `lane/oracle-ratchet-${RUN_SLUG}`
const ratchetHeartbeat = `${RUN_SLUG}-ratchet`
const ratchet = await agent(
  `Ratchet exact reviewed integration ${sprint.integration_branch}@${sprint.integration_sha}. Acquire NEW isolated worktree with powershell scripts\\lease-worktree.ps1 -Branch ${ratchetBranch} -Base ${sprint.integration_sha} -Agent oracle-ratchet -RunId ${RUN_ID} -HeartbeatId ${ratchetHeartbeat} -MaxPool 20; existing mismatched branch must fail. In that worktree, first recompute canonical holdout membership digest and require committed receipt ${capability.holdout_digest_receipt}; only Ratchet may open membership/rows. Run aggregate holdout Mode A+B. ACCEPT iff target cells improve, aggregate regression <=2%, and rel-avx2 speed >= pin; else REJECT. Commit scorecard and evidenced NORTHSTAR/LEDGER on ratchet branch. ACCEPT atomically CAS-updates ${CANONICAL_REF} from ${BASE_SHA} to final ratchet commit with git update-ref. REJECT must leave ${CANONICAL_REF} exactly ${BASE_SHA}. Release lease on all paths. Return aggregate holdout_summary, oracle_suspects_excluded, and headline_metric_deltas; every metric names an evidence_index whose holdout bench output visibly contains the metric, baseline, candidate, and delta. Successful evidence contains only exit_code=0; metric gate failures belong in diagnostics but REJECT evidence commands still exit 0.`,
  { agentType: 'vol-verifier', schema: RATCHET, label: 'ratchet' },
)
const ratchetError = ratchetContractError(ratchet, {
  tested_sha: sprint.integration_sha, tested_branch: sprint.integration_branch,
  ratchet_branch: ratchetBranch, holdout_digest: capability.holdout_digest_receipt,
  canonical_ref: CANONICAL_REF, canonical_before: BASE_SHA,
  run_id: RUN_ID, heartbeat_id: ratchetHeartbeat,
})

return {
  iteration: measure.iter, capability_state: 'ready', verdict: ratchetError ? 'FAILED' : ratchet.verdict,
  holdout: ratchetError ? null : ratchet.holdout_summary,
  confirmed: ratchetError ? [] : ratchet.hypotheses_confirmed,
  refuted: ratchetError ? [] : ratchet.hypotheses_refuted,
  sprint: { passed: true, integration_branch: sprint.integration_branch, integration_sha: sprint.integration_sha },
  ledger: ratchetError ? [] : ratchet.ledger_appended,
  ratchet_evidence: ratchetError ? [] : ratchet.evidence,
  ratchet: ratchetError ? null : ratchet,
  failure: ratchetError,
  run_id: RUN_ID, base_sha: BASE_SHA,
  canonical_after: ratchetError ? BASE_SHA : ratchet.canonical_after,
}
