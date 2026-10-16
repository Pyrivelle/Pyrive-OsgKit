param(
    [ValidateSet('Debug', 'Release')][string]$Configuration = 'Release',
    [ValidateSet('PereViaOsg', 'PereViaOsgTests')][string]$Target = 'PereViaOsg',
    [string]$BuildDirectory = 'build',
    [switch]$RunTests
)
$ErrorActionPreference = 'Stop'
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$vs = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
$cmake = Join-Path $vs 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
if (-not [System.IO.Path]::IsPathRooted($BuildDirectory)) { $BuildDirectory = Join-Path $PSScriptRoot $BuildDirectory }
if ($RunTests) { $Target = 'PereViaOsgTests' }
& $cmake --build $BuildDirectory --config $Configuration --target $Target --parallel 4
if ($LASTEXITCODE -ne 0) { throw "Build failed: $LASTEXITCODE" }
if ($RunTests) {
    & (Join-Path $BuildDirectory "$Configuration\PereViaOsgTests.exe") --tests
    if ($LASTEXITCODE -ne 0) { throw "Tests failed: $LASTEXITCODE" }
}
