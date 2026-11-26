# Portable hertzyevs package for someone without Qt / Arena / Visual Studio: unzip, double-click HertzyEVS.exe.
#   powershell -ExecutionPolicy Bypass -File tools\ship_hertzy.ps1 -Edition full|viewer|demo -Zip <out.zip> [-Clips <clip.raw>,...] [-Project Koelkast]
# Editions (CMake HZ_EDITION, each in its own build folder):
#   full   = Library, Live, Analyse, Camera; the camera only, no simulator        build-full\Release
#   viewer = Library + Analyse only: no recording, no camera, no Arena DLLs        build-viewer\Release
#   demo   = everything, with the simulator instead of a camera, no Arena DLLs   build-demo\Release
#   cmake -S . -B build-<edition> -DHZ_EDITION=<edition> ; cmake --build build-<edition> --config Release --target hertzyevs hertzyevs_launcher
#   (demo: cmake -S . -B build-demo -DHZ_EDITION=dev "-DARENA_SDK_DIR=")
# The full edition bundles the LUCID Arena SDK runtime DLLs: check LUCID's licence before publishing that package.
# Layout of the zip:
#   HertzyEVS\HertzyEVS.exe            launcher (icon) -> app\hertzyevs.exe
#   HertzyEVS\app\...                 hertzyevs + Qt (windeployqt) + Arena SDK runtime + VC++ runtime
#   HertzyEVS\recordings\<Project>\   the clips (.raw + .json + .thumb.png); the app opens this folder (defaultRoot)
#   HertzyEVS\README.txt
# Needs a Release build of that edition (see above); the dev build in build\ has the simulator and is not shipped.
param(
    [ValidateSet("full", "viewer", "demo")] [string] $Edition = "full",
    [string[]] $Clips = @(),
    [string] $Project = "Recordings",
    [Parameter(Mandatory)] [string] $Zip,
    [string] $Name = "",
    [string] $Build = "",
    [string] $Qt = "$env:USERPROFILE\Qt\6.8.3\msvc2022_64",
    [string] $Arena = "C:\Program Files\Lucid Vision Labs\Arena SDK\x64Release",
    [string] $Ffmpeg = "",          # ffmpeg.exe (static build with libx264) to ship next to hertzyevs.exe, for MP4 export
    [string[]] $Rename = @()        # "old=new" clip base names for the package, e.g. rec_20261120_122825=fan_test_20261120_122825
)
$ErrorActionPreference = "Stop"
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
if (-not $Name) { $Name = if ($Edition -eq "viewer") { "HertzyEVS Viewer" } elseif ($Edition -eq "demo") { "HertzyEVS Demo" } else { "HertzyEVS" } }
if (-not $Build) { $Build = Join-Path $here "..\build-$Edition\Release" }
$Build = (Resolve-Path $Build).Path
$stage = Join-Path ([IO.Path]::GetTempPath()) ("hertzyevs_ship_" + [guid]::NewGuid().ToString("N"))
$root = Join-Path $stage $Name
$app = Join-Path $root "app"
$rec = Join-Path (Join-Path $root "recordings") $Project
New-Item -ItemType Directory -Force $app, $rec | Out-Null

# app + Qt
Copy-Item "$Build\hertzyevs.exe" $app
$env:PATH = "$Qt\bin;$env:PATH"   # windeployqt resolves Qt6Core.dll etc. through PATH
& "$Qt\bin\windeployqt.exe" --release --no-translations --no-system-d3d-compiler --no-opengl-sw --verbose 0 `
    --qmldir "$here\..\qml" "$app\hertzyevs.exe"
if ($LASTEXITCODE) { throw "windeployqt failed" }

# Arena SDK runtime (the full edition imports Arena; it will not start without these). The viewer has no camera code.
$arenaDlls = "Arena_v140.dll", "GenTL_LUCID_v140.dll", "GenTL_LUCID_v140.cti", "lucidlog_v140.dll",
    "GCBase_MD_VC140_v3_3_LUCID.dll", "GenApi_MD_VC140_v3_3_LUCID.dll", "Log_MD_VC140_v3_3_LUCID.dll",
    "MathParser_MD_VC140_v3_3_LUCID.dll", "NodeMapData_MD_VC140_v3_3_LUCID.dll", "XmlParser_MD_VC140_v3_3_LUCID.dll",
    "log4cpp_MD_VC140_v3_3_LUCID.dll", "lucid_metavision_sdk_base.dll", "lucid_metavision_sdk_core.dll", "lucid_imx63x_plugin5.dll"
if ($Edition -eq "full") { foreach ($d in $arenaDlls) { Copy-Item (Join-Path $Arena $d) $app } }

# VC++ runtime (app-local, so no redistributable install is needed)
$vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -property installationPath
$crt = Get-ChildItem "$vs\VC\Redist\MSVC" -Directory | Where-Object { $_.Name -match '^\d+(\.\d+)+$' } | Sort-Object { [version]$_.Name } -Descending |
    ForEach-Object { Get-ChildItem (Join-Path $_.FullName "x64") -Directory -Filter "Microsoft.VC14*.CRT" -ErrorAction SilentlyContinue } |
    Select-Object -First 1
Copy-Item "$($crt.FullName)\*.dll" $app

# ffmpeg for MP4 export (the app looks next to its exe first)
if ($Ffmpeg) {
    Copy-Item $Ffmpeg (Join-Path $app "ffmpeg.exe")
    $lic = Join-Path (Split-Path -Parent $Ffmpeg) "LICENSE.txt"   # GPL text that comes with the build
    if (Test-Path $lic) { Copy-Item $lic (Join-Path $app "ffmpeg-LICENSE.txt") }
}

# third-party licence texts (Qt LGPL, pocketfft BSD, ...): required when the binaries are passed on
$lic = Join-Path $root "licenses"
Copy-Item (Join-Path $here "..\licenses") $lic -Recurse
Copy-Item (Join-Path $here "..\THIRD_PARTY.md") $lic

# launcher + recordings
Copy-Item "$Build\hertzyevs_launcher.exe" (Join-Path $root "$Name.exe")
foreach ($c in $Clips) {
    $c = (Resolve-Path $c).Path
    $base = [IO.Path]::Combine([IO.Path]::GetDirectoryName($c), [IO.Path]::GetFileNameWithoutExtension($c))
    $old = [IO.Path]::GetFileName($base)
    $new = $old
    foreach ($r in $Rename) { $kv = $r.Split("=", 2); if ($kv.Count -eq 2 -and $kv[0] -eq $old) { $new = $kv[1] } }
    foreach ($ext in ".raw", ".thumb.png") { if (Test-Path "$base$ext") { Copy-Item "$base$ext" (Join-Path $rec "$new$ext") } }
    if (Test-Path "$base.json") {
        # sidecar: point it at the renamed .raw and the package's project; keep everything else (the rest of the file unchanged)
        $t = [IO.File]::ReadAllText("$base.json")
        $t = [regex]::Replace($t, '"raw"\s*:\s*"[^"]*"', ('"raw": "' + $new + '.raw"'))
        $t = [regex]::Replace($t, '"project"\s*:\s*"[^"]*"', ('"project": "' + $Project + '"'))
        [IO.File]::WriteAllText((Join-Path $rec "$new.json"), $t, (New-Object Text.UTF8Encoding $false))   # no BOM: Qt's JSON reader rejects it
    }
    if (Test-Path "$base.measure") { Copy-Item "$base.measure" (Join-Path $rec "$new.measure") -Recurse }
}

@"
$Name - $(if ($Edition -eq "viewer") { "event-camera recordings viewer" } else { "frequency and RPM workbench for the Triton2 EVS event camera" })
==========================================

1. Unzip the whole folder (do not run it from inside the zip).
2. Double-click $Name.exe.
3. The Library page opens with the recordings in recordings\$Project.
   Double-click a recording (or select it and open it) to view it on the Analyse page:
   playback in slow motion, strobe, spectra of drawn lines, measurements.

Notes
- No installation and no admin rights needed. Windows 10/11, 64-bit.
- Windows SmartScreen may warn because the app is not signed: More info > Run anyway.
$(if ($Edition -eq "viewer") { "- This is the viewer: it opens and analyses recordings; it does not record. Put .raw recordings
  (with their .json) in a project folder under recordings\, or pick another library folder on the Library page." }
  elseif ($Edition -eq "demo") { "- This is the demo: the Live page shows a simulated lab bench (ringing bars + a fan with a tape mark) instead of a
  camera. Draw a small ROI on the fan blades, switch on Rotation speed, and record clips to try the Analyse page." }
  else { "- The Live and Camera pages need the LUCID Triton2 EVS camera; without it they stay empty." })
- Measurements you save are written next to the recording in the recordings folder.
- Third-party software and its licences (Qt: LGPL v3, pocketfft: BSD): see the licenses folder.
$(if ($Ffmpeg) { "- Video export (Analyse > Playback > Export MP4) uses the bundled ffmpeg (app\ffmpeg.exe, GPL, https://ffmpeg.org)." } else { "- Video export writes MP4 when ffmpeg is on the PATH (https://ffmpeg.org), otherwise a folder of PNG frames." })
"@ | Set-Content -Encoding utf8 (Join-Path $root "README.txt")

if (Test-Path $Zip) { Remove-Item $Zip }
# entries written one by one with "/" separators (Compress-Archive and ZipFile.CreateFromDirectory under Windows
# PowerShell 5.1 write "\", which 7-Zip / unzip / macOS turn into flat file names)
Add-Type -AssemblyName System.IO.Compression, System.IO.Compression.FileSystem
if (-not [IO.Path]::IsPathRooted($Zip)) { $Zip = Join-Path (Get-Location) $Zip }
$za = [IO.Compression.ZipFile]::Open($Zip, [IO.Compression.ZipArchiveMode]::Create)
try {
    foreach ($f in Get-ChildItem $stage -Recurse -File) {
        $entry = $f.FullName.Substring($stage.Length).TrimStart([char]92).Replace([string][char]92, '/')
        [void][IO.Compression.ZipFileExtensions]::CreateEntryFromFile($za, $f.FullName, $entry, [IO.Compression.CompressionLevel]::Optimal)
    }
} finally { $za.Dispose() }
Remove-Item -Recurse -Force $stage
Get-Item $Zip | Select-Object FullName, @{n = "MB"; e = { [math]::Round($_.Length / 1MB, 1) } }
