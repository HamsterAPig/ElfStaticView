param(
  [Parameter(Mandatory = $true)]
  [string]$InputPath,
  [Parameter(Mandatory = $true)]
  [string]$OutputPath,
  [Parameter(Mandatory = $true)]
  [string]$ObjcopyPath,
  [Parameter(Mandatory = $true)]
  [ValidateSet("addrx1", "addrx2", "addrx3", "addrx4")]
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

function Add-ByteRange {
  param(
    [System.Collections.Generic.List[byte]]$Target,
    [byte[]]$Data,
    [int]$Start,
    [int]$End
  )

  if ($Start -gt $End -or $Start -lt 0 -or $End -ge $Data.Length) {
    return
  }

  $count = $End - $Start + 1
  $slice = New-Object byte[] $count
  [Array]::Copy($Data, $Start, $slice, 0, $count)
  $Target.AddRange($slice)
}

function Write-Le32 {
  param(
    [System.Collections.Generic.List[byte]]$Target,
    [int]$Offset,
    [uint32]$Value
  )

  $bytes = [BitConverter]::GetBytes($Value)
  for ($index = 0; $index -lt 4; $index++) {
    $Target[$Offset + $index] = $bytes[$index]
  }
}

$tempDir = Join-Path $env:TEMP ("elf-static-view-dwarf5-addrx-form-" + [Guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Path $tempDir | Out-Null

try {
  $abbrevPath = Join-Path $tempDir "debug_abbrev.bin"
  $debugInfoPath = Join-Path $tempDir "debug_info.bin"
  & $ObjcopyPath --dump-section ".debug_abbrev=$abbrevPath" --dump-section ".debug_info=$debugInfoPath" $InputPath
  if ($LASTEXITCODE -ne 0) {
    throw "导出 .debug_abbrev/.debug_info 失败"
  }

  $formByte = switch ($Mode) {
    "addrx1" { [byte]0x29 }
    "addrx2" { [byte]0x2A }
    "addrx3" { [byte]0x2B }
    "addrx4" { [byte]0x2C }
  }

  [byte[]]$newPayload = switch ($Mode) {
    "addrx1" { [byte[]](0x01) }
    "addrx2" { [byte[]](0x01, 0x00) }
    "addrx3" { [byte[]](0x01, 0x00, 0x00) }
    "addrx4" { [byte[]](0x01, 0x00, 0x00, 0x00) }
  }

  $abbrev = [System.IO.File]::ReadAllBytes($abbrevPath)
  $pattern = [byte[]](0x11,0x1B,0x12,0x06,0x73,0x17,0x00,0x00)
  $matchIndex = Find-BytePattern -Data $abbrev -Pattern $pattern
  if ($matchIndex -lt 0) {
    throw "未找到 compile_unit 中 DW_AT_low_pc 的 DW_FORM_addrx 模式"
  }

  $patchedAbbrev = [System.Collections.Generic.List[byte]]::new()
  Add-ByteRange -Target $patchedAbbrev -Data $abbrev -Start 0 -End ($matchIndex - 1)
  $patchedAbbrev.AddRange([byte[]](0x11, $formByte, 0x12, 0x06, 0x73, 0x17, 0x00, 0x00))
  $tailStart = $matchIndex + $pattern.Length
  Add-ByteRange -Target $patchedAbbrev -Data $abbrev -Start $tailStart -End ($abbrev.Length - 1)

  $debugInfo = [System.Collections.Generic.List[byte]]::new()
  $originalInfo = [System.IO.File]::ReadAllBytes($debugInfoPath)
  if ($originalInfo.Length -lt 0x65) {
    throw ".debug_info 长度不足，无法修改 compile_unit 的 DW_AT_low_pc"
  }

  # 关键前提：
  # - 这里把 compile_unit 的 DW_AT_low_pc 从 DW_FORM_addrx 改成 addrx1/2/3/4；
  # - 选用的 index 固定为 0，这样 payload 只需要扩到目标宽度，不影响地址含义；
  # - 仅修改第一个 compile_unit 的 low_pc，对后续 subprogram 仍保留 DW_FORM_addrx，
  #   这样既能显式覆盖 form，又不扩大 patch 面。
  # 原始 dwarf5_strx_type_unit.elf 的 compile_unit DIE 从 0x44 开始，字段顺序固定为：
  # producer(1) + language(2) + name(1) + str_offsets_base(4) + stmt_list(4) +
  # comp_dir(1) + low_pc(1) + high_pc(4) + addr_base(4)。
  # 所以 low_pc 的 payload 固定落在 0x52。
  $payloadOffset = 0x52
  $newLength = $newPayload.Length
  $originalLength = 1

  Add-ByteRange -Target $debugInfo -Data $originalInfo -Start 0 -End ($payloadOffset - 1)
  $debugInfo.AddRange($newPayload)
  $suffixStart = $payloadOffset + $originalLength
  Add-ByteRange -Target $debugInfo -Data $originalInfo -Start $suffixStart -End ($originalInfo.Length - 1)

  $delta = $newPayload.Length - $originalLength
  $compileUnitLength = [BitConverter]::ToUInt32($originalInfo, 0x38)
  $newCompileUnitLengthBytes = [BitConverter]::GetBytes([uint32]($compileUnitLength + $delta))
  for ($i = 0; $i -lt 4; $i++) {
    $debugInfo[0x38 + $i] = $newCompileUnitLengthBytes[$i]
  }

  if ($delta -gt 0) {
    # 仅对低位 form 扩宽后受影响的 ref4 做平移修正。
    # 注意 ref4 保存的是“当前 CU header 相对偏移”：
    # - global_name.type 在 0x5D，原值 0x2F -> 指向绝对 0x67
    # - use_name.type    在 0x7D，原值 0x63 -> 指向绝对 0x9B
    # - local.type       在 0x87，原值 0x2F -> 指向绝对 0x67
    # - main.type        在 0x97，原值 0x63 -> 指向绝对 0x9B
    Write-Le32 -Target $debugInfo -Offset (0x5D + $delta) -Value ([uint32](0x2F + $delta))
    Write-Le32 -Target $debugInfo -Offset (0x7C + $delta) -Value ([uint32](0x63 + $delta))
    Write-Le32 -Target $debugInfo -Offset (0x87 + $delta) -Value ([uint32](0x2F + $delta))
    Write-Le32 -Target $debugInfo -Offset (0x97 + $delta) -Value ([uint32](0x63 + $delta))
  }

  [System.IO.File]::WriteAllBytes($abbrevPath, $patchedAbbrev.ToArray())
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
