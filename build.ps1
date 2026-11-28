#Requires -Version 5.1
$ErrorActionPreference = 'Stop'

$RootDir = if ($PSScriptRoot) { $PSScriptRoot } else { (Get-Location).Path }
$SrcDir  = Join-Path $RootDir 'src'
$IcoPath = Join-Path $SrcDir 'app.ico'
$RcPath  = Join-Path $SrcDir 'resource.rc'
$ResPath = Join-Path $SrcDir 'resource.res'
$CppPath = Join-Path $SrcDir 'main.cpp'
$Wv2Lib  = Join-Path $SrcDir 'webview2\WebView2LoaderStatic.lib'
$OutExe  = Join-Path $RootDir 'Adobe Tools.exe'

# 1. Generate multi-resolution high-DPI app.ico (16, 32, 48, 256) from docs/logo.png (with procedural fallback)
Add-Type -AssemblyName System.Drawing
$LogoPng = Join-Path $RootDir 'docs\logo.png'

function New-IconBitmap([int]$Size) {
    $bmp = New-Object System.Drawing.Bitmap($Size, $Size, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode     = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
    $g.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
    $g.PixelOffsetMode   = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
    $g.CompositingQuality = [System.Drawing.Drawing2D.CompositingQuality]::HighQuality
    $g.Clear([System.Drawing.Color]::Transparent)

    if (Test-Path -LiteralPath $LogoPng) {
        $src = [System.Drawing.Image]::FromFile($LogoPng)
        try {
            $g.DrawImage($src, 0, 0, $Size, $Size)
        } finally {
            $src.Dispose()
        }
        $g.Dispose()
        return $bmp
    }

    $pad = [float]($Size * 0.06)
    $w   = [float]($Size - $pad * 2)
    $r   = [float]($Size * 0.24)

    $path = New-Object System.Drawing.Drawing2D.GraphicsPath
    $d = $r * 2
    $path.AddArc($pad, $pad, $d, $d, 180, 90)
    $path.AddArc($pad + $w - $d, $pad, $d, $d, 270, 90)
    $path.AddArc($pad + $w - $d, $pad + $w - $d, $d, $d, 0, 90)
    $path.AddArc($pad, $pad + $w - $d, $d, $d, 90, 90)
    $path.CloseFigure()

    $rect = New-Object System.Drawing.RectangleF($pad, $pad, $w, $w)
    $bgBrush = New-Object System.Drawing.Drawing2D.LinearGradientBrush(
        $rect,
        [System.Drawing.Color]::FromArgb(255, 35, 35, 37),
        [System.Drawing.Color]::FromArgb(255, 15, 15, 16),
        45.0
    )
    $g.FillPath($bgBrush, $path)

    $borderPen = New-Object System.Drawing.Pen([System.Drawing.Color]::FromArgb(100, 255, 255, 255), [Math]::Max(1.0, $Size * 0.03))
    $g.DrawPath($borderPen, $path)

    $cx = $Size / 2.0
    $cy = $Size / 2.0
    $ringR = $Size * 0.28
    $penW  = [Math]::Max(1.8, $Size * 0.085)
    $whitePen = New-Object System.Drawing.Pen([System.Drawing.Color]::White, $penW)
    $whitePen.StartCap = [System.Drawing.Drawing2D.LineCap]::Round
    $whitePen.EndCap   = [System.Drawing.Drawing2D.LineCap]::Round

    $g.DrawEllipse($whitePen, [float]($cx - $ringR), [float]($cy - $ringR), [float]($ringR * 2), [float]($ringR * 2))
    $slashOffset = $ringR * 0.7071
    $g.DrawLine($whitePen, [float]($cx - $slashOffset), [float]($cy - $slashOffset), [float]($cx + $slashOffset), [float]($cy + $slashOffset))

    $whitePen.Dispose()
    $borderPen.Dispose()
    $bgBrush.Dispose()
    $path.Dispose()
    $g.Dispose()
    return $bmp
}

$sizes = @(16, 32, 48, 256)
$pngBuffers = New-Object 'System.Collections.Generic.List[byte[]]'
foreach ($s in $sizes) {
    $bmp = New-IconBitmap -Size $s
    $ms = New-Object System.IO.MemoryStream
    $bmp.Save($ms, [System.Drawing.Imaging.ImageFormat]::Png)
    $pngBuffers.Add($ms.ToArray())
    $ms.Dispose()
    $bmp.Dispose()
}

$icoStream = New-Object System.IO.MemoryStream
$bw = New-Object System.IO.BinaryWriter($icoStream)
$bw.Write([uint16]0)
$bw.Write([uint16]1)
$bw.Write([uint16]$sizes.Count)

$offset = 6 + ($sizes.Count * 16)
for ($i = 0; $i -lt $sizes.Count; $i++) {
    $s = $sizes[$i]
    $buf = $pngBuffers[$i]
    $dimByte = if ($s -ge 256) { [byte]0 } else { [byte]$s }
    $bw.Write($dimByte)
    $bw.Write($dimByte)
    $bw.Write([byte]0)
    $bw.Write([byte]0)
    $bw.Write([uint16]1)
    $bw.Write([uint16]32)
    $bw.Write([uint32]$buf.Length)
    $bw.Write([uint32]$offset)
    $offset += $buf.Length
}
foreach ($buf in $pngBuffers) { $bw.Write($buf) }
[System.IO.File]::WriteAllBytes($IcoPath, $icoStream.ToArray())
$bw.Dispose()
$icoStream.Dispose()
Write-Host "[1/3] Generated multi-res icon: $IcoPath" -ForegroundColor Cyan

# 2. Locate MSVC 2022 (BuildTools / Community / Professional / Enterprise) & Windows 10/11 SDK
$msvcDir = $null
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (Test-Path $vswhere) {
    $vsInstall = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath 2>$null
    if ($vsInstall) {
        $cand = Join-Path $vsInstall "VC\Tools\MSVC"
        if (Test-Path $cand) {
            $msvcDir = (Get-ChildItem $cand -Directory | Sort-Object Name -Descending | Select-Object -First 1).FullName
        }
    }
}
if (-not $msvcDir) {
    $vsRoots = @(
        "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Tools\MSVC",
        "C:\Program Files\Microsoft Visual Studio\2022\Enterprise\VC\Tools\MSVC",
        "C:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Tools\MSVC",
        "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\MSVC",
        "C:\Program Files\Microsoft Visual Studio\2022\BuildTools\VC\Tools\MSVC"
    )
    foreach ($root in $vsRoots) {
        if (Test-Path $root) {
            $msvcDir = (Get-ChildItem $root -Directory | Sort-Object Name -Descending | Select-Object -First 1).FullName
            if ($msvcDir) { break }
        }
    }
}
if (-not $msvcDir) { throw "Could not locate MSVC x64 toolchain (Visual Studio 2022 BuildTools/Community/Enterprise)." }

$sdkRoot    = "C:\Program Files (x86)\Windows Kits\10"
$sdkVer     = (Get-ChildItem "$sdkRoot\Include" -Directory | Sort-Object Name -Descending | Select-Object -First 1).Name

$clExe = Join-Path $msvcDir "bin\Hostx64\x64\cl.exe"
$rcExe = Join-Path $sdkRoot "bin\$sdkVer\x64\rc.exe"

$env:PATH = "$msvcDir\bin\Hostx64\x64;$sdkRoot\bin\$sdkVer\x64;$env:PATH"
$env:INCLUDE = @(
    "$msvcDir\include",
    "$sdkRoot\Include\$sdkVer\ucrt",
    "$sdkRoot\Include\$sdkVer\shared",
    "$sdkRoot\Include\$sdkVer\um",
    "$sdkRoot\Include\$sdkVer\winrt"
) -join ";"
$env:LIB = @(
    "$msvcDir\lib\x64",
    "$sdkRoot\Lib\$sdkVer\ucrt\x64",
    "$sdkRoot\Lib\$sdkVer\um\x64"
) -join ";"

Write-Host "[2/3] Compiling C++17 + WebView2 HTML5 UI with MSVC ($sdkVer)..." -ForegroundColor Cyan
Push-Location $SrcDir
try {
    & $rcExe /nologo /fo "resource.res" "resource.rc"
    if ($LASTEXITCODE -ne 0) { throw "rc.exe failed with exit code $LASTEXITCODE" }

    & $clExe /nologo /std:c++17 /EHsc /O2 /MT /utf-8 "main.cpp" "/Fe:..\Adobe Tools.exe" /link /SUBSYSTEM:WINDOWS "resource.res" "webview2\WebView2LoaderStatic.lib"
    if ($LASTEXITCODE -ne 0) { throw "cl.exe failed with exit code $LASTEXITCODE" }
} finally {
    Remove-Item "main.obj", "resource.res" -Force -ErrorAction SilentlyContinue
    Pop-Location
}

Write-Host "[3/3] Built standalone C++/HTML5 executable: $OutExe ($((Get-Item $OutExe).Length) bytes)" -ForegroundColor Green
