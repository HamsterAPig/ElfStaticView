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

$tempDir = Join-Path $env:TEMP ("elf-static-view-indirect-" + [Guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Path $tempDir | Out-Null

try {
  $abbrevPath = Join-Path $tempDir "debug_abbrev.bin"
  $typesPath = Join-Path $tempDir "debug_types.bin"

  & $ObjcopyPath --dump-section ".debug_abbrev=$abbrevPath" --dump-section ".debug_types=$typesPath" $InputPath
  if ($LASTEXITCODE -ne 0) {
    throw "导出 .debug_abbrev/.debug_types 失败"
  }

  $abbrev = [System.IO.File]::ReadAllBytes($abbrevPath)
  $abbrevPattern = [byte[]](0x02,0x13,0x01,0x36,0x0b,0x03,0x0e,0x0b,0x0b,0x3a,0x0b,0x3b,0x0b,0x00,0x00)
  $abbrevIndex = Find-BytePattern -Data $abbrev -Pattern $abbrevPattern
  if ($abbrevIndex -lt 0) {
    throw "未找到 structure_type abbrev 模式"
  }
  $abbrev[$abbrevIndex + 6] = 0x16
  [System.IO.File]::WriteAllBytes($abbrevPath, $abbrev)

  $types = [System.IO.File]::ReadAllBytes($typesPath)
  $unitLength = [BitConverter]::ToUInt32($types, 0)
  $typeOffset = [BitConverter]::ToUInt32($types, 19)
  $dieOffset = [int]$typeOffset
  if ($dieOffset + 7 -ge $types.Length) {
    throw "type unit DIE 偏移越界"
  }

  $patchedTypes = New-Object byte[] ($types.Length + 1)
  [Array]::Copy($types, 0, $patchedTypes, 0, $dieOffset + 2)
  $patchedTypes[$dieOffset + 2] = 0x0e
  [Array]::Copy($types,
                $dieOffset + 2,
                $patchedTypes,
                $dieOffset + 3,
                $types.Length - ($dieOffset + 2))

  [BitConverter]::GetBytes([uint32]($unitLength + 1)).CopyTo($patchedTypes, 0)

  $memberRefPattern = [byte[]](0x03)
  $patchedCount = 0
  for ($index = $dieOffset; $index -le $patchedTypes.Length - 9; $index++) {
    if ($patchedTypes[$index] -ne 0x03) {
      continue
    }
    $target = [BitConverter]::ToUInt32($patchedTypes, $index + 5)
    if ($target -eq 0x40) {
      [BitConverter]::GetBytes([uint32]0x41).CopyTo($patchedTypes, $index + 5)
      $patchedCount++
    }
  }
  if ($patchedCount -lt 2) {
    throw "未找到足够的 member ref4 目标用于修正偏移"
  }

  [System.IO.File]::WriteAllBytes($typesPath, $patchedTypes)

  & $ObjcopyPath --update-section ".debug_abbrev=$abbrevPath" --update-section ".debug_types=$typesPath" $InputPath $OutputPath
  if ($LASTEXITCODE -ne 0) {
    throw "回写 patch 后 section 失败"
  }
} finally {
  Remove-Item -LiteralPath $tempDir -Recurse -Force -ErrorAction SilentlyContinue
}
