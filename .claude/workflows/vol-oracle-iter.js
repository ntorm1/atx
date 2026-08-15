export const meta = {
  name: 'vol-oracle-iter',
  description: 'One transactional SpiderRock capability bootstrap or exact-SHA oracle RSI iteration',
  whenToUse: 'Run exactly one oracle step; canonical mutation occurs only after typed receipts validate.',
  phases: [
    { title: 'Capability', detail: 'freeze canonical SHA and committed digest receipt without membership access' },
    { title: 'Bootstrap', detail: 'one fixed lane, independent review, typed prepare, CAS finalizer, audit' },
    { title: 'Measure', detail: 'ready only: aggregate smoke+tune Mode A+B' },
    { title: 'Attribute', detail: 'ready only: tool-less strict typed aggregate payload' },
    { title: 'Improve', detail: 'ready only: gated vol-sprint exact integration SHA' },
    { title: 'Ratchet', detail: 'ready only: typed exact-SHA holdout prepare, workflow verdict, CAS, audit' },
  ],
}

if (args && args.base && args.base !== 'main') throw new Error('vol-oracle-iter capability probe is fixed to main before canonical creation')
const REQUESTED_BASE = 'main'
const FOCUS = (args && args.focus) || ''
const RUN_ID = `vol-oracle-${Date.now()}-${Math.random().toString(16).slice(2)}`
const RUN_SLUG = RUN_ID.replace(/[^A-Za-z0-9._-]/g, '-')
const CANONICAL_REF = 'refs/heads/oracle/canonical'
const ZERO_SHA = '0000000000000000000000000000000000000000'
const RATCHET_GATE_IDS = ['holdout_mode_a', 'holdout_mode_b', 'rel_avx2_speed']

const EVIDENCE_ITEM = {
  type: 'object', additionalProperties: false, required: ['command', 'exit_code', 'output'],
  properties: { command: { type: 'string' }, exit_code: { type: 'integer' }, output: { type: 'string' } },
}
const LEASE_RECEIPT = {
  type: 'object', additionalProperties: false,
  required: ['action', 'lease_name', 'run_id', 'branch', 'base_sha', 'worktree', 'heartbeat_id', 'keeper_pid', 'keeper_process_started_utc', 'keeper_ready_utc', 'exit_code', 'output'],
  properties: {
    action: { type: 'string', enum: ['acquire', 'release'] }, lease_name: { type: 'string' }, run_id: { type: 'string' },
    branch: { type: 'string' }, base_sha: { type: 'string' }, worktree: { type: 'string' }, heartbeat_id: { type: 'string' },
    keeper_pid: { type: 'integer' }, keeper_process_started_utc: { type: 'string' }, keeper_ready_utc: { type: 'string' }, exit_code: { type: 'integer' }, output: { type: 'string' },
  },
}
const HEAD_RECEIPT = {
  type: 'object', additionalProperties: false, required: ['ref', 'sha', 'command', 'exit_code', 'output'],
  properties: { ref: { type: 'string' }, sha: { type: 'string' }, command: { type: 'string' }, exit_code: { type: 'integer' }, output: { type: 'string' } },
}
const CAS_RECEIPT = {
  type: 'object', additionalProperties: false, required: ['ref', 'new_sha', 'expected_old_sha', 'command', 'exit_code', 'output'],
  properties: {
    ref: { type: 'string' }, new_sha: { type: 'string' }, expected_old_sha: { type: 'string' }, command: { type: 'string' },
    exit_code: { type: 'integer' }, output: { type: 'string' },
  },
}
const GATE_RECEIPT = {
  type: 'object', additionalProperties: false, required: ['gate_id', 'command', 'exit_code', 'output'],
  properties: { gate_id: { type: 'string' }, command: { type: 'string' }, exit_code: { type: 'integer' }, output: { type: 'string' } },
}
const INTEGRATION_RECEIPT = {
  type: 'object', additionalProperties: false, required: ['reviewed_sha', 'head_after', 'command', 'exit_code', 'output'],
  properties: {
    reviewed_sha: { type: 'string' }, head_after: { type: 'string' }, command: { type: 'string' },
    exit_code: { type: 'integer' }, output: { type: 'string' },
  },
}
const FINDING = {
  type: 'object', additionalProperties: false, required: ['location', 'severity', 'problem', 'fix'],
  properties: {
    location: { type: 'string' }, severity: { type: 'string', enum: ['blocker', 'major', 'minor'] },
    problem: { type: 'string' }, fix: { type: 'string' },
  },
}
const CAPABILITY = {
  type: 'object', additionalProperties: false,
  required: ['state', 'canonical_ref', 'canonical_exists', 'base_ref', 'base_sha', 'holdout_digest_receipt', 'next_iter', 'evidence'],
  properties: {
    state: { type: 'string', enum: ['missing_data', 'missing_mode_a', 'missing_conventions', 'missing_mode_b', 'ready'] },
    canonical_ref: { type: 'string' }, canonical_exists: { type: 'boolean' }, base_ref: { type: 'string' }, base_sha: { type: 'string' },
    holdout_digest_receipt: { type: 'string' }, next_iter: { type: 'string' }, evidence: { type: 'array', items: EVIDENCE_ITEM },
    diagnostics: { type: 'array', items: EVIDENCE_ITEM },
  },
}
const BOOTSTRAP_REPORT = {
  type: 'object', additionalProperties: false,
  required: ['state', 'outcome', 'branch', 'sha', 'base_sha', 'worktree', 'lease_name', 'lease_run_id', 'heartbeat_id', 'keeper_pid', 'keeper_process_started_utc', 'acquisition_receipt', 'holdout_digest_receipt', 'evidence', 'deviations'],
  properties: {
    state: { type: 'string', enum: ['missing_data', 'missing_mode_a', 'missing_conventions', 'missing_mode_b'] },
    outcome: { type: 'string', enum: ['DONE', 'BLOCKED'] }, branch: { type: 'string' }, sha: { type: 'string' }, base_sha: { type: 'string' },
    worktree: { type: 'string' }, lease_name: { type: 'string' }, lease_run_id: { type: 'string' }, heartbeat_id: { type: 'string' },
    keeper_pid: { type: 'integer' }, keeper_process_started_utc: { type: 'string' }, acquisition_receipt: LEASE_RECEIPT,
    holdout_digest_receipt: { type: 'string' }, evidence: { type: 'array', items: EVIDENCE_ITEM },
    diagnostics: { type: 'array', items: EVIDENCE_ITEM }, deviations: { type: 'string' },
  },
}
const REVIEW = {
  type: 'object', additionalProperties: false, required: ['verdict', 'reviewed_sha', 'evidence', 'findings'],
  properties: {
    verdict: { type: 'string', enum: ['APPROVE', 'BLOCK'] }, reviewed_sha: { type: 'string' },
    evidence: { type: 'array', items: EVIDENCE_ITEM }, diagnostics: { type: 'array', items: EVIDENCE_ITEM },
    findings: { type: 'array', items: FINDING },
  },
}
const CLEANUP = {
  type: 'object', additionalProperties: false, required: ['passed', 'released', 'release_receipts', 'evidence'],
  properties: {
    passed: { type: 'boolean' }, released: { type: 'array', items: { type: 'string' } }, release_receipts: { type: 'array', items: LEASE_RECEIPT },
    evidence: { type: 'array', items: EVIDENCE_ITEM }, diagnostics: { type: 'array', items: EVIDENCE_ITEM },
  },
}
const BOOTSTRAP_PREPARE = {
  type: 'object', additionalProperties: false,
  required: ['passed', 'reviewed_sha', 'integration_branch', 'integration_sha', 'integration_worktree', 'integration_lease', 'lease_run_id', 'integration_heartbeat_id', 'keeper_pid', 'keeper_process_started_utc', 'holdout_digest_receipt', 'next_state', 'lane_release_receipt', 'acquisition_receipt', 'integration_receipt', 'head_receipt', 'gate_receipts', 'integration_release_receipt', 'evidence'],
  properties: {
    passed: { type: 'boolean' }, reviewed_sha: { type: 'string' }, integration_branch: { type: 'string' }, integration_sha: { type: 'string' },
    integration_worktree: { type: 'string' }, integration_lease: { type: 'string' }, lease_run_id: { type: 'string' },
    integration_heartbeat_id: { type: 'string' }, keeper_pid: { type: 'integer' }, keeper_process_started_utc: { type: 'string' },
    holdout_digest_receipt: { type: 'string' }, next_state: { type: 'string', enum: ['missing_mode_a', 'missing_conventions', 'missing_mode_b', 'ready'] },
    lane_release_receipt: LEASE_RECEIPT, acquisition_receipt: LEASE_RECEIPT, integration_receipt: INTEGRATION_RECEIPT,
    head_receipt: HEAD_RECEIPT, gate_receipts: { type: 'array', items: GATE_RECEIPT }, integration_release_receipt: LEASE_RECEIPT,
    evidence: { type: 'array', items: EVIDENCE_ITEM }, diagnostics: { type: 'array', items: EVIDENCE_ITEM },
  },
}
const AGGREGATE_METRIC = {
  type: 'object', additionalProperties: false,
  required: ['cell_id', 'mode', 'metric', 'count', 'mae', 'rmse', 'p95', 'within_tolerance_rate', 'unit'],
  properties: {
    cell_id: { type: 'string' }, mode: { type: 'string', enum: ['A', 'B'] }, metric: { type: 'string' }, count: { type: 'integer' },
    mae: { type: 'number' }, rmse: { type: 'number' }, p95: { type: 'number' }, within_tolerance_rate: { type: 'number' }, unit: { type: 'string' },
  },
}
const ATTR_PAYLOAD = {
  type: 'object', additionalProperties: false,
  required: ['schema_version', 'iteration', 'metrics', 'prior_refuted_ids', 'oracle_suspect_cells', 'convention_summary', 'source_symbols'],
  properties: {
    schema_version: { type: 'integer', enum: [1] }, iteration: { type: 'string' }, metrics: { type: 'array', items: AGGREGATE_METRIC },
    prior_refuted_ids: { type: 'array', items: { type: 'string' } }, oracle_suspect_cells: { type: 'array', items: { type: 'string' } },
    convention_summary: { type: 'string' }, source_symbols: { type: 'array', items: { type: 'string' } },
  },
}
const MEASURE = {
  type: 'object', additionalProperties: false,
  required: ['status', 'iter', 'scorecard_path', 'branch', 'sha', 'base_sha', 'worktree', 'lease_name', 'lease_run_id', 'heartbeat_id', 'keeper_pid', 'keeper_process_started_utc', 'acquisition_receipt', 'release_receipt', 'lease_released', 'attribution_payload', 'evidence'],
  properties: {
    status: { type: 'string', enum: ['ok', 'failed'] }, iter: { type: 'string' }, scorecard_path: { type: 'string' }, branch: { type: 'string' },
    sha: { type: 'string' }, base_sha: { type: 'string' }, worktree: { type: 'string' }, lease_name: { type: 'string' }, lease_run_id: { type: 'string' },
    heartbeat_id: { type: 'string' }, keeper_pid: { type: 'integer' }, keeper_process_started_utc: { type: 'string' },
    acquisition_receipt: LEASE_RECEIPT, release_receipt: LEASE_RECEIPT, lease_released: { type: 'boolean' }, attribution_payload: ATTR_PAYLOAD,
    evidence: { type: 'array', items: EVIDENCE_ITEM }, diagnostics: { type: 'array', items: EVIDENCE_ITEM },
  },
}
const ATTR = {
  type: 'object', additionalProperties: false, required: ['hypotheses', 'new_suspect_candidates'],
  properties: {
    hypotheses: { type: 'array', items: {
      type: 'object', additionalProperties: false, required: ['id', 'target_cells', 'modes', 'mechanism', 'prediction', 'blast_radius', 'effort'],
      properties: {
        id: { type: 'string' }, target_cells: { type: 'array', items: { type: 'string' } }, modes: { type: 'array', items: { type: 'string', enum: ['A', 'B'] } },
        mechanism: { type: 'string' }, prediction: { type: 'string' }, blast_radius: { type: 'string' }, effort: { type: 'string', enum: ['S', 'M', 'L'] },
      },
    } },
    new_suspect_candidates: { type: 'array', items: { type: 'string' } },
  },
}
const RATCHET_METRIC = {
  type: 'object', additionalProperties: false,
  required: ['metric_id', 'mode', 'gate', 'direction', 'baseline', 'candidate', 'delta', 'pin', 'unit', 'evidence_index'],
  properties: {
    metric_id: { type: 'string' }, mode: { type: 'string', enum: ['A', 'B', 'ALL'] }, gate: { type: 'string', enum: ['target', 'aggregate', 'speed'] },
    direction: { type: 'string', enum: ['lower', 'higher'] }, baseline: { type: 'number' }, candidate: { type: 'number' }, delta: { type: 'number' },
    pin: { type: 'number' }, unit: { type: 'string' }, evidence_index: { type: 'integer' },
  },
}
const DIGEST_RECEIPT = {
  type: 'object', additionalProperties: false, required: ['expected_digest', 'actual_digest', 'command', 'exit_code', 'output'],
  properties: {
    expected_digest: { type: 'string' }, actual_digest: { type: 'string' }, command: { type: 'string' },
    exit_code: { type: 'integer' }, output: { type: 'string' },
  },
}
const MARKET_RECEIPT = {
  type: 'object', additionalProperties: false, required: ['cell_id', 'market_sides_with', 'command', 'exit_code', 'output'],
  properties: {
    cell_id: { type: 'string' }, market_sides_with: { type: 'string', enum: ['atx-vol'] }, command: { type: 'string' },
    exit_code: { type: 'integer' }, output: { type: 'string' },
  },
}
const RATCHET_PREPARE = {
  type: 'object', additionalProperties: false,
  required: ['tested_branch', 'tested_sha', 'base_sha', 'ratchet_branch', 'ratchet_sha', 'worktree', 'lease_name', 'lease_run_id', 'heartbeat_id', 'keeper_pid', 'keeper_process_started_utc', 'acquisition_receipt', 'release_receipt', 'digest_receipt', 'applicable_modes', 'metrics', 'metric_evidence', 'gate_receipts', 'oracle_suspects_excluded', 'market_evidence', 'memory_verdict', 'holdout_summary', 'hypotheses_confirmed', 'hypotheses_refuted', 'ledger_appended', 'northstar_updated', 'evidence'],
  properties: {
    tested_branch: { type: 'string' }, tested_sha: { type: 'string' }, base_sha: { type: 'string' }, ratchet_branch: { type: 'string' },
    ratchet_sha: { type: 'string' }, worktree: { type: 'string' }, lease_name: { type: 'string' }, lease_run_id: { type: 'string' },
    heartbeat_id: { type: 'string' }, keeper_pid: { type: 'integer' }, keeper_process_started_utc: { type: 'string' },
    acquisition_receipt: LEASE_RECEIPT, release_receipt: LEASE_RECEIPT, digest_receipt: DIGEST_RECEIPT,
    applicable_modes: { type: 'array', items: { type: 'string', enum: ['A', 'B'] } }, metrics: { type: 'array', items: RATCHET_METRIC },
    metric_evidence: { type: 'array', items: EVIDENCE_ITEM }, gate_receipts: { type: 'array', items: GATE_RECEIPT },
    oracle_suspects_excluded: { type: 'array', items: { type: 'string' } }, market_evidence: { type: 'array', items: MARKET_RECEIPT },
    memory_verdict: { type: 'string', enum: ['ACCEPT', 'REJECT'] }, holdout_summary: { type: 'string' },
    hypotheses_confirmed: { type: 'array', items: { type: 'string' } }, hypotheses_refuted: { type: 'array', items: { type: 'string' } },
    ledger_appended: { type: 'array', items: { type: 'string' } }, northstar_updated: { type: 'boolean' },
    evidence: { type: 'array', items: EVIDENCE_ITEM }, diagnostics: { type: 'array', items: EVIDENCE_ITEM },
  },
}

const BOOTSTRAP_LANES = {
  missing_data: { stage: '1', slug: 'data', next: 'missing_mode_a', gate_ids: ['disk', 'ingest_manifest', 'cohort_manifests', 'holdout_digest'], contract: 'Verify >=15 GiB, ingest, create/repair smoke+tune+holdout manifests, commit holdout.sha256. Never benchmark holdout or emit membership/rows.' },
  missing_mode_a: { stage: '2', slug: 'mode-a', next: 'missing_conventions', gate_ids: ['mode_a_targeted_tests', 'mode_a_smoke'], contract: 'Implement/test Mode A, run aggregate smoke only, commit bootstrap/mode-a.json. Do not implement/stub Mode B; never benchmark holdout.' },
  missing_conventions: { stage: '3', slug: 'conventions', next: 'missing_mode_b', gate_ids: ['convention_tests', 'mode_a_smoke_tune', 'residual_floor'], contract: 'Resolve conventions on aggregate smoke+tune Mode A, commit CONVENTIONS.md + iter-000 + evidenced memory. Never benchmark holdout or read Mode B.' },
  missing_mode_b: { stage: '4', slug: 'mode-b', next: 'ready', gate_ids: ['mode_b_targeted_tests', 'mode_b_smoke_tune'], contract: 'Implement/test Mode B, run aggregate smoke+tune, commit bootstrap/mode-b.json. Never change holdout/conventions or benchmark holdout.' },
}

function validSuccessEvidence(evidence) {
  return Array.isArray(evidence) && evidence.length > 0 && evidence.every(item => item && typeof item.command === 'string' && item.command.trim() && item.exit_code === 0 && typeof item.output === 'string' && item.output.trim())
}

function validLeaseReceipt(receipt, expected, action) {
  if (!receipt || receipt.action !== action || receipt.exit_code !== 0 || !String(receipt.output || '').trim()) return false
  if (receipt.lease_name !== expected.lease_name || receipt.run_id !== expected.run_id || receipt.branch !== expected.branch || receipt.base_sha !== expected.base_sha ||
      receipt.worktree !== expected.worktree || receipt.heartbeat_id !== expected.heartbeat_id || receipt.keeper_pid !== expected.keeper_pid ||
      receipt.keeper_process_started_utc !== expected.keeper_process_started_utc) return false
  if (!Number.isInteger(receipt.keeper_pid) || receipt.keeper_pid <= 0 || !/^\d{4}-/.test(receipt.keeper_process_started_utc || '') || !/^\d{4}-/.test(receipt.keeper_ready_utc || '')) return false
  return receipt.output.includes(receipt.lease_name) && receipt.output.includes(receipt.run_id) &&
    (action === 'release' || (receipt.output.includes(String(receipt.keeper_pid)) && receipt.output.includes(receipt.heartbeat_id) && receipt.output.includes(receipt.keeper_ready_utc)))
}

function validHeadReceipt(receipt, ref, sha) {
  return !!receipt && receipt.ref === ref && receipt.sha === sha && receipt.exit_code === 0 && receipt.command.trim() === `git rev-parse ${ref}` && receipt.output.trim() === sha
}

function validGateReceipt(receipt, gateId) {
  if (!receipt || receipt.gate_id !== gateId || receipt.exit_code !== 0 || !String(receipt.output || '').trim()) return false
  const command = String(receipt.command || '')
  return command.includes(gateId) && /atx-build\.ps1|atx-vol-oracle-bench|python|node|invoke-pester/i.test(command)
}

function validIntegrationCommand(receipt, reviewedSha) {
  return !!receipt && receipt.reviewed_sha === reviewedSha && receipt.exit_code === 0 &&
    new RegExp(`^git\\s+(?:merge(?:\\s+--(?:ff-only|no-ff|no-edit))*|cherry-pick(?:\\s+--(?:ff|no-commit))*)\\s+${reviewedSha}$`, 'i').test(String(receipt.command || '').trim())
}

function casReceiptError(receipt, expected) {
  if (!receipt) return 'CAS receipt missing'
  if (receipt.ref !== expected.ref || receipt.new_sha !== expected.new_sha || receipt.expected_old_sha !== expected.expected_old_sha) return 'CAS identity mismatch'
  if (receipt.command.trim() !== `git update-ref ${expected.ref} ${expected.new_sha} ${expected.expected_old_sha}` || receipt.exit_code !== 0 || !String(receipt.output || '').trim()) return 'CAS command receipt invalid'
  return null
}

function auditReceiptError(receipt, ref) {
  if (!receipt) return 'canonical audit missing'
  if (receipt.ref !== ref || receipt.command.trim() !== `git rev-parse ${ref}` ||
      (!/^[0-9a-f]{40}$/i.test(receipt.sha || '') && receipt.sha !== 'MISSING') || receipt.output.trim() !== receipt.sha) return 'canonical audit invalid'
  if ((receipt.sha === 'MISSING' && receipt.exit_code === 0) || (receipt.sha !== 'MISSING' && receipt.exit_code !== 0)) return 'canonical audit invalid'
  return null
}

function reviewContractError(review, expectedSha) {
  if (!review) return 'missing review'
  if (!['APPROVE', 'BLOCK'].includes(review.verdict) || !Array.isArray(review.findings)) return 'review shape invalid'
  if (review.reviewed_sha !== expectedSha) return 'reviewed SHA mismatch'
  if (!validSuccessEvidence(review.evidence)) return 'review lacks successful evidence'
  const blockers = review.findings.filter(finding => finding.severity === 'blocker')
  if (review.verdict === 'APPROVE' && blockers.length) return 'APPROVE contains blocker'
  if (review.verdict === 'BLOCK' && !blockers.length) return 'BLOCK lacks blocker'
  return null
}

function bootstrapReportError(report, expected) {
  if (!report || report.outcome !== 'DONE') return 'bootstrap build incomplete'
  if (report.state !== expected.state || report.branch !== expected.branch || report.base_sha !== expected.base_sha || report.lease_run_id !== expected.run_id || report.heartbeat_id !== expected.heartbeat_id) return 'bootstrap build identity mismatch'
  if (!/^[0-9a-f]{40}$/i.test(report.sha || '') || !/^[0-9a-f]{64}$/i.test(report.holdout_digest_receipt || '') ||
      (expected.holdout_digest_receipt && report.holdout_digest_receipt !== expected.holdout_digest_receipt) || !validSuccessEvidence(report.evidence)) return 'bootstrap build evidence/SHA/receipt invalid'
  if (!/^pool-[0-9]+$/.test(report.lease_name || '') || !/[\\/]atx-wt[\\/]pool-[0-9]+$/i.test(report.worktree || '') ||
      !report.worktree.replace(/\//g, '\\').toLowerCase().endsWith(`\\${report.lease_name.toLowerCase()}`)) return 'bootstrap build not isolated'
  if (!validLeaseReceipt(report.acquisition_receipt, {
    lease_name: report.lease_name, run_id: expected.run_id, branch: expected.branch, base_sha: expected.base_sha, worktree: report.worktree,
    heartbeat_id: expected.heartbeat_id, keeper_pid: report.keeper_pid, keeper_process_started_utc: report.keeper_process_started_utc,
  }, 'acquire')) return 'bootstrap acquisition receipt invalid'
  return null
}

function bootstrapPrepareError(report, review, prepare, expected) {
  const buildError = bootstrapReportError(report, expected)
  if (buildError) return buildError
  const reviewError = reviewContractError(review, report.sha)
  if (reviewError || review.verdict !== 'APPROVE') return reviewError || 'bootstrap not approved'
  if (!prepare || !prepare.passed || !validSuccessEvidence(prepare.evidence)) return 'scoped verifier missing/failed'
  if (prepare.reviewed_sha !== report.sha || prepare.integration_sha !== report.sha || prepare.integration_branch !== expected.integration_branch ||
      prepare.lease_run_id !== expected.run_id || prepare.next_state !== expected.next_state || prepare.holdout_digest_receipt !== report.holdout_digest_receipt) return 'bootstrap prepare identity/state mismatch'
  if (!/^pool-[0-9]+$/.test(prepare.integration_lease || '') || !/[\\/]atx-wt[\\/]pool-[0-9]+$/i.test(prepare.integration_worktree || '') ||
      !prepare.integration_worktree.replace(/\//g, '\\').toLowerCase().endsWith(`\\${prepare.integration_lease.toLowerCase()}`)) return 'bootstrap verifier not isolated'
  const laneLease = { lease_name: report.lease_name, run_id: expected.run_id, branch: expected.branch, base_sha: expected.base_sha, worktree: report.worktree, heartbeat_id: expected.heartbeat_id, keeper_pid: report.keeper_pid, keeper_process_started_utc: report.keeper_process_started_utc }
  const integrationLease = { lease_name: prepare.integration_lease, run_id: expected.run_id, branch: expected.integration_branch, base_sha: expected.base_sha, worktree: prepare.integration_worktree, heartbeat_id: expected.integration_heartbeat_id, keeper_pid: prepare.keeper_pid, keeper_process_started_utc: prepare.keeper_process_started_utc }
  if (!validLeaseReceipt(prepare.lane_release_receipt, laneLease, 'release')) return 'bootstrap lane release receipt invalid'
  if (!validLeaseReceipt(prepare.acquisition_receipt, integrationLease, 'acquire')) return 'bootstrap integration acquisition receipt invalid'
  if (!validLeaseReceipt(prepare.integration_release_receipt, integrationLease, 'release')) return 'bootstrap integration release receipt invalid'
  if (!validIntegrationCommand(prepare.integration_receipt, report.sha) || prepare.integration_receipt.head_after !== report.sha ||
      !String(prepare.integration_receipt.output || '').includes(report.sha)) return 'exact reviewed SHA integration receipt invalid'
  if (!validHeadReceipt(prepare.head_receipt, 'HEAD', report.sha)) return 'bootstrap HEAD receipt invalid'
  if (!Array.isArray(prepare.gate_receipts)) return 'bootstrap gate receipts missing'
  for (const gateId of expected.gate_ids) {
    const matches = prepare.gate_receipts.filter(receipt => receipt && receipt.gate_id === gateId)
    if (matches.length !== 1 || !validGateReceipt(matches[0], gateId)) return `bootstrap required gate receipt invalid: ${gateId}`
  }
  return null
}

function aggregatePayloadError(payload) {
  if (!payload || typeof payload !== 'object' || Array.isArray(payload)) return 'aggregate payload missing/not object'
  const exactKeys = (value, keys) => { const actual = Object.keys(value).sort(); const wanted = [...keys].sort(); return actual.length === wanted.length && actual.every((key, index) => key === wanted[index]) }
  const payloadKeys = ['schema_version', 'iteration', 'metrics', 'prior_refuted_ids', 'oracle_suspect_cells', 'convention_summary', 'source_symbols']
  if (!exactKeys(payload, payloadKeys) || payload.schema_version !== 1 || typeof payload.iteration !== 'string') return 'aggregate payload keys/version invalid'
  const safeString = value => typeof value === 'string' && value.length > 0 && value.length <= 512 &&
    !/holdout|sha-?256|[0-9a-f]{40,64}|[A-Za-z]:[\\/]|(?:bidPrc|askPrc|srPrc|srVol)\s*[:=]\s*[-+0-9.]|(?:[A-Za-z0-9+/]{80,}={0,2})/i.test(value)
  if (!safeString(payload.iteration) || !safeString(payload.convention_summary)) return 'aggregate payload unsafe scalar'
  for (const key of ['prior_refuted_ids', 'oracle_suspect_cells', 'source_symbols']) {
    if (!Array.isArray(payload[key]) || payload[key].length > 256 || !payload[key].every(safeString)) return `aggregate payload ${key} invalid`
  }
  const metricKeys = ['cell_id', 'mode', 'metric', 'count', 'mae', 'rmse', 'p95', 'within_tolerance_rate', 'unit']
  if (!Array.isArray(payload.metrics) || !payload.metrics.length || payload.metrics.length > 2000) return 'aggregate metrics invalid'
  for (const metric of payload.metrics) {
    if (!metric || typeof metric !== 'object' || Array.isArray(metric) || !exactKeys(metric, metricKeys)) return 'aggregate metric keys invalid'
    if (!safeString(metric.cell_id) || !safeString(metric.metric) || !safeString(metric.unit) || !['A', 'B'].includes(metric.mode) || !Number.isInteger(metric.count) || metric.count < 0 ||
        ![metric.mae, metric.rmse, metric.p95, metric.within_tolerance_rate].every(Number.isFinite) || metric.within_tolerance_rate < 0 || metric.within_tolerance_rate > 1) return 'aggregate metric value invalid'
  }
  return null
}

function metricDeltaConsistent(metric) {
  if (!metric || ![metric.baseline, metric.candidate, metric.delta, metric.pin].every(Number.isFinite)) return false
  const expected = metric.candidate - metric.baseline
  const scale = Math.max(1, Math.abs(metric.baseline), Math.abs(metric.candidate), Math.abs(metric.delta))
  return Math.abs(metric.delta - expected) <= Number.EPSILON * scale * 16
}

function relativeRegression(metric) {
  const denominator = Math.max(Math.abs(metric.baseline), 1e-15)
  return metric.direction === 'lower' ? (metric.candidate - metric.baseline) / denominator : (metric.baseline - metric.candidate) / denominator
}

function ratchetPrepareContractError(ratchet, expected) {
  if (!ratchet || !validSuccessEvidence(ratchet.evidence)) return 'Ratchet evidence missing/failed'
  if (ratchet.tested_sha !== expected.tested_sha || ratchet.tested_branch !== expected.tested_branch || ratchet.base_sha !== expected.tested_sha ||
      ratchet.ratchet_branch !== expected.ratchet_branch || ratchet.lease_run_id !== expected.run_id || ratchet.heartbeat_id !== expected.heartbeat_id ||
      !/^[0-9a-f]{40}$/i.test(ratchet.ratchet_sha || '')) return 'Ratchet exact integration/base identity mismatch'
  if (!/^pool-[0-9]+$/.test(ratchet.lease_name || '') || !/[\\/]atx-wt[\\/]pool-[0-9]+$/i.test(ratchet.worktree || '') ||
      !ratchet.worktree.replace(/\//g, '\\').toLowerCase().endsWith(`\\${ratchet.lease_name.toLowerCase()}`)) return 'Ratchet lease/worktree invalid'
  const lease = { lease_name: ratchet.lease_name, run_id: expected.run_id, branch: expected.ratchet_branch, base_sha: expected.tested_sha, worktree: ratchet.worktree, heartbeat_id: expected.heartbeat_id, keeper_pid: ratchet.keeper_pid, keeper_process_started_utc: ratchet.keeper_process_started_utc }
  if (!validLeaseReceipt(ratchet.acquisition_receipt, lease, 'acquire')) return 'Ratchet acquisition receipt invalid'
  if (!validLeaseReceipt(ratchet.release_receipt, lease, 'release')) return 'Ratchet release receipt invalid'
  const digest = ratchet.digest_receipt
  if (!digest || digest.expected_digest !== expected.holdout_digest || digest.actual_digest !== expected.holdout_digest || digest.exit_code !== 0 ||
      !/holdout/i.test(digest.command || '') || !/digest/i.test(digest.command || '') || !String(digest.output || '').includes(expected.holdout_digest)) return 'Ratchet digest recomputation receipt invalid'
  if (!Array.isArray(ratchet.applicable_modes) || ratchet.applicable_modes.length !== expected.applicable_modes.length || !expected.applicable_modes.every(mode => ratchet.applicable_modes.includes(mode))) return 'Ratchet applicable modes mismatch'
  if (!Array.isArray(ratchet.metrics) || !ratchet.metrics.length || !Array.isArray(ratchet.metric_evidence)) return 'Ratchet typed metrics missing'
  for (const metric of ratchet.metrics) {
    if (!metricDeltaConsistent(metric)) return 'Ratchet metric delta inconsistent'
    if (!['A', 'B', 'ALL'].includes(metric.mode) || !['target', 'aggregate', 'speed'].includes(metric.gate) || !['lower', 'higher'].includes(metric.direction) ||
        !Number.isInteger(metric.evidence_index) || metric.evidence_index < 0 || metric.evidence_index >= ratchet.metric_evidence.length) return 'Ratchet metric shape/evidence index invalid'
    const evidence = ratchet.metric_evidence[metric.evidence_index]
    if (!evidence || evidence.exit_code !== 0 || !/holdout|speed/i.test(evidence.command || '') || !String(evidence.output || '').includes(metric.metric_id) ||
        !String(evidence.output || '').includes(String(metric.baseline)) || !String(evidence.output || '').includes(String(metric.candidate)) ||
        !String(evidence.output || '').includes(String(metric.delta))) return 'Ratchet metric not supported by referenced output'
  }
  if (!ratchet.metrics.some(metric => metric.gate === 'aggregate') || ratchet.metrics.filter(metric => metric.gate === 'speed').length !== 1) return 'Ratchet aggregate/speed metrics missing'
  for (const mode of expected.applicable_modes) if (!ratchet.metrics.some(metric => metric.gate === 'target' && metric.mode === mode)) return `Ratchet target metrics missing for Mode ${mode}`
  if (!Array.isArray(ratchet.gate_receipts)) return 'Ratchet gate receipts missing'
  for (const gateId of RATCHET_GATE_IDS) {
    const matches = ratchet.gate_receipts.filter(receipt => receipt && receipt.gate_id === gateId)
    if (matches.length !== 1 || !validGateReceipt(matches[0], gateId)) return `Ratchet required gate receipt invalid: ${gateId}`
  }
  if (!Array.isArray(ratchet.oracle_suspects_excluded) || !Array.isArray(ratchet.market_evidence) || ratchet.market_evidence.length !== ratchet.oracle_suspects_excluded.length) return 'Ratchet suspect market evidence missing'
  for (const cell of ratchet.oracle_suspects_excluded) {
    const matches = ratchet.market_evidence.filter(receipt => receipt && receipt.cell_id === cell)
    if (matches.length !== 1 || matches[0].market_sides_with !== 'atx-vol' || matches[0].exit_code !== 0 || !String(matches[0].command || '').includes(cell) || !String(matches[0].output || '').trim()) return `Ratchet suspect evidence invalid: ${cell}`
  }
  if (!ratchet.northstar_updated || !Array.isArray(ratchet.ledger_appended) || !ratchet.ledger_appended.length || typeof ratchet.holdout_summary !== 'string' || !ratchet.holdout_summary.trim()) return 'Ratchet memory/summary incomplete'
  return null
}

function computeRatchetVerdict(ratchet) {
  const targetPass = ratchet.metrics.filter(metric => metric.gate === 'target').every(metric => metric.direction === 'lower' ? metric.candidate < metric.baseline : metric.candidate > metric.baseline)
  const aggregatePass = ratchet.metrics.filter(metric => metric.gate === 'aggregate').every(metric => relativeRegression(metric) <= 0.02 + 1e-12)
  const speed = ratchet.metrics.find(metric => metric.gate === 'speed')
  const speedPass = speed.direction === 'higher' && speed.candidate >= speed.pin
  return targetPass && aggregatePass && speedPass ? 'ACCEPT' : 'REJECT'
}

phase('Capability')
const capability = await agent(
  `Run exactly powershell scripts\\oracle-capability.ps1. Return its JSON unchanged. No other command or file access.`,
  { agentType: 'vol-capability-inspector', schema: CAPABILITY, label: 'capability' },
)
if (!capability || capability.canonical_ref !== CANONICAL_REF || !/^[0-9a-f]{40}$/i.test(capability.base_sha || '') || !validSuccessEvidence(capability.evidence)) throw new Error('capability freeze invalid')
if (capability.base_ref !== (capability.canonical_exists ? CANONICAL_REF : REQUESTED_BASE)) throw new Error('capability base-ref selection invalid')
if (capability.evidence.length !== 1 || capability.evidence[0].command !== 'powershell scripts\\oracle-capability.ps1' ||
    !capability.evidence[0].output.includes(`state=${capability.state}`) ||
    !capability.evidence[0].output.includes(`canonical_exists=${String(capability.canonical_exists)}`)) throw new Error('capability probe receipt invalid')
if (capability.state !== 'missing_data' && !/^[0-9a-f]{64}$/i.test(capability.holdout_digest_receipt || '')) throw new Error('holdout digest receipt invalid')
const BASE_SHA = capability.base_sha
const CANONICAL_EXPECTED_OLD = capability.canonical_exists ? BASE_SHA : ZERO_SHA

if (capability.state !== 'ready') {
  const lane = BOOTSTRAP_LANES[capability.state]
  const branch = `lane/oracle-bootstrap-${lane.slug}-${RUN_SLUG}`
  const heartbeat = `${RUN_SLUG}-bootstrap-${lane.slug}`
  const integrationBranch = `integration/oracle-bootstrap-${lane.slug}-${RUN_SLUG}`
  const integrationHeartbeat = `${RUN_SLUG}-bootstrap-integration`
  const expected = { state: capability.state, branch, base_sha: BASE_SHA, run_id: RUN_ID, heartbeat_id: heartbeat, holdout_digest_receipt: capability.holdout_digest_receipt, integration_branch: integrationBranch, integration_heartbeat_id: integrationHeartbeat, next_state: lane.next, gate_ids: lane.gate_ids }
  phase('Bootstrap Build')
  let report = await agent(
    `ONE fixed bootstrap lane; no planner/holdout. Stage=${lane.stage}, base=${BASE_SHA}. ${lane.contract} Acquire ${branch} with RunId=${RUN_ID}, HeartbeatId=${heartbeat}; the independent keeper owns liveness. Implement, scoped-test, commit, keep lease, and return typed keeper acquisition plus exit-code-zero evidence.`,
    { agentType: 'vol-builder', schema: BOOTSTRAP_REPORT, label: `bootstrap-build:${capability.state}` },
  )
  let reportError = bootstrapReportError(report, expected)
  let review = null
  if (!reportError) {
    phase('Bootstrap Review')
    review = await agent(`Fresh exact-SHA review ${BASE_SHA}...${report.sha}. Verify stage ${lane.stage}, no holdout, tests/evidence.`, { agentType: 'vol-reviewer', schema: REVIEW, label: `bootstrap-review:${capability.state}` })
    reportError = reviewContractError(review, report.sha)
  }
  if (!reportError && review.verdict === 'BLOCK') {
    phase('Bootstrap Fix')
    report = await agent(`Fix exactly blockers ${JSON.stringify(review.findings.filter(finding => finding.severity === 'blocker'))} in ${report.worktree}; keep same keeper lease, rerun checks, commit, return new SHA/receipts.`, { agentType: 'vol-builder', schema: BOOTSTRAP_REPORT, label: `bootstrap-fix:${capability.state}` })
    reportError = bootstrapReportError(report, expected)
    if (!reportError) {
      phase('Bootstrap Re-review')
      review = await agent(`FRESH post-Fix review of exactly ${report.sha}; never reuse prior verdict.`, { agentType: 'vol-reviewer', schema: REVIEW, label: `bootstrap-rereview:${capability.state}` })
      reportError = reviewContractError(review, report.sha)
    }
  }
  if (reportError || !review || review.verdict !== 'APPROVE') {
    let cleanup = null
    if (report && /^pool-[0-9]+$/.test(report.lease_name || '') && report.lease_run_id === RUN_ID) cleanup = await agent(`Abort without integration. Release only ${report.lease_name} with RunId=${RUN_ID}; return typed release.`, { agentType: 'vol-verifier', schema: CLEANUP, label: 'bootstrap-abort-cleanup' })
    return { iteration: `bootstrap-${lane.stage}`, capability_state: capability.state, verdict: 'FAILED', holdout: null, confirmed: [], refuted: [], sprint: null, ledger: [], ratchet_evidence: [], failure: reportError || 'bootstrap not approved', cleanup, run_id: RUN_ID, base_sha: BASE_SHA, canonical_after: null }
  }
  phase('Bootstrap Verify')
  const prepare = await agent(
    `PREPARE ONLY; never update canonical. Release ${report.lease_name}; acquire new keeper lease ${integrationBranch} at ${BASE_SHA}, RunId=${RUN_ID}, HeartbeatId=${integrationHeartbeat}. Integrate exact ${report.sha}, prove HEAD, run exactly gates ${JSON.stringify(lane.gate_ids)}, prove next_state=${lane.next} without holdout membership, release. Return typed release/acquire/integration/HEAD/gate/release receipts.`,
    { agentType: 'vol-verifier', schema: BOOTSTRAP_PREPARE, label: `bootstrap-prepare:${capability.state}` },
  )
  const prepareError = bootstrapPrepareError(report, review, prepare, expected)
  if (prepareError) return { iteration: `bootstrap-${lane.stage}`, capability_state: capability.state, verdict: 'FAILED', holdout: null, confirmed: [], refuted: [], sprint: null, ledger: [], ratchet_evidence: [], failure: prepareError, bootstrap: { report, review, prepare }, run_id: RUN_ID, base_sha: BASE_SHA, canonical_after: null }
  phase('Bootstrap Finalize')
  const casExpected = { ref: CANONICAL_REF, new_sha: report.sha, expected_old_sha: CANONICAL_EXPECTED_OLD }
  let finalize = null
  let finalizeThrown = null
  try {
    finalize = await agent(`Run exactly git update-ref ${CANONICAL_REF} ${report.sha} ${CANONICAL_EXPECTED_OLD}; do nothing else; return typed receipt.`, { agentType: 'vol-ref-finalizer', schema: CAS_RECEIPT, label: 'bootstrap-cas-finalizer' })
  } catch (error) { finalizeThrown = String(error) }
  const finalizeError = casReceiptError(finalize, casExpected)
  phase('Bootstrap Audit')
  let audit = null
  let auditThrown = null
  try {
    audit = await agent(`Read-only: run exactly git rev-parse ${CANONICAL_REF}; return actual SHA with exit 0, or sha/output=MISSING with the command's nonzero exit.`, { agentType: 'vol-ref-auditor', schema: HEAD_RECEIPT, label: 'bootstrap-post-cas-audit' })
  } catch (error) { auditThrown = String(error) }
  const auditError = auditThrown || auditReceiptError(audit, CANONICAL_REF)
  const canonicalAfter = auditError || audit.sha === 'MISSING' ? null : audit.sha
  const landed = !finalizeError && !auditError && canonicalAfter === report.sha
  return { iteration: `bootstrap-${lane.stage}`, capability_state: capability.state, verdict: landed ? 'BOOTSTRAP' : 'FAILED', holdout: null, confirmed: [], refuted: [], sprint: null, ledger: [], ratchet_evidence: [], bootstrap: { report, review, prepare, finalize, audit }, failure: landed ? null : (finalizeThrown || finalizeError || auditError || 'post-CAS canonical mismatch'), landing_status: landed ? 'COMMITTED' : (canonicalAfter === report.sha ? 'LANDED_AUDITED_WITH_INVALID_RECEIPT' : 'NOT_COMMITTED'), run_id: RUN_ID, base_sha: BASE_SHA, canonical_after: canonicalAfter }
}

if (!capability.canonical_exists) throw new Error('ready requires existing canonical ref')
phase('Measure')
const measureBranch = `lane/oracle-measure-${capability.next_iter}-${RUN_SLUG}`
const measureHeartbeat = `${RUN_SLUG}-measure`
const measure = await agent(`Measure ${capability.next_iter} from ${BASE_SHA} in keeper-backed ${measureBranch}, RunId=${RUN_ID}, HeartbeatId=${measureHeartbeat}. Run aggregate smoke+tune A+B and pinned speed; commit/release. Return strict attribution_payload schema_version=1 only, with no unknown keys, paths, hashes, membership, rows, or encoded blobs, plus typed acquire/release.`, { schema: MEASURE, label: 'measure' })
const measureLease = measure && { lease_name: measure.lease_name, run_id: RUN_ID, branch: measureBranch, base_sha: BASE_SHA, worktree: measure.worktree, heartbeat_id: measureHeartbeat, keeper_pid: measure.keeper_pid, keeper_process_started_utc: measure.keeper_process_started_utc }
const measureError = !measure || measure.status !== 'ok' || measure.iter !== capability.next_iter || measure.branch !== measureBranch || measure.base_sha !== BASE_SHA || measure.lease_run_id !== RUN_ID || measure.heartbeat_id !== measureHeartbeat || !/^[0-9a-f]{40}$/i.test(measure.sha || '') || !measure.lease_released || !validSuccessEvidence(measure.evidence) || !validLeaseReceipt(measure.acquisition_receipt, measureLease || {}, 'acquire') || !validLeaseReceipt(measure.release_receipt, measureLease || {}, 'release') || aggregatePayloadError(measure.attribution_payload)
if (measureError) return { iteration: capability.next_iter, capability_state: 'ready', verdict: 'FAILED', holdout: null, confirmed: [], refuted: [], sprint: null, ledger: [], ratchet_evidence: [], failure: 'Measure failed strict contract; no holdout', run_id: RUN_ID, base_sha: BASE_SHA, canonical_after: null }

phase('Attribute')
const attribution = await agent(`Tool-less aggregate attribution. Strict payload:\n${JSON.stringify(measure.attribution_payload)}\nRank 1-3 falsifiable hypotheses and declare modes A/B. Never request tools, paths, hashes, membership, rows, or encoded data.${FOCUS ? `\nFocus: ${FOCUS}` : ''}`, { agentType: 'vol-analyst', schema: ATTR, label: 'attribute' })
if (!attribution || !Array.isArray(attribution.hypotheses) || !attribution.hypotheses.length || attribution.hypotheses.some(item => !Array.isArray(item.modes) || !item.modes.length)) return { iteration: measure.iter, capability_state: 'ready', verdict: 'FAILED', holdout: null, confirmed: [], refuted: [], sprint: null, ledger: [], ratchet_evidence: [], failure: 'Attribution failed; no holdout', run_id: RUN_ID, base_sha: BASE_SHA, canonical_after: null }
const applicableModes = [...new Set(attribution.hypotheses.flatMap(item => item.modes))].sort()

phase('Improve')
let sprint
try { sprint = await workflow('vol-sprint', { task: `Oracle RSI ${measure.iter}; typed aggregate hypotheses:\n${JSON.stringify(attribution.hypotheses)}\nHard cutover; CHANGELOG BREAKING; no flags/shims/licensed rows.`, base: measure.sha }) }
catch (error) { sprint = { passed: false, error: String(error) } }
const sprintValid = sprint && sprint.passed && /^integration\//.test(sprint.integration_branch || '') && /^[0-9a-f]{40}$/i.test(sprint.integration_sha || '') && validSuccessEvidence(sprint.gate_evidence)
if (!sprintValid) return { iteration: measure.iter, capability_state: 'ready', verdict: 'FAILED', holdout: null, confirmed: [], refuted: [], sprint: sprint || null, ledger: [], ratchet_evidence: [], failure: 'Sprint incomplete/invalid; no holdout and no REJECT increment', run_id: RUN_ID, base_sha: BASE_SHA, canonical_after: null }

phase('Ratchet Prepare')
const ratchetBranch = `lane/oracle-ratchet-${RUN_SLUG}`
const ratchetHeartbeat = `${RUN_SLUG}-ratchet`
const ratchet = await agent(`PREPARE ONLY; never update canonical or choose authoritative verdict. Lease keeper-backed ${ratchetBranch} at exact ${sprint.integration_branch}@${sprint.integration_sha}, RunId=${RUN_ID}, HeartbeatId=${ratchetHeartbeat}. Recompute digest=${capability.holdout_digest_receipt}; run gates ${JSON.stringify(RATCHET_GATE_IDS)}. Return typed baseline/candidate/delta target/aggregate/pinned-speed metrics (delta=candidate-baseline), applicable modes exactly ${JSON.stringify(applicableModes)}, and one market receipt per suspect exclusion. Prepare/commit scorecard and memory with memory_verdict derived from rules, release. Workflow independently computes verdict/CAS.`, { agentType: 'vol-verifier', schema: RATCHET_PREPARE, label: 'ratchet-prepare' })
const ratchetError = ratchetPrepareContractError(ratchet, { tested_sha: sprint.integration_sha, tested_branch: sprint.integration_branch, ratchet_branch: ratchetBranch, holdout_digest: capability.holdout_digest_receipt, run_id: RUN_ID, heartbeat_id: ratchetHeartbeat, applicable_modes: applicableModes })
if (ratchetError) return { iteration: measure.iter, capability_state: 'ready', verdict: 'FAILED', holdout: null, confirmed: [], refuted: [], sprint: { passed: true, integration_branch: sprint.integration_branch, integration_sha: sprint.integration_sha }, ledger: [], ratchet_evidence: [], ratchet: null, failure: ratchetError, run_id: RUN_ID, base_sha: BASE_SHA, canonical_after: null }
const computedVerdict = computeRatchetVerdict(ratchet)
if (ratchet.memory_verdict !== computedVerdict) return { iteration: measure.iter, capability_state: 'ready', verdict: 'FAILED', holdout: null, confirmed: [], refuted: [], sprint: { passed: true, integration_branch: sprint.integration_branch, integration_sha: sprint.integration_sha }, ledger: [], ratchet_evidence: [], ratchet: null, failure: 'prepared memory verdict disagrees with workflow computation', run_id: RUN_ID, base_sha: BASE_SHA, canonical_after: null }

let finalize = null
let finalizeError = null
let finalizeThrown = null
if (computedVerdict === 'ACCEPT') {
  phase('Ratchet Finalize')
  const expected = { ref: CANONICAL_REF, new_sha: ratchet.ratchet_sha, expected_old_sha: BASE_SHA }
  try {
    finalize = await agent(`Run exactly git update-ref ${CANONICAL_REF} ${ratchet.ratchet_sha} ${BASE_SHA}; do nothing else; return typed receipt.`, { agentType: 'vol-ref-finalizer', schema: CAS_RECEIPT, label: 'ratchet-cas-finalizer' })
  } catch (error) { finalizeThrown = String(error) }
  finalizeError = casReceiptError(finalize, expected)
}
phase('Ratchet Audit')
let audit = null
let auditThrown = null
try {
  audit = await agent(`Read-only: run exactly git rev-parse ${CANONICAL_REF}; return actual full SHA.`, { agentType: 'vol-ref-auditor', schema: HEAD_RECEIPT, label: 'ratchet-post-decision-audit' })
} catch (error) { auditThrown = String(error) }
const auditError = auditThrown || auditReceiptError(audit, CANONICAL_REF)
const canonicalAfter = auditError || audit.sha === 'MISSING' ? null : audit.sha
const transactionOk = computedVerdict === 'ACCEPT' ? !finalizeError && !auditError && canonicalAfter === ratchet.ratchet_sha : !auditError && canonicalAfter === BASE_SHA
const verdict = transactionOk ? computedVerdict : 'FAILED'
const transactionEvidence = [
  ...ratchet.evidence,
  { command: ratchet.acquisition_receipt.action, exit_code: ratchet.acquisition_receipt.exit_code, output: ratchet.acquisition_receipt.output },
  { command: ratchet.digest_receipt.command, exit_code: ratchet.digest_receipt.exit_code, output: ratchet.digest_receipt.output },
  ...ratchet.metric_evidence,
  ...ratchet.gate_receipts.map(receipt => ({ command: receipt.command, exit_code: receipt.exit_code, output: receipt.output })),
  ...ratchet.market_evidence.map(receipt => ({ command: receipt.command, exit_code: receipt.exit_code, output: receipt.output })),
  { command: ratchet.release_receipt.action, exit_code: ratchet.release_receipt.exit_code, output: ratchet.release_receipt.output },
  ...(finalize ? [{ command: finalize.command, exit_code: finalize.exit_code, output: finalize.output }] : []),
  ...(audit ? [{ command: audit.command, exit_code: audit.exit_code, output: audit.output }] : []),
]
return {
  iteration: measure.iter, capability_state: 'ready', verdict,
  holdout: verdict === 'FAILED' ? null : { summary: ratchet.holdout_summary, metrics: ratchet.metrics, oracle_suspects_excluded: ratchet.oracle_suspects_excluded },
  confirmed: verdict === 'FAILED' ? [] : ratchet.hypotheses_confirmed, refuted: verdict === 'FAILED' ? [] : ratchet.hypotheses_refuted,
  sprint: { passed: true, integration_branch: sprint.integration_branch, integration_sha: sprint.integration_sha }, ledger: verdict === 'FAILED' ? [] : ratchet.ledger_appended,
  ratchet_evidence: transactionEvidence, ratchet: { prepare: ratchet, computed_verdict: computedVerdict, finalize, audit },
  failure: transactionOk ? null : (finalizeThrown || finalizeError || auditError || 'canonical post-decision mismatch'),
  landing_status: transactionOk ? (computedVerdict === 'ACCEPT' ? 'COMMITTED' : 'UNCHANGED_REJECT') : (canonicalAfter === ratchet.ratchet_sha ? 'LANDED_AUDITED_WITH_INVALID_RECEIPT' : 'CANONICAL_MISMATCH'),
  run_id: RUN_ID, base_sha: BASE_SHA, canonical_after: canonicalAfter,
}
