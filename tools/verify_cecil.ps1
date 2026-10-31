# Independent verification of a patched mod DLL using Mono.Cecil.
#   * metadata parses with a second, unrelated implementation (= file really is intact)
#   * type / method / instruction counts identical to the backup
#   * ldstr operands read back as the translated text (token re-targeting correct)
#
# Usage: powershell -File tools\verify_cecil.ps1 -Patched <dll> -Backup <dll.hanbak>
# NOTE: keep this file ASCII-only. Windows PowerShell 5.1 reads .ps1 as ANSI
#       when there is no BOM, which mangles non-ASCII literals.
param(
    [Parameter(Mandatory = $true)][string]$Patched,
    [Parameter(Mandatory = $true)][string]$Backup,
    [string]$Cecil = "D:\2SFS Project\Spaceflight Simulator Game\Spaceflight Simulator_Data\Managed\Mono.Cecil.dll"
)

$ErrorActionPreference = 'Stop'
Add-Type -Path $Cecil | Out-Null

function Get-Ldstrs([string]$path) {
    $asm = [Mono.Cecil.AssemblyDefinition]::ReadAssembly($path)
    $list = New-Object System.Collections.Generic.List[string]
    $typeCount = 0
    $methodCount = 0
    $insnCount = 0
    $stack = New-Object System.Collections.Stack
    foreach ($t in $asm.MainModule.Types) { $stack.Push($t) }
    while ($stack.Count -gt 0) {
        $t = $stack.Pop()
        $typeCount++
        foreach ($nt in $t.NestedTypes) { $stack.Push($nt) }
        foreach ($m in $t.Methods) {
            if (-not $m.HasBody) { continue }
            $methodCount++
            foreach ($i in $m.Body.Instructions) {
                $insnCount++
                if ($i.OpCode.Code -eq [Mono.Cecil.Cil.Code]::Ldstr) {
                    $list.Add([string]$i.Operand)
                }
            }
        }
    }
    $asm.Dispose()
    return [pscustomobject]@{
        Types = $typeCount; Methods = $methodCount; Insns = $insnCount; Strings = $list
    }
}

$p = Get-Ldstrs $Patched
$b = Get-Ldstrs $Backup

Write-Host ("backup : types {0} / methods-with-body {1} / instructions {2} / ldstr {3}" -f $b.Types, $b.Methods, $b.Insns, $b.Strings.Count)
Write-Host ("patched: types {0} / methods-with-body {1} / instructions {2} / ldstr {3}" -f $p.Types, $p.Methods, $p.Insns, $p.Strings.Count)

$ok = $true
if ($b.Types -ne $p.Types) { Write-Host "[FAIL] type count differs"; $ok = $false }
if ($b.Methods -ne $p.Methods) { Write-Host "[FAIL] method count differs"; $ok = $false }
if ($b.Insns -ne $p.Insns) { Write-Host "[FAIL] instruction count differs"; $ok = $false }
if ($b.Strings.Count -ne $p.Strings.Count) { Write-Host "[FAIL] ldstr count differs"; $ok = $false }

Write-Host ""
Write-Host "changed literals:"
$changed = 0
for ($i = 0; $i -lt [Math]::Min($b.Strings.Count, $p.Strings.Count); $i++) {
    if ($b.Strings[$i] -ne $p.Strings[$i]) {
        $changed++
        Write-Host ("  [{0}] {1}" -f $i, $b.Strings[$i])
        Write-Host ("      -> {0}" -f $p.Strings[$i])
    }
}
Write-Host ""
Write-Host ("total changed ldstr texts: {0}" -f $changed)
if ($ok) { Write-Host "[PASS] metadata parsed by Mono.Cecil, structural counts match backup" }
exit ($(if ($ok) { 0 } else { 1 }))
