# Package a release: build -> dist\SFSHanInjector-<ver>\ + zip
# Usage (from repo root): powershell -File tools\package.ps1
# NOTE: keep this file ASCII-only. Windows PowerShell 5.1 reads .ps1 as ANSI
#       when there is no BOM, which breaks non-ASCII literals.
# The doc files are read as UTF-8 explicitly and rewritten with a BOM
# (so Notepad shows Chinese correctly) - never decoded through the ANSI code page.

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$ver = '0.1.0'
$name = "SFSHanInjector-$ver"
$dist = Join-Path $root 'dist'
$out = Join-Path $dist $name

Write-Host "== build =="
cmake -S $root -B (Join-Path $root 'build') -G Ninja -DCMAKE_BUILD_TYPE=Release | Out-Null
cmake --build (Join-Path $root 'build') | Out-Null
$exe = Join-Path $root 'build\SFSHanInjector.exe'
if (-not (Test-Path $exe)) { throw "build failed: $exe missing" }

Write-Host "== assemble $out =="
if (Test-Path $out) { Remove-Item $out -Recurse -Force }
New-Item -ItemType Directory -Force -Path $out | Out-Null
Copy-Item $exe (Join-Path $out 'SFSHanInjector.exe')
Copy-Item (Join-Path $root 'data') (Join-Path $out 'data') -Recurse

Write-Host "== convert docs to UTF-8 with BOM =="
$enc = New-Object System.Text.UTF8Encoding($true)
foreach ($f in @(([char]0x5B89 + [char]0x88C5 + [char]0x8BF4 + [char]0x660E + '.txt'), 'README.md')) {
    $src = Join-Path $root $f
    if (Test-Path -LiteralPath $src) {
        $text = [System.IO.File]::ReadAllText($src, [System.Text.Encoding]::UTF8)
        [System.IO.File]::WriteAllText((Join-Path $out $f), $text, $enc)
        Write-Host ("  {0} ({1} chars)" -f $f, $text.Length)
    }
}

Write-Host "== zip =="
$zip = Join-Path $dist "$name.zip"
if (Test-Path $zip) { Remove-Item $zip -Force }
Compress-Archive -Path $out -DestinationPath $zip -CompressionLevel Optimal

Write-Host "== result =="
Get-ChildItem $out | Select-Object Name, Length | Format-Table -AutoSize
$mb = (Get-Item $zip).Length / 1MB
Write-Host ("zip: {0}  {1:N1} MB" -f $zip, $mb)
