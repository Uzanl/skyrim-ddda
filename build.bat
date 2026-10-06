@echo off
rem Builds the DDDA bridge (x86 dinput8.dll), its smoke tests (x86) and the
rem test reader (x64). Output goes to build\. Needs Visual Studio with the
rem "Desktop development with C++" workload (found through vswhere).
setlocal
set ROOT=%~dp0
set CFLAGS=/nologo /std:c++20 /O2 /MT /W4 /EHsc /permissive-

set VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set VSDIR=%%i
if not defined VSDIR (
    echo Visual Studio with C++ tools not found
    exit /b 1
)
set VS=%VSDIR%\VC\Auxiliary\Build

if not exist "%ROOT%build\x86" mkdir "%ROOT%build\x86"
if not exist "%ROOT%build\x64" mkdir "%ROOT%build\x64"

setlocal
call "%VS%\vcvars32.bat" >nul 2>nul || exit /b 1
cl %CFLAGS% /LD "%ROOT%src\ddda_bridge\dllmain.cpp" "%ROOT%src\ddda_bridge\file_overlay.cpp" "%ROOT%src\ddda_bridge\frame_capture.cpp" "%ROOT%src\ddda_bridge\frame_trace.cpp" "%ROOT%src\ddda_bridge\isolate.cpp" "%ROOT%src\ddda_bridge\relight.cpp" "%ROOT%src\ddda_bridge\damage_log.cpp" /Fo"%ROOT%build\x86\\" /Fe"%ROOT%build\x86\dinput8.dll" /link user32.lib || exit /b 1
rem Smoke tests load build\x86\dinput8.dll from their own folder.
cl /nologo /EHsc "%ROOT%tools\dll_smoketest\load_test.cpp" /Fo"%ROOT%build\x86\\" /Fe"%ROOT%build\x86\load_test.exe" ole32.lib || exit /b 1
cl /nologo /EHsc "%ROOT%tools\dll_smoketest\dinput_test.cpp" /Fo"%ROOT%build\x86\\" /Fe"%ROOT%build\x86\dinput_test.exe" dinput8.lib dxguid.lib ole32.lib user32.lib || exit /b 1
endlocal

setlocal
call "%VS%\vcvars64.bat" >nul 2>nul || exit /b 1
cl %CFLAGS% "%ROOT%tools\bridge_reader\main.cpp" /Fo"%ROOT%build\x64\\" /Fe"%ROOT%build\x64\bridge_reader.exe" || exit /b 1
cl %CFLAGS% "%ROOT%tools\cam_driver\main.cpp" /Fo"%ROOT%build\x64\cam_driver.obj" /Fe"%ROOT%build\x64\cam_driver.exe" || exit /b 1
cl %CFLAGS% /LD "%ROOT%src\skse_plugin\plugin.cpp" /Fo"%ROOT%build\x64\\" /Fe"%ROOT%build\x64\DDDABridge.dll" /link shell32.lib ole32.lib || exit /b 1
rem ReShade add-on (headers fetched into external\reshade, tag v6.8.0).
cl %CFLAGS% /LD /I"%ROOT%external\reshade\include" "%ROOT%src\reshade_addon\addon.cpp" /Fo"%ROOT%build\x64\addon.obj" /Fe"%ROOT%build\x64\DDDABridge.addon64" /link gdi32.lib || exit /b 1
endlocal

echo build ok
