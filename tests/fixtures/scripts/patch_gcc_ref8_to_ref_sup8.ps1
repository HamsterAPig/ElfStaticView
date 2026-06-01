param(
  [Parameter(Mandatory = $true)]
  [string]$InputPath,
  [Parameter(Mandatory = $true)]
  [string]$OutputPath,
  [Parameter(Mandatory = $true)]
  [string]$ObjcopyPath
)

$tempDir = Join-Path $env:TEMP ("elf-static-view-gcc-ref-sup8-" + [Guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Path $tempDir | Out-Null

try {
  $abbrevPath = Join-Path $tempDir "debug_abbrev.bin"
  & $ObjcopyPath --dump-section ".debug_abbrev=$abbrevPath" $InputPath
  if ($LASTEXITCODE -ne 0) {
    throw "导出 .debug_abbrev 失败"
  }

  $abbrev = [System.IO.File]::ReadAllBytes($abbrevPath)
  $pattern = [byte[]](0x05,0x34,0x00,0x03,0x08,0x3a,0x0b,0x3b,0x0b,0x39,0x0b,0x49,0x14,0x02,0x18,0x00,0x00)
  $matchIndex = -1
  for ($index = 0; $index -le $abbrev.Length - $pattern.Length; $index++) {
    $matched = $true
    for ($inner = 0; $inner -lt $pattern.Length; $inner++) {
      if ($abbrev[$index + $inner] -ne $pattern[$inner]) {
        $matched = $false
        break
      }
    }
    if ($matched) {
      $matchIndex = $index
      break
    }
  }
  if ($matchIndex -lt 0) {
    throw "未找到 gcc variable ref8 abbrev 模板"
  }

  # [5] DW_TAG_variable 的 DW_AT_type form 位于 pattern 内第 12 字节。
  # 把 DW_FORM_ref8(0x14) 改成 DW_FORM_ref_sup8(0x24)，宽度保持 8 字节。
  $abbrev[$matchIndex + 12] = 0x24
  [System.IO.File]::WriteAllBytes($abbrevPath, $abbrev)

  & $ObjcopyPath --update-section ".debug_abbrev=$abbrevPath" $InputPath $OutputPath
  if ($LASTEXITCODE -ne 0) {
    throw "回写 .debug_abbrev 失败"
  }
} finally {
  Remove-Item -LiteralPath $tempDir -Recurse -Force -ErrorAction SilentlyContinue
}
