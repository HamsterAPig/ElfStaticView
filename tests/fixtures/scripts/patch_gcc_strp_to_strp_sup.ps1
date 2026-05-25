param(
  [Parameter(Mandatory = $true)]
  [string]$InputPath,
  [Parameter(Mandatory = $true)]
  [string]$OutputPath,
  [Parameter(Mandatory = $true)]
  [string]$ObjcopyPath,
  [Parameter(Mandatory = $true)]
  [string]$DebugSupBytesPath
)

$tempDir = Join-Path $env:TEMP ("elf-static-view-gcc-strp-sup-" + [Guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Path $tempDir | Out-Null

try {
  $abbrevPath = Join-Path $tempDir "debug_abbrev.bin"
  & $ObjcopyPath --dump-section ".debug_abbrev=$abbrevPath" $InputPath
  if ($LASTEXITCODE -ne 0) {
    throw "导出 .debug_abbrev 失败"
  }

  $abbrev = [System.IO.File]::ReadAllBytes($abbrevPath)
  $pattern = [byte[]](0x01,0x11,0x00,0x10,0x17,0x11,0x01,0x12,0x0f,0x03,0x0e,0x1b,0x0e,0x25,0x0e,0x13,0x05,0x00,0x00)
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
    throw "未找到 gcc 第二个 compile_unit 的 strp abbrev 模板"
  }

  # 第二个 compile unit 的三个字符串 form: DW_AT_name / comp_dir / producer
  # pattern 里这三个 DW_FORM_strp(0x0e) 分别位于 +10/+12/+14。
  # 之前错改成了 attr 字节，导致 .debug_abbrev 损坏并触发 duplicated attr。
  # 这里改成 DW_FORM_strp_sup(0x1d)，宽度仍是 4 字节，payload 可直接复用。
  $abbrev[$matchIndex + 10] = 0x1d
  $abbrev[$matchIndex + 12] = 0x1d
  $abbrev[$matchIndex + 14] = 0x1d

  [System.IO.File]::WriteAllBytes($abbrevPath, $abbrev)

  & $ObjcopyPath `
    --update-section ".debug_abbrev=$abbrevPath" `
    --add-section ".debug_sup=$DebugSupBytesPath" `
    $InputPath $OutputPath
  if ($LASTEXITCODE -ne 0) {
    throw "回写 .debug_abbrev/.debug_sup 失败"
  }
} finally {
  Remove-Item -LiteralPath $tempDir -Recurse -Force -ErrorAction SilentlyContinue
}
