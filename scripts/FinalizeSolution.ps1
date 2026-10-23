param([Parameter(Mandatory=$true)][string]$BuildDirectory)
$ErrorActionPreference = 'Stop'
$sourceRoot = [System.IO.Path]::GetFullPath((Split-Path $PSScriptRoot -Parent))
$buildRoot = [System.IO.Path]::GetFullPath($BuildDirectory).TrimEnd('\')
if (-not $buildRoot.StartsWith($sourceRoot + '\', [System.StringComparison]::OrdinalIgnoreCase)) {
    throw 'BuildDirectory must stay inside the workspace for generated-file cleanup.'
}
$generated = Join-Path $buildRoot 'PereVia.sln'
$generatedXml = Join-Path $buildRoot 'PereVia.slnx'
$relative = [Uri]::UnescapeDataString(([Uri]($sourceRoot + '\')).MakeRelativeUri([Uri]($buildRoot + '\')).ToString()).Replace('/', '\')
$allowed = @('PereViaOsg', 'PereViaOsgTests')
if (Test-Path $generatedXml) {
    # The VS 2026 generator emits SLNX. Keep the repository's single root SLN
    # entry point, referencing the native v145 projects and GUIDs from CMake.
    [xml]$solution = Get-Content -LiteralPath $generatedXml -Raw
    $projects = @($solution.Solution.Project | Where-Object {
        $allowed -contains [System.IO.Path]::GetFileNameWithoutExtension($_.Path)
    } | Sort-Object { if ($_.DefaultStartup -eq 'true') { 0 } else { 1 } })
    if ($projects.Count -ne 2) { throw 'Expected exactly two library/demo projects in SLNX.' }
    foreach ($project in $projects) {
        $project.Type = $project.Type.ToUpperInvariant()
        $project.Id = $project.Id.ToUpperInvariant()
    }
    $lines = [System.Collections.Generic.List[string]]::new()
    $lines.Add('Microsoft Visual Studio Solution File, Format Version 12.00')
    $lines.Add('# Visual Studio Version 18')
    foreach ($project in $projects) {
        $name = [System.IO.Path]::GetFileNameWithoutExtension($project.Path)
        $lines.Add(('Project("{{{0}}}") = "{1}", "{2}", "{{{3}}}"' -f $project.Type, $name, $project.Path, $project.Id))
        $lines.Add("`tProjectSection(ProjectDependencies) = postProject")
        foreach ($dependency in $project.BuildDependency) {
            $target = $projects | Where-Object { $_.Path -eq $dependency.Project }
            if ($target) { $lines.Add(("`t`t{{{0}}} = {{{0}}}" -f $target.Id)) }
        }
        $lines.Add("`tEndProjectSection")
        $lines.Add('EndProject')
    }
    $lines.Add('Global')
    $lines.Add("`tGlobalSection(SolutionConfigurationPlatforms) = preSolution")
    foreach ($config in @('Debug', 'Release')) { $lines.Add("`t`t$config|x64 = $config|x64") }
    $lines.Add("`tEndGlobalSection")
    $lines.Add("`tGlobalSection(ProjectConfigurationPlatforms) = postSolution")
    foreach ($project in $projects) {
        foreach ($config in @('Debug', 'Release')) {
            $lines.Add(("`t`t{{{0}}}.{1}|x64.ActiveCfg = {1}|x64" -f $project.Id, $config))
            $excluded = @($project.Build | Where-Object { $_.Project -eq 'false' -and $_.Solution -eq "$config|*" }).Count -gt 0
            if (-not $excluded) { $lines.Add(("`t`t{{{0}}}.{1}|x64.Build.0 = {1}|x64" -f $project.Id, $config)) }
        }
    }
    $lines.Add("`tEndGlobalSection")
    $lines.Add('EndGlobal')
} elseif (Test-Path $generated) {
    $lines = [System.IO.File]::ReadAllLines($generated)
} else {
    throw 'CMake did not generate PereVia.sln or PereVia.slnx.'
}
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
    $_.Extension -in @('.sln', '.slnx') -or $_.Name -like '*.vcxproj*'
} | ForEach-Object {
    $absolute = [System.IO.Path]::GetFullPath($_.FullName)
    if (-not $absolute.StartsWith($buildRoot + '\', [System.StringComparison]::OrdinalIgnoreCase)) { throw 'Unsafe generated path.' }
    $keep = ($_.DirectoryName -eq $buildRoot) -and ($_.Name -match '^PereViaOsg(Tests)?\.vcxproj(\.filters|\.user)?$')
    if (-not $keep) { Remove-Item -LiteralPath $absolute -Force }
}
Write-Output "Generated $output with exactly two projects."
