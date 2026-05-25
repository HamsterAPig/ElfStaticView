param(
  [Parameter(Mandatory = $true)]
  [string]$InputPath,
  [Parameter(Mandatory = $true)]
  [string]$OutputPath,
  [Parameter(Mandatory = $true)]
  [string]$ObjcopyPath,
  [Parameter(Mandatory = $true)]
  [ValidateSet("ref1", "ref2", "ref_udata")]
  [string]$Mode
)

function Find-BytePattern {
  param(
    [byte[]]$Data,
    [byte[]]$Pattern
  )

  for ($index = 0; $index -le $Data.Length - $Pattern.Length; $index++) {
    $matched = $true
    for ($inner = 0; $inner -lt $Pattern.Length; $inner++) {
      if ($Data[$index + $inner] -ne $Pattern[$inner]) {
        $matched = $false
        break
      }
    }
    if ($matched) {
      return $index
    }
  }

  return -1
}

$tempDir = Join-Path $env:TEMP ("elf-static-view-gcc-small-ref-" + [Guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Path $tempDir | Out-Null

try {
  $abbrevPath = Join-Path $tempDir "debug_abbrev.bin"
  $debugInfoPath = Join-Path $tempDir "debug_info.bin"
  & $ObjcopyPath --dump-section ".debug_abbrev=$abbrevPath" --dump-section ".debug_info=$debugInfoPath" $InputPath
  if ($LASTEXITCODE -ne 0) {
    throw "导出 .debug_abbrev/.debug_info 失败"
  }

  $formByte = switch ($Mode) {
    "ref1" { [byte]0x11 }
    "ref2" { [byte]0x12 }
    "ref_udata" { [byte]0x15 }
  }
  [byte[]]$newPayload = switch ($Mode) {
    "ref1" { [byte[]](0x77) }
    "ref2" { [byte[]](0x77, 0x00) }
    "ref_udata" { [byte[]](0x77) }
  }
  $shrinkBytes = 4 - $newPayload.Length

  $abbrev = [System.IO.File]::ReadAllBytes($abbrevPath)
  $pattern = [byte[]](0x05,0x34,0x00,0x03,0x08,0x3a,0x0b,0x3b,0x0b,0x39,0x0b,0x49,0x13,0x02,0x18,0x00,0x00)
  $matchIndex = Find-BytePattern -Data $abbrev -Pattern $pattern
  if ($matchIndex -lt 0) {
    throw "未找到 gcc variable ref4 abbrev 模板"
  }
  # [5] DW_TAG_variable:
  # 05 34 00 03 08 3a 0b 3b 0b 39 0b 49 13 02 18 00 00
  #                              ^^^^^^^
  #                              DW_AT_type + DW_FORM_ref4
  # type form 在 +12，不是 +13。
  $abbrev[$matchIndex + 12] = $formByte
  [System.IO.File]::WriteAllBytes($abbrevPath, $abbrev)

  $debugInfo = [System.Collections.Generic.List[byte]]::new()
  $debugInfo.AddRange([System.IO.File]::ReadAllBytes($debugInfoPath))

  # 第一 CU 的 sup_value DIE 位于 0x000000a1。
  # 布局：abbrev(1) + "sup_value\0"(10) + decl_file(1) + decl_line(1) + decl_column(1)
  # 然后是原始 ref4 payload 4 字节 0x77 00 00 00。
  $typeFieldOffset = 0x000000a1 + 1 + 10 + 1 + 1 + 1
  $debugInfo.RemoveRange($typeFieldOffset, 4)
  $debugInfo.InsertRange($typeFieldOffset, $newPayload)

  $buffer = $debugInfo.ToArray()
  $originalLength = [BitConverter]::ToUInt32($buffer, 0)
  $newLengthBytes = [BitConverter]::GetBytes([uint32]($originalLength - $shrinkBytes))
  for ($i = 0; $i -lt 4; $i++) {
    $debugInfo[$i] = $newLengthBytes[$i]
  }

  [System.IO.File]::WriteAllBytes($debugInfoPath, $debugInfo.ToArray())

  & $ObjcopyPath `
    --update-section ".debug_abbrev=$abbrevPath" `
    --update-section ".debug_info=$debugInfoPath" `
    $InputPath $OutputPath
  if ($LASTEXITCODE -ne 0) {
    throw "回写 .debug_abbrev/.debug_info 失败"
  }
} finally {
  Remove-Item -LiteralPath $tempDir -Recurse -Force -ErrorAction SilentlyContinue
}
