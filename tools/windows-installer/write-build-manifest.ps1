[CmdletBinding()]
param(
  [Parameter(Mandatory = $true)]
  [string] $PayloadRoot,

  [Parameter(Mandatory = $true)]
  [ValidatePattern('^\d+\.\d+\.\d+$')]
  [string] $Version
)

$ErrorActionPreference = "Stop"
$root = (Resolve-Path -LiteralPath $PayloadRoot).Path
$files = [ordered]@{}

Get-ChildItem -LiteralPath $root -File -Recurse |
  Sort-Object FullName |
  ForEach-Object {
    $relative = [IO.Path]::GetRelativePath($root, $_.FullName).Replace('\', '/')
    if ($relative -ne "build-manifest.json") {
      $files[$relative] = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
    }
  }

$gitSha = $env:GITHUB_SHA
if ([string]::IsNullOrWhiteSpace($gitSha)) {
  try { $gitSha = (& git rev-parse HEAD).Trim() } catch { $gitSha = "unknown" }
}

$manifest = [ordered]@{
  schemaVersion = 1
  product = "Lossless Audio SonoBus Relay"
  version = $Version
  gitSha = $gitSha
  ref = if ([string]::IsNullOrWhiteSpace($env:GITHUB_REF)) { "unknown" } else { $env:GITHUB_REF }
  workflow = if ([string]::IsNullOrWhiteSpace($env:GITHUB_WORKFLOW)) { "unknown" } else { $env:GITHUB_WORKFLOW }
  runId = if ([string]::IsNullOrWhiteSpace($env:GITHUB_RUN_ID)) { "unknown" } else { $env:GITHUB_RUN_ID }
  runAttempt = if ([string]::IsNullOrWhiteSpace($env:GITHUB_RUN_ATTEMPT)) { "unknown" } else { $env:GITHUB_RUN_ATTEMPT }
  generatedAtUtc = (Get-Date).ToUniversalTime().ToString("o")
  files = $files
}

$out = Join-Path $root "build-manifest.json"
$manifest | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $out -Encoding utf8
Write-Host "Wrote $out with $($files.Count) payload file hashes for $Version at $gitSha."
