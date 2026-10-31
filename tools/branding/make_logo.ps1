# MiniMe Slicer logo generator (SliceOfMe fork).
# Draws the logo with System.Drawing and writes the PNG / ICO files the app loads from resources/images,
# keeping Bambu Studio's file names so no code has to change. SVG versions are written by make_logo_svg.ps1.
#
# Design: a tiny printed figure - three stacked print layers (body), a face on the top layer and a
# nozzle "hat" - on a teal-to-purple rounded square.
#
# Usage (Windows PowerShell 5.1): powershell -ExecutionPolicy Bypass -File tools\branding\make_logo.ps1

Add-Type -AssemblyName System.Drawing
$images = Join-Path $PSScriptRoot '..\..\resources\images' | Resolve-Path

$Teal   = [System.Drawing.ColorTranslator]::FromHtml('#12C2A0')
$Purple = [System.Drawing.ColorTranslator]::FromHtml('#6C3CE0')
$Ink    = [System.Drawing.ColorTranslator]::FromHtml('#262E30')

function New-RoundRect([float]$x, [float]$y, [float]$w, [float]$h, [float]$r) {
    $p = New-Object System.Drawing.Drawing2D.GraphicsPath
    $d = 2 * $r
    $p.AddArc($x, $y, $d, $d, 180, 90)
    $p.AddArc($x + $w - $d, $y, $d, $d, 270, 90)
    $p.AddArc($x + $w - $d, $y + $h - $d, $d, $d, 0, 90)
    $p.AddArc($x, $y + $h - $d, $d, $d, 90, 90)
    $p.CloseFigure()
    return $p
}

# Draws on a 256x256 design grid scaled to $size.
#   -Background: gradient tile with white figure; otherwise gradient figure on transparent.
#   -Gray: grayscale version.
function New-Logo([int]$size, [switch]$Background, [switch]$Gray) {
    $bmp = New-Object System.Drawing.Bitmap $size, $size, ([System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode = 'AntiAlias'
    $g.Clear([System.Drawing.Color]::Transparent)
    $g.ScaleTransform($size / 256.0, $size / 256.0)

    $c1 = $Teal; $c2 = $Purple
    if ($Gray) { $c1 = [System.Drawing.Color]::FromArgb(150,150,150); $c2 = [System.Drawing.Color]::FromArgb(90,90,90) }
    $rect = New-Object System.Drawing.RectangleF 0, 0, 256, 256
    $grad = New-Object System.Drawing.Drawing2D.LinearGradientBrush $rect, $c1, $c2, 45.0

    if ($Background) {
        $g.FillPath($grad, (New-RoundRect 0 0 256 256 56))
        $figure = New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::White)
        $eyes   = New-Object System.Drawing.SolidBrush $Ink
    } else {
        $figure = $grad
        $eyes   = New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::White)
    }

    # nozzle hat
    $g.FillPath($figure, (New-RoundRect 112 40 32 20 6))
    $hat = New-Object System.Drawing.Drawing2D.GraphicsPath
    $hat.AddPolygon([System.Drawing.PointF[]]@(
        (New-Object System.Drawing.PointF 116, 58), (New-Object System.Drawing.PointF 140, 58), (New-Object System.Drawing.PointF 128, 76)))
    $g.FillPath($figure, $hat)
    # head (top layer) with eyes
    $g.FillPath($figure, (New-RoundRect 84 82 88 50 14))
    $g.FillEllipse($eyes, 105, 99, 14, 14)
    $g.FillEllipse($eyes, 137, 99, 14, 14)
    # body layers, widest at the bottom like a print
    $g.FillPath($figure, (New-RoundRect 64 140 128 28 10))
    $g.FillPath($figure, (New-RoundRect 44 176 168 32 11))

    $g.Dispose()
    return $bmp
}

function Save-Png($bmp, $name) {
    $path = Join-Path $images $name
    $bmp.Save($path, [System.Drawing.Imaging.ImageFormat]::Png)
    Write-Output "wrote $name"
}

# ICO with PNG-compressed entries (supported since Windows Vista).
function Save-Ico([int[]]$sizes, $name) {
    $pngs = foreach ($s in $sizes) {
        $ms = New-Object System.IO.MemoryStream
        (New-Logo $s -Background).Save($ms, [System.Drawing.Imaging.ImageFormat]::Png)
        ,$ms.ToArray()
    }
    $out = New-Object System.IO.MemoryStream
    $w = New-Object System.IO.BinaryWriter $out
    $w.Write([UInt16]0); $w.Write([UInt16]1); $w.Write([UInt16]$sizes.Count)
    $offset = 6 + 16 * $sizes.Count
    for ($i = 0; $i -lt $sizes.Count; $i++) {
        $s = $sizes[$i]; $dim = if ($s -ge 256) { 0 } else { $s }
        $w.Write([byte]$dim); $w.Write([byte]$dim); $w.Write([byte]0); $w.Write([byte]0)
        $w.Write([UInt16]1); $w.Write([UInt16]32)
        $w.Write([UInt32]$pngs[$i].Length); $w.Write([UInt32]$offset)
        $offset += $pngs[$i].Length
    }
    foreach ($p in $pngs) { $w.Write($p) }
    $w.Flush()
    [System.IO.File]::WriteAllBytes((Join-Path $images $name), $out.ToArray())
    Write-Output "wrote $name"
}

Save-Png (New-Logo 154 -Background) 'BambuStudio.png'
Save-Png (New-Logo 32 -Background)  'BambuStudio_32px.png'
Save-Png (New-Logo 128 -Background) 'BambuStudio_128px.png'
Save-Png (New-Logo 192 -Background) 'BambuStudio_192px.png'
Save-Png (New-Logo 192 -Background -Gray) 'BambuStudio_192px_grayscale.png'
Save-Png (New-Logo 192) 'BambuStudio_192px_transparent.png'
Save-Ico @(16, 24, 32, 48, 64, 128, 256) 'BambuStudio.ico'
Save-Ico @(16, 24, 32, 48, 64, 128, 256) 'BambuStudioTitle.ico'
