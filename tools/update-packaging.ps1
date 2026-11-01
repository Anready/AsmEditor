# Updates the Scoop and winget manifests in packaging\ to a published release, with the SHA-256 digests GitHub
# lists for its files:  powershell -File tools\update-packaging.ps1 1.4.1
param([Parameter(Mandatory = $true)][string]$Version)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent

$release = Invoke-RestMethod "https://api.github.com/repos/Anready/AsmEditor/releases/tags/v$Version"
function Digest($name) {
    $asset = $release.assets | Where-Object { $_.name -eq $name }
    if (-not $asset -or -not $asset.digest) { throw "The release v$Version has no digest for $name" }
    return $asset.digest -replace '^sha256:', ''
}
$zipHash = Digest 'AsmEditor-portable.zip'
$setupHash = (Digest "AsmEditor-setup-$Version.exe").ToUpper()
$date = ([datetime]$release.published_at).ToString('yyyy-MM-dd')

# Scoop: version, url and hash (edited in place, so the file keeps its layout).
$utf8 = New-Object System.Text.UTF8Encoding($false)  # without a BOM
$scoop = Join-Path $root 'packaging\scoop\asmeditor.json'
$text = Get-Content $scoop -Raw
$text = $text -replace '"version": "[^"]*"', "`"version`": `"$Version`""
$text = $text -replace '"url": "[^"]*/v[0-9][^/"]*/AsmEditor-portable.zip"', "`"url`": `"https://github.com/Anready/AsmEditor/releases/download/v$Version/AsmEditor-portable.zip`""
$text = $text -replace '"hash": "[^"]*"', "`"hash`": `"$zipHash`""
[IO.File]::WriteAllText($scoop, $text, $utf8)

# winget: a folder per version, copied from the newest one with the numbers replaced.
$base = Join-Path $root 'packaging\winget\manifests\a\Anready\AsmEditor'
$previous = Get-ChildItem $base -Directory | Sort-Object { [version]$_.Name } | Select-Object -Last 1
$target = Join-Path $base $Version
if ($previous.Name -ne $Version) { Copy-Item $previous.FullName $target -Recurse }
foreach ($file in Get-ChildItem $target -Filter *.yaml) {
    $text = Get-Content $file.FullName -Raw
    $text = $text -replace [regex]::Escape($previous.Name), $Version
    $text = $text -replace '(?m)^(\s*InstallerSha256:).*$', "`$1 $setupHash"
    $text = $text -replace '(?m)^(ReleaseDate:).*$', "`$1 $date"
    [IO.File]::WriteAllText($file.FullName, $text, $utf8)
}
"Scoop and winget manifests are at $Version. Submit packaging\winget\manifests\a\Anready\AsmEditor\$Version to"
"microsoft/winget-pkgs (wingetcreate submit, or a pull request), and the Scoop manifest to your bucket."
