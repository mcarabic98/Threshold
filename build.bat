@echo off
rem Configure + build helper: bootstraps MSVC env, then runs CMake/Ninja.
rem tools\ (portable CMake/Ninja) is optional: system-wide ones are used if absent.
call "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
set "PATH=%~dp0tools\cmake\bin;%~dp0tools\ninja;%PATH%"
cd /d "%~dp0"
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
if errorlevel 1 exit /b 1
cmake --build build
