param(
  [Parameter(Mandatory = $true)]
  [string] $Installer
)

$ErrorActionPreference = "Stop"
$installerPath = (Resolve-Path $Installer).Path
$appDir = Join-Path $env:RUNNER_TEMP ("sonobus-installer-" + [guid]::NewGuid())
$vst3Root = Join-Path $env:CommonProgramFiles "VST3"
$pluginDirs = @(
  (Join-Path $vst3Root "SonoBus.vst3"),
  (Join-Path $vst3Root "SonoBusInstrument.vst3")
)
$payloadRoot = Join-Path (Get-Location).Path "installer-input\SonoBus"
$uninstaller = $null
$verifyUninstall = $false

try {
  $process = Start-Process -FilePath $installerPath -ArgumentList @(
    "/VERYSILENT",
    "/SUPPRESSMSGBOXES",
    "/NORESTART",
    "/DIR=`"$appDir`""
  ) -Wait -PassThru
  if ($process.ExitCode -ne 0) { throw "Installer exited with code $($process.ExitCode)." }

  $runtimes = @(
    (Join-Path $appDir "ffmpeg.exe"),
    (Join-Path $appDir "ffmpeg32.exe"),
    (Join-Path $pluginDirs[0] "Contents/x86_64-win/ffmpeg.exe"),
    (Join-Path $pluginDirs[0] "Contents/x86_64-win/ffmpeg32.exe"),
    (Join-Path $pluginDirs[1] "Contents/x86_64-win/ffmpeg.exe"),
    (Join-Path $pluginDirs[1] "Contents/x86_64-win/ffmpeg32.exe")
  )
  $helpers = @(
    (Join-Path $appDir "SonoBusVideoCaptureHelper.exe"),
    (Join-Path $pluginDirs[0] "Contents/x86_64-win/SonoBusVideoCaptureHelper.exe"),
    (Join-Path $pluginDirs[1] "Contents/x86_64-win/SonoBusVideoCaptureHelper.exe")
  )
  $molixiuBridge = @(
    (Join-Path $appDir "SonoBusMoLiXiuBridge.exe"),
    (Join-Path $appDir "SonoBusMoLiXiuHook.dll"),
    (Join-Path $pluginDirs[0] "Contents/x86_64-win/SonoBusMoLiXiuBridge.exe"),
    (Join-Path $pluginDirs[0] "Contents/x86_64-win/SonoBusMoLiXiuHook.dll"),
    (Join-Path $pluginDirs[1] "Contents/x86_64-win/SonoBusMoLiXiuBridge.exe"),
    (Join-Path $pluginDirs[1] "Contents/x86_64-win/SonoBusMoLiXiuHook.dll")
  )
  $required = @(
    (Join-Path $appDir "SonoBus.exe"),
    (Join-Path $appDir "ffmpeg-LICENSE"),
    (Join-Path $appDir "ffmpeg32-LICENSE"),
    (Join-Path $appDir "ffmpeg-README.txt"),
    (Join-Path $appDir "ffmpeg-RUNTIME.md"),
    (Join-Path $appDir "build-manifest.json"),
    (Join-Path $pluginDirs[0] "Contents/x86_64-win/SonoBus.vst3"),
    (Join-Path $pluginDirs[1] "Contents/x86_64-win/SonoBusInstrument.vst3")
  ) + $runtimes + $helpers + $molixiuBridge

  foreach ($path in $required) {
    if (-not (Test-Path $path -PathType Leaf)) { throw "Installer omitted: $path" }
  }

  $sourceHash = (Get-FileHash -Algorithm SHA256 $env:SONOBUS_FFMPEG_PATH).Hash
  foreach ($runtime in $runtimes | Where-Object { $_ -notlike "*ffmpeg32.exe" }) {
    if ((Get-FileHash -Algorithm SHA256 $runtime).Hash -ne $sourceHash) {
      throw "Installed FFmpeg checksum mismatch: $runtime"
    }
  }
  $sourceHash32 = (Get-FileHash -Algorithm SHA256 $env:SONOBUS_FFMPEG32_PATH).Hash
  foreach ($runtime in $runtimes | Where-Object { $_ -like "*ffmpeg32.exe" }) {
    if ((Get-FileHash -Algorithm SHA256 $runtime).Hash -ne $sourceHash32) {
      throw "Installed 32-bit FFmpeg checksum mismatch: $runtime"
    }
  }

  if (-not (Test-Path -LiteralPath $payloadRoot -PathType Container)) {
    throw "Installer source payload is missing: $payloadRoot"
  }
  $payloadManifestPath = Join-Path $payloadRoot "build-manifest.json"
  if (-not (Test-Path -LiteralPath $payloadManifestPath -PathType Leaf)) {
    throw "Installer source manifest is missing: $payloadManifestPath"
  }
  $payloadManifest = Get-Content -LiteralPath $payloadManifestPath -Raw | ConvertFrom-Json
  if ($payloadManifest.schemaVersion -ne 1) { throw "Unsupported build manifest schema." }
  $installedManifestPath = Join-Path $appDir "build-manifest.json"
  $payloadManifestHash = (Get-FileHash -LiteralPath $payloadManifestPath -Algorithm SHA256).Hash.ToLowerInvariant()
  $installedManifestHash = (Get-FileHash -LiteralPath $installedManifestPath -Algorithm SHA256).Hash.ToLowerInvariant()
  if ($installedManifestHash -ne $payloadManifestHash) {
    throw "Installed build manifest checksum mismatch: $installedManifestPath"
  }
  $hashChecks = @(
    @{ RelativePath = "SonoBusVideoCaptureHelper.exe"; Destinations = @(
      (Join-Path $appDir "SonoBusVideoCaptureHelper.exe"),
      (Join-Path $pluginDirs[0] "Contents/x86_64-win/SonoBusVideoCaptureHelper.exe"),
      (Join-Path $pluginDirs[1] "Contents/x86_64-win/SonoBusVideoCaptureHelper.exe")
    ) },
    @{ RelativePath = "SonoBusMoLiXiuBridge.exe"; Destinations = @(
      (Join-Path $appDir "SonoBusMoLiXiuBridge.exe"),
      (Join-Path $pluginDirs[0] "Contents/x86_64-win/SonoBusMoLiXiuBridge.exe"),
      (Join-Path $pluginDirs[1] "Contents/x86_64-win/SonoBusMoLiXiuBridge.exe")
    ) },
    @{ RelativePath = "SonoBusMoLiXiuHook.dll"; Destinations = @(
      (Join-Path $appDir "SonoBusMoLiXiuHook.dll"),
      (Join-Path $pluginDirs[0] "Contents/x86_64-win/SonoBusMoLiXiuHook.dll"),
      (Join-Path $pluginDirs[1] "Contents/x86_64-win/SonoBusMoLiXiuHook.dll")
    ) },
    @{ RelativePath = "SonoBus.vst3/Contents/x86_64-win/SonoBus.vst3"; Destinations = @(
      (Join-Path $pluginDirs[0] "Contents/x86_64-win/SonoBus.vst3")
    ) },
    @{ RelativePath = "SonoBusInstrument.vst3/Contents/x86_64-win/SonoBusInstrument.vst3"; Destinations = @(
      (Join-Path $pluginDirs[1] "Contents/x86_64-win/SonoBusInstrument.vst3")
    ) }
  )
  foreach ($check in $hashChecks) {
    $source = Join-Path $payloadRoot ($check.RelativePath -replace '/', '\')
    if (-not (Test-Path -LiteralPath $source -PathType Leaf)) { throw "Manifest source is missing: $source" }
    $manifestEntry = $payloadManifest.files.PSObject.Properties[$check.RelativePath]
    if ($null -eq $manifestEntry) { throw "Manifest omitted: $($check.RelativePath)" }
    $sourceHash = (Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash.ToLowerInvariant()
    if ([string]$manifestEntry.Value -ne $sourceHash) { throw "Source manifest checksum mismatch: $($check.RelativePath)" }
    foreach ($destination in $check.Destinations) {
      if (-not (Test-Path -LiteralPath $destination -PathType Leaf)) { throw "Installer omitted: $destination" }
      $destinationHash = (Get-FileHash -LiteralPath $destination -Algorithm SHA256).Hash.ToLowerInvariant()
      if ($destinationHash -ne $sourceHash) { throw "Installed payload checksum mismatch: $destination" }
    }
  }

  & $helpers[0] --list | Out-Null
  if ($LASTEXITCODE -ne 0) { throw "Installed SharedReadOnly camera helper smoke test failed." }

  $uninstaller = Get-ChildItem -Path $appDir -Filter "unins*.exe" -File -ErrorAction SilentlyContinue | Select-Object -First 1
  if ($null -eq $uninstaller) { throw "Installer omitted its uninstaller." }
  $verifyUninstall = $true
} finally {
  $uninstallError = $null
  if ($null -ne $uninstaller) {
    $process = Start-Process -FilePath $uninstaller.FullName -ArgumentList "/VERYSILENT", "/SUPPRESSMSGBOXES", "/NORESTART" -Wait -PassThru
    if ($verifyUninstall -and $process.ExitCode -ne 0) {
      $uninstallError = "Uninstaller exited with code $($process.ExitCode)."
    }
  }

  if ($verifyUninstall -and $null -eq $uninstallError) {
    foreach ($path in $required + $pluginDirs) {
      if (Test-Path $path) { $uninstallError = "Uninstaller left installed payload: $path"; break }
    }
  }

  foreach ($pluginDir in $pluginDirs) { Remove-Item -Recurse -Force $pluginDir -ErrorAction SilentlyContinue }
  Remove-Item -Recurse -Force $appDir -ErrorAction SilentlyContinue
  if ($null -ne $uninstallError) { throw $uninstallError }
}
