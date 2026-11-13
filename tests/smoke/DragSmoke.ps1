# Drags tabs with the real mouse between urPDF windows (MSI variant).
# Takes over the mouse and keyboard; do not touch them while the script runs.
param([string] $Executable = "$PSScriptRoot/../../out/msi/x64/Debug/bin/urPDF.exe")
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot/SmokeLib.ps1"
Assert-NoRunningInstance

Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class DragNative {
    [DllImport("user32.dll")] public static extern IntPtr SetThreadDpiAwarenessContext(IntPtr context);
    [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
    [DllImport("user32.dll")] public static extern int GetSystemMetrics(int index);
    [DllImport("user32.dll")] public static extern uint GetDpiForWindow(IntPtr hwnd);
    [DllImport("user32.dll")] public static extern void mouse_event(uint flags, int dx, int dy, uint data, UIntPtr extra);
    [DllImport("user32.dll")] public static extern void keybd_event(byte key, byte scan, uint flags, UIntPtr extra);
    [DllImport("user32.dll")] public static extern bool MoveWindow(IntPtr hwnd, int x, int y, int width, int height, bool repaint);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr hwnd);
}
'@
[DragNative]::SetThreadDpiAwarenessContext([IntPtr](-4)) | Out-Null

function Get-TabElements([IntPtr] $Handle) {
    $root = [System.Windows.Automation.AutomationElement]::FromHandle($Handle)
    $condition = New-Object System.Windows.Automation.PropertyCondition(
        [System.Windows.Automation.AutomationElement]::ControlTypeProperty, [System.Windows.Automation.ControlType]::TabItem)
    @($root.FindAll([System.Windows.Automation.TreeScope]::Descendants, $condition))
}

function Find-Tab([IntPtr] $Handle, [string] $Name, [int] $Occurrence = 0) {
    $matching = @(Get-TabElements $Handle | Where-Object { $_.Current.Name -eq $Name })
    if ($matching.Count -le $Occurrence) { throw "Tab '$Name' not found." }
    return $matching[$Occurrence]
}

function Center($Element) {
    $r = $Element.Current.BoundingRectangle
    @([int]($r.X + [Math]::Min(40, $r.Width / 2)), [int]($r.Y + $r.Height / 2))
}

function Drag([int[]] $From, [int[]] $To, [switch] $Escape, [scriptblock] $WhileHeld) {
    [DragNative]::SetCursorPos($From[0], $From[1]) | Out-Null
    Start-Sleep -Milliseconds 150
    [DragNative]::mouse_event(0x2, 0, 0, 0, [UIntPtr]::Zero)
    Start-Sleep -Milliseconds 150
    $steps = 30
    for ($i = 1; $i -le $steps; ++$i) {
        $x = [int]($From[0] + ($To[0] - $From[0]) * $i / $steps)
        $y = [int]($From[1] + ($To[1] - $From[1]) * $i / $steps)
        # Inject actual mouse movement: SetCursorPos alone doesn't exercise XAML dragging.
        $absoluteX = [int](($x - [DragNative]::GetSystemMetrics(76)) * 65535 / ([DragNative]::GetSystemMetrics(78) - 1))
        $absoluteY = [int](($y - [DragNative]::GetSystemMetrics(77)) * 65535 / ([DragNative]::GetSystemMetrics(79) - 1))
        [DragNative]::mouse_event(0xC001, $absoluteX, $absoluteY, 0, [UIntPtr]::Zero)
        Start-Sleep -Milliseconds 25
    }
    Start-Sleep -Milliseconds 400
    if ($WhileHeld) { & $WhileHeld }
    if ($Escape) {
        [DragNative]::keybd_event(0x1B, 0, 0, [UIntPtr]::Zero)
        [DragNative]::keybd_event(0x1B, 0, 2, [UIntPtr]::Zero)
        Start-Sleep -Milliseconds 300
    }
    [DragNative]::mouse_event(0x4, 0, 0, 0, [UIntPtr]::Zero)
    Start-Sleep -Milliseconds 1200
}

function Describe { (Get-AppWindows | ForEach-Object { '[' + ((Get-TabNames $_) -join ', ') + ']' }) -join ' ' }

function Fresh {
    if ($script:main -and !$script:main.HasExited) { Stop-Main | Out-Null }
    Start-Main @(Quote $pdfA)
    Wait-Until { (Get-AllTabNames) -contains 'Alpha.pdf' } | Out-Null
    Start-Secondary @(Quote $pdfC) | Out-Null
    Wait-Until { (Get-AllTabNames) -contains 'Gamma.pdf' } | Out-Null
    $script:w1 = @(Get-AppWindows)[0]
    [DragNative]::MoveWindow($script:w1, 80, 80, 1200, 650, $true) | Out-Null
    [DragNative]::SetForegroundWindow($script:w1) | Out-Null
    Start-Sleep -Seconds 1
}

function TearOut([IntPtr] $Window, [string] $Name, [switch] $Escape) {
    $before = @(Get-AppWindows)
    $point = Center (Find-Tab $Window $Name)
    Drag $point @(($point[0] + 250), ($point[1] + 420)) -Escape:$Escape -WhileHeld {
        Record 'Dragging outside keeps the tab in its source until release' `
            ((Get-AppWindows).Count -eq $before.Count -and @(Get-TabNames $Window) -contains $Name) (Describe)
    }
    @(Get-AppWindows | Where-Object { $before -notcontains $_ })
}

function DropInto([IntPtr] $Source, [string] $Name, [IntPtr] $Target) {
    # Keep both source tabs visible, with room for the system caption buttons.
    [DragNative]::MoveWindow($Source, 400, 560, 1200, 400, $true) | Out-Null
    Start-Sleep -Milliseconds 700
    $last = @(Get-TabElements $Target)[-1].Current.BoundingRectangle
    Drag (Center (Find-Tab $Source $Name)) @([int]($last.X + $last.Width + 15), [int]($last.Y + $last.Height / 2)) -WhileHeld {
        Record 'Hovering over another strip keeps the tab in its source until release' `
            (@(Get-TabNames $Source) -contains $Name) (Describe)
    }
}

# Drops onto the left edge of the target's tab at $Index, so the tab lands before it.
function DropBefore([IntPtr] $Source, [string] $Name, [IntPtr] $Target, [int] $Index) {
    [DragNative]::MoveWindow($Source, 400, 560, 1200, 400, $true) | Out-Null
    Start-Sleep -Milliseconds 700
    $tab = @(Get-TabElements $Target)[$Index].Current.BoundingRectangle
    Drag (Center (Find-Tab $Source $Name)) @([int]($tab.X + 8), [int]($tab.Y + $tab.Height / 2))
}

function AddEmptyTab([IntPtr] $Window) {
    [DragNative]::SetForegroundWindow($Window) | Out-Null
    Start-Sleep -Milliseconds 200
    $plus = [System.Windows.Automation.AutomationElement]::FromHandle($Window).FindFirst([System.Windows.Automation.TreeScope]::Descendants,
        (New-Object System.Windows.Automation.PropertyCondition([System.Windows.Automation.AutomationElement]::ControlTypeProperty, [System.Windows.Automation.ControlType]::SplitButton)))
    $plus.GetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern).Invoke()
    Start-Sleep -Seconds 1
}

function RecordTabWidths([IntPtr] $Window, [string] $Name, [int] $ExpectedCount) {
    $scale = [DragNative]::GetDpiForWindow($Window) / 96.0
    $widths = @(Get-TabElements $Window | ForEach-Object {
        $width = $_.Current.BoundingRectangle.Width
        $selection = $_.GetCurrentPattern([System.Windows.Automation.SelectionItemPattern]::Pattern)
        # UIA includes WinUI's selected-tab corner flares, 4 DIPs on each side.
        if ($selection.Current.IsSelected) { $width -= 8.0 * $scale }
        $width
    })
    $expected = 240.0 * $scale
    $wrong = @($widths | Where-Object { [double]::IsNaN($_) -or [double]::IsInfinity($_) -or [Math]::Abs($_ - $expected) -gt 1.0 })
    Record $Name ($widths.Count -eq $ExpectedCount -and $wrong.Count -eq 0) "expected=$expected px; actual=$($widths -join ', ')"
}

try {
    Set-OpenInExisting $true

    Fresh
    [DragNative]::MoveWindow($w1, 80, 80, 2000, 650, $true) | Out-Null
    $longName = 'A very long PDF document filename that should be truncated instead of widening the tab.pdf'
    $longPdf = Join-Path (Split-Path $pdfA) $longName
    New-PdfFixture $longPdf
    Start-Secondary @(Quote $longPdf) | Out-Null
    Wait-Until { (Get-AllTabNames) -contains $longName } | Out-Null
    AddEmptyTab $w1
    RecordTabWidths $w1 'Short, long and empty tabs have the same fixed width' 4
    (Find-Tab $w1 $longName).GetCurrentPattern([System.Windows.Automation.SelectionItemPattern]::Pattern).Select()
    Start-Sleep -Milliseconds 500
    RecordTabWidths $w1 'Selecting a long filename does not change tab widths' 4

    Fresh
    $alpha = Center (Find-Tab $w1 'Alpha.pdf')
    $gamma = (Find-Tab $w1 'Gamma.pdf').Current.BoundingRectangle
    Drag $alpha @([int]($gamma.X + $gamma.Width - 10), $alpha[1])
    $order = @(Get-TabNames $w1)
    Record 'Reorder PDF tabs inside the strip' ($order[0] -eq 'Gamma.pdf' -and $order[1] -eq 'Alpha.pdf') ($order -join ', ')

    Fresh
    $new = TearOut $w1 'Gamma.pdf'
    $ok = $new.Count -eq 1 -and (@(Get-TabNames $new[0]) -join ',') -eq 'Gamma.pdf' -and (@(Get-TabNames $w1) -join ',') -eq 'Alpha.pdf'
    Record 'Tear out PDF tab into a new window (one tab, no duplicates)' $ok (Describe)
    if ($new.Count -eq 1) {
        RecordTabWidths $new[0] 'Torn-out tab retains its fixed width' 1
        DropInto $new[0] 'Gamma.pdf' $w1
        $ok = (Wait-Until { (Get-AppWindows).Count -eq 1 } 5) -and (@(Get-TabNames $w1) -join ',') -eq 'Alpha.pdf,Gamma.pdf'
        Record 'Move last tab into existing window at drop index; empty source closes' $ok (Describe)
    }

    Fresh
    $new = TearOut $w1 'Gamma.pdf' -Escape
    Record 'Esc cancels tear-out without creating a window or moving the tab' `
        ($new.Count -eq 0 -and (Get-AppWindows).Count -eq 1 -and ((@(Get-TabNames $w1) | Sort-Object) -join ',') -eq 'Alpha.pdf,Gamma.pdf') (Describe)

    Fresh
    AddEmptyTab $w1
    $new = TearOut $w1 'New tab'
    $ok = $new.Count -eq 1 -and (@(Get-TabNames $new[0]) -join ',') -eq 'New tab' -and @(Get-TabNames $w1) -notcontains 'New tab'
    Record 'Tear out empty tab: new window has exactly that one tab' $ok (Describe)
    if ($new.Count -eq 1) {
        $w2 = $new[0]
        [DragNative]::MoveWindow($w2, 400, 560, 1200, 400, $true) | Out-Null
        Start-Sleep -Milliseconds 700
        $last = @(Get-TabElements $w2)[-1].Current.BoundingRectangle
        # Keep the target strip visible when the source is activated by the drag.
        [DragNative]::MoveWindow($w1, 80, 60, 1200, 380, $true) | Out-Null
        Drag (Center (Find-Tab $w1 'Alpha.pdf')) @([int]($last.X + $last.Width - 8), [int]($last.Y + $last.Height / 2))
        $tabs2 = @(Get-TabNames $w2)
        Record 'PDF dropped into window holding a single empty tab keeps that empty tab' (($tabs2 -join ',') -eq 'New tab,Alpha.pdf') (Describe)

        $script:main.Refresh(); Record 'Process alive after PDF transfer' (!$script:main.HasExited) "windows=$((Get-AppWindows).Count)"
        AddEmptyTab $w1
        $script:main.Refresh(); Record 'Empty tab added in source' (!$script:main.HasExited -and @(Get-TabNames $w1) -contains 'New tab') (Describe)
        [DragNative]::MoveWindow($w2, 80, 60, 2000, 380, $true) | Out-Null
        try { DropInto $w1 'New tab' $w2 } catch { Record 'Empty-tab drop step' $false $_.Exception.Message }
        $script:main.Refresh(); Record 'Process alive after empty-tab transfer' (!$script:main.HasExited) "exit=$(if ($script:main.HasExited) { $script:main.ExitCode })"
        $tabs2 = @(Get-TabNames $w2)
        Record 'Empty tab moved into existing window (no duplicates, source keeps PDF)' ((@($tabs2) -join ',') -eq 'New tab,Alpha.pdf,New tab' -and (@(Get-TabNames $w1) -join ',') -eq 'Gamma.pdf') (Describe)
    }

    # A new window's startup tab and several empty tabs survive incoming PDFs.
    Stop-Main | Out-Null
    Set-OpenInExisting $false
    Start-Main @(Quote $pdfA)
    Wait-Until { (Get-AllTabNames) -contains 'Alpha.pdf' } | Out-Null
    $w1 = @(Get-AppWindows)[0]
    Start-Secondary @(Quote $pdfC) | Out-Null
    Wait-Until { (Get-AllTabNames) -contains 'Gamma.pdf' } | Out-Null
    $w2 = @(Get-AppWindows | Where-Object { $_ -ne $w1 })[0]
    Start-Secondary @() | Out-Null
    Wait-Until { (Get-AppWindows).Count -eq 3 } | Out-Null
    $w3 = @(Get-AppWindows | Where-Object { $_ -ne $w1 -and $_ -ne $w2 })[0]
    [DragNative]::MoveWindow($w3, 80, 60, 2000, 380, $true) | Out-Null
    [DragNative]::MoveWindow($w2, 1100, 560, 1200, 400, $true) | Out-Null
    DropInto $w1 'Alpha.pdf' $w3
    Record 'PDF dropped into a new window keeps its startup tab' ((@(Get-TabNames $w3) -join ',') -eq 'New tab,Alpha.pdf') (Describe)
    AddEmptyTab $w3
    DropBefore $w2 'Gamma.pdf' $w3 1
    $ok = (@(Get-TabNames $w3) -join ',') -eq 'New tab,Gamma.pdf,Alpha.pdf,New tab' -and (Wait-Until { (Get-AppWindows).Count -eq 1 } 5)
    Record 'PDF dropped between tabs lands at the drop index; all empty tabs stay' $ok (Describe)

    $exited = Stop-Main
    Record 'Process exits cleanly after drag scenarios' ($exited -and $script:main.ExitCode -eq 0) "exit $($script:main.ExitCode)"
}
finally {
    [DragNative]::mouse_event(0x4, 0, 0, 0, [UIntPtr]::Zero)
    if ($script:main -and !$script:main.HasExited) { $script:main.Kill() }
    Restore-Settings
    Write-Results
}
if (@($results | Where-Object Result -eq 'FAIL').Count) { exit 1 }
