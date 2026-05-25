param(
  [Parameter(Mandatory = $true)]
  [string]$InputPath,
  [Parameter(Mandatory = $true)]
  [string]$OutputPath,
  [Parameter(Mandatory = $true)]
  [string]$ObjcopyPath
)

$tempDir = Join-Path $env:TEMP ("elf-static-view-loclists-startx-endx-" + [Guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Path $tempDir | Out-Null

try {
  $loclistsPath = Join-Path $tempDir "debug_loclists.bin"
  & $ObjcopyPath --dump-section ".debug_loclists=$loclistsPath" $InputPath
  if ($LASTEXITCODE -ne 0) {
    throw "导出 .debug_loclists 失败"
  }

  $loclists = [System.IO.File]::ReadAllBytes($loclistsPath)
  if ($loclists.Length -lt 0x23) {
    throw ".debug_loclists 长度不足，无法改写成 startx_endx"
  }

  $patched = New-Object byte[] $loclists.Length
  [Array]::Copy($loclists, $patched, $loclists.Length)

  # 0x15 起原本是：
  #   04 00 0f 0a <expr>
  #   DW_LLE_offset_pair(low=0, high=0x0f)
  # 这里改成：
  #   02 00 01 0a <expr>
  #   DW_LLE_startx_endx(start index=0, end index=1)
  # 两种编码长度一致，仍然复用同一段表达式，且直接依赖 .debug_addr[0..1]。
  $patched[0x15] = 0x02
  $patched[0x17] = 0x01

  [System.IO.File]::WriteAllBytes($loclistsPath, $patched)

  & $ObjcopyPath --update-section ".debug_loclists=$loclistsPath" $InputPath $OutputPath
  if ($LASTEXITCODE -ne 0) {
    throw "回写 patch 后 section 失败"
  }
} finally {
  Remove-Item -LiteralPath $tempDir -Recurse -Force -ErrorAction SilentlyContinue
}
