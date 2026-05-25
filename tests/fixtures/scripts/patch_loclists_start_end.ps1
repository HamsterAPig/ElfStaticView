param(
  [Parameter(Mandatory = $true)]
  [string]$InputPath,
  [Parameter(Mandatory = $true)]
  [string]$OutputPath,
  [Parameter(Mandatory = $true)]
  [string]$ObjcopyPath
)

$tempDir = Join-Path $env:TEMP ("elf-static-view-loclists-start-end-" + [Guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Path $tempDir | Out-Null

try {
  $loclistsPath = Join-Path $tempDir "debug_loclists.bin"
  & $ObjcopyPath --dump-section ".debug_loclists=$loclistsPath" $InputPath
  if ($LASTEXITCODE -ne 0) {
    throw "导出 .debug_loclists 失败"
  }

  $loclists = [System.IO.File]::ReadAllBytes($loclistsPath)
  if ($loclists.Length -lt 0x23) {
    throw ".debug_loclists 长度不足，无法改写成 start_end"
  }

  $replacement = [byte[]](
    0x07,
    0x00,0x13,0x00,0x00,0x00,0x00,0x00,0x00,
    0x0f,0x13,0x00,0x00,0x00,0x00,0x00,0x00,
    0x0a,
    0x75,0x2a,0x10,0xff,0xff,0xff,0xff,0x0f,0x1a,0x9f
  )
  $replacedLength = 14
  $delta = $replacement.Length - $replacedLength
  $patched = New-Object byte[] ($loclists.Length + $delta)

  [Array]::Copy($loclists, 0, $patched, 0, 0x15)
  [Array]::Copy($replacement, 0, $patched, 0x15, $replacement.Length)
  [Array]::Copy($loclists, 0x15 + $replacedLength, $patched, 0x15 + $replacement.Length, $loclists.Length - (0x15 + $replacedLength))
  [BitConverter]::GetBytes([uint32]([BitConverter]::ToUInt32($loclists, 0) + $delta)).CopyTo($patched, 0)

  [System.IO.File]::WriteAllBytes($loclistsPath, $patched)

  & $ObjcopyPath --update-section ".debug_loclists=$loclistsPath" $InputPath $OutputPath
  if ($LASTEXITCODE -ne 0) {
    throw "回写 patch 后 section 失败"
  }
} finally {
  Remove-Item -LiteralPath $tempDir -Recurse -Force -ErrorAction SilentlyContinue
}
