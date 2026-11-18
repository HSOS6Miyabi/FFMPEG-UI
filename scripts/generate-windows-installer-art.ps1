param(
    [Parameter(Mandatory = $true)][string]$IconPath,
    [Parameter(Mandatory = $true)][string]$OutputDirectory
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null
$icon = [System.Drawing.Image]::FromFile((Resolve-Path -LiteralPath $IconPath).Path)

function New-InstallerArtwork {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][ValidateSet('Welcome', 'Header')][string]$Kind
    )

    $width = if ($Kind -eq 'Welcome') { 328 } else { 300 }
    $height = if ($Kind -eq 'Welcome') { 628 } else { 114 }
    $bitmap = [System.Drawing.Bitmap]::new(
        $width, $height, [System.Drawing.Imaging.PixelFormat]::Format24bppRgb)
    $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
    $graphics.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
    $graphics.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
    $graphics.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality

    try {
        if ($Kind -eq 'Welcome') {
            $bounds = [System.Drawing.Rectangle]::new(0, 0, $width, $height)
            $top = [System.Drawing.Color]::FromArgb(38, 55, 132)
            $bottom = [System.Drawing.Color]::FromArgb(83, 106, 230)
            $background = [System.Drawing.Drawing2D.LinearGradientBrush]::new(
                $bounds, $top, $bottom, [System.Drawing.Drawing2D.LinearGradientMode]::Vertical)
            try { $graphics.FillRectangle($background, $bounds) }
            finally { $background.Dispose() }

            $outline = [System.Drawing.Pen]::new(
                [System.Drawing.Color]::FromArgb(42, 220, 238, 255), 2.0)
            try {
                $graphics.DrawEllipse($outline, -132, -55, 362, 362)
                $graphics.DrawEllipse($outline, 116, -63, 304, 304)
                $graphics.DrawEllipse($outline, -104, 450, 360, 360)
            } finally { $outline.Dispose() }

            $shadow = [System.Drawing.SolidBrush]::new(
                [System.Drawing.Color]::FromArgb(45, 8, 18, 63))
            try {
                $graphics.FillEllipse($shadow, 75, 132, 186, 188)
                $graphics.FillRectangle($shadow, 54, 364, 220, 82)
            } finally { $shadow.Dispose() }

            $linePen = [System.Drawing.Pen]::new(
                [System.Drawing.Color]::FromArgb(95, 202, 235, 255), 2.0)
            try {
                $graphics.DrawRectangle($linePen, 54, 364, 218, 82)
                $graphics.DrawRectangle($linePen, 70, 381, 105, 46)
                $graphics.DrawRectangle($linePen, 188, 381, 64, 46)
                $graphics.DrawLine($linePen, 89, 402, 119, 402)
                $graphics.DrawLine($linePen, 199, 402, 233, 402)
            } finally { $linePen.Dispose() }

            $dot = [System.Drawing.SolidBrush]::new(
                [System.Drawing.Color]::FromArgb(112, 155, 252, 233))
            try {
                for ($row = 0; $row -lt 3; $row++) {
                    for ($col = 0; $col -lt 3; $col++) {
                        $graphics.FillEllipse($dot, 56 + $col * 18, 490 + $row * 18, 5, 5)
                    }
                }
            } finally { $dot.Dispose() }

            $graphics.DrawImage($icon, [System.Drawing.Rectangle]::new(81, 128, 166, 166))
        } else {
            $graphics.Clear([System.Drawing.Color]::FromArgb(247, 249, 255))
            $stripe = [System.Drawing.SolidBrush]::new(
                [System.Drawing.Color]::FromArgb(70, 88, 109, 232))
            try { $graphics.FillRectangle($stripe, 0, 107, 300, 7) }
            finally { $stripe.Dispose() }
            $shape = [System.Drawing.Point[]]@(
                [System.Drawing.Point]::new(0, 114),
                [System.Drawing.Point]::new(184, 114),
                [System.Drawing.Point]::new(300, 8),
                [System.Drawing.Point]::new(300, 0),
                [System.Drawing.Point]::new(225, 0)
            )
            $accent = [System.Drawing.SolidBrush]::new(
                [System.Drawing.Color]::FromArgb(24, 84, 105, 232))
            try { $graphics.FillPolygon($accent, $shape) }
            finally { $accent.Dispose() }
            $graphics.DrawImage($icon, [System.Drawing.Rectangle]::new(214, 17, 74, 74))
        }
        $bitmap.Save($Path, [System.Drawing.Imaging.ImageFormat]::Bmp)
    } finally {
        $graphics.Dispose()
        $bitmap.Dispose()
    }
}

try {
    New-InstallerArtwork -Path (Join-Path $OutputDirectory 'installer-welcome.bmp') -Kind Welcome
    New-InstallerArtwork -Path (Join-Path $OutputDirectory 'installer-header.bmp') -Kind Header
} finally {
    $icon.Dispose()
}
