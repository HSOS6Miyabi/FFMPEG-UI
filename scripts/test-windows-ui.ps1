$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class WorkbenchCapture
{
    [StructLayout(LayoutKind.Sequential)]
    public struct Rect { public int Left, Top, Right, Bottom; }
    [DllImport("user32.dll", CharSet=CharSet.Unicode)]
    public static extern IntPtr FindWindow(string className, string title);
    [DllImport("user32.dll")]
    [return: MarshalAs(UnmanagedType.Bool)]
    public static extern bool GetWindowRect(IntPtr hwnd, out Rect rectangle);
    [DllImport("user32.dll")]
    [return: MarshalAs(UnmanagedType.Bool)]
    public static extern bool PrintWindow(IntPtr hwnd, IntPtr target, uint flags);
    [DllImport("user32.dll")]
    public static extern IntPtr GetWindowDC(IntPtr hwnd);
    [DllImport("user32.dll")]
    public static extern int ReleaseDC(IntPtr hwnd, IntPtr dc);
    [DllImport("gdi32.dll")]
    [return: MarshalAs(UnmanagedType.Bool)]
    public static extern bool BitBlt(IntPtr target, int x, int y, int width, int height,
                                    IntPtr source, int sourceX, int sourceY, uint operation);
    [DllImport("user32.dll")]
    [return: MarshalAs(UnmanagedType.Bool)]
    public static extern bool SetWindowPos(IntPtr hwnd, IntPtr after, int x, int y,
                                          int width, int height, uint flags);
    [DllImport("user32.dll")]
    [return: MarshalAs(UnmanagedType.Bool)]
    public static extern bool PostMessage(IntPtr hwnd, uint message,
                                          IntPtr wparam, IntPtr lparam);
}
'@ -ErrorAction Stop

function Save-WindowScreenshot([IntPtr] $Handle, [string] $Path) {
    $bounds = [WorkbenchCapture+Rect]::new()
    if (-not [WorkbenchCapture]::GetWindowRect($Handle, [ref] $bounds)) {
        throw "Cannot inspect the window size."
    }
    $width = $bounds.Right - $bounds.Left
    $height = $bounds.Bottom - $bounds.Top
    if ($width -lt 200 -or $height -lt 200) {
        throw "Unexpected Windows UI size."
    }
    $bitmap = [System.Drawing.Bitmap]::new($width, $height)
    try {
        $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
        try {
            $dc = $graphics.GetHdc()
            try {
                $printed = [WorkbenchCapture]::PrintWindow($Handle, $dc, 2)
                if (-not $printed) {
                    $source = [WorkbenchCapture]::GetWindowDC($Handle)
                    try {
                        if ($source -ne [IntPtr]::Zero) {
                            [void] [WorkbenchCapture]::BitBlt(
                                $dc, 0, 0, $width, $height, $source, 0, 0, 0x00CC0020)
                        }
                    } finally {
                        if ($source -ne [IntPtr]::Zero) {
                            [void] [WorkbenchCapture]::ReleaseDC($Handle, $source)
                        }
                    }
                }
            } finally { $graphics.ReleaseHdc($dc) }
        } finally { $graphics.Dispose() }

        $colors = [System.Collections.Generic.HashSet[int]]::new()
        foreach ($vertical in @(0.12, 0.25, 0.4, 0.55, 0.7, 0.85)) {
            foreach ($horizontal in @(0.08, 0.2, 0.38, 0.54, 0.7, 0.86)) {
                $pixel = $bitmap.GetPixel(
                    [int]($width * $horizontal), [int]($height * $vertical))
                [void] $colors.Add($pixel.ToArgb())
            }
        }
        if ($colors.Count -lt 4) {
            throw "The captured Windows UI image is blank or has too few visual details."
        }
        $bitmap.Save($Path, [System.Drawing.Imaging.ImageFormat]::Png)
        Write-Host "Captured $Path with $($colors.Count) sampled colors."
    } finally { $bitmap.Dispose() }
}

function Click-Window([IntPtr] $Handle, [int] $X, [int] $Y) {
    $location = [IntPtr]::new((($Y -band 0xffff) -shl 16) -bor ($X -band 0xffff))
    [void] [WorkbenchCapture]::PostMessage($Handle, 0x0201, [IntPtr]::new(1), $location)
    [void] [WorkbenchCapture]::PostMessage($Handle, 0x0202, [IntPtr]::Zero, $location)
    Start-Sleep -Milliseconds 900
}

$repoRoot = Split-Path -Parent $PSScriptRoot
$exe = (Resolve-Path -LiteralPath (Join-Path $repoRoot 'build/windows-release/video-compression-workbench.exe')).Path
$output = Join-Path $repoRoot 'artifacts/ui'
New-Item -ItemType Directory -Force -Path $output | Out-Null
$env:FFMPEG_STUDIO_CONFIG_DIR = Join-Path $env:RUNNER_TEMP 'workbench-windows-ui-config'
New-Item -ItemType Directory -Force -Path $env:FFMPEG_STUDIO_CONFIG_DIR | Out-Null
$video = Join-Path $env:RUNNER_TEMP 'workbench-windows-preview.mp4'
& ffmpeg -hide_banner -loglevel error -y -f lavfi -i 'testsrc2=size=320x180:rate=24:duration=4' -an -c:v mpeg4 -q:v 5 $video
if ($LASTEXITCODE -ne 0) { throw 'Cannot create a test video.' }
[System.IO.File]::WriteAllText(
    (Join-Path $env:FFMPEG_STUDIO_CONFIG_DIR 'queue.ffq'),
    "FFmpegStudioQueue/1" + [Environment]::NewLine + $video + [Environment]::NewLine,
    [System.Text.UTF8Encoding]::new($false))

$app = Start-Process -FilePath $exe -PassThru
try {
    $handle = [IntPtr]::Zero
    for ($i = 0; $i -lt 50; $i++) {
        Start-Sleep -Milliseconds 500
        $app.Refresh()
        $handle = $app.MainWindowHandle
        if ($handle -eq [IntPtr]::Zero) {
            $handle = [WorkbenchCapture]::FindWindow('VideoCompressionWorkbenchWindow', $null)
        }
        if ($handle -ne [IntPtr]::Zero) { break }
        if ($app.HasExited) { throw 'The Windows application exited before opening a window.' }
    }
    if ($handle -eq [IntPtr]::Zero) { throw 'The Windows application window was not found.' }
    [void] [WorkbenchCapture]::SetWindowPos($handle, [IntPtr]::Zero, 0, 0, 1400, 840, 0x0004)
    Start-Sleep -Seconds 3

    Save-WindowScreenshot $handle (Join-Path $output 'video-compression-workbench-windows.png')
    Click-Window $handle 860 165
    Save-WindowScreenshot $handle (Join-Path $output 'video-compression-workbench-windows-expanded.png')
    Click-Window $handle 860 165
    Click-Window $handle 795 389

    $trimmer = [IntPtr]::Zero
    for ($i = 0; $i -lt 35; $i++) {
        Start-Sleep -Milliseconds 500
        $trimmer = [WorkbenchCapture]::FindWindow('VideoCompressionWorkbenchTrimmer', $null)
        if ($trimmer -ne [IntPtr]::Zero) { break }
    }
    if ($trimmer -eq [IntPtr]::Zero) { throw 'The Windows trimmer window was not opened.' }
    Save-WindowScreenshot $trimmer (Join-Path $output 'video-compression-workbench-windows-trimmer.png')
    [void] [WorkbenchCapture]::PostMessage($trimmer, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero)
    Start-Sleep -Milliseconds 650

    [void] [WorkbenchCapture]::SetWindowPos($handle, [IntPtr]::Zero, 0, 0, 1280, 720, 0x0004)
    Start-Sleep -Milliseconds 1100
    Save-WindowScreenshot $handle (Join-Path $output 'video-compression-workbench-windows-compact-settings.png')
    Click-Window $handle 970 38
    Save-WindowScreenshot $handle (Join-Path $output 'video-compression-workbench-windows-compact-queue.png')
} finally {
    if (-not $app.HasExited) {
        [void] $app.CloseMainWindow()
        if (-not $app.WaitForExit(3500)) { $app.Kill(); $app.WaitForExit() }
    }
}
