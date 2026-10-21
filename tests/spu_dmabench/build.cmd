@echo off
setlocal
set "VSROOT=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools"
call "%VSROOT%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
set "PATH=%VSROOT%\VC\Tools\Llvm\x64\bin;%PATH%"
set ROOT=%~dp0..\..
set B=%ROOT%\build
set OUT=%ROOT%\build_dmabench
if not exist %OUT% mkdir %OUT%
python %~dp0mkobjs.py %B% %OUT%\objs.txt || exit /b 1
clang-cl /nologo /O2 /Ob2 /MT /EHsc /std:c++20 /clang:-march=x86-64-v3 /I%ROOT%\runtime\include /I%ROOT%\runtime\src /I%ROOT%\out\include /c %ROOT%\tests\spu_dmabench\dmabench.cpp /Fo%OUT%\dmabench.obj || exit /b 1
lld-link /nologo /machine:x64 /subsystem:console /out:%OUT%\dmabench.exe %OUT%\dmabench.obj @%OUT%\objs.txt %B%\gowa_ppu.lib %B%\gowa_spu.lib %B%\gowa_ppu_ops.lib %B%\third_party\glslang\glslang\glslang.lib %B%\third_party\glslang\SPIRV\SPIRV.lib %B%\third_party\glslang\glslang\glslang-default-resource-limits.lib dbghelp.lib winmm.lib %ROOT%\third_party\streamline\lib\x64\sl.interposer.lib delayimp.lib version.lib kernel32.lib user32.lib gdi32.lib shell32.lib ole32.lib advapi32.lib || exit /b 1
