#requires -Version 7.0
<#
.SYNOPSIS
Runs bounded, real-account upload/download checks using disposable generated files.
.EXAMPLE
pwsh -File scripts/live-smoke.ps1 -DryRun
.EXAMPLE
pwsh -File scripts/live-smoke.ps1 -Cleanup
.EXAMPLE
pwsh -File scripts/live-smoke.ps1 -Large -TransferTimeoutSeconds 3600 -Cleanup
.NOTES
Uses the CLI's existing config / QUARKPP_CONFIG / QUARKPP_COOKIE resolution.
Never reads or prints credentials. Large transfers require over 12 GiB of free disk
and may consume over 8 GiB of network traffic. Upload restart is not resumed.
JSON lines go to stdout; a credential-free manifest remains under build/live-smoke.
Cleanup moves only this invocation's verified test directory to the recycle bin.
#>
[CmdletBinding()]
param(
    [string] $Executable,
    [string] $Config,
    [string] $ParentFid = '0',
    [string] $OutputRoot,
    [string] $Seed,
    [switch] $Large,
    [switch] $Cleanup,
    [switch] $DryRun,
    [ValidateRange(10, 86400)] [int] $TransferTimeoutSeconds = 900,
    [ValidateRange(5, 600)] [int] $RequestTimeoutSeconds = 60
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
if ($Large -and -not $PSBoundParameters.ContainsKey('TransferTimeoutSeconds')) { $TransferTimeoutSeconds = 3600 }
$repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$runId = (Get-Date).ToUniversalTime().ToString('yyyyMMddTHHmmssZ') + '-' + [Guid]::NewGuid().ToString('N').Substring(0, 8)
$remoteName = 'QuarkPlusPlus-live-' + $runId
if (-not $Seed) { $Seed = $runId }
if (-not $OutputRoot) { $OutputRoot = Join-Path $repo 'build/live-smoke' }
$runRoot = [IO.Path]::GetFullPath((Join-Path $OutputRoot $runId))
if (-not $Executable) {
    $Executable = Join-Path $repo $(if ($IsWindows) { 'dist/bin/quarkpp.exe' } else { 'dist/bin/quarkpp' })
}
$Executable = [IO.Path]::GetFullPath($Executable)
if ($Config) { $Config = [IO.Path]::GetFullPath($Config) }
elseif (-not [string]::IsNullOrWhiteSpace($env:QUARKPP_CONFIG)) {
    # Resolve caller-relative paths before the CLI is launched from the repository.
    $Config = [IO.Path]::GetFullPath($env:QUARKPP_CONFIG)
}
$fixtures = @(
    @{ name = 'empty.bin'; bytes = [long]0 },
    @{ name = 'small-测试.bin'; bytes = [long](1MB + 33) }
)
if ($Large) { $fixtures += @{ name = 'large-over-4GiB.bin'; bytes = [long](4GB + 17MB + 1) } }

function Emit([string] $Event, [hashtable] $Data) {
    $record = [ordered]@{ event = $Event; utc = [DateTime]::UtcNow.ToString('o'); run_id = $runId }
    foreach ($key in $Data.Keys) { $record[$key] = $Data[$key] }
    Write-Output ($record | ConvertTo-Json -Depth 8 -Compress)
}

if ($DryRun) {
    Emit 'plan' @{
        remote_name = $remoteName; parent_fid = $ParentFid; local_directory = $runRoot
        fixtures = $fixtures; cleanup_requested = [bool]$Cleanup
        transfer_timeout_seconds = $TransferTimeoutSeconds
        operations = @('mkdir', 'upload', 'info', 'download', 'sha256', 'resume-from-known-prefix', 'ls')
        network_accessed = $false; credentials_read = $false
    }
    return
}

if (-not (Test-Path -LiteralPath $Executable -PathType Leaf)) { throw 'CLI executable is missing. Build/install first or pass -Executable.' }
if ($Config -and -not (Test-Path -LiteralPath $Config -PathType Leaf)) { throw 'The selected configuration file is missing.' }
if ([string]::IsNullOrWhiteSpace($ParentFid)) { throw 'ParentFid must not be empty.' }
[void][IO.Directory]::CreateDirectory($runRoot)
$manifestPath = Join-Path $runRoot 'manifest.json'
$manifest = [ordered]@{
    run_id = $runId; seed = $Seed; remote_name = $remoteName; parent_fid = $ParentFid
    remote_fid = $null; status = 'running'; cleanup = 'not_requested'; files = @(); operations = @()
}
function Save-Manifest {
    $manifest | ConvertTo-Json -Depth 10 | Set-Content -LiteralPath ($manifestPath + '.tmp') -Encoding utf8
    Move-Item -LiteralPath ($manifestPath + '.tmp') -Destination $manifestPath -Force
}

function Invoke-Cli([string] $Label, [string[]] $Arguments, [int] $Timeout = $RequestTimeoutSeconds) {
    $start = [Diagnostics.ProcessStartInfo]::new($Executable)
    $start.WorkingDirectory = $repo
    $start.UseShellExecute = $false
    $start.CreateNoWindow = $true
    $start.RedirectStandardOutput = $true
    $start.RedirectStandardError = $true
    $start.StandardOutputEncoding = [Text.UTF8Encoding]::new($false)
    $start.StandardErrorEncoding = [Text.UTF8Encoding]::new($false)
    if ($Config) { $start.ArgumentList.Add('--config'); $start.ArgumentList.Add($Config) }
    $start.ArgumentList.Add('--json')
    foreach ($argument in $Arguments) { $start.ArgumentList.Add($argument) }
    $process = [Diagnostics.Process]::new()
    $process.StartInfo = $start
    $timer = [Diagnostics.Stopwatch]::StartNew()
    $peak = $null
    $outcome = 'failed'
    try {
        if (-not $process.Start()) { throw "Could not start operation $Label." }
        $stdout = $process.StandardOutput.ReadToEndAsync()
        $stderr = $process.StandardError.ReadToEndAsync()
        while (-not $process.WaitForExit(250)) {
            $process.Refresh()
            try { $peak = [Math]::Max([long]$peak, $process.PeakWorkingSet64) } catch { }
            if ($timer.Elapsed.TotalSeconds -ge $Timeout) {
                $process.Kill($true)
                [void]$process.WaitForExit(10000)
                throw "Operation $Label exceeded its $Timeout second limit. Cloud work may still exist."
            }
        }
        # Both streams are drained concurrently; no provider output or secrets are logged.
        $text = $stdout.GetAwaiter().GetResult()
        [void]$stderr.GetAwaiter().GetResult()
        if ($process.ExitCode -ne 0) { throw "Operation $Label failed with exit code $($process.ExitCode); raw provider output is suppressed." }
        try { $result = ConvertFrom-Json -InputObject $text -Depth 50 -NoEnumerate }
        catch { throw "Operation $Label returned invalid JSON; raw output is suppressed." }
        $outcome = 'passed'
        return $result
    }
    finally {
        $timer.Stop()
        $operation = @{ operation = $Label; status = $outcome; seconds = [Math]::Round($timer.Elapsed.TotalSeconds, 3); peak_working_set_bytes = $peak }
        $manifest.operations += $operation
        Save-Manifest
        # Information stream keeps structured events separate from the return value.
        Write-Information ($operation | ConvertTo-Json -Compress) -InformationAction Continue
        $process.Dispose()
    }
}

# Xorshift64* gives deterministic bounded-memory fixtures with distinct content
# for each seed. It is test data generation, not a cryptographic random source.
if (-not ('QuarkPPLiveFixture' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.IO;
using System.Text;
using System.Security.Cryptography;
public static class QuarkPPLiveFixture {
    public static void Create(string path, long size, string seed) {
        byte[] digest;
        using (var hash = SHA256.Create()) digest = hash.ComputeHash(Encoding.UTF8.GetBytes(seed));
        ulong state = 0;
        for (int i = 0; i < 8; ++i) state |= (ulong)digest[i] << (8 * i);
        if (state == 0) state = 1;
        var block = new byte[1024 * 1024];
        using (var stream = new FileStream(path, FileMode.CreateNew, FileAccess.Write, FileShare.None)) {
            while (size > 0) {
                int count = (int)Math.Min(block.Length, size);
                for (int i = 0; i < count; i += 8) {
                    state ^= state >> 12; state ^= state << 25; state ^= state >> 27;
                    ulong value = unchecked(state * 2685821657736338717UL);
                    for (int j = 0; j < 8 && i + j < count; ++j) block[i + j] = (byte)(value >> (8 * j));
                }
                stream.Write(block, 0, count); size -= count;
            }
        }
    }
    public static void CopyPrefix(string source, string destination, long length) {
        var block = new byte[1024 * 1024];
        using (var input = File.OpenRead(source))
        using (var output = new FileStream(destination, FileMode.CreateNew, FileAccess.Write, FileShare.None)) {
            while (length > 0) {
                int count = input.Read(block, 0, (int)Math.Min(length, block.Length));
                if (count == 0) throw new EndOfStreamException();
                output.Write(block, 0, count); length -= count;
            }
        }
    }
}
'@
}

$failed = $false
Save-Manifest
try {
    Emit 'started' @{ manifest = $manifestPath; large = [bool]$Large; remote_name = $remoteName }
    $created = Invoke-Cli 'create-test-directory' @('mkdir', $remoteName, '--parent-fid', $ParentFid)
    if (-not $created.fid -or $created.fid -eq '0' -or $created.fid -eq $ParentFid) { throw 'Creation returned an invalid directory ID.' }
    $manifest.remote_fid = [string]$created.fid
    Save-Manifest
    foreach ($fixture in $fixtures) {
        $source = Join-Path $runRoot $fixture.name
        [QuarkPPLiveFixture]::Create($source, $fixture.bytes, ($Seed + '/' + $fixture.name))
        $hash = (Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash
        $file = [ordered]@{ name = $fixture.name; bytes = $fixture.bytes; sha256 = $hash; fid = $null; status = 'generated'; fast_upload = $null }
        $manifest.files += $file
        Save-Manifest
        Emit 'fixture-ready' @{ name = $fixture.name; bytes = $fixture.bytes; sha256 = $hash }
        $uploaded = Invoke-Cli ('upload-' + $fixture.name) @('upload', $source, '--to-fid', $manifest.remote_fid) $TransferTimeoutSeconds
        if (-not $uploaded.fid -or $uploaded.fid -eq '0') { throw 'Upload returned an invalid file ID.' }
        $file.fid = [string]$uploaded.fid
        $file.fast_upload = [bool]$uploaded.fast_upload
        $file.status = 'uploaded'
        Save-Manifest
        $info = Invoke-Cli ('info-' + $fixture.name) @('info', '--fid', $file.fid)
        if ($info.dir -or [long]$info.size -ne $fixture.bytes -or $info.name -ne $fixture.name -or $info.parent_fid -ne $manifest.remote_fid) {
            throw 'Uploaded metadata does not match the fixture and destination.'
        }
        $downloadDir = Join-Path $runRoot 'download'
        [void](Invoke-Cli ('download-' + $fixture.name) @('download', '--fid', $file.fid, '--out', $downloadDir, '--no-resume') $TransferTimeoutSeconds)
        if ((Get-FileHash -LiteralPath (Join-Path $downloadDir $fixture.name) -Algorithm SHA256).Hash -ne $hash) { throw 'Downloaded SHA-256 mismatch.' }
        if ($fixture.bytes -gt 0) {
            $resumeDir = Join-Path $runRoot 'resume'
            [void][IO.Directory]::CreateDirectory($resumeDir)
            $prefix = if ($fixture.bytes -gt 4GB) { [long](4GB + 17) } else { [long]([Math]::Floor($fixture.bytes / 2)) }
            [QuarkPPLiveFixture]::CopyPrefix($source, (Join-Path $resumeDir $fixture.name), $prefix)
            [void](Invoke-Cli ('resume-' + $fixture.name) @('download', '--fid', $file.fid, '--out', $resumeDir) $TransferTimeoutSeconds)
            if ((Get-FileHash -LiteralPath (Join-Path $resumeDir $fixture.name) -Algorithm SHA256).Hash -ne $hash) { throw 'Resumed download SHA-256 mismatch.' }
            $file.resume_offset = $prefix
        }
        $file.status = 'passed'
        Save-Manifest
        Emit 'fixture-passed' @{ name = $file.name; fid = $file.fid; bytes = $file.bytes; sha256 = $hash; fast_upload = $file.fast_upload }
    }
    $listing = @(Invoke-Cli 'list-test-directory' @('ls', '--fid', $manifest.remote_fid))
    foreach ($file in $manifest.files) {
        if (@($listing | Where-Object { $_.fid -eq $file.fid -and [long]$_.size -eq $file.bytes }).Count -ne 1) { throw 'The final listing does not contain exactly one matching uploaded file.' }
    }
    $manifest.status = 'passed'
}
catch {
    $failed = $true
    $manifest.status = 'failed'
    # Controlled messages only: no exception content from credential/config readers.
    Emit 'failed' @{ reason = $_.Exception.Message; manifest = $manifestPath }
}
finally {
    if ($Cleanup -and $manifest.remote_fid) {
        try {
            $info = Invoke-Cli 'verify-cleanup-directory' @('info', '--fid', $manifest.remote_fid)
            if (-not $info.dir -or $info.name -ne $remoteName -or $info.parent_fid -ne $ParentFid -or $info.fid -ne $manifest.remote_fid) { throw 'Cleanup directory identity changed; refusing deletion.' }
            $children = @(Invoke-Cli 'verify-cleanup-contents' @('ls', '--fid', $manifest.remote_fid))
            $known = @($manifest.files | Where-Object { $_.fid } | ForEach-Object { $_.fid })
            if (@($children | Where-Object { $_.dir -or $_.fid -notin $known }).Count -gt 0) { throw 'Untracked content exists in the test directory; refusing deletion.' }
            [void](Invoke-Cli 'recycle-test-directory' @('rm', '--fid', $manifest.remote_fid))
            $manifest.cleanup = 'recycle_requested'
            Emit 'cleanup' @{ status = $manifest.cleanup; fid = $manifest.remote_fid }
        }
        catch {
            $failed = $true
            $manifest.cleanup = 'failed'
            Emit 'cleanup-failed' @{ reason = $_.Exception.Message; fid = $manifest.remote_fid }
        }
    }
    Save-Manifest
    Emit 'finished' @{ status = $manifest.status; cleanup = $manifest.cleanup; remote_fid = $manifest.remote_fid; manifest = $manifestPath }
}
if ($failed) { exit 1 }
