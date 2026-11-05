# Builds windows/resources/vrscreen.ico from the Mac app icon (mac/Resources/AppIcon.icns), so
# both apps share one icon. The .icns PNG chunks (32, 64, 128, 256 px) become PNG entries of the
# .ico (supported since Windows Vista).
#   powershell -ExecutionPolicy Bypass -File windows\scripts\make-ico.ps1
$ErrorActionPreference = 'Stop'
$root = Resolve-Path (Join-Path $PSScriptRoot '..\..')
$icns = [IO.File]::ReadAllBytes((Join-Path $root 'mac\Resources\AppIcon.icns'))
$wanted = @{ 'ic11' = 32; 'ic12' = 64; 'ic07' = 128; 'ic08' = 256 }
$images = @{}
$i = 8
while ($i -lt $icns.Length) {
    $type = [Text.Encoding]::ASCII.GetString($icns, $i, 4)
    $len = ([int]$icns[$i + 4] -shl 24) -bor ([int]$icns[$i + 5] -shl 16) -bor ([int]$icns[$i + 6] -shl 8) -bor [int]$icns[$i + 7]
    if ($wanted.ContainsKey($type) -and $icns[$i + 8] -eq 0x89) {
        $png = New-Object byte[] ($len - 8)
        [Array]::Copy($icns, $i + 8, $png, 0, $len - 8)
        $images[$wanted[$type]] = $png
    }
    $i += $len
}
$sizes = $images.Keys | Sort-Object
$out = New-Object IO.MemoryStream
$w = New-Object IO.BinaryWriter $out
$w.Write([UInt16]0); $w.Write([UInt16]1); $w.Write([UInt16]$sizes.Count)
$offset = 6 + 16 * $sizes.Count
foreach ($s in $sizes) {
    $b = if ($s -ge 256) { 0 } else { $s }
    $w.Write([byte]$b); $w.Write([byte]$b); $w.Write([byte]0); $w.Write([byte]0)
    $w.Write([UInt16]1); $w.Write([UInt16]32)
    $w.Write([UInt32]$images[$s].Length); $w.Write([UInt32]$offset)
    $offset += $images[$s].Length
}
foreach ($s in $sizes) { $w.Write($images[$s]) }
$dest = Join-Path $root 'windows\resources\vrscreen.ico'
New-Item -ItemType Directory -Force (Split-Path $dest) | Out-Null
[IO.File]::WriteAllBytes($dest, $out.ToArray())
"wrote $dest ($($sizes -join ', ') px)"
