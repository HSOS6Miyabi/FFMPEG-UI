param(
    [string]$InstallerPath
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
Set-Location $repoRoot

$versionMatch = [regex]::Match(
    (Get-Content "CMakeLists.txt" -Raw),
    '(?m)^project\(VideoCompressionWorkbench VERSION (\d+\.\d+\.\d+) LANGUAGES CXX\)')
if (-not $versionMatch.Success) { throw "Cannot read the application version." }
$version = $versionMatch.Groups[1].Value
if ([string]::IsNullOrEmpty($InstallerPath)) {
    $InstallerPath = Join-Path $repoRoot "dist/video-compression-workbench-$version-windows-x64-setup.exe"
}
$InstallerPath = (Resolve-Path -LiteralPath $InstallerPath).Path
$originalApp = Join-Path $repoRoot "build/windows-release/video-compression-workbench.exe"
$installDir = Join-Path $env:RUNNER_TEMP "video-workbench-install-test"
$registryPath = "HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\VideoCompressionWorkbench"

function Assert-Equal([object]$Actual, [object]$Expected, [string]$Description) {
    if ($Actual -ne $Expected) {
        throw "$Description : expected '$Expected', got '$Actual'"
    }
}

function Assert-Metadata([string]$Path, [string]$ExpectedDescription) {
    $info = [System.Diagnostics.FileVersionInfo]::GetVersionInfo($Path)
    Assert-Equal $info.CompanyName "Kenvix" "CompanyName in $Path"
    Assert-Equal $info.ProductName "视频压缩工作台" "ProductName in $Path"
    Assert-Equal $info.FileDescription $ExpectedDescription "FileDescription in $Path"
    Assert-Equal $info.ProductVersion $version "ProductVersion in $Path"
    if ($info.LegalCopyright -notmatch 'Kenvix.*i@kenvix\.com') {
        throw "Missing author and copyright contact in $Path"
    }
    if ($info.Comments -notmatch 'Kenvix.*i@kenvix\.com') {
        throw "Missing author email in $Path"
    }
    Write-Host "Verified version metadata: $Path"
}

Add-Type -AssemblyName System.Drawing
Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class InstallerIconResource {
    [DllImport("shell32.dll", CharSet = CharSet.Unicode, EntryPoint = "ExtractIconExW")]
    public static extern uint ExtractIconEx(string file, int iconIndex,
        IntPtr[] largeIcons, IntPtr[] smallIcons, uint count);
}
'@

function Assert-GuiSubsystem([string]$Path) {
    $bytes = [System.IO.File]::ReadAllBytes($Path)
    $peOffset = [System.BitConverter]::ToInt32($bytes, 0x3c)
    $signature = [System.BitConverter]::ToUInt32($bytes, $peOffset)
    Assert-Equal $signature 0x00004550 "Portable executable signature"
    $subsystem = [System.BitConverter]::ToUInt16($bytes, $peOffset + 24 + 68)
    Assert-Equal $subsystem 2 "Windows graphical subsystem"
}

function Assert-EmbeddedIcon([string]$Path, [string]$ExpectedIconPath) {
    $iconCount = [InstallerIconResource]::ExtractIconEx($Path, -1, $null, $null, 0)
    if ($iconCount -lt 1) { throw "The executable has no embedded icon: $Path" }

    $expected = [System.Drawing.Icon]::new($ExpectedIconPath)
    $actual = [System.Drawing.Icon]::ExtractAssociatedIcon($Path)
    if ($null -eq $actual) { throw "Cannot extract an executable icon: $Path" }
    try {
        $expectedBitmap = $expected.ToBitmap()
        $actualBitmap = $actual.ToBitmap()
        $expectedScaled = [System.Drawing.Bitmap]::new(32, 32)
        $actualScaled = [System.Drawing.Bitmap]::new(32, 32)
        try {
            foreach ($pair in @(@($expectedBitmap, $expectedScaled), @($actualBitmap, $actualScaled))) {
                $g = [System.Drawing.Graphics]::FromImage($pair[1])
                try {
                    $g.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
                    $g.DrawImage($pair[0], 0, 0, 32, 32)
                } finally { $g.Dispose() }
            }
            $difference = 0.0
            $samples = 0
            for ($y = 6; $y -le 26; $y += 2) {
                for ($x = 6; $x -le 26; $x += 2) {
                    $a = $actualScaled.GetPixel($x, $y)
                    $b = $expectedScaled.GetPixel($x, $y)
                    $difference += [Math]::Abs($a.R - $b.R) +
                                   [Math]::Abs($a.G - $b.G) +
                                   [Math]::Abs($a.B - $b.B)
                    $samples++
                }
            }
            if ($difference / ($samples * 3) -gt 45) {
                throw "The icon in $Path differs from the application artwork."
            }
        } finally {
            $expectedBitmap.Dispose()
            $actualBitmap.Dispose()
            $expectedScaled.Dispose()
            $actualScaled.Dispose()
        }
    } finally {
        $expected.Dispose()
        $actual.Dispose()
    }
}

function Invoke-WorkbenchCommand([string]$Application, [string[]]$Arguments) {
    $output = Join-Path $env:RUNNER_TEMP ("workbench-out-" + [guid]::NewGuid().ToString() + ".txt")
    $errors = Join-Path $env:RUNNER_TEMP ("workbench-err-" + [guid]::NewGuid().ToString() + ".txt")
    try {
        $process = Start-Process -FilePath $Application -ArgumentList $Arguments -PassThru -Wait -RedirectStandardOutput $output -RedirectStandardError $errors
        $text = if (Test-Path -LiteralPath $output) {
            Get-Content -LiteralPath $output -Raw -Encoding UTF8
        } else { '' }
        $errorText = if (Test-Path -LiteralPath $errors) {
            Get-Content -LiteralPath $errors -Raw -Encoding UTF8
        } else { '' }
        if ($process.ExitCode -ne 0) {
            throw "Command $($Arguments -join ' ') failed with exit code $($process.ExitCode): $text $errorText"
        }
        Write-Host $text
        return $text
    } finally {
        Remove-Item -LiteralPath $output, $errors -Force -ErrorAction SilentlyContinue
    }
}

Assert-Metadata $originalApp "视频压缩工作台"
Assert-Metadata $InstallerPath "视频压缩工作台安装程序"
Assert-GuiSubsystem $originalApp
$expectedIcon = Join-Path $repoRoot "assets/icons/video-compression-workbench.ico"
Assert-EmbeddedIcon $originalApp $expectedIcon
Assert-EmbeddedIcon $InstallerPath $expectedIcon

$welcomeBitmap = Join-Path $repoRoot "dist/installer-art/installer-welcome.bmp"
$headerBitmap = Join-Path $repoRoot "dist/installer-art/installer-header.bmp"
foreach ($artwork in @(@($welcomeBitmap, 328, 628), @($headerBitmap, 300, 114))) {
    $bitmap = [System.Drawing.Bitmap]::new([string]$artwork[0])
    try {
        Assert-Equal $bitmap.Width $artwork[1] "Installer artwork width"
        Assert-Equal $bitmap.Height $artwork[2] "Installer artwork height"
    } finally { $bitmap.Dispose() }
}


if (Test-Path -LiteralPath $installDir) {
    Remove-Item -LiteralPath $installDir -Recurse -Force
}
$process = Start-Process -FilePath $InstallerPath -ArgumentList @("/S", "/D=$installDir") -Wait -PassThru
if ($process.ExitCode -ne 0) {
    throw "Silent installation failed with exit code $($process.ExitCode)"
}
$installedApp = Join-Path $installDir "video-compression-workbench.exe"
$installedFfmpeg = Join-Path $installDir "bin/ffmpeg.exe"
$installedFfprobe = Join-Path $installDir "bin/ffprobe.exe"
$uninstaller = Join-Path $installDir "Uninstall.exe"
foreach ($required in @($installedApp, $installedFfmpeg, $installedFfprobe, $uninstaller)) {
    if (-not (Test-Path -LiteralPath $required -PathType Leaf)) {
        throw "Missing installed file: $required"
    }
}
$registry = Get-ItemProperty -LiteralPath $registryPath -ErrorAction Stop
Assert-Equal $registry.Publisher "Kenvix" "Installed publisher"
Assert-Equal $registry.DisplayVersion $version "Installed application version"
Assert-Equal $registry.Contact "i@kenvix.com" "Installed support contact"

$expectedFfmpegVersion = "9.0.2"
$ffmpegOutput = & $installedFfmpeg -hide_banner -version | Out-String
if ($LASTEXITCODE -ne 0 -or $ffmpegOutput -notmatch "ffmpeg version $expectedFfmpegVersion") {
    throw "Bundled FFmpeg cannot start or its version is unexpected."
}
$ffprobeOutput = & $installedFfprobe -hide_banner -version | Out-String
if ($LASTEXITCODE -ne 0 -or $ffprobeOutput -notmatch "ffprobe version $expectedFfmpegVersion") {
    throw "Bundled ffprobe cannot start or its version is unexpected."
}

$originalPath = $env:PATH
$originalConfig = $env:FFMPEG_STUDIO_CONFIG_DIR
$originalFfmpeg = $env:FFMPEG_PATH
$originalFfprobe = $env:FFPROBE_PATH
try {
    $env:PATH = "$env:SystemRoot\System32;$env:SystemRoot"
    $env:FFMPEG_STUDIO_CONFIG_DIR = Join-Path $env:RUNNER_TEMP "video-workbench-install-test-config"
    Remove-Item Env:FFMPEG_PATH -ErrorAction SilentlyContinue
    Remove-Item Env:FFPROBE_PATH -ErrorAction SilentlyContinue
    $encoders = Invoke-WorkbenchCommand $installedApp @("--encoders")
    if ($encoders -notmatch "ffmpeg version $expectedFfmpegVersion") {
        throw "The installed application did not locate its bundled FFmpeg binaries."
    }
    Write-Host "Verified bundled FFmpeg discovery with the system FFmpeg path removed."
    $testOutput = Invoke-WorkbenchCommand $installedApp @("--self-test")
    if ($testOutput -notmatch 'Self-test result: d+ passed, 0 failed') {
        throw "Installed application self-tests did not report success."
    }
} finally {
    $env:PATH = $originalPath
    $env:FFMPEG_STUDIO_CONFIG_DIR = $originalConfig
    if ($null -eq $originalFfmpeg) { Remove-Item Env:FFMPEG_PATH -ErrorAction SilentlyContinue }
    else { $env:FFMPEG_PATH = $originalFfmpeg }
    if ($null -eq $originalFfprobe) { Remove-Item Env:FFPROBE_PATH -ErrorAction SilentlyContinue }
    else { $env:FFPROBE_PATH = $originalFfprobe }
}

$uninstallProcess = Start-Process -FilePath $uninstaller -ArgumentList @("/S") -Wait -PassThru
if ($uninstallProcess.ExitCode -ne 0) {
    throw "Silent uninstallation failed with exit code $($uninstallProcess.ExitCode)"
}
if (Test-Path -LiteralPath $installedApp -PathType Leaf) {
    throw "Uninstallation left the application executable behind."
}
if (Test-Path -LiteralPath $registryPath) {
    throw "Uninstallation left the application registry entry behind."
}

Write-Host "Windows installer installation, bundled tools, metadata, media tests and uninstall succeeded."
