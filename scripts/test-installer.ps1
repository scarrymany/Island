[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
if ($env:GITHUB_ACTIONS -ne 'true' -or [string]::IsNullOrWhiteSpace($env:RUNNER_TEMP)) {
    throw 'Installer acceptance runs only on a disposable GitHub Actions Windows runner.'
}

$workspace = Split-Path -Parent $PSScriptRoot
$registryPath = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\Island'
$machineRegistryPath = 'HKLM:\Software\Microsoft\Windows\CurrentVersion\Uninstall\Island'
if ((Test-Path -LiteralPath $registryPath) -or (Test-Path -LiteralPath $machineRegistryPath)) {
    throw 'The runner already has Island installed; refusing to change an existing installation.'
}
$runnerRoot = [IO.Path]::GetFullPath($env:RUNNER_TEMP).TrimEnd('\') + '\'
$testRoot = [IO.Path]::GetFullPath((Join-Path $runnerRoot ('island-installer-' + [Guid]::NewGuid().ToString('N'))))
$installPath = Join-Path $testRoot 'SCARP ISLAND'
if (!$installPath.StartsWith($runnerRoot, [StringComparison]::OrdinalIgnoreCase) -or (Test-Path -LiteralPath $testRoot)) {
    throw 'Installer test directory is not an unused path under RUNNER_TEMP.'
}
New-Item -ItemType Directory -Path $testRoot | Out-Null

function Invoke-TestProcess([string]$Path, [string]$Arguments, [int]$TimeoutMs = 60000) {
    $process = Start-Process -FilePath $Path -ArgumentList $Arguments -WindowStyle Hidden -PassThru
    try {
        if (!$process.WaitForExit($TimeoutMs)) {
            $process.Kill($true)
            throw "Process timed out: $Path"
        }
        if ($process.ExitCode -ne 0) { throw "Process failed ($($process.ExitCode)): $Path" }
    } finally {
        $process.Dispose()
    }
}

$installer = Join-Path $workspace 'dist/Island-Setup.exe'
# NSIS requires /D to be the final argument without quotes, including paths with spaces.
Invoke-TestProcess $installer "/S /D=$installPath"
$application = Join-Path $installPath 'Island.exe'
if (!(Test-Path -LiteralPath $application)) { throw 'Installer did not deploy Island.exe.' }
$registration = Get-ItemProperty -LiteralPath $registryPath
$expectedVersion = [Diagnostics.FileVersionInfo]::GetVersionInfo((Join-Path $workspace 'dist/Island/Island.exe')).ProductVersion
if ($registration.DisplayName -notlike 'SCARP ISLAND*' -or $registration.DisplayVersion -ne $expectedVersion) {
    throw 'Installed application name or version is incorrect.'
}
if (Test-Path -LiteralPath $machineRegistryPath) { throw 'Installer unexpectedly registered a machine-wide application.' }
if ([Diagnostics.FileVersionInfo]::GetVersionInfo($application).ProductVersion -ne $expectedVersion) {
    throw 'Installed binary version does not match the package.'
}

$shortcut = @(Get-ChildItem -LiteralPath ([Environment]::GetFolderPath('Programs')) -Filter 'SCARP ISLAND.lnk' -File -Recurse)
if ($shortcut.Count -ne 1) { throw 'The per-user Start menu shortcut is missing or duplicated.' }
$shell = New-Object -ComObject WScript.Shell
try {
    $link = $shell.CreateShortcut($shortcut[0].FullName)
    try {
        if ($link.TargetPath -ne $application) { throw 'Start menu shortcut points to the wrong executable.' }
    } finally { [void][Runtime.InteropServices.Marshal]::FinalReleaseComObject($link) }
} finally { [void][Runtime.InteropServices.Marshal]::FinalReleaseComObject($shell) }

$diagnostics = Join-Path $testRoot 'installed-smoke.json'
$env:PATH = "$env:WINDIR/System32;$env:WINDIR;$env:WINDIR/System32/Wbem"
Remove-Item Env:QT_PLUGIN_PATH, Env:QT_QPA_PLATFORM_PLUGIN_PATH, Env:QT_QPA_PLATFORM -ErrorAction SilentlyContinue
Invoke-TestProcess $application ('--demo --smoke-test --diagnostics "{0}"' -f $diagnostics) 20000
Get-Content -LiteralPath $diagnostics -Raw | ConvertFrom-Json | Out-Null

$uninstallers = @(Get-ChildItem -LiteralPath $installPath -Filter 'Uninstall*.exe' -File)
if ($uninstallers.Count -ne 1) { throw 'Expected exactly one installed uninstaller.' }
$testUninstaller = Join-Path $testRoot 'uninstall-test.exe'
Copy-Item -LiteralPath $uninstallers[0].FullName -Destination $testUninstaller
# Run a copy outside the install directory so NSIS can delete its installed executable.
Invoke-TestProcess $testUninstaller "/S _?=$installPath"
if ((Test-Path -LiteralPath $registryPath) -or (Test-Path -LiteralPath $application) -or
    (Test-Path -LiteralPath $shortcut[0].FullName)) {
    throw 'Uninstall left application files, registration or the Start menu shortcut behind.'
}
if (Test-Path -LiteralPath $installPath) {
    $remaining = @(Get-ChildItem -LiteralPath $installPath -Force)
    if ($remaining.Count) { throw "Uninstall left files in $installPath." }
}
Write-Output "Installed SCARP ISLAND $expectedVersion, verified the user shortcut and no-SDK startup, and uninstalled successfully."
