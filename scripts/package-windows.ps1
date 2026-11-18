param(
    [string]$BuildDirectory = "build/windows-release",
    [string]$OutputDirectory = "dist"
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
Set-Location $repoRoot

$buildPath = Join-Path $repoRoot $BuildDirectory
$outputPath = Join-Path $repoRoot $OutputDirectory
$payloadPath = Join-Path $outputPath "windows"
$applicationPath = Join-Path $buildPath "video-compression-workbench.exe"
if (-not (Test-Path -LiteralPath $applicationPath -PathType Leaf)) {
    throw "The Windows application has not been built: $applicationPath"
}

$versionExpression = '(?m)^project\(VideoCompressionWorkbench VERSION (\d+\.\d+\.\d+) LANGUAGES CXX\)'
$versionMatch = [regex]::Match((Get-Content "CMakeLists.txt" -Raw), $versionExpression)
if (-not $versionMatch.Success) {
    throw "Cannot read the product version from CMakeLists.txt"
}
$version = $versionMatch.Groups[1].Value

$ffmpegVersion = "9.0.2"
$archiveName = "ffmpeg-$ffmpegVersion-essentials_build.zip"
$sourceUrl = "https://github.com/GyanD/codexffmpeg/releases/download/$ffmpegVersion/$archiveName"
$expectedSha256 = "60f467265b1e312373dbcd92200c2618a74850f98d3d078e94296bb3fa2047ba"
$archivePath = Join-Path $env:RUNNER_TEMP $archiveName
$extractPath = Join-Path $env:RUNNER_TEMP "video-compression-workbench-ffmpeg"

if (-not (Test-Path -LiteralPath $archivePath -PathType Leaf) -or
    (Get-FileHash -LiteralPath $archivePath -Algorithm SHA256).Hash.ToLowerInvariant() -ne $expectedSha256) {
    Invoke-WebRequest -Uri $sourceUrl -OutFile $archivePath -MaximumRedirection 10
}
$actualSha256 = (Get-FileHash -LiteralPath $archivePath -Algorithm SHA256).Hash.ToLowerInvariant()
if ($actualSha256 -ne $expectedSha256) {
    Remove-Item -LiteralPath $archivePath -Force
    throw "The FFmpeg archive checksum does not match the pinned release."
}

if (Test-Path -LiteralPath $extractPath) {
    Remove-Item -LiteralPath $extractPath -Recurse -Force
}
Expand-Archive -LiteralPath $archivePath -DestinationPath $extractPath -Force

$ffmpeg = Get-ChildItem -LiteralPath $extractPath -Recurse -File -Filter "ffmpeg.exe" |
    Where-Object { Test-Path -LiteralPath (Join-Path $_.Directory.FullName "ffprobe.exe") } |
    Select-Object -First 1
if ($null -eq $ffmpeg) {
    throw "The pinned archive does not contain matching ffmpeg.exe and ffprobe.exe files."
}

$ffmpegRoot = (Resolve-Path (Join-Path $ffmpeg.Directory.FullName "..")).Path
if (Test-Path -LiteralPath $payloadPath) {
    Remove-Item -LiteralPath $payloadPath -Recurse -Force
}
New-Item -ItemType Directory -Force (Join-Path $payloadPath "bin") | Out-Null
New-Item -ItemType Directory -Force (Join-Path $payloadPath "docs") | Out-Null

Copy-Item -LiteralPath $applicationPath -Destination $payloadPath
Copy-Item -LiteralPath (Join-Path $repoRoot "README.md") -Destination $payloadPath
Copy-Item -Path (Join-Path $repoRoot "docs/*") -Destination (Join-Path $payloadPath "docs") -Recurse
Copy-Item -LiteralPath $ffmpeg.FullName -Destination (Join-Path $payloadPath "bin/ffmpeg.exe")
Copy-Item -LiteralPath (Join-Path $ffmpeg.Directory.FullName "ffprobe.exe") -Destination (Join-Path $payloadPath "bin/ffprobe.exe")

$ffmpegNotices = Join-Path $payloadPath "docs/third-party/FFmpeg"
New-Item -ItemType Directory -Force $ffmpegNotices | Out-Null

$licenseFiles = @(Get-ChildItem -LiteralPath $ffmpegRoot -Recurse -File |
    Where-Object { $_.Name -match '^(LICENSE|LICENSE\.txt|COPYING(\.[A-Za-z0-9.-]+)?)$' })
if ($licenseFiles.Count -eq 0) {
    throw "The FFmpeg archive contains no license text. Refusing to package it."
}
foreach ($licenseFile in $licenseFiles) {
    $relative = [System.IO.Path]::GetRelativePath($ffmpegRoot, $licenseFile.FullName)
    $target = Join-Path $ffmpegNotices $relative
    New-Item -ItemType Directory -Force (Split-Path -Parent $target) | Out-Null
    Copy-Item -LiteralPath $licenseFile.FullName -Destination $target
}

$vcpkgShares = Join-Path $buildPath "vcpkg_installed/x64-windows-static/share"
if (Test-Path -LiteralPath $vcpkgShares) {
    $copyrightFiles = @(Get-ChildItem -LiteralPath $vcpkgShares -Recurse -File -Filter "copyright")
    if ($copyrightFiles.Count -eq 0) {
        throw "The vcpkg dependency licenses are missing."
    }
    $vcpkgNotices = Join-Path $payloadPath "docs/third-party/vcpkg"
    New-Item -ItemType Directory -Force $vcpkgNotices | Out-Null
    foreach ($licenseFile in $copyrightFiles) {
        $packageName = Split-Path -Leaf (Split-Path -Parent $licenseFile.FullName)
        Copy-Item -LiteralPath $licenseFile.FullName -Destination (Join-Path $vcpkgNotices "$packageName.txt")
    }
} else {
    throw "Cannot locate licenses for statically linked vcpkg dependencies: $vcpkgShares"
}

$zipPath = Join-Path $outputPath "video-compression-workbench-windows-x86_64.zip"
if (Test-Path -LiteralPath $zipPath) { Remove-Item -LiteralPath $zipPath -Force }
Compress-Archive -Path (Join-Path $payloadPath "*") -DestinationPath $zipPath -CompressionLevel Optimal

$nsis = Join-Path ([Environment]::GetFolderPath("ProgramFilesX86")) "NSIS/makensis.exe"
if (-not (Test-Path -LiteralPath $nsis)) {
    $nsisCommand = Get-Command "makensis.exe" -ErrorAction SilentlyContinue
    if ($null -eq $nsisCommand) { throw "NSIS makensis.exe is not installed." }
    $nsis = $nsisCommand.Source
}

$installerPath = Join-Path $outputPath "video-compression-workbench-$version-windows-x64-setup.exe"
if (Test-Path -LiteralPath $installerPath) {
    Remove-Item -LiteralPath $installerPath -Force
}
& $nsis "/V3" "/DPRODUCT_VERSION=$version" "/DPAYLOAD_DIR=$payloadPath" "/DOUTPUT_FILE=$installerPath" (Join-Path $repoRoot "packaging/windows-installer.nsi")
if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $installerPath -PathType Leaf)) {
    throw "NSIS did not produce an installer."
}

Write-Host "Packaged FFmpeg $ffmpegVersion (SHA-256: $actualSha256)"
Write-Host "Portable package: $zipPath"
Write-Host "NSIS installer: $installerPath"
