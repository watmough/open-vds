Add-Type -AssemblyName System.Drawing

$srcPath = Join-Path $PSScriptRoot "seismic-vibe.png"
$dstPath = Join-Path $PSScriptRoot "dialog.bmp"

$img = [System.Drawing.Image]::FromFile($srcPath)

# WiX dialog bitmap size: 493 x 312
$bmp = New-Object System.Drawing.Bitmap(493, 312)
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic

# Calculate crop - take left portion of image, scaled to fit height
$srcHeight = $img.Height
$srcWidth = $img.Width
$targetRatio = 493.0 / 312.0
$srcRatio = $srcWidth / $srcHeight

if ($srcRatio -gt $targetRatio) {
    # Source is wider - crop from right (use left portion)
    $useWidth = [int]($srcHeight * $targetRatio)
    $srcRect = New-Object System.Drawing.Rectangle(0, 0, $useWidth, $srcHeight)
} else {
    # Source is taller - use full width
    $useHeight = [int]($srcWidth / $targetRatio)
    $srcRect = New-Object System.Drawing.Rectangle(0, 0, $srcWidth, $useHeight)
}

$dstRect = New-Object System.Drawing.Rectangle(0, 0, 493, 312)
$g.DrawImage($img, $dstRect, $srcRect, [System.Drawing.GraphicsUnit]::Pixel)

$bmp.Save($dstPath, [System.Drawing.Imaging.ImageFormat]::Bmp)

$g.Dispose()
$bmp.Dispose()
$img.Dispose()

Write-Host "Created dialog.bmp (493x312) from seismic-vibe.png"
