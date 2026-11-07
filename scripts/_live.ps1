param([int]$Seconds = 240)
# C-6 LIVE FEED — plume renders the LIVE game in real time (no .bin replay). Runs the beta takeover
# (builds per-draw packets) AND the plume present window (renders them) in ONE process, handing each
# frame across in memory (HighcutLivePushDraw/CommitFrame). This is the co-run the plan flagged as the
# #1 risk — if the two coexist, plume tracks the live game.
#
# USE: launch, drive past the intro to live GAMEPLAY, press F10 to arm the takeover. Watch the
# 'NHL high-cut (plume Vulkan)' window — it should start rendering the live scene each frame.
$dir = "e:\Repositories\nhl-legacy-recomp\out\build\win-amd64-relwithdebinfo"
Get-Process nhllegacy -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 400
Get-ChildItem env: | Where-Object Name -like 'NHL_*' | ForEach-Object { Remove-Item "env:$($_.Name)" }
# Producer (beta takeover, builds packets) :
$env:NHL_BACKEND = "beta"; $env:NHL_BETA_TAKEOVER = "1"; $env:NHL_BETA_LIVE = "1"
$env:NHL_BETA_FLAT = "1"; $env:NHL_BETA_DEPTH = "1"
$env:NHL_BETA_LIVE_HOTKEY = "1"            # F10 arms the takeover
$env:NHL_HIGHCUT_FRAME_CAPTURE = "1"       # required: drives the per-draw packet build + frame boundary
# Consumer (plume present window, renders) + the live bridge:
$env:NHL_HIGHCUT_PRESENT = "1"             # stand up the plume Vulkan window
$env:NHL_HIGHCUT_C5 = "1"                  # enable the C-5 renderable-draw path in plume
$env:NHL_HIGHCUT_LIVE_FEED = "1"           # C-6: rebuild from the in-memory bridge each committed frame
$argline = '--game_data_root "H:\Emulators\games\XBOX\NHL Legacy - Vanilla"'
$p = Start-Process -FilePath "$dir\nhllegacy.exe" -ArgumentList $argline -WorkingDirectory $dir -PassThru -NoNewWindow -RedirectStandardError "$dir\live_stderr.log" -RedirectStandardOutput "$dir\live_stdout.log"
Write-Output "GAME LAUNCHED (pid $($p.Id)). Drive to LIVE GAMEPLAY, press F10, watch the plume window render live."
$deadline = (Get-Date).AddSeconds($Seconds)
while ((Get-Date) -lt $deadline -and -not $p.HasExited) { Start-Sleep -Seconds 2 }
if (-not $p.HasExited) { $p.Kill() }
Start-Sleep -Milliseconds 600
$log = (Get-ChildItem "$dir\logs\*.log" | Sort-Object LastWriteTime -Descending | Select-Object -First 1).FullName
Write-Output "LOG=$log"
Write-Output "--- takeover + live-feed signals ---"
Select-String -Path $log -Pattern 'LIVE takeover ACTIVE|highcut-C5-LIVE|loaded \d+ renderable' | Select-Object -Last 6 | ForEach-Object { ($_.Line -split '\] ')[-1] }
Write-Output "--- crashes / device-removed (target none) ---"
Select-String -Path $log -Pattern 'device removed|0x887A|TDR|abort|assert|exception' | Select-Object -Last 5 | ForEach-Object { ($_.Line -split '\] ')[-1] }
