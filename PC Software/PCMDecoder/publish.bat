@echo off
rem Builds PcmDecoder into a standalone .exe in publish\.
rem
rem MIND THIS: "dotnet publish" cannot build this project. The
rem DeckLinkAPI COM reference needs MSBuild's tlbimp step against the
rem DLL that the Blackmagic Desktop Video driver registered, so the
rem full Visual Studio MSBuild does the publish.

setlocal
cd /d "%~dp0"

set "MSBUILD=C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe"
if not exist "%MSBUILD%" for /f "usebackq delims=" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -requires Microsoft.Component.MSBuild -find MSBuild\**\Bin\MSBuild.exe`) do set "MSBUILD=%%i"
if not exist "%MSBUILD%" echo MSBuild not found: install Visual Studio with the .NET desktop workload. & exit /b 1

echo Small (needs the .NET 8 Desktop runtime)...
"%MSBUILD%" src\PcmDecoder.csproj -restore -t:Publish -p:Configuration=Release -p:Platform=x64 ^
  -p:RuntimeIdentifier=win-x64 -p:SelfContained=false -p:PublishSingleFile=true ^
  -p:PublishDir=..\publish\ -v:m -nologo || goto :failed

rem The .pdb is only debug symbols; nothing in publish\ needs it to run.
del /q publish\PcmDecoder.pdb 2>nul

echo.
echo Done:
echo   publish\PcmDecoder.exe        needs the .NET 8 Desktop runtime
goto :end

:failed
echo Build failed.
exit /b 1

:end
endlocal
