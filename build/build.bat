@echo off
rem Builds dinput8.dll on Windows with LLVM (https://github.com/llvm/llvm-project/releases).
rem clang, lld-link and llvm-dlltool must be on the PATH. No Windows SDK needed.
cd /d "%~dp0\.."
if not exist out mkdir out
llvm-dlltool -m i386:x86-64 -d src\kernel32.def -l out\kernel32.lib || goto :err
clang --target=x86_64-pc-windows-msvc -O2 -Wall -Wextra -Wno-unused-parameter ^
      -fno-builtin -fno-stack-protector -ffreestanding ^
      -c src\sekiro_crashfix.c -o out\sekiro_crashfix.obj || goto :err
lld-link /dll /out:out\dinput8.dll /entry:DllMain /nodefaultlib /subsystem:windows ^
         /machine:x64 out\sekiro_crashfix.obj out\kernel32.lib || goto :err
echo built out\dinput8.dll
exit /b 0
:err
echo build failed
exit /b 1
