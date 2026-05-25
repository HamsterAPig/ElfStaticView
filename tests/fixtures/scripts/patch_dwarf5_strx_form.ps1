param(
  [Parameter(Mandatory = $true)]
  [string]$InputPath,
  [Parameter(Mandatory = $true)]
  [string]$OutputPath,
  [Parameter(Mandatory = $true)]
  [string]$ObjcopyPath,
  [Parameter(Mandatory = $true)]
  [ValidateSet("strx2", "strx3", "strx4")]
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

$tempDir = Join-Path $env:TEMP ("elf-static-view-dwarf5-strx-form-" + [Guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Path $tempDir | Out-Null

try {
  $abbrevPath = Join-Path $tempDir "debug_abbrev.bin"
  $debugInfoPath = Join-Path $tempDir "debug_info.bin"
  & $ObjcopyPath --dump-section ".debug_abbrev=$abbrevPath" --dump-section ".debug_info=$debugInfoPath" $InputPath
  if ($LASTEXITCODE -ne 0) {
    throw "导出 .debug_abbrev/.debug_info 失败"
  }

  $formByte = switch ($Mode) {
    "strx2" { [byte]0x26 }
    "strx3" { [byte]0x27 }
    "strx4" { [byte]0x28 }
  }
  [byte[]]$newPayload = switch ($Mode) {
    "strx2" { [byte[]](0x06, 0x00) }
    "strx3" { [byte[]](0x06, 0x00, 0x00) }
    "strx4" { [byte[]](0x06, 0x00, 0x00, 0x00) }
  }

  $abbrev = [System.IO.File]::ReadAllBytes($abbrevPath)
  $pattern = [byte[]](0x36,0x0B,0x03,0x25,0x0B,0x0B,0x3A,0x0B,0x3B,0x0B,0x00,0x00)
  $matchIndex = Find-BytePattern -Data $abbrev -Pattern $pattern
  if ($matchIndex -lt 0) {
    throw "未找到 type unit 中 structure_type 的 DW_FORM_strx1 模式"
  }

  $patchedAbbrev = [System.Collections.Generic.List[byte]]::new()
  Add-ByteRange -Target $patchedAbbrev -Data $abbrev -Start 0 -End ($matchIndex - 1)
  $patchedAbbrev.AddRange([byte[]](0x36, 0x0B, 0x03, $formByte, 0x0B, 0x0B, 0x3A, 0x0B, 0x3B, 0x0B, 0x00, 0x00))
  $tailStart = $matchIndex + $pattern.Length
  Add-ByteRange -Target $patchedAbbrev -Data $abbrev -Start $tailStart -End ($abbrev.Length - 1)

  $originalInfo = [System.IO.File]::ReadAllBytes($debugInfoPath)
  if ($originalInfo.Length -lt 0x38) {
    throw ".debug_info 长度不足，无法修改 type unit"
  }

  # 只修改第一个 type unit 的 structure_type.name：
  # unit 从 0x18 开始，abbrev code=0x02 后紧接 calling_convention，再后面就是 name(strx1)。
  $payloadOffset = 0x25
  $originalLength = 1

  $patchedInfo = [System.Collections.Generic.List[byte]]::new()
  Add-ByteRange -Target $patchedInfo -Data $originalInfo -Start 0 -End ($payloadOffset - 1)
  $patchedInfo.AddRange($newPayload)
  $suffixStart = $payloadOffset + $originalLength
  Add-ByteRange -Target $patchedInfo -Data $originalInfo -Start $suffixStart -End ($originalInfo.Length - 1)

  $delta = $newPayload.Length - $originalLength
  $typeUnitLength = [BitConverter]::ToUInt32($originalInfo, 0)
  $newTypeUnitLengthBytes = [BitConverter]::GetBytes([uint32]($typeUnitLength + $delta))
  for ($i = 0; $i -lt 4; $i++) {
    $patchedInfo[$i] = $newTypeUnitLengthBytes[$i]
  }

  if ($delta -gt 0) {
    # member.value 的 DW_FORM_ref4 指向同一个 type unit 内的 base_type，
    # 原始相对偏移是 0x33；name 扩宽后 base_type 会整体后移 delta。
    Write-Le32 -Target $patchedInfo -Offset (0x2B + $delta) -Value ([uint32](0x33 + $delta))
  }

  # 第二个 compile unit header 从原来的 0x38 整体后移 delta。
  $originalCompileUnitOffset = 0x38
  $newCompileUnitOffset = $originalCompileUnitOffset + $delta
  if ($newCompileUnitOffset + 12 -gt $patchedInfo.Count) {
    throw "patch 后 compile unit header 越界"
  }

  [System.IO.File]::WriteAllBytes($abbrevPath, $patchedAbbrev.ToArray())
  [System.IO.File]::WriteAllBytes($debugInfoPath, $patchedInfo.ToArray())

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
