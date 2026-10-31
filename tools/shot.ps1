# Screenshot a window by (partial) title into a PNG. ASCII-only source on purpose.
# Uses PrintWindow (PW_RENDERFULLCONTENT) so overlapping windows do not spoil the shot.
# Usage: powershell -File tools\shot.ps1 -Title "SFS" -Out shot.png
param(
    [Parameter(Mandatory = $true)][string]$Title,
    [Parameter(Mandatory = $true)][string]$Out
)

Add-Type -AssemblyName System.Drawing
if (-not ("WinShot2" -as [type])) {
    Add-Type @"
using System;
using System.Text;
using System.Collections.Generic;
using System.Runtime.InteropServices;
public class WinShot2 {
    public delegate bool EnumWindowsProc(IntPtr hWnd, IntPtr lParam);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumWindowsProc cb, IntPtr p);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowTextW(IntPtr h, StringBuilder s, int n);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr after, int x, int y, int cx, int cy, uint flags);
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint flags);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    public struct RECT { public int Left, Top, Right, Bottom; }
    public static List<IntPtr> Find(string needle) {
        var list = new List<IntPtr>();
        EnumWindows(delegate(IntPtr h, IntPtr p) {
            if (!IsWindowVisible(h)) return true;
            var sb = new StringBuilder(512);
            GetWindowTextW(h, sb, 512);
            if (sb.ToString().IndexOf(needle, StringComparison.OrdinalIgnoreCase) >= 0) list.Add(h);
            return true;
        }, IntPtr.Zero);
        return list;
    }
    public static bool Topmost(IntPtr h) { return SetWindowPos(h, new IntPtr(-1), 0, 0, 0, 0, 0x0001 | 0x0002 | 0x0040); }
    public static bool NotTopmost(IntPtr h) { return SetWindowPos(h, new IntPtr(-2), 0, 0, 0, 0, 0x0001 | 0x0002); }
}
"@
}

$handles = [WinShot2]::Find($Title)
if ($handles.Count -eq 0) { Write-Host "window not found"; exit 2 }
$h = $handles[$handles.Count - 1]
[WinShot2]::Topmost($h) | Out-Null
[WinShot2]::SetForegroundWindow($h) | Out-Null
Start-Sleep -Milliseconds 900

$r = New-Object WinShot2+RECT
[WinShot2]::GetWindowRect($h, [ref]$r) | Out-Null
$w = $r.Right - $r.Left
$hh = $r.Bottom - $r.Top
if ($w -le 0 -or $hh -le 0) { Write-Host "bad rect"; exit 3 }

$bmp = New-Object System.Drawing.Bitmap($w, $hh)
$g = [System.Drawing.Graphics]::FromImage($bmp)
$hdc = $g.GetHdc()
$ok = [WinShot2]::PrintWindow($h, $hdc, 2)
$g.ReleaseHdc($hdc)
if (-not $ok) {
    $g.CopyFromScreen($r.Left, $r.Top, 0, 0, $bmp.Size)
}
$g.Dispose()
$bmp.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png)
$bmp.Dispose()
[WinShot2]::NotTopmost($h) | Out-Null
Write-Host ("saved {0} ({1}x{2}) printwindow={3}" -f $Out, $w, $hh, $ok)
