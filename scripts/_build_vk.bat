@echo off
REM ============================================================================
REM DEPRECATED 2026-06-17. The plain non-FFX `-vk` build is RETIRED.
REM The single canonical build is `-vk-ffx` (FidelityFX on; FFX is opt-in at
REM runtime so it is a strict superset of the old -vk build). Both the SDK and
REM the game now build from the same source trees -- there is no separate -vk
REM source, only this output dir + the non-FFX SDK install, which go stale.
REM
REM Use instead (run from the repo root):
REM   scripts\_ffx_sdk_build_install.bat   (SDK -> out/install/win-amd64-ffx)
REM   scripts\_game_ffx_build.bat          (game -> out/build/win-amd64-vk-ffx)
REM Canonical exe: out/build/win-amd64-vk-ffx/nhllegacy.exe
REM ============================================================================
echo [DEPRECATED] -vk is retired; building the canonical -vk-ffx build instead.
echo             SDK:  scripts\_ffx_sdk_build_install.bat
echo             Game: scripts\_game_ffx_build.bat  (exe: out\build\win-amd64-vk-ffx\nhllegacy.exe)
echo.
call "%~dp0_game_ffx_build.bat" %*
