@echo off
rem Builds the PC software into a standalone .exe in publish\.
rem
rem MIND THIS: "--self-contained false" on the command line is IGNORED
rem here as soon as you also pass -r; a self-contained bundle then comes
rem out anyway. With -p:SelfContained=false it does work, which is why
rem both builds below set every option through -p:.

setlocal
cd /d "%~dp0"

echo Small (needs the .NET 8 Desktop runtime)...
dotnet publish src\PAL-sign.csproj -c Release ^
  -p:RuntimeIdentifier=win-x64 -p:SelfContained=false -p:PublishSingleFile=true ^
  -o publish --nologo || goto :failed

rem The .pdb is only debug symbols; nothing in publish\ needs it to run.
del /q publish\PAL-sign.pdb 2>nul

echo.
echo Done:
echo   publish\PAL-sign.exe        needs the .NET 8 Desktop runtime
goto :end

:failed
echo Build failed.
exit /b 1

:end
endlocal
