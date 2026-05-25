param(
  [Parameter(Mandatory = $true)]
  [string]$InputPath,
  [Parameter(Mandatory = $true)]
  [string]$OutputPath
)

$bytes = [System.IO.File]::ReadAllBytes($InputPath)
$replacements = 0

for ($index = 0; $index -le $bytes.Length - 3; $index++) {
  if ($bytes[$index] -eq 0x02 -and $bytes[$index + 1] -eq 0xA1) {
    $bytes[$index + 1] = 0xFB
    $replacements++
    $index += 2
  }
}

if ($replacements -eq 0) {
  throw "未找到可替换的 DW_OP_addrx 模式"
}

[System.IO.File]::WriteAllBytes($OutputPath, $bytes)
