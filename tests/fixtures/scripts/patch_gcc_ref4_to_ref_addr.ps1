param(
  [Parameter(Mandatory = $true)]
  [string]$InputPath,
  [Parameter(Mandatory = $true)]
  [string]$OutputPath,
  [Parameter(Mandatory = $true)]
  [string]$ObjcopyPath
)

$tempDir = Join-Path $env:TEMP ("elf-static-view-gcc-ref-addr-" + [Guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Path $tempDir | Out-Null

try {
  $abbrevPath = Join-Path $tempDir "debug_abbrev.bin"
  & $ObjcopyPath --dump-section ".debug_abbrev=$abbrevPath" $InputPath
  if ($LASTEXITCODE -ne 0) {
    throw "导出 .debug_abbrev 失败"
  }

  $abbrev = [System.IO.File]::ReadAllBytes($abbrevPath)
  $pattern = [byte[]](0x05,0x34,0x00,0x03,0x08,0x3a,0x0b,0x3b,0x0b,0x39,0x0b,0x49,0x13,0x02,0x18,0x00,0x00)
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
    throw "未找到 gcc variable ref4 abbrev 模板"
  }

  # 只把第一个 CU 里的 sup_value: DW_AT_type 改成 DW_FORM_ref_addr(0x10)。
  # 这个引用目标本来就是 .debug_info 全局偏移 0x77，payload 数值无需改写。
  $abbrev[$matchIndex + 13] = 0x10
  [System.IO.File]::WriteAllBytes($abbrevPath, $abbrev)

  & $ObjcopyPath --update-section ".debug_abbrev=$abbrevPath" $InputPath $OutputPath
  if ($LASTEXITCODE -ne 0) {
    throw "回写 .debug_abbrev 失败"
  }
} finally {
  Remove-Item -LiteralPath $tempDir -Recurse -Force -ErrorAction SilentlyContinue
}
