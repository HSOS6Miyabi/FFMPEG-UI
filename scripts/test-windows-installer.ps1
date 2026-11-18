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

Assert-Metadata $originalApp "视频压缩工作台"
Assert-Metadata $InstallerPath "视频压缩工作台安装程序"

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
    $encoders = & $installedApp --encoders | Out-String
    if ($LASTEXITCODE -ne 0 -or $encoders -notmatch "ffmpeg version $expectedFfmpegVersion") {
        throw "The installed application did not locate its bundled FFmpeg binaries."
    }
    Write-Host "Verified bundled FFmpeg discovery with the system FFmpeg path removed."
    & $installedApp --self-test
    if ($LASTEXITCODE -ne 0) {
        throw "Installed application self-tests failed."
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
