param([string]$ResourcesDirectory = (Join-Path $PSScriptRoot '..\resources'))
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
$ResourcesDirectory = (Resolve-Path -LiteralPath $ResourcesDirectory).Path
[xml]$source = Get-Content -LiteralPath (Join-Path $ResourcesDirectory 'island.svg') -Raw
$sizes = @(16, 20, 24, 32, 48, 64, 128, 256)
$frames = [System.Collections.Generic.List[object]]::new()

function Read-Number($Node, [string]$Name) {
    return [float]::Parse($Node.GetAttribute($Name), [Globalization.CultureInfo]::InvariantCulture)
}

function New-RoundedPath([float]$X, [float]$Y, [float]$Width, [float]$Height, [float]$Radius) {
    $path = [Drawing.Drawing2D.GraphicsPath]::new()
    $diameter = 2 * $Radius
    $path.AddArc($X, $Y, $diameter, $diameter, 180, 90)
    $path.AddArc($X + $Width - $diameter, $Y, $diameter, $diameter, 270, 90)
    $path.AddArc($X + $Width - $diameter, $Y + $Height - $diameter, $diameter, $diameter, 0, 90)
    $path.AddArc($X, $Y + $Height - $diameter, $diameter, $diameter, 90, 90)
    $path.CloseFigure()
    return $path
}

foreach ($size in $sizes) {
    $workSize = $size * 8
    $canvas = [Drawing.Bitmap]::new($workSize, $workSize, [Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $graphics = [Drawing.Graphics]::FromImage($canvas)
    $graphics.SmoothingMode = [Drawing.Drawing2D.SmoothingMode]::AntiAlias
    $graphics.PixelOffsetMode = [Drawing.Drawing2D.PixelOffsetMode]::HighQuality
    $graphics.ScaleTransform($workSize / 256.0, $workSize / 256.0)
    try {
        foreach ($shape in $source.svg.rect) {
            $x = Read-Number $shape 'x'
            $y = Read-Number $shape 'y'
            $width = Read-Number $shape 'width'
            $height = Read-Number $shape 'height'
            $path = New-RoundedPath $x $y $width $height (Read-Number $shape 'rx')
            if ($shape.fill -eq 'url(#tile)') {
                $stops = $source.svg.defs.linearGradient.stop
                $brush = [Drawing.Drawing2D.LinearGradientBrush]::new(
                    [Drawing.RectangleF]::new($x, $y, $width, $height),
                    [Drawing.ColorTranslator]::FromHtml($stops[0].GetAttribute('stop-color')),
                    [Drawing.ColorTranslator]::FromHtml($stops[1].GetAttribute('stop-color')), 90.0)
            } else {
                $brush = [Drawing.SolidBrush]::new([Drawing.ColorTranslator]::FromHtml($shape.fill))
            }
            try {
                $graphics.FillPath($brush, $path)
                if ($shape.HasAttribute('stroke')) {
                    $pen = [Drawing.Pen]::new([Drawing.ColorTranslator]::FromHtml($shape.stroke),
                        (Read-Number $shape 'stroke-width'))
                    try { $graphics.DrawPath($pen, $path) } finally { $pen.Dispose() }
                }
            } finally { $brush.Dispose(); $path.Dispose() }
        }
        $image = [Drawing.Bitmap]::new($size, $size, [Drawing.Imaging.PixelFormat]::Format32bppArgb)
        $resizer = [Drawing.Graphics]::FromImage($image)
        $resizer.CompositingMode = [Drawing.Drawing2D.CompositingMode]::SourceCopy
        $resizer.InterpolationMode = [Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
        $resizer.PixelOffsetMode = [Drawing.Drawing2D.PixelOffsetMode]::HighQuality
        try {
            $resizer.DrawImage($canvas, [Drawing.Rectangle]::new(0, 0, $size, $size),
                0, 0, $workSize, $workSize, [Drawing.GraphicsUnit]::Pixel)
            $stream = [IO.MemoryStream]::new()
            try {
                $image.Save($stream, [Drawing.Imaging.ImageFormat]::Png)
                $frames.Add(@{ Size = $size; Bytes = $stream.ToArray() })
            } finally { $stream.Dispose() }
            if ($size -eq 256) { $image.Save((Join-Path $ResourcesDirectory 'island.png'), [Drawing.Imaging.ImageFormat]::Png) }
        } finally { $resizer.Dispose(); $image.Dispose() }
    } finally { $graphics.Dispose(); $canvas.Dispose() }
}

$output = [IO.File]::Create((Join-Path $ResourcesDirectory 'island.ico'))
$writer = [IO.BinaryWriter]::new($output)
try {
    $writer.Write([uint16]0)
    $writer.Write([uint16]1)
    $writer.Write([uint16]$frames.Count)
    $offset = 6 + 16 * $frames.Count
    foreach ($frame in $frames) {
        $dimension = if ($frame.Size -eq 256) { 0 } else { $frame.Size }
        $writer.Write([byte]$dimension)
        $writer.Write([byte]$dimension)
        $writer.Write([byte]0)
        $writer.Write([byte]0)
        $writer.Write([uint16]1)
        $writer.Write([uint16]32)
        $writer.Write([uint32]$frame.Bytes.Length)
        $writer.Write([uint32]$offset)
        $offset += $frame.Bytes.Length
    }
    foreach ($frame in $frames) { $writer.Write([byte[]]$frame.Bytes) }
} finally { $writer.Dispose(); $output.Dispose() }
Write-Output "Rendered island.ico with sizes $($sizes -join ', ') and island.png from island.svg."
