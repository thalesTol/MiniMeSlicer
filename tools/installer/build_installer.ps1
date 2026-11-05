# MiniMe Slicer: builds the Windows installer (dist\MiniMeSlicer-Setup-<version>.exe) from the installed program.
# Needs: Inno Setup 6 (winget install JRSoftware.InnoSetup) and Visual Studio 2022 (for the C++ runtime DLLs).
# Run after building and installing: cmake --install build --config RelWithDebInfo
#
# Usage (Windows PowerShell): powershell -ExecutionPolicy Bypass -File tools\installer\build_installer.ps1

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
$root   = Join-Path $PSScriptRoot '..\..' | Resolve-Path
$source = Join-Path $root 'install_dir'
$art    = Join-Path $root 'tools\branding\art'
$work   = Join-Path $root 'build\installer'
$out    = Join-Path $root 'dist'
New-Item -ItemType Directory -Force $work, $out | Out-Null

if (-not (Test-Path (Join-Path $source 'minime-slicer.exe'))) { throw "No installed program in $source; run cmake --install first" }

$iscc = @("$env:LOCALAPPDATA\Programs\Inno Setup 6\ISCC.exe", "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe", "$env:ProgramFiles\Inno Setup 6\ISCC.exe") |
        Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $iscc) { throw 'Inno Setup 6 is needed: winget install JRSoftware.InnoSetup' }

# the newest Visual C++ runtime that comes with Visual Studio
$crt = Get-ChildItem 'C:\Program Files\Microsoft Visual Studio\*\*\VC\Redist\MSVC\*\x64\Microsoft.VC14*.CRT' -Directory -ErrorAction SilentlyContinue |
       Sort-Object { [version]($_.Parent.Parent.Name -replace '[^0-9.]', '') } -ErrorAction SilentlyContinue | Select-Object -Last 1
if (-not $crt) { throw 'Visual C++ runtime DLLs not found (Visual Studio 2022, VC\Redist\MSVC)' }

# version from version.inc: 02.08.04.57 -> 2.8.4.57
$m = Select-String -Path (Join-Path $root 'version.inc') -Pattern 'SLIC3R_VERSION "([0-9.]+)"' | Select-Object -First 1
$version = (($m.Matches[0].Groups[1].Value -split '\.') | ForEach-Object { [int]$_ }) -join '.'

# wizard pictures: the poster on the side, the icon in the header (24-bit BMP, at 100 % and 200 %)
function Save-Bmp([int]$w, [int]$h, [scriptblock]$draw, [string]$name) {
    $bmp = New-Object Drawing.Bitmap $w, $h, ([Drawing.Imaging.PixelFormat]::Format24bppRgb)
    $g = [Drawing.Graphics]::FromImage($bmp)
    $g.InterpolationMode = 'HighQualityBicubic'; $g.SmoothingMode = 'AntiAlias'; $g.PixelOffsetMode = 'HighQuality'
    & $draw $g $w $h
    $g.Dispose()
    $bmp.Save((Join-Path $work $name), [Drawing.Imaging.ImageFormat]::Bmp)
}
$poster = [Drawing.Image]::FromFile((Join-Path $art 'banner.png'))
$icon   = [Drawing.Image]::FromFile((Join-Path $art 'icon_256.png'))
$side = {
    param($g, $w, $h)
    # fill the height; the poster is a little wider than the wizard's side picture, so its sides are cut evenly
    $sw = $poster.Width * $h / $poster.Height
    $g.Clear([Drawing.ColorTranslator]::FromHtml('#080D17'))
    $g.DrawImage($poster, [single](($w - $sw) / 2), [single]0, [single]$sw, [single]$h)
}
$small = {
    param($g, $w, $h)
    $g.Clear([Drawing.Color]::White)
    $g.DrawImage($icon, 0, 0, $w, $h)
}
Save-Bmp 164 314 $side 'wizard_164.bmp'
Save-Bmp 328 628 $side 'wizard_328.bmp'
Save-Bmp 55 55 $small 'small_55.bmp'
Save-Bmp 110 110 $small 'small_110.bmp'
$poster.Dispose(); $icon.Dispose()

Write-Output "MiniMe Slicer $version from $source, runtime from $($crt.FullName)"
& $iscc /Qp "/DAppVersion=$version" "/DSourceDir=$source" "/DCrtDir=$($crt.FullName)" "/DImagesDir=$work" "/DOutputDir=$out" (Join-Path $PSScriptRoot 'minime.iss')
if ($LASTEXITCODE -ne 0) { throw "Inno Setup failed ($LASTEXITCODE)" }
Get-Item (Join-Path $out "MiniMeSlicer-Setup-$version.exe") | Select-Object FullName, @{ n = 'MB'; e = { [int]($_.Length / 1MB) } }
