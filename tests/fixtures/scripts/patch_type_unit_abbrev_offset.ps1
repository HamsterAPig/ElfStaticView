param(
  [Parameter(Mandatory = $true)]
  [string]$InputPath,
  [Parameter(Mandatory = $true)]
  [string]$OutputPath,
  [Parameter(Mandatory = $true)]
  [string]$ObjcopyPath
)

$tempDir = Join-Path $env:TEMP ("elf-static-view-type-unit-abbrev-" + [Guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Path $tempDir | Out-Null

try {
  $abbrevPath = Join-Path $tempDir "debug_abbrev.bin"
  $infoPath = Join-Path $tempDir "debug_info.bin"

  & $ObjcopyPath --dump-section ".debug_abbrev=$abbrevPath" --dump-section ".debug_info=$infoPath" $InputPath
  if ($LASTEXITCODE -ne 0) {
    throw "导出 .debug_abbrev/.debug_info 失败"
  }

  $abbrev = [System.IO.File]::ReadAllBytes($abbrevPath)
  $info = [System.IO.File]::ReadAllBytes($infoPath)

  if ($info.Length -lt 12) {
    throw ".debug_info 长度不足，无法修改 type unit header"
  }

  $newAbbrevOffset = [uint32]$abbrev.Length
  $patchedAbbrev = New-Object byte[] ($abbrev.Length * 2)
  [Array]::Copy($abbrev, 0, $patchedAbbrev, 0, $abbrev.Length)
  [Array]::Copy($abbrev, 0, $patchedAbbrev, $abbrev.Length, $abbrev.Length)

  [BitConverter]::GetBytes($newAbbrevOffset).CopyTo($info, 8)

  [System.IO.File]::WriteAllBytes($abbrevPath, $patchedAbbrev)
  [System.IO.File]::WriteAllBytes($infoPath, $info)

  & $ObjcopyPath --update-section ".debug_abbrev=$abbrevPath" --update-section ".debug_info=$infoPath" $InputPath $OutputPath
  if ($LASTEXITCODE -ne 0) {
    throw "回写 patch 后 section 失败"
  }
} finally {
  Remove-Item -LiteralPath $tempDir -Recurse -Force -ErrorAction SilentlyContinue
}
