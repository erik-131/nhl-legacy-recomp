@echo off
REM Build the plume cleared-window smoke test (high-cut M0c) under the project's
REM proven toolchain: VS2022 BuildTools vcvars64 + Ninja (MSVC cl). Isolated build
REM dir out\highcut-smoke (gitignored). Run from anywhere.
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
set "SRC=e:\Repositories\nhl-legacy-recomp\gpu\smoke"
set "BIN=e:\Repositories\nhl-legacy-recomp\out\highcut-smoke"
cmake -S "%SRC%" -B "%BIN%" -G Ninja -DCMAKE_BUILD_TYPE=Release
echo CONFIG_EXIT=%ERRORLEVEL%
cmake --build "%BIN%"
echo BUILD_EXIT=%ERRORLEVEL%
