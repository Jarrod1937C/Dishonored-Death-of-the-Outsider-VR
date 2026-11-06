# Launches the game with the overlay auto-shown, waits for the mod log to report the first Present,
# optionally grabs a screenshot, prints the log and kills the game.
param(
    [string]$GameDir = 'C:\Program Files (x86)\Steam\steamapps\common\Dishonored_DeathOfTheOutsider',
    [int]$TimeoutSec = 90,
    [int]$SettleSec = 8,          # seconds to wait after first Present before screenshot/kill
    [string]$Screenshot = '',     # PNG path; empty = no screenshot
    [switch]$KeepRunning
)
$ErrorActionPreference = 'Stop'
$log = "$GameDir\DOTOVR.log"
Remove-Item $log -Force -ErrorAction SilentlyContinue
$env:DVR_OVERLAY_AUTOSHOW = '1'
$p = Start-Process -FilePath "$GameDir\Dishonored_DO.exe" -WorkingDirectory $GameDir -PassThru
$deadline = (Get-Date).AddSeconds($TimeoutSec)
$ok = $false
while ((Get-Date) -lt $deadline) {
    Start-Sleep -Seconds 2
    if ($p.HasExited) { "Game exited early (code $($p.ExitCode))"; break }
    if ((Test-Path $log) -and (Select-String -Path $log -Pattern 'first Present' -Quiet)) { $ok = $true; break }
}
if ($ok) {
    Start-Sleep -Seconds $SettleSec
    if ($Screenshot) {
        Add-Type -AssemblyName System.Windows.Forms, System.Drawing
        $b = [System.Windows.Forms.Screen]::PrimaryScreen.Bounds
        $bmp = New-Object System.Drawing.Bitmap $b.Width, $b.Height
        $g = [System.Drawing.Graphics]::FromImage($bmp)
        $g.CopyFromScreen($b.Location, [System.Drawing.Point]::Empty, $b.Size)
        $bmp.Save($Screenshot, [System.Drawing.Imaging.ImageFormat]::Png)
        $g.Dispose(); $bmp.Dispose()
        "Screenshot: $Screenshot"
    }
}
if (Test-Path $log) { Get-Content $log } else { "No log produced." }
if (-not $KeepRunning -and -not $p.HasExited) { Stop-Process -Id $p.Id -Force }
if ($ok) { "RESULT: OK (hook reached Present)" } else { "RESULT: FAIL"; exit 1 }
