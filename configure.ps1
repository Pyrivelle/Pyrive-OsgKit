param(
    [string]$VcpkgRoot = 'C:\CProj\vcpkg',
    [string]$BuildDirectory = 'build',
    # Optional existing vcpkg installed directory; reuse it without installing ports.
    [string]$InstalledDirectory = ''
)
$ErrorActionPreference = 'Stop'
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path $vswhere)) { throw 'Visual Studio Installer / vswhere was not found.' }
$vs = & $vswhere -latest -prerelease -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw 'Visual Studio with MSVC x64 tools was not found.' }
$vsVersion = & $vswhere -latest -prerelease -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationVersion
$generator = switch (([version]$vsVersion).Major) {
    18 { 'Visual Studio 18 2026' }
    17 { 'Visual Studio 17 2022' }
    default { throw "Unsupported Visual Studio version: $vsVersion" }
}
$cmakeDir = Join-Path $vs 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin'
$ninjaDir = Join-Path $vs 'Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja'
$sevenZipDir = Join-Path $vs 'Common7\IDE\Extensions\Xamarin.VisualStudio\7-Zip'
$cmake = Join-Path $cmakeDir 'cmake.exe'
$vcpkgExe = Join-Path $VcpkgRoot 'vcpkg.exe'
$toolchain = Join-Path $VcpkgRoot 'scripts\buildsystems\vcpkg.cmake'
if (-not (Test-Path $cmake)) { throw 'Install the Visual Studio CMake tools component.' }
if (-not (Test-Path $vcpkgExe)) { throw "Bootstrap vcpkg at $VcpkgRoot first." }
if (-not [System.IO.Path]::IsPathRooted($BuildDirectory)) { $BuildDirectory = Join-Path $PSScriptRoot $BuildDirectory }
$reuseInstalled = -not [string]::IsNullOrWhiteSpace($InstalledDirectory)
$installed = if ($reuseInstalled) { (Resolve-Path -LiteralPath $InstalledDirectory).Path } else { Join-Path $BuildDirectory 'vcpkg_installed' }
if ($reuseInstalled) {
    foreach ($required in @('include/osg/Geometry', 'lib/osg.lib', 'debug/lib/osgd.lib')) {
        if (-not (Test-Path (Join-Path $installed "x64-windows/$required"))) {
            throw "InstalledDirectory is missing x64-windows/$required"
        }
    }
}

$previousPath = $env:PATH
$previousForce = $env:VCPKG_FORCE_SYSTEM_BINARIES
try {
    $env:PATH = "$cmakeDir;$ninjaDir;$sevenZipDir;$env:PATH"
    if ((Get-Command 7z -ErrorAction SilentlyContinue) -and (Get-Command ninja -ErrorAction SilentlyContinue)) {
        $env:VCPKG_FORCE_SYSTEM_BINARIES = '1'
    }
    # Install explicitly here. VS regenerations then use the installed dependencies
    # without requiring this script's PATH or downloading vcpkg tools again.
    if (-not $reuseInstalled) {
        & $vcpkgExe install --triplet x64-windows "--x-manifest-root=$PSScriptRoot" "--x-install-root=$installed"
        if ($LASTEXITCODE -ne 0) { throw "vcpkg install failed: $LASTEXITCODE" }
    }
    Write-Output "Configuring with $generator at $vs"
    & $cmake -S $PSScriptRoot -B $BuildDirectory -G $generator -A x64 `
        "-DCMAKE_GENERATOR_INSTANCE=$vs" `
        "-DCMAKE_TOOLCHAIN_FILE=$toolchain" "-DVCPKG_INSTALLED_DIR=$installed" `
        -DVCPKG_TARGET_TRIPLET=x64-windows -DVCPKG_MANIFEST_INSTALL=OFF
    if ($LASTEXITCODE -ne 0) { throw "CMake configure failed: $LASTEXITCODE" }
    & (Join-Path $PSScriptRoot 'scripts\FinalizeSolution.ps1') -BuildDirectory $BuildDirectory
} finally {
    $env:PATH = $previousPath
    $env:VCPKG_FORCE_SYSTEM_BINARIES = $previousForce
}
