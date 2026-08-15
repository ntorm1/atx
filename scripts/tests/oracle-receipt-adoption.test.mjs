import test from 'node:test'
import assert from 'node:assert/strict'
import { readFileSync } from 'node:fs'

const read = path => readFileSync(path, 'utf8')

test('Stage 1 is adoption-first and Stage 2 is receipt-first with targeted gates', () => {
  const workflow = read('.claude/workflows/vol-oracle-iter.js')
  const stage1 = workflow.match(/missing_data:\s*\{[^\n]+/u)?.[0] || ''
  const stage2 = workflow.match(/missing_mode_a:\s*\{[^\n]+/u)?.[0] || ''
  assert.match(stage1, /oracle-adopt-existing-data\.ps1/u)
  assert.match(stage1, /ADOPTED/u)
  assert.match(stage1, /INGEST_REQUIRED/u)
  assert.match(stage1, /gate_ids: \['aggregate_store', 'ingest_manifest', 'cohort_manifests', 'holdout_digest'\]/u)
  assert.doesNotMatch(stage1, /gate_ids: \[[^\]]*'disk'/u)
  assert.match(stage2, /targeted Mode A gates first/u)
  assert.match(stage2, /write only bootstrap\/mode-a\.json/u)
  assert.match(stage2, /Implement\/fix Mode A only when an exact targeted gate proves it necessary/u)
})

test('adoption and preflight contracts are aggregate-only and metadata-only', () => {
  const adoption = read('scripts/oracle-adopt-existing-data.ps1')
  const metadata = read('atx-vol/scripts/oracle_store_metadata.py')
  const preflight = read('scripts/oracle-bootstrap-preflight.ps1')
  assert.match(adoption, /Get-CommittedJson/u)
  assert.match(adoption, /Test-Disjoint/u)
  assert.match(adoption, /holdout_membership_sha256/u)
  assert.doesNotMatch(adoption, /atx-vol-oracle-bench/u)
  assert.match(metadata, /ParquetFile\(path\)\.metadata/u)
  assert.doesNotMatch(metadata, /read_table|to_pandas|scan_parquet/u)
  assert.match(preflight, /'aggregate_store'/u)
})
