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
    (Join-Path $pluginDirs[0] "Contents/x86_64-win/ffmpeg.exe"),
    (Join-Path $pluginDirs[1] "Contents/x86_64-win/ffmpeg.exe")
  )
  $required = @(
    (Join-Path $appDir "SonoBus.exe"),
    (Join-Path $appDir "ffmpeg-LICENSE"),
    (Join-Path $appDir "ffmpeg-README.txt"),
    (Join-Path $appDir "ffmpeg-RUNTIME.md"),
    (Join-Path $pluginDirs[0] "Contents/x86_64-win/SonoBus.vst3"),
    (Join-Path $pluginDirs[1] "Contents/x86_64-win/SonoBusInstrument.vst3")
  ) + $runtimes

  foreach ($path in $required) {
    if (-not (Test-Path $path -PathType Leaf)) { throw "Installer omitted: $path" }
  }

  $sourceHash = (Get-FileHash -Algorithm SHA256 $env:SONOBUS_FFMPEG_PATH).Hash
  foreach ($runtime in $runtimes) {
    if ((Get-FileHash -Algorithm SHA256 $runtime).Hash -ne $sourceHash) {
      throw "Installed FFmpeg checksum mismatch: $runtime"
    }
  }

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
