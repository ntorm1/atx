param([ValidateSet('Audit','Apply')][string]$Mode='Audit',[string]$ReviewSha256='')
$ErrorActionPreference='Stop'
if(-not $env:ATX_GUARD_JOB){throw 'External guard required'}
$base='C:\atx\.superpowers\sdd\tier1-v4'
$tmpRoot='C:\atx\atx-db\data\alpha_panel\v1\_tmp'
$auditPath=Join-Path $base 'orphan-duckdb-spill-audit-v2.json'
$applyPath=Join-Path $base 'logs\orphan-duckdb-spill-applied-v2.json'
$startPath=Join-Path $base 'logs\orphan-duckdb-spill-apply-v2.start.json'
$producer='C:\atx\atx-db\src\atx_db\alpha_panel\common.py'
$producerSha='9411975069075772e123bf570e85420681de830787191cdbcd358c7d9a110172'
$owners=@(10616,14368,16792,17328,18448,1880,20004,21160,22460,22916,25892,25924,4552,9864)
Add-Type -TypeDefinition @'
using System;
using System.ComponentModel;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;
using Microsoft.Win32.SafeHandles;
public static class OrphanSpillHandleV2 {
    [StructLayout(LayoutKind.Sequential)]
    private struct Info {
        public uint Attributes;
        public System.Runtime.InteropServices.ComTypes.FILETIME Creation, Access, Write;
        public uint Volume, SizeHigh, SizeLow, Links, IndexHigh, IndexLow;
    }
    [DllImport("kernel32.dll", CharSet=CharSet.Unicode, SetLastError=true)]
    private static extern SafeFileHandle CreateFileW(string path,uint access,uint share,IntPtr security,uint disposition,uint flags,IntPtr template);
    [DllImport("kernel32.dll", SetLastError=true)]
    private static extern bool GetFileInformationByHandle(SafeFileHandle handle,out Info info);
    [DllImport("kernel32.dll", CharSet=CharSet.Unicode, SetLastError=true)]
    private static extern uint GetFinalPathNameByHandleW(SafeFileHandle handle,StringBuilder path,uint length,uint flags);
    [DllImport("kernel32.dll", SetLastError=true)]
    private static extern bool SetFileInformationByHandle(SafeFileHandle handle,int kind,[In] byte[] value,uint length);
    public static FileStream Open(string path) {
        // READ | DELETE, no sharing, OPEN_EXISTING, OPEN_REPARSE_POINT.
        // No FILE_FLAG_DELETE_ON_CLOSE: audit/error disposal never deletes.
        SafeFileHandle handle=CreateFileW(path,0x80010000u,0,IntPtr.Zero,3,0x00200080u,IntPtr.Zero);
        if(handle.IsInvalid){int error=Marshal.GetLastWin32Error();handle.Dispose();throw new Win32Exception(error);}
        try {
            Info info;
            if(!GetFileInformationByHandle(handle,out info))throw new Win32Exception(Marshal.GetLastWin32Error());
            if((info.Attributes & 0x410u)!=0)throw new IOException("Opened object is directory or reparse point");
            if(info.Links!=1)throw new IOException("Spill file has additional hard links");
            StringBuilder finalPath=new StringBuilder(32768);
            uint size=GetFinalPathNameByHandleW(handle,finalPath,(uint)finalPath.Capacity,0);
            if(size==0 || size>=finalPath.Capacity)throw new Win32Exception(Marshal.GetLastWin32Error());
            string actual=finalPath.ToString();
            if(actual.StartsWith(@"\\?\",StringComparison.Ordinal))actual=actual.Substring(4);
            if(!String.Equals(actual,Path.GetFullPath(path),StringComparison.OrdinalIgnoreCase))throw new IOException("Opened object escaped exact path");
            return new FileStream(handle,FileAccess.Read);
        } catch {handle.Dispose();throw;}
    }
    public static void MarkDelete(FileStream stream) {
        // FILE_DISPOSITION_INFO contains one BOOLEAN. Class4 targets this
        // exact open object; there is no pathname lookup at deletion time.
        if(!SetFileInformationByHandle(stream.SafeFileHandle,4,new byte[]{1},1))throw new Win32Exception(Marshal.GetLastWin32Error());
    }
}
'@
function Assert-NoReparse([string]$path){
    $resolved=(Resolve-Path -LiteralPath $path -ErrorAction Stop).ProviderPath
    if(-not [String]::Equals($resolved,[IO.Path]::GetFullPath($path),[StringComparison]::OrdinalIgnoreCase)){throw 'Resolved path differs'}
    $item=Get-Item -LiteralPath $resolved -Force
    while($null -ne $item){
        if($item.Attributes -band [IO.FileAttributes]::ReparsePoint){throw 'Reparse member or ancestor'}
        if($item -is [IO.FileInfo]){$item=$item.Directory}
        elseif($item -is [IO.DirectoryInfo]){$item=$item.Parent}
        else{throw 'Unexpected filesystem object'}
    }
}
function Get-Sha([string]$path){return (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant()}
function Assert-OwnersAbsent {
    foreach($owner in $owners){
        if(Get-Process -Id $owner -ErrorAction SilentlyContinue){throw ('Spill PID present; do not infer ownership: '+$owner)}
    }
}
function Get-Inventory {
    $rows=@()
    Assert-NoReparse $tmpRoot
    foreach($owner in $owners){
        $directory=Join-Path $tmpRoot ('spill-'+$owner)
        Assert-NoReparse $directory
        $resolved=(Resolve-Path -LiteralPath $directory).ProviderPath
        if([IO.Path]::GetDirectoryName($resolved) -cne $tmpRoot -or (Get-Item -LiteralPath $resolved).PSIsContainer -ne $true){throw 'Spill escaped exact parent'}
        foreach($file in @(Get-ChildItem -LiteralPath $resolved -Force | Sort-Object Name)){
            Assert-NoReparse $file.FullName
            if($file.PSIsContainer -or $file.Name -cnotmatch '^duckdb_temp_storage_(DEFAULT|S[0-9]+K)-[0-9]+\.tmp$'){throw 'Not a flat native DuckDB spill file'}
            $rows += [pscustomobject]@{path=$file.FullName;owner_pid=$owner;bytes=$file.Length;last_write_utc_ticks=$file.LastWriteTimeUtc.Ticks;creation_utc_ticks=$file.CreationTimeUtc.Ticks}
        }
    }
    if($rows.Count -ne 80 -or ($rows | Measure-Object bytes -Sum).Sum -gt 8GB){throw 'Unexpected bounded spill inventory'}
    return $rows
}
Assert-NoReparse $producer
if((Get-Sha $producer) -cne $producerSha){throw 'Original per-PID spill producer changed'}
Assert-OwnersAbsent
$inspectionPath=Join-Path $base 't17-interruption-inspection-v5.json'
if((Get-Sha $inspectionPath) -cne '6090f38683aadb68a5409cf850ff339447a2a26db87ac9f0b4a69616cd4aaf95'){throw 'Original native inventory changed'}
$inspection=Get-Content -LiteralPath $inspectionPath -Raw | ConvertFrom-Json
$protected=@{}
foreach($entry in $inspection.native_members.PSObject.Properties){$protected[$entry.Name]=$entry.Value}
$snapshot='C:\atx\atx-db\data\alpha_panel\v1\fundamentals_release_v1\generations\r17_20261004_002\snapshot.json'
$protected[$snapshot]=[pscustomobject]@{bytes=10411360;sha256='c73f6293a5785c185567b01c443747aa2c7294614b45bfdaf2b8016ec9807a0c'}
$protected[$producer]=[pscustomobject]@{bytes=(Get-Item -LiteralPath $producer).Length;sha256=$producerSha}
$protected[$inspectionPath]=[pscustomobject]@{bytes=34026;sha256='6090f38683aadb68a5409cf850ff339447a2a26db87ac9f0b4a69616cd4aaf95'}
function Assert-Protected {
    foreach($path in $protected.Keys){
        Assert-NoReparse $path
        if((Get-Item -LiteralPath $path).Length -ne $protected[$path].bytes -or (Get-Sha $path) -cne $protected[$path].sha256){throw 'Protected native/source/evidence bytes changed'}
    }
}
Assert-Protected
$inventory=@(Get-Inventory)
$snapshotText=([IO.File]::ReadAllText($snapshot)).Replace('\\','/').Replace('\','/').ToLowerInvariant()
foreach($owner in $owners){
    if($snapshotText.Contains('/_tmp/spill-'+$owner+'/')){throw 'Frozen snapshot references spill target'}
}
$snapshotText=$null
$handles=New-Object 'System.Collections.Generic.List[System.IO.FileStream]'
try{
    foreach($row in $inventory){
        # Denies read/write/delete sharing and validates the opened object.
        # Acquiring this handle does NOT request deletion in either mode.
        $handle=[OrphanSpillHandleV2]::Open($row.path)
        $handles.Add($handle)
        if($handle.Length -ne $row.bytes){throw 'Locked spill size changed'}
    }
    Assert-OwnersAbsent
    $current=@(Get-Inventory)
    if(($inventory | ConvertTo-Json -Depth 4 -Compress) -cne ($current | ConvertTo-Json -Depth 4 -Compress)){throw 'Spill inventory changed'}
    $record=[ordered]@{
        status='AUDITED_DISPOSABLE_ORPHAN_DUCKDB_SPILL_ONLY'
        authorization='User requested unnecessary artifacts/temp cache cleanup; only proven orphan native spill blocks'
        producer_path=$producer; producer_sha256=$producerSha
        controller_sha256=(Get-Sha $PSCommandPath); owner_pids=$owners
        interpretation='common.connect48-74 assigns this exact per-process temp_directory; these blocks are ephemeral query spill, not DuckDB database/WAL, originals, source members, admitted units, Parquet, TSV or retained failure logs.'
        files=$inventory; file_count=$inventory.Count; logical_bytes=($inventory | Measure-Object bytes -Sum).Sum
        exclusive_delete_capable_handles_acquired=$handles.Count; protected_identities=$protected
        timestamp_utc=[DateTime]::UtcNow.ToString('o'); free_before_bytes=(Get-PSDrive -Name C).Free
        gate_promotion=$false
    }
    if($Mode -ceq 'Audit'){
        if(Test-Path -LiteralPath $auditPath){throw 'Preserve existing audit'}
        [IO.File]::WriteAllText($auditPath,($record | ConvertTo-Json -Depth 8),[Text.UTF8Encoding]::new($false))
    }else{
        if((Test-Path -LiteralPath $applyPath) -or (Test-Path -LiteralPath $startPath)){throw 'Preserve prior application'}
        $prior=Get-Content -LiteralPath $auditPath -Raw | ConvertFrom-Json
        if($prior.controller_sha256 -cne $record.controller_sha256 -or ($prior.files | ConvertTo-Json -Depth 4 -Compress) -cne ($inventory | ConvertTo-Json -Depth 4 -Compress)){throw 'Reviewed audit/code/inventory binding changed'}
        $auditGuardPath=Join-Path $base 'logs\orphan-duckdb-spill-audit-v2.receipt.json'
        $auditGuard=Get-Content -LiteralPath $auditGuardPath -Raw | ConvertFrom-Json
        $expectedCommand=@('C:\Windows\System32\WindowsPowerShell\v1.0\powershell.exe','-NoProfile','-ExecutionPolicy','Bypass','-File',$PSCommandPath,'-Mode','Audit')
        if($auditGuard.status -cne 'completed' -or $auditGuard.returncode -ne 0 -or $auditGuard.cap_hit -isnot [bool] -or $auditGuard.cap_hit -or $auditGuard.stop_reason -or $auditGuard.job_limit_gb -ne 0.4 -or $auditGuard.guard_version -cne 'c58-m0-r1' -or ($auditGuard.command | ConvertTo-Json -Compress) -cne ($expectedCommand | ConvertTo-Json -Compress)){throw 'Exact successful unchanged audit guard required'}
        $reviewPath=Join-Path $base 'review-orphan-duckdb-spill-cleanup-v1.md'
        if($ReviewSha256 -cnotmatch '^[0-9a-f]{64}$' -or (Get-Sha $reviewPath) -cne $ReviewSha256){throw 'Exact independent review binding required'}
        $record.independent_review_sha256=$ReviewSha256
        $record.audit_sha256=Get-Sha $auditPath
        $record.audit_guard_sha256=Get-Sha $auditGuardPath
        Assert-Protected
        Assert-OwnersAbsent
        $record.status='APPLY_STARTED'
        [IO.File]::WriteAllText($startPath,($record | ConvertTo-Json -Depth 8),[Text.UTF8Encoding]::new($false))
        for($index=0;$index -lt $inventory.Count;$index++){
            $row=$inventory[$index]
            Assert-OwnersAbsent
            Assert-NoReparse $row.path
            # The target was opened/validated without read/write/delete
            # sharing. Mark that exact object only after every Apply check.
            if([IO.Path]::GetDirectoryName([IO.Path]::GetDirectoryName($row.path)) -cne $tmpRoot){throw 'File target escaped allowed spill parent'}
            [OrphanSpillHandleV2]::MarkDelete($handles[$index])
        }
    }
}finally{
    foreach($handle in $handles){$handle.Dispose()}
}
Assert-Protected
if($Mode -ceq 'Apply'){
    foreach($row in $inventory){if(Test-Path -LiteralPath $row.path){throw 'Spill file still exists'}}
    $record.status='80_ORPHAN_NATIVE_SPILL_FILES_REMOVED_EMPTY_DIRECTORIES_RETAINED'
    $record.free_after_bytes=(Get-PSDrive -Name C).Free
    $record.observed_free_change_bytes=$record.free_after_bytes-$record.free_before_bytes
    $record.protected_before_after_verified=$true
    [IO.File]::WriteAllText($applyPath,($record | ConvertTo-Json -Depth 8),[Text.UTF8Encoding]::new($false))
}
[pscustomobject]@{status=$record.status;files=$record.file_count;logical_bytes=$record.logical_bytes;free_before_bytes=$record.free_before_bytes;free_after_bytes=$record.free_after_bytes} | ConvertTo-Json -Compress
