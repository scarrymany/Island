[CmdletBinding(SupportsShouldProcess)]
param(
    [string]$Tag = '',
    [switch]$ValidateOnly,
    [switch]$Publish
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$workspace = Split-Path -Parent $PSScriptRoot
$repository = 'scarrymany/Island'
$dist = Join-Path $workspace 'dist'
$assetNames = @('Island-Setup.exe', 'Island-win-x64.zip')
$manifestPath = Join-Path $dist 'SHA256SUMS'
$cmake = Get-Content -LiteralPath (Join-Path $workspace 'CMakeLists.txt') -Raw
$versionMatch = [regex]::Match($cmake, '(?im)^\s*project\(Island\s+VERSION\s+(\d+\.\d+\.\d+)\s')
if (!$versionMatch.Success) { throw 'The Island project version was not found in CMakeLists.txt.' }
$version = $versionMatch.Groups[1].Value
if ($Tag -and $Tag -cne "v$version") { throw "Tag '$Tag' does not match project version v$version." }
if ($Publish -and (!$Tag -or $ValidateOnly)) { throw 'Publishing requires -Tag and cannot use -ValidateOnly.' }
if ($ValidateOnly) {
    Write-Output "Validated Island $version."
    return
}

if (!$Publish) {
    $applicationDir = Join-Path $dist 'Island'
    $applicationPath = Join-Path $applicationDir 'Island.exe'
    foreach ($required in @($applicationPath, (Join-Path $dist 'Island-Setup.exe'),
        (Join-Path $applicationDir 'Qt6Core.dll'), (Join-Path $applicationDir 'plugins\platforms\qwindows.dll'),
        (Join-Path $applicationDir 'qt.conf'),
        (Join-Path $applicationDir 'LICENSE'), (Join-Path $applicationDir 'THIRD_PARTY_NOTICES.md'))) {
        if (!(Test-Path -LiteralPath $required -PathType Leaf)) { throw "Missing package file: $required. Run scripts/build.ps1 -Package first." }
    }
    $binaryVersion = [System.Diagnostics.FileVersionInfo]::GetVersionInfo($applicationPath).ProductVersion
    if ($binaryVersion -ne $version) { throw "Built executable version '$binaryVersion' does not match '$version'. Rebuild before packaging." }
    $archivePath = Join-Path $dist 'Island-win-x64.zip'
    if ($PSCmdlet.ShouldProcess($archivePath, 'Package the deployed Island folder and write SHA-256 checksums')) {
        Compress-Archive -LiteralPath $applicationDir -DestinationPath $archivePath -CompressionLevel Optimal -Force
        $lines = foreach ($name in $assetNames) {
            $hash = (Get-FileHash -LiteralPath (Join-Path $dist $name) -Algorithm SHA256).Hash.ToLowerInvariant()
            "$hash  $name"
        }
        [System.IO.File]::WriteAllLines($manifestPath, $lines, [System.Text.UTF8Encoding]::new($false))
        Write-Output "Prepared Island $version packages in $dist."
    }
    return
}

$expectedHashes = @{}
foreach ($line in Get-Content -LiteralPath $manifestPath) {
    $match = [regex]::Match($line, '^([0-9a-f]{64})  (Island-Setup\.exe|Island-win-x64\.zip)$')
    if (!$match.Success -or $expectedHashes.ContainsKey($match.Groups[2].Value)) {
        throw 'SHA256SUMS contains an invalid or duplicate entry.'
    }
    $expectedHashes[$match.Groups[2].Value] = $match.Groups[1].Value
}
foreach ($name in $assetNames) {
    $path = Join-Path $dist $name
    if (!$expectedHashes.ContainsKey($name) -or !(Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "Missing release asset or checksum: $name."
    }
    $actualHash = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($actualHash -cne $expectedHashes[$name]) { throw "SHA-256 mismatch for $name." }
}

$tagCommit = & git -C $workspace rev-parse --verify "refs/tags/$Tag^{commit}"
if ($LASTEXITCODE -ne 0) { throw "Local tag '$Tag' is missing. Fetch the release tag first." }
$headCommit = & git -C $workspace rev-parse HEAD
if ($LASTEXITCODE -ne 0 -or $tagCommit -ne $headCommit) { throw 'Checkout HEAD must match the release tag.' }
$changes = & git -C $workspace status --porcelain --untracked-files=no
if ($LASTEXITCODE -ne 0 -or $changes) { throw 'Commit tracked changes before publishing a release.' }
if (!(Get-Command gh -ErrorAction SilentlyContinue)) { throw 'GitHub CLI (gh) is required for publication.' }
if (!$PSCmdlet.ShouldProcess("$repository $Tag", 'Create a release draft, upload packages, verify remote SHA-256 and publish')) { return }

$assets = @($assetNames | ForEach-Object { Join-Path $dist $_ }) + $manifestPath
$notes = Join-Path $workspace "docs/releases/$Tag.md"
$notesArguments = if (Test-Path -LiteralPath $notes -PathType Leaf) { @('--notes-file', $notes) } else { @('--generate-notes') }
& gh release create $Tag @assets --repo $repository --verify-tag @notesArguments --title "SCARP ISLAND $Tag" --draft
if ($LASTEXITCODE -ne 0) { throw 'Could not create the release draft. Existing releases are never overwritten.' }

$releaseId = & gh release view $Tag --repo $repository --json databaseId --jq '.databaseId'
if ($LASTEXITCODE -ne 0 -or $releaseId -notmatch '^\d+$') { throw 'Could not resolve the draft release ID. The release remains a draft.' }
$releaseJson = & gh api "repos/$repository/releases/$releaseId"
if ($LASTEXITCODE -ne 0) { throw 'Could not verify uploaded assets. The release remains a draft.' }
$release = $releaseJson | ConvertFrom-Json
foreach ($name in $assetNames) {
    $asset = @($release.assets | Where-Object { $_.name -ceq $name })
    $localSize = (Get-Item -LiteralPath (Join-Path $dist $name)).Length
    if ($asset.Count -ne 1 -or $asset[0].digest -cne "sha256:$($expectedHashes[$name])" -or $asset[0].size -ne $localSize) {
        throw "GitHub digest or size verification failed for $name. The release remains a draft."
    }
}
& gh release edit $Tag --repo $repository --draft=false --latest --verify-tag
if ($LASTEXITCODE -ne 0) { throw 'Package verification passed, but publishing the draft failed.' }
Write-Output "Published https://github.com/$repository/releases/tag/$Tag"
