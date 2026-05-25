param(
  [Parameter(Mandatory = $true)]
  [string]$InputPath,
  [Parameter(Mandatory = $true)]
  [string]$OutputPath,
  [Parameter(Mandatory = $true)]
  [string]$ObjcopyPath
)

$tempDir = Join-Path $env:TEMP ("elf-static-view-rnglists-startx-endx-" + [Guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Path $tempDir | Out-Null

try {
  $rnglistsPath = Join-Path $tempDir "debug_rnglists.bin"
  & $ObjcopyPath --dump-section ".debug_rnglists=$rnglistsPath" $InputPath
  if ($LASTEXITCODE -ne 0) {
    throw "导出 .debug_rnglists 失败"
  }

  $rnglists = [System.IO.File]::ReadAllBytes($rnglistsPath)
  if ($rnglists.Length -lt 0x25) {
    throw ".debug_rnglists 长度不足，无法改写成 startx_endx"
  }

  # 直接重写 ranges payload：
  #   DW_RLE_startx_endx 0x0, 0x1
  #   DW_RLE_start_length 0x1400028a0, 0x13
  #   DW_RLE_end_of_list
  # 这里显式依赖 .debug_addr[0] -> 0x140001460, .debug_addr[1] -> 0x140001477。
  $payload = [byte[]](
    0x02,0x00,0x01,
    0x07,
    0xA0,0x28,0x00,0x40,0x01,0x00,0x00,0x00,
    0x13,
    0x00
  )
  $newLength = 16 + $payload.Length
  $patched = New-Object byte[] $newLength

  [Array]::Copy($rnglists, 0, $patched, 0, 16)
  [Array]::Copy($payload, 0, $patched, 16, $payload.Length)
  [BitConverter]::GetBytes([uint32]($newLength - 4)).CopyTo($patched, 0)

  [System.IO.File]::WriteAllBytes($rnglistsPath, $patched)

  & $ObjcopyPath --update-section ".debug_rnglists=$rnglistsPath" $InputPath $OutputPath
  if ($LASTEXITCODE -ne 0) {
    throw "回写 patch 后 section 失败"
  }
} finally {
  Remove-Item -LiteralPath $tempDir -Recurse -Force -ErrorAction SilentlyContinue
}
