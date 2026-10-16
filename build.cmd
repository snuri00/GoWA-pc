@echo off
setlocal
set "VSROOT=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools"
call "%VSROOT%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
set "PATH=%VSROOT%\VC\Tools\Llvm\x64\bin;%PATH%"
if not exist build\build.ninja (
    cmake -S . -B build -G Ninja -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl -DCMAKE_LINKER=lld-link -DCMAKE_BUILD_TYPE=RelWithDebInfo %* || exit /b 1
)
cmake --build build %BUILD_ARGS% || exit /b 1
