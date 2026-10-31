# Build the single-file exe: append a custom payload container to the exe tail.
# Layout: [exe][payload][u32 payload_offset][8B "SFSHANEND"]
# payload: [u32 magic 'SFSH'][u32 count][ (u16 nameLen, u32 dataLen, name, data)... ]
# NOTE: keep this file ASCII-only (PS 5.1 reads BOM-less files as ANSI).
$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot
$build = Join-Path $root 'build'
$dist = Join-Path $root 'dist'
$exeSrc = Join-Path $build 'SFSChineseInjector.exe'
if (-not (Test-Path $exeSrc)) { $exeSrc = Join-Path $build 'sfs_han_cli.exe' }
$exeOut = Join-Path $dist 'SFSChineseInjector.exe'

if (-not (Test-Path $exeSrc)) { throw "missing $exeSrc" }
if (-not (Test-Path $dist)) { New-Item -ItemType Directory -Path $dist | Out-Null }

$payload = @(
    @{ name = 'inject/SFSHanBootstrap.dll'; path = (Join-Path $root 'data\inject\SFSHanBootstrap.dll') },
    @{ name = 'runtime/SFSHanRuntime.dll'; path = (Join-Path $root 'data\runtime\SFSHanRuntime.dll') },
    @{ name = 'fonts/HarmonyOS_Sans_SC_Regular.ttf'; path = (Join-Path $root 'data\fonts\HarmonyOS_Sans_SC_Regular.ttf') },
    @{ name = 'dict.json'; path = (Join-Path $root 'data\dict.json') },
    @{ name = 'translations/zh-CN.txt'; path = (Join-Path $root 'data\translations\zh-CN.txt') }
)
foreach ($p in $payload) {
    if (-not (Test-Path $p.path)) { throw "missing payload file: $($p.path)" }
}

$exeBytes = [System.IO.File]::ReadAllBytes($exeSrc)
$ms = New-Object System.IO.MemoryStream
$bw = New-Object System.IO.BinaryWriter($ms)

$bw.Write([uint32]0x48534653)             # magic 'SFSH'
$bw.Write([uint32]$payload.Count)
foreach ($p in $payload) {
    $nameBytes = [System.Text.Encoding]::UTF8.GetBytes($p.name)
    $dataBytes = [System.IO.File]::ReadAllBytes($p.path)
    $bw.Write([uint16]$nameBytes.Length)
    $bw.Write([uint32]$dataBytes.Length)
    $bw.Write($nameBytes)
    $bw.Write($dataBytes)
    Write-Host ("  + {0,-42} {1,10:N0} B" -f $p.name, $dataBytes.Length)
}
$payloadBytes = $ms.ToArray()
$bw.Dispose()
$ms.Dispose()

$start = [uint32]$exeBytes.Length
$fs = [System.IO.File]::Open($exeOut, [System.IO.FileMode]::Create)
$fs.Write($exeBytes, 0, $exeBytes.Length)
$fs.Write($payloadBytes, 0, $payloadBytes.Length)
$off = [BitConverter]::GetBytes($start)
$fs.Write($off, 0, $off.Length)
$end = [System.Text.Encoding]::ASCII.GetBytes('SFSHANEN')
$fs.Write($end, 0, $end.Length)
$fs.Dispose()

$size = (Get-Item $exeOut).Length
Write-Host ("`nsingle-file exe: {0}  ({1:N1} MB)" -f $exeOut, ($size / 1MB))
