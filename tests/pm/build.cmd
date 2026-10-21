@echo off
setlocal
set "VSROOT=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools"
call "%VSROOT%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
set "PATH=%VSROOT%\VC\Tools\Llvm\x64\bin;%PATH%"
set "SRC=%~dp0."
set "OUT=%~1"
if "%OUT%"=="" set "OUT=%~dp0..\..\build_pm"
if not exist "%OUT%\build.ninja" (
    cmake -S "%SRC%" -B "%OUT%" -G Ninja -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl -DCMAKE_LINKER=lld-link -DCMAKE_BUILD_TYPE=Release || exit /b 1
)
cmake --build "%OUT%" || exit /b 1
