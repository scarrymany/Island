param(
    [string]$QtPath = (Join-Path $PSScriptRoot '..\.tools\Qt\6.8.3\msvc2022_64'),
    [ValidateSet('Debug', 'Release')][string]$Configuration = 'Release',
    [switch]$Package
)
$ErrorActionPreference = 'Stop'
$workspace = Split-Path -Parent $PSScriptRoot
$cmakeCommand = Get-Command cmake -ErrorAction SilentlyContinue
$cmake = if ($cmakeCommand) { $cmakeCommand.Source } else { Join-Path $workspace '.venv\Scripts\cmake.exe' }
if (!(Test-Path -LiteralPath $cmake)) { throw 'Install CMake or add it to PATH.' }
if (!(Test-Path -LiteralPath (Join-Path $QtPath 'lib\cmake\Qt6\Qt6Config.cmake'))) { throw "Qt 6.8+ was not found at $QtPath" }
$build = Join-Path $workspace 'build\native'
& $cmake -S $workspace -B $build -G 'Visual Studio 17 2022' -A x64 "-DCMAKE_PREFIX_PATH=$QtPath" -DBUILD_TESTING=ON
if ($LASTEXITCODE) { throw 'CMake configuration failed.' }
& $cmake --build $build --config $Configuration --parallel
if ($LASTEXITCODE) { throw 'Build failed.' }
$env:PATH = (Join-Path $QtPath 'bin') + ';' + $env:PATH
$ctest = Join-Path (Split-Path $cmake) 'ctest.exe'
& $ctest --test-dir $build -C $Configuration --output-on-failure
if ($LASTEXITCODE) { throw 'Tests failed.' }
& $cmake --install $build --config $Configuration --prefix (Join-Path $workspace 'dist\Island')
if ($LASTEXITCODE) { throw 'Deployment failed.' }
if ($Package) {
    $cpack = Join-Path (Split-Path $cmake) 'cpack.exe'
    & $cpack --config (Join-Path $build 'CPackConfig.cmake') -C $Configuration -G NSIS -B (Join-Path $workspace 'dist')
    if ($LASTEXITCODE) { throw 'Installer packaging failed. Install NSIS and add it to PATH.' }
}
