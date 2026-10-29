@echo off
rem Generell kjorer: sender nhl_cmd.sh til CT 105 (192.168.0.22) og lagrer utdata i nhl_cmd.log
cd /d "%~dp0"
echo START %date% %time% > nhl_cmd.log
ssh -i "%USERPROFILE%\.ssh\nhl_nuc" -o BatchMode=yes -o StrictHostKeyChecking=accept-new root@192.168.0.22 "tr -d '\r' | bash -s" < nhl_cmd.sh >> nhl_cmd.log 2>&1
echo END %errorlevel% >> nhl_cmd.log
