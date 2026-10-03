@echo off
rem Builds the CommonLibSSE-based SKSE plugin (src\skse_ghosts) into build\skse.
rem Uses the CMake, Ninja and vcpkg that ship with Visual Studio. The first run
rem downloads and builds the vcpkg dependencies (several minutes).
setlocal
set ROOT=%~dp0
set VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set VSDIR=%%i
if not defined VSDIR (
    echo Visual Studio with C++ tools not found
    exit /b 1
)
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul 2>nul || exit /b 1
set VCPKG_ROOT=%VSDIR%\VC\vcpkg
cmake -S "%ROOT%src\skse_ghosts" -B "%ROOT%build\skse" -G Ninja -DCMAKE_BUILD_TYPE=Release ^
    -DCMAKE_TOOLCHAIN_FILE="%VCPKG_ROOT%\scripts\buildsystems\vcpkg.cmake" ^
    -DVCPKG_TARGET_TRIPLET=x64-windows-static-md || exit /b 1
cmake --build "%ROOT%build\skse" --target DDDAGhosts || exit /b 1
echo skse build ok
