$ErrorActionPreference = 'Stop'
$stagePath = 'C:\atx\atx-db\data\alpha_panel\v1\fundamentals_v11'
$partialPath = Join-Path $stagePath '_work\parts\history-0001.parquet.partial'
$destinationDirectory = Join-Path $stagePath '_interrupted_batch_0001-20261003T2317Z'
$receiptPath = 'C:\atx\.superpowers\sdd\tier1-v4\logs\t5-preserved-batch-0001-v1.json'
if (Test-Path -LiteralPath $receiptPath) { throw 'Preserve prior receipt' }
$resolvedStage = (Resolve-Path -LiteralPath $stagePath).Path.TrimEnd('\')
$resolvedPartial = (Resolve-Path -LiteralPath $partialPath).Path
$resolvedDestination = [System.IO.Path]::GetFullPath($destinationDirectory)
if (-not $resolvedPartial.StartsWith($resolvedStage + '\', [System.StringComparison]::OrdinalIgnoreCase)) { throw 'Source escaped stage' }
if (-not $resolvedDestination.StartsWith($resolvedStage + '\', [System.StringComparison]::OrdinalIgnoreCase)) { throw 'Destination escaped stage' }
if ($resolvedPartial -ne ($resolvedStage + '\_work\parts\history-0001.parquet.partial')) { throw 'Unexpected source' }
if (Test-Path -LiteralPath $resolvedDestination) { throw 'Preserve destination' }
if (Test-Path -LiteralPath (Join-Path $resolvedStage 'manifest.json')) { throw 'Refuse published stage' }
if (Test-Path -LiteralPath (Join-Path $resolvedStage '_work\parts\batch-0001.json')) { throw 'Refuse completed batch' }
$file = Get-Item -LiteralPath $resolvedPartial
$partialBytes = $file.Length
$partialLastWrite = $file.LastWriteTimeUtc.ToString('o')
$before = (Get-FileHash -LiteralPath $resolvedPartial -Algorithm SHA256).Hash.ToLowerInvariant()
$null = New-Item -ItemType Directory -Path $resolvedDestination
$preserved = Join-Path $resolvedDestination $file.Name
Move-Item -LiteralPath $resolvedPartial -Destination $preserved
$after = (Get-FileHash -LiteralPath $preserved -Algorithm SHA256).Hash.ToLowerInvariant()
if ($after -ne $before) { throw 'Preserved file identity mismatch' }
$record = [ordered]@{ status='PRESERVED_INTERRUPTED_PARTIAL'; source=$resolvedPartial; destination=$preserved; bytes=$partialBytes; sha256=$after; source_last_write_utc=$partialLastWrite; qualification='Forensic partial only; not a completed batch or reusable input.' }
$record | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $receiptPath -Encoding UTF8
$record | ConvertTo-Json -Compress
