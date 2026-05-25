param(
  [Parameter(Mandatory = $true)]
  [string]$InputPath,
  [Parameter(Mandatory = $true)]
  [string]$OutputPath,
  [Parameter(Mandatory = $true)]
  [string]$ObjcopyPath
)

$tempDir = Join-Path $env:TEMP ("elf-static-view-loclists-base-default-" + [Guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Path $tempDir | Out-Null

try {
  $loclistsPath = Join-Path $tempDir "debug_loclists.bin"
  & $ObjcopyPath --dump-section ".debug_loclists=$loclistsPath" $InputPath
  if ($LASTEXITCODE -ne 0) {
    throw "导出 .debug_loclists 失败"
  }

  $loclists = [System.IO.File]::ReadAllBytes($loclistsPath)
  $insert = [byte[]](0x06,0x00,0x13,0x00,0x00,0x00,0x00,0x00,0x00,0x05,0x01,0x50)
  $patched = New-Object byte[] ($loclists.Length + $insert.Length)

  [Array]::Copy($loclists, 0, $patched, 0, 0x10)
  [Array]::Copy($insert, 0, $patched, 0x10, $insert.Length)
  [Array]::Copy($loclists, 0x10, $patched, 0x10 + $insert.Length, $loclists.Length - 0x10)
  [BitConverter]::GetBytes([uint32]([BitConverter]::ToUInt32($loclists, 0) + $insert.Length)).CopyTo($patched, 0)

  [System.IO.File]::WriteAllBytes($loclistsPath, $patched)

  & $ObjcopyPath --update-section ".debug_loclists=$loclistsPath" $InputPath $OutputPath
  if ($LASTEXITCODE -ne 0) {
    throw "回写 patch 后 section 失败"
  }
} finally {
  Remove-Item -LiteralPath $tempDir -Recurse -Force -ErrorAction SilentlyContinue
}
