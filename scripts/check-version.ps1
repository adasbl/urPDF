<#
.SYNOPSIS
Verifies that every file carrying the application version agrees.
.DESCRIPTION
The version is set with scripts/bump-version.ps1. This script fails when a file was
edited by hand and no longer matches the others, or when a release tag does not match
the version in the files. MSIX uses four components (x.y.z.0), everything else x.y.z.
.EXAMPLE
.\scripts\check-version.ps1
.EXAMPLE
.\scripts\check-version.ps1 -Tag v0.2.0
#>
[CmdletBinding()]
param(
    # Optional release tag (v0.2.0 or 0.2.0) that must match the version in the files.
    [string] $Tag,

    # Optional built urPDF.exe whose embedded file/product version must match as well.
    [string[]] $ExePath = @()
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

. (Join-Path $PSScriptRoot 'version-sources.ps1')
$repositoryRoot = Split-Path -Parent $PSScriptRoot

$found = foreach ($source in $script:VersionSources) {
    $path = Join-Path $repositoryRoot $source.RelativePath
    $content = [System.IO.File]::ReadAllText($path)
    $matches = [regex]::Matches($content, $source.Pattern)
    if ($matches.Count -ne 1) {
        throw "Expected exactly one application version in $($source.RelativePath); found $($matches.Count)."
    }

    [pscustomobject]@{
        File   = $source.RelativePath
        Found  = $matches[0].Groups['version'].Value
        Suffix = $source.Suffix
    }
}

# vcpkg.json is the reference (it feeds the MSI); every other file must match it.
$reference = ($found | Where-Object { $_.File -eq 'vcpkg.json' }).Found
if ($reference -notmatch '\A\d+\.\d+\.\d+\z') {
    throw "vcpkg.json version-string must be x.y.z, found '$reference'."
}

$errors = @()
foreach ($item in $found) {
    $expected = $reference + $item.Suffix
    if ($item.Found -cne $expected) {
        $errors += "$($item.File): found '$($item.Found)', expected '$expected'"
    }
}

if ($Tag) {
    $tagVersion = $Tag -replace '\Arefs/tags/', '' -replace '\Av', ''
    if ($tagVersion -cne $reference) {
        $errors += "Tag '$Tag' does not match the application version '$reference'"
    }
}

foreach ($exe in $ExePath) {
    if (-not (Test-Path -LiteralPath $exe)) {
        $errors += "${exe}: file not found"
        continue
    }
    $info = (Get-Item -LiteralPath $exe).VersionInfo
    if ($info.FileVersion -cne "$reference.0") {
        $errors += "${exe}: FileVersion is '$($info.FileVersion)', expected '$reference.0'"
    }
    if ($info.ProductVersion -cne $reference) {
        $errors += "${exe}: ProductVersion is '$($info.ProductVersion)', expected '$reference'"
    }
}

if ($errors.Count -gt 0) {
    $errors | ForEach-Object { Write-Error $_ -ErrorAction Continue }
    throw "Version mismatch. Run .\scripts\bump-version.ps1 $reference (or the intended version) to synchronise."
}

Write-Output "Version $reference is consistent in $($found.Count) files (MSIX: $reference.0)."
