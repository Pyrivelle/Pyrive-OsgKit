param([Parameter(Mandatory=$true)][string]$BuildDirectory)
$ErrorActionPreference = 'Stop'
$sourceRoot = [System.IO.Path]::GetFullPath((Split-Path $PSScriptRoot -Parent))
$buildRoot = [System.IO.Path]::GetFullPath($BuildDirectory).TrimEnd('\')
if (-not $buildRoot.StartsWith($sourceRoot + '\', [System.StringComparison]::OrdinalIgnoreCase)) {
    throw 'BuildDirectory must stay inside the workspace for generated-file cleanup.'
}
$generated = Join-Path $buildRoot 'PereVia.sln'
if (-not (Test-Path $generated)) { throw 'CMake did not generate PereVia.sln.' }
$relative = [Uri]::UnescapeDataString(([Uri]($sourceRoot + '\')).MakeRelativeUri([Uri]($buildRoot + '\')).ToString()).Replace('/', '\')
$lines = [System.IO.File]::ReadAllLines($generated)
$allowed = @('PereViaOsg', 'PereViaOsgTests')
$guids = @{}
foreach ($line in $lines) {
    if ($line -match '^Project\("[^"]+"\) = "([^"]+)", "([^"]+)", "({[^}]+})"' -and $allowed -contains $Matches[1]) {
        $guids[$Matches[3]] = $true
    }
}
if ($guids.Count -ne 2) { throw 'Expected exactly the PereViaOsg and PereViaOsgTests targets.' }
$result = [System.Collections.Generic.List[string]]::new()
$skip = $false
foreach ($line in $lines) {
    if ($line -match '^Project\("[^"]+"\) = "([^"]+)", "([^"]+)", "({[^}]+})"') {
        $name = $Matches[1]
        $projectPath = $Matches[2]
        $skip = $allowed -notcontains $name
        if (-not $skip) { $line = $line.Replace('"' + $projectPath + '"', '"' + $relative + $projectPath + '"') }
    }
    if (-not $skip) {
        if ($line -match '^\s*({[^}]+})[.\s=]' -and -not $guids.ContainsKey($Matches[1])) { continue }
        $result.Add($line)
    }
    if ($line -eq 'EndProject') { $skip = $false }
}
$output = Join-Path $sourceRoot 'PereVia.sln'
[System.IO.File]::WriteAllLines($output, $result, [System.Text.UTF8Encoding]::new($true))

# Only remove disposable generated solution/project files within the checked build tree.
Get-ChildItem -LiteralPath $buildRoot -Recurse -File | Where-Object {
    $_.Extension -eq '.sln' -or $_.Name -like '*.vcxproj*'
} | ForEach-Object {
    $absolute = [System.IO.Path]::GetFullPath($_.FullName)
    if (-not $absolute.StartsWith($buildRoot + '\', [System.StringComparison]::OrdinalIgnoreCase)) { throw 'Unsafe generated path.' }
    $keep = ($_.DirectoryName -eq $buildRoot) -and ($_.Name -match '^PereViaOsg(Tests)?\.vcxproj(\.filters|\.user)?$')
    if (-not $keep) { Remove-Item -LiteralPath $absolute -Force }
}
Write-Output "Generated $output with exactly two projects."
