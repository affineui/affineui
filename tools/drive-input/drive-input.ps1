# drive-input — synthetic pointer input for windowed AffineUI apps.
#
# Windowed-subsystem exes take no stdin, so a profiling / repro run can't be
# scripted the usual way. This posts real WM_MOUSEMOVE / button messages
# into the target window via win32 PostMessage, so hover sweeps, drags, and
# clicks are reproducible run-to-run (essential for before/after perf
# comparison). See docs/HOW_TO_PROFILE.md.
#
# Usage:
#   tools\drive-input\drive-input.ps1 -ProcId <pid> [-Title <window title>]
#                                     [-Script <path-to-.ps1>]
#
#   -ProcId   process id of the running app (found by pid, then by title)
#   -Title    window title fallback if the pid lookup misses
#   -Script   a PowerShell fragment that calls the exposed helpers
#             (MoveTo/Down/Up/Click/Drag). If omitted, a generic
#             hover-sweep + a few clicks run as a smoke exercise.
#
# The helpers are dot-sourced into the -Script's scope:
#   MoveTo x y            post a mouse move
#   Down x y / Up x y     post a left button down / up (at x,y)
#   Click x y             move, down, up (with small settle sleeps)
#   Drag x0 y0 x1 y1 [n]  press at (x0,y0), move to (x1,y1) over n steps, release
#
# Coordinates are CLIENT pixels (0,0 = top-left of the window content).

param(
    [Parameter(Mandatory = $true)][int]$ProcId,
    [string]$Title = "",
    [string]$Script = ""
)

Add-Type @'
using System;
using System.Runtime.InteropServices;
public class Win32Input {
    [DllImport("user32.dll", SetLastError=true)]
    public static extern bool PostMessage(IntPtr hWnd, uint Msg, IntPtr wParam, IntPtr lParam);
    [DllImport("user32.dll")]
    public static extern IntPtr FindWindow(string lpClassName, string lpWindowName);
    [DllImport("user32.dll")]
    public static extern bool EnumWindows(EnumWindowsProc lpEnumFunc, IntPtr lParam);
    [DllImport("user32.dll")]
    public static extern uint GetWindowThreadProcessId(IntPtr hWnd, out uint lpdwProcessId);
    [DllImport("user32.dll")]
    public static extern bool IsWindowVisible(IntPtr hWnd);
    public delegate bool EnumWindowsProc(IntPtr hWnd, IntPtr lParam);
    public static IntPtr Found = IntPtr.Zero;
    public static uint TargetPid = 0;
    public static bool Enum(IntPtr h, IntPtr l) {
        uint pid; GetWindowThreadProcessId(h, out pid);
        if (pid == TargetPid && IsWindowVisible(h)) { Found = h; return false; }
        return true;
    }
    public static IntPtr FindByPid(uint pid) {
        TargetPid = pid; Found = IntPtr.Zero;
        EnumWindows(Enum, IntPtr.Zero);
        return Found;
    }
}
'@

# The window may not exist for a second or two after launch; retry.
$script:Hwnd = [IntPtr]::Zero
for ($try = 0; $try -lt 20 -and $script:Hwnd -eq [IntPtr]::Zero; $try++) {
    $script:Hwnd = [Win32Input]::FindByPid([uint32]$ProcId)
    if ($script:Hwnd -eq [IntPtr]::Zero -and $Title -ne "") {
        $script:Hwnd = [Win32Input]::FindWindow($null, $Title)
    }
    if ($script:Hwnd -eq [IntPtr]::Zero) { Start-Sleep -Milliseconds 500 }
}
if ($script:Hwnd -eq [IntPtr]::Zero) {
    Write-Error "drive-input: window not found for pid $ProcId (title '$Title')"
    exit 1
}
Write-Host "drive-input: hwnd=$($script:Hwnd)"

$WM_MOUSEMOVE = 0x0200; $WM_LBUTTONDOWN = 0x0201; $WM_LBUTTONUP = 0x0202
# NB: 'LP' is a PowerShell alias for Out-Printer — never name a helper LP.
function MakeLParam([int]$x, [int]$y) { return [IntPtr](($y -shl 16) -bor ($x -band 0xFFFF)) }
function MoveTo([int]$x, [int]$y) { [void][Win32Input]::PostMessage($script:Hwnd, $WM_MOUSEMOVE, [IntPtr]0, (MakeLParam $x $y)) }
function Down([int]$x, [int]$y) { [void][Win32Input]::PostMessage($script:Hwnd, $WM_LBUTTONDOWN, [IntPtr]1, (MakeLParam $x $y)) }
function Up([int]$x, [int]$y)   { [void][Win32Input]::PostMessage($script:Hwnd, $WM_LBUTTONUP, [IntPtr]0, (MakeLParam $x $y)) }
function Click([int]$x, [int]$y) {
    MoveTo $x $y; Start-Sleep -Milliseconds 60
    Down $x $y; Start-Sleep -Milliseconds 50; Up $x $y
    Start-Sleep -Milliseconds 200
}
function Drag([int]$x0, [int]$y0, [int]$x1, [int]$y1, [int]$steps = 16) {
    MoveTo $x0 $y0; Start-Sleep -Milliseconds 60
    Down $x0 $y0; Start-Sleep -Milliseconds 50
    for ($i = 1; $i -le $steps; $i++) {
        $t = $i / [double]$steps
        MoveTo ([int]($x0 + ($x1 - $x0) * $t)) ([int]($y0 + ($y1 - $y0) * $t))
        Start-Sleep -Milliseconds 30
    }
    Up $x1 $y1; Start-Sleep -Milliseconds 300
}

if ($Script -ne "" -and (Test-Path $Script)) {
    Write-Host "drive-input: running $Script"
    . $Script
} else {
    # Default smoke exercise: a hover sweep + a few clicks. Replace with a
    # -Script targeting the specific gesture you are profiling.
    Write-Host "drive-input: default hover-sweep + clicks (pass -Script for a real workload)"
    for ($i = 0; $i -lt 30; $i++) { MoveTo (100 + $i * 40) (80 + $i * 20); Start-Sleep -Milliseconds 30 }
    Start-Sleep -Milliseconds 500
    Click 300 300
    Click 600 200
}
Write-Host "drive-input: done"
