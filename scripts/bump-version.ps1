<#
.SYNOPSIS
.EXAMPLE
.\scripts\bump-version.ps1 0.2.0
.EXAMPLE
.\scripts\bump-version.ps1 -Version 0.2.0 -WhatIf
#>
[CmdletBinding(SupportsShouldProcess = $true)]
param(
    [Parameter(Mandatory = $true, Position = 0)]
    [ValidatePattern('\A(0|[1-9][0-9]{0,4})\.(0|[1-9][0-9]{0,4})\.(0|[1-9][0-9]{0,4})\z')]
    [string] $Version
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

foreach ($component in $Version.Split('.')) {
    if ([int] $component -gt 65535) {
        throw 'Each version component must be between 0 and 65535 (MSIX limit).'
    }
}

$repositoryRoot = Split-Path -Parent $PSScriptRoot
$packageVersion = "$Version.0"

function Get-VersionChange {
    param(
        [string] $RelativePath,
        [string] $Pattern,
        [string] $NewValue
    )

    $path = Join-Path $repositoryRoot $RelativePath
    $bytes = [System.IO.File]::ReadAllBytes($path)
    $hasBom = $bytes.Length -ge 3 -and
        $bytes[0] -eq 0xEF -and $bytes[1] -eq 0xBB -and $bytes[2] -eq 0xBF
    $encoding = [System.Text.UTF8Encoding]::new($hasBom, $true)
    $content = [System.IO.File]::ReadAllText($path, $encoding)
    $expression = [regex]::new($Pattern)
    $matches = $expression.Matches($content)
    if ($matches.Count -ne 1) {
        throw "Expected exactly one application version in $RelativePath; found $($matches.Count). No files have been changed."
    }

    $match = $matches[0]
    $updatedContent = $content.Remove($match.Index, $match.Length).Insert(
        $match.Index,
        $match.Groups['prefix'].Value + $NewValue + $match.Groups['suffix'].Value
    )

    if ($RelativePath.EndsWith('.json')) {
        $null = ConvertFrom-Json -InputObject $updatedContent
    }
    else {
        $null = [xml] $updatedContent
    }

    [pscustomobject]@{
        Path = $path
        RelativePath = $RelativePath
        OriginalContent = $content
        Content = $updatedContent
        Encoding = $encoding
        OldValue = $match.Groups['version'].Value
        NewValue = $NewValue
    }
}

. (Join-Path $PSScriptRoot 'version-sources.ps1')

$changes = @(
    foreach ($source in $script:VersionSources) {
        Get-VersionChange -RelativePath $source.RelativePath `
            -Pattern $source.Pattern `
            -NewValue ($Version + $source.Suffix)
    }
)

$pendingChanges = @($changes | Where-Object { $_.Content -cne $_.OriginalContent })
if ($pendingChanges.Count -eq 0) {
    Write-Output "All application version fields are already set to $Version (MSIX: $packageVersion)."
    return
}

foreach ($change in $pendingChanges) {
    Write-Output "$($change.RelativePath): $($change.OldValue) -> $($change.NewValue)"
}

if ($PSCmdlet.ShouldProcess($repositoryRoot, "Update urPDF version to $Version in $($pendingChanges.Count) files")) {
    foreach ($change in $pendingChanges) {
        [System.IO.File]::WriteAllText($change.Path, $change.Content, $change.Encoding)
    }
    Write-Output "Version updated. Suggested commit message: bump: version $Version"
}
