param(
    [string]$Executable = "build/win-clang-coherent/bin/subuwutuner-gui.exe",
    [string]$Project = "fixtures/demo.stune",
    [string]$OutputDirectory = ".cache/ui-smoke",
    [ValidateSet("Demo", "Welcome", "Designer", "EditRoundTrip", "Readiness")]
    [string]$Scenario = "Demo",
    [int]$WindowWidth = 1400,
    [int]$WindowHeight = 880,
    [switch]$KeepOpen
)

$ErrorActionPreference = "Stop"

# UI interaction shares the physical desktop and pointer. Serialize passes so
# concurrent shells cannot click or capture one another's SubuwuTuner window.
$smokeMutex = [System.Threading.Mutex]::new(
    $false, "Local\SubuwuTunerGuiVisualSmoke")
$mutexHeld = $smokeMutex.WaitOne([TimeSpan]::FromSeconds(30))
if (-not $mutexHeld) {
    $smokeMutex.Dispose()
    throw "Timed out waiting for another GUI visual-smoke pass to finish."
}

$exePath = (Resolve-Path -LiteralPath $Executable).Path
$outputPath = [System.IO.Path]::GetFullPath(
    (Join-Path (Get-Location) $OutputDirectory))
[System.IO.Directory]::CreateDirectory($outputPath) | Out-Null

# EditRoundTrip mutates a project on disk (edit + save + reopen). Never point
# it at the checked-in fixture; copy the project into the output directory and
# drive the isolated copy so the round trip is repeatable and non-destructive.
$projectPath = if ($Scenario -eq "EditRoundTrip") {
    $sourceProject = (Resolve-Path -LiteralPath $Project).Path
    $isolatedProject = Join-Path $outputPath "roundtrip.stune"
    if (Test-Path -LiteralPath $isolatedProject) {
        Remove-Item -LiteralPath $isolatedProject -Recurse -Force
    }
    Copy-Item -LiteralPath $sourceProject -Destination $isolatedProject -Recurse
    # The project references its definition pack by a path relative to the
    # .stune directory (demo.stune/project.toml -> "../demo-pack"), so that
    # sibling pack must be copied alongside the isolated project or the open
    # fails with "file not found: <output>/demo-pack". Read the relative
    # definition path out of project.toml and mirror exactly that directory
    # (never the whole fixtures tree, which includes large private dumps).
    $projectToml = Get-Content -LiteralPath (Join-Path $isolatedProject "project.toml") -Raw
    $defMatch = [regex]::Match($projectToml, '(?ms)\[project\.definition\].*?path\s*=\s*"([^"]+)"')
    if ($defMatch.Success) {
        $relDef = $defMatch.Groups[1].Value
        $sourceDef = [System.IO.Path]::GetFullPath(
            (Join-Path $sourceProject $relDef))
        $destDef = [System.IO.Path]::GetFullPath(
            (Join-Path $isolatedProject $relDef))
        if ((Test-Path -LiteralPath $sourceDef) -and
            ($destDef -ne $sourceDef) -and -not (Test-Path -LiteralPath $destDef)) {
            $destDefParent = Split-Path -Parent $destDef
            [System.IO.Directory]::CreateDirectory($destDefParent) | Out-Null
            Copy-Item -LiteralPath $sourceDef -Destination $destDef -Recurse
        }
    }
    $isolatedProject
} elseif ($Scenario -in @("Demo", "Designer", "Readiness")) {
    (Resolve-Path -LiteralPath $Project).Path
} else { "" }

Add-Type -AssemblyName System.Drawing
Add-Type @'
using System;
using System.Runtime.InteropServices;

public static class SubuwuVisualSmokeNative {
    [StructLayout(LayoutKind.Sequential)]
    public struct Rect { public int Left, Top, Right, Bottom; }

    [DllImport("user32.dll")]
    public static extern bool GetWindowRect(IntPtr window, out Rect rect);

    [DllImport("user32.dll")]
    public static extern bool SetWindowPos(
        IntPtr window, IntPtr insertAfter, int x, int y, int width, int height,
        uint flags);

    [DllImport("user32.dll")]
    public static extern bool SetCursorPos(int x, int y);

    [DllImport("user32.dll")]
    public static extern void mouse_event(
        uint flags, uint x, uint y, uint data, UIntPtr extraInfo);

    [DllImport("user32.dll")]
    public static extern void keybd_event(
        byte virtualKey, byte scanCode, uint flags, UIntPtr extraInfo);
}
'@

function Get-TargetRect([System.Diagnostics.Process]$Process) {
    $Process.Refresh()
    $rect = New-Object SubuwuVisualSmokeNative+Rect
    if ($Process.MainWindowHandle -eq 0 -or
        -not [SubuwuVisualSmokeNative]::GetWindowRect(
            $Process.MainWindowHandle, [ref]$rect)) {
        throw "SubuwuTuner did not expose a capturable main window."
    }
    return $rect
}

function Save-WindowScreenshot(
    [System.Diagnostics.Process]$Process,
    [string]$Name
) {
    $rect = Get-TargetRect $Process
    $width = $rect.Right - $rect.Left
    $height = $rect.Bottom - $rect.Top
    if ($width -lt 640 -or $height -lt 400) {
        throw "Unexpected GUI window size: ${width}x${height}."
    }

    # OpenGL windows commonly produce black frames through PrintWindow.
    # Briefly place this exact HWND above other windows and copy its screen
    # rectangle instead. Remove topmost immediately after capture.
    $topmost = [IntPtr](-1)
    $notTopmost = [IntPtr](-2)
    $noMoveSizeActivate = 0x53
    [SubuwuVisualSmokeNative]::SetWindowPos(
        $Process.MainWindowHandle, $topmost, 0, 0, 0, 0,
        $noMoveSizeActivate) | Out-Null
    # Park away from ImGui controls so workspace tooltips do not obscure the
    # screenshot under inspection.
    [SubuwuVisualSmokeNative]::SetCursorPos(
        $rect.Right - 120, $rect.Top + 15) | Out-Null
    Start-Sleep -Milliseconds 350

    $bitmap = New-Object System.Drawing.Bitmap $width, $height
    $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
    try {
        $graphics.CopyFromScreen($rect.Left, $rect.Top, 0, 0, $bitmap.Size)
        $path = Join-Path $outputPath ($Name + ".png")
        $bitmap.Save($path, [System.Drawing.Imaging.ImageFormat]::Png)
    } finally {
        $graphics.Dispose()
        $bitmap.Dispose()
        [SubuwuVisualSmokeNative]::SetWindowPos(
            $Process.MainWindowHandle, $notTopmost, 0, 0, 0, 0,
            $noMoveSizeActivate) | Out-Null
    }
    return $path
}

function Invoke-RelativeClick(
    [System.Diagnostics.Process]$Process,
    [int]$X,
    [int]$Y
) {
    $rect = Get-TargetRect $Process
    [SubuwuVisualSmokeNative]::SetWindowPos(
        $Process.MainWindowHandle, [IntPtr](-1), 0, 0, 0, 0, 0x53) | Out-Null
    [SubuwuVisualSmokeNative]::SetCursorPos(
        $rect.Left + $X, $rect.Top + $Y) | Out-Null
    [SubuwuVisualSmokeNative]::mouse_event(
        0x0002, 0, 0, 0, [UIntPtr]::Zero)
    [SubuwuVisualSmokeNative]::mouse_event(
        0x0004, 0, 0, 0, [UIntPtr]::Zero)
    Start-Sleep -Milliseconds 500
    [SubuwuVisualSmokeNative]::SetWindowPos(
        $Process.MainWindowHandle, [IntPtr](-2), 0, 0, 0, 0, 0x53) | Out-Null
}

function Invoke-RelativeRightClick(
    [System.Diagnostics.Process]$Process,
    [int]$X,
    [int]$Y
) {
    $rect = Get-TargetRect $Process
    [SubuwuVisualSmokeNative]::SetWindowPos(
        $Process.MainWindowHandle, [IntPtr](-1), 0, 0, 0, 0, 0x53) | Out-Null
    [SubuwuVisualSmokeNative]::SetCursorPos($rect.Left + $X, $rect.Top + $Y) | Out-Null
    [SubuwuVisualSmokeNative]::mouse_event(0x0008, 0, 0, 0, [UIntPtr]::Zero)
    [SubuwuVisualSmokeNative]::mouse_event(0x0010, 0, 0, 0, [UIntPtr]::Zero)
    Start-Sleep -Milliseconds 500
    [SubuwuVisualSmokeNative]::SetWindowPos(
        $Process.MainWindowHandle, [IntPtr](-2), 0, 0, 0, 0, 0x53) | Out-Null
}

function Invoke-RelativeDrag(
    [System.Diagnostics.Process]$Process,
    [int]$FromX,
    [int]$FromY,
    [int]$ToX,
    [int]$ToY
) {
    $rect = Get-TargetRect $Process
    [SubuwuVisualSmokeNative]::SetWindowPos(
        $Process.MainWindowHandle, [IntPtr](-1), 0, 0, 0, 0, 0x53) | Out-Null
    [SubuwuVisualSmokeNative]::SetCursorPos(
        $rect.Left + $FromX, $rect.Top + $FromY) | Out-Null
    [SubuwuVisualSmokeNative]::mouse_event(0x0002, 0, 0, 0, [UIntPtr]::Zero)
    for ($step = 1; $step -le 8; ++$step) {
        $x = $FromX + [int](($ToX - $FromX) * $step / 8)
        $y = $FromY + [int](($ToY - $FromY) * $step / 8)
        [SubuwuVisualSmokeNative]::SetCursorPos($rect.Left + $x, $rect.Top + $y) | Out-Null
        Start-Sleep -Milliseconds 35
    }
    [SubuwuVisualSmokeNative]::mouse_event(0x0004, 0, 0, 0, [UIntPtr]::Zero)
    Start-Sleep -Milliseconds 500
    [SubuwuVisualSmokeNative]::SetWindowPos(
        $Process.MainWindowHandle, [IntPtr](-2), 0, 0, 0, 0, 0x53) | Out-Null
}

function Invoke-KeyChord(
    [System.Diagnostics.Process]$Process,
    [byte[]]$Keys
) {
    $rect = Get-TargetRect $Process
    # Clicking the native title bar activates exactly this HWND without
    # triggering an ImGui control. Keep it topmost only for the interaction.
    [SubuwuVisualSmokeNative]::SetWindowPos(
        $Process.MainWindowHandle, [IntPtr](-1), 0, 0, 0, 0, 0x53) | Out-Null
    [SubuwuVisualSmokeNative]::SetCursorPos(
        $rect.Left + [int](($rect.Right - $rect.Left) / 2), $rect.Top + 14) | Out-Null
    [SubuwuVisualSmokeNative]::mouse_event(
        0x0002, 0, 0, 0, [UIntPtr]::Zero)
    [SubuwuVisualSmokeNative]::mouse_event(
        0x0004, 0, 0, 0, [UIntPtr]::Zero)
    foreach ($key in $Keys) {
        [SubuwuVisualSmokeNative]::keybd_event(
            $key, 0, 0, [UIntPtr]::Zero)
    }
    for ($index = $Keys.Length - 1; $index -ge 0; --$index) {
        [SubuwuVisualSmokeNative]::keybd_event(
            $Keys[$index], 0, 0x0002, [UIntPtr]::Zero)
    }
    Start-Sleep -Milliseconds 500
    [SubuwuVisualSmokeNative]::SetWindowPos(
        $Process.MainWindowHandle, [IntPtr](-2), 0, 0, 0, 0, 0x53) | Out-Null
}

# Type an ASCII string into the focused control one key at a time. Unlike
# Invoke-KeyChord (which presses a whole chord simultaneously) this sends
# discrete down/up events so a word lands in a text field. Activation uses the
# same non-client title-bar click as Invoke-KeyChord: GLFW never receives
# non-client clicks, so an open ImGui popup (e.g. the command palette) keeps
# its filter focus. Only letters, digits, and spaces are sent; other
# characters (em dash, etc.) are skipped, so pass an ASCII substring.
function Invoke-Text(
    [System.Diagnostics.Process]$Process,
    [string]$Text
) {
    [SubuwuVisualSmokeNative]::SetWindowPos(
        $Process.MainWindowHandle, [IntPtr](-1), 0, 0, 0, 0, 0x53) | Out-Null
    $rect = Get-TargetRect $Process
    [SubuwuVisualSmokeNative]::SetCursorPos(
        $rect.Left + [int](($rect.Right - $rect.Left) / 2), $rect.Top + 14) | Out-Null
    [SubuwuVisualSmokeNative]::mouse_event(0x0002, 0, 0, 0, [UIntPtr]::Zero)
    [SubuwuVisualSmokeNative]::mouse_event(0x0004, 0, 0, 0, [UIntPtr]::Zero)
    foreach ($ch in $Text.ToCharArray()) {
        $vk = 0
        if ($ch -ge 'a' -and $ch -le 'z') {
            $vk = [byte][char]([string]$ch).ToUpper()
        } elseif (($ch -ge 'A' -and $ch -le 'Z') -or ($ch -ge '0' -and $ch -le '9')) {
            $vk = [byte][char]$ch
        } elseif ($ch -eq ' ') {
            $vk = 0x20
        } else {
            continue
        }
        [SubuwuVisualSmokeNative]::keybd_event($vk, 0, 0, [UIntPtr]::Zero)
        [SubuwuVisualSmokeNative]::keybd_event($vk, 0, 0x0002, [UIntPtr]::Zero)
        Start-Sleep -Milliseconds 45
    }
    Start-Sleep -Milliseconds 400
    [SubuwuVisualSmokeNative]::SetWindowPos(
        $Process.MainWindowHandle, [IntPtr](-2), 0, 0, 0, 0, 0x53) | Out-Null
}

$stdoutPath = Join-Path $outputPath "stdout.log"
$stderrPath = Join-Path $outputPath "stderr.log"
$manifestPath = Join-Path $outputPath "manifest.json"
$startArgs = @{
    FilePath = $exePath
    PassThru = $true
    RedirectStandardOutput = $stdoutPath
    RedirectStandardError = $stderrPath
}
if ($Scenario -in @("Demo", "Designer", "EditRoundTrip", "Readiness")) {
    $startArgs.ArgumentList = $projectPath
}
$process = Start-Process @startArgs

$captures = @()
$result = $null
try {
    for ($attempt = 0; $attempt -lt 40; ++$attempt) {
        if ($process.HasExited) {
            throw "SubuwuTuner exited during startup with code $($process.ExitCode)."
        }
        $process.Refresh()
        if ($process.MainWindowHandle -ne 0 -and $process.Responding) { break }
        Start-Sleep -Milliseconds 250
    }
    if ($process.MainWindowHandle -eq 0) {
        throw "Timed out waiting for the SubuwuTuner window."
    }

    if ($WindowWidth -gt 0 -and $WindowHeight -gt 0) {
        # SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE. GLFW applies its own
        # 640x400 floor, so values below that exercise the supported minimum.
        [SubuwuVisualSmokeNative]::SetWindowPos(
            $process.MainWindowHandle, [IntPtr]::Zero, 0, 0,
            $WindowWidth, $WindowHeight, 0x0016) | Out-Null
        Start-Sleep -Milliseconds 500
    }

    if ($Scenario -eq "Welcome") {
        $captures += Save-WindowScreenshot $process "01-welcome"
    } elseif ($Scenario -eq "Designer") {
        Invoke-KeyChord $process ([byte[]](0x11, 0x33)) # Ctrl+3: Features
        $captures += Save-WindowScreenshot $process "01-designer-empty"
        Invoke-RelativeClick $process 120 200 # Insert palette
        $captures += Save-WindowScreenshot $process "02-insert-palette"
        Invoke-RelativeClick $process 185 240 # Hook: Throttle position
        $captures += Save-WindowScreenshot $process "03-hook-node"
        Invoke-RelativeClick $process 120 200 # Insert palette
        Invoke-RelativeClick $process 155 435 # Primitive: Add
        $captures += Save-WindowScreenshot $process "04-hook-and-primitive"
        Invoke-RelativeDrag $process 272 341 122 421 # Engine RPM -> Add.a
        $captures += Save-WindowScreenshot $process "05-wired-nodes"
        Invoke-RelativeRightClick $process 122 441 # Add.b input popup
        $captures += Save-WindowScreenshot $process "06-pin-default-popup"
        Invoke-RelativeClick $process 180 513 # constant-value input
        Invoke-KeyChord $process ([byte[]](0x11, 0x41)) # Ctrl+A
        Invoke-KeyChord $process ([byte[]](0x35)) # 5
        Invoke-KeyChord $process ([byte[]](0x0D)) # Enter commits
        $captures += Save-WindowScreenshot $process "07-default-committed"
        Invoke-RelativeRightClick $process 180 400 # Add node context menu
        $captures += Save-WindowScreenshot $process "08-node-context-menu"
        Invoke-KeyChord $process ([byte[]](0x1B))
        Invoke-RelativeRightClick $process 195 380 # connected edge context menu
        $captures += Save-WindowScreenshot $process "09-edge-context-menu"
        Invoke-KeyChord $process ([byte[]](0x1B))
        Invoke-RelativeClick $process 96 227 # validation/lint status text
        $captures += Save-WindowScreenshot $process "10-validation-popup"
        Invoke-KeyChord $process ([byte[]](0x1B))
        Invoke-RelativeClick $process 175 227 # Compile preview
        $captures += Save-WindowScreenshot $process "11-compile-preview"
        # Toolbar order: Insert | Undo | Redo | Clear graph | Reset view | Save |
        # Load | ... (Undo/Redo were added between Insert and Clear graph).
        Invoke-RelativeClick $process 583 200 # Load, expect dirty-document gate
        $captures += Save-WindowScreenshot $process "12-load-discard-confirmation"
        Invoke-RelativeClick $process 552 479 # Keep editing
        Invoke-RelativeClick $process 318 200 # Clear graph, expect discard gate
        $captures += Save-WindowScreenshot $process "13-clear-discard-confirmation"
        Invoke-RelativeClick $process 552 479 # Keep editing (dismiss clear gate)
        # Graph undo/redo: undo reverts the last edit, redo re-applies it.
        Invoke-RelativeClick $process 190 200 # Undo
        $captures += Save-WindowScreenshot $process "14-after-undo"
        # Undo dropped the pin default, so lint now shows a warning. Open the
        # status popup to capture the clickable "jump to node" findings.
        Invoke-RelativeClick $process 115 227 # "1 warning" status pill
        $captures += Save-WindowScreenshot $process "15-lint-findings"
        Invoke-KeyChord $process ([byte[]](0x1B)) # dismiss popup
        Invoke-RelativeClick $process 241 200 # Redo
        $captures += Save-WindowScreenshot $process "16-after-redo"
    } elseif ($Scenario -eq "Readiness") {
        # Dirty the project with one cell edit so the readiness verification
        # summary shows an actionable item and the Save-project remediation
        # button appears, then open the Project Readiness panel via the View
        # menu and capture it (flash-ready-checksum vs project-integrity rows,
        # the summary tally, and the remediation action).
        Invoke-KeyChord $process ([byte[]](0x11, 0x4B)) # Ctrl+K
        Invoke-Text $process "Boost"
        Invoke-KeyChord $process ([byte[]](0x0D)) # open Boost table
        Invoke-RelativeClick $process 485 208 # first data cell
        Invoke-KeyChord $process ([byte[]](0x71)) # F2: inline editor
        Invoke-KeyChord $process ([byte[]](0x11, 0x41)) # Ctrl+A
        Invoke-Text $process "17"
        Invoke-KeyChord $process ([byte[]](0x0D)) # commit -> project dirty
        Invoke-RelativeClick $process 145 44 # View menu in the menu bar
        $captures += Save-WindowScreenshot $process "01-view-menu"
        Invoke-RelativeClick $process 177 151 # Project Readiness menu item
        $captures += Save-WindowScreenshot $process "02-readiness-panel"
    } elseif ($Scenario -eq "EditRoundTrip") {
        # Isolated-copy edit round trip: open a table via the command palette,
        # edit a cell, undo/redo, save, then close and reopen the project from
        # disk and confirm the edited value and history survived.
        Invoke-KeyChord $process ([byte[]](0x11, 0x31)) # Ctrl+1: Tune
        $captures += Save-WindowScreenshot $process "01-tune-workspace"
        # Open the Boost table through the palette (robust vs sidebar clicks).
        Invoke-KeyChord $process ([byte[]](0x11, 0x4B)) # Ctrl+K
        Invoke-Text $process "Boost"
        $captures += Save-WindowScreenshot $process "02-palette-boost"
        Invoke-KeyChord $process ([byte[]](0x0D)) # Enter: open top result
        $captures += Save-WindowScreenshot $process "03-table-open"
        # Edit one cell: select it, press F2 to open the inline editor
        # (grid cells select on single click and only enter edit mode on
        # F2 / double-click — see src/ui/src/panels/table_view.cpp), select
        # the existing value, type a distinctive replacement, commit.
        Invoke-RelativeClick $process 485 208 # first data cell (col 1000, row 1.0)
        $captures += Save-WindowScreenshot $process "04-cell-selected"
        Invoke-KeyChord $process ([byte[]](0x71)) # F2: open inline editor
        Invoke-KeyChord $process ([byte[]](0x11, 0x41)) # Ctrl+A: select value text
        Invoke-Text $process "17"
        Invoke-KeyChord $process ([byte[]](0x0D)) # Enter commits the edit
        $captures += Save-WindowScreenshot $process "05-cell-edited"
        # Undo then redo; the History panel reflects both transitions.
        Invoke-KeyChord $process ([byte[]](0x11, 0x5A)) # Ctrl+Z: undo
        $captures += Save-WindowScreenshot $process "06-after-undo"
        # Redo via the palette command rather than the Ctrl+Shift+Z chord:
        # more reliable (single committed action) and avoids any global
        # Ctrl+Shift+Z hotkey collision with overlay software.
        Invoke-KeyChord $process ([byte[]](0x11, 0x4B)) # Ctrl+K
        Invoke-Text $process "Redo"
        Invoke-KeyChord $process ([byte[]](0x0D)) # Enter: run Redo
        $captures += Save-WindowScreenshot $process "07-after-redo"
        # Save (read-after-write verified) then close the project.
        Invoke-KeyChord $process ([byte[]](0x11, 0x53)) # Ctrl+S: save
        $captures += Save-WindowScreenshot $process "08-saved"
        Invoke-KeyChord $process ([byte[]](0x11, 0x4B)) # Ctrl+K
        Invoke-Text $process "Close"
        Invoke-KeyChord $process ([byte[]](0x0D)) # Enter: close project
        $captures += Save-WindowScreenshot $process "09-closed"
        # Reopen from the recents list; the project re-reads from disk.
        Invoke-KeyChord $process ([byte[]](0x11, 0x4B)) # Ctrl+K
        Invoke-Text $process "roundtrip"
        $captures += Save-WindowScreenshot $process "10-palette-reopen"
        Invoke-KeyChord $process ([byte[]](0x0D)) # Enter: reopen recent
        $captures += Save-WindowScreenshot $process "11-reopened"
        # Reopen the same table and confirm the edited value persisted.
        Invoke-KeyChord $process ([byte[]](0x11, 0x31)) # Ctrl+1: Tune
        Invoke-KeyChord $process ([byte[]](0x11, 0x4B)) # Ctrl+K
        Invoke-Text $process "Boost"
        Invoke-KeyChord $process ([byte[]](0x0D))
        $captures += Save-WindowScreenshot $process "12-persisted-value"
    } else {
        $captures += Save-WindowScreenshot $process "01-tune-workspace"
        Invoke-KeyChord $process ([byte[]](0x11, 0x32)) # Ctrl+2: Log
        $captures += Save-WindowScreenshot $process "02-log-empty-state"
        # The bundled-sample action is intentionally non-dialog and
        # hardware-independent, making it safe for autonomous interaction.
        $logRect = Get-TargetRect $process
        $logWidth = $logRect.Right - $logRect.Left
        $sampleButtonY = if ($logWidth -ge 1000) { 138 } else { 152 }
        Invoke-RelativeClick $process 215 $sampleButtonY
        $captures += Save-WindowScreenshot $process "03-log-sample"
        # Suggested edits: seed the auto-tune knock modal from this log.
        Invoke-RelativeClick $process 178 502 # "Suggest timing pull from this log ->"
        $captures += Save-WindowScreenshot $process "03c-autotune-seeded"
        Invoke-RelativeClick $process 449 630 # Run preview (debug)
        $captures += Save-WindowScreenshot $process "03d-autotune-proposals"
        Invoke-RelativeClick $process 591 679 # Close
        # Log -> tune: "Open in Tune" on the knock finding jumps to the Tune
        # workspace, selects the ignition table, and highlights the cell at
        # the finding's rpm/load.
        Invoke-RelativeClick $process 252 381 # "Open in Tune ->" on first finding
        $captures += Save-WindowScreenshot $process "03b-log-to-tune"
        Invoke-KeyChord $process ([byte[]](0x11, 0x32)) # Ctrl+2: back to Log
        Invoke-KeyChord $process ([byte[]](0x11, 0x33)) # Ctrl+3: Features
        $captures += Save-WindowScreenshot $process "04-feature-workspace"
        Invoke-KeyChord $process ([byte[]](0x11, 0x4B)) # Ctrl+K: command palette
        $captures += Save-WindowScreenshot $process "05-command-palette"
        Invoke-KeyChord $process ([byte[]](0x1B)) # Escape: dismiss
        $captures += Save-WindowScreenshot $process "06-command-palette-dismissed"
    }

    $process.Refresh()
    if (-not $process.Responding) {
        throw "SubuwuTuner stopped responding during the visual smoke pass."
    }
    $stderrText = if (Test-Path -LiteralPath $stderrPath) {
        Get-Content -LiteralPath $stderrPath -Raw
    } else { "" }

    $screenshotInfo = foreach ($capture in $captures) {
        $image = [System.Drawing.Bitmap]::FromFile($capture)
        try {
            $colors = [System.Collections.Generic.HashSet[string]]::new()
            $sampleCount = 0
            $nonBlack = 0
            # Sample a dense grid with 5-bit-per-channel quantization. The
            # earlier 20x15 grid at 4-bit quantization false-failed on
            # legitimately-rendered but low-chroma, mostly-dark screens
            # (e.g. the Log Explorer empty state), which only hit 5 color
            # buckets on the sparse grid and tripped the >=6 gate. A
            # 60x40 grid at 5-bit quantization scores real frames at
            # 26-31 colors while a truly-black frame still scores 1.
            $stepX = [Math]::Max(1, [int]($image.Width / 60))
            $stepY = [Math]::Max(1, [int]($image.Height / 40))
            for ($y = 0; $y -lt $image.Height; $y += $stepY) {
                for ($x = 0; $x -lt $image.Width; $x += $stepX) {
                    $pixel = $image.GetPixel($x, $y)
                    $colors.Add("$($pixel.R -shr 3):$($pixel.G -shr 3):$($pixel.B -shr 3)") |
                        Out-Null
                    if ($pixel.R -gt 8 -or $pixel.G -gt 8 -or $pixel.B -gt 8) {
                        ++$nonBlack
                    }
                    ++$sampleCount
                }
            }
            $captureBytes = (Get-Item -LiteralPath $capture).Length
            $nonBlackRatio = if ($sampleCount -eq 0) { 0.0 } else {
                $nonBlack / $sampleCount
            }
            $sane = $captureBytes -gt 4096 -and $colors.Count -ge 6 -and
                    $nonBlackRatio -ge 0.05
            if (-not $sane) {
                throw "Screenshot sanity check failed for $capture " +
                      "(bytes=$captureBytes, colors=$($colors.Count), " +
                      "non-black=$nonBlackRatio)."
            }
            [pscustomobject]@{
                path = $capture
                width = $image.Width
                height = $image.Height
                bytes = $captureBytes
                sampled_colors = $colors.Count
                non_black_ratio = $nonBlackRatio
                sanity_passed = $sane
            }
        } finally {
            $image.Dispose()
        }
    }
    $result = [pscustomobject]@{
        executable = $exePath
        scenario = $Scenario
        project = $projectPath
        requested_window = "${WindowWidth}x${WindowHeight}"
        pid = $process.Id
        title = $process.MainWindowTitle
        responding = $process.Responding
        stderr_empty = [string]::IsNullOrWhiteSpace($stderrText)
        stdout_bytes = (Get-Item -LiteralPath $stdoutPath).Length
        stderr_bytes = (Get-Item -LiteralPath $stderrPath).Length
        termination = "pending"
        exit_code = $null
        screenshots = @($screenshotInfo)
    }

    if (-not [string]::IsNullOrWhiteSpace($stderrText)) {
        Write-Warning "GUI wrote to stderr; inspect $stderrPath"
    }
} finally {
    if (-not $KeepOpen -and -not $process.HasExited) {
        Stop-Process -Id $process.Id
        $process.WaitForExit()
    }
    if ($null -ne $result) {
        if ($KeepOpen -and -not $process.HasExited) {
            $result.termination = "kept-open"
        } else {
            $process.Refresh()
            $result.termination = "stopped-by-harness"
            $result.exit_code = $process.ExitCode
        }
        $encodedResult = $result | ConvertTo-Json -Depth 4
        Set-Content -LiteralPath $manifestPath -Value $encodedResult -Encoding UTF8
        $encodedResult
    }
    if ($mutexHeld) {
        $smokeMutex.ReleaseMutex()
    }
    $smokeMutex.Dispose()
}
