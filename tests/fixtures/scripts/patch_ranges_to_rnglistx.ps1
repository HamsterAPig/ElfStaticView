param(
  [Parameter(Mandatory = $true)]
  [string]$InputPath,
  [Parameter(Mandatory = $true)]
  [string]$OutputPath,
  [Parameter(Mandatory = $true)]
  [string]$ObjcopyPath
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

$tempDir = Join-Path $env:TEMP ("elf-static-view-rnglistx-" + [Guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Path $tempDir | Out-Null

try {
  $abbrevPath = Join-Path $tempDir "debug_abbrev.bin"
  $debugInfoPath = Join-Path $tempDir "debug_info.bin"
  $debugRnglistsPath = Join-Path $tempDir "debug_rnglists.bin"

  & $ObjcopyPath --dump-section ".debug_abbrev=$abbrevPath" --dump-section ".debug_info=$debugInfoPath" --dump-section ".debug_rnglists=$debugRnglistsPath" $InputPath
  if ($LASTEXITCODE -ne 0) {
    throw "导出 .debug_abbrev/.debug_info/.debug_rnglists 失败"
  }

  $abbrev = [System.IO.File]::ReadAllBytes($abbrevPath)
  $pattern = [byte[]](0x55,0x17)
  $matchIndex = Find-BytePattern -Data $abbrev -Pattern $pattern
  if ($matchIndex -lt 0) {
    throw "未找到 DW_AT_ranges + DW_FORM_sec_offset 模式"
  }

  # 0x55 = DW_AT_ranges, 0x23 = DW_FORM_rnglistx
  $abbrev[$matchIndex + 1] = 0x23

  $info = [System.IO.File]::ReadAllBytes($debugInfoPath)

  # 第一个 CU 的 DW_AT_ranges 原本是 DW_FORM_sec_offset(4 bytes) 且值为 0x0c。
  # 这里改成 DW_FORM_rnglistx 后，为避免重排后续字段，写一个“过长但合法”的 ULEB128 0：
  #   0x80 0x80 0x80 0x00
  # 这样 libdwarf 读取到的 index 仍是 0，但 payload 宽度保持 4 字节不变。
  $rangesPayloadOffset = 0x59
  $info[$rangesPayloadOffset + 0] = 0x80
  $info[$rangesPayloadOffset + 1] = 0x80
  $info[$rangesPayloadOffset + 2] = 0x80
  $info[$rangesPayloadOffset + 3] = 0x00

  $rnglists = [System.IO.File]::ReadAllBytes($debugRnglistsPath)
  if ($rnglists.Length -lt 0x0c) {
    throw ".debug_rnglists 长度不足，无法补 offset-entry table"
  }

  # 把原始 header：
  #   length(4) version(2) addr_size(1) seg_size(1) offset_entry_count(4=0)
  # 改成：
  #   length(4) version(2) addr_size(1) seg_size(1) offset_entry_count(4=1) offsets[0]=0x04
  # 同时把原 ranges payload 整体后移 4 字节，保证 index 0 指向 header 后第一个表项之后的真实 list。
  $patchedRnglists = New-Object byte[] ($rnglists.Length + 4)
  [Array]::Copy($rnglists, 0, $patchedRnglists, 0, 8)
  [BitConverter]::GetBytes([uint32]1).CopyTo($patchedRnglists, 8)
  [BitConverter]::GetBytes([uint32]4).CopyTo($patchedRnglists, 12)
  [Array]::Copy($rnglists, 12, $patchedRnglists, 16, $rnglists.Length - 12)
  [BitConverter]::GetBytes([uint32]([BitConverter]::ToUInt32($rnglists, 0) + 4)).CopyTo($patchedRnglists, 0)

  [System.IO.File]::WriteAllBytes($abbrevPath, $abbrev)
  [System.IO.File]::WriteAllBytes($debugInfoPath, $info)
  [System.IO.File]::WriteAllBytes($debugRnglistsPath, $patchedRnglists)

  & $ObjcopyPath `
    --update-section ".debug_abbrev=$abbrevPath" `
    --update-section ".debug_info=$debugInfoPath" `
    --update-section ".debug_rnglists=$debugRnglistsPath" `
    $InputPath $OutputPath
  if ($LASTEXITCODE -ne 0) {
    throw "回写 .debug_abbrev/.debug_info/.debug_rnglists 失败"
  }
} finally {
  Remove-Item -LiteralPath $tempDir -Recurse -Force -ErrorAction SilentlyContinue
}
