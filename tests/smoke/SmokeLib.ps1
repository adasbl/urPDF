# Shared helpers for the interactive smoke tests. Dot-source after setting $Executable.
$Executable = (Resolve-Path $Executable).Path
Add-Type -AssemblyName UIAutomationClient, UIAutomationTypes
Add-Type @'
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text;
public static class SmokeNative {
    public delegate bool EnumProc(IntPtr hwnd, IntPtr lparam);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc proc, IntPtr lparam);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr hwnd, out uint pid);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr hwnd);
    [DllImport("user32.dll")] public static extern bool IsIconic(IntPtr hwnd);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetClassName(IntPtr hwnd, StringBuilder name, int count);
    [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr hwnd, uint message, UIntPtr wparam, IntPtr lparam);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr hwnd, int command);
    public static List<IntPtr> Windows(uint processId, string className) {
        var result = new List<IntPtr>();
        EnumWindows((hwnd, _) => {
            uint pid;
            GetWindowThreadProcessId(hwnd, out pid);
            var name = new StringBuilder(256);
            GetClassName(hwnd, name, name.Capacity);
            if (pid == processId && IsWindowVisible(hwnd) && name.ToString() == className) { result.Add(hwnd); }
            return true;
        }, IntPtr.Zero);
        return result;
    }
}
'@

$results = [System.Collections.Generic.List[object]]::new()
function Record($Name, [bool] $Passed, $Detail = '') {
    $results.Add([pscustomobject]@{ Test = $Name; Result = $(if ($Passed) { 'PASS' } else { 'FAIL' }); Detail = $Detail })
}

function Wait-Until([scriptblock] $Condition, [int] $Seconds = 15) {
    $deadline = [DateTime]::UtcNow.AddSeconds($Seconds)
    do {
        if (& $Condition) { return $true }
        Start-Sleep -Milliseconds 200
    } while ([DateTime]::UtcNow -lt $deadline)
    return $false
}

function Get-AppWindows { @([SmokeNative]::Windows([uint32]$script:main.Id, 'WinUIDesktopWin32WindowClass')) }

function Get-TabNames([IntPtr] $Handle) {
    $root = [System.Windows.Automation.AutomationElement]::FromHandle($Handle)
    $condition = New-Object System.Windows.Automation.PropertyCondition(
        [System.Windows.Automation.AutomationElement]::ControlTypeProperty, [System.Windows.Automation.ControlType]::TabItem)
    @($root.FindAll([System.Windows.Automation.TreeScope]::Descendants, $condition) | ForEach-Object { $_.Current.Name })
}

function Get-AllTabNames { @(Get-AppWindows | ForEach-Object { Get-TabNames $_ }) }

function Start-Secondary([string[]] $Arguments) {
    $process = if ($Arguments) { Start-Process -FilePath $Executable -ArgumentList $Arguments -PassThru } else { Start-Process -FilePath $Executable -PassThru }
    if (!$process.WaitForExit(15000)) { $process.Kill(); throw 'Secondary instance did not exit (activation was not redirected).' }
    return $process.ExitCode
}

function Start-Main([string[]] $Arguments, [string] $WorkingDirectory) {
    $options = @{ FilePath = $Executable; PassThru = $true }
    if ($Arguments) { $options.ArgumentList = $Arguments }
    if ($WorkingDirectory) { $options.WorkingDirectory = $WorkingDirectory }
    $script:main = Start-Process @options
    if (!(Wait-Until { $script:main.Refresh(); $script:main.HasExited -or (Get-AppWindows).Count -ge 1 })) { throw 'No main window appeared.' }
    if ($script:main.HasExited) { throw "Main instance exited immediately ($($script:main.ExitCode)); another urPDF instance may own the activation key." }
}

function Stop-Main {
    foreach ($window in Get-AppWindows) { [SmokeNative]::PostMessage($window, 0x10, [UIntPtr]::Zero, [IntPtr]::Zero) | Out-Null }
    $exited = $script:main.WaitForExit(15000)
    if (!$exited) { $script:main.Kill(); $script:main.WaitForExit(5000) | Out-Null }
    return $exited
}

function Quote([string] $Path) { '"' + $Path + '"' }

function New-PdfFixture([string] $Path) {
    $objects = @(
        '<< /Type /Catalog /Pages 2 0 R >>',
        '<< /Type /Pages /Kids [3 0 R] /Count 1 >>',
        '<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] >>')
    $builder = [System.Text.StringBuilder]::new("%PDF-1.4`n")
    $offsets = @()
    for ($i = 0; $i -lt $objects.Count; ++$i) {
        $offsets += $builder.Length
        [void]$builder.Append("$($i + 1) 0 obj`n$($objects[$i])`nendobj`n")
    }
    $xref = $builder.Length
    [void]$builder.Append("xref`n0 $($objects.Count + 1)`n0000000000 65535 f `n")
    foreach ($offset in $offsets) { [void]$builder.Append(('{0:D10} 00000 n `n' -f $offset)) }
    [void]$builder.Append("trailer`n<< /Size $($objects.Count + 1) /Root 1 0 R >>`nstartxref`n$xref`n%%EOF`n")
    [IO.File]::WriteAllText($Path, $builder.ToString(), [Text.Encoding]::ASCII)
}

$fixtures = Join-Path $env:TEMP "urPDF smoke ąęłóż"
New-Item -ItemType Directory -Force -Path $fixtures | Out-Null
$pdfA = Join-Path $fixtures 'Alpha.pdf'
$pdfB = Join-Path $fixtures 'Dokument źdźbło ąę 2.pdf'
$pdfC = Join-Path $fixtures 'Gamma.pdf'
$pdfD = Join-Path $fixtures 'Delta.pdf'
$pdfE = Join-Path $fixtures 'Epsilon.pdf'
foreach ($pdf in $pdfA, $pdfB, $pdfC, $pdfD, $pdfE) { New-PdfFixture $pdf }
$script:main = $null

$settingsPath = Join-Path $env:LOCALAPPDATA 'urPDF/settings.json'
$settingsSnapshots = foreach ($path in @($settingsPath, ($settingsPath + '.bak'))) {
    $exists = Test-Path -LiteralPath $path
    [pscustomobject]@{ Path = $path; Exists = $exists; Bytes = $(if ($exists) { ,([IO.File]::ReadAllBytes($path)) } else { $null }) }
}

function Restore-Settings {
    foreach ($snapshot in $settingsSnapshots) {
        if ($snapshot.Exists) { [IO.File]::WriteAllBytes($snapshot.Path, $snapshot.Bytes) }
        elseif (Test-Path -LiteralPath $snapshot.Path) { Remove-Item -LiteralPath $snapshot.Path }
    }
}

function Set-OpenInExisting([bool] $Enabled) {
    $file = Join-Path $fixtures 'settings.json'
    $value = if ($Enabled) { 'true' } else { 'false' }
    [IO.File]::WriteAllText($file, "{`"schemaVersion`":1,`"OpenFilesInExistingWindow`":$value}")
    $process = Start-Process -FilePath $Executable -ArgumentList '--import-settings', (Quote $file) -PassThru -Wait
    if ($process.ExitCode -ne 0) { throw "Settings import failed: $($process.ExitCode)" }
}

function Set-AskToSetAsDefaultPdfViewer([bool] $Enabled) {
    $file = Join-Path $fixtures 'default-viewer-prompt.json'
    $value = if ($Enabled) { 'true' } else { 'false' }
    [IO.File]::WriteAllText($file, "{`"schemaVersion`":1,`"AskToSetAsDefaultPdfViewer`":$value}")
    $process = Start-Process -FilePath $Executable -ArgumentList '--import-settings', (Quote $file) -PassThru -Wait
    if ($process.ExitCode -ne 0) { throw "Settings import failed: $($process.ExitCode)" }
}

function Assert-NoRunningInstance {
    if (Get-Process urPDF -ErrorAction SilentlyContinue) { throw 'Close all running urPDF instances first; a running instance owns the activation key.' }
}

function Write-Results {
    $results | Format-Table -AutoSize -Wrap | Out-String -Width 220
    $failed = @($results | Where-Object Result -eq 'FAIL').Count
    "$($results.Count - $failed)/$($results.Count) passed"
}
