param(
  [Parameter(Mandatory = $true)]
  [string]$InputPath,
  [Parameter(Mandatory = $true)]
  [string]$OutputPath,
  [Parameter(Mandatory = $true)]
  [string]$ObjcopyPath,
  [string]$AltFileName = "gnu_alt_side.elf"
)

$tempDir = Join-Path $env:TEMP ("elf-static-view-gcc-gnu-alt-" + [Guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Path $tempDir | Out-Null

try {
  $abbrevPath = Join-Path $tempDir "debug_abbrev.bin"
  $debugInfoPath = Join-Path $tempDir "debug_info.bin"
  $altlinkPath = Join-Path $tempDir "gnu_debugaltlink.bin"

  & $ObjcopyPath --dump-section ".debug_abbrev=$abbrevPath" --dump-section ".debug_info=$debugInfoPath" $InputPath
  if ($LASTEXITCODE -ne 0) {
    throw "导出 .debug_abbrev/.debug_info 失败"
  }

  $abbrev = [System.IO.File]::ReadAllBytes($abbrevPath)
  $abbrevList = [System.Collections.Generic.List[byte]]::new()
  $abbrevList.AddRange($abbrev)

  $strpPattern = [byte[]](0x01,0x11,0x00,0x10,0x17,0x11,0x01,0x12,0x0f,0x03,0x0e,0x1b,0x0e,0x25,0x0e,0x13,0x05,0x00,0x00)
  # GNU form code 0x1f21 / 0x1f20 需要按 ULEB128 编码：
  # 0x1f21 -> 0xA1 0x3E
  # 0x1f20 -> 0xA0 0x3E
  $strpReplacement = [byte[]](0x01,0x11,0x00,0x10,0x17,0x11,0x01,0x12,0x0f,0x03,0xa1,0x3e,0x1b,0xa1,0x3e,0x25,0xa1,0x3e,0x13,0x05,0x00,0x00)
  $strpMatch = -1
  for ($index = 0; $index -le $abbrevList.Count - $strpPattern.Length; $index++) {
    $matched = $true
    for ($inner = 0; $inner -lt $strpPattern.Length; $inner++) {
      if ($abbrevList[$index + $inner] -ne $strpPattern[$inner]) {
        $matched = $false
        break
      }
    }
    if ($matched) {
      $strpMatch = $index
      break
    }
  }
  if ($strpMatch -lt 0) {
    throw "未找到 gcc 第二个 compile_unit 的 strp abbrev 模板"
  }
  $abbrevList.RemoveRange($strpMatch, $strpPattern.Length)
  $abbrevList.InsertRange($strpMatch, $strpReplacement)

  $ref4Pattern = [byte[]](0x05,0x34,0x00,0x03,0x08,0x3a,0x0b,0x3b,0x0b,0x39,0x0b,0x49,0x13,0x02,0x18,0x00,0x00)
  $ref4Replacement = [byte[]](0x05,0x34,0x00,0x03,0x08,0x3a,0x0b,0x3b,0x0b,0x39,0x0b,0x49,0xa0,0x3e,0x02,0x18,0x00,0x00)
  $ref4Match = -1
  for ($index = 0; $index -le $abbrevList.Count - $ref4Pattern.Length; $index++) {
    $matched = $true
    for ($inner = 0; $inner -lt $ref4Pattern.Length; $inner++) {
      if ($abbrevList[$index + $inner] -ne $ref4Pattern[$inner]) {
        $matched = $false
        break
      }
    }
    if ($matched) {
      $ref4Match = $index
      break
    }
  }
  if ($ref4Match -lt 0) {
    throw "未找到 gcc variable ref4 abbrev 模板"
  }
  $abbrevList.RemoveRange($ref4Match, $ref4Pattern.Length)
  $abbrevList.InsertRange($ref4Match, $ref4Replacement)

  [System.IO.File]::WriteAllBytes($abbrevPath, $abbrevList.ToArray())

  $debugInfo = [System.IO.File]::ReadAllBytes($debugInfoPath)
  if ($debugInfo.Length -lt 0x11b) {
    throw ".debug_info 长度不足，无法修正后续 CU 的 abbrev_offset"
  }

  # 当前 GCC 样本有三个 CU：
  # CU0: header 起始 0x0000, abbrev_offset = 0x0000（不变）
  # CU1: header 起始 0x00dd, abbrev_offset 字段位于 0x00e5..0x00e8
  #      原值 0x0071 -> 0x0072
  #      只受前一个 abbrev table 中 ref4 -> GNU_ref_alt 的 +1 影响
  # CU2: header 起始 0x0105, abbrev_offset 字段位于 0x010d..0x0110
  #      原值 0x0085 -> 0x0089
  #      同时受 ref4(+1) 和第二个 table 中 strp -> GNU_strp_alt(+3) 影响
  $debugInfo[0x00e5] = 0x72
  $debugInfo[0x00e6] = 0x00
  $debugInfo[0x00e7] = 0x00
  $debugInfo[0x00e8] = 0x00
  $debugInfo[0x010d] = 0x89
  $debugInfo[0x010e] = 0x00
  $debugInfo[0x010f] = 0x00
  $debugInfo[0x0110] = 0x00
  [System.IO.File]::WriteAllBytes($debugInfoPath, $debugInfo)

  $content = [System.Collections.Generic.List[byte]]::new()
  $content.AddRange([System.Text.Encoding]::ASCII.GetBytes($AltFileName))
  $content.Add(0)
  while (($content.Count % 4) -ne 0) {
    $content.Add(0)
  }
  1..20 | ForEach-Object { $content.Add(0x22) }
  [System.IO.File]::WriteAllBytes($altlinkPath, $content.ToArray())

  & $ObjcopyPath `
    --update-section ".debug_abbrev=$abbrevPath" `
    --update-section ".debug_info=$debugInfoPath" `
    --add-section ".gnu_debugaltlink=$altlinkPath" `
    $InputPath $OutputPath
  if ($LASTEXITCODE -ne 0) {
    throw "回写 .debug_abbrev/.debug_info/.gnu_debugaltlink 失败"
  }
} finally {
  Remove-Item -LiteralPath $tempDir -Recurse -Force -ErrorAction SilentlyContinue
}
