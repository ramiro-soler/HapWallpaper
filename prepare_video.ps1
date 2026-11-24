<#
.SYNOPSIS
  Turns any video into a HapWallpaper wallpaper (wallpaper.hap and/or wallpaper.mp4).

.DESCRIPTION
  - Detects the primary monitor's resolution (or uses -Width/-Height).
  - Scales and center-crops the video to fill the screen with no black bars.
  - Writes wallpaper.hap (BC1 frames: 0 % GPU video decoder, ~1.8 MB per frame at
    2560x1440) and/or wallpaper.mp4 in AV1 (small file, uses the GPU decoder).
  - Optional: use only part of the video, darken the clock/tray corner.
  - Only use videos you made or have permission to use as a wallpaper.

.EXAMPLE
  .\prepare_video.ps1 -Video C:\videos\my_wallpaper.mp4
.EXAMPLE
  .\prepare_video.ps1 -Video my_wallpaper.mp4 -Format both -Fps 30 -Start 5 -Duration 20
.EXAMPLE
  .\prepare_video.ps1 -Video my_wallpaper.mp4 -ClockShadow
.EXAMPLE
  .\prepare_video.ps1 -Video my_wallpaper.mp4 -FFmpeg C:\tools\ffmpeg\bin\ffmpeg.exe
#>
param(
    [Parameter(Mandatory = $true)] [string] $Video,
    [ValidateSet('hap', 'av1', 'both')] [string] $Format = 'hap',
    [ValidateRange(1, 240)] [int] $Fps = 30,
    # 0 = primary monitor resolution
    [ValidateRange(0, 16384)] [int] $Width = 0,
    [ValidateRange(0, 16384)] [int] $Height = 0,
    [ValidateRange(0, 86400)] [double] $Start = 0,
    # 0 = until the end of the video
    [ValidateRange(0, 86400)] [double] $Duration = 0,
    # softly darkens the bottom-right corner (clock and tray)
    [switch] $ClockShadow,
    [string] $Output = $PSScriptRoot,
    # path to a specific ffmpeg.exe (otherwise every ffmpeg on the PC is tried)
    [string] $FFmpeg = ''
)
$ErrorActionPreference = 'Stop'
if (-not (Test-Path $Video)) { Write-Host "Video not found: $Video" -ForegroundColor Red; exit 1 }

# --- ffmpeg: many builds (Gyan "essentials", BtbN, ffmpeg bundled with other apps) have no HAP
# encoder, and the first one on PATH is often one of those. So every ffmpeg found is checked and
# the first one with the needed encoders is used.
$needHap = $Format -ne 'av1'
$needAv1 = $Format -ne 'hap'
function Find-FFmpegCandidates {
    $c = @()
    if ($FFmpeg) { $c += $FFmpeg }
    $c += @(Get-Command ffmpeg -All -CommandType Application -ErrorAction SilentlyContinue | ForEach-Object Source)
    # winget installs (found even if this terminal was opened before the install updated PATH)
    $roots = @("$env:LOCALAPPDATA\Microsoft\WinGet\Packages", "$env:ProgramFiles\WinGet\Packages", "$env:LOCALAPPDATA\Microsoft\WinGet\Links")
    foreach ($r in $roots) {
        if (Test-Path $r) { $c += @(Get-ChildItem $r -Recurse -Filter ffmpeg.exe -ErrorAction SilentlyContinue | ForEach-Object FullName) }
    }
    $c | Where-Object { $_ -and (Test-Path $_) } | ForEach-Object { (Resolve-Path $_).Path } | Select-Object -Unique
}
function Test-FFmpeg([string] $exe) {
    $ErrorActionPreference = 'Continue' # a broken or non-ffmpeg exe must not stop the search
    $enc = ''; $ver = '?'
    try {
        $enc = & $exe -hide_banner -encoders 2>$null | Out-String
        $ver = [string](& $exe -hide_banner -version 2>$null | Select-Object -First 1) -replace '^ffmpeg version\s+', '' -replace '\s+Copyright.*$', ''
    } catch { }
    $missing = @()
    if ($needHap -and $enc -notmatch '(?m)^\s*V\S*\s+hap\s') { $missing += 'HAP' }
    if ($needAv1 -and $enc -notmatch '(?m)^\s*V\S*\s+libsvtav1\s') { $missing += 'libsvtav1 (AV1)' }
    [pscustomobject]@{ Path = $exe; Version = $ver; Missing = $missing }
}
function Select-FFmpeg {
    $tested = @(Find-FFmpegCandidates | ForEach-Object { Test-FFmpeg $_ })
    [pscustomobject]@{ Good = ($tested | Where-Object { -not $_.Missing } | Select-Object -First 1); Tested = $tested }
}

$pick = Select-FFmpeg
if (-not $pick.Good) {
    if ($pick.Tested) {
        Write-Host "Found ffmpeg, but none of them can do this:" -ForegroundColor Yellow
        foreach ($t in $pick.Tested) { Write-Host ("  {0}`n    version {1} - missing: {2}" -f $t.Path, $t.Version, ($t.Missing -join ', ')) }
        Write-Host "  (Usually an 'essentials' or minimal build. HAP needs a build with libsnappy, like Gyan's 'full' build.)"
    } else {
        Write-Host "ffmpeg was not found." -ForegroundColor Yellow
    }
    if (Get-Command winget -ErrorAction SilentlyContinue) {
        $ans = Read-Host "Install the 'full' ffmpeg build now with winget (Gyan.FFmpeg, ~200 MB)? [Y/n]"
        if ($ans -notmatch '^(n|no)$') {
            & winget install --id Gyan.FFmpeg -e --accept-source-agreements --accept-package-agreements
            $pick = Select-FFmpeg
        }
    }
    if (-not $pick.Good) {
        Write-Host "No usable ffmpeg. Install the full build and run this again:" -ForegroundColor Red
        Write-Host "  winget install --id Gyan.FFmpeg -e"
        Write-Host "or download 'ffmpeg-release-full' from https://www.gyan.dev/ffmpeg/builds/ and pass it with -FFmpeg C:\path\to\ffmpeg.exe"
        exit 1
    }
}
$ffmpeg = $pick.Good.Path
Write-Host "Using ffmpeg: $ffmpeg ($($pick.Good.Version))"
$Video = (Resolve-Path $Video).Path

# --- primary monitor resolution
if (-not $Width -or -not $Height) {
    $vc = Get-CimInstance Win32_VideoController | Where-Object CurrentHorizontalResolution | Select-Object -First 1
    $Width = [int]$vc.CurrentHorizontalResolution; $Height = [int]$vc.CurrentVerticalResolution
    if (-not $Width) { Write-Host "Couldn't detect the resolution. Pass -Width and -Height." -ForegroundColor Red; exit 1 }
}
if ($Format -ne 'av1' -and ($Width % 4 -or $Height % 4)) { Write-Host "Width and height must be multiples of 4 for HAP." -ForegroundColor Red; exit 1 }

# --- source info
$ffprobe = Join-Path (Split-Path $ffmpeg) 'ffprobe.exe'
if (-not (Test-Path $ffprobe)) { $ffprobe = (Get-Command ffprobe -ErrorAction SilentlyContinue).Source }
if (-not $ffprobe) { Write-Host "ffprobe.exe was not found next to $ffmpeg." -ForegroundColor Red; exit 1 }
$probe = & $ffprobe -v error -select_streams v:0 -show_entries stream=width,height,r_frame_rate:format=duration -of default=nw=1 $Video
$info = @{}; $probe | ForEach-Object { $k, $v = $_ -split '=', 2; $info[$k] = $v }
$total = [double]::Parse($info['duration'], [Globalization.CultureInfo]::InvariantCulture)
$dur = if ($Duration -gt 0) { $Duration } else { $total - $Start }
$frameCount = [math]::Floor($dur * $Fps)
if ($frameCount -lt 1) { Write-Host ("Nothing to encode: the video is {0:N1} s long and -Start is {1} s." -f $total, $Start) -ForegroundColor Red; exit 1 }
Write-Host ("Source: {0}x{1}, {2:N1} s  ->  screen {3}x{4}, {5} fps, {6} frames" -f $info['width'], $info['height'], $total, $Width, $Height, $Fps, $frameCount)
New-Item -ItemType Directory -Force $Output | Out-Null
if ($Format -ne 'av1') {
    $mb = $frameCount * ($Width * $Height / 2 + 8) / 1MB
    Write-Host ("wallpaper.hap will take ~{0:N0} MB" -f $mb)
    $free = (Get-PSDrive ((Resolve-Path $Output).Drive.Name)).Free / 1MB
    if ($mb * 1.1 -gt $free) { Write-Host "Not enough free space on the output drive." -ForegroundColor Red; exit 1 }
}

# --- filters: fill the screen (scale + center crop), fps, extras
$vf = "scale=${Width}:${Height}:force_original_aspect_ratio=increase:flags=lanczos,crop=${Width}:${Height},setsar=1,fps=$Fps"
if ($ClockShadow) {
    # dark gradient in the bottom-right corner (clock and tray area)
    $sw = [int]($Width * 0.35); $sh = [int]($Height * 0.12)
    $vf += ",format=yuv420p,geq=lum='lum(X,Y)*(1-0.55*clip((X-($Width-$sw))/$sw,0,1)*clip((Y-($Height-$sh))/$sh,0,1))':cb='cb(X,Y)':cr='cr(X,Y)'"
}
$trim = @()
if ($Start -gt 0) { $trim += @('-ss', "$Start") }
$trimOut = @()
if ($Duration -gt 0) { $trimOut += @('-t', "$Duration") }

$timer = [Diagnostics.Stopwatch]::StartNew()
$ErrorActionPreference = 'Continue' # ffmpeg writes its progress to stderr
if ($Format -ne 'av1') {
    $out = Join-Path $Output 'wallpaper.hap'
    Write-Host "Writing $out ..."
    & $ffmpeg -v error -stats -y @trim -i $Video @trimOut -vf $vf -pix_fmt rgba -c:v hap -format hap -compressor none -an -f rawvideo $out
    if ($LASTEXITCODE) { Write-Host "ffmpeg failed." -ForegroundColor Red; exit 1 }
    $stride = $Width * $Height / 2 + 8
    $len = (Get-Item $out).Length
    if ($len % $stride) {
        # Without fixed-size frames the player can't use the file: don't leave it behind.
        Remove-Item $out -Force
        if ($Format -eq 'hap') {
            Write-Host "This ffmpeg wrote HAP frames of varying size, which HapWallpaper can't play. Try another ffmpeg build (-FFmpeg) or use -Format av1." -ForegroundColor Red
            exit 1
        }
        Write-Host "This ffmpeg wrote HAP frames of varying size, which HapWallpaper can't play; deleted wallpaper.hap. Writing only the MP4." -ForegroundColor Yellow
    }
    else {
        # 32-byte trailer so the player knows the format:
        # "HAPWALL1" + width, height, fps numerator, fps denominator, frames, bytes per frame.
        $frames = [uint32]($len / $stride)
        $fs = [IO.File]::Open($out, 'Append', 'Write')
        $bw = New-Object IO.BinaryWriter -ArgumentList $fs
        $bw.Write([Text.Encoding]::ASCII.GetBytes('HAPWALL1'))
        foreach ($v in @($Width, $Height, $Fps, 1, $frames, $stride)) { $bw.Write([uint32]$v) }
        $bw.Close()
        Write-Host ("OK: {0} frames at {1} fps, {2:N0} MB" -f $frames, $Fps, ($len / 1MB)) -ForegroundColor Green
    }
}
if ($Format -ne 'hap') {
    $out = Join-Path $Output 'wallpaper.mp4'
    Write-Host "Writing $out ..."
    & $ffmpeg -v error -stats -y @trim -i $Video @trimOut -vf $vf -c:v libsvtav1 -preset 6 -crf 30 -g ($Fps * 4) -pix_fmt yuv420p -an $out
    if ($LASTEXITCODE) { Write-Host "ffmpeg failed." -ForegroundColor Red; exit 1 }
    Write-Host ("OK: {0:N1} MB" -f ((Get-Item $out).Length / 1MB)) -ForegroundColor Green
}
Write-Host ("Done in {0:N0} s. Restart HapWallpaper (right-click the tray icon > Exit, then run it again)." -f $timer.Elapsed.TotalSeconds)
