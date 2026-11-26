# Starts urPDF the way File Explorer does (MSI variant) and checks where documents open.
param([string] $Executable = "$PSScriptRoot/../../out/msi/x64/Debug/bin/urPDF.exe")
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot/SmokeLib.ps1"
Assert-NoRunningInstance

try {
    # Command-line settings commands.
    $exportFile = Join-Path $fixtures 'export.json'
    $process = Start-Process -FilePath $Executable -ArgumentList '--export-settings', (Quote $exportFile) -PassThru -Wait
    $exported = (Get-Content -LiteralPath $exportFile -Raw | ConvertFrom-Json)
    Record '--export-settings writes JSON' ($process.ExitCode -eq 0 -and $null -ne $exported.OpenFilesInExistingWindow -and $null -ne $exported.AskToSetAsDefaultPdfViewer) "exit $($process.ExitCode)"
    $invalid = Join-Path $fixtures 'invalid.json'
    [IO.File]::WriteAllText($invalid, '{"schemaVersion":1,"OpenFilesInExistingWindow":"maybe"}')
    $process = Start-Process -FilePath $Executable -ArgumentList '--import-settings', (Quote $invalid) -PassThru -Wait
    Record '--import-settings rejects invalid value' ($process.ExitCode -ne 0) "exit $($process.ExitCode)"

    Set-AskToSetAsDefaultPdfViewer $false

    # Debug builds simulate a MuPDF initialization failure when this variable is set.
    $env:URPDF_SIMULATE_MUPDF_INIT_FAILURE = '1'
    try { $failed = Start-Process -FilePath $Executable -PassThru }
    finally { Remove-Item Env:URPDF_SIMULATE_MUPDF_INIT_FAILURE }
    $shown = Wait-Until { $script:dialogs = @([SmokeNative]::Windows([uint32]$failed.Id, '#32770')); $script:dialogs.Count -ge 1 }
    foreach ($dialog in $script:dialogs) { [SmokeNative]::PostMessage($dialog, 0x10, [UIntPtr]::Zero, [IntPtr]::Zero) | Out-Null }
    $exited = $failed.WaitForExit(10000)
    if (!$exited) { $failed.Kill() }
    Record 'MuPDF initialization failure shows a message and exits with an error' ($shown -and $exited -and $failed.ExitCode -ne 0) "dialog=$shown; exit=$(if ($exited) { $failed.ExitCode })"
    Start-Main @(Quote $pdfA)
    Record 'Launch after a failed initialization becomes the main instance' (Wait-Until { (Get-AllTabNames) -contains 'Alpha.pdf' }) ((Get-AllTabNames) -join ', ')
    Stop-Main | Out-Null

    # Setting disabled: every activation gets its own window in the same process.
    Set-OpenInExisting $false
    Start-Main @(Quote $pdfA)
    Record 'Off: first launch opens file' (Wait-Until { (Get-AllTabNames) -contains 'Alpha.pdf' }) ((Get-AllTabNames) -join ', ')
    $exit = Start-Secondary @(Quote $pdfB)
    $ok = Wait-Until { (Get-AppWindows).Count -eq 2 -and (Get-AllTabNames) -contains (Split-Path $pdfB -Leaf) }
    Record 'Off: second file redirected into new window of same process' ($ok -and $exit -eq 0) "windows=$((Get-AppWindows).Count); tabs=$((Get-AllTabNames) -join ', ')"
    $exit = Start-Secondary @()
    $ok = Wait-Until { (Get-AppWindows).Count -eq 3 }
    Record 'Off: launch without file opens new empty window' $ok "windows=$((Get-AppWindows).Count); tabs=$((Get-AllTabNames) -join ', ')"
    $first = (Get-AppWindows)[0]
    [SmokeNative]::PostMessage($first, 0x10, [UIntPtr]::Zero, [IntPtr]::Zero) | Out-Null
    $ok = Wait-Until { (Get-AppWindows).Count -eq 2 }
    Start-Sleep -Milliseconds 500
    $script:main.Refresh()
    Record 'Closing one window keeps the others and the process' ($ok -and !$script:main.HasExited) "windows=$((Get-AppWindows).Count)"
    $exited = Stop-Main
    Record 'Closing last window ends the process cleanly' ($exited -and $script:main.ExitCode -eq 0) "exit $($script:main.ExitCode)"

    # Setting enabled: activations go to the last active window.
    Set-OpenInExisting $true
    Start-Main @(Quote $pdfA)
    Wait-Until { (Get-AllTabNames) -contains 'Alpha.pdf' } | Out-Null
    $exit = Start-Secondary @(Quote $pdfB)
    $ok = Wait-Until { (Get-AllTabNames) -contains (Split-Path $pdfB -Leaf) }
    Record 'On: file with spaces and Polish characters opens as tab in existing window' ($ok -and (Get-AppWindows).Count -eq 1) "windows=$((Get-AppWindows).Count); tabs=$((Get-AllTabNames) -join ', ')"
    New-PdfFixture (Join-Path $fixtures 'Relative.pdf')
    $relative = Start-Process -FilePath $Executable -ArgumentList 'Relative.pdf' -WorkingDirectory $fixtures -PassThru
    $ok = $relative.WaitForExit(15000) -and (Wait-Until { (Get-AllTabNames) -contains 'Relative.pdf' })
    Record 'On: relative path is resolved and redirected to the main instance' ($ok -and (Get-AppWindows).Count -eq 1) "tabs=$((Get-AllTabNames) -join ', ')"
    $exit = Start-Secondary @()
    Start-Sleep -Seconds 2
    Record 'On: launch without file does not open another window' ((Get-AppWindows).Count -eq 1) "windows=$((Get-AppWindows).Count)"

    $jobs = @(
        (Start-Process -FilePath $Executable -ArgumentList (Quote $pdfC) -PassThru),
        (Start-Process -FilePath $Executable -ArgumentList (Quote $pdfD) -PassThru))
    foreach ($job in $jobs) { $job.WaitForExit(15000) | Out-Null }
    $ok = Wait-Until { $names = Get-AllTabNames; ($names -contains 'Gamma.pdf') -and ($names -contains 'Delta.pdf') }
    $names = Get-AllTabNames
    Record 'On: simultaneous activations (busy window) are all opened' ($ok -and (Get-AppWindows).Count -eq 1 -and $names.Count -eq 5) "tabs=$($names -join ', ')"

    $window = (Get-AppWindows)[0]
    [SmokeNative]::ShowWindow($window, 6) | Out-Null
    Start-Sleep -Milliseconds 500
    $exit = Start-Secondary @(Quote $pdfE)
    $ok = Wait-Until { (Get-AllTabNames) -contains 'Epsilon.pdf' }
    Start-Sleep -Milliseconds 500
    Record 'On: minimized window is restored and receives the file' ($ok -and ![SmokeNative]::IsIconic($window)) "iconic=$([SmokeNative]::IsIconic($window)); tabs=$((Get-AllTabNames) -join ', ')"

    $exited = Stop-Main
    Record 'On: closing the window ends the process cleanly' ($exited -and $script:main.ExitCode -eq 0) "exit $($script:main.ExitCode)"

    # No running window: the activation starts a new main instance.
    Start-Main @(Quote $pdfC)
    Record 'On: no existing window creates one with the file' (Wait-Until { (Get-AllTabNames) -contains 'Gamma.pdf' }) ((Get-AllTabNames) -join ', ')
    $exited = Stop-Main
}
finally {
    if ($script:main -and !$script:main.HasExited) { $script:main.Kill() }
    Restore-Settings
    Write-Results
}
