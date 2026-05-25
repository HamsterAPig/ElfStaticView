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

$tempDir = Join-Path $env:TEMP ("elf-static-view-refsig8-" + [Guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Path $tempDir | Out-Null

try {
  $abbrevPath = Join-Path $tempDir "debug_abbrev.bin"
  $infoPath = Join-Path $tempDir "debug_info.bin"

  & $ObjcopyPath --dump-section ".debug_abbrev=$abbrevPath" --dump-section ".debug_info=$infoPath" $InputPath
  if ($LASTEXITCODE -ne 0) {
    throw "导出 .debug_abbrev/.debug_info 失败"
  }

  $abbrev = [System.IO.File]::ReadAllBytes($abbrevPath)
  $oldAbbrev = [byte[]](0x06,0x34,0x00,0x03,0x0e,0x49,0x13,0x3a,0x0b,0x3b,0x0b,0x02,0x18,0x6e,0x0e,0x00,0x00)
  $newAbbrev = [byte[]](0x06,0x34,0x00,0x03,0x0e,0x49,0x20,0x3a,0x0b,0x3b,0x0b,0x02,0x18,0x00,0x00)
  $abbrevIndex = Find-BytePattern -Data $abbrev -Pattern $oldAbbrev
  if ($abbrevIndex -lt 0) {
    throw "未找到待替换的 variable abbrev"
  }

  $patchedAbbrev = New-Object byte[] ($abbrev.Length - ($oldAbbrev.Length - $newAbbrev.Length))
  [Array]::Copy($abbrev, 0, $patchedAbbrev, 0, $abbrevIndex)
  [Array]::Copy($newAbbrev, 0, $patchedAbbrev, $abbrevIndex, $newAbbrev.Length)
  [Array]::Copy($abbrev,
                $abbrevIndex + $oldAbbrev.Length,
                $patchedAbbrev,
                $abbrevIndex + $newAbbrev.Length,
                $abbrev.Length - ($abbrevIndex + $oldAbbrev.Length))
  [System.IO.File]::WriteAllBytes($abbrevPath, $patchedAbbrev)

  $info = [System.IO.File]::ReadAllBytes($infoPath)
  $globalIndex = -1
  $signatureIndex = -1
  for ($index = 0; $index -le $info.Length - 25; $index++) {
    if ($info[$index] -ne 0x06) {
      continue
    }
    $exprlocLength = $info[$index + 11]
    $nextDieIndex = $index + 12 + $exprlocLength + 4
    if ($nextDieIndex + 8 -ge $info.Length) {
      continue
    }
    if ($info[$nextDieIndex] -eq 0x07) {
      $globalIndex = $index
      $signatureIndex = $nextDieIndex + 1
      break
    }
  }
  if ($globalIndex -lt 0 -or $signatureIndex -lt 0) {
    throw "未找到 global_value DIE 模式"
  }

  $signature = New-Object byte[] 8
  [Array]::Copy($info, $signatureIndex, $signature, 0, $signature.Length)
  $exprlocLength = $info[$globalIndex + 11]
  $patchedDie = New-Object byte[] (12 + $exprlocLength + 4)
  $patchedDie[0] = 0x06
  [Array]::Copy($info, $globalIndex + 1, $patchedDie, 1, 4)
  [Array]::Copy($signature, 0, $patchedDie, 5, $signature.Length)
  $patchedDie[13] = $info[$globalIndex + 9]
  $patchedDie[14] = $info[$globalIndex + 10]
  $patchedDie[15] = $exprlocLength
  [Array]::Copy($info, $globalIndex + 12, $patchedDie, 16, $exprlocLength)
  [Array]::Copy($patchedDie, 0, $info, $globalIndex, $patchedDie.Length)
  [System.IO.File]::WriteAllBytes($infoPath, $info)

  & $ObjcopyPath --update-section ".debug_abbrev=$abbrevPath" --update-section ".debug_info=$infoPath" $InputPath $OutputPath
  if ($LASTEXITCODE -ne 0) {
    throw "回写 patch 后 section 失败"
  }
} finally {
  Remove-Item -LiteralPath $tempDir -Recurse -Force -ErrorAction SilentlyContinue
}
