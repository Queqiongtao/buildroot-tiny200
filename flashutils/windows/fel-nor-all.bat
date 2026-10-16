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
%SELFPATH%\bin\xfel.exe spinor erase 0x0 0x1000000
%SELFPATH%\bin\xfel.exe spinor write 0x0 "%IMAGE%"
%SELFPATH%\bin\xfel.exe reset
pause
