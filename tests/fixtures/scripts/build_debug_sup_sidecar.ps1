param(
  [Parameter(Mandatory = $true)]
  [string]$InputPath,
  [Parameter(Mandatory = $true)]
  [string]$OutputPath,
  [Parameter(Mandatory = $true)]
  [string]$ObjcopyPath
)

$tempDir = Join-Path $env:TEMP ("elf-static-view-debug-sup-" + [Guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Path $tempDir | Out-Null

try {
  $debugSupSectionPath = Join-Path $tempDir "debug_sup.bin"
  $mainFileName = [System.IO.Path]::GetFileName($InputPath)
  $fileNameBytes = [System.Text.Encoding]::ASCII.GetBytes($mainFileName)
  $checksumText = "fake checksum_content"
  $checksumBytes = [System.Text.Encoding]::ASCII.GetBytes($checksumText)
  $checksumLength = [byte[]]@([byte]($checksumBytes.Length + 1))

  $content = New-Object System.Collections.Generic.List[byte]
  $content.AddRange([byte[]](0x02, 0x00))
  $content.Add(0x01)
  $content.AddRange($fileNameBytes)
  $content.Add(0x00)
  $content.AddRange($checksumLength)
  $content.AddRange($checksumBytes)
  $content.Add(0x00)
  [System.IO.File]::WriteAllBytes($debugSupSectionPath, $content.ToArray())

  & $ObjcopyPath --add-section ".debug_sup=$debugSupSectionPath" $InputPath $OutputPath
  if ($LASTEXITCODE -ne 0) {
    throw "写入 .debug_sup section 失败"
  }
} finally {
  Remove-Item -LiteralPath $tempDir -Recurse -Force -ErrorAction SilentlyContinue
}
