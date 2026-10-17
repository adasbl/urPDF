<#
.SYNOPSIS
    Collects the self-contained urPDF payload that the MSI installs, and checks it
    runs without Visual Studio, the Store or any installed runtime.

.DESCRIPTION
    Input is the unpackaged build (WindowsPackageType=None), which is built
    self-contained: Windows App SDK libraries sit next to urPDF.exe. MuPDF is linked
    statically into urPDF.exe, so only the Microsoft C++ runtime still has to be added.

    Steps:
      1. Copy the build output to the payload folder (no .pdb/.lib/.exp).
      2. Add the app-local Microsoft C++ runtime (VC redistributable DLLs).
      3. Verify every static dependency resolves to the payload or to Windows itself.
      4. Start urPDF.exe with a minimal PATH and confirm that the modules it loads
         come from the payload, not from the machine (WindowsApps, VS, vcpkg, ...).

.EXAMPLE
    ./scripts/stage-msi-payload.ps1 -Configuration Release -Platform x64
#>
[CmdletBinding()]
param(
    [ValidateSet('Release', 'Debug')]
    [string] $Configuration = 'Release',

    [ValidateSet('x64', 'ARM64')]
    [string] $Platform = 'x64',

    # Build output of the unpackaged variant. Default: out/msi/<Platform>/<Configuration>/bin
    [string] $SourceDir,

    # Folder to stage. Default: out/msi/<Platform>/<Configuration>/payload
    [string] $OutputDir,

    [switch] $SkipSmokeTest,

    [int] $SmokeSeconds = 8
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$repoRoot = Split-Path -Parent $PSScriptRoot
$buildRoot = Join-Path $repoRoot "out/msi/$Platform/$Configuration"
if (-not $SourceDir) { $SourceDir = Join-Path $buildRoot 'bin' }
if (-not $OutputDir) { $OutputDir = Join-Path $buildRoot 'payload' }

if ($Configuration -eq 'Debug') {
    throw 'The payload is only supported for Release: the debug C++ runtime is not redistributable.'
}
if (-not (Test-Path -LiteralPath (Join-Path $SourceDir 'urPDF.exe'))) {
    throw "urPDF.exe was not found in '$SourceDir'. Build the unpackaged variant first (WindowsPackageType=None)."
}

function Get-VisualStudioPath {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
    $path = & $vswhere -latest -prerelease -products '*' -version '[18.0,19.0)' `
        -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if ($LASTEXITCODE -ne 0 -or -not $path) { throw 'Visual Studio 2026 with the C++ workload was not found.' }
    return $path
}

$visualStudio = Get-VisualStudioPath
$archFolder = if ($Platform -eq 'ARM64') { 'arm64' } else { 'x64' }

# 1. Copy build output -----------------------------------------------------------------
if (Test-Path -LiteralPath $OutputDir) { Remove-Item -LiteralPath $OutputDir -Recurse -Force }
$null = New-Item -ItemType Directory -Force -Path $OutputDir

$sourceFull = (Resolve-Path -LiteralPath $SourceDir).Path
Get-ChildItem -LiteralPath $sourceFull -Recurse -File |
    Where-Object { $_.Extension -notin '.pdb', '.lib', '.exp', '.ilk', '.iobj', '.ipdb' } |
    ForEach-Object {
        $relative = $_.FullName.Substring($sourceFull.Length).TrimStart('\')
        $target = Join-Path $OutputDir $relative
        $null = New-Item -ItemType Directory -Force -Path (Split-Path -Parent $target)
        Copy-Item -LiteralPath $_.FullName -Destination $target
    }

# 2. App-local C++ runtime ---------------------------------------------------------------
# Use the redistributable that matches the toolset used to build (v145 -> VC145).
$redistRoot = Join-Path $visualStudio 'VC/Redist/MSVC'
$crtFolder = Get-ChildItem -LiteralPath $redistRoot -Directory |
    Where-Object { $_.Name -match '^\d+\.\d+\.\d+$' } |
    Sort-Object { [version]$_.Name } -Descending |
    ForEach-Object { Join-Path $_.FullName "$archFolder/Microsoft.VC145.CRT" } |
    Where-Object { Test-Path -LiteralPath $_ } |
    Select-Object -First 1
if (-not $crtFolder) { throw "Microsoft.VC145.CRT for $archFolder was not found under '$redistRoot'." }

Get-ChildItem -LiteralPath $crtFolder -Filter *.dll | Copy-Item -Destination $OutputDir
Write-Host "C++ runtime: $crtFolder"

# 3. Dependency check -------------------------------------------------------------------
$dumpbin = Get-ChildItem -LiteralPath (Join-Path $visualStudio 'VC/Tools/MSVC') -Recurse -Filter dumpbin.exe |
    Where-Object { $_.FullName -like '*Hostx64*x64*' } | Select-Object -First 1 -ExpandProperty FullName
if (-not $dumpbin) { throw 'dumpbin.exe was not found.' }

$payloadFull = (Resolve-Path -LiteralPath $OutputDir).Path
$inPayload = @{}
Get-ChildItem -LiteralPath $payloadFull -File | ForEach-Object { $inPayload[$_.Name.ToLowerInvariant()] = $true }

# The machine running the check usually has these installed, so they would be found in
# System32 even though a clean computer would not have them. They must ship with the app.
$mustShip = '^(vcruntime|msvcp|concrt|vccorlib|vcomp|mfc)\d*'
$debugRuntime = '^(vcruntime|msvcp|concrt|ucrtbase)\S*d\.dll$'
$problems = [System.Collections.Generic.List[string]]::new()

foreach ($binary in Get-ChildItem -LiteralPath $payloadFull -Recurse -File -Include *.dll, *.exe) {
    foreach ($line in (& $dumpbin /dependents $binary.FullName 2>$null)) {
        if ($line -notmatch '^\s+(\S+\.dll)\s*$') { continue }
        $dependency = $Matches[1].ToLowerInvariant()
        if ($dependency -match '^(api|ext)-ms-') { continue }
        if ($dependency -match $debugRuntime) {
            $problems.Add("$($binary.Name) needs debug runtime $dependency")
        }
        elseif ($inPayload.ContainsKey($dependency)) { continue }
        elseif ($dependency -notmatch $mustShip -and (Test-Path -LiteralPath (Join-Path $env:windir "System32/$dependency"))) { continue }
        else { $problems.Add("$($binary.Name) needs $dependency, which is not in the payload") }
    }
}

if ($problems.Count -gt 0) {
    $problems | Sort-Object -Unique | ForEach-Object { Write-Error $_ -ErrorAction Continue }
    throw "Dependency check failed with $($problems.Count) problem(s)."
}
Write-Host 'Dependency check: all static dependencies are in the payload or part of Windows.'

# 4. Smoke test ------------------------------------------------------------------------
if (-not $SkipSmokeTest) {
    $exe = Join-Path $payloadFull 'urPDF.exe'
    $samplePdf = Join-Path ([System.IO.Path]::GetTempPath()) 'urPDF-smoke.pdf'
    $pdfText = @'
%PDF-1.4
1 0 obj<</Type/Catalog/Pages 2 0 R>>endobj
2 0 obj<</Type/Pages/Kids[3 0 R]/Count 1>>endobj
3 0 obj<</Type/Page/Parent 2 0 R/MediaBox[0 0 200 200]/Contents 4 0 R/Resources<</Font<</F1 5 0 R>>>>>>endobj
4 0 obj<</Length 44>>stream
BT /F1 24 Tf 20 100 Td (urPDF) Tj ET
endstream
endobj
5 0 obj<</Type/Font/Subtype/Type1/BaseFont/Helvetica>>endobj
trailer<</Root 1 0 R/Size 6>>
%%EOF
'@
    # MuPDF rebuilds the missing cross-reference table, which is enough for this file.
    [System.IO.File]::WriteAllText($samplePdf, $pdfText, [System.Text.Encoding]::ASCII)

    $savedPath = $env:PATH
    $process = $null
    try {
        # Nothing from Visual Studio, vcpkg or the Windows App SDK is reachable through PATH.
        $env:PATH = "$env:windir\System32;$env:windir"
        $process = Start-Process -FilePath $exe -ArgumentList ('"{0}"' -f $samplePdf) `
            -WorkingDirectory ([System.IO.Path]::GetTempPath()) -PassThru
    }
    finally {
        $env:PATH = $savedPath
    }

    try {
        $deadline = [DateTime]::UtcNow.AddSeconds($SmokeSeconds)
        while ([DateTime]::UtcNow -lt $deadline -and -not $process.HasExited) { Start-Sleep -Milliseconds 250 }
        if ($process.HasExited) {
            throw "urPDF.exe exited after start with code $($process.ExitCode) (0x$('{0:X8}' -f $process.ExitCode))."
        }
        $process.Refresh()
        if ($process.MainWindowHandle -eq [IntPtr]::Zero) { throw 'urPDF.exe is running but did not create a window.' }

        $mustComeFromPayload = 'Microsoft.UI.Xaml.dll', 'Microsoft.UI.Xaml.Controls.dll', 'Microsoft.WindowsAppRuntime.dll',
            'vcruntime140.dll', 'vcruntime140_1.dll', 'msvcp140.dll'
        $loaded = @($process.Modules | ForEach-Object { [pscustomobject]@{ Name = $_.ModuleName; Path = $_.FileName } })
        $windowsDir = $env:windir.TrimEnd('\') + '\'
        $payloadDir = $payloadFull.TrimEnd('\') + '\'

        $outside = @()
        # Everything that is not part of Windows must come from the payload.
        foreach ($module in $loaded) {
            if ($module.Path.StartsWith($payloadDir, [StringComparison]::OrdinalIgnoreCase)) { continue }
            if ($module.Path.StartsWith($windowsDir, [StringComparison]::OrdinalIgnoreCase)) { continue }
            $outside += "$($module.Name) loaded from $($module.Path)"
        }
        foreach ($name in $mustComeFromPayload) {
            $module = $loaded | Where-Object { $_.Name -ieq $name } | Select-Object -First 1
            if (-not $module) { $outside += "$name was not loaded" }
            elseif (-not $module.Path.StartsWith($payloadDir, [StringComparison]::OrdinalIgnoreCase)) {
                $outside += "$name loaded from $($module.Path)"
            }
        }
        if ($outside.Count -gt 0) {
            $outside | ForEach-Object { Write-Error $_ -ErrorAction Continue }
            throw 'The application is using libraries from outside the payload.'
        }
        Write-Host "Smoke test: window created; $($loaded.Count) modules loaded, all from the payload or Windows."
    }
    finally {
        if ($process -and -not $process.HasExited) {
            $null = $process.CloseMainWindow()
            if (-not $process.WaitForExit(3000)) { $process.Kill() }
        }
        Remove-Item -LiteralPath $samplePdf -ErrorAction SilentlyContinue
    }
}

$files = Get-ChildItem -LiteralPath $payloadFull -Recurse -File
$sizeMb = ($files | Measure-Object Length -Sum).Sum / 1MB
Write-Host ('Payload ready: {0} ({1} files, {2:N1} MB)' -f $payloadFull, $files.Count, $sizeMb)
