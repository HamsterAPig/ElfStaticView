param(
  [Parameter(Mandatory = $true)]
  [string]$InputPath,
  [Parameter(Mandatory = $true)]
  [string]$OutputPath,
  [Parameter(Mandatory = $true)]
  [string]$ObjcopyPath
)

$tempDir = Join-Path $env:TEMP ("elf-static-view-loclists-startx-length-" + [Guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Path $tempDir | Out-Null

try {
  $loclistsPath = Join-Path $tempDir "debug_loclists.bin"
  & $ObjcopyPath --dump-section ".debug_loclists=$loclistsPath" $InputPath
  if ($LASTEXITCODE -ne 0) {
    throw "导出 .debug_loclists 失败"
  }

  $loclists = [System.IO.File]::ReadAllBytes($loclistsPath)
  if ($loclists.Length -lt 0x1b) {
    throw ".debug_loclists 长度不足，无法改写成 startx_length"
  }

  $patched = New-Object byte[] $loclists.Length
  [Array]::Copy($loclists, $patched, $loclists.Length)

  # 0x10 起原本是：
  #   01 00                DW_LLE_base_addressx(0)
  #   05 01 50             default_location
  #   04 00 0f ...         DW_LLE_offset_pair
  # 这里把第二条有效范围改成：
  #   03 00 0f <expr>      DW_LLE_startx_length(index=0, length=0x0f)
  # 这样仍然依赖同一个 .debug_addr[0]，但显式覆盖 startx_length。
  $patched[0x15] = 0x03

  [System.IO.File]::WriteAllBytes($loclistsPath, $patched)

  & $ObjcopyPath --update-section ".debug_loclists=$loclistsPath" $InputPath $OutputPath
  if ($LASTEXITCODE -ne 0) {
    throw "回写 patch 后 section 失败"
  }
} finally {
  Remove-Item -LiteralPath $tempDir -Recurse -Force -ErrorAction SilentlyContinue
}
