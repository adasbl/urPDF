[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string] $Executable
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$executablePath = (Resolve-Path -LiteralPath $Executable).Path
if (Get-Process -Name urPDF -ErrorAction SilentlyContinue) {
    throw 'Close urPDF before running these tests.'
}

$settingsPath = Join-Path $env:LOCALAPPDATA 'urPDF/settings.json'
$resultsDirectory = Join-Path $PSScriptRoot ('../out/window-settings-integration/' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $resultsDirectory -Force | Out-Null
$resultsDirectory = (Resolve-Path -LiteralPath $resultsDirectory).Path
$utf8 = New-Object System.Text.UTF8Encoding($false)
$passed = New-Object 'System.Collections.Generic.List[string]'
$snapshots = foreach ($path in @($settingsPath, ($settingsPath + '.bak'))) {
    $exists = Test-Path -LiteralPath $path
    $bytes = if ($exists) { ,([System.IO.File]::ReadAllBytes($path)) } else { $null }
    [pscustomobject]@{ Path = $path; Exists = $exists; Bytes = $bytes }
}

function Assert-Condition([bool] $Condition, [string] $Message) {
    if (!$Condition) { throw $Message }
}

function Invoke-SettingsCommand([string] $Command, [string] $Path, [int] $ExpectedExitCode = 0) {
    $process = Start-Process -FilePath $executablePath -ArgumentList @($Command, ('"' + $Path + '"')) -WindowStyle Hidden -PassThru
    try {
        if (!$process.WaitForExit(30000)) {
            $process.Kill()
            throw "$Command timed out."
        }
        Assert-Condition ($process.ExitCode -eq $ExpectedExitCode) "$Command returned $($process.ExitCode), expected $ExpectedExitCode. Use an unpackaged build."
    }
    finally {
        $process.Dispose()
    }
}

function Export-Settings {
    $path = Join-Path $resultsDirectory 'export.json'
    Invoke-SettingsCommand '--export-settings' $path
    return ([System.IO.File]::ReadAllText($path) | ConvertFrom-Json)
}

function Assert-DefaultWindowSettings($Settings) {
    Assert-Condition ($Settings.WindowStartupMode -eq 'RememberLastState') 'Expected the default startup preference.'
    Assert-Condition ($Settings.WindowState.maximized -eq $false) 'Expected a normal default window.'
    Assert-Condition ($null -eq $Settings.WindowState.PSObject.Properties['normalWidth']) 'Default settings must not invent a saved width.'
    Assert-Condition ($null -eq $Settings.WindowState.PSObject.Properties['normalHeight']) 'Default settings must not invent a saved height.'
}

try {
    foreach ($snapshot in $snapshots) {
        if ($snapshot.Exists) {
            [System.IO.File]::WriteAllBytes((Join-Path $resultsDirectory ([System.IO.Path]::GetFileName($snapshot.Path) + '.original')), $snapshot.Bytes)
        }
    }

    if (Test-Path -LiteralPath $settingsPath) { Remove-Item -LiteralPath $settingsPath }
    Assert-DefaultWindowSettings (Export-Settings)
    $passed.Add('First launch without settings')

    New-Item -ItemType Directory -Path (Split-Path -Parent $settingsPath) -Force | Out-Null
    [System.IO.File]::WriteAllText($settingsPath, '{"schemaVersion":1,"AppTheme":"Dark","Transparency":"Mica"}', $utf8)
    $legacy = Export-Settings
    Assert-DefaultWindowSettings $legacy
    Assert-Condition ($legacy.AppTheme -eq 'Dark' -and $legacy.Transparency -eq 'Mica') 'Legacy appearance settings were lost.'
    $passed.Add('Legacy settings retain appearance and window defaults')

    $importPath = Join-Path $resultsDirectory 'import.json'
    foreach ($mode in @('RememberLastState', 'Maximized', 'Normal')) {
        foreach ($maximized in @($false, $true)) {
            $fixture = @{
                schemaVersion = 1
                AppTheme = 'Dark'
                Transparency = 'Mica'
                WindowStartupMode = $mode
                WindowState = @{ normalWidth = 1200; normalHeight = 800; maximized = $maximized }
            } | ConvertTo-Json -Depth 4
            [System.IO.File]::WriteAllText($importPath, $fixture, $utf8)
            Invoke-SettingsCommand '--import-settings' $importPath
            Assert-Condition (Test-Path -LiteralPath $settingsPath) 'Import did not create the unpackaged settings file.'
            $reloaded = Export-Settings
            Assert-Condition ($reloaded.WindowStartupMode -eq $mode) 'Startup preference did not survive process restart.'
            Assert-Condition ($reloaded.WindowState.normalWidth -eq 1200 -and $reloaded.WindowState.normalHeight -eq 800) 'Window size did not survive process restart.'
            Assert-Condition ($reloaded.WindowState.maximized -eq $maximized) 'Maximized state did not survive process restart.'
            $passed.Add("Persistence: $mode, maximized=$maximized")
        }
    }

    $beforeInvalidImport = [System.IO.File]::ReadAllText($settingsPath)
    [System.IO.File]::WriteAllText($importPath, '{"schemaVersion":1,"WindowState":{"normalWidth":0,"normalHeight":800,"maximized":false}}', $utf8)
    Invoke-SettingsCommand '--import-settings' $importPath 1
    Assert-Condition ([System.IO.File]::ReadAllText($settingsPath) -ceq $beforeInvalidImport) 'Invalid import changed the settings file.'
    $passed.Add('Invalid import leaves persisted settings unchanged')

    [System.IO.File]::WriteAllText($importPath, '{"schemaVersion":1,"AppTheme":"Light"}', $utf8)
    Invoke-SettingsCommand '--import-settings' $importPath
    $partial = Export-Settings
    Assert-Condition ($partial.AppTheme -eq 'Light' -and $partial.WindowStartupMode -eq 'Normal') 'Partial import changed the startup preference.'
    Assert-Condition ($partial.WindowState.normalWidth -eq 1200 -and $partial.WindowState.normalHeight -eq 800 -and $partial.WindowState.maximized) 'Partial import changed the saved window state.'
    $passed.Add('Partial legacy import preserves saved window settings')

    [System.IO.File]::WriteAllText($settingsPath, 'invalid json', $utf8)
    Assert-DefaultWindowSettings (Export-Settings)
    Assert-Condition ([System.IO.File]::ReadAllText($settingsPath + '.bak') -ceq 'invalid json') 'Malformed settings were not backed up.'
    $passed.Add('Malformed settings file falls back to defaults and is backed up')

    $invalidStoredSettings = @{
        schemaVersion = 1
        AppTheme = 'Dark'
        WindowStartupMode = 'Minimized'
        WindowState = '{"normalWidth":-1,"normalHeight":800,"maximized":true}'
    } | ConvertTo-Json
    [System.IO.File]::WriteAllText($settingsPath, $invalidStoredSettings, $utf8)
    $invalidStored = Export-Settings
    Assert-DefaultWindowSettings $invalidStored
    Assert-Condition ($invalidStored.AppTheme -eq 'Dark') 'Invalid window data changed valid appearance settings.'
    $passed.Add('Invalid stored window values fall back to defaults')

    $passed | Set-Content -LiteralPath (Join-Path $resultsDirectory 'passed.txt')
    Write-Output "Passed $($passed.Count) integration checks. Results: $resultsDirectory"
}
finally {
    foreach ($snapshot in $snapshots) {
        if ($snapshot.Exists) {
            [System.IO.File]::WriteAllBytes($snapshot.Path, $snapshot.Bytes)
        }
        elseif (Test-Path -LiteralPath $snapshot.Path) {
            Remove-Item -LiteralPath $snapshot.Path
        }
    }
}
