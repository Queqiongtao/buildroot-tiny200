@echo off
set SELFPATH=%~dp0
set ROOTDIR=%SELFPATH%..\..
set IMAGE=%ROOTDIR%\output\images\sysimage-nor.img
echo | set /p="Waiting for FEL Device";
:loop
echo | set /p="."
for /f %%i in ('%SELFPATH%\bin\xfel.exe version 2^>nul ^| findstr "AWUSBFEX"') do set OUTPUT=%%i
if "%OUTPUT%" == "" goto loop
echo " Gotcha!"
%SELFPATH%\bin\sunxi-fel.exe -p spiflash-write 0 "%IMAGE%"
%SELFPATH%\bin\xfel.exe reset
pause
